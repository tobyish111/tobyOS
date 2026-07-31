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
#include <tobyos/percpu.h>   /* slice 56: yield-if-ready in the recv wait loop */
#include <tobyos/smp.h>

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
    /* Slice 67: a wake here means this object became ready, which is
     * exactly the event a parked poll/select/epoll waiter is waiting
     * for -- tell them instead of letting the timer sweep find it. */
    poll_event_notify();
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
        kind != SOCK_KIND_UNIX && kind != SOCK_KIND_NETLINK) return 0;
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
#ifdef CHROMIUM_BOOT
    /* Slice 76: AF_UNIX endpoint lifecycle. The crashpad handshake sendmsg
     * fails EPIPE (slice 75) because the peer endpoint is already gone, and
     * refcounting LOOKS right by inspection -- so log the actual ref/close
     * ordering across the fork instead of reasoning about it. */
    if (s->kind == SOCK_KIND_UNIX) {
        static int rl = 0; if (rl < 40) { rl++;
            struct proc *p = current_proc();
            kprintf("[uxref] pid=%d REF slot=%d -> refs=%d\n",
                    p ? p->pid : -1, sock_index(s), s->refs);
        }
    }
#endif
}

/* Drop one fd reference. Linux fd inheritance duplicates the DESCRIPTOR, not the
 * endpoint: a socket stays alive (and its AF_UNIX peer sees no EOF) until the
 * last fd across every process that inherited it is closed -- exactly the rule
 * pipes already follow via reader/writer counts. */
void sock_close(struct sock *s) {
    if (!s || !s->in_use) return;        /* already torn down -- double-close guard */
#ifdef CHROMIUM_BOOT
    if (s->kind == SOCK_KIND_UNIX) {     /* slice 76: see sock_ref */
        static int cl = 0; if (cl < 40) { cl++;
            struct proc *p = current_proc();
            kprintf("[uxref] pid=%d CLOSE slot=%d refs=%d -> %s\n",
                    p ? p->pid : -1, sock_index(s), s->refs,
                    s->refs > 1 ? "survives" : "TEARDOWN (peer sees EOF)");
        }
    }
#endif
    if (--s->refs > 0) return;           /* another fd still holds this endpoint */

    /* Last reference: tell an AF_UNIX peer we are gone (wakes its blocked recv
     * as EOF) BEFORE clearing our slot, since it reads our peer_ip. No-op for
     * non-UNIX kinds and for the in-kernel fake-X endpoint. */
    sock_unix_peer_close(s);

    s->in_use = false;
    wq_wake_all(&s->wq_recv);

    if (s->kind == SOCK_KIND_TCP && s->tcp) {
        /* Slice 56: NEVER the blocking close here. tcp_close() waits up to 5s
         * for the peer's FIN and then lingers TIME_WAIT -- synchronously,
         * inside the caller's close(2). POSIX close() on a socket returns
         * immediately (SO_LINGER default off); chrome closes its ~8 page-load
         * connections back-to-back on its network-service IO thread, so the
         * blocking variant wedged that event loop for 50-70s ([cur] pid=40
         * RUNNING in close for 7s, repeatedly) -- no h2 WINDOW_UPDATEs, no
         * data-pipe drain, media never buffers, CDP probes unanswered.
         * tcp_close_nowait sends the FIN and lets tcp_service_tick run out
         * the handshake + TIME_WAIT in the background. */
        tcp_close_nowait(s->tcp);
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
    d->len      = (uint32_t)len;
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
 * read/write bounce user data first). Messages carry a u32 length (slice 56d:
 * the old u16 cap SILENTLY TRUNCATED >64KB Mojo messages -- see
 * unix_enqueue_fds); loud 8MiB sanity cap only. */
/* Enqueue one AF_UNIX message, optionally carrying SCM_RIGHTS descriptions.
 * `files` (already file_clone'd by the caller) are handed to the message; on
 * failure they are released here so the caller never has to unwind. */
static int unix_enqueue_fds(struct sock *s, const void *payload, size_t len,
                            struct file **files, int nfiles) {
    if (!s || !s->in_use) {
        for (int i = 0; i < nfiles; i++) if (files[i]) file_close(files[i]);
        return -1;
    }
    /* Slice 56d ROOT-CAUSE FIX (wall R1): this line used to read
     * `if (len > 65535) len = 65535;` -- SILENT truncation of any AF_UNIX
     * message over 64KB, while sock_unix_send_fds returned the FULL n to the
     * sender. Chrome's browser->renderer Mojo channel crosses 64KB right at
     * player start; the receiver's byte stream sheared mid-message, ipcz
     * validation failed, and the renderer cleanly exit(0)'d -- killing the
     * page (measured: a 4096+61439=65535-byte read immediately preceding
     * every renderer death). dgram.len is now u32; enqueue the WHOLE message.
     * Keep a LOUD sanity cap far above any real channel message. */
    if (len > (8u << 20)) {
        kprintf("[unix] REFUSING oversized dgram len=%lu (>8MiB) -- sender bug?\n",
                (unsigned long)len);
        for (int i = 0; i < nfiles; i++) if (files[i]) file_close(files[i]);
        return -1;
    }
    if (nfiles > SOCK_SCM_MAX_FDS) nfiles = SOCK_SCM_MAX_FDS;
    if (s->count == SOCK_RX_DGRAMS) {
        /* Ring full: NEVER drop -- this is a reliable stream (Mojo). Signal
         * backpressure so the sender retries once the receiver drains. The
         * passed SCM_RIGHTS fds belong to the unsent message; release them so
         * the retry re-clones cleanly (no leak, no premature consumption). */
        for (int i = 0; i < nfiles; i++) if (files[i]) file_close(files[i]);
        s->dropped++;      /* now a backpressure counter, not lost data */
        return SOCK_ERR_AGAIN;
    }
    uint8_t *copy = (uint8_t *)kmalloc(len ? len : 1);
    if (!copy) {
        for (int i = 0; i < nfiles; i++) if (files[i]) file_close(files[i]);
        return -1;
    }
    if (len) memcpy(copy, payload, len);
    struct sock_dgram *d = &s->dgrams[s->head];
    d->src_ip = 0; d->src_port = 0; d->len = (uint32_t)len; d->payload = copy;
    d->nfds = 0;
    /* Slice 77: stamp the SENDER's identity now -- SCM_CREDENTIALS is
     * delivered at dequeue, by which time the sender may have exited (that
     * is the normal case for crashpad's double-fork child). */
    {
        struct proc *sp = current_proc();
        d->snd_pid = sp ? sp->pid : 0;
        d->snd_uid = sp ? (uint32_t)sp->uid : 0;
        d->snd_gid = sp ? (uint32_t)sp->gid : 0;
    }
#ifdef CHROMIUM_BOOT
    {   uint32_t h = 2166136261u;                 /* FNV-1a over the copied bytes */
        for (size_t i = 0; i < len; i++) { h ^= copy[i]; h *= 16777619u; }
        d->chksum = h; }
#endif
    for (int i = 0; i < nfiles; i++) d->fds[d->nfds++] = files[i];
    s->head = (uint8_t)((s->head + 1) % SOCK_RX_DGRAMS);
    s->count++;
    wq_wake_all(&s->wq_recv);
    return 0;
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
    int erc = unix_enqueue_fds(peer, kbuf, n, files, nfiles);
    if (erc == SOCK_ERR_AGAIN) return SOCK_ERR_AGAIN;   /* backpressure -> EAGAIN */
    if (erc < 0) return -32;                            /* other failure -> EPIPE */
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

/* Would a send on `self` right now hit backpressure (peer RX ring full)?
 * file_poll_ready uses this so POLLOUT reflects real writability -- returning
 * POLLOUT while the peer ring is full is what let chrome overrun it. */
bool sock_unix_send_would_block(struct sock *self) {
    if (!self || self->kind != SOCK_KIND_UNIX || !self->peer_ip) return false;
    struct sock *peer = sock_by_fd((int)self->peer_ip - 1);
    return peer && peer->in_use && peer->count >= SOCK_RX_DGRAMS;
}

long sock_unix_recv(struct sock *self, void *kbuf, size_t n, uint32_t timeout_ms) {
    return sock_unix_recv_fds(self, kbuf, n, timeout_ms, 0, 0, 0);
}

/* Slice 77: credentials of the message most recently dequeued by
 * sock_unix_recv_fds, for lx_recvmsg to emit as SCM_CREDENTIALS. Valid
 * only immediately after that call (single-threaded per syscall, BKL
 * held), which is exactly how the SCM_RIGHTS hand-off already works. */
int32_t  g_unix_last_cred_pid;
uint32_t g_unix_last_cred_uid, g_unix_last_cred_gid;

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
#ifdef CHROMIUM_BOOT
    /* Prove the transport delivers EXACT bytes: recompute the payload hash on
     * first touch and compare to the value stamped at enqueue. A mismatch means
     * the queued buffer was corrupted in the kernel (aliasing / a stray write /
     * heap reuse); a match means the socket is byte-perfect and the
     * ILLEGAL_MEMORY_RANGE corruption is ABOVE it (shared memory or chrome). */
    if (self->tail_off == 0 && d->payload && d->len) {
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < d->len; i++) { h ^= d->payload[i]; h *= 16777619u; }
        if (h != d->chksum) {
            static int cm = 0;
            if (cm < 40) { cm++;
                kprintf("[sockchk] CORRUPT sock=%d len=%u enq=0x%x deq=0x%x\n",
                        sock_index(self), (unsigned)d->len,
                        (unsigned)d->chksum, (unsigned)h); }
        }
    }
#endif
    /* Stream semantics: a short read consumes from tail_off and leaves the rest
     * of this dgram queued (X11 is a byte stream; xcb reads the 8-byte setup
     * prefix then the length*4 body in two reads). For a message-boundary
     * (SEQPACKET) reader whose buffer >= the message, tail_off goes 0->len in
     * one call, so behaviour is unchanged (Mojo reads whole messages). */
    /* SCM_RIGHTS: hand the message's descriptions over on the FIRST read that
     * touches it (tail_off == 0), matching Linux where ancillary data arrives
     * with the first byte of its message. A reader with no control buffer
     * (plain read(), or out_files == NULL) DISCARDS them, as Linux does. */
    /* Slice 77: publish the sender's credentials for this message alongside
     * the fds -- same "first read of the message" rule. */
    if (self->tail_off == 0) {
        g_unix_last_cred_pid = d->snd_pid;
        g_unix_last_cred_uid = d->snd_uid;
        g_unix_last_cred_gid = d->snd_gid;
    }
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
    self->tail_off = (uint32_t)(self->tail_off + copy);
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
#ifdef CHROMIUM_BOOT
            /* Slice 56 wall R1: every AF_UNIX peer-EOF, with who caused it.
             * A renderer that cleanly exits after its browser channel EOFs
             * would show a [uxclose] against its channel right before
             * [rexit]; a [uxclose] we cannot attribute to a legit close is
             * the kernel dropping a channel. */
            static int uxc = 0;
            if (uxc < 120) { uxc++;
                struct proc *cp = current_proc();
                kprintf("[uxclose] pid=%d closed unix sock slot=%d -> peer slot=%d sees EOF\n",
                        cp ? cp->pid : -1, sock_index(self),
                        (int)self->peer_ip - 1);
            }
#endif
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
        /* Slice 56: if another proc is READY on THIS CPU, cede the core
         * instead of hlt-hogging it. A blocking recv that lands on the BSP
         * otherwise parks pid 0 (poll_tick / net pump / GUI) behind us for
         * the whole wait -- pid 0 is is_idle so no other core can steal it,
         * and the tick never preempts kernel mode -- which froze every
         * timer and poller in the box for ~50s at a time (slice-56 stall).
         * Empty queue keeps the old power-friendly hlt. */
        {
            struct percpu *me = smp_this_cpu();
            if (me && __atomic_load_n(&me->ready_head, __ATOMIC_ACQUIRE))
                sched_yield();
            else
                hlt();
        }
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

/* True when a recv would complete immediately. Non-blocking callers use this to
 * decide EAGAIN; blocking ones ignore it and park as before. TCP defers to the
 * connection's own readiness (buffered data, EOF, or error all count -- each
 * makes recv return without blocking). */
bool sock_recv_ready(struct sock *s) {
    if (!s || !s->in_use) return true;         /* dead socket: recv returns at once */
    if (s->kind == SOCK_KIND_TCP) {
        if (!s->tcp) return true;              /* unconnected -> immediate error */
        return (tcp_poll_flags(s->tcp) & (TCP_RDY_RECV | TCP_RDY_ERR)) != 0;
    }
    /* Data queued, or the peer is gone (recv returns EOF immediately). The
     * x_server exclusion mirrors file_poll_ready EXACTLY: the in-kernel fake-X
     * endpoint has no peer proc by design (peer_ip == 0 always), so without it
     * this would call an X socket permanently readable. These two predicates
     * disagreeing is precisely what once made epoll say "readable" forever
     * while recvmsg said EAGAIN -- keep them in lockstep. */
    if (s->kind == SOCK_KIND_UNIX)
        return s->count > 0 || (s->peer_ip == 0 && !s->x_server);
    return s->count > 0;                       /* UDP / netlink: queued datagram */
}

/* ---- AF_NETLINK / NETLINK_ROUTE ---------------------------------------
 *
 * Enough of rtnetlink for net::AddressTrackerLinux (chrome's network-change
 * notifier). Verified against the real source rather than guessed:
 *   socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)   -- note: NOT non-blocking
 *   bind(nl_groups = RTMGRP_IPV4_IFADDR|RTMGRP_IPV6_IFADDR|RTMGRP_NOTIFY|RTMGRP_LINK)
 *   sendto(RTM_GETADDR, NLM_F_REQUEST|NLM_F_DUMP) ; read until NLMSG_DONE
 *   sendto(RTM_GETLINK, NLM_F_REQUEST|NLM_F_DUMP) ; read until NLMSG_DONE
 * Its read loop does the FIRST recv blocking and the rest with MSG_DONTWAIT,
 * ending on NLMSG_DONE or EAGAIN. We answer each dump SYNCHRONOUSLY inside
 * send -- the reply is already queued when sendto returns -- so both the
 * blocking and the MSG_DONTWAIT reads behave correctly with no timing window.
 *
 * A link that is online must report IFF_UP|IFF_LOWER_UP|IFF_RUNNING and must
 * NOT be IFF_LOOPBACK, or AddressTrackerLinux leaves online_links_ empty and
 * chrome concludes CONNECTION_NONE -- i.e. "this machine is offline".
 *
 * We never send unsolicited notifications: tobyOS's link comes up before chrome
 * starts and never changes, which is exactly the steady state the tracker
 * expects after its initial dump. */

#define NL_NLMSG_DONE      3
#define NL_RTM_NEWLINK    16
#define NL_RTM_GETLINK    18
#define NL_RTM_NEWADDR    20
#define NL_RTM_GETADDR    22
#define NL_NLM_F_MULTI  0x002

#define NL_IFA_ADDRESS     1
#define NL_IFA_LOCAL       2
#define NL_IFA_LABEL       3
#define NL_IFLA_ADDRESS    1
#define NL_IFLA_IFNAME     3

#define NL_IFF_UP        0x0001
#define NL_IFF_BROADCAST 0x0002
#define NL_IFF_LOOPBACK  0x0008
#define NL_IFF_RUNNING   0x0040
#define NL_IFF_MULTICAST 0x1000
#define NL_IFF_LOWER_UP  0x10000

#define NL_ARPHRD_ETHER      1
#define NL_ARPHRD_LOOPBACK 772

#define NL_ALIGN(n)  (((n) + 3u) & ~3u)

struct nl_msghdr {                 /* struct nlmsghdr */
    uint32_t len;
    uint16_t type;
    uint16_t flags;
    uint32_t seq;
    uint32_t pid;
};

/* Append one netlink message header + payload + attributes into buf. */
struct nl_build {
    uint8_t *buf;
    size_t   cap;
    size_t   off;
    /* Offset of the message header currently being built, so its length can be
     * patched once every attribute has been appended. */
    size_t   msg_start;
    /* Set once anything did not fit. Every later append is then a no-op --
     * without this a truncated begin() would leave msg_start pointing at the
     * PREVIOUS message and end() would overwrite its length, corrupting a
     * message that was already complete. Netlink has no partial messages. */
    bool     full;
};

static void nlb_msg_begin(struct nl_build *b, uint16_t type, uint16_t flags,
                          uint32_t seq, uint32_t pid,
                          const void *body, size_t body_len) {
    if (b->full) return;
    if (b->off + sizeof(struct nl_msghdr) + NL_ALIGN(body_len) > b->cap) {
        b->full = true;
        return;
    }
    b->msg_start = b->off;
    struct nl_msghdr h = { 0 };
    h.type = type; h.flags = flags; h.seq = seq; h.pid = pid;
    memcpy(b->buf + b->off, &h, sizeof h);
    b->off += sizeof h;
    if (body_len) memcpy(b->buf + b->off, body, body_len);
    /* Pad the fixed body out to a 4-byte boundary; attributes start aligned. */
    memset(b->buf + b->off + body_len, 0, NL_ALIGN(body_len) - body_len);
    b->off += NL_ALIGN(body_len);
}

static void nlb_attr(struct nl_build *b, uint16_t type,
                     const void *val, size_t len) {
    if (b->full) return;
    size_t need = 4u + NL_ALIGN(len);
    if (b->off + need > b->cap) { b->full = true; return; }
    uint16_t rta_len = (uint16_t)(4u + len);          /* excludes the padding */
    memcpy(b->buf + b->off,     &rta_len, 2);
    memcpy(b->buf + b->off + 2, &type,    2);
    if (len) memcpy(b->buf + b->off + 4, val, len);
    memset(b->buf + b->off + 4 + len, 0, NL_ALIGN(len) - len);
    b->off += need;
}

static void nlb_msg_end(struct nl_build *b) {
    if (b->full) return;
    if (b->msg_start + sizeof(struct nl_msghdr) > b->cap) return;
    uint32_t total = (uint32_t)(b->off - b->msg_start);
    memcpy(b->buf + b->msg_start, &total, 4);         /* patch nlmsg_len */
}

/* struct ifaddrmsg (8 bytes) */
struct nl_ifaddrmsg {
    uint8_t  family, prefixlen, flags, scope;
    uint32_t index;
};
/* struct ifinfomsg (16 bytes) */
struct nl_ifinfomsg {
    uint8_t  family, pad;
    uint16_t type;
    int32_t  index;
    uint32_t flags, change;
};

/* Number of leading 1-bits in a network-order netmask (e.g. 255.255.255.0 -> 24). */
static uint8_t nl_prefix_len(uint32_t mask_be) {
    uint32_t m = ntohl(mask_be);
    uint8_t n = 0;
    while (m & 0x80000000u) { n++; m <<= 1; }
    return n;
}

long sock_netlink_send(struct sock *s, const void *kbuf, size_t n) {
    if (!s || !s->in_use) return -1;
    if (n < sizeof(struct nl_msghdr)) return -1;

    /* Auto-bind an unbound socket to the caller's pid, exactly as Linux does on
     * the first send. This is NOT cosmetic: glibc's check_pf (which getaddrinfo
     * runs on EVERY resolution) never calls bind, and then discards any reply
     * whose nlmsg_pid != getpid():
     *     if (nladdr.nl_pid != 0 || nlmh->nlmsg_pid != pid
     *         || nlmh->nlmsg_seq != req.nlh.nlmsg_seq) continue;
     * A discarded dump leaves seen_ipv4 = false, and getaddrinfo then filters
     * out every IPv4 answer -- so name resolution returns nothing at all. Note
     * the asymmetry that makes this sharp: when the netlink socket FAILS to
     * open, glibc assumes both families exist and carries on; it is a netlink
     * that succeeds and answers unusably that breaks DNS outright. */
    if (s->nl_pid == 0) {
        struct proc *cp = current_proc();
        if (cp) s->nl_pid = (uint32_t)cp->pid;
    }

    /* One 1 KiB reply datagram is ample: two links + one address with their
     * attributes come to a few hundred bytes, and chrome reads into 4 KiB. */
    uint8_t *out = (uint8_t *)kmalloc(1024);
    if (!out) return -1;
    struct nl_build b = { out, 1024, 0, 0, false };

    /* Walk every request in the caller's buffer (chrome sends one at a time). */
    size_t off = 0;
    while (off + sizeof(struct nl_msghdr) <= n) {
        struct nl_msghdr rq;
        memcpy(&rq, (const uint8_t *)kbuf + off, sizeof rq);
        if (rq.len < sizeof(struct nl_msghdr) || off + rq.len > n) break;

        if (rq.type == NL_RTM_GETADDR) {
            /* One IPv4 address on eth0 (index 2). Loopback is deliberately
             * omitted: the tracker discards loopback/unspecified addresses. */
            if (g_my_ip) {
                struct nl_ifaddrmsg ia = { 0 };
                ia.family    = 2;                     /* AF_INET */
                ia.prefixlen = nl_prefix_len(g_my_netmask);
                ia.scope     = 0;                     /* RT_SCOPE_UNIVERSE */
                ia.index     = 2;
                nlb_msg_begin(&b, NL_RTM_NEWADDR, NL_NLM_F_MULTI,
                              rq.seq, s->nl_pid, &ia, sizeof ia);
                nlb_attr(&b, NL_IFA_ADDRESS, &g_my_ip, 4);
                nlb_attr(&b, NL_IFA_LOCAL,   &g_my_ip, 4);
                nlb_attr(&b, NL_IFA_LABEL,   "eth0", 5);
                nlb_msg_end(&b);
            }
        } else if (rq.type == NL_RTM_GETLINK) {
            struct nl_ifinfomsg lo = { 0 };
            lo.type  = NL_ARPHRD_LOOPBACK;
            lo.index = 1;
            lo.flags = NL_IFF_UP | NL_IFF_LOOPBACK | NL_IFF_RUNNING |
                       NL_IFF_LOWER_UP;
            nlb_msg_begin(&b, NL_RTM_NEWLINK, NL_NLM_F_MULTI,
                          rq.seq, s->nl_pid, &lo, sizeof lo);
            nlb_attr(&b, NL_IFLA_IFNAME, "lo", 3);
            nlb_msg_end(&b);

            /* eth0 -- the one that must read as ONLINE. */
            struct nl_ifinfomsg eth = { 0 };
            eth.type  = NL_ARPHRD_ETHER;
            eth.index = 2;
            eth.flags = NL_IFF_UP | NL_IFF_BROADCAST | NL_IFF_MULTICAST |
                        (net_is_up() ? (NL_IFF_RUNNING | NL_IFF_LOWER_UP) : 0);
            nlb_msg_begin(&b, NL_RTM_NEWLINK, NL_NLM_F_MULTI,
                          rq.seq, s->nl_pid, &eth, sizeof eth);
            nlb_attr(&b, NL_IFLA_IFNAME,  "eth0", 5);
            nlb_attr(&b, NL_IFLA_ADDRESS, g_my_mac, ETH_ADDR_LEN);
            nlb_msg_end(&b);
        }
        /* Anything else (RTM_GETROUTE, RTM_GETNEIGH, ...): no entries, just
         * the DONE below. A dump that returns nothing is a valid answer. */

        /* Every dump terminates with NLMSG_DONE; its body is an int error
         * code, which for a successful dump is 0. */
        int32_t done = 0;
        nlb_msg_begin(&b, NL_NLMSG_DONE, NL_NLM_F_MULTI,
                      rq.seq, s->nl_pid, &done, sizeof done);
        nlb_msg_end(&b);

        off += NL_ALIGN(rq.len);
    }

    if (b.off) sock_deliver(s, 0, 0, out, b.off);
    kfree(out);
    return (long)n;                    /* the request itself was fully consumed */
}

long sock_netlink_recv(struct sock *s, void *kbuf, size_t n) {
    return sock_netlink_recv_peek(s, kbuf, n, false);
}

long sock_netlink_recv_peek(struct sock *s, void *kbuf, size_t n, bool peek) {
    if (!s || !s->in_use) return -1;
    if (s->count == 0) return SOCK_ERR_AGAIN;
    struct sock_dgram *d = &s->dgrams[s->tail];
    size_t copy = d->len < n ? d->len : n;
    if (copy && d->payload) memcpy(kbuf, d->payload, copy);
    if (peek) return (long)copy;              /* MSG_PEEK: leave it queued */
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
