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
#define SOCK_RX_DGRAMS         128     /* per-socket UDP/UNIX ring depth. Was 64
                                        * (tier 2.5: X InternAtom + pump needs
                                        * more headroom; 256 blew BSS); was 8,
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
#define SOCK_KIND_NETLINK      4       /* AF_NETLINK/NETLINK_ROUTE (Track B) */

/* User-visible "domain"/"type" constants. */
#define AF_UNIX                1
#define AF_INET                2
#define AF_INET6               10      /* not implemented; lx_socket -> EAFNOSUPPORT */
#define AF_NETLINK             16      /* rtnetlink (chrome's NetworkChangeNotifier) */
#define NETLINK_ROUTE          0
#define SOCK_STREAM            1       /* TCP */
#define SOCK_DGRAM             2       /* UDP */
#define SOCK_RAW               3       /* AF_NETLINK uses this */
#define SOCK_SEQPACKET         5       /* AF_UNIX message socket (Mojo uses this) */

/* Type-flag bits OR'd into socket()/accept4()'s `type` argument. Chrome's
 * network service creates EVERY socket with SOCK_NONBLOCK and drives it from
 * epoll; a blocking recv on its IO thread stalls the whole message pump. */
#define SOCK_NONBLOCK          0x800   /* == O_NONBLOCK */
#define SOCK_CLOEXEC           0x80000

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
#define SO_PASSCRED            16      /* slice 77: receive SCM_CREDENTIALS */
#define SO_PEERCRED            17      /* slice 81: struct ucred of the peer */

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
    /* Slice 56d: uint16_t until 2026-07-28 -- and unix_enqueue_fds SILENTLY
     * TRUNCATED any AF_UNIX message > 65535 bytes while telling the sender it
     * all went out. Chrome's browser->renderer Mojo channel messages cross
     * 64KB right at player start; the sheared byte stream failed ipcz
     * validation and the renderer cleanly exit(0)'d (the slice-56 R1 wall;
     * measured as a 4096+61439=65535-byte read immediately before every
     * death). 32 bits + no truncation. */
    uint32_t  len;
    uint8_t  *payload;         /* kmalloc'd; freed on dequeue */
    struct file *fds[SOCK_SCM_MAX_FDS];
    uint8_t   nfds;
    /* Slice 77: SCM_CREDENTIALS. Linux tags every AF_UNIX message with the
     * SENDER's identity and hands it to a receiver that set SO_PASSCRED.
     * chrome's crashpad handshake depends on it: the handler replies with
     * 8 bytes + SCM_CREDENTIALS, and the browser feeds the pid straight
     * into prctl(PR_SET_PTRACER, pid). Captured at enqueue because by
     * dequeue time the sender may be gone (crashpad's first child exits
     * immediately by design). */
    int32_t   snd_pid;
    uint32_t  snd_uid, snd_gid;
#ifdef CHROMIUM_BOOT
    uint32_t  chksum;          /* FNV of payload at enqueue; verified at dequeue
                                * to prove the transport delivers exact bytes. */
#endif
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
    uint16_t        head;              /* u16: SOCK_RX_DGRAMS may exceed 255 */
    uint16_t        tail;
    uint16_t        count;
    uint16_t        dropped;
    uint32_t        tail_off;          /* AF_UNIX stream: bytes already consumed
                                        * from the head dgram (partial reads leave
                                        * the remainder queued, vs SEQPACKET which
                                        * would drop it) -- X11 is a byte stream.
                                        * u32: must span sock_dgram.len (slice 56d). */
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

    /* ---- Slice 89: SLOT GENERATION (fixes the hazard slice 78 named) ----
     * Pool slots are recycled IMMEDIATELY by sock_alloc, and an AF_UNIX
     * endpoint identifies its peer purely by `peer_ip` == pool index + 1.
     * A stale index therefore ALIASES whatever socket now owns that slot:
     * sends land on the wrong channel, and -- the fatal one -- a close
     * runs `peer->peer_ip = 0` on an unrelated LIVE socket, severing a
     * channel nobody closed. Downstream that is `sendmsg -> -EPIPE` on a
     * healthy fd and chrome's own "Crashing due to FD ownership violation"
     * + exit_group(191).
     *
     * `gen` is bumped every time a slot is handed out, so an index alone is
     * no longer an identity: holders remember the generation they linked to
     * and every resolution checks it. A recycled slot fails the check and
     * reads as "peer gone", which is both TRUE and safe. */
    uint32_t         gen;        /* bumped on each sock_alloc of this slot */
    uint32_t         peer_gen;   /* generation of the peer we linked to */

    /* ---- Non-blocking mode (O_NONBLOCK) ------------------------------
     * Set by SOCK_NONBLOCK at socket()/accept4() time or fcntl(F_SETFL).
     * Until this existed the flag was parsed and THROWN AWAY, so every
     * recv on an empty socket blocked the caller instead of returning
     * EAGAIN -- fine for the wget-shaped clients this stack grew up with,
     * fatal for an epoll-driven one like chrome's network service. */
    bool             nonblock;
    /* Slice 77: SO_PASSCRED -- receiver asked for SCM_CREDENTIALS on every
     * message (chrome's crashpad client sets it on its handshake socket). */
    bool             passcred;
    /* Slice 80: the socket's REAL type (SOCK_STREAM/DGRAM/SEQPACKET) as the
     * caller asked for it. getsockopt(SO_TYPE) used to answer from `kind`
     * alone -- TCP=STREAM, everything else=DGRAM -- so every AF_UNIX socket
     * reported SOCK_DGRAM, including the SOCK_SEQPACKET socketpairs Mojo and
     * crashpad use and then inspect. */
    uint8_t          sotype;
    /* Slice 81: credentials captured when this endpoint was created. Linux
     * answers SO_PEERCRED from the values latched at socketpair()/connect()
     * time, NOT from whoever holds the fd now -- which matters here because
     * these endpoints are inherited across fork and exec. */
    int32_t          cr_pid;
    uint32_t         cr_uid, cr_gid;

    /* ---- Non-blocking connect state ----------------------------------
     * connecting=1 between a connect() that returned EINPROGRESS and the
     * handshake resolving. so_error holds the result for getsockopt(SO_ERROR)
     * (0 = connected), which is how a poller learns the outcome; it is
     * read-and-cleared, as on Linux. conn_deadline bounds a handshake that
     * draws no reply at all (no RST, no SYN-ACK), which would otherwise sit
     * in SYN_SENT forever and make poll() silent for good. */
    uint8_t          connecting;
    int              so_error;         /* pending errno; 0 = none */
    uint64_t         conn_deadline;    /* pit ticks; 0 = no deadline */

    /* AF_NETLINK: the nl_pid this socket bound to (unique per socket, as on
     * Linux, where an unbound sendmsg auto-binds to the tid). */
    uint32_t         nl_pid;

    /* Wait queue of procs blocked in recvfrom / recv. */
    struct proc    *wq_recv;
};

/* True when a recv on this socket would return data/EOF immediately (i.e.
 * would NOT block). Used to decide EAGAIN on a non-blocking socket. */
bool sock_recv_ready(struct sock *s);

/* AF_NETLINK (NETLINK_ROUTE). Chrome's net::AddressTrackerLinux opens one to
 * learn the interface/address list and to watch for changes; without it
 * NetworkChangeNotifier has no idea whether the machine is online. We answer
 * the dump requests it makes and stay silent afterwards (our link never
 * changes), which is exactly the steady state it expects. */
long sock_netlink_send(struct sock *s, const void *kbuf, size_t n);
long sock_netlink_recv(struct sock *s, void *kbuf, size_t n);
/* As sock_netlink_recv, but `peek` (MSG_PEEK) leaves the message queued. */
long sock_netlink_recv_peek(struct sock *s, void *kbuf, size_t n, bool peek);

/* Initialise the socket pool. Called from net_init. */
void sock_init(void);

/* Allocate one socket. kind = SOCK_KIND_UDP or SOCK_KIND_TCP. */
struct sock *sock_alloc(int kind);

/* Pool index for side tables (fake X conn state). -1 if not from the pool. */
int sock_pool_index(const struct sock *s);
/* Slice 89: current generation of this socket's pool slot (0 if gone). */
uint32_t sock_generation(const struct sock *s);

/* Invoke cb(s) for every in-use AF_UNIX socket with x_server set. */
void sock_foreach_xserver(void (*cb)(struct sock *s));

/* Enqueue bytes into an AF_UNIX peer/self RX ring (used by fake X replies).
 * Returns 0 on success, -1 on hard fail, SOCK_ERR_AGAIN if the ring is full. */
int sock_unix_enqueue(struct sock *s, const void *payload, size_t len);

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
struct sock *sock_peer_of(struct sock *self);   /* slice 81: SO_PEERCRED */
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

/* Slice 86: named AF_UNIX servers -- bind(2)/listen(2)/accept(2). Needed by
 * full chrome's ProcessSingleton (SingletonSocket), which binds a
 * sockaddr_un of length 110 and listens; chrome-headless-shell has no
 * ProcessSingleton, which is why it never exercised this path. The binding
 * registry lives in socket.c so struct sock does not change size. */
int  sock_unix_bind_name(struct sock *s, const char *name, bool abstract);
const char *sock_unix_bound_name(const struct sock *s);
int  sock_unix_listen(struct sock *s);
bool sock_unix_can_accept(const struct sock *s);
int  sock_unix_accept(struct sock *s, struct sock **out);
void sock_unix_unbind(struct sock *s);

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
