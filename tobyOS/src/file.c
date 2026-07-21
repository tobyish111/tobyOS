/* file.c -- struct file backend dispatch + console-as-file.
 *
 * Each fd in struct proc.fds[] owns a heap-allocated struct file. There
 * is no struct-level refcount: file_clone() always allocates a fresh
 * struct, file_close() always frees one. The shared state (the pipe's
 * reader/writer counts) is what actually tracks "is this end alive".
 *
 * Console reads are non-blocking: kbd_trygetc() either returns the next
 * buffered char or -1. Returning 0 (EOF) when the buffer is empty would
 * confuse user programs reading from a terminal; instead we busy-poll.
 * For now the only user of console-stdin in this kernel is /bin/cat
 * when invoked WITHOUT a pipe (rare -- the shell prefers builtins for
 * file paths), so the busy-poll is acceptable. With pipes the read/write
 * end blocks properly via pipe.c's wait queues.
 */

#include <tobyos/file.h>
#include <tobyos/pipe.h>
#include <tobyos/socket.h>
#include <tobyos/tcp.h>
#include <tobyos/signal.h>   /* EINTR_RET */
#include <tobyos/gui.h>
#include <tobyos/term.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/keyboard.h>
#include <tobyos/signal.h>
#include <tobyos/tty.h>
#include <tobyos/pty.h>
#include <tobyos/mmap.h>   /* memfd_* (FILE_KIND_MEMFD) */
#include <tobyos/klibc.h>
#include <tobyos/cpu.h>
#include <tobyos/sched.h>
#include <tobyos/abi/abi.h>

static long console_read(void *buf, size_t n) {
    if (n == 0) return 0;
    /* Track B/B21: the console is a real controlling TTY now -- reads go
     * through the cooked line discipline (canonical line editing + ECHO, or
     * raw/VMIN when an app clears ICANON), which also honours the termios the
     * program set via TCSETS. tty_console_read writes into this kernel buffer;
     * sys_read copies it out. Signal interruption surfaces as EINTR_RET so the
     * syscall return path delivers the pending signal. */
    return tty_console_read((char *)buf, n);
}

static long console_write(const void *buf, size_t n) {
    /* B21/B22: route through the TTY output path so it can answer terminal
     * queries (ESC[6n cursor-position report) that interactive line editors
     * issue; every byte still reaches the console + serial. */
    tty_console_output((const char *)buf, n);
    return (long)n;
}

struct file *console_file_make(void) {
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_CONSOLE;
    return f;
}

/* ---- eventfd (Track C) ----------------------------------------------------
 * A counting object: write(8) adds to the counter, read(8) drains it (or
 * decrements by 1 in EFD_SEMAPHORE mode). libcurl's multi handle creates one
 * as its wakeup fd; it's added to a poll set and only becomes readable once
 * curl_multi_wakeup() writes to it, so for a plain transfer it simply sits
 * not-readable. Blocking read/write fall back to a cooperative yield loop
 * (only reachable for the non-NONBLOCK case, which curl never uses). */
#define EFD_MAXVAL 0xfffffffffffffffeULL
#define EFD_EAGAIN 11           /* Linux EAGAIN (== EWOULDBLOCK) */

struct eventfd {
    uint64_t count;
    uint32_t flags;
    int      refs;
};

struct file *eventfd_file_make(unsigned int initval, unsigned int flags) {
    struct eventfd *e = (struct eventfd *)kmalloc(sizeof(*e));
    if (!e) return 0;
    e->count = (uint64_t)initval;
    e->flags = flags;
    e->refs  = 1;
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) { kfree(e); return 0; }
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_EVENTFD;
    f->efd  = e;
    return f;
}

static long eventfd_read(struct file *f, void *buf, size_t n) {
    struct eventfd *e = f->efd;
    if (!e) return -1;
    if (n < 8) return -ABI_EINVAL;
    while (e->count == 0) {
        if (e->flags & EFD_NONBLOCK) return -EFD_EAGAIN;
        sched_yield();                 /* cooperative wait for a writer */
    }
    uint64_t out;
    if (e->flags & EFD_SEMAPHORE) { out = 1; e->count -= 1; }
    else                         { out = e->count; e->count = 0; }
    memcpy(buf, &out, 8);
    return 8;
}

int eventfd_pollin(struct file *f) {
    return (f && f->efd && f->efd->count > 0) ? 1 : 0;
}

static long eventfd_write(struct file *f, const void *buf, size_t n) {
    struct eventfd *e = f->efd;
    if (!e) return -1;
    if (n < 8) return -ABI_EINVAL;
    uint64_t add;
    memcpy(&add, buf, 8);
    if (add == 0xffffffffffffffffULL) return -ABI_EINVAL;  /* reserved */
    while (e->count + add > EFD_MAXVAL) {
        if (e->flags & EFD_NONBLOCK) return -EFD_EAGAIN;
        sched_yield();                 /* cooperative wait for a reader */
    }
    e->count += add;
    return 8;
}

struct file *file_clone(struct file *src) {
    if (!src) return 0;
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    f->kind = src->kind;
    switch (src->kind) {
    case FILE_KIND_CONSOLE:
    case FILE_KIND_NULL:
        break;
    case FILE_KIND_PIPE_R:
        f->pipe = src->pipe;
        f->pipe->readers++;
        break;
    case FILE_KIND_PIPE_W:
        f->pipe = src->pipe;
        f->pipe->writers++;
        break;
    case FILE_KIND_VFS:
        /* Milestone 25A: dup()/dup2() of a VFS fd. The two struct file
         * objects MUST refer to the same underlying open description so
         * that exactly one ops->close happens when the last fd is shut.
         * We byte-copy the vfs_file (ops/mnt/priv + cursor, etc.) and
         * share the side-allocated refcount that sys_open minted; we
         * cannot lazily allocate the refcount here because the source
         * file might already be the only ref + we'd race with its own
         * close. (sys_open is the chokepoint that *must* mint refs=1.)
         *
         * If the source has no refcount (defensive -- shouldn't happen
         * for VFS kinds anymore, but the kernel had legacy paths that
         * stuffed FILE_KIND_VFS structs together with embedded handles
         * before refs existed), we refuse the clone rather than risk
         * a double-free. */
        if (!src->vfs_refs) {
            kfree(f);
            return 0;
        }
        f->vfs      = src->vfs;
        f->vfs_refs = src->vfs_refs;
        f->o_accmode = src->o_accmode;   /* dup'd fd keeps the access mode (F_GETFL) */
        /* Carry the shared-mapping region across dup/fork/SCM_RIGHTS. This is
         * what actually makes cross-process shared memory work once the file is
         * unlinked: the inode number is gone (and reused), so the descriptor is
         * the only thing tying the sharers together. Entries are permanent for
         * now, so there is no refcount to bump here. */
        f->shm      = src->shm;
        (*f->vfs_refs)++;
        break;
    case FILE_KIND_SOCKET:
        /* dup()/dup2()/fork inheritance share the SAME endpoint -- POSIX
         * duplicates the descriptor, not the socket. (This used to refuse the
         * clone, reasoning that a child shouldn't silently share a parent
         * socket; that is simply not what Linux fork() does, and it left the
         * child with NULL for every socket fd. Chrome's launcher fork()s and
         * then dup2()s the inherited Mojo socketpair fd into the child, so the
         * refusal made every child _exit(127) and the GPU process never came
         * up.) The refcount keeps the pool slot alive, and the AF_UNIX peer
         * only sees EOF once the last holder closes. */
        f->sock = src->sock;
        sock_ref(f->sock);
        break;
    case FILE_KIND_WINDOW:
        /* GUI windows are exclusively owned by their creator; cloning
         * a window across fork would leave two processes racing to
         * draw + poll events into the same backing store. Refuse the
         * clone -- the caller falls back to a console fd. */
        kfree(f);
        return 0;
    case FILE_KIND_TERM:
        /* Terminal sessions are owned by the GUI terminal app -- no
         * sharing across fork. */
        kfree(f);
        return 0;
    case FILE_KIND_PTY_MASTER:
    case FILE_KIND_PTY_SLAVE:
        /* dup()/dup2()/fork inheritance of a PTY end shares the same pair;
         * bump the end's refcount so close()s balance out. */
        f->pty = src->pty;
        pty_clone_ref(f);
        break;
    case FILE_KIND_EVENTFD:
        /* dup/fork share the same counter object; bump its refcount. */
        f->efd = src->efd;
        if (f->efd) f->efd->refs++;
        break;
    case FILE_KIND_MEMFD:
        /* dup/dup2/fork share the SAME page-backed object; bump its refcount.
         * The per-fd cursor (vfs.pos) starts fresh at 0 for the new fd. */
        f->memfd = src->memfd;
        memfd_ref(f->memfd);
        break;
    case FILE_KIND_DIR:
        /* Deep-copy the directory path so each clone owns its own buffer
         * (avoids a double-free on close); resume offset carries over. */
        if (src->dirpath) {
            size_t n = strlen(src->dirpath) + 1;
            f->dirpath = (char *)kmalloc(n);
            if (!f->dirpath) { kfree(f); return 0; }
            memcpy(f->dirpath, src->dirpath, n);
        }
        f->dir_off = src->dir_off;
        break;
    case FILE_KIND_EVDEV:
        /* dir_off holds the evdev minor (0=kbd,1=mouse); the queue is global,
         * so a clone just remembers which device it reads. */
        f->dir_off = src->dir_off;
        break;
    }
    return f;
}

void file_close(struct file *f) {
    if (!f) return;
    switch (f->kind) {
    case FILE_KIND_PIPE_R:
        pipe_close_reader(f->pipe);
        break;
    case FILE_KIND_PIPE_W:
        pipe_close_writer(f->pipe);
        break;
    case FILE_KIND_VFS:
        /* Drop our share of the open-file description. ops->close runs
         * exactly once, when the last fd referencing this handle is
         * closed. If vfs_refs is NULL the file was minted before the
         * 25A refcount path (defensive only -- modern sys_open always
         * allocates refs=1) so fall back to the legacy unconditional
         * close so we don't leak the priv. */
        if (f->vfs_refs) {
            if (--(*f->vfs_refs) == 0) {
                if (f->vfs.ops) (void)f->vfs.ops->close(&f->vfs);
                kfree(f->vfs_refs);
            }
        } else {
            if (f->vfs.ops) (void)f->vfs.ops->close(&f->vfs);
        }
        break;
    case FILE_KIND_SOCKET:
        /* Drops one reference. The AF_UNIX peer-EOF wake now happens INSIDE
         * sock_close at the last reference -- doing it here would signal EOF
         * as soon as any one inheritor closed its copy. */
        sock_close(f->sock);
        break;
    case FILE_KIND_WINDOW:
        gui_window_close(f->win);
        break;
    case FILE_KIND_TERM:
        term_session_close(f->term);
        break;
    case FILE_KIND_DIR:
        if (f->dirpath) kfree(f->dirpath);
        break;
    case FILE_KIND_EPOLL:
        if (f->epoll) kfree(f->epoll);
        break;
    case FILE_KIND_PTY_MASTER:
    case FILE_KIND_PTY_SLAVE:
        pty_close(f);
        break;
    case FILE_KIND_EVENTFD:
        if (f->efd && --f->efd->refs == 0) kfree(f->efd);
        break;
    case FILE_KIND_MEMFD:
        memfd_unref(f->memfd);   /* frees pages+object at the last ref */
        break;
    case FILE_KIND_CONSOLE:
    case FILE_KIND_NULL:
        break;
    }
    kfree(f);
}

long file_read(struct file *f, void *buf, size_t n) {
    if (!f || !buf) return -1;
    if (n == 0) return 0;
    switch (f->kind) {
    case FILE_KIND_CONSOLE:
        return console_read(buf, n);
    case FILE_KIND_PTY_MASTER:
        return pty_master_read(f, buf, n);
    case FILE_KIND_PTY_SLAVE:
        return pty_slave_read(f, buf, n);
    case FILE_KIND_EVENTFD:
        return eventfd_read(f, buf, n);
    case FILE_KIND_MEMFD: {
        long r = memfd_read(f->memfd, f->vfs.pos, buf, n);
        if (r > 0) f->vfs.pos += (size_t)r;
        return r;
    }
    case FILE_KIND_DEVNULL:
        return 0;                                  /* always EOF */
    case FILE_KIND_DEVZERO:
        memset(buf, 0, n);
        return (long)n;                            /* endless zeroes */
    case FILE_KIND_PIPE_R:
        return pipe_read(f->pipe, buf, n);
    case FILE_KIND_VFS:
        if (!f->vfs.ops || !f->vfs.ops->read) return -1;
        return f->vfs.ops->read(&f->vfs, buf, n);
    case FILE_KIND_SOCKET:
        /* A CONNECTED TCP socket is a byte stream: read() == recv(). A
         * connect()-ed UDP socket reads the next datagram. */
        if (f->sock && f->sock->kind == SOCK_KIND_TCP && f->sock->tcp &&
            !f->sock->tcp_listening) {
            long r = tcp_recv(f->sock->tcp, buf, n,
                              f->sock->recv_timeout_ms);
            return (r == -1) ? 0 : r;    /* peer FIN -> EOF */
        }
        if (f->sock && f->sock->kind == SOCK_KIND_UDP) {
            long r = sock_recvfrom_to(f->sock, buf, n, 0, 0,
                                      f->sock->recv_timeout_ms);
            return (r == EINTR_RET) ? -1 : (r < 0 ? 0 : r);
        }
        if (f->sock && f->sock->kind == SOCK_KIND_UNIX) {
            /* AF_UNIX socketpair: block for one message (0 = EOF/peer closed). */
            long r = sock_unix_recv(f->sock, buf, n, 0);
            return (r == EINTR_RET) ? -1 : r;
        }
        return -2;
    case FILE_KIND_WINDOW:
        /* Windows are drawn via SYS_GUI_FILL/TEXT and event-polled via
         * SYS_GUI_POLL_EVENT; raw byte streams are meaningless here. */
        return -2;
    case FILE_KIND_TERM:
        /* Terminals are operated on via SYS_TERM_READ / SYS_TERM_WRITE
         * only. */
        return -2;
    case FILE_KIND_PIPE_W:
    case FILE_KIND_NULL:
    default:
        return -1;
    }
}

long file_write(struct file *f, const void *buf, size_t n) {
    if (!f || !buf) return -1;
    if (n == 0) return 0;
    switch (f->kind) {
    case FILE_KIND_CONSOLE:
        return console_write(buf, n);
    case FILE_KIND_PTY_MASTER:
        return pty_master_write(f, buf, n);
    case FILE_KIND_PTY_SLAVE:
        return pty_slave_write(f, buf, n);
    case FILE_KIND_EVENTFD:
        return eventfd_write(f, buf, n);
    case FILE_KIND_MEMFD: {
        long w = memfd_write(f->memfd, f->vfs.pos, buf, n);
        if (w > 0) f->vfs.pos += (size_t)w;
        return w;
    }
    case FILE_KIND_DEVNULL:
    case FILE_KIND_DEVZERO:
        return (long)n;                            /* swallow everything */
    case FILE_KIND_PIPE_W:
        return pipe_write(f->pipe, buf, n);
    case FILE_KIND_VFS:
        if (!f->vfs.ops || !f->vfs.ops->write) return -1;
        return f->vfs.ops->write(&f->vfs, buf, n);
    case FILE_KIND_SOCKET:
        /* Connected TCP byte stream: write() == send(). A connect()-ed UDP
         * socket sends a datagram to its peer. */
        if (f->sock && f->sock->kind == SOCK_KIND_TCP && f->sock->tcp &&
            !f->sock->tcp_listening) {
            long w = tcp_send(f->sock->tcp, buf, n);
            return (w < 0) ? -1 : w;
        }
        if (f->sock && f->sock->kind == SOCK_KIND_UDP && f->sock->peer_port) {
            long w = sock_sendto(f->sock, buf, n,
                                 f->sock->peer_ip, f->sock->peer_port);
            return (w < 0) ? -1 : w;
        }
        if (f->sock && f->sock->kind == SOCK_KIND_UNIX) {
            /* AF_UNIX socketpair: deliver one message to the peer. */
            return sock_unix_send(f->sock, buf, n);
        }
        return -2;
    case FILE_KIND_WINDOW:
        return -2;
    case FILE_KIND_TERM:
        return -2;
    case FILE_KIND_PIPE_R:
    case FILE_KIND_NULL:
    default:
        return -1;
    }
}
