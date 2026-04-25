/* percpu.h -- per-CPU bookkeeping.
 *
 * One slot per logical CPU we know about (from MADT). Filled in by
 * smp.c, indexed by a small "cpu_idx" we hand out at enumeration time.
 *
 * Milestone 22 step 5 promoted this struct to also hold the per-CPU
 * scheduler state: the ready queue (head/tail/lock) and the
 * `current` pointer that names whichever proc is presently running
 * on this CPU. A LAPIC timer fires on every CPU at 100 Hz, calls
 * apic timer ISR, and that ISR consults g_percpu[smp_current_cpu_idx()]
 * to decide whether to preempt. APs that have no work to do sit on
 * `sti; hlt` inside sched_idle() and wake up on the next timer tick
 * (or when somebody enqueues something onto their queue).
 *
 * GS_BASE per-CPU is still NOT installed; APs receive their cpu_idx
 * as the first argument to ap_entry() and the rest of the kernel
 * resolves "which CPU am I?" via smp_current_cpu_idx() (which reads
 * the LAPIC ID once and matches it against g_percpu[].apic_id).
 */

#ifndef TOBYOS_PERCPU_H
#define TOBYOS_PERCPU_H

#include <tobyos/types.h>
#include <tobyos/spinlock.h>

#define MAX_CPUS 32u

struct proc;

struct percpu {
    uint32_t cpu_idx;       /* 0..n-1, our internal numbering */
    uint32_t apic_id;       /* what the LAPIC reports for this CPU */
    bool     is_bsp;        /* true for cpu_idx 0 (always the BSP) */
    bool     online;        /* set by the CPU itself when init done  */
    uint64_t stack_top;     /* per-CPU kernel stack top (NULL for BSP) */

    /* ---- Milestone 22 step 5: per-CPU scheduler state -------- */
    /* What's currently running on this CPU. NULL for an AP that
     * hasn't picked up any work yet (it's just sitting in sched_idle
     * on `sti; hlt`). The BSP starts with current = pid 0 once
     * proc_init runs. */
    struct proc    *current;

    /* This CPU's runnable queue. enqueue/dequeue must hold ready_lock.
     * For v1 the round-robin distributor pushes everything to BSP
     * (cpu 0) because user procs need a per-CPU TSS/syscall stack
     * we haven't built yet. APs have empty queues and just idle. */
    struct proc    *ready_head;
    struct proc    *ready_tail;
    spinlock_t      ready_lock;

    /* Count of LAPIC timer interrupts taken on this CPU. Mostly a
     * "is the timer alive" diagnostic for the `cpus` shell command. */
    uint64_t        timer_ticks;
};

#endif /* TOBYOS_PERCPU_H */
