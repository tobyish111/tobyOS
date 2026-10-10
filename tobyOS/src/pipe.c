/* pipe.c -- circular-buffer pipe with blocking read/write.
 *
 * The single-CPU cooperative scheduler removes the need for any locking:
 *   - Every kernel-mode code path runs with preemption disabled (we
 *     never preempt; SYSCALL enters with IF=0 via FMASK).
 *   - The only way a different proc gets to run is sched_yield(),
 *     which is exactly where we drop into BLOCKED state.
 *
 * Wait-queue protocol: a proc joining a queue sets its `next_wait`
 * field to the previous head and updates `head = self`, marks itself
 * BLOCKED, then sched_yield()s. The wakeup path walks the entire list
 * once, clears `next_wait`, sets each proc to READY, and enqueues them
 * on the scheduler. Spurious wakeups are tolerated -- callers loop on
 * the predicate (count > 0 / writers > 0 / etc) themselves.
 */

#include <tobyos/pipe.h>
#include <tobyos/file.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/signal.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/vfs.h>
#include <tobyos/perf.h>
#include <tobyos/abi/abi.h>

/* ---- wait-queue helpers ----------------------------------------- */

/* Park `p` on the queue rooted at *head. The back-pointer in
 * p->wait_head is what lets signal_send() splice us out of the queue
 * if it forcibly wakes us before the natural wq_wake_all() runs. */
static void wq_add(struct proc **head, struct proc *p) {
    p->next_wait = *head;
    p->wait_head = head;
    *head = p;
}

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

/* Take `p` off whatever queue it is still on (a deadline woke it, not
 * wq_wake_all) -- signal.c's wait_queue_unlink, for this file's queues. */
static void wq_unlink(struct proc *p) {
    if (!p || !p->wait_head) return;
    struct proc **slot = p->wait_head;
    while (*slot && *slot != p) slot = &(*slot)->next_wait;
    if (*slot == p) *slot = p->next_wait;
    p->next_wait = 0;
    p->wait_head = 0;
}

/* PARK THE CALLER ON *HEAD UNTIL A WAKE -- and pid 0 the way it waits for a
 * foreground command (proc.c pid0_serviced_wait). The kernel shell runs in
 * pid 0, and its own FIFO redirections wait here: for the other end
 * (fifo_open), for data (pipe_read), for room (pipe_write). A plain block
 * stopped pid 0's duties -- the watchdog, the network, SSH, the poll
 * waiters -- for as long as the other end took (finding 67's shape), which
 * is why the kernel shell refused a FIFO at all. So pid 0, once the idle
 * loop runs, does its duties first and then sleeps at most 10 ms on the
 * queue, leaving it again if the deadline and not a wake ended the sleep;
 * the caller's loop looks again either way. Everyone else blocks as
 * before. */
extern bool g_idle_loop_running;
extern void kernel_idle_service_nested(void);
#define PIPE_PID0_SLICE_NS 10000000ull

static void pipe_park(struct proc **head, struct proc *self) {
    if (self->pid == 0 && g_idle_loop_running) {
        kernel_idle_service_nested();
        wq_add(head, self);
        self->sleep_deadline_ns = perf_now_ns() + PIPE_PID0_SLICE_NS;
        self->state = PROC_BLOCKED;
        sched_yield();
        self->sleep_deadline_ns = 0;
        wq_unlink(self);
        return;
    }
    wq_add(head, self);
    self->state = PROC_BLOCKED;
    sched_yield();
}

/* ---- pipe lifecycle --------------------------------------------- */

static void fifo_pipe_gone(struct pipe *p);

static void pipe_maybe_free(struct pipe *p) {
    if (p->readers == 0 && p->writers == 0) {
        if (p->fifo) fifo_pipe_gone(p);    /* its FIFO forgets it, and the data */
        kfree(p);
    }
}

int pipe_create(struct file **out_r, struct file **out_w) {
    if (!out_r || !out_w) return -1;

    struct pipe *p = (struct pipe *)kmalloc(sizeof(*p));
    if (!p) return -1;
    memset(p, 0, sizeof(*p));
    p->readers = 1;
    p->writers = 1;

    struct file *fr = (struct file *)kmalloc(sizeof(*fr));
    struct file *fw = (struct file *)kmalloc(sizeof(*fw));
    if (!fr || !fw) {
        kfree(fr); kfree(fw); kfree(p);
        return -1;
    }
    memset(fr, 0, sizeof(*fr));
    memset(fw, 0, sizeof(*fw));
    fr->kind = FILE_KIND_PIPE_R;
    fr->pipe = p;
    fw->kind = FILE_KIND_PIPE_W;
    fw->pipe = p;

    *out_r = fr;
    *out_w = fw;
    return 0;
}

void pipe_close_reader(struct pipe *p) {
    if (!p) return;
    p->readers--;
    if (p->readers == 0) {
        /* Wake any blocked writer so they observe readers == 0 and
         * return -EPIPE on their next iteration. */
        wq_wake_all(&p->wq_write);
    }
    pipe_maybe_free(p);
}

void pipe_close_writer(struct pipe *p) {
    if (!p) return;
    p->writers--;
    if (p->writers == 0) {
        /* Wake any blocked reader so they observe writers == 0 and
         * return 0 (EOF) on their next iteration. */
        wq_wake_all(&p->wq_read);
    }
    pipe_maybe_free(p);
}

/* ---- read / write ----------------------------------------------- */

/* SLICE 129: copy the ring in at most TWO memcpy runs instead of a byte at a
 * time. The old loops did `idx = (idx + 1) % PIPE_BUF_SZ` PER BYTE -- a real
 * div, since the compiler cannot fold a modulo by a non-constant-folded
 * expression into a mask here -- plus a one-byte load/store. That was merely
 * wasteful at 4 KiB; at the 64 KiB ring this pipe now carries (see pipe.h) it
 * would be the dominant cost of moving a frame, so the conversion is part of
 * the same change, not a separate cleanup.
 *
 * Both helpers assume the caller has already established there is work to do
 * and space/data for it; they clamp to `count`/room regardless. PIPE_BUF_SZ is
 * a power of two so the wrap is a mask. */
static size_t pipe_drain(struct pipe *p, uint8_t *out, size_t n) {
    size_t got = 0;
    while (got < n && p->count > 0) {
        size_t run = PIPE_BUF_SZ - p->tail;        /* to the end of the ring */
        if (run > p->count) run = p->count;        /* to the end of the data */
        if (run > n - got)  run = n - got;         /* to the end of the caller */
        memcpy(out + got, p->buf + p->tail, run);
        p->tail = (uint32_t)((p->tail + run) & PIPE_BUF_MASK);
        p->count -= (uint32_t)run;
        got += run;
    }
    return got;                                    /* loops at most twice */
}

static size_t pipe_fill(struct pipe *p, const uint8_t *in, size_t n) {
    size_t put = 0;
    while (put < n && p->count < PIPE_BUF_SZ) {
        size_t run  = PIPE_BUF_SZ - p->head;       /* to the end of the ring */
        size_t room = PIPE_BUF_SZ - p->count;      /* to the end of the space */
        if (run > room)    run = room;
        if (run > n - put) run = n - put;          /* to the end of the caller */
        memcpy(p->buf + p->head, in + put, run);
        p->head = (uint32_t)((p->head + run) & PIPE_BUF_MASK);
        p->count += (uint32_t)run;
        put += run;
    }
    return put;                                    /* loops at most twice */
}

long pipe_read(struct pipe *p, void *buf, size_t n) {
    if (!p || !buf) return -1;
    if (n == 0) return 0;
    uint8_t *out = (uint8_t *)buf;

    /* Block while there's nothing to read AND a writer is alive. */
    while (p->count == 0 && p->writers > 0) {
        struct proc *self = current_proc();
        /* Signal arrived before we even parked? Bail immediately so
         * the syscall return path can deliver it. */
        if (SIG_SHOULD_EINTR(self)) return EINTR_RET;
        pipe_park(&p->wq_read, self);
        /* Resumed. Two reasons to be back here:
         *   - wq_wake_all woke us (data arrived / writer closed) -- loop
         *     re-checks the predicate, normal case.
         *   - signal_send() spliced us off the queue and made us
         *     READY because a SIGINT/SIGTERM hit us -- bail with EINTR. */
        if (SIG_SHOULD_EINTR(self)) return EINTR_RET;
    }

    if (p->count == 0) {
        /* writers == 0 here -> EOF. */
        return 0;
    }

    size_t got = pipe_drain(p, out, n);

    /* We freed buffer space -- wake any blocked writer. */
    if (p->wq_write) wq_wake_all(&p->wq_write);

    return (long)got;
}

/* Non-blocking read (slice 45): never parks. Returns bytes copied (>0), 0 at
 * EOF (no data AND no writers left), or -11 (-ABI_EAGAIN) when the pipe is
 * momentarily empty but a writer is still alive. Lets a native app drain a pipe
 * from an event loop without a blocking read stalling the loop (chromewin's CDP
 * screencast pump). */
long pipe_tryread(struct pipe *p, void *buf, size_t n) {
    if (!p || !buf) return -1;
    if (n == 0) return 0;
    if (p->count == 0)
        return (p->writers > 0) ? -11 /* -ABI_EAGAIN */ : 0 /* EOF */;

    size_t got = pipe_drain(p, (uint8_t *)buf, n);
    if (p->wq_write) wq_wake_all(&p->wq_write);
    return (long)got;
}

long pipe_write(struct pipe *p, const void *buf, size_t n) {
    if (!p || !buf) return -1;
    if (n == 0) return 0;
    const uint8_t *in = (const uint8_t *)buf;

    size_t put = 0;
    while (put < n) {
        /* If no readers left, give up. POSIX-ish: short-write count
         * if some bytes already made it; -3 (EPIPE) if none did. */
        if (p->readers == 0) {
            return put > 0 ? (long)put : -3;
        }
        /* Block while the buffer is full. */
        while (p->count == PIPE_BUF_SZ && p->readers > 0) {
            struct proc *self = current_proc();
            if (SIG_SHOULD_EINTR(self)) {
                return put > 0 ? (long)put : EINTR_RET;
            }
            pipe_park(&p->wq_write, self);
            if (SIG_SHOULD_EINTR(self)) {
                return put > 0 ? (long)put : EINTR_RET;
            }
        }
        if (p->readers == 0) {
            return put > 0 ? (long)put : -3;
        }
        put += pipe_fill(p, in + put, n - put);
        /* We added bytes -- wake any blocked reader. */
        if (p->wq_read) wq_wake_all(&p->wq_read);
    }
    return (long)put;
}

long pipe_trywrite(struct pipe *p, const void *buf, size_t n) {
    if (!p || !buf) return -1;
    if (n == 0) return 0;
    if (p->readers == 0) return -3;
    if (p->count == PIPE_BUF_SZ) return -ABI_EAGAIN;
    size_t put = pipe_fill(p, (const uint8_t *)buf, n);
    if (p->wq_read) wq_wake_all(&p->wq_read);
    return (long)put;
}

/* ---- named FIFOs (mkfifo) --------------------------------------------
 *
 * A FIFO is a file whose mode says S_IFIFO; opening one attaches to a
 * kernel pipe kept here, one per FIFO, made at the first open and freed --
 * its unread data with it -- when the last end closes (pipe_maybe_free),
 * as Linux keeps one per inode. The opens are what the FIFO's open(2)
 * rules count: a reader waits for a writer, a writer for a reader, so the
 * counters let a waiter see an end that came and went while it slept. */
#define FIFO_MAX 64
struct fifo {
    bool         used;
    uint32_t     dev;
    uint64_t     ino;
    char         path[VFS_PATH_MAX];    /* when the filesystem has no inode */
    struct pipe *p;
    uint32_t     r_opens, w_opens;      /* opens of each end, ever */
    int          waiting;               /* openers parked here */
    struct proc *wq;
};
static struct fifo g_fifos[FIFO_MAX];

static void fifo_pipe_gone(struct pipe *p) {
    struct fifo *ff = (struct fifo *)p->fifo;
    if (!ff) return;
    ff->p = 0;
    if (ff->waiting == 0) ff->used = false;
}

static struct fifo *fifo_find(const char *kpath, uint32_t dev, uint64_t ino) {
    struct fifo *free_slot = 0;
    for (int i = 0; i < FIFO_MAX; i++) {
        struct fifo *ff = &g_fifos[i];
        if (!ff->used) { if (!free_slot) free_slot = ff; continue; }
        if (ino && dev) {
            if (ff->ino == ino && ff->dev == dev) return ff;
        } else if (!ff->ino && strcmp(ff->path, kpath) == 0) {
            return ff;
        }
    }
    if (!free_slot) return 0;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = true;
    free_slot->dev = (ino && dev) ? dev : 0;
    free_slot->ino = (ino && dev) ? ino : 0;
    size_t kl = strlen(kpath);
    if (kl >= sizeof free_slot->path) kl = sizeof free_slot->path - 1;
    memcpy(free_slot->path, kpath, kl);
    free_slot->path[kl] = 0;
    return free_slot;
}

bool pipe_fifo_identity(struct pipe *p, uint32_t *dev, uint64_t *ino,
                        const char **path) {
    struct fifo *ff = p ? (struct fifo *)p->fifo : 0;
    if (!ff) return false;
    *dev = ff->dev;
    *ino = ff->ino;
    *path = ff->path;
    return true;
}

/* Park on the FIFO until the other end has opened (its counter moves) or
 * is open; false when a signal that acts ended the wait.
 *
 * THE OTHER END'S COMING IS LOOKED AT BEFORE THE SIGNAL, after every wake,
 * as Linux's wait_for_partner does. A writer that opened, wrote, closed
 * and EXITED while the reader slept leaves the reader's SIGCHLD pending
 * too; asked first, the signal ended the open with EINTR, SA_RESTART
 * opened again, and the new open waited for a writer that had already
 * been -- bash's `( ... > fifo ) & while read ...; done < fifo` hung for
 * good in the shparity gate (fixture 297 f9). */
static bool fifo_wait_other(struct fifo *ff, bool want_writer) {
    uint32_t start = want_writer ? ff->w_opens : ff->r_opens;
    struct proc *self = current_proc();
    ff->waiting++;
    for (;;) {
        struct pipe *p = ff->p;
        int other = !p ? 0 : (want_writer ? p->writers : p->readers);
        uint32_t now = want_writer ? ff->w_opens : ff->r_opens;
        if (other > 0 || now != start) break;
        if (SIG_SHOULD_EINTR(self)) { ff->waiting--; return false; }
        pipe_park(&ff->wq, self);
    }
    ff->waiting--;
    return true;
}

long fifo_open(const char *kpath, uint32_t dev, uint64_t ino, int accmode,
               bool nonblock, struct file **out) {
    struct fifo *ff = fifo_find(kpath, dev, ino);
    if (!ff) return -ABI_ENFILE;
    bool rd = (accmode == ABI_O_RDONLY || accmode == ABI_O_RDWR);
    bool wr = (accmode == ABI_O_WRONLY || accmode == ABI_O_RDWR);
    /* a writer that will not wait finds no reader: ENXIO, and nothing made */
    if (accmode == ABI_O_WRONLY && nonblock && (!ff->p || ff->p->readers == 0)) {
        if (!ff->p && ff->waiting == 0) ff->used = false;
        return -ABI_ENXIO;
    }
    if (!ff->p) {
        struct pipe *p = (struct pipe *)kmalloc(sizeof(*p));
        if (!p) { if (ff->waiting == 0) ff->used = false; return -ABI_ENOMEM; }
        memset(p, 0, sizeof(*p));
        p->fifo = ff;
        ff->p = p;
    }
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) {
        if (ff->p->readers == 0 && ff->p->writers == 0) {
            struct pipe *dead = ff->p;
            fifo_pipe_gone(dead);
            kfree(dead);
        }
        return -ABI_ENOMEM;
    }
    memset(f, 0, sizeof(*f));
    struct pipe *p = ff->p;
    f->kind = (rd && wr) ? FILE_KIND_PIPE_RW : rd ? FILE_KIND_PIPE_R : FILE_KIND_PIPE_W;
    f->pipe = p;
    f->pipe_nonblock = nonblock;
    f->o_accmode = accmode;
    if (rd) { p->readers++; ff->r_opens++; }
    if (wr) { p->writers++; ff->w_opens++; }
    if (ff->wq) wq_wake_all(&ff->wq);           /* the other end has come */
    if (!nonblock && accmode != ABI_O_RDWR) {
        if (!fifo_wait_other(ff, /*want_writer=*/rd)) {
            file_close(f);                       /* undoes the counts */
            return EINTR_RET;
        }
    }
    *out = f;
    return 0;
}
