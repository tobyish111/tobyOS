/* udp.h -- UDP datagram send/receive (RFC 768).
 *
 * Send: udp_send builds an 8-byte UDP header, optionally fills the
 * pseudo-header checksum (we always do; some hosts drop datagrams
 * with checksum=0). Hands the packet to ip_send.
 *
 * Receive: udp_recv hands the datagram off to socket.c, which finds
 * the listening socket by destination port and queues the payload.
 */

#ifndef TOBYOS_UDP_H
#define TOBYOS_UDP_H

#include <tobyos/types.h>
#include <tobyos/net.h>

struct __attribute__((packed)) udp_hdr {
    uint16_t src_port;         /* network byte order */
    uint16_t dst_port;         /* network byte order */
    uint16_t length;           /* network byte order: header + payload */
    uint16_t checksum;         /* network byte order; 0 = "no checksum" */
};

#define UDP_HDR_LEN sizeof(struct udp_hdr)

/* Build a UDP datagram and send it. All ports are in network byte
 * order. Returns true on success. */
bool udp_send(uint16_t src_port_be, uint32_t dst_ip_be, uint16_t dst_port_be,
              const void *payload, size_t payload_len);

/* Called by ip_recv on every UDP datagram destined for our IP. */
void udp_recv(uint32_t src_ip_be, const void *udp_packet, size_t len);

#endif /* TOBYOS_UDP_H */
