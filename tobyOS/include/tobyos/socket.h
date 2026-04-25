/* socket.h -- minimal socket layer (UDP only, milestone 9).
 *
 * Sockets are allocated from a fixed kernel-side pool (SOCK_MAX = 16).
 * Each one carries a small ring of received datagrams (SOCK_RX_DGRAMS),
 * each up to ETH_MTU bytes. Receivers block on the socket's wait queue
 * via the same wq_add / wq_wake_all pattern pipes use, so SIGINT/SIGTERM
 * still cleanly unblocks a hung recvfrom.
 *
 * Userspace addressing: all IPs/ports passed across the syscall boundary
 * are network byte order (big-endian).
 *
 * Lifecycle:
 *   - sock_alloc()  -> returns an idle socket (SOCK_FREE -> SOCK_INUSE).
 *   - sock_bind()   -> assign a local port (must be unique).
 *   - sock_sendto() -> emit an UDP datagram via udp_send.
 *   - sock_recvfrom() -> blocks until at least one datagram is queued
 *                        OR a signal arrives (returns -EINTR).
 *   - sock_close()  -> wakes any blocker, frees pool slot.
 */

#ifndef TOBYOS_SOCKET_H
#define TOBYOS_SOCKET_H

#include <tobyos/types.h>
#include <tobyos/net.h>

#define SOCK_MAX               16
#define SOCK_RX_DGRAMS         8       /* per-socket ring depth */

#define SOCK_KIND_UDP          1

/* User-visible "domain"/"type" constants. We accept just two values
 * for type (mapped to SOCK_KIND_UDP); domain must be AF_INET. */
#define AF_INET                2
#define SOCK_DGRAM             2

/* One queued datagram. payload is heap-allocated, len bytes. */
struct sock_dgram {
    uint32_t  src_ip;          /* network byte order */
    uint16_t  src_port;        /* network byte order */
    uint16_t  len;
    uint8_t  *payload;         /* kmalloc'd; freed on dequeue */
};

struct proc;

struct sock {
    bool            in_use;
    int             kind;              /* only SOCK_KIND_UDP for now */
    uint16_t        local_port;        /* network byte order; 0 = unbound */

    /* RX ring. Producer: udp_recv (kernel from net_poll). Consumer:
     * sock_recvfrom (syscall). Both run on the same CPU with IRQs
     * effectively serialised, so a plain head/tail counter is fine. */
    struct sock_dgram dgrams[SOCK_RX_DGRAMS];
    uint8_t         head;              /* next slot to write */
    uint8_t         tail;              /* next slot to read  */
    uint8_t         count;             /* in-flight count    */
    uint16_t        dropped;           /* lifetime drop counter (queue full) */

    /* Wait queue of procs blocked in recvfrom. */
    struct proc    *wq_recv;
};

/* Initialise the socket pool. Called from net_init. */
void sock_init(void);

/* Allocate one socket. Returns NULL when the pool is exhausted. */
struct sock *sock_alloc(int kind);

/* Free a socket: wake every blocked recvfrom, drop queued datagrams,
 * release the pool slot. Safe with NULL. */
void sock_close(struct sock *s);

/* Bind a socket to a local UDP port (network byte order). port=0 is
 * rejected this milestone (no ephemeral allocation). Returns 0 / -1. */
int sock_bind(struct sock *s, uint16_t port_be);

/* Emit a datagram. If the socket is unbound, picks an ephemeral port
 * (33000..33999) and binds it transparently. Returns the number of
 * payload bytes sent (== len) or a negative error:
 *   -1 : invalid args / not bound / oversized
 *   -3 : ARP resolution failed (no MAC for next-hop yet)
 *   -4 : NIC missing / net not up
 */
long sock_sendto(struct sock *s, const void *buf, size_t len,
                 uint32_t dst_ip_be, uint16_t dst_port_be);

/* Receive one datagram. Blocks until data is available OR a signal
 * arrives. On return:
 *   > 0 : payload bytes copied into `buf` (truncated to `n`)
 *  -EINTR_RET (-4) : woken by a signal before any data
 *
 * src_ip_be / src_port_be are optional out-params (may be NULL). */
long sock_recvfrom(struct sock *s, void *buf, size_t n,
                   uint32_t *src_ip_be, uint16_t *src_port_be);

/* Look up the socket bound to dst_port_be, return NULL if none. */
struct sock *sock_lookup_by_port(uint16_t dst_port_be);

/* Diagnostic: shell `netstat`. Lists every active socket. */
void sock_dump(void);

#endif /* TOBYOS_SOCKET_H */
