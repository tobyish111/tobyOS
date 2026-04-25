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
    (void)src_ip_be;
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

    /* MAC mirror check: server should echo our MAC in chaddr. */
    if (memcmp(p->chaddr, g_my_mac, ETH_ADDR_LEN) != 0) return;

    /* Parse the options blob. opts_len is up to whatever followed the
     * 240-byte fixed header in this datagram (capped at our struct). */
    size_t opt_avail = pl - (DHCP_PKT_LEN - DHCP_OPTIONS_LEN);
    if (opt_avail > DHCP_OPTIONS_LEN) opt_avail = DHCP_OPTIONS_LEN;

    uint8_t mt_len = 0;
    const uint8_t *mt = opt_find(p->options, opt_avail,
                                 DHCP_OPT_MSG_TYPE, &mt_len);
    if (!mt || mt_len != 1) return;

    if (*mt == DHCP_MSG_OFFER) {
        /* Capture yiaddr + server-id + the lease parameters now (so a
         * later ACK only needs to confirm). */
        uint8_t l;
        const uint8_t *sid = opt_find(p->options, opt_avail,
                                      DHCP_OPT_SERVER_ID, &l);
        if (!sid || l != 4) return;
        g_xact.offered_ip_be = p->yiaddr;
        g_xact.server_id_be  = opt_u32_be(sid);
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
        if ((v = opt_find(p->options, opt_avail, DHCP_OPT_SUBNET, &l)) && l == 4)
            L->netmask_be = opt_u32_be(v);
        if ((v = opt_find(p->options, opt_avail, DHCP_OPT_ROUTER, &l)) && l >= 4)
            L->gateway_be = opt_u32_be(v);
        if ((v = opt_find(p->options, opt_avail, DHCP_OPT_DNS, &l)) && l >= 4)
            L->dns_be = opt_u32_be(v);   /* keep first DNS server */
        if ((v = opt_find(p->options, opt_avail, DHCP_OPT_LEASE_TIME, &l)) && l == 4)
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
    uint64_t end_total = pit_ticks() + ((uint64_t)hz * timeout_ms) / 1000u;

    /* ---- step 1: DISCOVER ---- */
    if (!dhcp_send_discover(g_xact.xid)) {
        kprintf("[dhcp] DISCOVER send failed (NIC tx error)\n");
        g_xact.in_flight = false;
        return false;
    }

    /* ---- step 2: wait for OFFER (or NAK) ---- *
     *
     * Half the budget for the OFFER, the rest for the ACK. SLIRP and
     * dnsmasq both reply within a few ms; this is generous. */
    uint64_t mid_deadline = pit_ticks() + (end_total - pit_ticks()) / 2;
    if (!dhcp_wait(mid_deadline, &g_xact.saw_offer, &g_xact.saw_nak)) {
        kprintf("[dhcp] timeout waiting for OFFER (after %u ms)\n",
                (unsigned)timeout_ms / 2);
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
