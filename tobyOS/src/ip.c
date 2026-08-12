/* ip.c -- IPv4 send / receive (RFC 791).
 *
 * Send: next-hop ARP, fixed 20-byte header (IHL=5), optional
 * fragmentation when the payload does not fit in one Ethernet MTU.
 * All fragments of one datagram share the same IPv4 ID.
 *
 * Receive: validates header, reassembles fragments up to IP_REASS_MAX
 * bytes, demuxes UDP/TCP on complete datagrams.  Unfragmented path is
 * unchanged from the original milestone-21 code.
 *
 * DHCP bootstrap: many PCIe NICs verify IPv4 checksum in hardware but do
 * not normalize the header checksum field in the RX buffer. Wireshark
 * decodes OFFERs correctly while software verification here fails and every
 * OFFER is dropped (DISCOVER loops, same xid, duplicate OFFERs on wire).
 * While g_my_ip==0, UDP destined to port 68 may proceed if the IPv4 HCS
 * alone fails (narrow exception; see ip_recv).
 */

#include <tobyos/ip.h>
#include <tobyos/eth.h>
#include <tobyos/arp.h>
#include <tobyos/icmp.h>
#include <tobyos/udp.h>
#include <tobyos/tcp.h>
#include <tobyos/pit.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define IP_BROADCAST_BE  0xFFFFFFFFu
#define IP_DEFAULT_TTL   64

/* Largest IPv4 payload we will reassemble (DoS cap). */
#define IP_REASS_MAX     8192u
#define IP_REASS_SLOTS   4
#define IP_REASS_TIMEOUT_MS 15000u

#define IP_MAX_PAYLOAD_SEND (65535u - IP_HDR_LEN)

static uint16_t g_ip_id;

/* ---- reassembly state ------------------------------------------ */

struct ip_reass_slot {
    bool     used;
    uint16_t id_be;          /* Identification from IPv4 header (wire BE) */
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  proto;
    uint16_t total;          /* 0 until a fragment with MF=0 sets it */
    uint32_t have_unique;    /* distinct payload bytes accounted for */
    uint64_t expire_tick;
    uint8_t  buf[IP_REASS_MAX];
    uint8_t  bm[(IP_REASS_MAX + 7u) / 8u];
};

static struct ip_reass_slot g_reass[IP_REASS_SLOTS];

static void bm_clear(struct ip_reass_slot *s) {
    memset(s->bm, 0, sizeof(s->bm));
    s->have_unique = 0;
}

/* Mark bytes [off, off+len) in the bitmap; returns count of newly set bits. */
static unsigned bm_mark(struct ip_reass_slot *s, unsigned off, unsigned len) {
    unsigned added = 0;
    for (unsigned i = 0; i < len; i++) {
        unsigned b = off + i;
        if (b >= IP_REASS_MAX) return 0;
        unsigned bi = b >> 3;
        unsigned bp = b & 7u;
        uint8_t mask = (uint8_t)(1u << bp);
        if ((s->bm[bi] & mask) == 0) {
            s->bm[bi] |= mask;
            added++;
        }
    }
    return added;
}

static bool bm_complete(const struct ip_reass_slot *s) {
    if (s->total == 0 || s->total > IP_REASS_MAX) return false;
    if (s->have_unique < (uint32_t)s->total) return false;
    for (unsigned i = 0; i < s->total; i++) {
        unsigned bi = i >> 3;
        unsigned bp = i & 7u;
        if ((s->bm[bi] & (uint8_t)(1u << bp)) == 0) return false;
    }
    return true;
}

static void reass_expire_stale(void) {
    uint64_t now = pit_ticks();
    for (int i = 0; i < IP_REASS_SLOTS; i++) {
        if (g_reass[i].used && now >= g_reass[i].expire_tick) {
            memset(&g_reass[i], 0, sizeof(g_reass[i]));
        }
    }
}

static struct ip_reass_slot *reass_find(uint16_t id_be, uint32_t src,
                                       uint32_t dst, uint8_t proto) {
    for (int i = 0; i < IP_REASS_SLOTS; i++) {
        struct ip_reass_slot *s = &g_reass[i];
        if (!s->used) continue;
        if (s->id_be == id_be && s->src_ip == src && s->dst_ip == dst &&
            s->proto == proto) {
            return s;
        }
    }
    return NULL;
}

static struct ip_reass_slot *reass_alloc(void) {
    for (int i = 0; i < IP_REASS_SLOTS; i++) {
        if (!g_reass[i].used) {
            memset(&g_reass[i], 0, sizeof(g_reass[i]));
            g_reass[i].used = true;
            return &g_reass[i];
        }
    }
    /* Drop slot 0 if table is full (rare on a tiny OS). */
    memset(&g_reass[0], 0, sizeof(g_reass[0]));
    g_reass[0].used = true;
    return &g_reass[0];
}

static void ip_deliver_l4(uint32_t src_ip_be, uint8_t proto,
                          const uint8_t *l4, size_t l4len) {
    switch (proto) {
    case IP_PROTO_ICMP:
        icmp_recv(src_ip_be, l4, l4len);
        break;
    case IP_PROTO_UDP:
        udp_recv(src_ip_be, l4, l4len);
        break;
    case IP_PROTO_TCP:
        tcp_recv_packet(src_ip_be, l4, l4len);
        break;
    default:
        break;
    }
}

/* Returns true if caller should stop (consumed as fragment path). */
static bool ip_recv_maybe_reassemble(const struct ip_hdr *h,
                                     const uint8_t *frame,
                                     size_t frame_len, uint16_t total_ip) {
    uint16_t ff = ntohs(h->flags_frag);
    unsigned frag_off = ((unsigned)ff & 0x1FFFu) << 3;
    bool more_frag = (ff & 0x2000) != 0;

    if (frag_off == 0 && !more_frag)
        return false; /* unfragmented — caller uses fast path */

    const uint8_t *l4    = frame + IP_HDR_LEN;
    unsigned       l4len = (unsigned)(total_ip - IP_HDR_LEN);
    if (l4len > frame_len - IP_HDR_LEN) l4len = (unsigned)(frame_len - IP_HDR_LEN);

    reass_expire_stale();

    struct ip_reass_slot *s = reass_find(h->id, h->src_ip, h->dst_ip, h->proto);
    if (!s) {
        /* First fragment of a datagram must start at offset 0 (RFC 791). If we
         * have no slot yet and frag_off != 0, this is an orphan tail/middle or
         * corrupted flags — do NOT allocate a slot (would never complete and
         * DHCP-sized UDP can look like a bogus "fragment"). */
        if (frag_off != 0) return true;
        s = reass_alloc();
        s->id_be   = h->id;
        s->src_ip  = h->src_ip;
        s->dst_ip  = h->dst_ip;
        s->proto   = h->proto;
        s->total   = 0;
        bm_clear(s);
    }

    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    s->expire_tick = pit_ticks() + ((uint64_t)hz * IP_REASS_TIMEOUT_MS) / 1000u;

    if (!more_frag) {
        unsigned end = frag_off + l4len;
        if (end > IP_REASS_MAX) {
            memset(s, 0, sizeof(*s));
            return true;
        }
        if (end > s->total || s->total == 0) s->total = (uint16_t)end;
    } else {
        if (frag_off + l4len > IP_REASS_MAX) {
            memset(s, 0, sizeof(*s));
            return true;
        }
    }

    memcpy(s->buf + frag_off, l4, l4len);
    unsigned added = bm_mark(s, frag_off, l4len);
    s->have_unique += added;

    if (s->total > 0 && bm_complete(s)) {
        ip_deliver_l4(s->src_ip, s->proto, s->buf, s->total);
        memset(s, 0, sizeof(*s));
    }
    return true;
}

/* ---- send (with fragmentation) ----------------------------------- */

static bool ip_send_frame(uint32_t dst_ip_be, uint8_t proto,
                          const uint8_t mac[ETH_ADDR_LEN], uint16_t id_be,
                          uint16_t flags_frag_be, const void *payload,
                          size_t payload_len) {
    if (payload_len + IP_HDR_LEN > ETH_MTU) return false;

    uint8_t buf[ETH_MTU];
    struct ip_hdr *h = (struct ip_hdr *)buf;
    h->ver_ihl    = (uint8_t)((4u << 4) | 5u);
    h->tos        = 0;
    h->total_len  = htons((uint16_t)(IP_HDR_LEN + payload_len));
    h->id         = id_be;
    h->flags_frag = htons(flags_frag_be);
    h->ttl        = IP_DEFAULT_TTL;
    h->proto      = proto;
    h->checksum   = 0;
    /* CUT 5: the SOURCE ADDRESS is namespace-relative.
     *
     * This was `g_my_ip` -- the host's -- unconditionally, which meant every IP
     * packet a container sent left with the HOST's source address on it. Cut 2
     * fixed the ethernet header's source MAC and then the ARP payload's sender
     * MAC after the first one turned out not to be enough; this is the same
     * finding one layer further up, and the same reason it went unnoticed:
     * nothing had ever sent IP (as opposed to ARP) from inside a namespace, so
     * no test could see it. net_my_ip() is g_my_ip in the initial namespace, so
     * the host path is unchanged -- including while DHCP is still SELECTING and
     * it is legitimately 0. */
    h->src_ip     = net_my_ip();
    h->dst_ip     = dst_ip_be;
    h->checksum   = net_checksum(h, IP_HDR_LEN);

    if (payload_len) memcpy(buf + IP_HDR_LEN, payload, payload_len);

    return eth_send(mac, ETH_TYPE_IPV4, buf, IP_HDR_LEN + payload_len);
}

bool ip_send(uint32_t dst_ip_be, uint8_t proto,
             const void *payload, size_t payload_len) {
    if (payload_len > IP_MAX_PAYLOAD_SEND) return false;

    /* CUT 5: THE ROUTE DECISION IS NAMESPACE-RELATIVE.
     *
     * "Is this destination on my subnet, and if not who is my gateway" was
     * answered from the HOST's address, mask and gateway no matter who was
     * asking -- so a container sending off its own subnet resolved the host's
     * gateway, on a network it is not attached to. This is the whole of routing
     * in this kernel: there is no route table, only "my subnet or my gateway".
     * Saying so plainly matters, because forwarding and NAT will need more than
     * this and the next reader should not mistake it for a route lookup. */
    uint32_t me       = net_my_ip();
    uint32_t mask     = net_my_netmask();
    uint32_t next_hop = dst_ip_be;
    if ((dst_ip_be & mask) != (me & mask) &&
        dst_ip_be != IP_BROADCAST_BE) {
        next_hop = net_my_gateway();
        /* No gateway configured: with no route table there is nowhere to send
         * an off-subnet packet, and ARPing for the destination itself would
         * put a frame on the wire addressed to a host that cannot answer. */
        if (next_hop == 0) return false;
    }

    uint8_t mac[ETH_ADDR_LEN];
    if (dst_ip_be == IP_BROADCAST_BE) {
        memcpy(mac, g_eth_broadcast, ETH_ADDR_LEN);
    } else if (!arp_resolve(next_hop, mac)) {
        arp_request(next_hop);
        return false;
    }

    if (payload_len + IP_HDR_LEN <= ETH_MTU) {
        uint16_t id_be = htons(g_ip_id++);
        return ip_send_frame(dst_ip_be, proto, mac, id_be, 0, payload,
                             payload_len);
    }

    /* Fragmented send: all pieces share one IPv4 ID (host order before htons). */
    uint16_t id_host = g_ip_id++;
    uint16_t id_be   = htons(id_host);

    const uint8_t *p = (const uint8_t *)payload;
    size_t           remain = payload_len;

    const size_t max_payload = ETH_MTU - IP_HDR_LEN;

    while (remain > 0) {
        size_t chunk = remain;
        bool   mf    = false;
        if (chunk > max_payload) {
            chunk = max_payload;
            /* All non-final fragments carry a payload length multiple of 8. */
            chunk -= (chunk % 8u);
            if (chunk == 0) return false;
            mf = true;
        }

        unsigned frag_off_bytes = (unsigned)(payload_len - remain);
        uint16_t frag_words     = (uint16_t)(frag_off_bytes / 8u);
        uint16_t flags          = (uint16_t)(frag_words & 0x1FFFu);
        if (mf) flags |= 0x2000u;

        if (!ip_send_frame(dst_ip_be, proto, mac, id_be, flags, p, chunk))
            return false;

        p      += chunk;
        remain -= chunk;
    }
    return true;
}

/* True if this IPv4 datagram is addressed to us (or should be demuxed during
 * DHCP when we have no address yet).
 *
 * While g_my_ip == 0 (SELECTING / REQUESTING), accept any destination IPv4 so
 * OFFER/ACK unicasted to yiaddr still reach udp_recv — the stack has no other
 * address to match yet. Some servers ignore the BOOTP broadcast flag and
 * send unicast to the offered IP; the NIC still delivers by Ethernet DA.
 *
 * After configuration: accept our unicast, 255.255.255.255, and the subnet's
 * directed broadcast (e.g. 192.168.1.255) — some DHCP implementations use
 * the latter for OFFER/ACK instead of global broadcast. */
static bool ip_dst_is_for_us(uint32_t dst_ip_be) {
    /* Slice 12 cut 2: "us" is namespace-relative. net_my_ip() is g_my_ip in the
     * initial namespace, so the wire path is unchanged; inside a namespace with
     * a veth end it is that namespace's address, which is what stops a container
     * accepting packets addressed to the host. */
    uint32_t me   = net_my_ip();
    uint32_t mask = net_my_netmask();
    if (dst_ip_be == me) return true;
    if (dst_ip_be == IP_BROADCAST_BE) return true;
    if (me == 0) return true;
    if (mask) {
        uint32_t bcast = (me & mask) | ~mask;
        if (dst_ip_be == bcast) return true;
    }
    return false;
}

/* True if we should accept a datagram whose IPv4 header checksum failed
 * software verification anyway. Real NICs often offload RX checksum: the
 * frame is valid on the wire but the IP checksum field left in RAM does
 * not match RFC 791 verification — without this, valid DHCP OFFER/ACK never
 * reach udp_recv. Restrict to SELECTING: g_my_ip==0, UDP, dst port 68. */
static bool ip_rx_allow_bad_ipv4_csum(const struct ip_hdr *h,
                                      const void *payload, size_t len,
                                      uint16_t total) {
    if (g_my_ip != 0) return false;
    if (h->proto != IP_PROTO_UDP) return false;
    if (total < IP_HDR_LEN + UDP_HDR_LEN || len < IP_HDR_LEN + UDP_HDR_LEN)
        return false;
    const uint8_t *udp = (const uint8_t *)payload + IP_HDR_LEN;
    /* UDP destination port 68 (BOOTP/DHCP client), wire byte order. */
    return udp[2] == 0 && udp[3] == 68;
}

void ip_recv(const void *payload, size_t len) {
    if (len < IP_HDR_LEN) return;
    const struct ip_hdr *h = (const struct ip_hdr *)payload;

    if ((h->ver_ihl >> 4)   != 4) return;
    if ((h->ver_ihl & 0x0F) != 5) return;

    uint16_t total = ntohs(h->total_len);
    if (total > len) {
        /* Truncated IPv4 — common if RX length disagrees with IP total_len. */
        if (g_my_ip == 0 && len >= IP_HDR_LEN + 8) {
            const uint8_t *b = (const uint8_t *)payload;
            if (b[9] == IP_PROTO_UDP && len >= IP_HDR_LEN + 4) {
                const uint8_t *u = b + IP_HDR_LEN;
                if (u[2] == 0 && u[3] == 68) {
                    static unsigned trunc_dhcp;
                    if (trunc_dhcp++ < 3u)
                        kprintf("[ip] DHCP drop: ip total_len=%u buf=%zu (truncated)\n",
                                (unsigned)total, len);
                }
            }
        }
        return;
    }

    if (net_checksum(h, IP_HDR_LEN) != 0) {
        if (!ip_rx_allow_bad_ipv4_csum(h, payload, len, total)) return;
    }

    if (!ip_dst_is_for_us(h->dst_ip)) return;

    const uint8_t *frame = (const uint8_t *)payload;

    if (ip_recv_maybe_reassemble(h, frame, len, total))
        return;

    const uint8_t *l4    = frame + IP_HDR_LEN;
    size_t         l4len = total - IP_HDR_LEN;

    ip_deliver_l4(h->src_ip, h->proto, l4, l4len);
}
