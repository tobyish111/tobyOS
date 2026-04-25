/* signal.c -- minimal signal subsystem (milestone 8).
 *
 * See signal.h for the high-level model. Implementation notes:
 *
 *   - The pending-bits live on `struct proc::pending_signals`; this
 *     file just sets them and arranges for the proc to actually look
 *     at them.
 *
 *   - signal_send() is callable from IRQ context (the keyboard IRQ
 *     uses it for Ctrl+C). It performs three actions atomically with
 *     respect to the running CPU because:
 *       * we have one CPU running the scheduler / userspace;
 *       * IRQs run with IF=0 by virtue of the IDT gate; and
 *       * cooperative kernel paths run with IF=0 (SYSCALL clears it
 *         via FMASK, kernel internal paths don't sti).
 *     So set_bit + (maybe) wake-from-block + sched_enqueue is a single
 *     uninterruptible critical section on this CPU.
 *
 *   - Wait-queue unlink: when a proc joined a pipe wq, pipe.c stored
 *     the head pointer in p->wait_head. We follow it to splice p out
 *     in O(N), where N is the number of waiters on that one queue
 *     (typically 1 in this kernel).
 *
 *   - signal_deliver_if_pending() picks the lowest-numbered pending
 *     signal and proc_exits with code (128+sig) -- the same convention
 *     POSIX shells use to report signal-killed children. The extra
 *     bookkeeping (clear foreground if the dying proc IS the
 *     foreground) keeps the keyboard from sending SIGINT into a
 *     reaped/recycled PCB slot on the next Ctrl+C.
 */

#include <tobyos/signal.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/printk.h>

/* Foreground proc's PID. 0 means "shell prompt is the foreground" --
 * we never deliver signals to pid 0 itself. */
static int g_foreground_pid = 0;

void signal_init(void) {
    g_foreground_pid = 0;
    kprintf("[signal] subsystem up (SIGINT=%d, SIGTERM=%d)\n",
            SIGINT, SIGTERM);
}

int signal_get_foreground(void) {
    return g_foreground_pid;
}

void signal_set_foreground(int pid) {
    g_foreground_pid = pid;
}

/* Walk the wait-queue rooted at *head and unlink p, if it's there.
 * Both p->next_wait and p->wait_head are cleared on success. */
static void wait_queue_unlink(struct proc *p) {
    if (!p || !p->wait_head) return;
    struct proc **slot = p->wait_head;
    while (*slot && *slot != p) slot = &(*slot)->next_wait;
    if (*slot == p) *slot = p->next_wait;
    p->next_wait = 0;
    p->wait_head = 0;
}

void signal_send(struct proc *p, int sig) {
    if (!p) return;
    if (sig <= 0 || sig >= SIG_MAX) return;
    if (p->pid == 0) return;                  /* never signal the kernel */
    if (p->state == PROC_UNUSED || p->state == PROC_TERMINATED) return;

    p->pending_signals |= SIGMASK(sig);

    /* Unblock if asleep. The blocking primitive will notice the
     * pending bit on its first iteration after sched_yield returns
     * and bail with -EINTR. */
    if (p->state == PROC_BLOCKED) {
        wait_queue_unlink(p);
        /* If the proc was BLOCKED on a child (proc_wait), it may also
         * have wait_pid set -- harmless to leave; the wait loop will
         * re-check signal_pending_self before going back to sleep. */
        p->state = PROC_READY;
        sched_enqueue(p);
    }
}

void signal_send_to_pid(int pid, int sig) {
    signal_send(proc_lookup(pid), sig);
}

void signal_send_to_foreground(int sig) {
    if (g_foreground_pid > 0) {
        signal_send_to_pid(g_foreground_pid, sig);
    }
}

bool signal_pending_self(void) {
    struct proc *p = current_proc();
    return p && p->pending_signals != 0;
}

void signal_deliver_if_pending(void) {
    struct proc *p = current_proc();
    if (!p || p->pending_signals == 0) return;
    if (p->pid == 0) {
        /* Defensive: kernel pid 0 should never accumulate signals,
         * but if it somehow did, just drop them rather than killing
         * the kernel. */
        p->pending_signals = 0;
        return;
    }

    /* Pick the lowest-numbered pending signal. With our two-signal
     * universe this is always SIGINT before SIGTERM if both were sent. */
    int sig = 0;
    for (int i = 1; i < SIG_MAX; i++) {
        if (p->pending_signals & SIGMASK(i)) { sig = i; break; }
    }
    if (sig == 0) return;       /* shouldn't happen; mask was non-zero */

    /* Clear foreground if the dying proc IS the foreground, so the
     * next Ctrl+C doesn't target a soon-to-be-reaped PCB slot. */
    if (g_foreground_pid == p->pid) g_foreground_pid = 0;

    kprintf("[signal] pid=%d '%s' killed by signal %d\n",
            p->pid, p->name, sig);

    /* Default action: terminate with conventional shell exit code. */
    proc_exit(128 + sig);
    /* unreachable */
}
