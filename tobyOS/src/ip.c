/* ip.c -- IPv4 send / receive (no options, no fragments).
 *
 * Send: pick next-hop, ARP it, build a 20-byte header + payload,
 * fill the header checksum, hand to eth_send. Datagram identifier
 * monotonically increments per send.
 *
 * Receive: parse header, validate version/IHL/checksum, drop
 * fragments, demux on `proto`. We accept only datagrams addressed
 * to us or to 255.255.255.255.
 */

#include <tobyos/ip.h>
#include <tobyos/eth.h>
#include <tobyos/arp.h>
#include <tobyos/udp.h>
#include <tobyos/tcp.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define IP_BROADCAST_BE  0xFFFFFFFFu
#define IP_DEFAULT_TTL   64

static uint16_t g_ip_id;       /* monotonically increasing IPv4 ID */

bool ip_send(uint32_t dst_ip_be, uint8_t proto,
             const void *payload, size_t payload_len) {
    if (payload_len + IP_HDR_LEN > ETH_MTU) return false;

    /* Pick next hop: same subnet -> direct; else gateway. */
    uint32_t next_hop = dst_ip_be;
    if ((dst_ip_be & g_my_netmask) != (g_my_ip & g_my_netmask) &&
        dst_ip_be != IP_BROADCAST_BE) {
        next_hop = g_gateway_ip;
    }

    uint8_t mac[ETH_ADDR_LEN];
    if (dst_ip_be == IP_BROADCAST_BE) {
        memcpy(mac, g_eth_broadcast, ETH_ADDR_LEN);
    } else if (!arp_resolve(next_hop, mac)) {
        /* Cache miss -- fire a request and bail. The caller (or its
         * userspace driver) can retry once the reply lands; net_init
         * pre-populated the gateway so this is the rare case. */
        arp_request(next_hop);
        return false;
    }

    uint8_t buf[ETH_MTU];
    struct ip_hdr *h = (struct ip_hdr *)buf;
    h->ver_ihl    = (uint8_t)((4u << 4) | 5u);
    h->tos        = 0;
    h->total_len  = htons((uint16_t)(IP_HDR_LEN + payload_len));
    h->id         = htons(g_ip_id++);
    h->flags_frag = 0;
    h->ttl        = IP_DEFAULT_TTL;
    h->proto      = proto;
    h->checksum   = 0;
    h->src_ip     = g_my_ip;
    h->dst_ip     = dst_ip_be;
    h->checksum   = net_checksum(h, IP_HDR_LEN);

    if (payload_len) memcpy(buf + IP_HDR_LEN, payload, payload_len);

    return eth_send(mac, ETH_TYPE_IPV4, buf, IP_HDR_LEN + payload_len);
}

void ip_recv(const void *payload, size_t len) {
    if (len < IP_HDR_LEN) return;
    const struct ip_hdr *h = (const struct ip_hdr *)payload;

    if ((h->ver_ihl >> 4)   != 4) return;
    if ((h->ver_ihl & 0x0F) != 5) return;       /* IHL=5 (no options) */

    uint16_t total = ntohs(h->total_len);
    if (total > len) return;

    /* Reject any fragmented datagram (DF=0 with offset != 0, or MF=1). */
    uint16_t ff = ntohs(h->flags_frag);
    if ((ff & 0x1FFF) != 0) return;             /* fragment offset != 0 */
    if (ff & 0x2000)        return;             /* MF set               */

    /* Header checksum -- net_checksum returns 0 on a valid header
     * (including the existing checksum field). */
    if (net_checksum(h, IP_HDR_LEN) != 0) return;

    /* Address filter: us, the limited broadcast, or anything when we
     * don't yet have an IP (early DHCP path). The third clause is
     * essential because some DHCP servers ignore the BOOTP broadcast
     * bit and unicast the OFFER to the offered IP -- which the NIC
     * happily delivers (our MAC matches) but g_my_ip == 0 would
     * otherwise reject. The window is microseconds: g_my_ip becomes
     * non-zero the moment dhcp_acquire returns. */
    if (h->dst_ip != g_my_ip &&
        h->dst_ip != IP_BROADCAST_BE &&
        g_my_ip   != 0) {
        return;
    }

    const uint8_t *l4    = (const uint8_t *)payload + IP_HDR_LEN;
    size_t         l4len = total - IP_HDR_LEN;

    switch (h->proto) {
    case IP_PROTO_UDP:
        udp_recv(h->src_ip, l4, l4len);
        break;
    case IP_PROTO_TCP:
        tcp_recv_packet(h->src_ip, l4, l4len);
        break;
    default:
        /* No ICMP echo for now; silently drop. */
        break;
    }
}
