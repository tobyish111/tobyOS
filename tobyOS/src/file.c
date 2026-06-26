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
#include <tobyos/klibc.h>
#include <tobyos/cpu.h>

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
    const char *cb = (const char *)buf;
    for (size_t i = 0; i < n; i++) kputc(cb[i]);
    return (long)n;
}

struct file *console_file_make(void) {
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_CONSOLE;
    return f;
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
        (*f->vfs_refs)++;
        break;
    case FILE_KIND_SOCKET:
        /* Sockets aren't dup'd via inheritance: a child shouldn't
         * silently share a parent socket. Refuse so the caller can
         * substitute a console fd if needed. */
        kfree(f);
        return 0;
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
