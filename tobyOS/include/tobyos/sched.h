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
struct regs;

/* ---- Priority classes ------------------------------------------------------
 * `prio` on struct proc is HIGHER-runs-first, with 0 == NORMAL so a freshly
 * zero-initialised PCB defaults to NORMAL with no extra code. These class
 * constants are the values userland sets via SYS_SETPRIORITY; the kernel
 * clamps to [PRIO_MIN, PRIO_MAX]. (Mirrored to userland in <tobyos/abi/abi.h>
 * as ABI_PRIO_*.) */
#define PRIO_IDLE    (-2)   /* only runs when nothing else is runnable */
#define PRIO_LOW     (-1)   /* background work */
#define PRIO_NORMAL    0    /* default */
#define PRIO_HIGH      1    /* foreground / interactive */
#define PRIO_RT        2    /* latency-critical (still preemptible) */
#define PRIO_MIN     PRIO_IDLE
#define PRIO_MAX     PRIO_RT

/* ---- Timeslice + anti-starvation tunables ----------------------------------
 * Quantum is counted in LAPIC-timer ticks (the per-CPU 100 Hz periodic timer,
 * 10 ms/tick). Higher-priority classes get a longer slice, but every class is
 * preemptible so nothing monopolises a core. Aging is counted in PIT ticks
 * (the global 1000 Hz wall clock, pit_ticks(), 1 ms/tick): a READY proc's
 * effective priority climbs one level for every SCHED_AGE_STEP_TICKS it has
 * waited without running, capped at SCHED_AGE_MAX_BOOST -- enough for a starved
 * IDLE proc to eventually out-rank a running RT proc, which bounds worst-case
 * wait. (Validated: 3 HIGH vs 3 LOW CPU-bound workers on 4 cores -> HIGH ~18x
 * the CPU of LOW, yet every LOW proc still made progress: no starvation.) */
#define SCHED_QUANTUM_BASE     5    /* LAPIC ticks for NORMAL (~50 ms @100Hz)   */
#define SCHED_AGE_STEP_TICKS  60    /* +1 effective level per ~60 ms waiting     */
#define SCHED_AGE_MAX_BOOST    5    /* PRIO_MAX-PRIO_MIN+1: can lift IDLE>RT     */
#define SCHED_IO_BOOST         2    /* interactivity bonus: one priority class   */

/* Class quantum: foreground classes get a slightly longer slice. */
static inline int sched_quantum_for(int prio) {
    int q = SCHED_QUANTUM_BASE + prio;   /* RT=7, HIGH=6, NORMAL=5, LOW=4, IDLE=3 */
    return q < 1 ? 1 : q;
}

/* Initialise scheduler state. Must be called once after proc_init. */
void sched_init(void);

/* Append `p` to a CPU's ready queue (round-robin v1 -> BSP). The proc
 * must be PROC_READY (or about to be made READY by the caller); the
 * scheduler does not change p->state itself. Idempotent: if p is
 * already on a queue, this is a no-op. */
void sched_enqueue(struct proc *p);
/* Remove `p` from any ready queue. MUST be called before freeing a proc's
 * kernel stack or recycling its slot -- a proc left linked in a ready queue is
 * still reachable by the scheduler and will be switched into with freed state. */
void sched_dequeue(struct proc *p);
/* Diagnostic: walk every CPU's ready list looking for `p`. Returns 1 if it is
 * still linked (or the list is cyclic -- *cycle_out), 0 if not. Walks rather
 * than trusting on_rq, so it can audit the flag. */
int  sched_debug_find_queued(struct proc *p, int *cpu_out, bool *cycle_out);

/* Move a READY process to the front of the BSP ready queue. Used for
 * latency-sensitive GUI input so the focused app consumes key events
 * before unrelated desktop apps get more round-robin turns. */
void sched_boost_pid(int pid);

/* Pick the next runnable proc on the CURRENT CPU and context-switch
 * to it. If the current proc is PROC_RUNNING and this CPU's ready
 * queue is empty, this returns immediately (current keeps the CPU).
 * If the current proc is BLOCKED/TERMINATED and nothing's ready, it
 * spins doing `sti; hlt` until an IRQ wakes someone up. */
void sched_yield(void);

/* Big Kernel Lock: serializes kernel-mode execution across CPUs so user code
 * runs in parallel while not-yet-fine-grained-locked subsystems (VFS, GUI,
 * proc table) stay safe. bkl_enter at syscall entry / around pid 0's per-tick
 * shared-state work; bkl_exit at syscall exit / before idling. The scheduler
 * drops/reacquires it around blocking. bkl_exit is idempotent. */
void bkl_enter(void);
void bkl_exit(void);

/* True if the calling CPU currently holds the BKL. Lets a long cooperative
 * wait (e.g. a blocking network syscall) release the BKL across its idle
 * period and reacquire it afterwards, so it never starves the other cores. */
bool bkl_held(void);

/* Allow secondary CPUs to start stealing + running user procs. Called once by
 * the BSP after its boot sequence, right before entering the GUI idle loop --
 * before this, APs idle so they can't race kernel_main's unlocked boot work. */
void sched_enable_ap_run(void);

/* AP idle entry. Never returns. Sits on `sti; hlt` and wakes on
 * timer/IRQ; if anything ever lands on this CPU's ready queue, it
 * promotes to RUNNING and context-switches. APs call this from
 * ap_entry() once their LAPIC + LAPIC timer are alive. */
__attribute__((noreturn)) void sched_idle(void);

/* Called from the LAPIC timer ISR (which runs on every CPU at 100 Hz once
 * apic_timer_periodic_init has fired on that CPU). Increments this CPU's tick
 * counter and, when the timer interrupted ring-3 user code whose timeslice is
 * spent, forces a preemptive yield (fair per-AP timeslicing). `r` is the
 * interrupt trap frame so we can test the interrupted privilege level -- we
 * only ever preempt from ring 3, where the proc holds no kernel lock/BKL, so
 * the yield is as safe as the proven PIT preemption path. The caller MUST have
 * already EOI'd the LAPIC before invoking this (we may not return promptly).
 * Cheap on the no-preempt path -- safe to call from interrupt context. */
void sched_tick(struct regs *r);

/* Sentinel returned by the prio accessors when `pid` has no live PCB. Well
 * outside [PRIO_MIN, PRIO_MAX] so it can never collide with a real priority. */
#define SCHED_PRIO_NONE  (-1000000)

/* Set proc `pid`'s scheduling priority (clamped to [PRIO_MIN, PRIO_MAX]).
 * Returns the applied value, or SCHED_PRIO_NONE if pid not found. */
int sched_set_prio(int pid, int prio);

/* Read proc `pid`'s scheduling priority, or SCHED_PRIO_NONE if pid not found. */
int sched_get_prio(int pid);

#endif /* TOBYOS_SCHED_H */
