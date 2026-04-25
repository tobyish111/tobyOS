/* tcp.h -- minimal RFC 793 TCP (client active-open only).
 *
 * What we implement:
 *   - 3-way handshake (CLOSED -> SYN_SENT -> ESTABLISHED).
 *   - In-order byte-stream send and receive.
 *   - Cumulative ACK on every received segment.
 *   - Simple per-segment retransmission of the last unacked send
 *     (~500 ms timeout, up to 5 retries) -- enough to survive the
 *     occasional dropped packet on a flaky link.
 *   - Graceful close (FIN/ACK in both directions, then CLOSED).
 *   - RST handling (peer abort -> immediate CLOSED, recv returns -1).
 *
 * What we DON'T implement (not required by 24C):
 *   - Listen / accept (server side).
 *   - Out-of-order receive buffering. We drop OOO and rely on the
 *     peer's retransmission.
 *   - Congestion control / RFC 6298 RTO calculation. Our retransmit
 *     timer is a constant 500 ms.
 *   - Selective ACK / SACK / window scaling / timestamps options.
 *     We send 20-byte headers and skip past any options on receive.
 *   - 2*MSL TIME_WAIT delay. We collapse straight to CLOSED.
 *
 * Everything is synchronous and polling-based, the same shape as
 * dhcp.c / dns.c -- no scheduler dependency, safe to call from boot.
 *
 * Public surface:
 *   tcp_init()             -- initialise the connection table once.
 *   tcp_recv_packet()      -- hook called by ip.c for IP_PROTO_TCP.
 *   tcp_connect()          -- active open; blocks until ESTABLISHED.
 *   tcp_send()             -- write bytes; blocks until ACKed.
 *   tcp_recv()             -- read bytes; blocks until any data, EOF,
 *                             reset, or timeout.
 *   tcp_close()            -- graceful FIN exchange, then CLOSED.
 *   tcp_dump()             -- diagnostic; called by `netstat`.
 */

#ifndef TOBYOS_TCP_H
#define TOBYOS_TCP_H

#include <tobyos/types.h>

/* Connection states (subset of RFC 793 -- we only ever drive the
 * client-side active-open path, plus the symmetric close paths). */
typedef enum {
    TCP_CLOSED      = 0,
    TCP_SYN_SENT    = 1,
    TCP_ESTABLISHED = 2,
    TCP_FIN_WAIT_1  = 3,
    TCP_FIN_WAIT_2  = 4,
    TCP_CLOSE_WAIT  = 5,
    TCP_LAST_ACK    = 6,
} tcp_state_t;

/* TCP control-bit flags (byte 13 of the header). */
#define TCP_FLAG_FIN    0x01
#define TCP_FLAG_SYN    0x02
#define TCP_FLAG_RST    0x04
#define TCP_FLAG_PSH    0x08
#define TCP_FLAG_ACK    0x10
#define TCP_FLAG_URG    0x20

/* Wire header. Multi-byte fields are big-endian on the wire. */
struct __attribute__((packed)) tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;            /* sequence number (BE) */
    uint32_t ack;            /* acknowledgment number (BE) */
    uint8_t  data_off;       /* upper 4 bits = header length / 4 */
    uint8_t  flags;
    uint16_t window;         /* receive window (BE) */
    uint16_t checksum;
    uint16_t urgent;
};
#define TCP_HDR_LEN sizeof(struct tcp_hdr)

/* Extracted from data_off byte: header length in bytes (multiples
 * of 4, between 20 and 60). */
static inline unsigned tcp_hdr_bytes(uint8_t data_off) {
    return ((unsigned)(data_off >> 4)) * 4u;
}

/* Connection slot. Opaque to callers (treat the pointer as a token). */
struct tcp_conn;

/* Initialise the global connection table. Call once during boot,
 * after sock_init() and before any code that might tcp_connect(). */
bool tcp_init(void);

/* Called by ip_recv() for every IP_PROTO_TCP datagram. The caller
 * provides src_ip_be (from the IP header) and a pointer/length
 * spanning the TCP header + payload only. Always safe to call. */
void tcp_recv_packet(uint32_t src_ip_be, const void *tcp_packet, size_t len);

/* Active open. Returns an opaque connection handle on success
 * (state == ESTABLISHED), or NULL if:
 *   - no free connection slot
 *   - dst_ip_be == 0
 *   - timeout expired before SYN+ACK arrived
 *   - peer answered with RST (connection refused)
 *
 * Allocates an ephemeral local port automatically. */
struct tcp_conn *tcp_connect(uint32_t dst_ip_be, uint16_t dst_port_be,
                             uint32_t timeout_ms);

/* Send `len` bytes from `buf`. Blocks until all bytes are queued and
 * acknowledged by the peer (with simple retransmission). Returns the
 * number of bytes sent, or a negative error:
 *   -1 : invalid args or connection not ESTABLISHED
 *   -2 : peer reset / closed mid-send
 *   -3 : retransmission limit hit (link presumed dead) */
long tcp_send(struct tcp_conn *c, const void *buf, size_t len);

/* Receive up to `cap` bytes into `buf`. Blocks until at least one
 * byte arrives, the peer FINs (orderly close), the peer RSTs, or the
 * timeout expires.
 * Returns:
 *   > 0   : bytes copied
 *     0   : timeout (try again)
 *    -1   : peer closed cleanly (FIN received) AND buffer drained
 *    -2   : peer reset */
long tcp_recv(struct tcp_conn *c, void *buf, size_t cap, uint32_t timeout_ms);

/* Initiate graceful teardown. Drives the FIN exchange to completion
 * (FIN_WAIT_1 -> FIN_WAIT_2 -> CLOSED, or CLOSE_WAIT -> LAST_ACK ->
 * CLOSED depending on who FINs first), then frees the slot. Safe with
 * NULL. After return, the handle MUST NOT be used. */
void tcp_close(struct tcp_conn *c);

/* Diagnostic. `netstat` dispatches here for the TCP section. */
void tcp_dump(void);

/* ---- helpers exposed for the shell builtin ----------------------- */

/* Read-only accessor for the state of an open connection (for debug
 * printing). Returns TCP_CLOSED if c is NULL. */
tcp_state_t tcp_state(const struct tcp_conn *c);

/* Pretty-print a state enum. Returns a static string. */
const char *tcp_state_name(tcp_state_t s);

#endif /* TOBYOS_TCP_H */
