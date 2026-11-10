/* icmpv6.c -- ICMPv6 echo reply + Neighbor Discovery (RFC 4861).
 *
 * Handles:
 *   - Echo Request → Echo Reply  (ping6)
 *   - Neighbor Solicitation → Neighbor Advertisement
 *   - Neighbor Advertisement → cache update
 *
 * The neighbor cache is 16 entries, link-local scope only.
 * ICMPv6 checksum includes a pseudo-header per RFC 4443.
 */

#include <tobyos/icmpv6.h>
#include <tobyos/ipv6.h>
#include <tobyos/dhcpv6.h>
#include <tobyos/eth.h>
#include <tobyos/net.h>
#include <tobyos/perf.h>   /* perf_now_ns: the bounded NA wait (2026-08-23) */
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* ---- checksum --------------------------------------------------- */

static uint16_t icmpv6_checksum(const struct ipv6_addr *src,
                                const struct ipv6_addr *dst,
                                const void *payload, size_t len) {
    uint32_t sum = 0;

    /* Pseudo-header: src(16) + dst(16) + upper-layer length(4) + next-hdr(4) */
    const uint16_t *s = (const uint16_t *)src->bytes;
    const uint16_t *d = (const uint16_t *)dst->bytes;
    for (int i = 0; i < 8; i++) {
        sum += ntohs(s[i]);
        sum += ntohs(d[i]);
    }
    sum += (uint32_t)len;
    sum += IPV6_NH_ICMPV6;

    /* Payload */
    const uint8_t *p = (const uint8_t *)payload;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
    if (len & 1)
        sum += (uint32_t)p[len - 1] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return htons(~(uint16_t)sum);
}

/* ---- neighbor cache --------------------------------------------- */

#define ND_CACHE_SIZE 16

struct nd_entry {
    bool             valid;
    struct ipv6_addr ip;
    uint8_t          mac[6];
};

static struct nd_entry g_nd_cache[ND_CACHE_SIZE];

static struct nd_entry *nd_lookup(const struct ipv6_addr *ip) {
    for (int i = 0; i < ND_CACHE_SIZE; i++) {
        if (g_nd_cache[i].valid && ipv6_addr_equal(&g_nd_cache[i].ip, ip))
            return &g_nd_cache[i];
    }
    return NULL;
}

static void nd_insert(const struct ipv6_addr *ip, const uint8_t mac[6]) {
    /* Update existing */
    struct nd_entry *e = nd_lookup(ip);
    if (e) {
        memcpy(e->mac, mac, 6);
        return;
    }
    /* Find free slot */
    for (int i = 0; i < ND_CACHE_SIZE; i++) {
        if (!g_nd_cache[i].valid) {
            g_nd_cache[i].valid = true;
            g_nd_cache[i].ip = *ip;
            memcpy(g_nd_cache[i].mac, mac, 6);
            return;
        }
    }
    /* Evict slot 0 */
    g_nd_cache[0].valid = true;
    g_nd_cache[0].ip = *ip;
    memcpy(g_nd_cache[0].mac, mac, 6);
}

bool icmpv6_resolve(const struct ipv6_addr *ip, uint8_t mac_out[6]) {
    struct nd_entry *e = nd_lookup(ip);
    if (e) {
        memcpy(mac_out, e->mac, 6);
        return true;
    }

    /* Send a Neighbor Solicitation to the solicited-node multicast address. */
    struct ipv6_addr sn;
    ipv6_make_solicited_node(&sn, ip);

    /* NS message: 4 bytes (type+code+checksum) + 4 bytes reserved + 16 target
     * + 8 bytes source link-layer address option = 32 bytes. */
    uint8_t ns_buf[32];
    memset(ns_buf, 0, sizeof(ns_buf));

    struct icmpv6_hdr *hdr = (struct icmpv6_hdr *)ns_buf;
    hdr->type = ICMPV6_NEIGHBOR_SOLICIT;
    hdr->code = 0;
    hdr->checksum = 0;
    /* reserved word at offset 4 is already zero */

    /* Target address at offset 8 */
    memcpy(ns_buf + 8, ip->bytes, 16);

    /* Source link-layer address option at offset 24 */
    ns_buf[24] = ND_OPT_SRC_LINKADDR;
    ns_buf[25] = 1;       /* length in units of 8 bytes */
    memcpy(ns_buf + 26, g_my_mac, 6);

    const struct ipv6_addr *our = ipv6_our_linklocal();
    hdr->checksum = icmpv6_checksum(our, &sn, ns_buf, sizeof(ns_buf));

    /* NDP travels at hop limit 255 (RFC 4861) -- receivers silently
     * discard anything else. */
    ipv6_send_hl(&sn, IPV6_NH_ICMPV6, ns_buf, sizeof(ns_buf), 255);

    /* Bounded wait for the NA (2026-08-23). The old fire-and-forget
     * return meant the FIRST send to every cold neighbor failed and the
     * caller had to retry -- v4 gets away with that shape because its
     * gateway is ARP-warm from DHCP before anything else sends, but in
     * v6 every on-link peer starts cold. Pump the NIC and poll the
     * cache: a live neighbor (SLIRP, a LAN host) answers in well under
     * a millisecond, so the 200 ms cap is only ever paid to a DEAD
     * neighbor. Pure pump+clock loop -- no hlt, no yield -- so it is
     * safe from any context that may send (syscall or net tick). */
    {
        uint64_t deadline = perf_now_ns() + 200ull * 1000000ull;
        struct net_dev *nd = net_default();
        while (perf_now_ns() < deadline) {
            if (nd && nd->rx_drain) nd->rx_drain(nd);
            e = nd_lookup(ip);
            if (e) {
                memcpy(mac_out, e->mac, 6);
                return true;
            }
        }
    }
    return false;
}

/* ---- send Neighbor Advertisement -------------------------------- */

static void send_na(const struct ipv6_addr *dst_ip,
                    const struct ipv6_addr *target, uint8_t flags) {
    /* NA message: 4 (hdr) + 4 (flags+reserved) + 16 (target) + 8 (TLLA opt) */
    uint8_t na_buf[32];
    memset(na_buf, 0, sizeof(na_buf));

    struct icmpv6_hdr *hdr = (struct icmpv6_hdr *)na_buf;
    hdr->type = ICMPV6_NEIGHBOR_ADVERT;
    hdr->code = 0;
    hdr->checksum = 0;

    /* Flags byte at offset 4: R(7)|S(6)|O(5)|reserved(0-4) */
    na_buf[4] = flags;

    /* Target address at offset 8 */
    memcpy(na_buf + 8, target->bytes, 16);

    /* Target link-layer address option at offset 24 */
    na_buf[24] = ND_OPT_TGT_LINKADDR;
    na_buf[25] = 1;
    memcpy(na_buf + 26, g_my_mac, 6);

    /* Checksum over the source ipv6_send will REALLY stamp (2026-08-23).
     * This hardcoded our link-local while ipv6_send stamps the SLAAC
     * global for a global-scope destination -- so every NA answering a
     * global-address solicitation carried a bad checksum, the solicitor
     * dropped it as corrupt, and re-solicited forever (observed: SLIRP
     * NS'ing us every 3 s while holding our DNS answer). */
    hdr->checksum = icmpv6_checksum(ipv6_src_for(dst_ip), dst_ip,
                                    na_buf, sizeof(na_buf));

    ipv6_send_hl(dst_ip, IPV6_NH_ICMPV6, na_buf, sizeof(na_buf), 255);
}

/* ---- Router Solicitation (SLAAC kickoff) ------------------------ */

void icmpv6_send_router_solicit(void) {
    /* RS: 4 (hdr) + 4 (reserved) + 8 (source link-layer addr option). */
    uint8_t rs_buf[16];
    memset(rs_buf, 0, sizeof(rs_buf));

    struct icmpv6_hdr *hdr = (struct icmpv6_hdr *)rs_buf;
    hdr->type = ICMPV6_ROUTER_SOLICIT;
    hdr->code = 0;
    /* reserved word at offset 4 is zero */

    rs_buf[8]  = ND_OPT_SRC_LINKADDR;
    rs_buf[9]  = 1;                      /* length in 8-byte units */
    memcpy(rs_buf + 10, g_my_mac, 6);

    const struct ipv6_addr *our = ipv6_our_linklocal();
    hdr->checksum = icmpv6_checksum(our, &ipv6_addr_all_routers,
                                    rs_buf, sizeof(rs_buf));
    /* Hop limit 255 is LOAD-BEARING here: RFC 4861 receivers (SLIRP
     * included) silently discard NDP at any other value, and the
     * hardwired 64 this used to ride meant every RS this stack ever
     * sent was ignored -- "SLIRP does not answer RS" (the old selftest
     * comment) was OUR bug wearing the router's name. */
    ipv6_send_hl(&ipv6_addr_all_routers, IPV6_NH_ICMPV6, rs_buf,
                 sizeof(rs_buf), 255);
}

/* ---- Router Advertisement (SLAAC) ------------------------------- */

static void handle_router_advert(const struct ipv6_addr *src,
                                 const uint8_t *payload, size_t len) {
    /* Fixed RA part is 16 bytes (4 hdr + cur_hop + flags + lifetime +
     * reachable + retrans); options follow. */
    if (len < 16) return;

    /* Byte 5 carries the M (Managed) / O (Other) flags. M => obtain the
     * address from a DHCPv6 server (stateful); O (without M) => addresses
     * via SLAAC but other config (DNS) via DHCPv6 INFORMATION-REQUEST. We
     * kick the DHCPv6 client off here; SLAAC below still runs in parallel
     * (a host may use both). */
    uint8_t ra_flags = payload[5];
    if (ra_flags & ND_RA_FLAG_MANAGED) {
        dhcpv6_start(true);
    } else if (ra_flags & ND_RA_FLAG_OTHER) {
        dhcpv6_start(false);
    }

    /* Walk options looking for Prefix Information (and an optional source
     * link-layer address we can cache for the router). */
    size_t off = 16;
    while (off + 2 <= len) {
        uint8_t  otype = payload[off];
        uint8_t  ounit = payload[off + 1];      /* length in 8-byte units */
        if (ounit == 0) break;                  /* malformed -- stop */
        size_t   olen  = (size_t)ounit * 8;
        if (off + olen > len) break;

        if (otype == ND_OPT_SRC_LINKADDR && olen >= 8) {
            nd_insert(src, payload + off + 2);  /* cache router's MAC */
        } else if (otype == ND_OPT_PREFIX_INFO && olen >= 32) {
            uint8_t prefix_len = payload[off + 2];
            uint8_t flags      = payload[off + 3];
            if (prefix_len == 64 && (flags & ND_PREFIX_FLAG_AUTO)) {
                struct ipv6_addr prefix;
                memcpy(prefix.bytes, payload + off + 16, 16);
                ipv6_slaac_configure(&prefix, src);
            }
        }
        off += olen;
    }
}

/* ---- SLAAC self-test (-DSLAAC_SELFTEST) -------------------------
 *
 * QEMU's user-mode SLIRP does not answer Router Solicitations with RAs, so
 * the RA-parse -> global-address path can't be exercised on the wire here
 * (the RS-send side IS confirmed in a packet capture). This synthesizes a
 * realistic RA -- /64 prefix 2001:db8:: with the autonomous flag, plus a
 * source link-layer option -- feeds it through the real handler, and asserts
 * the global address + default router were configured. Opt-in, boot-time. */
void icmpv6_slaac_selftest(void) {
    /* RA: 16-byte fixed part + 32-byte Prefix Information + 8-byte SLLAO. */
    uint8_t ra[56];
    memset(ra, 0, sizeof(ra));
    ra[0] = ICMPV6_ROUTER_ADVERT;
    ra[4] = 64;                 /* cur_hop_limit */
    ra[6] = 0x07; ra[7] = 0x08; /* router lifetime ~1800s (cosmetic) */

    /* Prefix Information option at offset 16. */
    uint8_t *pi = ra + 16;
    pi[0] = ND_OPT_PREFIX_INFO;
    pi[1] = 4;                  /* 4 * 8 = 32 bytes */
    pi[2] = 64;                 /* prefix length */
    pi[3] = ND_PREFIX_FLAG_ONLINK | ND_PREFIX_FLAG_AUTO;
    pi[16] = 0x20; pi[17] = 0x01; pi[18] = 0x0d; pi[19] = 0xb8;  /* 2001:db8:: */

    /* Source link-layer address option at offset 48. */
    uint8_t *sl = ra + 48;
    sl[0] = ND_OPT_SRC_LINKADDR;
    sl[1] = 1;
    sl[2]=0x52; sl[3]=0x54; sl[4]=0x00; sl[5]=0xaa; sl[6]=0xbb; sl[7]=0xcc;

    struct ipv6_addr router;
    memset(&router, 0, sizeof(router));
    router.bytes[0]=0xfe; router.bytes[1]=0x80;
    router.bytes[8]=0x50; router.bytes[9]=0x54; router.bytes[10]=0x00;
    router.bytes[11]=0xff; router.bytes[12]=0xfe;
    router.bytes[13]=0xaa; router.bytes[14]=0xbb; router.bytes[15]=0xcc;

    handle_router_advert(&router, ra, sizeof(ra));

    const struct ipv6_addr *g = ipv6_our_global();
    bool ok = g && g->bytes[0]==0x20 && g->bytes[1]==0x01 &&
              g->bytes[2]==0x0d && g->bytes[3]==0xb8 &&
              /* interface id copied from our link-local low 64 bits */
              g->bytes[8]==ipv6_our_linklocal()->bytes[8] &&
              g->bytes[15]==ipv6_our_linklocal()->bytes[15] &&
              ipv6_default_router() != 0;
    if (ok) {
        char buf[40]; ipv6_format(buf, sizeof(buf), g);
        kprintf("[icmpv6] SLAAC SELFTEST: global %s -- PASS\n", buf);
    } else {
        kprintf("[icmpv6] SLAAC SELFTEST: FAIL (have_global=%d)\n",
                (int)ipv6_have_global());
    }
}

/* ---- receive handlers ------------------------------------------- */

static void handle_echo_request(const struct ipv6_addr *src,
                                const struct ipv6_addr *dst,
                                const uint8_t *payload, size_t len) {
    (void)dst;
    if (len < 8) return;   /* type(1)+code(1)+checksum(2)+id(2)+seq(2) */

    char srcbuf[40];
    ipv6_format(srcbuf, sizeof(srcbuf), src);
    kprintf("[icmpv6] echo request from %s len=%u\n", srcbuf, (unsigned)len);

    /* Build reply: copy the request, change type to reply, recompute csum */
    uint8_t reply[ETH_MTU - IPV6_HDR_LEN];
    if (len > sizeof(reply)) return;
    memcpy(reply, payload, len);

    struct icmpv6_hdr *rh = (struct icmpv6_hdr *)reply;
    rh->type     = ICMPV6_ECHO_REPLY;
    rh->code     = 0;
    rh->checksum = 0;

    /* Same rule as send_na: checksum over what ipv6_send really stamps
     * (the global, for a global-scope pinger), not a hardcoded
     * link-local. */
    rh->checksum = icmpv6_checksum(ipv6_src_for(src), src, reply, len);

    if (ipv6_send(src, IPV6_NH_ICMPV6, reply, len) == 0)
        kprintf("[icmpv6] echo reply sent\n");
    else
        kprintf("[icmpv6] echo reply failed\n");
}

static void handle_neighbor_solicit(const struct ipv6_addr *src,
                                    const uint8_t *payload, size_t len) {
    /* NS: 4 (hdr) + 4 (reserved) + 16 (target) = 24 minimum */
    if (len < 24) return;

    struct ipv6_addr target;
    memcpy(target.bytes, payload + 8, 16);

    /* Answer for ANY of our unicast addresses (2026-08-23). This
     * responder predates SLAAC and only knew the link-local -- so the
     * stack ACQUIRED global addresses it then refused to defend: SLIRP
     * resolved fec0::3 for us, sent the DNS answer, NS'd our global to
     * deliver it, got silence, and retransmitted the solicitation every
     * 3 s while our recv timed out. Same silent black hole for every
     * inbound TCP6 segment addressed to the global. */
    bool ours = ipv6_addr_equal(&target, ipv6_our_linklocal());
    if (!ours && ipv6_have_global())
        ours = ipv6_addr_equal(&target, ipv6_our_global());
    if (!ours && ipv6_have_dhcp6())
        ours = ipv6_addr_equal(&target, ipv6_dhcp6_addr());
    if (!ours)
        return;   /* genuinely not asking about us */

    char srcbuf[40];
    ipv6_format(srcbuf, sizeof(srcbuf), src);
    kprintf("[icmpv6] neighbor solicitation for us from %s\n", srcbuf);

    /* Extract source link-layer address option if present */
    if (len >= 32 && payload[24] == ND_OPT_SRC_LINKADDR && payload[25] == 1) {
        nd_insert(src, payload + 26);
    }

    /* Reply with NA: Solicited + Override flags */
    uint8_t na_flags = 0x60;   /* S(6) + O(5) */

    /* If source is :: (DAD probe), reply to all-nodes multicast */
    if (ipv6_addr_is_zero(src))
        send_na(&ipv6_addr_all_nodes, &target, na_flags);
    else
        send_na(src, &target, na_flags);
}

static void handle_neighbor_advert(const struct ipv6_addr *src,
                                   const uint8_t *payload, size_t len) {
    (void)src;
    /* NA: 4 (hdr) + 4 (flags) + 16 (target) = 24 minimum */
    if (len < 24) return;

    struct ipv6_addr target;
    memcpy(target.bytes, payload + 8, 16);

    /* Extract target link-layer address option if present */
    uint8_t mac[6];
    bool has_mac = false;
    if (len >= 32 && payload[24] == ND_OPT_TGT_LINKADDR && payload[25] == 1) {
        memcpy(mac, payload + 26, 6);
        has_mac = true;
    }

    if (has_mac) {
        nd_insert(&target, mac);
        char buf[40], mb[18];
        ipv6_format(buf, sizeof(buf), &target);
        net_format_mac(mb, mac);
        kprintf("[icmpv6] neighbor %s is at %s\n", buf, mb);
    }
}

/* ---- main receive entry ----------------------------------------- */

void icmpv6_recv(const struct ipv6_addr *src, const struct ipv6_addr *dst,
                 const void *payload, size_t len) {
    if (len < sizeof(struct icmpv6_hdr)) return;

    /* Verify checksum */
    if (icmpv6_checksum(src, dst, payload, len) != 0) {
        kprintf("[icmpv6] bad checksum, dropping\n");
        return;
    }

    const struct icmpv6_hdr *hdr = (const struct icmpv6_hdr *)payload;
    const uint8_t *data = (const uint8_t *)payload;

    switch (hdr->type) {
    case ICMPV6_ECHO_REQUEST:
        handle_echo_request(src, dst, data, len);
        break;
    case ICMPV6_NEIGHBOR_SOLICIT:
        handle_neighbor_solicit(src, data, len);
        break;
    case ICMPV6_NEIGHBOR_ADVERT:
        handle_neighbor_advert(src, data, len);
        break;
    case ICMPV6_ROUTER_ADVERT:
        handle_router_advert(src, data, len);
        break;
    case ICMPV6_DEST_UNREACH: {
        /* RFC 4443 destination unreachable, carrying the invoking packet:
         * 4 (hdr) + 4 (unused) + inner v6 header (40) + inner L4. Route
         * it to the socket that sent the original datagram so a
         * connect()ed-UDP caller reads the wire's verdict as an errno
         * instead of timing out (2026-08-23: SLIRP answers exactly this
         * -- type 1 code 0 -- for a v6 DNS query when the host has no
         * IPv6 resolver, and we used to throw it away unread). */
        if (len < 56) break;                    /* need through inner L4 ports */
        const uint8_t *in6 = data + 8;          /* inner ipv6 header */
        struct ipv6_addr peer;
        memcpy(peer.bytes, in6 + 24, 16);       /* inner DST = our peer */
        uint16_t lport_be, pport_be;
        memcpy(&lport_be, data + 48, 2);        /* inner L4 src = ours */
        memcpy(&pport_be, data + 50, 2);        /* inner L4 dst = peer */
        if (in6[6] == IPV6_NH_UDP) {
            extern void sock_udp6_icmp_error(uint16_t local_port_be,
                                             const struct ipv6_addr *peer,
                                             uint16_t peer_port_be,
                                             uint8_t code);
            sock_udp6_icmp_error(lport_be, &peer, pport_be, hdr->code);
        } else if (in6[6] == IPV6_NH_TCP) {
            /* Kills a SYN_SENT handshake with the real errno (Linux's
             * tcp_v6_err shape) -- SLIRP answers v6 SYNs it cannot
             * service with exactly this, and unread it meant every such
             * connect sat out its full timeout. */
            extern void tcp6_icmp_error(uint16_t local_port_be,
                                        const struct ipv6_addr *peer,
                                        uint16_t peer_port_be, uint8_t code);
            tcp6_icmp_error(lport_be, &peer, pport_be, hdr->code);
        }
        break;
    }
    default: {
        /* Capped: an ICMPv6 type nothing handles -- for an ERROR
         * (type < 128) print enough of the invoking packet to see what
         * the far side is complaining about. Silent unknowns are how
         * "SLIRP refuses our DNS query with an error we throw away"
         * looked exactly like "no answer" (2026-08-23). */
        static int unk;
        if (unk < 8) { unk++;
            char sb[40];
            ipv6_format(sb, sizeof(sb), src);
            kprintf("[icmpv6] unhandled type=%u code=%u len=%u from %s "
                    "b4-7=%02x %02x %02x %02x\n",
                    (unsigned)hdr->type, (unsigned)hdr->code,
                    (unsigned)len, sb,
                    len > 4 ? data[4] : 0, len > 5 ? data[5] : 0,
                    len > 6 ? data[6] : 0, len > 7 ? data[7] : 0);
        }
        break;
    }
    }
}

/* ---- init -------------------------------------------------------- */

void icmpv6_init(void) {
    memset(g_nd_cache, 0, sizeof(g_nd_cache));
    kprintf("[icmpv6] initialized\n");
}
