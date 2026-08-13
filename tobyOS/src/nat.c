/* nat.c -- IP forwarding + MASQUERADE (Phase 3 slice 12, cut 6).
 *
 * ---------------------------------------------------------------------------
 * THE LAST PIECE: a container reaching the world
 *
 * Cuts 1-5 gave a container an interface, an address, real IP with its OWN
 * source address, a route table and a bridge. All of that lets it reach its
 * peer. It still could not reach the internet, because 10.66.0.2 is not an
 * address anything outside this machine will route back to.
 *
 * Masquerading is the fix every container runtime uses: on the way out, the
 * host rewrites the source address to its own and substitutes a source port it
 * picked; on the way back, it looks the port up and puts the original address
 * and port back. The mapping table is the entire mechanism.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS DELIBERATELY LIMITED, said plainly
 *
 *   - MASQUERADE ONLY. No DNAT, no port forwarding inward, no filtering. There
 *     is no iptables here and this does not pretend to be one.
 *   - ENABLING IT IS A KERNEL CALL, not `/proc/sys/net/ipv4/ip_forward`. This
 *     kernel has no /proc/sys at all (grep finds none), so a userspace runtime
 *     cannot turn forwarding on. Stating that is better than adding a file that
 *     accepts a 1 and changes nothing. Enabling NAT enables forwarding for the
 *     namespace -- Linux separates the two and this does not.
 *   - FRAGMENTS ARE NOT TRANSLATED. Only the first fragment carries the L4
 *     ports a mapping is keyed on, so a later fragment cannot be matched
 *     without reassembling first. They are DROPPED rather than forwarded
 *     untranslated, because forwarding them with the container's source
 *     address would leak an unroutable address onto the wire.
 *   - TCP connection state is not tracked; an entry lives on a timer. A
 *     long-idle connection can therefore lose its mapping, which is exactly
 *     what a real NAT box does to you.
 */

#include <tobyos/net.h>
#include <tobyos/ip.h>
#include <tobyos/nsproxy.h>
#include <tobyos/pit.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

#define NAT_MAX_MAPS   32
#define NAT_PORT_LO    50000u
#define NAT_PORT_HI    60000u
#define NAT_TTL_MS     120000u

struct nat_map {
    bool     in_use;
    void    *ns;
    uint8_t  proto;
    uint32_t inner_ip;      /* the container */
    uint16_t inner_id;      /* its source port, or ICMP echo id (network order) */
    uint16_t outer_id;      /* what we substituted (network order) */
    uint32_t peer_ip;       /* the far end */
    uint16_t peer_id;
    uint64_t expire_tick;
};

static struct nat_map g_maps[NAT_MAX_MAPS];
static uint16_t       g_next_port = NAT_PORT_LO;
static uint32_t       g_stat_out, g_stat_in, g_stat_drop;

/* ---- enabling ---- */

bool netns_nat_enable(void *ns, struct net_dev *uplink) {
    if (!uplink) return false;
    return net_ns_set_nat(ns, uplink);
}
bool netns_nat_enabled(void *ns) { return net_ns_nat_uplink(ns) != 0; }

/* ---- the mapping table ---- */

static uint64_t nat_now_expire(void) {
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    return pit_ticks() + ((uint64_t)hz * NAT_TTL_MS) / 1000u;
}

static void nat_expire_stale(void) {
    uint64_t now = pit_ticks();
    for (int i = 0; i < NAT_MAX_MAPS; i++)
        if (g_maps[i].in_use && now >= g_maps[i].expire_tick)
            memset(&g_maps[i], 0, sizeof g_maps[i]);
}

/* Is this outer port free for a NEW mapping? Also refuses anything our own
 * stack might bind: a translated port that collides with a local socket would
 * steal that socket's replies. The pool is deliberately above the ephemeral
 * ranges socket.c (33000..34000) and tcp.c use. */
static bool outer_port_free(uint16_t port_be) {
    for (int i = 0; i < NAT_MAX_MAPS; i++)
        if (g_maps[i].in_use && g_maps[i].outer_id == port_be) return false;
    return true;
}

static struct nat_map *nat_find_out(void *ns, uint8_t proto, uint32_t inner_ip,
                                    uint16_t inner_id, uint32_t peer_ip,
                                    uint16_t peer_id) {
    for (int i = 0; i < NAT_MAX_MAPS; i++) {
        struct nat_map *m = &g_maps[i];
        if (!m->in_use || m->ns != ns || m->proto != proto) continue;
        if (m->inner_ip == inner_ip && m->inner_id == inner_id &&
            m->peer_ip == peer_ip && m->peer_id == peer_id)
            return m;
    }
    return 0;
}

static struct nat_map *nat_find_in(void *ns, uint8_t proto, uint16_t outer_id,
                                   uint32_t peer_ip) {
    for (int i = 0; i < NAT_MAX_MAPS; i++) {
        struct nat_map *m = &g_maps[i];
        if (!m->in_use || m->ns != ns || m->proto != proto) continue;
        /* The peer address is part of the match on purpose: without it any
         * host on the internet could inject into a container by guessing a
         * translated port. */
        if (m->outer_id == outer_id && m->peer_ip == peer_ip) return m;
    }
    return 0;
}

static struct nat_map *nat_create(void *ns, uint8_t proto, uint32_t inner_ip,
                                  uint16_t inner_id, uint32_t peer_ip,
                                  uint16_t peer_id) {
    struct nat_map *m = 0;
    for (int i = 0; i < NAT_MAX_MAPS; i++)
        if (!g_maps[i].in_use) { m = &g_maps[i]; break; }
    if (!m) return 0;                     /* table full: drop, do not reuse */

    uint16_t chosen = 0;
    for (unsigned tries = 0; tries < (NAT_PORT_HI - NAT_PORT_LO); tries++) {
        uint16_t p = g_next_port++;
        if (g_next_port >= NAT_PORT_HI) g_next_port = NAT_PORT_LO;
        uint16_t be = htons(p);
        if (outer_port_free(be)) { chosen = be; break; }
    }
    if (!chosen) return 0;

    memset(m, 0, sizeof *m);
    m->in_use   = true;
    m->ns       = ns;
    m->proto    = proto;
    m->inner_ip = inner_ip;
    m->inner_id = inner_id;
    m->outer_id = chosen;
    m->peer_ip  = peer_ip;
    m->peer_id  = peer_id;
    m->expire_tick = nat_now_expire();
    return m;
}

/* ---- header surgery ---- */

/* Where the 16-bit identifier we translate lives, and whether this protocol is
 * one we can translate at all. TCP/UDP use the port; ICMP echo uses its id. */
static bool l4_id_offset(uint8_t proto, const uint8_t *l4, size_t l4len,
                         size_t *src_off, size_t *dst_off) {
    if (proto == IP_PROTO_TCP || proto == IP_PROTO_UDP) {
        if (l4len < 4) return false;
        *src_off = 0; *dst_off = 2;
        return true;
    }
    if (proto == IP_PROTO_ICMP) {
        /* Echo request/reply only: type 8 / type 0, id at offset 4. Anything
         * else (an error report) embeds the original header and would need its
         * payload rewritten too -- not attempted rather than done wrong. */
        if (l4len < 8) return false;
        if (l4[0] != 8 && l4[0] != 0) return false;
        *src_off = 4; *dst_off = 4;
        return true;
    }
    return false;
}

static void l4_rechecksum(uint8_t proto, struct ip_hdr *h, uint8_t *l4,
                          size_t l4len) {
    if (proto == IP_PROTO_TCP) {
        struct { uint16_t a; } *unused = 0; (void)unused;
        l4[16] = 0; l4[17] = 0;
        uint16_t c = net_l4_checksum(IP_PROTO_TCP, h->src_ip, h->dst_ip,
                                     l4, l4len);
        memcpy(l4 + 16, &c, 2);
    } else if (proto == IP_PROTO_UDP) {
        l4[6] = 0; l4[7] = 0;
        uint16_t c = net_l4_checksum(IP_PROTO_UDP, h->src_ip, h->dst_ip,
                                     l4, l4len);
        /* RFC 768: a computed 0 is transmitted as 0xFFFF, because 0 means
         * "no checksum" and would disable verification instead of asserting
         * it. */
        if (c == 0) c = 0xFFFFu;
        memcpy(l4 + 6, &c, 2);
    } else if (proto == IP_PROTO_ICMP) {
        /* ICMP's checksum covers no pseudo-header, so only the bytes we
         * changed matter -- but a full recompute is the same cost and cannot
         * drift. */
        l4[2] = 0; l4[3] = 0;
        uint16_t c = net_checksum(l4, l4len);
        memcpy(l4 + 2, &c, 2);
    }
}

static void ip_rechecksum(struct ip_hdr *h) {
    h->checksum = 0;
    h->checksum = net_checksum(h, IP_HDR_LEN);
}

/* A fragment other than the first has no L4 header to translate. */
static bool is_translatable_fragment(const struct ip_hdr *h) {
    uint16_t ff = ntohs(h->flags_frag);
    return (ff & 0x1FFFu) == 0;          /* offset 0; MF is fine (first frag) */
}

/* ---- the two directions ---- */

bool netns_nat_outbound(void *ns, void *pkt, size_t total) {
    struct net_dev *uplink = net_ns_nat_uplink(ns);
    if (!uplink || total < IP_HDR_LEN) return false;
    struct ip_hdr *h = (struct ip_hdr *)pkt;
    if (!is_translatable_fragment(h)) { g_stat_drop++; return false; }

    uint8_t *l4 = (uint8_t *)pkt + IP_HDR_LEN;
    size_t   l4len = total - IP_HDR_LEN;
    size_t   so = 0, dof = 0;
    if (!l4_id_offset(h->proto, l4, l4len, &so, &dof)) { g_stat_drop++; return false; }

    uint16_t inner_id = 0, peer_id = 0;
    memcpy(&inner_id, l4 + so,  2);
    memcpy(&peer_id,  l4 + dof, 2);

    nat_expire_stale();
    struct nat_map *m = nat_find_out(ns, h->proto, h->src_ip, inner_id,
                                     h->dst_ip, peer_id);
    if (!m) m = nat_create(ns, h->proto, h->src_ip, inner_id, h->dst_ip,
                           peer_id);
    if (!m) { g_stat_drop++; return false; }
    m->expire_tick = nat_now_expire();

    /* THE MASQUERADE: our uplink's address, and the port we own. */
    struct net_ctx p = net_ctx_enter(ns, uplink);
    uint32_t outer_ip = net_my_ip();
    net_ctx_leave(p);
    if (!outer_ip) { g_stat_drop++; return false; }

    h->src_ip = outer_ip;
    memcpy(l4 + so, &m->outer_id, 2);
    ip_rechecksum(h);
    l4_rechecksum(h->proto, h, l4, l4len);

    g_stat_out++;
    return ip_forward_packet(ns, pkt, total);
}

bool netns_nat_inbound(void *ns, void *pkt, size_t total) {
    if (!net_ns_nat_uplink(ns) || total < IP_HDR_LEN) return false;
    struct ip_hdr *h = (struct ip_hdr *)pkt;
    if (!is_translatable_fragment(h)) return false;

    uint8_t *l4 = (uint8_t *)pkt + IP_HDR_LEN;
    size_t   l4len = total - IP_HDR_LEN;
    size_t   so = 0, dof = 0;
    if (!l4_id_offset(h->proto, l4, l4len, &so, &dof)) return false;

    /* The reply's DESTINATION port is the outer id we handed out. For ICMP the
     * id field is the same offset in both directions, which is why l4_id_offset
     * reports the same value twice for it. */
    uint16_t outer_id = 0;
    memcpy(&outer_id, l4 + dof, 2);

    nat_expire_stale();
    struct nat_map *m = nat_find_in(ns, h->proto, outer_id, h->src_ip);
    if (!m) return false;                 /* not ours: local delivery decides */
    m->expire_tick = nat_now_expire();

    h->dst_ip = m->inner_ip;
    memcpy(l4 + dof, &m->inner_id, 2);
    ip_rechecksum(h);
    l4_rechecksum(h->proto, h, l4, l4len);

    g_stat_in++;
    return ip_forward_packet(ns, pkt, total);
}

void netns_nat_stats(uint32_t *out, uint32_t *in, uint32_t *dropped,
                     uint32_t *maps) {
    if (out)     *out     = g_stat_out;
    if (in)      *in      = g_stat_in;
    if (dropped) *dropped = g_stat_drop;
    if (maps) {
        uint32_t n = 0;
        for (int i = 0; i < NAT_MAX_MAPS; i++) if (g_maps[i].in_use) n++;
        *maps = n;
    }
}
