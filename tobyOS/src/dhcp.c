/* dhcp.c -- DHCPv4 boot client (RFC 2131 / 2132).
 *
 * One exchange, five logical steps (lease commit is the caller, net.c):
 *
 *   1. DHCPDISCOVER (broadcast IP, BOOTP broadcast flag; src 0.0.0.0)
 *   2. Wait DHCPOFFER — yiaddr + server-id (option 54 / UDP src / siaddr / .1)
 *   3. DHCPREQUEST with offered IP + server-id
 *   4. Wait DHCPACK (or NAK → fail)
 *   5. Caller applies ip / netmask / router / DNS from `struct dhcp_lease`
 *
 * RX: udp_recv() → dhcp_recv_hook() on UDP dst port 68. IPv4 must accept
 * yiaddr-unicast while g_my_ip==0 (see ip_dst_is_for_us in ip.c).
 *
 * Invariants (do not “tighten” without a capture on the same boot):
 *   - Session binding is xid + DHCP magic + in_flight — not chaddr/61 on
 *     OFFER (many CPEs broadcast OFFERs with empty/wrong client hw fields).
 *   - dhcp_wait drains every registered NIC: OFFER/ACK can land on any
 *     probed device; draining only net_default() misses replies if the
 *     default slot is not the port that actually RX'd the broadcast.
 *
 * Real hardware gotchas fixed here and in eth.c:
 *   - Tagged Ethernet (802.1Q / 802.1ad): OFFER/ACK may arrive VLAN-tagged;
 *     DISCOVER is often untagged — eth_recv must peel tags or IPv4 is never
 *     demuxed (DISCOVER “works” in Wireshark on a trunk, host stack sees nothing).
 *   - yiaddr==0 with address only in DHCP option 50 on OFFER — still accept.
 */

#include <tobyos/dhcp.h>
#include <tobyos/net.h>
#include <tobyos/udp.h>
#include <tobyos/pit.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* Fraction of `timeout_ms` spent on step 2 (repeated DISCOVER + wait OFFER);
 * the rest is for step 4 (wait ACK after REQUEST). */
#ifndef DHCP_OFFER_MS_NUM
#define DHCP_OFFER_MS_NUM 70u
#endif
#ifndef DHCP_OFFER_MS_DEN
#define DHCP_OFFER_MS_DEN 100u
#endif
/* After the *first* DISCOVER: listen this long before sending another.
 * Short gaps (e.g. 400 ms) spam DISCOVER while the server is still sending
 * OFFER(s) and can confuse capture / some CPEs; RFC 2131 implies backoff. */
#ifndef DHCP_DISCOVER_INITIAL_WAIT_MS
#define DHCP_DISCOVER_INITIAL_WAIT_MS 2500u
#endif
/* Gap between 2nd, 3rd, … DISCOVER while waiting for OFFER. */
#ifndef DHCP_DISCOVER_RETRY_GAP_MS
#define DHCP_DISCOVER_RETRY_GAP_MS 1200u
#endif
/* Legacy override: if set in Makefile, use as retry gap (initial stays default). */
#ifdef DHCP_DISCOVER_GAP_MS
#undef DHCP_DISCOVER_RETRY_GAP_MS
#define DHCP_DISCOVER_RETRY_GAP_MS DHCP_DISCOVER_GAP_MS
#endif

/* Each dhcp_wait loop runs this many full drain passes before sti+hlt.
 * Noisy LANs can deliver many frames between sleep wakeups; RTL8169 keeps a
 * 32-descriptor RX ring — without burst drains we can drop valid OFFER/ACK
 * while a port mirror still shows the packets. */
#ifndef DHCP_RX_DRAIN_BURST
#define DHCP_RX_DRAIN_BURST 48u
#endif

/* ---- transient transaction state -------------------------------- *
 *
 * Lives only across one dhcp_acquire() invocation. After the call
 * returns g_xact.in_flight is cleared so dhcp_recv_hook drops any
 * late-arriving frames. */
struct dhcp_xact {
    /* volatile: dhcp_recv_hook may run from NIC MSI while dhcp_wait()
     * is sleeping (sti+hlt). Without volatile, -O2 can cache saw_* in a
     * register across hlt() and we miss OFFER/ACK forever — intermittent
     * "DHCP worked before" failures. */
    volatile bool     in_flight;
    volatile uint32_t xid;         /* network byte order; volatile vs in_flight */

    /* __atomic uint8_t: main does store(1, RELEASE) before REQUEST TX; hook
     * loads with ACQUIRE so no OFFER handler can publish yiaddr after we've
     * committed to REQUEST (SMP-safe, not only compiler ordering). */
    uint8_t request_sent;

    /* Captured from OFFER, used to send REQUEST and verify ACK. */
    uint32_t      offered_ip_be;
    uint32_t      server_id_be;
    volatile bool saw_offer;
    volatile bool saw_ack;
    volatile bool saw_nak;

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
 * option 53 and DHCP appears "broken".
 *
 * `bp` = start of BOOTP payload (UDP body), `pl` = BOOTP byte length.
 * We walk **all** bytes after DHCP_BOOTP_MIN_BYTES fixed header (no 312 cap):
 * some CPEs ship >552 B BOOTP where option 53 sits past the legacy
 * options field — capping at DHCP_OPTIONS_LEN made Wireshark show OFFER
 * while we never accepted it. */
static const uint8_t *dhcp_opt_find_wire(const uint8_t *bp, size_t pl,
                                         uint8_t want_code,
                                         uint8_t *out_len) {
    if (pl <= DHCP_BOOTP_MIN_BYTES) return NULL;
    size_t         opt_len = pl - DHCP_BOOTP_MIN_BYTES;
    const uint8_t *opts    = bp + DHCP_BOOTP_MIN_BYTES;

    const uint8_t *v = opt_find(opts, opt_len, want_code, out_len);
    if (v) return v;

    uint8_t        ollen = 0;
    const uint8_t *ol  = opt_find(opts, opt_len, DHCP_OPT_OVERLOAD, &ollen);
    if (!ol || ollen < 1) return NULL;

    uint8_t flags = ol[0];
    /* BOOTP offsets: sname 44..107 (64 B), file 108..235 (128 B). */
    if (flags & 1u) {
        v = opt_find(bp + 108, 128, want_code, out_len);
        if (v) return v;
    }
    if (flags & 2u) {
        v = opt_find(bp + 44, 64, want_code, out_len);
        if (v) return v;
    }
    return NULL;
}

static uint32_t bootp_ipv4_at(const uint8_t *bp, unsigned off) {
    uint32_t v;
    memcpy(&v, bp + off, 4);
    return v;
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

/* yiaddr (BOOTP offset 16) is the usual offered lease IP on DHCPOFFER/DHCPACK.
 * Some gateways leave yiaddr clear and put the address only in option 50; if we
 * insist on yiaddr alone, OFFERs look valid in Wireshark but we never REQUEST. */
static uint32_t bootp_offered_ip_be(const uint8_t *bp, size_t pl) {
    uint32_t yi = bootp_ipv4_at(bp, 16);
    if (yi != 0) return yi;
    uint8_t         l50 = 0;
    const uint8_t *p50 =
        dhcp_opt_find_wire(bp, pl, DHCP_OPT_REQUESTED_IP, &l50);
    if (p50 && l50 >= 4) return opt_u32_be(p50);
    return 0;
}

/* ---- TX: build DISCOVER / REQUEST ------------------------------- */

/* RFC 1497 / 2131 cookie on the wire is always bytes 99,130,83,99 — do not
 * assign DHCP_MAGIC_BE as uint32_t on little-endian hosts or the on-wire
 * order becomes 63 53 82 63 and some servers ignore the packet. */
static const uint8_t dhcp_magic_cookie[4] = { 99, 130, 83, 99 };

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
    memcpy(&p->magic, dhcp_magic_cookie, 4);
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

/* Rate-limited: explains why a BOOTP/DHCP reply was not applied (bootlog). */
static void dhcp_rx_drop(unsigned *ctr, unsigned max, const char *why) {
    if (*ctr >= max) return;
    (*ctr)++;
    kprintf("[dhcp] rx drop (%u/%u): %s\n", *ctr, max, why);
}

void dhcp_recv_hook(uint32_t src_ip_be, const void *udp_packet, size_t len) {
    if (len < 8u + DHCP_BOOTP_MIN_BYTES) {
        static unsigned n_len;
        if (g_xact.in_flight && n_len++ < 4u)
            kprintf("[dhcp] rx drop: len=%u need>=%u\n", (unsigned)len,
                    (unsigned)(8u + DHCP_BOOTP_MIN_BYTES));
        return;
    }

    /* Skip the 8-byte UDP header (udp_recv passes us the UDP packet
     * starting at the UDP header). Parse BOOTP from raw offsets so we
     * are not limited by sizeof(struct dhcp_pkt) or a 312 B options cap. */
    const uint8_t *u  = (const uint8_t *)udp_packet;
    const uint8_t *bp = u + 8;
    size_t         pl = len - 8;

    if (pl < DHCP_BOOTP_MIN_BYTES) {
        static unsigned n_pl;
        if (g_xact.in_flight && n_pl++ < 4u)
            kprintf("[dhcp] rx drop: bootp pl=%u need>=%u\n", (unsigned)pl,
                    (unsigned)DHCP_BOOTP_MIN_BYTES);
        return;
    }
    if (bp[0] != DHCP_OP_REPLY) {
        static unsigned n_op;
        if (g_xact.in_flight && n_op++ < 4u)
            kprintf("[dhcp] rx drop: BOOTP op=%u not REPLY(2)\n",
                    (unsigned)bp[0]);
        return;
    }
    /* Match xid before in_flight: avoids dropping unrelated BOOTP noise,
     * and pairs with a release fence after xid is written in dhcp_acquire(). */
    {
        uint32_t want_xid;
        memcpy(&want_xid, bp + 4, 4);
        if (want_xid != g_xact.xid) {
            static unsigned xid_log;
            if (g_xact.in_flight && xid_log++ < 4u)
                kprintf("[dhcp] rx xid mismatch wire=%08x expect=%08x\n",
                        (unsigned)ntohl(want_xid), (unsigned)ntohl(g_xact.xid));
            return;
        }
    }

    /* DHCP magic at BOOTP offset 236..239; need full DHCP_BOOTP_MIN_BYTES. */
    if (memcmp(bp + 236, dhcp_magic_cookie, 4) != 0) {
        static unsigned badmagic;
        if (g_xact.in_flight && badmagic++ < 8u) {
            kprintf("[dhcp] rx drop: bad DHCP magic @236 (got %02x %02x %02x %02x)\n",
                    (unsigned)bp[236], (unsigned)bp[237],
                    (unsigned)bp[238], (unsigned)bp[239]);
        }
        return;
    }

    if (!g_xact.in_flight) {
        static unsigned noflight;
        if (noflight++ < 2u)
            kprintf("[dhcp] rx BOOTP reply but no DHCP transaction in flight\n");
        return;
    }

    /* Do not require chaddr / option-61 to match. Several home gateways
     * (Deco-style, etc.) broadcast OFFERs with an empty or non-echoed
     * client hw field while still using our xid — Wireshark shows valid
     * OFFERs; we used to drop every one here and loop on DISCOVER forever. */

    uint8_t              mt_len = 0;
    const uint8_t       *mt     =
        dhcp_opt_find_wire(bp, pl, DHCP_OPT_MSG_TYPE, &mt_len);
    uint8_t              inferred = 0;
    if (!mt || mt_len < 1) {
        uint32_t yi = bootp_offered_ip_be(bp, pl);
        /* SELECTING: no option 53 but offered IP present — treat as OFFER. */
        if (!g_xact.saw_offer && yi != 0) {
            inferred   = DHCP_MSG_OFFER;
            mt         = &inferred;
            mt_len     = 1;
        } else if (__atomic_load_n(&g_xact.request_sent, __ATOMIC_ACQUIRE) &&
                   yi != 0 && yi == g_xact.offered_ip_be) {
            /* REQUESTING: rare servers omit option 53 on ACK; yiaddr matches. */
            inferred   = DHCP_MSG_ACK;
            mt         = &inferred;
            mt_len     = 1;
        } else {
            static unsigned n_infer;
            if (g_xact.in_flight && n_infer++ < 10u) {
                char yb[16];
                net_format_ip(yb, yi);
                kprintf("[dhcp] rx drop: no-opt53 infer yi=%s saw_off=%d rq=%d\n",
                        yb, (int)g_xact.saw_offer,
                        (int)__atomic_load_n(&g_xact.request_sent,
                                             __ATOMIC_ACQUIRE));
            }
            return;
        }
    }

    /* Garbled or vendor option 53 — if yiaddr is set while selecting, treat
     * as OFFER so we can still REQUEST (xid already matches this session). */
    if (mt && mt_len >= 1u && !g_xact.saw_offer &&
        !__atomic_load_n(&g_xact.request_sent, __ATOMIC_ACQUIRE)) {
        uint8_t t = mt[0];
        if (t != DHCP_MSG_OFFER && t != DHCP_MSG_NAK && t != DHCP_MSG_ACK) {
            uint32_t yi_guess = bootp_offered_ip_be(bp, pl);
            if (yi_guess != 0) {
                inferred   = DHCP_MSG_OFFER;
                mt         = &inferred;
                mt_len     = 1;
            }
        }
    }

    if (mt[0] == DHCP_MSG_OFFER) {
        if (__atomic_load_n(&g_xact.request_sent, __ATOMIC_ACQUIRE)) {
            static unsigned n_off_rs;
            dhcp_rx_drop(&n_off_rs, 6u, "OFFER after REQUEST already sent");
            return;
        }
        if (g_xact.saw_offer) {
            static unsigned n_off_dup;
            dhcp_rx_drop(&n_off_dup, 6u, "OFFER duplicate (already selected one)");
            return;
        }

        /* Capture yiaddr (or option 50) + server-id + lease hints now. */
        uint32_t yi = bootp_offered_ip_be(bp, pl);
        if (yi == 0) {
            static unsigned n_off_yi;
            dhcp_rx_drop(&n_off_yi, 8u, "OFFER yiaddr and opt50 both empty");
            return;
        }

        uint8_t l;
        const uint8_t *sid =
            dhcp_opt_find_wire(bp, pl, DHCP_OPT_SERVER_ID, &l);
        uint32_t server_be = 0;
        if (sid && l >= 4) {
            server_be = opt_u32_be(sid);
        } else if (src_ip_be != 0) {
            /* RFC 2131 expects option 54; some CPEs omit it on broadcast
             * OFFERs. The UDP source address is the same server in the
             * common no-relay case. */
            server_be = src_ip_be;
        } else {
            uint32_t si = bootp_ipv4_at(bp, 20); /* BOOTP siaddr */
            if (si != 0) {
                server_be = si;
            } else {
                /* Last resort: many home /24 LANs use .1 as the DHCP server.
                 * If the frame truly has no src IP and no opt54/siaddr, this
                 * unblocks REQUEST; wrong subnet .1 yields NAK, not a hang. */
                const uint8_t *yb = (const uint8_t *)&yi;
                server_be = ip4(yb[0], yb[1], yb[2], 1u);
                char sb[16];
                net_format_ip(sb, server_be);
                kprintf("[dhcp] OFFER: guessed server-id %s (no opt54/src/siaddr)\n",
                        sb);
            }
        }
        g_xact.offered_ip_be = yi;
        g_xact.server_id_be  = server_be;
        /* RELEASE: offered_ip_be / server_id_be must be visible before saw_offer
         * is observed true on the BSP (and on other CPUs if RX runs there). */
        __atomic_thread_fence(__ATOMIC_RELEASE);
        g_xact.saw_offer = true;

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
        uint32_t yi = bootp_offered_ip_be(bp, pl);
        /* yiaddr in ACK should equal what we requested. */
        if (yi != g_xact.offered_ip_be) {
            char a[16], b[16];
            net_format_ip(a, g_xact.offered_ip_be);
            net_format_ip(b, yi);
            kprintf("[dhcp] WARN: ACK yiaddr=%s != requested %s -- accepting anyway\n",
                    b, a);
        }

        struct dhcp_lease *L = &g_xact.lease;
        L->ip_be      = yi;
        L->server_be  = g_xact.server_id_be;
        L->lease_secs = 0;
        L->netmask_be = 0;
        L->gateway_be = 0;
        L->dns_be     = 0;

        uint8_t l;
        const uint8_t *v;
        if ((v = dhcp_opt_find_wire(bp, pl, DHCP_OPT_SUBNET, &l)) && l == 4)
            L->netmask_be = opt_u32_be(v);
        if ((v = dhcp_opt_find_wire(bp, pl, DHCP_OPT_ROUTER, &l)) && l >= 4)
            L->gateway_be = opt_u32_be(v);
        if ((v = dhcp_opt_find_wire(bp, pl, DHCP_OPT_DNS, &l)) && l >= 4)
            L->dns_be = opt_u32_be(v);   /* keep first DNS server */
        if ((v = dhcp_opt_find_wire(bp, pl, DHCP_OPT_LEASE_TIME, &l)) && l == 4)
            L->lease_secs = opt_u32_host(v);

        char ib[16], mb[16], gb[16], db[16], sb[16];
        net_format_ip(ib, L->ip_be);
        net_format_ip(mb, L->netmask_be);
        net_format_ip(gb, L->gateway_be);
        net_format_ip(db, L->dns_be);
        net_format_ip(sb, L->server_be);
        kprintf("[dhcp] ACK    bound: ip=%s mask=%s gw=%s dns=%s lease=%us (server=%s)\n",
                ib, mb, gb, db, (unsigned)L->lease_secs, sb);

        __atomic_thread_fence(__ATOMIC_RELEASE);
        g_xact.saw_ack = true;
        return;
    }

    {
        static unsigned n_unk;
        if (g_xact.in_flight && n_unk++ < 10u)
            kprintf("[dhcp] rx drop: unhandled opt53 msg=%u\n", (unsigned)mt[0]);
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

/* Drain RX on all registered NICs so a broadcast OFFER is not missed when
 * the second (or first) probed controller is the one the switch delivered on. */
static void dhcp_rx_drain_all(void) {
    for (size_t i = 0; i < net_dev_count(); i++) {
        struct net_dev *d = net_dev_get(i);
        if (d && d->rx_drain) d->rx_drain(d);
    }
}

/* Drain RX for up to `deadline` PIT ticks or until `predicate` is true.
 * Returns true if predicate became true; false on timeout. */
static bool dhcp_wait(uint64_t deadline, const volatile bool *flag,
                      const volatile bool *flag_alt) {
    /* Poll every registered NIC — DHCP broadcasts may be received on a
     * non-default device depending on PCI probe order vs cabling. */
    for (unsigned spin = 0; spin < 24u && pit_ticks() < deadline; spin++) {
        for (unsigned b = 0; b < DHCP_RX_DRAIN_BURST; b++) {
            dhcp_rx_drain_all();
            if (*flag) return true;
            if (flag_alt && *flag_alt) return true;
        }
    }
    while (pit_ticks() < deadline) {
        for (unsigned b = 0; b < DHCP_RX_DRAIN_BURST; b++) {
            dhcp_rx_drain_all();
            if (*flag) return true;
            if (flag_alt && *flag_alt) return true;
        }
        sti();
        hlt();
        /* Ordering vs NIC MSI path that mutates saw_* while we sleep. */
        __asm__ volatile("" ::: "memory");
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

    /* Publish xid + in_flight with IRQs off so no RX runs mid-handshake setup. */
    uint64_t irqf = cpu_irqsave();
    memset(&g_xact, 0, sizeof(g_xact));
    g_xact.xid = htonl(dhcp_make_xid());
    __asm__ volatile("" ::: "memory");
    g_xact.in_flight = true;
    cpu_irqrestore(irqf);

    uint32_t hz = pit_hz(); if (hz == 0) hz = 100;
    uint64_t t0        = pit_ticks();
    uint64_t end_total = t0 + ((uint64_t)hz * (uint64_t)timeout_ms) / 1000u;

    /* Steps 1–2: DISCOVER, wait OFFER (retransmit DISCOVER until offer window). */
    uint64_t total_ticks = end_total - t0;
    uint64_t offer_until = t0 + total_ticks * DHCP_OFFER_MS_NUM / DHCP_OFFER_MS_DEN;
    if (offer_until <= t0) offer_until = t0 + 1;

    unsigned n_discover = 0;
    /* Drop stale BOOTP noise before we fix the session xid. */
    dhcp_rx_drain_all();

    while (pit_ticks() < offer_until && !g_xact.saw_offer && !g_xact.saw_nak) {
        n_discover++;
        if (!dhcp_send_discover(g_xact.xid)) {
            kprintf("[dhcp] DISCOVER send failed (NIC tx error)\n");
            g_xact.in_flight = false;
            return false;
        }

        /* OFFER can land immediately; drain hard before we sleep the window. */
        for (unsigned spin = 0; spin < DHCP_RX_DRAIN_BURST * 2u; spin++)
            dhcp_rx_drain_all();
        if (g_xact.saw_offer || g_xact.saw_nak)
            break;

        uint32_t gap_ms = (n_discover == 1u) ? DHCP_DISCOVER_INITIAL_WAIT_MS
                                             : DHCP_DISCOVER_RETRY_GAP_MS;
        uint64_t chunk_end =
            pit_ticks() + ((uint64_t)hz * (uint64_t)gap_ms) / 1000u;
        if (chunk_end > offer_until) chunk_end = offer_until;
        if (chunk_end <= pit_ticks()) chunk_end = pit_ticks() + 1;

        (void)dhcp_wait(chunk_end, &g_xact.saw_offer, &g_xact.saw_nak);
    }

    if (!g_xact.saw_offer && !g_xact.saw_nak) {
        uint32_t offer_budget_ms =
            (uint32_t)((uint64_t)timeout_ms * DHCP_OFFER_MS_NUM / DHCP_OFFER_MS_DEN);
        kprintf("[dhcp] timeout waiting for OFFER (%u DISCOVER, %u ms) xid=0x%08x\n",
                n_discover, (unsigned)offer_budget_ms,
                (unsigned)ntohl(g_xact.xid));
        g_xact.in_flight = false;
        return false;
    }
    if (g_xact.saw_nak) {
        g_xact.in_flight = false;
        return false;
    }

    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    /* Step 3: REQUEST (yiaddr + server-id from OFFER). */
    if (g_xact.server_id_be == 0) {
        kprintf("[dhcp] OFFER had no server-id — cannot send valid REQUEST\n");
        g_xact.in_flight = false;
        return false;
    }
    __atomic_store_n(&g_xact.request_sent, 1u, __ATOMIC_RELEASE);
    if (!dhcp_send_request(g_xact.xid, g_xact.offered_ip_be, g_xact.server_id_be)) {
        kprintf("[dhcp] REQUEST send failed (NIC tx error)\n");
        g_xact.in_flight = false;
        return false;
    }

    /* Step 4: wait ACK (or NAK). Step 5: caller copies lease → net_apply_lease. */
    if (!dhcp_wait(end_total, &g_xact.saw_ack, &g_xact.saw_nak)) {
        kprintf("[dhcp] timeout waiting for ACK\n");
        g_xact.in_flight = false;
        return false;
    }
    if (g_xact.saw_nak) {
        g_xact.in_flight = false;
        return false;
    }

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    *out = g_xact.lease;
    g_xact.in_flight = false;
    return true;
}
