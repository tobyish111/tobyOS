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
 * GS_BASE per-CPU IS installed now (syscall_init on the BSP, ap_entry on
 * each AP both wrmsr GS base -> this CPU's struct percpu). After boot,
 * smp_this_cpu() resolves "which CPU am I?" with a single gs-relative
 * load of the `self` pointer (see below). Before GS bases are installed
 * (early boot), it falls back to smp_current_cpu_idx(), which reads the
 * LAPIC ID and matches it against g_percpu[].apic_id.
 */

#ifndef TOBYOS_PERCPU_H
#define TOBYOS_PERCPU_H

#include <tobyos/types.h>
#include <tobyos/spinlock.h>

#define MAX_CPUS 32u

struct proc;

struct percpu {
    /* ---- asm-visible per-CPU fields (MUST stay first, offsets 0 and 8) ----
     * GS base on each CPU points at that CPU's struct percpu, so the SYSCALL
     * trampoline (syscall_entry.S) reads gs:[0] / gs:[8] for the kernel stack
     * and the user-RSP scratch. Two CPUs can be in the SYSCALL prologue at
     * once, so these MUST be per-CPU (a single global would race). Keep these
     * two fields at offsets 0/8 -- syscall_entry.S hard-codes them. */
    uint64_t syscall_rsp;        /* offset 0:  kernel stack top for SYSCALL  */
    uint64_t user_rsp_scratch;   /* offset 8:  SYSCALL prologue scratch slot */

    uint32_t cpu_idx;       /* 0..n-1, our internal numbering */
    uint32_t apic_id;       /* what the LAPIC reports for this CPU */
    bool     is_bsp;        /* true for cpu_idx 0 (always the BSP) */
    /* AP-bringup handshake (two stages so the BSP's SIPI retry is safe):
     *   started -- set by the AP as its VERY FIRST action in ap_entry, before
     *              it touches any lock or shared state. Tells the BSP "this AP
     *              received a SIPI and is executing", so the BSP must NOT
     *              re-INIT it (a re-INIT would reset it mid-init and could leak
     *              the console lock). Only an AP that never set `started` is
     *              eligible for an INIT-SIPI-SIPI retry.
     *   online  -- set after the AP finishes per-CPU init and is ready to
     *              schedule. The BSP waits on this (without retrying) once it
     *              has seen `started`. */
    bool     started;       /* AP began executing ap_entry (pre-init) */
    bool     online;        /* set by the CPU itself when init done  */
    bool     holds_bkl;     /* this CPU currently holds the big kernel lock */
    uint64_t stack_top;     /* per-CPU kernel stack top (NULL for BSP) */

    /* ---- Milestone 22 step 5: per-CPU scheduler state -------- */
    /* What's currently running on this CPU. NULL for an AP that
     * hasn't picked up any work yet (it's just sitting in sched_idle
     * on `sti; hlt`). The BSP starts with current = pid 0 once
     * proc_init runs. */
    struct proc    *current;

    /* This CPU's idle process (ap_idle on APs; NULL on the BSP, which idles
     * as pid 0 driving gui_tick). When a proc on an AP blocks/exits with no
     * other work, sched_yield switches back to this idle proc, which steals
     * runnable work off other CPUs. */
    struct proc    *idle;

    /* Slice 39: the proc this CPU just switched AWAY from. The task that
     * resumes on this CPU (do_switch's post-switch path, or a brand-new
     * proc's first-entry trampoline) clears prev_proc->on_cpu -- the point
     * at which the outgoing proc's context save is guaranteed complete and
     * it becomes safe for other CPUs to pick it off a ready queue. */
    struct proc    *prev_proc;

    /* This CPU's runnable queue. enqueue/dequeue must hold ready_lock.
     * For v1 the round-robin distributor pushes everything to BSP
     * (cpu 0) because user procs need a per-CPU TSS/syscall stack
     * we haven't built yet. APs have empty queues and just idle. */
    struct proc    *ready_head;
    struct proc    *ready_tail;
    spinlock_t      ready_lock;

    /* Count of LAPIC timer interrupts taken on this CPU. Mostly a
     * "is the timer alive" diagnostic for the `cpus` shell command.
     * Also the "total samples" denominator for per-core CPU usage. */
    uint64_t        timer_ticks;

    /* Statistical per-core usage: subset of timer_ticks where this CPU was
     * running real work (not its idle task) when the tick fired. sysmon
     * diffs (busy, total=timer_ticks) over its sample window to get the
     * per-core utilisation % shown by the task manager. */
    uint64_t        sched_busy_ticks;

    /* Self-pointer (== &g_percpu[cpu_idx]). Each CPU's GS base points at
     * its own struct percpu, so once GS is installed on every core,
     * smp_this_cpu() can read `gs:[offsetof(self)]` -- a single ordinary
     * load, no VM exit -- instead of doing an LAPIC MMIO read on every
     * call. The MMIO read (apic_read_id) is ~1000x slower than a memory
     * load under TCG, and smp_this_cpu()/current_proc() are called in the
     * hottest spin paths (BKL, sched_yield), so on headless QEMU the MMIO
     * cost collapsed throughput into a multi-core busy-spin livelock
     * (both cores pinned in apic_read_id, gui_tick never completing). */
    struct percpu  *self;

    /* Slice 94: 1 while this CPU is parked on sti;hlt with nothing to run
     * (sched_idle, or the BSP's in-place halt). sched_enqueue kicks ONE
     * such CPU with SCHED_WAKE_VECTOR so a fresh wake is picked up
     * immediately instead of waiting out the next LAPIC tick ([wlat]
     * measured that wait at avg 2-3 ms under chrome). Appended LAST:
     * offsets 0/8 are asm-visible and must not move. */
    volatile uint8_t idle_halted;
};

#endif /* TOBYOS_PERCPU_H */
