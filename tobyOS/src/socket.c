/* socket.c -- unified socket pool (UDP + TCP) + BSD-like syscall API.
 *
 * Sockets live in a fixed-size pool (SOCK_MAX). UDP sockets carry a
 * datagram ring; TCP sockets wrap the kernel's struct tcp_conn.
 *
 * Wait queue: sock_recvfrom / sys_recv use the same wq_add / wq_wake_all
 * pattern pipes use, so SIGINT cleanly unblocks a hung recv.
 */

#include <tobyos/socket.h>
#include <tobyos/udp.h>
#include <tobyos/tcp.h>
#include <tobyos/net.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/signal.h>
#include <tobyos/heap.h>
#include <tobyos/file.h>   /* file_close for SCM_RIGHTS-attached descriptions */
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/cpu.h>
#include <tobyos/cap.h>
#include <tobyos/pit.h>

static struct sock g_socks[SOCK_MAX];
static uint16_t    g_next_ephemeral = 33000;

static int sock_bind_ephemeral(struct sock *s);   /* defined below */

/* ---- wait-queue helpers (mirrors pipe.c) -----------------------
 * sock_recvfrom now actively drains the NIC while waiting (see there) rather
 * than parking on s->wq_recv, so wq_add is gone; wq_wake_all stays so
 * sock_close still releases any legacy waiter and is a safe no-op otherwise. */

static void wq_wake_all(struct proc **head) {
    struct proc *p = *head;
    *head = 0;
    while (p) {
        struct proc *next = p->next_wait;
        p->next_wait = 0;
        p->wait_head = 0;
        if (p->state == PROC_BLOCKED) {
            p->state = PROC_READY;
            sched_enqueue(p);
        }
        p = next;
    }
}

/* ---- pool ------------------------------------------------------- */

void sock_init(void) {
    memset(g_socks, 0, sizeof(g_socks));
}

static int sock_index(const struct sock *s) {
    if (!s) return -1;
    return (int)(s - g_socks);
}

static struct sock *sock_by_fd(int fd) {
    if (fd < 0 || fd >= SOCK_MAX) return NULL;
    struct sock *s = &g_socks[fd];
    return s->in_use ? s : NULL;
}

struct sock *sock_alloc(int kind) {
    if (kind != SOCK_KIND_UDP && kind != SOCK_KIND_TCP &&
        kind != SOCK_KIND_UNIX) return 0;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (!g_socks[i].in_use) {
            memset(&g_socks[i], 0, sizeof(g_socks[i]));
            g_socks[i].in_use = true;
            g_socks[i].refs   = 1;
            g_socks[i].kind   = kind;
            g_socks[i].recv_timeout_ms = 30000;
            g_socks[i].send_timeout_ms = 30000;
            return &g_socks[i];
        }
    }
    return 0;
}

/* Drop any SCM_RIGHTS descriptions still riding on a dgram (message dropped from
 * a full ring, or socket closed before the peer recvmsg'd them). Each was
 * file_clone'd by the sender, so it owns a reference that must be released. */
static void dgram_release_fds(struct sock_dgram *d) {
    if (!d) return;
    for (int i = 0; i < d->nfds; i++) {
        if (d->fds[i]) { file_close(d->fds[i]); d->fds[i] = 0; }
    }
    d->nfds = 0;
}

void sock_ref(struct sock *s) {
    if (!s || !s->in_use) return;
    s->refs++;
}

/* Drop one fd reference. Linux fd inheritance duplicates the DESCRIPTOR, not the
 * endpoint: a socket stays alive (and its AF_UNIX peer sees no EOF) until the
 * last fd across every process that inherited it is closed -- exactly the rule
 * pipes already follow via reader/writer counts. */
void sock_close(struct sock *s) {
    if (!s || !s->in_use) return;        /* already torn down -- double-close guard */
    if (--s->refs > 0) return;           /* another fd still holds this endpoint */

    /* Last reference: tell an AF_UNIX peer we are gone (wakes its blocked recv
     * as EOF) BEFORE clearing our slot, since it reads our peer_ip. No-op for
     * non-UNIX kinds and for the in-kernel fake-X endpoint. */
    sock_unix_peer_close(s);

    s->in_use = false;
    wq_wake_all(&s->wq_recv);

    if (s->kind == SOCK_KIND_TCP && s->tcp) {
        tcp_close(s->tcp);
        s->tcp = NULL;
    }

    for (int i = 0; i < SOCK_RX_DGRAMS; i++) {
        if (s->dgrams[i].payload) {
            kfree(s->dgrams[i].payload);
            s->dgrams[i].payload = 0;
        }
        dgram_release_fds(&s->dgrams[i]);   /* undelivered SCM_RIGHTS fds */
    }
    memset(s, 0, sizeof(*s));
}

struct sock *sock_lookup_by_port(uint16_t dst_port_be) {
    if (dst_port_be == 0) return 0;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (g_socks[i].in_use && g_socks[i].local_port == dst_port_be)
            return &g_socks[i];
    }
    return 0;
}

int sock_bind(struct sock *s, uint16_t port_be) {
    if (!s || !s->in_use) return -1;
    /* BSD semantics: port 0 means "assign any free ephemeral port" -- this is
     * what musl/busybox resolvers do (bind a UDP socket to 0 before sending a
     * DNS query). Treating it as an error broke `nslookup` (bind EADDRINUSE). */
    if (port_be == 0) return sock_bind_ephemeral(s);
    if (sock_lookup_by_port(port_be)) return -1;
    s->local_port = port_be;
    return 0;
}

static int sock_bind_ephemeral(struct sock *s) {
    for (int tries = 0; tries < 1000; tries++) {
        uint16_t p = g_next_ephemeral++;
        if (g_next_ephemeral >= 34000) g_next_ephemeral = 33000;
        uint16_t pbe = htons(p);
        if (!sock_lookup_by_port(pbe)) {
            s->local_port = pbe;
            return 0;
        }
    }
    return -1;
}

/* ---- delivery (called from udp_recv via extern) ---------------- */

void sock_deliver(struct sock *s,
                  uint32_t src_ip_be, uint16_t src_port_be,
                  const void *payload, size_t len) {
    if (!s || !s->in_use) return;
    if (len > ETH_MTU) len = ETH_MTU;

    if (s->count == SOCK_RX_DGRAMS) {
        struct sock_dgram *old = &s->dgrams[s->tail];
        if (old->payload) { kfree(old->payload); old->payload = 0; }
        s->tail = (uint8_t)((s->tail + 1) % SOCK_RX_DGRAMS);
        s->count--;
        s->dropped++;
    }

    uint8_t *copy = (uint8_t *)kmalloc(len ? len : 1);
    if (!copy) return;
    if (len) memcpy(copy, payload, len);

    struct sock_dgram *d = &s->dgrams[s->head];
    d->src_ip   = src_ip_be;
    d->src_port = src_port_be;
    d->len      = (uint16_t)len;
    d->payload  = copy;
    s->head = (uint8_t)((s->head + 1) % SOCK_RX_DGRAMS);
    s->count++;

    wq_wake_all(&s->wq_recv);
}

/* ---- AF_UNIX socketpair (Chromium Mojo IPC) -------------------- *
 * Two SOCK_KIND_UNIX endpoints cross-linked via `peer_ip` (= peer pool index
 * + 1; 0 = peer closed). A write enqueues ONE message into the peer's dgram
 * ring (SEQPACKET: message boundaries preserved), a read dequeues one from our
 * own. Purely in-memory -- no NIC, no CAP_NET. kbuf is a KERNEL buffer (sys_
 * read/write bounce user data first). Messages cap at 65535 (dgram len is a
 * uint16_t); Mojo's control frames are far smaller. No SCM_RIGHTS yet. */
/* Enqueue one AF_UNIX message, optionally carrying SCM_RIGHTS descriptions.
 * `files` (already file_clone'd by the caller) are handed to the message; on
 * failure they are released here so the caller never has to unwind. */
static void unix_enqueue_fds(struct sock *s, const void *payload, size_t len,
                             struct file **files, int nfiles) {
    if (!s || !s->in_use) {
        for (int i = 0; i < nfiles; i++) if (files[i]) file_close(files[i]);
        return;
    }
    if (len > 65535) len = 65535;
    if (nfiles > SOCK_SCM_MAX_FDS) nfiles = SOCK_SCM_MAX_FDS;
    if (s->count == SOCK_RX_DGRAMS) {            /* ring full: drop oldest */
        struct sock_dgram *old = &s->dgrams[s->tail];
        if (old->payload) { kfree(old->payload); old->payload = 0; }
        dgram_release_fds(old);
        s->tail = (uint8_t)((s->tail + 1) % SOCK_RX_DGRAMS);
        s->count--; s->dropped++;
    }
    uint8_t *copy = (uint8_t *)kmalloc(len ? len : 1);
    if (!copy) {
        for (int i = 0; i < nfiles; i++) if (files[i]) file_close(files[i]);
        return;
    }
    if (len) memcpy(copy, payload, len);
    struct sock_dgram *d = &s->dgrams[s->head];
    d->src_ip = 0; d->src_port = 0; d->len = (uint16_t)len; d->payload = copy;
    d->nfds = 0;
    for (int i = 0; i < nfiles; i++) d->fds[d->nfds++] = files[i];
    s->head = (uint8_t)((s->head + 1) % SOCK_RX_DGRAMS);
    s->count++;
    wq_wake_all(&s->wq_recv);
}

static void unix_enqueue(struct sock *s, const void *payload, size_t len) {
    unix_enqueue_fds(s, payload, len, 0, 0);
}

static long xserver_handle(struct sock *self, const void *buf, size_t n);

long sock_unix_send_fds(struct sock *self, const void *kbuf, size_t n,
                        struct file **files, int nfiles) {
    if (!self || !self->in_use || self->kind != SOCK_KIND_UNIX) {
        for (int i = 0; i < nfiles; i++) if (files[i]) file_close(files[i]);
        return -1;
    }
    if (self->x_server) {                        /* fake X server: no fd passing */
        for (int i = 0; i < nfiles; i++) if (files[i]) file_close(files[i]);
        return xserver_handle(self, kbuf, n);
    }
    struct sock *peer = (self->peer_ip == 0) ? 0
                      : sock_by_fd((int)self->peer_ip - 1);
    if (!peer || !peer->in_use || peer->kind != SOCK_KIND_UNIX) {
        self->peer_ip = 0;
        for (int i = 0; i < nfiles; i++) if (files[i]) file_close(files[i]);
        return -32;                              /* -EPIPE */
    }
    unix_enqueue_fds(peer, kbuf, n, files, nfiles);
#ifdef CHROMIUM_BOOT
    /* Slice 22 instrument: who is actually talking over the Mojo socketpair.
     * Chrome's children inherit fd 5 and then time out "with no connection",
     * so the question is whether the browser ever ENQUEUES to their end.
     * Bounded so a chatty channel can't drown the log. */
    {
        static int sent_logged;
        if (sent_logged < 40) {
            sent_logged++;
            struct proc *me = current_proc();
            kprintf("[unix] send pid=%d sock=%d -> peer=%d len=%u nfds=%d "
                    "peer_count=%u\n",
                    me ? me->pid : -1, sock_index(self), sock_index(peer),
                    (unsigned)n, nfiles, (unsigned)peer->count);
        }
    }
#endif
    return (long)n;
}

long sock_unix_send(struct sock *self, const void *kbuf, size_t n) {
    return sock_unix_send_fds(self, kbuf, n, 0, 0);
}

long sock_unix_recv(struct sock *self, void *kbuf, size_t n, uint32_t timeout_ms) {
    return sock_unix_recv_fds(self, kbuf, n, timeout_ms, 0, 0, 0);
}

long sock_unix_recv_fds(struct sock *self, void *kbuf, size_t n,
                        uint32_t timeout_ms,
                        struct file **out_files, int max_out, int *out_n) {
    if (out_n) *out_n = 0;
    if (!self || !self->in_use || self->kind != SOCK_KIND_UNIX) return -1;
    if (!kbuf && n) return -1;

#ifdef CHROMIUM_BOOT
    /* Slice 22 instrument: the receiving side of the same question. Tells
     * "child never called recv" from "called recv on a socket whose peer is
     * gone" (peer=-1 => the EOF arm below fires) from "called recv and nothing
     * was queued". */
    {
        static int recv_logged;
        if (recv_logged < 40) {
            recv_logged++;
            struct proc *me = current_proc();
            kprintf("[unix] recv pid=%d sock=%d peer=%d queued=%u want=%u\n",
                    me ? me->pid : -1, sock_index(self),
                    self->peer_ip ? (int)self->peer_ip - 1 : -1,
                    (unsigned)self->count, (unsigned)n);
        }
    }
#endif

    uint64_t deadline = 0;
    if (timeout_ms) {
        uint32_t hz = pit_hz(); if (hz == 0) hz = 100;
        deadline = pit_ticks() + ((uint64_t)hz * timeout_ms) / 1000u;
    }
    while (self->count == 0) {
        struct proc *me = current_proc();
        if (me->pending_signals) return EINTR_RET;
        if (!self->in_use)       return -1;
        if (self->peer_ip == 0 && !self->x_server)
            return 0;                            /* peer closed + drained -> EOF */
        if (deadline && pit_ticks() >= deadline) return 0;   /* timed out */
        /* Cooperative wait: drop the BKL so the peer thread can run + deliver
         * (unix_enqueue wakes wq_recv), idle, then re-check. */
        bool had_bkl = bkl_held();
        if (had_bkl) bkl_exit();
        sti();
        hlt();
        if (had_bkl) bkl_enter();
    }

    struct sock_dgram *d = &self->dgrams[self->tail];
    /* Stream semantics: a short read consumes from tail_off and leaves the rest
     * of this dgram queued (X11 is a byte stream; xcb reads the 8-byte setup
     * prefix then the length*4 body in two reads). For a message-boundary
     * (SEQPACKET) reader whose buffer >= the message, tail_off goes 0->len in
     * one call, so behaviour is unchanged (Mojo reads whole messages). */
    /* SCM_RIGHTS: hand the message's descriptions over on the FIRST read that
     * touches it (tail_off == 0), matching Linux where ancillary data arrives
     * with the first byte of its message. A reader with no control buffer
     * (plain read(), or out_files == NULL) DISCARDS them, as Linux does. */
    if (self->tail_off == 0 && d->nfds) {
        int give = 0;
        for (int i = 0; i < d->nfds; i++) {
            if (out_files && give < max_out) out_files[give++] = d->fds[i];
            else if (d->fds[i])             file_close(d->fds[i]);
            d->fds[i] = 0;
        }
        d->nfds = 0;
        if (out_n) *out_n = give;
    }
    size_t avail = (size_t)(d->len - self->tail_off);
    size_t copy  = avail < n ? avail : n;
    if (copy && d->payload) memcpy(kbuf, d->payload + self->tail_off, copy);
    self->tail_off = (uint16_t)(self->tail_off + copy);
    if (self->tail_off >= d->len) {              /* fully consumed -> advance */
        if (d->payload) { kfree(d->payload); d->payload = 0; }
        self->tail = (uint8_t)((self->tail + 1) % SOCK_RX_DGRAMS);
        self->count--;
        self->tail_off = 0;
    }
    return (long)copy;
}

void sock_unix_peer_close(struct sock *self) {
    if (!self || self->kind != SOCK_KIND_UNIX) return;
    if (self->x_server) return;                  /* no peer proc -- kernel side */
    if (self->peer_ip) {
        struct sock *peer = sock_by_fd((int)self->peer_ip - 1);
        if (peer && peer->in_use) {
            peer->peer_ip = 0;                   /* our slot is going away */
            wq_wake_all(&peer->wq_recv);         /* wake its blocked recv -> EOF */
        }
        self->peer_ip = 0;
    }
}

/* ---- in-kernel fake X server (headless xcb_connect) ---------------- *
 * ANGLE's Vulkan backend picks DisplayVkXcb on Linux and, after passing the WSI
 * extension check, calls xcb_connect(). Headless tobyOS has no X server, so
 * xcb_connect() fails (XCB_CONN_ERROR) -> ANGLE Display::initialize fails ->
 * eglInitialize fails -> chrome calls a NULL GL dispatch and crashes. We do NOT
 * need a real X server: chrome is --dump-dom / headless and never opens a
 * window -- we only need the xcb_connect() HANDSHAKE to succeed. So a socket
 * that connects to the X server address becomes an `x_server` loopback: when
 * the client writes its xConnClientPrefix we reply with an xConnSetupSuccess
 * advertising one 24-bit TrueColor screen/visual, which is all xcb_connect +
 * DisplayVkXcb::initialize need (they then read the cached setup, no further
 * round-trips). Anything the client writes afterwards is absorbed. */

static inline void xput16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void xput32(uint8_t *p, uint32_t v){
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

#define XSRV_ROOT_ID    0x0000002bu
#define XSRV_CMAP_ID    0x00000020u
#define XSRV_VISUAL_ID  0x00000021u

/* Build an X11 connection-setup "Success" reply (little-endian; chrome is x86,
 * byteOrder 'l'). Returns the total byte length written, or 0 if it won't fit. */
static size_t xserver_build_setup(uint8_t *o, size_t cap) {
    const uint8_t nformats = 1, nscreens = 1, ndepths = 1, nvisuals = 1;
    size_t body  = 32 + 8u*nformats + 40u*nscreens + 8u*ndepths + 24u*nvisuals;
    size_t total = 8 + body;
    if (total > cap) return 0;
    memset(o, 0, total);
    size_t i = 0;
    /* xConnSetupPrefix (8) */
    o[i++] = 1;                              /* success */
    o[i++] = 0;                              /* lengthReason */
    xput16(o+i, 11); i += 2;                 /* protocol-major-version */
    xput16(o+i, 0);  i += 2;                 /* protocol-minor-version */
    xput16(o+i, (uint16_t)(body/4)); i += 2; /* length of the data below, in 4-byte units */
    /* xConnSetup (32) */
    xput32(o+i, 0);            i += 4;        /* release-number */
    xput32(o+i, XSRV_ROOT_ID+1); i += 4;     /* resource-id-base */
    xput32(o+i, 0x001fffff);  i += 4;        /* resource-id-mask */
    xput32(o+i, 0);           i += 4;        /* motion-buffer-size */
    xput16(o+i, 0);           i += 2;        /* length of vendor (none) */
    xput16(o+i, 0xffff);      i += 2;        /* maximum-request-length */
    o[i++] = nscreens;                       /* number of SCREENs in roots */
    o[i++] = nformats;                       /* number of pixmap FORMATs */
    o[i++] = 0;                              /* image-byte-order = LSBFirst */
    o[i++] = 0;                              /* bitmap-format-bit-order = LeastSignificant */
    o[i++] = 32;                             /* bitmap-format-scanline-unit */
    o[i++] = 32;                             /* bitmap-format-scanline-pad */
    o[i++] = 8;                              /* min-keycode (must be >= 8) */
    o[i++] = 255;                            /* max-keycode */
    xput32(o+i, 0); i += 4;                  /* pad */
    /* pixmap FORMAT (8): 24-bit depth, 32 bpp */
    o[i++] = 24; o[i++] = 32; o[i++] = 32;   /* depth, bits-per-pixel, scanline-pad */
    i += 5;                                  /* pad[5] */
    /* SCREEN (40) */
    xput32(o+i, XSRV_ROOT_ID);  i += 4;      /* root window */
    xput32(o+i, XSRV_CMAP_ID);  i += 4;      /* default-colormap */
    xput32(o+i, 0x00ffffff);    i += 4;      /* white-pixel */
    xput32(o+i, 0x00000000);    i += 4;      /* black-pixel */
    xput32(o+i, 0);             i += 4;      /* current-input-masks */
    xput16(o+i, 1024); i += 2;               /* width-in-pixels */
    xput16(o+i, 768);  i += 2;               /* height-in-pixels */
    xput16(o+i, 270);  i += 2;               /* width-in-millimeters */
    xput16(o+i, 203);  i += 2;               /* height-in-millimeters */
    xput16(o+i, 1);    i += 2;               /* min-installed-maps */
    xput16(o+i, 1);    i += 2;               /* max-installed-maps */
    xput32(o+i, XSRV_VISUAL_ID); i += 4;     /* root-visual */
    o[i++] = 0;                              /* backing-stores = Never */
    o[i++] = 0;                              /* save-unders = False */
    o[i++] = 24;                             /* root-depth */
    o[i++] = ndepths;                        /* number of DEPTHs in allowed-depths */
    /* DEPTH (8) */
    o[i++] = 24;                             /* depth */
    o[i++] = 0;                              /* pad */
    xput16(o+i, nvisuals); i += 2;           /* number of VISUALTYPEs */
    i += 4;                                  /* pad */
    /* VISUALTYPE (24): TrueColor, 24-bit */
    xput32(o+i, XSRV_VISUAL_ID); i += 4;     /* visual-id */
    o[i++] = 4;                              /* class = TrueColor */
    o[i++] = 8;                              /* bits-per-rgb-value */
    xput16(o+i, 256); i += 2;                /* colormap-entries */
    xput32(o+i, 0x00ff0000); i += 4;         /* red-mask */
    xput32(o+i, 0x0000ff00); i += 4;         /* green-mask */
    xput32(o+i, 0x000000ff); i += 4;         /* blue-mask */
    i += 4;                                  /* pad */
    return i;                                /* == total */
}

/* Client -> X server bytes. The only exchange we service is the initial
 * connection setup; reply once, then absorb everything (headless ANGLE issues
 * no further X requests before --dump-dom fires). */
static long xserver_handle(struct sock *self, const void *buf, size_t n) {
    (void)buf;
    if (!self->x_setup_done) {
        if (n < 12) return (long)n;          /* await the full xConnClientPrefix */
        uint8_t reply[160];
        size_t rlen = xserver_build_setup(reply, sizeof reply);
        if (rlen) unix_enqueue(self, reply, rlen);   /* into our own rx ring */
        self->x_setup_done = 1;
        kprintf("[xsrv] setup: client sent %d bytes, replied %d bytes\n",
                (int)n, (int)rlen);
    }
    return (long)n;                          /* absorb all client bytes */
}

/* Is `name` the X display :0 socket (filesystem or abstract)? */
static bool is_x0_socket(const char *name) {
    return name && strcmp(name, "/tmp/.X11-unix/X0") == 0;
}

int sock_unix_connect_named(struct sock *s, const char *name, bool abstract) {
    (void)abstract;                          /* abstract + filesystem share a name */
    if (!s || s->kind != SOCK_KIND_UNIX) return -22;   /* -EINVAL */
    if (!is_x0_socket(name)) return -111;    /* -ECONNREFUSED: only the X socket */
    s->x_server     = 1;
    s->x_setup_done = 0;
    s->peer_ip      = 0;                      /* no real peer; x_server gates I/O */
    return 0;
}

/* Allocate a connected pair of AF_UNIX endpoints (socketpair(2)). Returns 0 and
 * sets *out_a/*out_b, or -1 if the pool is exhausted. */
int sock_unix_pair(struct sock **out_a, struct sock **out_b) {
    struct sock *a = sock_alloc(SOCK_KIND_UNIX);
    if (!a) return -1;
    struct sock *b = sock_alloc(SOCK_KIND_UNIX);
    if (!b) { sock_close(a); return -1; }
    a->peer_ip = (uint32_t)(sock_index(b) + 1);
    b->peer_ip = (uint32_t)(sock_index(a) + 1);
#ifdef CHROMIUM_BOOT
    {   /* Slice 22 instrument: map pool indices to channels, so the send/recv
         * traces above can be read as a conversation. */
        struct proc *me = current_proc();
        kprintf("[unix] pair pid=%d a=%d b=%d\n",
                me ? me->pid : -1, sock_index(a), sock_index(b));
    }
#endif
    *out_a = a;
    *out_b = b;
    return 0;
}

/* ---- UDP syscall surface --------------------------------------- */

long sock_sendto(struct sock *s, const void *buf, size_t len,
                 uint32_t dst_ip_be, uint16_t dst_port_be) {
    if (!cap_check(current_proc(), CAP_NET, "sock_sendto")) return -1;
    if (!s || !s->in_use)  return -1;
    if (!buf && len)       return -1;
    if (dst_port_be == 0)  return -1;
    if (len > ETH_MTU - 28) return -1;

    if (s->local_port == 0) {
        if (sock_bind_ephemeral(s) != 0) return -1;
    }

    if (!net_is_up()) return -4;

    if (!udp_send(s->local_port, dst_ip_be, dst_port_be, buf, len)) {
        return -3;
    }
    return (long)len;
}

long sock_recvfrom(struct sock *s, void *buf, size_t n,
                   uint32_t *src_ip_be, uint16_t *src_port_be) {
    return sock_recvfrom_to(s, buf, n, src_ip_be, src_port_be, 0);
}

long sock_recvfrom_to(struct sock *s, void *buf, size_t n,
                      uint32_t *src_ip_be, uint16_t *src_port_be,
                      uint32_t timeout_ms) {
    if (!cap_check(current_proc(), CAP_NET, "sock_recvfrom")) return -1;
    if (!s || !s->in_use)  return -1;
    if (!buf && n)         return -1;

    uint64_t deadline = 0;
    if (timeout_ms) {
        uint32_t hz = pit_hz(); if (hz == 0) hz = 100;
        deadline = pit_ticks() + ((uint64_t)hz * timeout_ms) / 1000u;
    }

    while (s->count == 0) {
        struct proc *self = current_proc();
        if (self->pending_signals) return EINTR_RET;
        if (!s->in_use)            return -1;
        if (deadline && pit_ticks() >= deadline) return 0;   /* timed out */
        /* Actively pull the NIC RX ring so inbound datagrams are delivered
         * (udp_recv -> sock_deliver) while we wait. The e1000 IRQ-driven wake
         * alone does not reliably advance a blocked UDP recv -- every other
         * blocking receiver in the stack (tcp_poll_until, dhcp_wait) drains
         * explicitly for the same reason. Drop the BKL across the idle hlt so
         * pid 0 (compositor + net pump) can run; re-take it before touching
         * shared socket state. (Was: pure wq_add + PROC_BLOCKED + sched_yield,
         * which never received an inbound datagram when no other context was
         * draining -- e.g. a boot-harness proc_wait -- a latent UDP-recv hang.) */
        struct net_dev *nd = net_default();
        if (nd && nd->rx_drain) nd->rx_drain(nd);
        if (s->count > 0) break;
        bool had_bkl = bkl_held();
        if (had_bkl) bkl_exit();
        sti();
        hlt();
        if (had_bkl) bkl_enter();
    }

    struct sock_dgram *d = &s->dgrams[s->tail];
    size_t copy = d->len < n ? d->len : n;
    if (copy && d->payload) memcpy(buf, d->payload, copy);

    if (src_ip_be)   *src_ip_be   = d->src_ip;
    if (src_port_be) *src_port_be = d->src_port;

    if (d->payload) { kfree(d->payload); d->payload = 0; }
    s->tail = (uint8_t)((s->tail + 1) % SOCK_RX_DGRAMS);
    s->count--;

    return (long)copy;
}

/* ---- Unified BSD-like kernel socket API (ksock_*) --------------- */

int ksock_socket(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET) return -1;

    int kind;
    if (type == SOCK_STREAM)     kind = SOCK_KIND_TCP;
    else if (type == SOCK_DGRAM) kind = SOCK_KIND_UDP;
    else return -1;

    struct sock *s = sock_alloc(kind);
    if (!s) return -1;
    return sock_index(s);
}

int ksock_bind(int sockfd, const struct sockaddr_in *addr) {
    struct sock *s = sock_by_fd(sockfd);
    if (!s || !addr) return -1;
    if (addr->sin_family != AF_INET) return -1;
    return sock_bind(s, addr->sin_port);
}

int ksock_connect(int sockfd, const struct sockaddr_in *addr) {
    struct sock *s = sock_by_fd(sockfd);
    if (!s || !addr) return -1;
    if (addr->sin_family != AF_INET) return -1;

    if (s->kind == SOCK_KIND_TCP) {
        struct tcp_conn *tc = tcp_connect(addr->sin_addr, addr->sin_port,
                                          s->send_timeout_ms);
        if (!tc) return -1;
        s->tcp = tc;
        return 0;
    }
    return -1;
}

int ksock_listen(int sockfd, int backlog) {
    struct sock *s = sock_by_fd(sockfd);
    if (!s) return -1;
    if (s->kind != SOCK_KIND_TCP) return -1;
    if (s->local_port == 0) return -1;

    struct tcp_conn *lsn = tcp_listen(s->local_port, backlog);
    if (!lsn) return -1;
    s->tcp = lsn;
    s->tcp_listening = true;
    return 0;
}

int ksock_accept(int sockfd, struct sockaddr_in *addr) {
    struct sock *s = sock_by_fd(sockfd);
    if (!s) return -1;
    if (s->kind != SOCK_KIND_TCP || !s->tcp_listening || !s->tcp) return -1;

    struct tcp_conn *child = tcp_accept(s->tcp, s->recv_timeout_ms);
    if (!child) return -1;

    struct sock *ns = sock_alloc(SOCK_KIND_TCP);
    if (!ns) {
        tcp_close(child);
        return -1;
    }
    ns->tcp = child;
    ns->local_port = s->local_port;

    if (addr) {
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
    }

    return sock_index(ns);
}

long ksock_send(int sockfd, const void *buf, size_t len, int flags) {
    (void)flags;
    struct sock *s = sock_by_fd(sockfd);
    if (!s) return -1;

    if (s->kind == SOCK_KIND_TCP) {
        if (!s->tcp) return -1;
        return tcp_send(s->tcp, buf, len);
    }
    return -1;
}

long ksock_recv(int sockfd, void *buf, size_t len, int flags) {
    (void)flags;
    struct sock *s = sock_by_fd(sockfd);
    if (!s) return -1;

    if (s->kind == SOCK_KIND_TCP) {
        if (!s->tcp) return -1;
        return tcp_recv(s->tcp, buf, len, s->recv_timeout_ms);
    }
    return -1;
}

int ksock_close(int sockfd) {
    struct sock *s = sock_by_fd(sockfd);
    if (!s) return -1;
    sock_close(s);
    return 0;
}

/* UDP datagram send/recv by pool fd (used by the ws2_32 sendto/recvfrom shims;
 * the BSD wrappers above are TCP-oriented). buf is kernel memory. */
long ksock_sendto(int sockfd, const void *buf, size_t len,
                  uint32_t dst_ip_be, uint16_t dst_port_be) {
    struct sock *s = sock_by_fd(sockfd);
    if (!s) return -1;
    return sock_sendto(s, buf, len, dst_ip_be, dst_port_be);
}

long ksock_recvfrom(int sockfd, void *buf, size_t len,
                    uint32_t *src_ip_be, uint16_t *src_port_be) {
    struct sock *s = sock_by_fd(sockfd);
    if (!s) return -1;
    return sock_recvfrom(s, buf, len, src_ip_be, src_port_be);
}

int ksock_setsockopt(int sockfd, int level, int optname,
                     const void *optval, size_t optlen) {
    struct sock *s = sock_by_fd(sockfd);
    if (!s) return -1;

    if (level == SOL_SOCKET) {
        if (optname == SO_RCVTIMEO && optval && optlen >= sizeof(uint32_t)) {
            s->recv_timeout_ms = *(const uint32_t *)optval;
            return 0;
        }
        if (optname == SO_SNDTIMEO && optval && optlen >= sizeof(uint32_t)) {
            s->send_timeout_ms = *(const uint32_t *)optval;
            return 0;
        }
        if (optname == SO_REUSEADDR) {
            return 0;
        }
    }
    return -1;
}

/* ---- diagnostics ----------------------------------------------- */

void sock_dump(void) {
    kprintf("sockets:\n");
    int n = 0;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (!g_socks[i].in_use) continue;
        const char *kind_str = g_socks[i].kind == SOCK_KIND_TCP ? "TCP" : "UDP";
        kprintf("  [%d]  %s  port=%u  queued=%u  dropped=%u\n",
                i, kind_str,
                (unsigned)ntohs(g_socks[i].local_port),
                (unsigned)g_socks[i].count,
                (unsigned)g_socks[i].dropped);
        n++;
    }
    if (n == 0) kprintf("  (none)\n");
}
