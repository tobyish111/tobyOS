/* dhcp.c -- DHCPv4 client (RFC 2131 / 2132).
 *
 * One-shot boot-time client. State is the four-step FSM:
 *
 *   INIT -> SELECTING -> REQUESTING -> BOUND
 *
 *      DISCOVER (broadcast) ─┐
 *                            ▼
 *                         SELECTING ──── any OFFER ───► remember server-id
 *                            ▼                          + offered IP
 *                         REQUEST  (broadcast)
 *                            ▼
 *                         REQUESTING ─── matching ACK ─► BOUND (return true)
 *                                       NAK  ──────────► fail (return false)
 *                                       timeout ───────► fail (return false)
 *
 * Reception path: udp_recv() invokes dhcp_recv_hook() for every UDP
 * datagram landing on port 68. If no exchange is in flight the hook
 * drops silently. Otherwise it writes one of the well-known message
 * types (DHCP_MSG_OFFER / ACK / NAK) into g_state's "received" slot
 * and dhcp_acquire's poll loop notices.
 *
 * Note on broadcasting: until step 4 we have no IP, so we set the
 * BOOTP "broadcast" flag (bit 15 of `flags`) which asks the server
 * to broadcast its replies rather than unicast them to a non-existent
 * ARP entry.
 */

#include <tobyos/dhcp.h>
#include <tobyos/net.h>
#include <tobyos/udp.h>
#include <tobyos/pit.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* First DHCP_OFFER_PHASE_NUM/DEN of the caller's `timeout_ms` is the
 * DISCOVER/OFFER window; the remainder is for REQUEST/ACK. Within the
 * offer window we retransmit DISCOVER every DHCP_DISCOVER_GAP_MS (wired
 * LANs / slow relays / single lost frames). */
#ifndef DHCP_OFFER_PHASE_NUM
#define DHCP_OFFER_PHASE_NUM 65u
#endif
#ifndef DHCP_OFFER_PHASE_DEN
#define DHCP_OFFER_PHASE_DEN 100u
#endif
#ifndef DHCP_DISCOVER_GAP_MS
#define DHCP_DISCOVER_GAP_MS 300u
#endif

/* ---- transient transaction state -------------------------------- *
 *
 * Lives only across one dhcp_acquire() invocation. After the call
 * returns g_xact.in_flight is cleared so dhcp_recv_hook drops any
 * late-arriving frames. */
struct dhcp_xact {
    bool      in_flight;
    uint32_t  xid;                 /* network byte order, mirrored back */

    /* Captured from OFFER, used to send REQUEST and verify ACK. */
    uint32_t  offered_ip_be;
    uint32_t  server_id_be;
    bool      saw_offer;
    bool      saw_ack;
    bool      saw_nak;

    /* Cached lease parsed from ACK. */
    struct dhcp_lease lease;
};

static struct dhcp_xact g_xact;

/* ---- option helpers --------------------------------------------- */

/* Append a single TLV option. Returns the new write cursor; the
 * caller must ensure (cursor + 2 + len) fits inside opts[]. */
static uint8_t *opt_put(uint8_t *cur, uint8_t code, uint8_t len,
                        const void *data) {
    *cur++ = code;
    *cur++ = len;
    if (len) memcpy(cur, data, len);
    return cur + len;
}

/* Append the END marker. Useful as the last call before send. */
static uint8_t *opt_end(uint8_t *cur) {
    *cur++ = DHCP_OPT_END;
    return cur;
}

/* Walk the options blob and copy out the TLV with `want_code`.
 * Returns a pointer to the option's data and its length, or NULL.
 * Stops at OPT_END (255). PAD (0) is skipped (no length byte). */
static const uint8_t *opt_find(const uint8_t *opts, size_t opts_len,
                               uint8_t want_code, uint8_t *out_len) {
    size_t i = 0;
    while (i < opts_len) {
        uint8_t code = opts[i++];
        if (code == DHCP_OPT_END) return NULL;
        if (code == DHCP_OPT_PAD) continue;
        if (i >= opts_len) return NULL;
        uint8_t len = opts[i++];
        if (i + len > opts_len) return NULL;
        if (code == want_code) {
            if (out_len) *out_len = len;
            return &opts[i];
        }
        i += len;
    }
    return NULL;
}

/* RFC 2132 option 52: when the fixed `options` field is full, DHCP options
 * continue in the BOOTP `file` (bit0) and/or `sname` (bit1) fields. Many
 * home gateways (Deco, ISP CPEs) rely on this; without it we never see
 * option 53 and DHCP appears "broken". */
static const uint8_t *dhcp_opt_find(const struct dhcp_pkt *p, size_t opt_avail,
                                    uint8_t want_code, uint8_t *out_len) {
    const uint8_t *v = opt_find(p->options, opt_avail, want_code, out_len);
    if (v) return v;

    uint8_t  ollen = 0;
    const uint8_t *ol =
        opt_find(p->options, opt_avail, DHCP_OPT_OVERLOAD, &ollen);
    if (!ol || ollen < 1) return NULL;

    uint8_t flags = ol[0];
    /* 1=file only, 2=sname only, 3=file then sname (RFC 2132). */
    if (flags & 1u) {
        v = opt_find(p->file, sizeof p->file, want_code, out_len);
        if (v) return v;
    }
    if (flags & 2u) {
        v = opt_find(p->sname, sizeof p->sname, want_code, out_len);
        if (v) return v;
    }
    return NULL;
}

static bool dhcp_chaddr_ok(const struct dhcp_pkt *p) {
    if (memcmp(p->chaddr, g_my_mac, ETH_ADDR_LEN) == 0) return true;
    /* Some servers zero or omit echoing the client hw addr; xid+magic
     * already scoped this datagram to our transaction.
     *
     * Only require the Ethernet portion (first hlen octets, up to 6) to
     * be zero. Many home gateways pad chaddr[6..15] with 0xff or other
     * junk; treating that as "wrong client" caused us to ignore valid
     * OFFERs (Wireshark showed OFFER + matching xid, no REQUEST). */
    unsigned n = p->hlen;
    if (n == 0 || n > ETH_ADDR_LEN) n = ETH_ADDR_LEN;
    for (unsigned i = 0; i < n; i++) {
        if (p->chaddr[i]) return false;
    }
    return true;
}

/* Read a 32-bit big-endian word out of an options blob (DHCP carries
 * IPs in BE on the wire; we keep them BE in our globals so the result
 * can be assigned straight in). */
static uint32_t opt_u32_be(const uint8_t *p) {
    /* Memory layout on the wire is already a.b.c.d in MSB-first order;
     * reading 4 bytes verbatim yields a value that matches our BE
     * convention for IPs. */
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint32_t opt_u32_host(const uint8_t *p) {
    /* For non-IP fields like lease time -- ntohl back to host order. */
    return ntohl(opt_u32_be(p));
}

/* ---- TX: build DISCOVER / REQUEST ------------------------------- */

/* Common header builder. Sets op/htype/hlen/xid/flags/chaddr/magic.
 * Caller still has to populate the per-message options. */
static void dhcp_build_header(struct dhcp_pkt *p, uint32_t xid_be) {
    memset(p, 0, sizeof(*p));
    p->op    = DHCP_OP_REQUEST;
    p->htype = DHCP_HTYPE_ETH;
    p->hlen  = DHCP_HLEN_ETH;
    p->xid   = xid_be;
    p->flags = htons(0x8000);                       /* broadcast bit */
    memcpy(p->chaddr, g_my_mac, ETH_ADDR_LEN);
    p->magic = DHCP_MAGIC_BE;
}

static bool dhcp_send_discover(uint32_t xid_be) {
    struct dhcp_pkt pkt;
    dhcp_build_header(&pkt, xid_be);

    uint8_t *o = pkt.options;
    uint8_t mt = DHCP_MSG_DISCOVER;
    o = opt_put(o, DHCP_OPT_MSG_TYPE, 1, &mt);

    /* Client identifier (option 61): 1-byte type (1=Ethernet) + MAC. */
    uint8_t cid[1 + ETH_ADDR_LEN];
    cid[0] = DHCP_HTYPE_ETH;
    memcpy(&cid[1], g_my_mac, ETH_ADDR_LEN);
    o = opt_put(o, DHCP_OPT_CLIENT_ID, sizeof(cid), cid);

    /* Parameter request list: tell the server which options we care
     * about. SLIRP/dnsmasq honour this. */
    uint8_t prl[3] = { DHCP_OPT_SUBNET, DHCP_OPT_ROUTER, DHCP_OPT_DNS };
    o = opt_put(o, DHCP_OPT_PARAM_REQ, sizeof(prl), prl);

    o = opt_end(o);
    (void)o;

    /* xid is BE on wire; print the host-order value for human eyes. */
    uint32_t xh = ntohl(xid_be);
    kprintf("[dhcp] DISCOVER xid=0x%08x\n", (unsigned)xh);

    return udp_send(htons(DHCP_PORT_CLIENT),
                    0xFFFFFFFFu /* IP_BROADCAST_BE */,
                    htons(DHCP_PORT_SERVER),
                    &pkt, DHCP_PKT_LEN);
}

static bool dhcp_send_request(uint32_t xid_be,
                              uint32_t requested_ip_be,
                              uint32_t server_id_be) {
    struct dhcp_pkt pkt;
    dhcp_build_header(&pkt, xid_be);

    uint8_t *o = pkt.options;
    uint8_t mt = DHCP_MSG_REQUEST;
    o = opt_put(o, DHCP_OPT_MSG_TYPE, 1, &mt);

    /* MUST include both the requested IP (option 50) and the server
     * identifier (option 54) in REQUEST during SELECTING. RFC 2131 §4.3.2. */
    o = opt_put(o, DHCP_OPT_REQUESTED_IP, 4, &requested_ip_be);
    o = opt_put(o, DHCP_OPT_SERVER_ID,    4, &server_id_be);

    uint8_t cid[1 + ETH_ADDR_LEN];
    cid[0] = DHCP_HTYPE_ETH;
    memcpy(&cid[1], g_my_mac, ETH_ADDR_LEN);
    o = opt_put(o, DHCP_OPT_CLIENT_ID, sizeof(cid), cid);

    uint8_t prl[3] = { DHCP_OPT_SUBNET, DHCP_OPT_ROUTER, DHCP_OPT_DNS };
    o = opt_put(o, DHCP_OPT_PARAM_REQ, sizeof(prl), prl);

    o = opt_end(o);
    (void)o;

    char ib[16];
    net_format_ip(ib, requested_ip_be);
    kprintf("[dhcp] REQUEST requested=%s\n", ib);

    return udp_send(htons(DHCP_PORT_CLIENT),
                    0xFFFFFFFFu,
                    htons(DHCP_PORT_SERVER),
                    &pkt, DHCP_PKT_LEN);
}

/* ---- RX hook ---------------------------------------------------- */

void dhcp_recv_hook(uint32_t src_ip_be, const void *udp_packet, size_t len) {
    if (!g_xact.in_flight) return;
    if (len < 8 + 240) return;                       /* UDP hdr + BOOTP fixed */

    /* Skip the 8-byte UDP header (udp_recv passes us the UDP packet
     * starting at the UDP header). */
    const uint8_t       *u = (const uint8_t *)udp_packet;
    const struct dhcp_pkt *p = (const struct dhcp_pkt *)(u + 8);
    size_t                pl = len - 8;

    if (pl < DHCP_PKT_LEN - DHCP_OPTIONS_LEN) return;
    if (p->op    != DHCP_OP_REPLY) return;
    if (p->htype != DHCP_HTYPE_ETH) return;
    if (p->magic != DHCP_MAGIC_BE) return;
    if (p->xid   != g_xact.xid) return;              /* not for us */

    if (!dhcp_chaddr_ok(p)) return;

    /* Parse the options blob. opts_len is up to whatever followed the
     * 240-byte fixed header in this datagram (capped at our struct). */
    size_t opt_avail = pl - (DHCP_PKT_LEN - DHCP_OPTIONS_LEN);
    if (opt_avail > DHCP_OPTIONS_LEN) opt_avail = DHCP_OPTIONS_LEN;

    uint8_t mt_len = 0;
    const uint8_t *mt =
        dhcp_opt_find(p, opt_avail, DHCP_OPT_MSG_TYPE, &mt_len);
    if (!mt || mt_len < 1) return;

    if (mt[0] == DHCP_MSG_OFFER) {
        /* Capture yiaddr + server-id + the lease parameters now (so a
         * later ACK only needs to confirm). */
        uint8_t l;
        const uint8_t *sid =
            dhcp_opt_find(p, opt_avail, DHCP_OPT_SERVER_ID, &l);
        uint32_t server_be = 0;
        if (sid && l >= 4) {
            server_be = opt_u32_be(sid);
        } else if (src_ip_be != 0) {
            /* RFC 2131 expects option 54; some CPEs omit it on broadcast
             * OFFERs. The UDP source address is the same server in the
             * common no-relay case. */
            server_be = src_ip_be;
        } else {
            return;
        }
        g_xact.offered_ip_be = p->yiaddr;
        g_xact.server_id_be  = server_be;
        g_xact.saw_offer     = true;

        char yb[16], sb[16];
        net_format_ip(yb, g_xact.offered_ip_be);
        net_format_ip(sb, g_xact.server_id_be);
        kprintf("[dhcp] OFFER  yiaddr=%s server=%s\n", yb, sb);
        return;
    }

    if (*mt == DHCP_MSG_NAK) {
        kprintf("[dhcp] NAK received -- abandoning lease\n");
        g_xact.saw_nak = true;
        return;
    }

    if (*mt == DHCP_MSG_ACK) {
        /* yiaddr in ACK should equal what we requested. */
        if (p->yiaddr != g_xact.offered_ip_be) {
            char a[16], b[16];
            net_format_ip(a, g_xact.offered_ip_be);
            net_format_ip(b, p->yiaddr);
            kprintf("[dhcp] WARN: ACK yiaddr=%s != requested %s -- accepting anyway\n",
                    b, a);
        }

        struct dhcp_lease *L = &g_xact.lease;
        L->ip_be      = p->yiaddr;
        L->server_be  = g_xact.server_id_be;
        L->lease_secs = 0;
        L->netmask_be = 0;
        L->gateway_be = 0;
        L->dns_be     = 0;

        uint8_t l;
        const uint8_t *v;
        if ((v = dhcp_opt_find(p, opt_avail, DHCP_OPT_SUBNET, &l)) && l == 4)
            L->netmask_be = opt_u32_be(v);
        if ((v = dhcp_opt_find(p, opt_avail, DHCP_OPT_ROUTER, &l)) && l >= 4)
            L->gateway_be = opt_u32_be(v);
        if ((v = dhcp_opt_find(p, opt_avail, DHCP_OPT_DNS, &l)) && l >= 4)
            L->dns_be = opt_u32_be(v);   /* keep first DNS server */
        if ((v = dhcp_opt_find(p, opt_avail, DHCP_OPT_LEASE_TIME, &l)) && l == 4)
            L->lease_secs = opt_u32_host(v);

        char ib[16], mb[16], gb[16], db[16], sb[16];
        net_format_ip(ib, L->ip_be);
        net_format_ip(mb, L->netmask_be);
        net_format_ip(gb, L->gateway_be);
        net_format_ip(db, L->dns_be);
        net_format_ip(sb, L->server_be);
        kprintf("[dhcp] ACK    bound: ip=%s mask=%s gw=%s dns=%s lease=%us (server=%s)\n",
                ib, mb, gb, db, (unsigned)L->lease_secs, sb);

        g_xact.saw_ack = true;
        return;
    }

    /* DECLINE / RELEASE / INFORM are not for clients to receive. */
}

/* ---- main entry point ------------------------------------------- */

/* Cheap deterministic-ish xid: TSC tick xor MAC xor pit_ticks(). The
 * server only requires that the xid round-trips intact, so we don't
 * need cryptographic randomness here. */
static uint32_t dhcp_make_xid(void) {
    uint64_t t = (uint64_t)pit_ticks() * 0x9E3779B97F4A7C15ull;
    t ^= ((uint64_t)g_my_mac[0] << 0)
      |  ((uint64_t)g_my_mac[1] << 8)
      |  ((uint64_t)g_my_mac[2] << 16)
      |  ((uint64_t)g_my_mac[3] << 24)
      |  ((uint64_t)g_my_mac[4] << 32)
      |  ((uint64_t)g_my_mac[5] << 40);
    return (uint32_t)(t ^ (t >> 32));
}

/* Drain RX for up to `deadline` PIT ticks or until `predicate` is true.
 * Returns true if predicate became true; false on timeout. */
static bool dhcp_wait(uint64_t deadline, const bool *flag, const bool *flag_alt) {
    struct net_dev *nd = net_default();
    while (pit_ticks() < deadline) {
        if (nd && nd->rx_drain) nd->rx_drain(nd);
        if (*flag) return true;
        if (flag_alt && *flag_alt) return true;
        sti();
        hlt();
    }
    return *flag || (flag_alt && *flag_alt);
}

bool dhcp_acquire(uint32_t timeout_ms, struct dhcp_lease *out) {
    if (!out) return false;

    struct net_dev *nd = net_default();
    if (!nd) {
        kprintf("[dhcp] no NIC -- cannot run handshake\n");
        return false;
    }

    /* Reset transient state. From here on dhcp_recv_hook will engage. */
    memset(&g_xact, 0, sizeof(g_xact));
    g_xact.xid       = htonl(dhcp_make_xid());
    g_xact.in_flight = true;

    uint32_t hz = pit_hz(); if (hz == 0) hz = 100;
    uint64_t t0        = pit_ticks();
    uint64_t end_total = t0 + ((uint64_t)hz * (uint64_t)timeout_ms) / 1000u;

    /* ---- steps 1–2: DISCOVER (possibly repeated) + wait for OFFER/NAK ---- */
    uint64_t total_ticks       = end_total - t0;
    uint64_t offer_phase_ticks = total_ticks * DHCP_OFFER_PHASE_NUM /
                                 DHCP_OFFER_PHASE_DEN;
    if (offer_phase_ticks == 0) offer_phase_ticks = 1;
    uint64_t mid_deadline = t0 + offer_phase_ticks;

    unsigned n_discover = 0;
    while (pit_ticks() < mid_deadline && !g_xact.saw_offer && !g_xact.saw_nak) {
        n_discover++;
        if (!dhcp_send_discover(g_xact.xid)) {
            kprintf("[dhcp] DISCOVER send failed (NIC tx error)\n");
            g_xact.in_flight = false;
            return false;
        }

        uint64_t chunk_end =
            pit_ticks() + ((uint64_t)hz * (uint64_t)DHCP_DISCOVER_GAP_MS) / 1000u;
        if (chunk_end > mid_deadline) chunk_end = mid_deadline;
        if (chunk_end <= pit_ticks()) chunk_end = pit_ticks() + 1;

        (void)dhcp_wait(chunk_end, &g_xact.saw_offer, &g_xact.saw_nak);
    }

    if (!g_xact.saw_offer && !g_xact.saw_nak) {
        uint32_t offer_ms =
            (uint32_t)((uint64_t)timeout_ms * DHCP_OFFER_PHASE_NUM /
                       DHCP_OFFER_PHASE_DEN);
        kprintf("[dhcp] timeout waiting for OFFER (%u DISCOVER, %u ms offer phase)\n",
                n_discover, (unsigned)offer_ms);
        g_xact.in_flight = false;
        return false;
    }
    if (g_xact.saw_nak) {
        g_xact.in_flight = false;
        return false;
    }

    /* ---- step 3: REQUEST ---- */
    if (!dhcp_send_request(g_xact.xid, g_xact.offered_ip_be, g_xact.server_id_be)) {
        kprintf("[dhcp] REQUEST send failed (NIC tx error)\n");
        g_xact.in_flight = false;
        return false;
    }

    /* ---- step 4: wait for ACK (or NAK) ---- */
    if (!dhcp_wait(end_total, &g_xact.saw_ack, &g_xact.saw_nak)) {
        kprintf("[dhcp] timeout waiting for ACK\n");
        g_xact.in_flight = false;
        return false;
    }
    if (g_xact.saw_nak) {
        g_xact.in_flight = false;
        return false;
    }

    *out = g_xact.lease;
    g_xact.in_flight = false;
    return true;
}
