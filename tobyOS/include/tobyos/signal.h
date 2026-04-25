/* signal.h -- minimal signal subsystem (milestone 8).
 *
 * The signal model in tobyOS is intentionally tiny:
 *
 *   - Each process carries a `pending_signals` bitmap (uint32_t) in its
 *     PCB. Bit `1 << sig` means "signal `sig` has been delivered but
 *     not yet acted upon".
 *
 *   - There are NO user-installed handlers, NO masks, NO queues.
 *     Every supported signal has the same default action: terminate
 *     the receiving process with exit code (128 + sig).
 *
 *   - Signals are checked + delivered at three well-defined "safe
 *     points" inside the kernel (never in the middle of an arbitrary
 *     C function):
 *       1. on the way back out of every syscall (syscall.c)
 *       2. inside the PIT timer IRQ if it interrupted ring 3 (pit.c)
 *       3. on resume from a blocking primitive that returns -EINTR
 *          (pipe_read / pipe_write / console_read), whose -EINTR
 *          eventually flows up through (1) and kills the proc.
 *
 *   - signal_send() against a PROC_BLOCKED proc unparks it: the proc
 *     is removed from any wait queue it's on (using the new
 *     proc->wait_head back-pointer), marked READY and re-enqueued on
 *     the scheduler. The blocking primitive's loop then notices the
 *     pending bit and returns -EINTR.
 *
 * The "foreground process" is a single global pid (g_foreground_pid):
 * the shell sets it before proc_wait()ing on a child, clears it
 * afterwards. Background children are simply NOT set as foreground,
 * so they never receive Ctrl+C. The keyboard IRQ uses
 * signal_send_to_foreground(SIGINT) on Ctrl+C and the resulting
 * SIGINT is silently dropped if no user proc is currently in the
 * foreground (e.g. the shell prompt is idle).
 */

#ifndef TOBYOS_SIGNAL_H
#define TOBYOS_SIGNAL_H

#include <tobyos/types.h>

/* Signal numbers. We follow POSIX values for the two we actually
 * support so any future ports of standard user code are unsurprising. */
#define SIGINT   2
#define SIGTERM  15

#define SIG_MAX  32             /* width of the pending bitmap */

#define SIGMASK(s) ((uint32_t)1u << (s))

/* errno-ish return value used by blocking primitives when they detect
 * a pending signal during wait. The actual numeric value doesn't have
 * to match Linux; user programs that care can pattern-match negative
 * results. We pick -4 because pipe.c already uses -3 for EPIPE. */
#define EINTR_RET ((long)-4)

struct proc;

/* Initialise signal state. Currently just zeroes the foreground pid;
 * called once at boot from kernel.c after proc_init(). */
void signal_init(void);

/* Foreground tracking. `pid == 0` means "no user-mode foreground
 * process" (i.e. the shell's own prompt is active). The shell is
 * pid 0 -- we never deliver signals to pid 0. */
int  signal_get_foreground(void);
void signal_set_foreground(int pid);

/* Set the `sig` bit on `p`'s pending mask. If `p` is BLOCKED, unlink
 * from its wait queue, mark READY, enqueue on the scheduler. Safe to
 * call from IRQ context. No-op for NULL / pid 0 / TERMINATED procs. */
void signal_send(struct proc *p, int sig);
void signal_send_to_pid(int pid, int sig);
void signal_send_to_foreground(int sig);

/* Called at the safe points described above. If the current proc has
 * any pending signal, calls proc_exit(128+sig) -- never returns in
 * that case. Otherwise returns immediately. */
void signal_deliver_if_pending(void);

/* Quick non-destructive query for the current proc. */
bool signal_pending_self(void);

#endif /* TOBYOS_SIGNAL_H */
