/* socket.h -- unified socket layer (UDP + TCP).
 *
 * Sockets are allocated from a fixed kernel-side pool (SOCK_MAX = 16).
 * UDP sockets carry a small ring of received datagrams (SOCK_RX_DGRAMS).
 * TCP sockets wrap a struct tcp_conn from tcp.c.
 *
 * Userspace addressing: all IPs/ports passed across the syscall boundary
 * are network byte order (big-endian).
 */

#ifndef TOBYOS_SOCKET_H
#define TOBYOS_SOCKET_H

#include <tobyos/types.h>
#include <tobyos/net.h>

#define SOCK_MAX               16
#define SOCK_RX_DGRAMS         8       /* per-socket UDP ring depth */

#define SOCK_KIND_UDP          1
#define SOCK_KIND_TCP          2

/* User-visible "domain"/"type" constants. */
#define AF_INET                2
#define SOCK_STREAM            1       /* TCP */
#define SOCK_DGRAM             2       /* UDP */

/* setsockopt levels + options. */
#define SOL_SOCKET             1
#define SO_RCVTIMEO            20
#define SO_SNDTIMEO            21
#define SO_REUSEADDR           2

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;         /* network byte order */
    uint32_t sin_addr;         /* network byte order */
};

/* One queued datagram (UDP). payload is heap-allocated, len bytes. */
struct sock_dgram {
    uint32_t  src_ip;          /* network byte order */
    uint16_t  src_port;        /* network byte order */
    uint16_t  len;
    uint8_t  *payload;         /* kmalloc'd; freed on dequeue */
};

struct proc;
struct tcp_conn;

struct sock {
    bool            in_use;
    int             kind;
    uint16_t        local_port;        /* network byte order; 0 = unbound */

    /* UDP: RX datagram ring. */
    struct sock_dgram dgrams[SOCK_RX_DGRAMS];
    uint8_t         head;
    uint8_t         tail;
    uint8_t         count;
    uint16_t        dropped;

    /* TCP: wrapped kernel connection. */
    struct tcp_conn *tcp;
    bool             tcp_listening;
    uint32_t         recv_timeout_ms;
    uint32_t         send_timeout_ms;

    /* Wait queue of procs blocked in recvfrom / recv. */
    struct proc    *wq_recv;
};

/* Initialise the socket pool. Called from net_init. */
void sock_init(void);

/* Allocate one socket. kind = SOCK_KIND_UDP or SOCK_KIND_TCP. */
struct sock *sock_alloc(int kind);

/* Free a socket. */
void sock_close(struct sock *s);

/* Bind a socket to a local port (network byte order). */
int sock_bind(struct sock *s, uint16_t port_be);

/* UDP sendto / recvfrom. */
long sock_sendto(struct sock *s, const void *buf, size_t len,
                 uint32_t dst_ip_be, uint16_t dst_port_be);
long sock_recvfrom(struct sock *s, void *buf, size_t n,
                   uint32_t *src_ip_be, uint16_t *src_port_be);

/* Look up the socket bound to dst_port_be, return NULL if none. */
struct sock *sock_lookup_by_port(uint16_t dst_port_be);

/* Diagnostic: shell `netstat`. */
void sock_dump(void);

/* ---- Unified BSD-like kernel socket API (used by syscall.c) ------ */

int  ksock_socket(int domain, int type, int protocol);
int  ksock_bind(int sockfd, const struct sockaddr_in *addr);
int  ksock_connect(int sockfd, const struct sockaddr_in *addr);
int  ksock_listen(int sockfd, int backlog);
int  ksock_accept(int sockfd, struct sockaddr_in *addr);
long ksock_send(int sockfd, const void *buf, size_t len, int flags);
long ksock_recv(int sockfd, void *buf, size_t len, int flags);
int  ksock_close(int sockfd);
int  ksock_setsockopt(int sockfd, int level, int optname,
                      const void *optval, size_t optlen);

#endif /* TOBYOS_SOCKET_H */
