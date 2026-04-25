/* sched.h -- cooperative round-robin scheduler with per-CPU queues.
 *
 * Scheduler state is sharded across g_percpu[] (see <tobyos/percpu.h>):
 * each CPU owns a FIFO of struct proc * + a `current` pointer + a
 * spinlock. sched_yield() consults THIS CPU's queue, picks its head
 * (or stays on the current proc if nothing else is runnable), and
 * context-switches.
 *
 * Round-robin enqueue: sched_enqueue() picks a target CPU via a
 * simple modulo counter. For Milestone 22 v1 the policy currently
 * collapses every enqueue onto the BSP's queue because user procs
 * need a per-CPU TSS/syscall stack we haven't built yet. The
 * round-robin counter is wired in but disabled at the policy layer
 * -- see sched.c for the gate. APs sit in sched_idle() doing
 * `sti; hlt` and wake on their per-CPU LAPIC timer (100 Hz) or any
 * other interrupt routed at this CPU.
 *
 * Yield points used in milestone 5+:
 *   - proc_exit()  : after marking ourselves TERMINATED
 *   - proc_wait()  : after blocking on a child PID
 *   - apic timer ISR (milestone 22 step 5) : best-effort preempt
 *     when the current proc has been running for too long.
 */

#ifndef TOBYOS_SCHED_H
#define TOBYOS_SCHED_H

#include <tobyos/types.h>

struct proc;

/* Initialise scheduler state. Must be called once after proc_init. */
void sched_init(void);

/* Append `p` to a CPU's ready queue (round-robin v1 -> BSP). The proc
 * must be PROC_READY (or about to be made READY by the caller); the
 * scheduler does not change p->state itself. Idempotent: if p is
 * already on a queue, this is a no-op. */
void sched_enqueue(struct proc *p);

/* Pick the next runnable proc on the CURRENT CPU and context-switch
 * to it. If the current proc is PROC_RUNNING and this CPU's ready
 * queue is empty, this returns immediately (current keeps the CPU).
 * If the current proc is BLOCKED/TERMINATED and nothing's ready, it
 * spins doing `sti; hlt` until an IRQ wakes someone up. */
void sched_yield(void);

/* AP idle entry. Never returns. Sits on `sti; hlt` and wakes on
 * timer/IRQ; if anything ever lands on this CPU's ready queue, it
 * promotes to RUNNING and context-switches. APs call this from
 * ap_entry() once their LAPIC + LAPIC timer are alive. */
__attribute__((noreturn)) void sched_idle(void);

/* Called from the LAPIC timer ISR (which runs on every CPU at 100 Hz
 * once apic_timer_periodic_init has fired on that CPU). Increments
 * this CPU's tick counter; on the BSP also drives any cooperative
 * preempt heuristic. Cheap -- safe to call from interrupt context. */
void sched_tick(void);

#endif /* TOBYOS_SCHED_H */
