/* sched.c -- cooperative round-robin scheduler with per-CPU queues.
 *
 * Milestone 22 step 5 sharded the single g_ready_queue into one queue
 * per CPU. Each CPU's queue lives in its g_percpu[] slot (head/tail
 * pointers + a spinlock). sched_enqueue() picks a target via a tiny
 * round-robin counter; sched_yield() consults THIS CPU's queue.
 *
 * v1 round-robin policy: APs cannot safely run user procs yet
 * (per-CPU TSS / per-CPU syscall stack hasn't been built), so the
 * dispatcher currently *always* lands new procs on the BSP queue.
 * The infrastructure (per-CPU queues, current pointer, spinlocks,
 * AP idle loop, LAPIC timer ticks) is wired so a future v2 only has
 * to flip the `enq_target_for(p)` policy from "always 0" to "next
 * round-robin slot".
 *
 * sched_yield() flow on the BSP:
 *   1. If current is still PROC_RUNNING, demote to READY and enqueue
 *      onto THIS CPU's queue. (If current is BLOCKED/TERMINATED the
 *      caller already changed state -- we don't enqueue.)
 *   2. Pop the head of this CPU's queue. If empty AND current can't
 *      keep running, spin sti+hlt until something becomes runnable.
 *   3. If next == current, we're done -- nothing to switch.
 *   4. Otherwise: update TSS.RSP0 + g_kernel_syscall_rsp +
 *      g_current_proc + this CPU's g_percpu.current, then
 *      proc_context_switch.
 *
 * sched_idle() is the AP-side counterpart: an infinite loop that
 *   - cli + checks if THIS CPU's queue has anything (it won't in v1)
 *   - sti + hlt -- block until the next interrupt
 *   - on wake, drop back to the top of the loop
 * APs never enter ring 3 in v1, so there's no syscall stack /
 * TSS RSP0 to maintain.
 */

#include <tobyos/sched.h>
#include <tobyos/proc.h>
#include <tobyos/tss.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>
#include <tobyos/gui.h>
#include <tobyos/perf.h>
#include <tobyos/percpu.h>
#include <tobyos/smp.h>
#include <tobyos/spinlock.h>
#include <tobyos/watchdog.h>

/* From syscall_entry.S: kernel SP loaded by the SYSCALL trampoline. */
extern uint64_t g_kernel_syscall_rsp;

/* From proc_switch.S. */
extern void proc_context_switch(uint64_t *save_old_rsp,
                                uint64_t  new_rsp,
                                uint64_t  new_cr3);

void sched_init(void) {
    /* Per-CPU queues live in g_percpu[]; smp.c memset-zeroed them
     * during build_percpu_table, which happens to be the right
     * initial state (head/tail = NULL, spinlock = unlocked). Wire
     * cpu 0's `current` to whatever proc_init established as pid 0
     * so the BSP has a sane starting `current` before sched_yield
     * is ever called. */
    struct percpu *bsp = smp_cpu_mut(0);
    if (bsp) {
        bsp->current = current_proc();
    }
    kprintf("[sched] per-CPU round-robin scheduler ready (BSP cur=pid %d)\n",
            current_proc() ? current_proc()->pid : -1);
}

/* ---------- queue helpers (callers must hold per-CPU ready_lock) ---------- */

static void queue_push_locked(struct percpu *cpu, struct proc *p) {
    p->next_ready = 0;
    if (cpu->ready_tail) {
        cpu->ready_tail->next_ready = p;
        cpu->ready_tail = p;
    } else {
        cpu->ready_head = cpu->ready_tail = p;
    }
}

static struct proc *queue_pop_locked(struct percpu *cpu) {
    struct proc *p = cpu->ready_head;
    if (!p) return 0;
    cpu->ready_head = p->next_ready;
    if (!cpu->ready_head) cpu->ready_tail = 0;
    p->next_ready = 0;
    return p;
}

/* Return the cpu_idx that should receive a freshly-enqueued proc.
 * v1 policy: always BSP, because APs lack the TSS/syscall plumbing
 * to safely run user procs. Once that's built (a future milestone),
 * flip this to a real round-robin counter. */
static uint32_t enq_target_for(struct proc *p) {
    (void)p;
    return 0;   /* BSP */
}

void sched_enqueue(struct proc *p) {
    if (!p || p->next_ready) return;
    uint32_t target = enq_target_for(p);
    struct percpu *cpu = smp_cpu_mut(target);
    if (!cpu) cpu = smp_cpu_mut(0);             /* should never happen */
    uint64_t flags = spin_lock_irqsave(&cpu->ready_lock);
    queue_push_locked(cpu, p);
    spin_unlock_irqrestore(&cpu->ready_lock, flags);
}

void sched_yield(void) {
    struct proc   *cur = current_proc();
    struct percpu *me  = smp_this_cpu();

    /* M28C: scheduler heartbeat for the watchdog. Single store -- safe
     * even on the fast path below. */
    wdog_kick_sched();

    /* ---- Milestone 19 fast path ----------------------------------
     *
     * If we're still RUNNING and our queue is empty, nobody is
     * going to displace us. Skipping the enqueue/dequeue dance is a
     * ~25-cycle win per yield AND keeps p->last_switch_tsc stable so
     * we don't charge ourselves spurious 0-length context switches.
     * Correctness argument: this is indistinguishable from the old
     * flow's return-early-when-nothing-else-ready case, minus the
     * pointer shuffle. We sample ready_head without the lock --
     * worst case we miss a concurrent enqueue and yield on the next
     * tick, which is fine. */
    if (cur && cur->state == PROC_RUNNING &&
        __atomic_load_n(&me->ready_head, __ATOMIC_ACQUIRE) == 0) {
        return;
    }

    /* If we're still RUNNING, demote to READY and rejoin OUR queue
     * so we get a turn again later. (Always our own CPU's queue, NOT
     * the round-robin target -- we want to stay where the syscall
     * stack/TSS RSP0 are valid for this proc.) */
    if (cur && cur->state == PROC_RUNNING) {
        cur->state = PROC_READY;
        uint64_t f = spin_lock_irqsave(&me->ready_lock);
        if (!cur->next_ready) queue_push_locked(me, cur);
        spin_unlock_irqrestore(&me->ready_lock, f);
    }

    struct proc *next;
    {
        uint64_t f = spin_lock_irqsave(&me->ready_lock);
        next = queue_pop_locked(me);
        spin_unlock_irqrestore(&me->ready_lock, f);
    }

    /* No one is ready. If the current proc can resume, just do that
     * (caller's intent was just "let someone else run if possible"). */
    if (!next) {
        if (cur && cur->state == PROC_READY) {
            cur->state = PROC_RUNNING;
            return;
        }
        /* Caller is BLOCKED or TERMINATED and nothing else is ready --
         * spin sti+hlt until an IRQ unblocks somebody (or until the
         * caller's wait condition is satisfied by an exit). */
        for (;;) {
            sti();
            hlt();
            uint64_t f = spin_lock_irqsave(&me->ready_lock);
            next = queue_pop_locked(me);
            spin_unlock_irqrestore(&me->ready_lock, f);
            if (next) break;
            if (cur && cur->state == PROC_READY) {
                cur->state = PROC_RUNNING;
                return;
            }
        }
    }

    /* If we just popped ourselves back off, no actual switch needed. */
    if (next == cur) {
        next->state = PROC_RUNNING;
        return;
    }

    /* ---- Milestone 19 per-proc CPU accounting --------------------
     *
     * We book-keep on the two sides of the boundary:
     *   - cur: accumulate (now - last_switch_tsc) into cur->cpu_ns
     *   - next: stamp last_switch_tsc = now
     * This way cpu_ns stays correct across arbitrary interleavings,
     * and pid 0's cpu_ns ends up being "idle time + kernel work",
     * which we treat as the system-idle bucket in `top`.
     *
     * Also account the zone + global context_switch counter so
     * `perf` shows how often we ping-pong. */
    uint64_t t_switch = perf_rdtsc();
    if (cur)  perf_proc_account_out(cur,  t_switch);
    perf_proc_account_in (next, t_switch);
    perf_count_ctx_switch();
    perf_zone_end(PERF_Z_SCHED_SWITCH, t_switch);

    /* Hand the kernel-side stacks over to the new process so any
     * subsequent IRQ-from-user / syscall lands on its kstack. We
     * also stamp this CPU's percpu->current so anyone resolving
     * "what's running on cpu X?" via smp_cpu(X)->current sees the
     * post-switch picture. */
    next->state            = PROC_RUNNING;
    g_current_proc         = next;
    me->current            = next;
    tss_set_rsp0((uint64_t)next->kstack_top);
    g_kernel_syscall_rsp   = (uint64_t)next->kstack_top;

    if (log_enabled(LOG_CAT_SCHED)) {
        klog(LOG_CAT_SCHED, "yield: %d -> %d (cr3=0x%lx)",
             cur ? cur->pid : -1, next->pid, (unsigned long)next->cr3);
    } else if (gui_trace_level() >= GUI_TRACE_VERBOSE) {
        gui_trace_logf("sched_yield: %d -> %d (old_rsp=%p new_rsp=%p new_cr3=0x%lx)",
                       cur ? cur->pid : -1, next->pid,
                       (void *)cur->saved_rsp, (void *)next->saved_rsp,
                       (unsigned long)next->cr3);
    }
    proc_context_switch(&cur->saved_rsp, next->saved_rsp, next->cr3);
    if (gui_trace_level() >= GUI_TRACE_VERBOSE) {
        struct proc *now = current_proc();
        gui_trace_logf("sched_yield: resumed as %d", now ? now->pid : -1);
    }
    /* When this returns, we're back as `cur` again -- some future
     * sched_yield switched control to us. Re-stamp our switch-in
     * time so the NEXT switch-out delta is correct. */
    {
        struct proc *resumed = current_proc();
        if (resumed) perf_proc_account_in(resumed, perf_rdtsc());
    }
}

/* ---------- AP idle loop ---------- */

void sched_idle(void) {
    /* APs in v1 never run user procs (no per-CPU TSS / syscall stack),
     * so we cannot context-switch INTO anything. We just sit on hlt
     * and let interrupts route at this CPU drive any work. The
     * per-CPU LAPIC timer fires at 100 Hz and bumps me->timer_ticks
     * via sched_tick(); MSI/MSI-X handlers routed to this CPU's
     * APIC ID would also wake us. Either way, the loop just goes
     * back to hlt afterwards.
     *
     * Should we ever flip enq_target_for() to spread procs across
     * CPUs, this loop will need to grow a real "pop from my queue,
     * switch to it" path -- but that requires per-CPU TSS first. */
    for (;;) {
        sti();
        hlt();
    }
}

/* ---------- LAPIC timer tick ---------- */

void sched_tick(void) {
    struct percpu *me = smp_this_cpu();
    if (me) me->timer_ticks++;
    /* M28C: count scheduler-tick events as heartbeats too. */
    wdog_kick_sched();
    /* Preemption hook: in v1 the BSP still preempts cooperatively
     * via sched_yield calls scattered through syscalls + proc_exit
     * + proc_wait, so this ISR doesn't force a switch. A future v2
     * can call sched_yield() from here on the BSP if the current
     * proc has consumed its quantum -- but that requires careful
     * audit of every kprintf path that could be reached from inside
     * a timer ISR (since sched_yield touches the heap via proc
     * accounting, and the heap isn't yet locked for SMP). For now
     * we just count ticks; user-visible preemption is unchanged. */
}
