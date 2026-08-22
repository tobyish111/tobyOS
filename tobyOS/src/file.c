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
#include <tobyos/snd_pcm.h>
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
#include <tobyos/rng.h>   /* getrandom + /dev/urandom entropy */
#include <tobyos/cpu.h>
#include <tobyos/sched.h>
#include <tobyos/proc.h>   /* current_proc -- [efd] Mojo-notification trace */
#include <tobyos/perf.h>   /* perf_now_ns -- timerfd deadlines (slice 3) */
#include <tobyos/nsproxy.h>  /* ns_file_clone_ref / ns_file_close (slice 8) */
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
    uint32_t id;     /* stable object identity, so a write in one process can be
                      * matched to the poll/read in another (Mojo notification). */
};

int eventfd_id(struct file *f) { return (f && f->efd) ? (int)f->efd->id : -1; }

struct file *eventfd_file_make(unsigned int initval, unsigned int flags) {
    struct eventfd *e = (struct eventfd *)kmalloc(sizeof(*e));
    if (!e) return 0;
    static uint32_t s_efd_id = 1;
    e->count = (uint64_t)initval;
    e->flags = flags;
    e->refs  = 1;
    e->id    = s_efd_id++;
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) { kfree(e); return 0; }
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_EVENTFD;
    f->efd  = e;
#ifdef CHROMIUM_BOOT
    {   static int ec = 0;
        if (ec < 200) { ec++;
            struct proc *me = current_proc();
            kprintf("[efd] pid=%d CREATE efd_id=%u flags=0x%x\n",
                    me ? me->pid : -1, e->id, flags); } }
#endif
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
    /* Slice 96: the drain makes the eventfd WRITABLE again -- wake any
     * EPOLLOUT poller / blocked writer (see the write-side note below). */
    poll_event_notify();
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
#ifdef CHROMIUM_BOOT
    /* [efd] who signals which eventfd OBJECT. Correlate the id here with the id
     * in [epreg] to see whether the browser ever signals the eventfd the
     * renderer's IO thread polls (V1) and whether they are the SAME object (V2). */
    {   static int ew = 0;
        if (ew < 300) { ew++;
            struct proc *me = current_proc();
            kprintf("[efd] pid=%d WRITE efd_id=%u +%lu -> count=%lu\n",
                    me ? me->pid : -1, e->id, (unsigned long)add,
                    (unsigned long)e->count); } }
#endif
    /* Slice 96: eventfd was a READINESS SOURCE WITH NO EVENT HOOK -- the
     * slice-67 design's "missed hook costs latency, bounded by the sweep"
     * held only where poll_tick actually runs. In the console boot harness
     * (pid 0 parked in proc_wait, never in idle_loop) the sweep never ran
     * and a cross-thread eventfd wake was LOST FOREVER (EFDTEST FAIL,
     * pre-existing, A/B'd vs HEAD). Under chrome it silently meant every
     * message-pump signal could ride the sweep's latency instead of waking
     * the pump now. Write syscalls hold the BKL, per the API's convention. */
    poll_event_notify();
    return 8;
}

/* ==========================================================================
 * Linux slice 3: timerfd + signalfd
 *
 * Both are "wait for X through the poll loop" objects, built on the eventfd
 * shape. The single most important thing here is the poll_event_notify()
 * discipline slice 96 established: a source that becomes ready without
 * announcing it leaves an already-blocked poller asleep forever.
 * ========================================================================== */

struct timerfd {
    uint64_t deadline_ns;   /* absolute monotonic; 0 == disarmed */
    uint64_t interval_ns;   /* 0 == one-shot */
    uint64_t last_seen_ns;  /* expiries already accounted to the reader */
    uint32_t flags;
    int      clockid;
    int      refs;
};

struct signalfd {
    uint64_t mask;          /* TOBYOS numbering: bit == signo */
    uint32_t flags;
    int      refs;
};

/* Expiries that have elapsed but not yet been reported. Computed on demand --
 * there is no timer interrupt driving this, which is why every entry point
 * (read AND poll) has to call it rather than trusting a counter. */
static uint64_t timerfd_pending_count(struct timerfd *t) {
    if (!t || t->deadline_ns == 0) return 0;
    uint64_t now = perf_now_ns();
    if (now < t->deadline_ns) return 0;
    if (t->interval_ns == 0) return 1;                 /* one-shot */
    uint64_t elapsed = now - t->deadline_ns;
    return 1 + elapsed / t->interval_ns;
}

struct file *timerfd_file_make(int clockid, unsigned int flags) {
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    struct timerfd *t = (struct timerfd *)kmalloc(sizeof(*t));
    if (!t) { kfree(f); return 0; }
    memset(t, 0, sizeof(*t));
    t->refs = 1; t->clockid = clockid; t->flags = flags;
    f->kind = FILE_KIND_TIMERFD;
    f->tfd  = t;
    return f;
}

int timerfd_pollin(struct file *f) {
    return (f && f->tfd && timerfd_pending_count(f->tfd) > 0) ? 1 : 0;
}

int timerfd_set(struct file *f, uint64_t value_ns, uint64_t interval_ns,
                int absolute, uint64_t *old_value, uint64_t *old_interval) {
    if (!f || !f->tfd) return -1;
    struct timerfd *t = f->tfd;
    uint64_t now = perf_now_ns();
    if (old_interval) *old_interval = t->interval_ns;
    if (old_value) {
        *old_value = (t->deadline_ns && t->deadline_ns > now)
                   ? t->deadline_ns - now : 0;
    }
    if (value_ns == 0) {                 /* disarm */
        t->deadline_ns = 0; t->interval_ns = 0;
    } else {
        t->deadline_ns = absolute ? value_ns : now + value_ns;
        t->interval_ns = interval_ns;
    }
    t->last_seen_ns = 0;
    /* Arming can make the fd ready immediately (a deadline already in the
     * past, which callers do use as "fire now"), so announce it. */
    poll_event_notify();
    return 0;
}

int timerfd_get(struct file *f, uint64_t *value_ns, uint64_t *interval_ns) {
    if (!f || !f->tfd) return -1;
    struct timerfd *t = f->tfd;
    uint64_t now = perf_now_ns();
    if (interval_ns) *interval_ns = t->interval_ns;
    if (value_ns) {
        *value_ns = (t->deadline_ns && t->deadline_ns > now)
                  ? t->deadline_ns - now : 0;
    }
    return 0;
}

/* read(2) on a timerfd yields a u64 expiry count and RESETS it. Blocks until
 * at least one expiry unless O_NONBLOCK. */
static long timerfd_read(struct file *f, void *buf, size_t n) {
    if (n < 8) return -ABI_EINVAL;
    struct timerfd *t = f->tfd;
    for (;;) {
        uint64_t cnt = timerfd_pending_count(t);
        if (cnt > 0) {
            /* Re-base so the same expiries are not reported twice. */
            if (t->interval_ns == 0) t->deadline_ns = 0;         /* one-shot done */
            else t->deadline_ns += cnt * t->interval_ns;
            memcpy(buf, &cnt, 8);
            return 8;
        }
        if (t->flags & EFD_NONBLOCK) return -EFD_EAGAIN;
        if (t->deadline_ns == 0) {
            /* Disarmed and non-blocking-less: a read would hang forever.
             * Linux blocks here too, but a disarmed timer that nobody will
             * arm is a caller bug, so make it visibly EAGAIN instead of a
             * silent hang -- easier to debug and strictly more useful. */
            return -EFD_EAGAIN;
        }
        struct proc *p = current_proc();
        if (p && p->pending_signals) return EINTR_RET;
        sched_yield();
    }
}

struct file *signalfd_file_make(uint64_t mask, unsigned int flags) {
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    struct signalfd *s = (struct signalfd *)kmalloc(sizeof(*s));
    if (!s) { kfree(f); return 0; }
    memset(s, 0, sizeof(*s));
    s->refs = 1; s->mask = mask; s->flags = flags;
    f->kind = FILE_KIND_SIGNALFD;
    f->sfd  = s;
    return f;
}

void signalfd_setmask(struct file *f, uint64_t mask) {
    if (f && f->sfd) f->sfd->mask = mask;
}

int signalfd_pollin(struct file *f) {
    struct proc *p = current_proc();
    if (!f || !f->sfd || !p) return 0;
    return (p->pending_signals & f->sfd->mask) ? 1 : 0;
}

/* read(2) on a signalfd yields struct signalfd_siginfo (128 bytes) and
 * CONSUMES the signal -- it is delivered here instead of to a handler. */
static long signalfd_read(struct file *f, void *buf, size_t n) {
    if (n < 128) return -ABI_EINVAL;
    struct signalfd *s = f->sfd;
    struct proc *p = current_proc();
    if (!p) return -ABI_EINVAL;
    for (;;) {
        uint64_t hit = p->pending_signals & s->mask;
        if (hit) {
            int signo = 0;
            for (int i = 0; i < 32; i++)
                if (hit & (1ull << i)) { signo = i; break; }
            p->pending_signals   &= ~(1u << signo);
            p->sigstate.pending  &= ~SIGMASK(signo);
            uint8_t si[128];
            memset(si, 0, sizeof si);
            *(uint32_t *)&si[0]  = (uint32_t)signo;              /* ssi_signo */
            *(uint32_t *)&si[8]  = 0;                            /* ssi_code  */
            *(uint32_t *)&si[12] = (uint32_t)p->sigstate.si_pid[signo]; /* ssi_pid */
            *(uint32_t *)&si[16] = p->sigstate.si_uid[signo];    /* ssi_uid   */
            memcpy(buf, si, sizeof si);
            return 128;
        }
        if (s->flags & EFD_NONBLOCK) return -EFD_EAGAIN;
        sched_yield();
    }
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
         * the only thing tying the sharers together. Slice 122: entries are no
         * longer permanent -- the pin count is what keeps this one alive. */
        f->shm      = src->shm;
        if (f->shm) shm_cache_pin(f->shm);
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
        f->sock_gen = sock_generation(f->sock);   /* slice 89 */
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
    /* Linux slice 3: same sharing rule -- dup()/fork() must observe ONE timer
     * and ONE signal mask, not private copies. */
    case FILE_KIND_TIMERFD:
        f->tfd = src->tfd;
        if (f->tfd) f->tfd->refs++;
        break;
    case FILE_KIND_SIGNALFD:
        f->sfd = src->sfd;
        if (f->sfd) f->sfd->refs++;
        break;
    /* Slice 8: an nsfd must survive fork/dup with the namespace still pinned --
     * that is the whole point of holding a reference through the descriptor. */
    case FILE_KIND_NSFD:
        ns_file_clone_ref(f, src);
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
    case FILE_KIND_DRM:
        /* Slice 104: dir_off holds the DRM char-device MINOR (renderD128
         * -> 128, card0 -> 0), and the whole device is global state, so a
         * clone only has to remember which node it is. Without this case
         * the switch fell through and left dir_off = 0, so every DUP of a
         * render node stat'd as minor 0 -- and Mesa dups the fd
         * (os_dupfd_cloexec) before probing it, which sent libdrm looking
         * for /sys/dev/char/226:0 instead of 226:128. */
        f->dir_off = src->dir_off;
        break;
    }
    return f;
}

void file_close(struct file *f) {
    if (!f) return;
    switch (f->kind) {
    case FILE_KIND_SND:
        /* Audio slice 3: release the substream and stop the DAC, or the
         * next open sees in_use and returns ENOENT forever. */
        lxsnd_close(f);
        break;
    case FILE_KIND_PIPE_R:
        pipe_close_reader(f->pipe);
        break;
    case FILE_KIND_PIPE_W:
        pipe_close_writer(f->pipe);
        break;
    case FILE_KIND_VFS:
        /* Slice 122: this struct file's shm pin dies with it. Pin count, not
         * vfs_refs, is what keeps the shared-page region alive: mappings can
         * outlive every fd (chrome closes the fd right after mmap), so the
         * region is only reaped once pins==0 AND no address space maps it. */
        if (f->shm) { shm_cache_unpin(f->shm); f->shm = 0; }
        /* Drop our share of the open-file description. ops->close runs
         * exactly once, when the last fd referencing this handle is
         * closed. If vfs_refs is NULL the file was minted before the
         * 25A refcount path (defensive only -- modern sys_open always
         * allocates refs=1) so fall back to the legacy unconditional
         * close so we don't leak the priv. */
        if (f->vfs_refs) {
            if (--(*f->vfs_refs) == 0) {
                /* The open file description dies here, and flock(2) locks
                 * are owned by the DESCRIPTION -- release before the refs
                 * pointer (their owner identity) is freed. */
                { extern void fl_release_ofd(void *ofd);
                  fl_release_ofd(f->vfs_refs); }
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
         * as soon as any one inheritor closed its copy.
         * Slice 89: only if the slot still holds the SAME socket. A stale or
         * double-closed descriptor would otherwise decrement a stranger's
         * refcount and tear down a live channel. */
        if (f->sock && sock_generation(f->sock) != f->sock_gen) {
            kprintf("[uxgen] file_close on RECYCLED sock slot "
                    "(had gen=%u now gen=%u) -- refusing the close\n",
                    f->sock_gen, sock_generation(f->sock));
            break;
        }
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
    case FILE_KIND_TIMERFD:
        if (f->tfd && --f->tfd->refs == 0) kfree(f->tfd);
        break;
    case FILE_KIND_SIGNALFD:
        if (f->sfd && --f->sfd->refs == 0) kfree(f->sfd);
        break;
    case FILE_KIND_NSFD:
        ns_file_close(f);      /* drops the fd's reference on the namespace */
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

/* ---- shared offset (see struct vfs_ofd in file.h) ------------------------ */

static struct vfs_ofd *file_ofd(struct file *f) {
    /* Only VFS handles carry a description; refs is the first member, so the
     * pointer the rest of the kernel knows as `int *vfs_refs` IS the struct. */
    if (!f || f->kind != FILE_KIND_VFS || !f->vfs_refs) return 0;
    return (struct vfs_ofd *)(void *)f->vfs_refs;
}

/* In the kernel all three standard descriptors are the same console, so the
 * fd number does not select anything -- but the CALLER still needs a real
 * object to clone. See file_std_handle() in file.h. */
struct file *file_std_handle(int fd) {
    (void)fd;
    return console_file_make();
}

size_t file_pos_get(struct file *f) {
    struct vfs_ofd *o = file_ofd(f);
    return o ? o->pos : (f ? f->vfs.pos : 0);
}

void file_pos_set(struct file *f, size_t pos) {
    if (!f) return;
    struct vfs_ofd *o = file_ofd(f);
    if (o) o->pos = pos;
    f->vfs.pos = pos;          /* keep the embedded copy consistent */
}

/* The driver advances the cursor it is handed, so load the shared offset
 * before the call and store the advanced value back after it. */
static void file_pos_load(struct file *f) {
    struct vfs_ofd *o = file_ofd(f);
    if (o) f->vfs.pos = o->pos;
}
static void file_pos_store(struct file *f) {
    struct vfs_ofd *o = file_ofd(f);
    if (o) o->pos = f->vfs.pos;
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
    case FILE_KIND_TIMERFD:
        return timerfd_read(f, buf, n);
    case FILE_KIND_SIGNALFD:
        return signalfd_read(f, buf, n);
    case FILE_KIND_MEMFD: {
        long r = memfd_read(f->memfd, f->vfs.pos, buf, n);
        if (r > 0) f->vfs.pos += (size_t)r;
        return r;
    }
    case FILE_KIND_DEVNULL:
        return 0;                                  /* always EOF */
    case FILE_KIND_DEVZERO:
    case FILE_KIND_DEVFULL:                        /* slice 6: reads as zeroes */
        memset(buf, 0, n);
        return (long)n;                            /* endless zeroes */
    case FILE_KIND_DEVRANDOM:
        rng_fill(buf, n);
        return (long)n;                            /* never short, never blocks */
    case FILE_KIND_PIPE_R:
        return pipe_read(f->pipe, buf, n);
    case FILE_KIND_VFS: {
        if (!f->vfs.ops || !f->vfs.ops->read) return -1;
        file_pos_load(f);
        long r = f->vfs.ops->read(&f->vfs, buf, n);
        file_pos_store(f);
        return r;
    }
    case FILE_KIND_SOCKET:
        /* shutdown(SHUT_RD): reads are EOF from now on -- checked before the
         * readiness probe because a shut socket is "ready" (ready to say EOF). */
        if (f->sock && f->sock->shut_rd)
            return 0;
        /* Non-blocking socket with nothing to read: EAGAIN, never park. Chrome
         * reads its TCP sockets with plain read() (not recv), so the check has
         * to live here as well as in the recv path -- and an optimistic read
         * before the data arrives is the COMMON case in an epoll-driven client,
         * not an edge case. */
        if (f->sock && f->sock->nonblock && !sock_recv_ready(f->sock))
            return SOCK_ERR_AGAIN;                     /* == -EAGAIN */
        /* A CONNECTED TCP socket is a byte stream: read() == recv(). A
         * connect()-ed UDP socket reads the next datagram. */
        if (f->sock && f->sock->kind == SOCK_KIND_TCP && f->sock->tcp &&
            !f->sock->tcp_listening) {
            long r = tcp_recv(f->sock->tcp, buf, n,
                              f->sock->recv_timeout_ms);
            return (r == -1) ? 0 : r;    /* peer FIN -> EOF */
        }
        if (f->sock && f->sock->kind == SOCK_KIND_NETLINK) {
            long r = sock_netlink_recv(f->sock, buf, n);
            return r;                    /* SOCK_ERR_AGAIN == -EAGAIN */
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
    case FILE_KIND_DEVFULL:
        return -ABI_ENOSPC;                        /* slice 6: always full */
    case FILE_KIND_DEVRANDOM:
        rng_mix(buf, n);                           /* writes seed the pool */
        return (long)n;
    case FILE_KIND_PIPE_W:
        return pipe_write(f->pipe, buf, n);
    case FILE_KIND_VFS: {
        if (!f->vfs.ops || !f->vfs.ops->write) return -1;
        file_pos_load(f);
        long w = f->vfs.ops->write(&f->vfs, buf, n);
        file_pos_store(f);
        return w;
    }
    case FILE_KIND_SOCKET:
        /* shutdown(SHUT_WR): every later send is EPIPE (sys_write raises
         * the SIGPIPE half, mirroring its pipe rule). */
        if (f->sock && f->sock->shut_tx)
            return -ABI_EPIPE;
        /* Connected TCP byte stream: write() == send(). A connect()-ed UDP
         * socket sends a datagram to its peer. */
        if (f->sock && f->sock->kind == SOCK_KIND_TCP && f->sock->tcp &&
            !f->sock->tcp_listening) {
            /* Non-blocking: queue what the window allows and return the short
             * count instead of waiting for every byte to be acknowledged. */
            if (f->sock->nonblock) {
                long w = tcp_send_nb(f->sock->tcp, buf, n);
                if (w < 0) return -1;
                return (w == 0) ? SOCK_ERR_AGAIN : w;
            }
            long w = tcp_send(f->sock->tcp, buf, n);
            return (w < 0) ? -1 : w;
        }
        if (f->sock && f->sock->kind == SOCK_KIND_NETLINK)
            return sock_netlink_send(f->sock, buf, n);
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
