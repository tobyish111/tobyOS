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
}

/* ---- pipe lifecycle --------------------------------------------- */

static void pipe_maybe_free(struct pipe *p) {
    if (p->readers == 0 && p->writers == 0) {
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

long pipe_read(struct pipe *p, void *buf, size_t n) {
    if (!p || !buf) return -1;
    if (n == 0) return 0;
    uint8_t *out = (uint8_t *)buf;

    /* Block while there's nothing to read AND a writer is alive. */
    while (p->count == 0 && p->writers > 0) {
        struct proc *self = current_proc();
        /* Signal arrived before we even parked? Bail immediately so
         * the syscall return path can deliver it. */
        if (self->pending_signals) return EINTR_RET;
        wq_add(&p->wq_read, self);
        self->state = PROC_BLOCKED;
        sched_yield();
        /* Resumed. Two reasons to be back here:
         *   - wq_wake_all woke us (data arrived / writer closed) -- loop
         *     re-checks the predicate, normal case.
         *   - signal_send() spliced us off the queue and made us
         *     READY because a SIGINT/SIGTERM hit us -- bail with EINTR. */
        if (self->pending_signals) return EINTR_RET;
    }

    if (p->count == 0) {
        /* writers == 0 here -> EOF. */
        return 0;
    }

    size_t got = 0;
    while (got < n && p->count > 0) {
        out[got++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SZ;
        p->count--;
    }

    /* We freed buffer space -- wake any blocked writer. */
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
            if (self->pending_signals) {
                return put > 0 ? (long)put : EINTR_RET;
            }
            wq_add(&p->wq_write, self);
            self->state = PROC_BLOCKED;
            sched_yield();
            if (self->pending_signals) {
                return put > 0 ? (long)put : EINTR_RET;
            }
        }
        if (p->readers == 0) {
            return put > 0 ? (long)put : -3;
        }
        while (put < n && p->count < PIPE_BUF_SZ) {
            p->buf[p->head] = in[put++];
            p->head = (p->head + 1) % PIPE_BUF_SZ;
            p->count++;
        }
        /* We added bytes -- wake any blocked reader. */
        if (p->wq_read) wq_wake_all(&p->wq_read);
    }
    return (long)put;
}
