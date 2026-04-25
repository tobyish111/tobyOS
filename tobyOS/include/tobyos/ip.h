/* ip.h -- IPv4 packet send/receive (RFC 791, minus options/fragments).
 *
 * Send path (ip_send):
 *   - Picks next-hop = same_subnet(dst) ? dst : g_gateway_ip
 *   - Looks up next-hop in ARP cache; on miss, fires ARP request and
 *     fails (caller can retry; net_init pre-populates the gateway).
 *   - Builds a 20-byte fixed header (no options), sets IHL=5, TTL=64,
 *     fills in the per-RFC checksum, and hands the packet to eth_send.
 *
 * Receive path (ip_recv):
 *   - Verifies version=4, IHL=5 (no options), header checksum.
 *   - Drops fragments (MF set or fragment_offset != 0).
 *   - Drops anything not addressed to us or to the broadcast (255.255.255.255).
 *   - Dispatches by `protocol`:
 *        17 (UDP)  -> udp_recv
 *        else      -> silent drop (no ICMP unreachables).
 */

#ifndef TOBYOS_IP_H
#define TOBYOS_IP_H

#include <tobyos/types.h>
#include <tobyos/net.h>

struct __attribute__((packed)) ip_hdr {
    uint8_t  ver_ihl;          /* hi-nibble = 4, lo-nibble = 5         */
    uint8_t  tos;
    uint16_t total_len;        /* big-endian: header + payload         */
    uint16_t id;
    uint16_t flags_frag;       /* big-endian; we set 0 (no fragments)  */
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;         /* RFC-791 over the header              */
    uint32_t src_ip;           /* network byte order                   */
    uint32_t dst_ip;           /* network byte order                   */
};

#define IP_HDR_LEN sizeof(struct ip_hdr)

/* Send `payload_len` bytes of L4 data to `dst_ip_be` with IP protocol
 * `proto`. Returns true on success, false on:
 *   - len > ETH_MTU - IP_HDR_LEN
 *   - ARP cache miss for next-hop (an ARP request is fired in that
 *     case so a retry shortly after will likely succeed). */
bool ip_send(uint32_t dst_ip_be, uint8_t proto,
             const void *payload, size_t payload_len);

/* Called by eth_recv on each IPv4 frame. */
void ip_recv(const void *payload, size_t len);

#endif /* TOBYOS_IP_H */
