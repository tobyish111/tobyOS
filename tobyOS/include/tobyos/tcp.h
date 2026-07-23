/* tcp.h -- TCP (RFC 793 + common extensions used in practice).
 *
 * Implemented:
 *   - Active open (client) and passive open (listen + accept).
 *   - 3-way handshake both directions; graceful FIN close; RST abort.
 *   - Cumulative ACKs, up to TCP_MAX_TX_PENDING data segments in flight
 *     (pipelined send).
 *   - Per-connection RTO with Jacobson/Karels RTT estimator (RFC 6298,
 *     microsecond-precision internally, millisecond user API).
 *   - Slow start + congestion avoidance (AIMD); fast retransmit on 3
 *     duplicate ACKs (RFC 5681).
 *   - TIME_WAIT (~2 s) before freeing a closed connection slot.
 *   - IPv4 path MTU via ip.c fragmentation (MSS-sized TCP segments).
 *
 * Still simplified vs a full production stack:
 *   - No SACK, window scaling, or timestamps (20-byte TCP headers only).
 *   - Out-of-order data beyond one segment is not reassembled (peer
 *     retransmits after duplicate ACK).
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

/* Active open that does NOT wait for the handshake: sends the SYN and returns
 * the connection in SYN_SENT. The caller polls tcp_state()/tcp_poll_flags()
 * for completion -- this is what a non-blocking connect() (EINPROGRESS) needs.
 * Returns NULL only if a slot/port could not be allocated or the SYN failed
 * to go out. */
struct tcp_conn *tcp_connect_nb(uint32_t dst_ip_be, uint16_t dst_port_be);

/* Send without waiting for ACKs: queues as much as the congestion/receive
 * window allows right now and returns the byte count accepted (0 means the
 * window is full -- the caller reports EAGAIN). tcp_send by contrast blocks
 * until everything is acknowledged, which stalls an epoll-driven caller. */
long tcp_send_nb(struct tcp_conn *c, const void *buf, size_t len);

/* Local/remote endpoint of a connection (network byte order), for
 * getsockname/getpeername. */
uint16_t tcp_local_port_be(const struct tcp_conn *c);
uint32_t tcp_remote_ip_be(const struct tcp_conn *c);
uint16_t tcp_remote_port_be(const struct tcp_conn *c);

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
/* Abortive close: send RST and free immediately -- no graceful FIN
 * exchange, no TIME_WAIT linger. For tearing down a handshake rejected
 * mid-flight (e.g. TLS cert validation) without the active-closer
 * TIME_WAIT block that tcp_close would incur (stage 13H). */
/* Graceful close that does NOT block: sends FIN and detaches, leaving
 * tcp_service_tick() to run out the handshake + TIME_WAIT and recycle
 * the slot. The conn pointer is dead the moment this returns. Prefer
 * over tcp_abort() where an RST would be rude. */
void tcp_close_nowait(struct tcp_conn *c);

/* Background timer service: retransmits + reaps detached conns whose
 * TIME_WAIT has expired. Driven from net_service_tick(). */
void tcp_service_tick(void);

void tcp_abort(struct tcp_conn *c);
void tcp_dump(void);
/* Toggle per-segment rx trace lines (default off -- they saturate a
 * 38400-baud serial link during any large download). */
void tcp_set_trace(int on);

tcp_state_t tcp_state(const struct tcp_conn *c);
const char *tcp_state_name(tcp_state_t s);

/* ---- Poll/epoll readiness predicates (B14) ----------------------------
 * Non-blocking, side-effect-free queries used by the syscall layer's
 * file_poll_ready() so poll/select/epoll can report TCP-socket readiness. */

/* True when `listener` has a completed handshake queued -- i.e. accept()
 * would return a connection immediately without blocking. */
bool tcp_can_accept(const struct tcp_conn *listener);

/* Readiness bitmask for a connected (non-listening) connection. */
#define TCP_RDY_RECV  0x1   /* recv won't block: data buffered, or EOF/error */
#define TCP_RDY_SEND  0x2   /* established/half-closed our way: send is OK */
#define TCP_RDY_HUP   0x4   /* peer sent FIN / connection torn down */
#define TCP_RDY_ERR   0x8   /* RST seen / aborted */
int tcp_poll_flags(const struct tcp_conn *c);

/* Jacobson/Karels RTT estimator (RFC 6298). */
void tcp_rtt_update(struct tcp_conn *c, uint32_t measured_rtt_ms);

/* Congestion-window increase on successful ACK (slow start or AIMD). */
void tcp_congestion_on_ack(struct tcp_conn *c, uint32_t bytes_acked);

/* Loss event: halve cwnd, set ssthresh (multiplicative decrease). */
void tcp_congestion_on_loss(struct tcp_conn *c);

/* Timer-driven retransmission check for all pending segments. */
void tcp_retransmit_check(struct tcp_conn *c);

/* Fast retransmit: triggered after 3 duplicate ACKs. */
void tcp_fast_retransmit(struct tcp_conn *c);

#endif /* TOBYOS_TCP_H */
