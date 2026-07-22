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

#define SOCK_MAX               128     /* pool depth (chrome/Mojo makes many pairs) */
#define SOCK_RX_DGRAMS         64      /* per-socket UDP/UNIX ring depth. Was 8,
                                        * which chrome's Mojo bursts overflowed --
                                        * and a full ring DROPPED the oldest
                                        * message, breaking the reliable stream
                                        * (measured: 80+ drops/run -> the
                                        * flags&3==3 header corruption + lost
                                        * navigation). The ring never drops now
                                        * (EAGAIN backpressure); the larger depth
                                        * just makes backpressure rare. */
#define SOCK_ERR_AGAIN         (-11)   /* enqueue would overflow: caller retries */

#define SOCK_KIND_UDP          1
#define SOCK_KIND_TCP          2
#define SOCK_KIND_UNIX         3       /* AF_UNIX socketpair endpoint (Track B) */

/* User-visible "domain"/"type" constants. */
#define AF_UNIX                1
#define AF_INET                2
#define AF_INET6               10      /* not implemented; lx_socket -> EAFNOSUPPORT */
#define SOCK_STREAM            1       /* TCP */
#define SOCK_DGRAM             2       /* UDP */
#define SOCK_SEQPACKET         5       /* AF_UNIX message socket (Mojo uses this) */

/* AF_UNIX socketpair endpoints (Track B, Chromium Mojo IPC): a bidirectional
 * in-memory message channel. Two SOCK_KIND_UNIX socks are cross-linked; each
 * reuses `peer_ip` to hold the PEER's pool index + 1 (0 = peer closed). write
 * enqueues one message into the peer's dgram ring, read dequeues one from our
 * own (SEQPACKET: message boundaries preserved). Decls live below struct sock. */

/* setsockopt/getsockopt levels + options (Linux x86-64 numeric values). */
#define SOL_SOCKET             1
#define SO_REUSEADDR           2
#define SO_TYPE                3       /* getsockopt: SOCK_STREAM / SOCK_DGRAM */
#define SO_ERROR               4       /* getsockopt: pending socket error (0) */
#define SO_DONTROUTE           5
#define SO_BROADCAST           6
#define SO_SNDBUF              7
#define SO_RCVBUF              8
#define SO_KEEPALIVE           9
#define SO_RCVTIMEO            20
#define SO_SNDTIMEO            21
#define SO_ACCEPTCONN          30      /* getsockopt: 1 if listen()ing */

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;         /* network byte order */
    uint32_t sin_addr;         /* network byte order */
};

/* Max fds carried by one SCM_RIGHTS ancillary message (Linux allows 253; Mojo
 * passes 1-2). Bounds the per-dgram fd array. */
#define SOCK_SCM_MAX_FDS       4

struct file;

/* One queued datagram (UDP/AF_UNIX). payload is heap-allocated, len bytes.
 * fds[] carries SCM_RIGHTS-passed open descriptions (already file_clone'd by the
 * sender, so they hold their own reference); recvmsg installs them into the
 * receiver's fd table, and a dropped/closed dgram file_close()s them. */
struct sock_dgram {
    uint32_t  src_ip;          /* network byte order */
    uint16_t  src_port;        /* network byte order */
    uint16_t  len;
    uint8_t  *payload;         /* kmalloc'd; freed on dequeue */
    struct file *fds[SOCK_SCM_MAX_FDS];
    uint8_t   nfds;
};

struct proc;
struct tcp_conn;

struct sock {
    bool            in_use;
    int             refs;              /* open fds referencing this endpoint.
                                        * sock_alloc mints 1; file_clone (dup/
                                        * dup2/fork inheritance) bumps it; each
                                        * sock_close drops one and only the last
                                        * tears the endpoint down. `in_use` stays
                                        * a pure liveness flag -- the pool scans
                                        * (bind/poll/deliver) test THAT, not this. */
    int             kind;
    uint16_t        local_port;        /* network byte order; 0 = unbound */

    /* UDP: RX datagram ring. */
    struct sock_dgram dgrams[SOCK_RX_DGRAMS];
    uint8_t         head;
    uint8_t         tail;
    uint8_t         count;
    uint16_t        dropped;
    uint16_t        tail_off;          /* AF_UNIX stream: bytes already consumed
                                        * from the head dgram (partial reads leave
                                        * the remainder queued, vs SEQPACKET which
                                        * would drop it) -- X11 is a byte stream. */
    uint8_t         x_server;          /* 1 = in-kernel fake-X-server loopback:
                                        * writes are fed to the X handshake
                                        * responder, which enqueues replies back
                                        * into THIS sock's own rx ring. */
    uint8_t         x_setup_done;      /* X11 connection-setup reply delivered. */

    /* TCP: wrapped kernel connection. */
    struct tcp_conn *tcp;
    bool             tcp_listening;
    uint32_t         recv_timeout_ms;
    uint32_t         send_timeout_ms;

    /* UDP: connect() peer (network byte order; 0 = not connected). A
     * connected UDP socket lets send()/recv() and read()/write() omit the
     * address, and recvfrom filters to this peer is not enforced (SLIRP is
     * the only sender in practice). */
    uint32_t         peer_ip;
    uint16_t         peer_port;

    /* Wait queue of procs blocked in recvfrom / recv. */
    struct proc    *wq_recv;
};

/* Initialise the socket pool. Called from net_init. */
void sock_init(void);

/* Allocate one socket. kind = SOCK_KIND_UDP or SOCK_KIND_TCP. */
struct sock *sock_alloc(int kind);

/* Free a socket. */
void sock_close(struct sock *s);

/* AF_UNIX socketpair (Chromium Mojo IPC) -- see the note above struct sock. */
long sock_unix_send(struct sock *self, const void *kbuf, size_t n);
/* True if a send would hit backpressure now (peer RX ring full). Lets
 * file_poll_ready report POLLOUT only when the peer can actually accept data. */
bool sock_unix_send_would_block(struct sock *self);
long sock_unix_recv(struct sock *self, void *kbuf, size_t n, uint32_t timeout_ms);
void sock_unix_peer_close(struct sock *self);

/* Take an extra reference on an endpoint (fd inheritance via file_clone).
 * Balanced by sock_close(), which only tears down at the last reference. */
void sock_ref(struct sock *s);

/* SCM_RIGHTS fd passing (Track B, slice 20). sendmsg hands over `nfiles` ALREADY
 * file_clone'd descriptions, which ride with the message; recvmsg returns the ones
 * attached to the message it consumed (at most max_out; *out_n is set) and the
 * caller installs them into its fd table. Chrome's Mojo shared-memory channel
 * (mojo/core/channel_linux.cc) passes its memfd this way. */
long sock_unix_send_fds(struct sock *self, const void *kbuf, size_t n,
                        struct file **files, int nfiles);
long sock_unix_recv_fds(struct sock *self, void *kbuf, size_t n,
                        uint32_t timeout_ms,
                        struct file **out_files, int max_out, int *out_n);
int  sock_unix_pair(struct sock **out_a, struct sock **out_b);

/* connect(2) on an AF_UNIX stream socket to a named/abstract address. Only the
 * X server socket (/tmp/.X11-unix/X0, filesystem or abstract) is recognised: it
 * turns `s` into an in-kernel fake-X-server endpoint (see x_server above) so
 * ANGLE's DisplayVkXcb xcb_connect() succeeds headless. Returns 0 on success,
 * a negative -errno (e.g. -ECONNREFUSED) for any other address. */
int  sock_unix_connect_named(struct sock *s, const char *name, bool abstract);

/* Bind a socket to a local port (network byte order). */
int sock_bind(struct sock *s, uint16_t port_be);

/* UDP sendto / recvfrom. */
long sock_sendto(struct sock *s, const void *buf, size_t len,
                 uint32_t dst_ip_be, uint16_t dst_port_be);
long sock_recvfrom(struct sock *s, void *buf, size_t n,
                   uint32_t *src_ip_be, uint16_t *src_port_be);
/* As sock_recvfrom, but bounded: returns 0 (no datagram) when timeout_ms
 * elapses with an empty ring. timeout_ms == 0 means block indefinitely. */
long sock_recvfrom_to(struct sock *s, void *buf, size_t n,
                      uint32_t *src_ip_be, uint16_t *src_port_be,
                      uint32_t timeout_ms);

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
long ksock_sendto(int sockfd, const void *buf, size_t len,
                  uint32_t dst_ip_be, uint16_t dst_port_be);
long ksock_recvfrom(int sockfd, void *buf, size_t len,
                    uint32_t *src_ip_be, uint16_t *src_port_be);
int  ksock_close(int sockfd);
int  ksock_setsockopt(int sockfd, int level, int optname,
                      const void *optval, size_t optlen);

#endif /* TOBYOS_SOCKET_H */
