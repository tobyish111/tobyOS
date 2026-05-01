/* ip.h -- IPv4 packet send/receive (RFC 791).
 *
 * Send: next-hop ARP, 20-byte header (IHL=5), optional fragmentation when
 * the payload exceeds one link MTU; all fragments share one IPv4 ID.
 *
 * Receive: header validation; fragment reassembly up to 8 KiB payload;
 * UDP/TCP demux; ICMP echo reply (ping).  No IP options.
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
    uint16_t flags_frag;       /* big-endian: DF/MF + 13-bit frag offset */
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;         /* RFC-791 over the header              */
    uint32_t src_ip;           /* network byte order                   */
    uint32_t dst_ip;           /* network byte order                   */
};

#define IP_HDR_LEN sizeof(struct ip_hdr)

/* Send `payload_len` bytes of L4 data to `dst_ip_be` with IP protocol
 * `proto`. Returns true on success, false on:
 *   - len > 65535 - IP_HDR_LEN
 *   - ARP cache miss for next-hop (an ARP request is fired in that
 *     case so a retry shortly after will likely succeed). */
bool ip_send(uint32_t dst_ip_be, uint8_t proto,
             const void *payload, size_t payload_len);

/* Called by eth_recv on each IPv4 frame. */
void ip_recv(const void *payload, size_t len);

#endif /* TOBYOS_IP_H */
