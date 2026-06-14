/* ipv6.c -- IPv6 packet send/receive (RFC 8200).
 *
 * Link-local only: the address is derived from the NIC MAC via EUI-64
 * (insert FF:FE, flip bit 6 of the first octet). No SLAAC / DHCPv6.
 *
 * ipv6_recv() is called from eth.c when EtherType 0x86DD arrives.
 * ipv6_send() builds the 40-byte header and hands the frame to eth_send().
 */

#include <tobyos/ipv6.h>
#include <tobyos/icmpv6.h>
#include <tobyos/dhcpv6.h>
#include <tobyos/eth.h>
#include <tobyos/net.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* ---- well-known addresses --------------------------------------- */

const struct ipv6_addr ipv6_addr_any = {{ 0 }};

const struct ipv6_addr ipv6_addr_loopback = {{
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1
}};

const struct ipv6_addr ipv6_addr_all_nodes = {{
    0xFF,0x02, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0x01
}};

const struct ipv6_addr ipv6_addr_all_routers = {{
    0xFF,0x02, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0x02
}};

/* ---- per-interface state ---------------------------------------- */

static struct ipv6_addr g_linklocal;
static bool             g_ipv6_up;

/* SLAAC state (set by the RA handler). */
static struct ipv6_addr g_global;          /* autoconfigured global address */
static bool             g_have_global;
static struct ipv6_addr g_router;          /* default router (RA source)    */
static bool             g_have_router;

/* DHCPv6 state (set by the stateful client when the RA M flag is seen). */
static struct ipv6_addr g_dhcp6;           /* server-assigned global address */
static bool             g_have_dhcp6;

/* ---- address utilities ------------------------------------------ */

bool ipv6_addr_is_zero(const struct ipv6_addr *a) {
    for (int i = 0; i < 16; i++)
        if (a->bytes[i]) return false;
    return true;
}

bool ipv6_addr_equal(const struct ipv6_addr *a, const struct ipv6_addr *b) {
    return memcmp(a->bytes, b->bytes, 16) == 0;
}

bool ipv6_addr_is_linklocal(const struct ipv6_addr *a) {
    return a->bytes[0] == 0xFE && (a->bytes[1] & 0xC0) == 0x80;
}

bool ipv6_addr_is_multicast(const struct ipv6_addr *a) {
    return a->bytes[0] == 0xFF;
}

/* EUI-64: mac[0..2] | FF:FE | mac[3..5], with bit 6 of byte 0 flipped. */
void ipv6_make_linklocal(struct ipv6_addr *out, const uint8_t mac[6]) {
    memset(out->bytes, 0, 16);
    out->bytes[0]  = 0xFE;
    out->bytes[1]  = 0x80;
    out->bytes[8]  = mac[0] ^ 0x02;   /* flip Universal/Local bit */
    out->bytes[9]  = mac[1];
    out->bytes[10] = mac[2];
    out->bytes[11] = 0xFF;
    out->bytes[12] = 0xFE;
    out->bytes[13] = mac[3];
    out->bytes[14] = mac[4];
    out->bytes[15] = mac[5];
}

void ipv6_make_solicited_node(struct ipv6_addr *out,
                              const struct ipv6_addr *target) {
    memset(out->bytes, 0, 16);
    out->bytes[0]  = 0xFF;
    out->bytes[1]  = 0x02;
    out->bytes[11] = 0x01;
    out->bytes[12] = 0xFF;
    out->bytes[13] = target->bytes[13];
    out->bytes[14] = target->bytes[14];
    out->bytes[15] = target->bytes[15];
}

/* Simplified formatter: prints each 16-bit group in hex separated by ':'.
 * Compresses the longest run of consecutive zero-groups to '::'. */
void ipv6_format(char *buf, size_t cap, const struct ipv6_addr *a) {
    if (cap == 0) return;

    uint16_t g[8];
    for (int i = 0; i < 8; i++)
        g[i] = ((uint16_t)a->bytes[i*2] << 8) | a->bytes[i*2+1];

    /* Find longest run of zeroes. */
    int best_start = -1, best_len = 0;
    int cur_start = -1,  cur_len = 0;
    for (int i = 0; i < 8; i++) {
        if (g[i] == 0) {
            if (cur_start < 0) cur_start = i;
            cur_len++;
        } else {
            if (cur_len > best_len) { best_start = cur_start; best_len = cur_len; }
            cur_start = -1;
            cur_len = 0;
        }
    }
    if (cur_len > best_len) { best_start = cur_start; best_len = cur_len; }
    if (best_len < 2) best_start = -1;

    char tmp[40];
    char *p = tmp;
    for (int i = 0; i < 8; ) {
        if (i == best_start) {
            *p++ = ':';
            if (i == 0) *p++ = ':';
            i += best_len;
            if (i >= 8) break;
        }
        if (i != 0 && i != best_start) *p++ = ':';
        /* hex group without leading zeroes */
        bool started = false;
        for (int s = 12; s >= 0; s -= 4) {
            unsigned nib = (g[i] >> s) & 0xF;
            if (nib || started || s == 0) {
                *p++ = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
                started = true;
            }
        }
        i++;
    }
    *p = '\0';

    size_t len = (size_t)(p - tmp);
    if (len >= cap) len = cap - 1;
    memcpy(buf, tmp, len);
    buf[len] = '\0';
}

/* ---- multicast MAC for IPv6 ------------------------------------- */

static void ipv6_multicast_mac(uint8_t mac[6], const struct ipv6_addr *mcast) {
    mac[0] = 0x33;
    mac[1] = 0x33;
    mac[2] = mcast->bytes[12];
    mac[3] = mcast->bytes[13];
    mac[4] = mcast->bytes[14];
    mac[5] = mcast->bytes[15];
}

/* ---- init -------------------------------------------------------- */

void ipv6_init(void) {
    ipv6_make_linklocal(&g_linklocal, g_my_mac);
    g_ipv6_up = true;

    char buf[40];
    ipv6_format(buf, sizeof(buf), &g_linklocal);
    kprintf("[ipv6] link-local %s\n", buf);

    /* Kick off SLAAC: solicit a Router Advertisement. The reply (handled in
     * icmpv6.c) installs a global address + default router. Best-effort --
     * if no router answers we stay link-local-only as before. */
    icmpv6_send_router_solicit();
}

const struct ipv6_addr *ipv6_our_linklocal(void) {
    return &g_linklocal;
}

bool ipv6_is_up(void) {
    return g_ipv6_up;
}

/* ---- SLAAC ------------------------------------------------------- */

void ipv6_slaac_configure(const struct ipv6_addr *prefix,
                          const struct ipv6_addr *router) {
    /* Global = prefix top 64 bits | our interface id (the low 64 bits of
     * the link-local, which we derived from the MAC). RFC 4862 SLAAC. */
    struct ipv6_addr g;
    for (int i = 0; i < 8; i++)  g.bytes[i] = prefix->bytes[i];
    for (int i = 8; i < 16; i++) g.bytes[i] = g_linklocal.bytes[i];

    bool changed = !g_have_global || !ipv6_addr_equal(&g, &g_global);
    g_global      = g;
    g_have_global = true;

    if (router) { g_router = *router; g_have_router = true; }

    if (changed) {
        char buf[40], rbuf[40];
        ipv6_format(buf, sizeof(buf), &g_global);
        ipv6_format(rbuf, sizeof(rbuf), &g_router);
        kprintf("[ipv6] SLAAC global %s via router %s\n", buf, rbuf);
    }
}

const struct ipv6_addr *ipv6_our_global(void) {
    return g_have_global ? &g_global : 0;
}

bool ipv6_have_global(void) {
    return g_have_global;
}

const struct ipv6_addr *ipv6_default_router(void) {
    return g_have_router ? &g_router : 0;
}

/* ---- DHCPv6 (stateful) ------------------------------------------- */

void ipv6_dhcp6_configure(const struct ipv6_addr *addr) {
    bool changed = !g_have_dhcp6 || !ipv6_addr_equal(addr, &g_dhcp6);
    g_dhcp6      = *addr;
    g_have_dhcp6 = true;
    if (changed) {
        char buf[40];
        ipv6_format(buf, sizeof(buf), &g_dhcp6);
        kprintf("[ipv6] DHCPv6 address %s\n", buf);
    }
}

const struct ipv6_addr *ipv6_dhcp6_addr(void) {
    return g_have_dhcp6 ? &g_dhcp6 : 0;
}

bool ipv6_have_dhcp6(void) { return g_have_dhcp6; }

void ipv6_dhcp6_reset_for_test(void) {
    g_have_dhcp6 = false;
    memset(&g_dhcp6, 0, sizeof(g_dhcp6));
}

/* ---- receive ----------------------------------------------------- */

/* True if this datagram is addressed to us. */
static bool ipv6_dst_is_for_us(const struct ipv6_addr *dst) {
    if (ipv6_addr_equal(dst, &g_linklocal)) return true;
    if (ipv6_addr_equal(dst, &ipv6_addr_all_nodes)) return true;
    if (ipv6_addr_equal(dst, &ipv6_addr_loopback)) return true;
    if (g_have_global && ipv6_addr_equal(dst, &g_global)) return true;
    if (g_have_dhcp6 && ipv6_addr_equal(dst, &g_dhcp6)) return true;

    /* Solicited-node multicast for each of our addresses. */
    struct ipv6_addr sn;
    ipv6_make_solicited_node(&sn, &g_linklocal);
    if (ipv6_addr_equal(dst, &sn)) return true;
    if (g_have_global) {
        ipv6_make_solicited_node(&sn, &g_global);
        if (ipv6_addr_equal(dst, &sn)) return true;
    }
    if (g_have_dhcp6) {
        ipv6_make_solicited_node(&sn, &g_dhcp6);
        if (ipv6_addr_equal(dst, &sn)) return true;
    }

    return false;
}

void ipv6_recv(const void *frame, size_t len) {
    if (!g_ipv6_up) return;
    if (len < IPV6_HDR_LEN) return;

    const struct ipv6_hdr *h = (const struct ipv6_hdr *)frame;

    /* Version must be 6. ver_tc_fl is in network byte order; version is
     * the top 4 bits of the first byte on the wire. */
    uint8_t ver = ((const uint8_t *)&h->ver_tc_fl)[0] >> 4;
    if (ver != 6) return;

    uint16_t plen = ntohs(h->payload_len);
    if ((size_t)plen + IPV6_HDR_LEN > len) return;

    if (!ipv6_dst_is_for_us(&h->dst)) return;

    const uint8_t *payload = (const uint8_t *)frame + IPV6_HDR_LEN;

    switch (h->next_header) {
    case IPV6_NH_ICMPV6:
        icmpv6_recv(&h->src, &h->dst, payload, plen);
        break;
    case IPV6_NH_UDP: {
        /* Minimal UDP-over-IPv6 demux: only the DHCPv6 client port (546)
         * is consumed in-kernel today. Header = src(2) dst(2) len(2) cks(2). */
        if (plen < 8) break;
        uint16_t dport = ((uint16_t)payload[2] << 8) | payload[3];
        uint16_t ulen  = ((uint16_t)payload[4] << 8) | payload[5];
        if (dport == 546 && ulen >= 8 && (size_t)ulen <= plen)
            dhcpv6_recv(&h->src, payload + 8, (size_t)ulen - 8);
        break;
    }
    case IPV6_NH_TCP:
        /* Future: TCP-over-IPv6 */
        break;
    default:
        break;
    }
}

/* ---- send -------------------------------------------------------- */

int ipv6_send(const struct ipv6_addr *dst, uint8_t next_header,
              const void *payload, size_t payload_len) {
    if (!g_ipv6_up) return -1;
    if (payload_len > ETH_MTU - IPV6_HDR_LEN) return -1;

    uint8_t dst_mac[6];

    /* Pick the next hop: on-link destinations (multicast, link-local, or
     * inside our SLAAC /64) resolve directly; everything else goes to the
     * default router learned from the Router Advertisement. */
    if (ipv6_addr_is_multicast(dst)) {
        ipv6_multicast_mac(dst_mac, dst);
    } else {
        bool on_link = ipv6_addr_is_linklocal(dst);
        if (!on_link && g_have_global) {
            /* same /64 as our global prefix? */
            bool same = true;
            for (int i = 0; i < 8; i++)
                if (dst->bytes[i] != g_global.bytes[i]) { same = false; break; }
            on_link = same;
        }
        const struct ipv6_addr *next = dst;
        if (!on_link && g_have_router) next = &g_router;
        if (!icmpv6_resolve(next, dst_mac))
            return -1;
    }

    uint8_t buf[ETH_MTU];
    struct ipv6_hdr *h = (struct ipv6_hdr *)buf;

    /* Version 6, traffic class 0, flow label 0 → first byte = 0x60 */
    memset(&h->ver_tc_fl, 0, 4);
    ((uint8_t *)&h->ver_tc_fl)[0] = 0x60;

    h->payload_len = htons((uint16_t)payload_len);
    h->next_header = next_header;
    h->hop_limit   = 64;
    /* Source selection: a link-local/multicast destination uses our
     * link-local; a global destination uses our SLAAC global if we have one
     * (a global source is required for the reply to be routable back). */
    if (g_have_global && !ipv6_addr_is_linklocal(dst) &&
        !ipv6_addr_is_multicast(dst))
        h->src = g_global;
    else
        h->src = g_linklocal;
    h->dst         = *dst;

    if (payload_len)
        memcpy(buf + IPV6_HDR_LEN, payload, payload_len);

    if (!eth_send(dst_mac, ETH_TYPE_IPV6, buf, IPV6_HDR_LEN + payload_len))
        return -1;

    return 0;
}
