/* tcp.h -- TCP (RFC 793 + common extensions used in practice).
 *
 * Implemented:
 *   - Active open (client) and passive open (listen + accept).
 *   - 3-way handshake both directions; graceful FIN close; RST abort.
 *   - Cumulative ACKs, up to TCP_MAX_TX_PENDING data segments in flight
 *     (pipelined send), slow-start style cwnd cap.
 *   - Per-connection RTO with exponential backoff; RTT sampling on ACK
 *     (RFC 6298–style smoothing, simplified integers).
 *   - TIME_WAIT (~2 s) before freeing a closed connection slot.
 *   - IPv4 path MTU via ip.c fragmentation (MSS-sized TCP segments).
 *
 * Still simplified vs a full production stack:
 *   - No SACK, window scaling, or timestamps (20-byte TCP headers only).
 *   - Out-of-order data beyond one segment is not reassembled (peer
 *     retransmits after duplicate ACK).
 *   - No slow-start / congestion avoidance beyond a fixed cwnd ceiling.
 *   - listen() does not integrate with the UDP socket syscall layer yet;
 *     use tcp_listen/tcp_accept from kernel or shell test code.
 */

#ifndef TOBYOS_TCP_H
#define TOBYOS_TCP_H

#include <tobyos/types.h>

typedef enum {
    TCP_CLOSED        = 0,
    TCP_LISTEN        = 1,
    TCP_SYN_SENT      = 2,
    TCP_SYN_RECEIVED  = 3,
    TCP_ESTABLISHED   = 4,
    TCP_FIN_WAIT_1    = 5,
    TCP_FIN_WAIT_2    = 6,
    TCP_CLOSE_WAIT    = 7,
    TCP_LAST_ACK      = 8,
    TCP_TIME_WAIT     = 9,
} tcp_state_t;

#define TCP_FLAG_FIN    0x01
#define TCP_FLAG_SYN    0x02
#define TCP_FLAG_RST    0x04
#define TCP_FLAG_PSH    0x08
#define TCP_FLAG_ACK    0x10
#define TCP_FLAG_URG    0x20

struct __attribute__((packed)) tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
};
#define TCP_HDR_LEN sizeof(struct tcp_hdr)

static inline unsigned tcp_hdr_bytes(uint8_t data_off) {
    return ((unsigned)(data_off >> 4)) * 4u;
}

struct tcp_conn;

bool tcp_init(void);
void tcp_recv_packet(uint32_t src_ip_be, const void *tcp_packet, size_t len);

struct tcp_conn *tcp_connect(uint32_t dst_ip_be, uint16_t dst_port_be,
                             uint32_t timeout_ms);

/* Passive open: bind local port (network byte order). backlog is capped
 * internally (see TCP_LISTEN_BACKLOG in tcp.c). Returns NULL if the port
 * is busy or no slot is free. */
struct tcp_conn *tcp_listen(uint16_t local_port_be, int backlog);

/* Block until a completed handshake is queued on `listener`, or timeout.
 * Returns a new connection in ESTABLISHED state, or NULL on timeout. */
struct tcp_conn *tcp_accept(struct tcp_conn *listener, uint32_t timeout_ms);

long tcp_send(struct tcp_conn *c, const void *buf, size_t len);
long tcp_recv(struct tcp_conn *c, void *buf, size_t cap, uint32_t timeout_ms);
void tcp_close(struct tcp_conn *c);
void tcp_dump(void);

tcp_state_t tcp_state(const struct tcp_conn *c);
const char *tcp_state_name(tcp_state_t s);

#endif /* TOBYOS_TCP_H */
