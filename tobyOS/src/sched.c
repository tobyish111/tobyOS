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
 *   - sti + hlt -- block until the next interrupt
 *   - on wake, drain THIS CPU's ready queue and migrate every proc
 *     back to the BSP's queue (APs lack per-CPU TSS so can't enter
 *     ring 3 yet)
 *   - drop back to the top of the loop
 * Once per-CPU TSS is wired up, APs will pop-and-switch directly.
 */

#include <tobyos/sched.h>
#include <tobyos/proc.h>
#include <tobyos/tss.h>
#include <tobyos/cpu.h>
#include <tobyos/printk.h>
#include <tobyos/serial.h>   /* serial_dropped -- log-loss accounting */
#include <tobyos/vmm.h>      /* vmm_pml4_is_live -- dead-CR3 guard */
#include <tobyos/panic.h>
#include <tobyos/gui.h>
#include <tobyos/perf.h>
#include <tobyos/percpu.h>
#include <tobyos/smp.h>
#include <tobyos/spinlock.h>
#include <tobyos/watchdog.h>
#include <tobyos/pit.h>
#include <tobyos/apic.h>     /* slice 94: apic_sched_wake (wake-kick IPI) */
#include <tobyos/isr.h>
#include <tobyos/signal.h>

/* The SYSCALL trampoline now loads its kernel stack from this CPU's
 * percpu->syscall_rsp (gs:[0]); the scheduler updates it on every switch. */

/* From proc_switch.S. */
extern void proc_context_switch(uint64_t *save_old_rsp,
                                uint64_t  new_rsp,
                                uint64_t  new_cr3);

/* ---------- Big Kernel Lock (BKL) ----------
 *
 * Serializes kernel-mode execution across CPUs so user code runs in parallel
 * on multiple cores while the kernel's coarse-locked subsystems (VFS, GUI
 * compositor, proc table, ...) stay race-free. Held across a syscall body
 * (syscall_dispatch) and around the pid-0 idle_loop's per-tick shared-state
 * work (gui_tick/services). RELEASED whenever a CPU gives up the core (the
 * scheduling slow path) and REACQUIRED when the holding proc resumes -- a
 * blocked proc must not hold it while halted or it wedges every other core.
 *
 * Held with IRQs ENABLED (plain spinlock) because syscall bodies must keep
 * taking device IRQs; IRQ handlers do NOT take the BKL (their shared state --
 * heap, PMM, per-CPU sched queues -- has its own fine-grained spinlocks). */
/* The BKL is a FAIR ticket lock, NOT the generic unfair test-and-set spinlock.
 * Fairness is essential here: under SMP, user procs on the APs take the BKL on
 * every syscall (and again for the syscall-return net_service_tick/gui_tick
 * work), so an unfair lock let the APs win re-acquisition indefinitely and
 * STARVE pid 0's idle-loop bkl_enter() -- the BSP never got a turn to run
 * gui_tick, which looked like a hard full-desktop freeze (random 1-30 s in,
 * heartbeat just stops). A ticket lock serves waiters FIFO, so the BSP always
 * gets its turn within one rotation. (The generic spinlock_t stays unfair --
 * fine for the short IRQ-off critical sections it guards.) */
typedef struct {
    volatile uint32_t next;      /* next ticket number to hand out */
    volatile uint32_t serving;   /* ticket currently allowed to proceed */
} bkl_ticket_t;
static bkl_ticket_t g_bkl = { 0, 0 };

/* Slice 64 (tier 2): BKL contention meter. rdtsc'd ONLY when the ticket
 * isn't immediately ours, so the uncontended path is unchanged (the
 * SMP_DIAG freeze-masking warning was about per-event ring LOGGING; plain
 * accumulating stores were verified freeze-safe). Read+reset by the 60s
 * deep dump. */
uint64_t g_bkl_wait_tsc[MAX_CPUS];
uint64_t g_bkl_waits[MAX_CPUS];
uint64_t g_bkl_acqs[MAX_CPUS];
#ifdef CHROMIUM_BOOT
/* Slice 94: [wlat] wake->run latency accumulators (see do_switch) +
 * wake-kick IPI count (see sched_enqueue). */
uint64_t g_wlat_sum, g_wlat_max, g_wlat_n;
uint64_t g_wkick;
#endif
static uint64_t g_bkl_t0[MAX_CPUS];    /* slice 64b: acquire stamp per CPU */
uint64_t g_bkl_held_tsc[MAX_CPUS];     /* total cycles HELD this interval */

static inline uint64_t bkl_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void bkl_lock(void) {
    uint32_t my = __atomic_fetch_add(&g_bkl.next, 1u, __ATOMIC_RELAXED);
    if (__atomic_load_n(&g_bkl.serving, __ATOMIC_ACQUIRE) == my) return;
    uint64_t t0 = bkl_rdtsc();
    while (__atomic_load_n(&g_bkl.serving, __ATOMIC_ACQUIRE) != my)
        __asm__ volatile ("pause" ::: "memory");
    struct percpu *me = smp_this_cpu();
    if (me && me->cpu_idx < MAX_CPUS) {
        g_bkl_wait_tsc[me->cpu_idx] += bkl_rdtsc() - t0;
        g_bkl_waits[me->cpu_idx]++;
    }
}
static inline void bkl_unlock(void) {
    __atomic_store_n(&g_bkl.serving, g_bkl.serving + 1u, __ATOMIC_RELEASE);
}

/* APs only start stealing + running user procs once the BSP has finished its
 * boot sequence and entered the GUI idle loop. Until then kernel_main touches
 * lots of shared state without the BKL, so APs must not run user code that
 * would race it. The BSP flips this on right before idle_loop(). */
static volatile bool g_ap_run_enabled = false;
void sched_enable_ap_run(void) { g_ap_run_enabled = true; }
/* Slice 92: the QFREEZETEST boot guard needs true SMP (its deadlock window
 * requires a sibling mid-syscall on another CPU during a CoW-fork sweep),
 * so it enables AP run early and restores the boot-harness invariant after.
 * Only gates NEW steals; the caller must ensure no user proc is still live
 * when disabling (the guard disables after proc_wait reaps everything). */
void sched_disable_ap_run(void) { g_ap_run_enabled = false; }

#ifdef SMP_DIAG
/* ---- BKL accounting instrumentation (-DSMP_DIAG) -------------------------
 * Used to diagnose the SMP desktop-freeze livelock (fixed in the per-phase
 * idle_loop commit); kept compiled-out for the day it resurfaces, e.g. on the
 * EliteDesk. Read live via QMP `x/` at a stall (addresses via nm).
 * g_bkl_reenters: bkl_enter() calls by a CPU that ALREADY held the BKL --
 *   non-zero means an accounting imbalance upstream (the idempotent guard in
 *   bkl_enter keeps it non-fatal either way).
 * g_bkl_owner   : cpu_idx that currently owns g_bkl (-1 == free).
 * g_bkl_enter_idmm / g_bkl_exit_idmm : times bkl_enter/bkl_exit saw an
 *   smp_this_cpu() whose cpu_idx disagrees with the recorded owner -- the
 *   signature of per-CPU mis-identification.
 * g_bkl_exit_notheld : bkl_exit() calls that no-op'd because holds_bkl was 0
 *   while g_bkl was actually held by someone (a leaked/lost release).
 *
 * WARNING: do NOT add per-bkl_enter/exit event LOGGING (ring buffers etc.) --
 * the extra hot-path stores perturb timing enough to MASK the freeze
 * (verified: an instrumented build ran clean while the same source froze
 * 3/3 without it). These counters are single stores and were freeze-safe. */
volatile uint64_t g_bkl_reenters = 0;
volatile int      g_bkl_owner       = -1;
volatile uint64_t g_bkl_enter_idmm  = 0;
volatile uint64_t g_bkl_exit_idmm   = 0;
volatile uint64_t g_bkl_exit_notheld = 0;
volatile int      g_bkl_mm_owner    = -1;   /* owner at the last mismatch */
volatile int      g_bkl_mm_caller   = -1;   /* mis-id'd cpu at last mismatch */
#endif

void bkl_enter(void) {
    struct percpu *me = smp_this_cpu();
    if (me->holds_bkl) {                               /* idempotent re-entry */
#ifdef SMP_DIAG
        g_bkl_reenters++;
#endif
        return;
    }
    bkl_lock();
    if (me->cpu_idx < MAX_CPUS) {
        g_bkl_acqs[me->cpu_idx]++;                           /* slice 64  */
        g_bkl_t0[me->cpu_idx] = bkl_rdtsc();                 /* slice 64b */
    }
    me->holds_bkl = true;
#ifdef SMP_DIAG
    g_bkl_owner = (int)me->cpu_idx;
#endif
}

void bkl_exit(void) {
    struct percpu *me = smp_this_cpu();
    if (!me->holds_bkl) {
#ifdef SMP_DIAG
        /* This CPU doesn't think it owns the BKL. If g_bkl is nonetheless held
         * by some cpu_idx, releasing was lost -> the lock leaks. Record it. */
        if (g_bkl_owner >= 0) {
            g_bkl_exit_notheld++;
            g_bkl_mm_owner  = g_bkl_owner;
            g_bkl_mm_caller = (int)me->cpu_idx;
        }
#endif
        return;                          /* idempotent: never double-unlock */
    }
#ifdef SMP_DIAG
    if (g_bkl_owner != (int)me->cpu_idx) {
        /* We "own" it per holds_bkl, but the recorded acquirer was a different
         * cpu_idx -> the same physical CPU was identified differently at
         * enter vs exit (mis-ID), or the BKL crossed CPUs. */
        g_bkl_exit_idmm++;
        g_bkl_mm_owner  = g_bkl_owner;
        g_bkl_mm_caller = (int)me->cpu_idx;
    }
    g_bkl_owner = -1;
#endif
    /* Slice 64b: attribute the cycles actually HELD to the syscall the
     * caller is inside. Blocking paths drop the lock via this same exit, so
     * every segment lands on the syscall that owned it -- unlike wall time
     * under the syscall, which counts parked-and-lockless waiting too (the
     * first attempt measured that and read futex=7.9M Mcyc, 17x a CPU's
     * whole cycle budget, which is how the error announced itself). */
    if (me->cpu_idx < MAX_CPUS) {
        uint64_t d = bkl_rdtsc() - g_bkl_t0[me->cpu_idx];
        g_bkl_held_tsc[me->cpu_idx] += d;
        extern void bkl_hold_account(uint64_t);
        bkl_hold_account(d);
    }
    me->holds_bkl = false;
    bkl_unlock();
}

bool bkl_held(void) {
    struct percpu *me = smp_this_cpu();
    return me && me->holds_bkl;
}

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

/* Effective priority = base class priority + an aging boost that grows with
 * how long the proc has waited READY-but-not-running since `enq_tick`. The
 * boost guarantees a starved low-priority proc eventually out-ranks a busy
 * high-priority one (anti-starvation), bounding worst-case wait. `now` is the
 * global PIT tick. */
static inline int eff_prio(const struct proc *p, uint64_t now) {
    uint64_t waited = (now > p->enq_tick) ? (now - p->enq_tick) : 0;
    int boost = (int)(waited / (uint64_t)SCHED_AGE_STEP_TICKS);
    if (boost > SCHED_AGE_MAX_BOOST) boost = SCHED_AGE_MAX_BOOST;
    /* Aging boost (anti-starvation) + interactivity boost (responsiveness).
     * io_boost is granted when a proc blocks before spending its quantum and
     * cleared when it burns a full one -- so an interactive task out-ranks a
     * CPU hog without permanently elevating anything. */
    return p->prio + boost + p->io_boost;
}

static void queue_push_locked(struct percpu *cpu, struct proc *p) {
    /* Defensive: never link a proc that is already queued. Doing so with
     * p == cpu->ready_tail produced p->next_ready = p, a self-cycle that hung
     * queue_pop_locked's walk (and therefore the entire system) forever. */
    if (p->on_rq) return;
    p->next_ready = 0;
    p->enq_tick   = pit_ticks();       /* start the aging clock for this wait */
#ifdef CHROMIUM_BOOT
    p->enq_ns     = perf_now_ns();     /* slice 94: [wlat] wake->run stamp */
#endif
    if (cpu->ready_tail) {
        cpu->ready_tail->next_ready = p;
        cpu->ready_tail = p;
    } else {
        cpu->ready_head = cpu->ready_tail = p;
    }
    p->on_rq = true;
}

static void queue_push_front_locked(struct percpu *cpu, struct proc *p) {
    if (!p) return;
    p->enq_tick   = pit_ticks();
#ifdef CHROMIUM_BOOT
    p->enq_ns     = perf_now_ns();     /* slice 94: [wlat] wake->run stamp */
#endif
    p->next_ready = cpu->ready_head;
    cpu->ready_head = p;
    if (!cpu->ready_tail) cpu->ready_tail = p;
}

/* Unlink + return the highest-effective-priority proc on cpu's queue, or NULL.
 * Ties are broken FIFO (the queue is walked head->tail and a strictly-greater
 * test keeps the first/oldest contender), so equal-priority procs round-robin
 * exactly as the old plain-FIFO pop did -- which is why a tree of all-NORMAL
 * procs behaves identically to before. */
static struct proc *queue_pop_locked(struct percpu *cpu) {
    uint64_t now = pit_ticks();
    struct proc *best = 0, *best_prev = 0, *prev = 0, *p = cpu->ready_head;
    int best_eff = 0;
    while (p) {
        /* Slice 39: skip procs whose context save hasn't completed (still
         * live on some CPU). They stay queued; the next pop after
         * sched_finish_switch clears on_cpu gets them with a VALID context.
         * ACQUIRE pairs with the RELEASE clear so saved_rsp is visible. */
        if (!__atomic_load_n(&p->on_cpu, __ATOMIC_ACQUIRE)) {
            int eff = eff_prio(p, now);
            if (!best || eff > best_eff) { best = p; best_prev = prev; best_eff = eff; }
        }
        prev = p;
        p = p->next_ready;
    }
    if (!best) return 0;
    if (best_prev) best_prev->next_ready = best->next_ready;
    else           cpu->ready_head       = best->next_ready;
    if (cpu->ready_tail == best) cpu->ready_tail = best_prev;
    best->next_ready = 0;
    best->on_rq      = false;
    return best;
}

static bool queue_remove_locked(struct percpu *cpu, struct proc *p) {
    if (!cpu || !p || !cpu->ready_head) return false;
    if (cpu->ready_head == p) {
        cpu->ready_head = p->next_ready;
        if (cpu->ready_tail == p) cpu->ready_tail = 0;
        p->next_ready = 0;
        p->on_rq      = false;
        return true;
    }

    struct proc *prev = cpu->ready_head;
    while (prev->next_ready && prev->next_ready != p) {
        prev = prev->next_ready;
    }
    if (prev->next_ready != p) return false;

    prev->next_ready = p->next_ready;
    if (cpu->ready_tail == p) cpu->ready_tail = prev;
    p->next_ready = 0;
    p->on_rq      = false;
    return true;
}

/* Unlink + return the highest-effective-priority NON-idle runnable proc from
 * cpu's queue, or NULL. Idle procs (pid 0, ap_idle) are pinned to their home
 * core and must never be migrated, so work-stealing skips them. Same FIFO-tie
 * + aging policy as queue_pop_locked. */
static struct proc *queue_steal_locked(struct percpu *cpu) {
    uint64_t now = pit_ticks();
    struct proc *best = 0, *best_prev = 0, *prev = 0, *p = cpu->ready_head;
    int best_eff = 0;
    while (p) {
        /* Skip idle procs (pinned) and on_cpu procs (context not yet saved
         * -- see queue_pop_locked / ledger slice 39). */
        if (!p->is_idle && !__atomic_load_n(&p->on_cpu, __ATOMIC_ACQUIRE)) {
            int eff = eff_prio(p, now);
            if (!best || eff > best_eff) { best = p; best_prev = prev; best_eff = eff; }
        }
        prev = p;
        p = p->next_ready;
    }
    if (!best) return 0;
    if (best_prev) best_prev->next_ready = best->next_ready;
    else           cpu->ready_head       = best->next_ready;
    if (cpu->ready_tail == best) cpu->ready_tail = best_prev;
    best->next_ready = 0;
    best->on_rq      = false;
    return best;
}

/* Count this CPU's stealable (non-idle) ready procs -- a load estimate for
 * balancing. Caller need not hold the lock; a racy count just picks a
 * slightly-stale victim, which self-corrects on the next idle pass. */
static int queue_depth(struct percpu *cpu) {
    int n = 0;
    for (struct proc *p = cpu->ready_head; p; p = p->next_ready)
        if (!p->is_idle) n++;
    return n;
}

/* Find one runnable non-idle proc to run on `me`: our own queue first, then
 * steal from the BUSIEST other CPU (load balancing -- migrate work off the
 * most-loaded core rather than the first non-empty one we happen to scan).
 * NULL if nothing is runnable anywhere. */
static struct proc *steal_one(struct percpu *me) {
    uint64_t f = spin_lock_irqsave(&me->ready_lock);
    struct proc *p = queue_steal_locked(me);
    spin_unlock_irqrestore(&me->ready_lock, f);
    if (p) return p;

    uint32_t n = smp_online_count();

    /* Pick the most-loaded remote CPU as the steal victim. */
    struct percpu *victim = 0;
    int best_depth = 0;
    for (uint32_t i = 1; i < n; i++) {
        struct percpu *o = smp_cpu_mut((me->cpu_idx + i) % n);
        if (!o || !o->ready_head) continue;
        int d = queue_depth(o);
        if (d > best_depth) { best_depth = d; victim = o; }
    }
    if (victim) {
        uint64_t f2 = spin_lock_irqsave(&victim->ready_lock);
        p = queue_steal_locked(victim);
        spin_unlock_irqrestore(&victim->ready_lock, f2);
        if (p) return p;
    }

    /* Fallback: the depths raced to zero (victim drained) -- sweep for any
     * remaining work so we never idle while a runnable proc exists. */
    for (uint32_t i = 1; i < n; i++) {
        struct percpu *o = smp_cpu_mut((me->cpu_idx + i) % n);
        if (!o || !o->ready_head) continue;
        uint64_t f2 = spin_lock_irqsave(&o->ready_lock);
        p = queue_steal_locked(o);
        spin_unlock_irqrestore(&o->ready_lock, f2);
        if (p) return p;
    }
    return 0;
}

/* Context-switch from `from` to `to` on CPU `me`: per-CPU current/TSS RSP0/
 * SYSCALL stack/TLS/FPU, then the register switch. `reacquire_bkl` re-takes
 * the BKL when `from` resumes (the caller released it before switching away).
 * Shared by sched_yield and the AP idle loop. */
static void do_switch(struct percpu *me, struct proc *from, struct proc *to,
                      bool reacquire_bkl) {
#ifdef CHROMIUM_BOOT
    /* Validate BEFORE latching on_cpu / current. Slice 88: a reaper may have
     * freed this PML4 after we popped the proc; skip to idle instead of panic. */
    if (to && to->pid != 0 && !vmm_pml4_is_live(to->cr3)) {
        kprintf("[sched] SKIP dead cr3 pid=%d cr3=0x%lx "
                "(is_thread=%d tgid=%d) from pid=%d\n",
                to->pid, (unsigned long)to->cr3, (int)to->is_thread,
                to->tgid, from ? from->pid : -1);
        to->state = PROC_UNUSED;
        to->kstack_top = 0;
        if (me->idle && from != me->idle)
            to = me->idle;
        else
            return;
    }
    if (to && to->pid != 0 &&
        (!to->kstack_top || !to->saved_rsp || to->state == PROC_UNUSED)) {
        kprintf("[sched] SKIP bad kstack pid=%d top=%p state=%d\n",
                to->pid, to->kstack_top, (int)to->state);
        if (me->idle && from != me->idle)
            to = me->idle;
        else
            return;
    }
#endif
    uint64_t t = perf_rdtsc();
    if (from) perf_proc_account_out(from, t);
    perf_proc_account_in(to, t);
    perf_count_ctx_switch();
    perf_zone_end(PERF_Z_SCHED_SWITCH, t);

#ifdef CHROMIUM_BOOT
    /* Slice 89 tripwire: switching into a proc that is STILL live on another
     * CPU means two CPUs will run the same kernel stack/register image --
     * the exact mechanism that produces "kernel pointers in user registers"
     * and wandering heap corruption. Every picker is supposed to skip
     * on_cpu procs; if this ever fires, one of them does not. */
    if (__atomic_load_n(&to->on_cpu, __ATOMIC_ACQUIRE)) {
        kprintf("[dsched] DUAL-SCHEDULE pid=%d state=%d on cpu%u (from=%d)\n",
                to->pid, (int)to->state, me->cpu_idx,
                from ? from->pid : -1);
    }
#endif
#ifdef CHROMIUM_BOOT
    /* Slice 94: [wlat] -- wall time between a proc being made READY and it
     * actually starting to run. This is THE number behind every IPC hop on
     * tobyOS (futex wake, pipe write, eventfd): if it is ~ms, scheduler
     * pickup latency dominates chrome's CDP/Mojo round-trips; if ~us, the
     * latency lives inside chrome and kernel work won't help. Idle procs
     * excluded (their enq stamp is not a wake). */
    if (!to->is_idle && to->enq_ns) {
        uint64_t wl = perf_now_ns() - to->enq_ns;
        to->enq_ns = 0;
        extern uint64_t g_wlat_sum, g_wlat_max, g_wlat_n;
        __atomic_fetch_add(&g_wlat_sum, wl, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_wlat_n, 1, __ATOMIC_RELAXED);
        if (wl > g_wlat_max) g_wlat_max = wl;   /* racy max: fine for a dump */
    }
#endif
    to->state       = PROC_RUNNING;
    to->quantum_left = sched_quantum_for(to->prio);  /* fresh timeslice */
    /* Slice 39: mark `to` live-on-CPU before it can possibly be re-enqueued,
     * and remember `from` so the code that runs AFTER the register switch
     * (post-switch line below, or a first-entry trampoline) can clear its
     * on_cpu once its context save is complete. See sched_finish_switch. */
    __atomic_store_n(&to->on_cpu, 1, __ATOMIC_RELAXED);
    me->prev_proc   = from;
    g_current_proc  = to;
    me->current     = to;
    tss_set_rsp0((uint64_t)to->kstack_top);
    me->syscall_rsp = (uint64_t)to->kstack_top;
    if (to->tls_base) wrmsr(0xC0000100, to->tls_base);   /* MSR_FS_BASE */
    /* SWAPGS shadow: a Win32 PE runs CPL3 with GS = its TEB (gs->gs_base);
     * everything else runs CPL3 with GS = &percpu (this CPU), which makes the
     * SWAPGS at every ring boundary an identity swap -- so native/Linux procs
     * are behaviourally unchanged. Restored into the active GS base by the
     * SWAPGS on the way out to CPL3 (syscall_entry.S / isr_stubs.S). */
    wrmsr(0xC0000102 /* IA32_KERNEL_GS_BASE */,
          to->gs_base ? to->gs_base : (uint64_t)me);

    fpu_save(from->fpu_state);
    proc_context_switch(&from->saved_rsp, to->saved_rsp, to->cr3);
    /* --- `from` resumes here when some later switch picks it again --- */
    /* Release the proc THIS cpu just switched away from (its saved_rsp is
     * now valid) BEFORE anything that can block (bkl_enter spins): holding
     * it on_cpu across a BKL wait would make it unschedulable meanwhile.
     * NOTE: `from` may resume on a DIFFERENT cpu than it left, so consult
     * smp_this_cpu() fresh rather than the stale `me` local. */
    sched_finish_switch();
    if (reacquire_bkl) bkl_enter();
    fpu_restore(from->fpu_state);

    struct proc *r = current_proc();
    if (r) perf_proc_account_in(r, perf_rdtsc());
}

/* Clear the on_cpu latch of the proc the CURRENT cpu last switched away
 * from. Called from every path that begins executing after a context
 * switch: do_switch's post-switch line, proc_first_user_entry, and
 * fork_child_entry. RELEASE pairs with the ACQUIRE test in the pickers so
 * the outgoing proc's saved_rsp store (program-order earlier on this CPU,
 * inside proc_context_switch) is visible before the proc becomes pickable. */
void sched_finish_switch(void) {
    struct percpu *cpu = smp_this_cpu();
    if (!cpu) return;
    struct proc *prev = cpu->prev_proc;
    if (prev) {
        cpu->prev_proc = 0;
        __atomic_store_n(&prev->on_cpu, 0, __ATOMIC_RELEASE);
        /* Slice 89: SEQ_CST fence between the on_cpu store and the
         * vm_quiesce load. This handoff races tg_vm_resume's mirror-image
         * {vm_quiesce:=0; load on_cpu}: with only release/acquire, x86
         * StoreLoad reordering lets BOTH sides read the stale value (we see
         * vm_quiesce still 1, resume sees on_cpu still 1) and NEITHER
         * requeues -- the thread is parked BLOCKED forever. The slice-88
         * "-smp 4 froze, threads READY in clone3 232s, APs idle" family
         * lives here. Both requeueing is fine (sched_enqueue no-ops on a
         * queued proc; the state==BLOCKED test closes the double-READY). */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        /* Deferred CoW-STW resume: tg_vm_resume left vm_quiesced set because
         * we were still on_cpu. Now safe to put a BLOCKED parked sibling
         * back on a run queue (vm_quiesce already cleared). */
        if (prev->vm_quiesced &&
            !__atomic_load_n(&prev->vm_quiesce, __ATOMIC_ACQUIRE) &&
            prev->state == PROC_BLOCKED) {
            prev->vm_quiesced = 0;
            prev->state = PROC_READY;
            sched_enqueue(prev);
        }
    }
}

/* Return the cpu_idx that should receive a freshly-enqueued proc.
 * v1 policy: always BSP (cpu 0). APs lack per-CPU TSS so they can't
 * context-switch into user processes. The AP idle loop migrates any
 * stray work back to the BSP, but enqueuing directly to BSP avoids
 * the migration latency entirely. Once per-CPU TSS is wired up,
 * switch to round-robin across all online CPUs. */
static uint32_t enq_target_for(struct proc *p) {
    (void)p;
    return 0;
}

/* Is `p` actually LINKED into some CPU's ready queue right now?
 *
 * Walks the lists rather than trusting a flag, so it can be used to audit the
 * flag itself. Bounded so a corrupt (cyclic) list cannot hang the auditor --
 * if we ever exceed PROC_MAX hops the list has a cycle, which is itself the
 * answer. Returns the owning CPU index in *cpu_out, or -1. */
int sched_debug_find_queued(struct proc *p, int *cpu_out, bool *cycle_out) {
    if (cpu_out) *cpu_out = -1;
    if (cycle_out) *cycle_out = false;
    if (!p) return 0;
    uint32_t n = smp_cpu_count();
    if (n == 0) n = 1;
    for (uint32_t c = 0; c < n; c++) {
        struct percpu *cpu = smp_cpu_mut(c);
        if (!cpu) continue;
        uint64_t flags = spin_lock_irqsave(&cpu->ready_lock);
        int hops = 0;
        bool found = false, cyc = false;
        for (struct proc *q = cpu->ready_head; q; q = q->next_ready) {
            if (q == p) found = true;
            if (++hops > PROC_MAX) { cyc = true; break; }
        }
        spin_unlock_irqrestore(&cpu->ready_lock, flags);
        if (cyc) { if (cycle_out) *cycle_out = true; if (cpu_out) *cpu_out = (int)c; return 1; }
        if (found) { if (cpu_out) *cpu_out = (int)c; return 1; }
    }
    return 0;
}

void sched_enqueue(struct proc *p) {
    if (!p) return;
    /* Slice 88: never re-arm a slot whose kstack was torn down, and never
     * put a VM-quiesced sibling back on a ready queue (tg_vm_resume owns
     * that). pid 0 is exempt: boot/idle legitimately has kstack_top == 0. */
    if (p->pid != 0 && (!p->kstack_top || p->state == PROC_UNUSED))
        return;
    if (__atomic_load_n(&p->vm_quiesce, __ATOMIC_ACQUIRE)) {
        p->state = PROC_BLOCKED;
        p->vm_quiesced = 1;
        /* Slice 89: re-check after a fence. tg_vm_resume iterates the proc
         * table ONCE; if it already passed this proc while we were parking
         * it here, nobody would ever requeue it (BLOCKED, off-queue, off-
         * cpu). If the flag cleared under us, undo the park and enqueue --
         * a concurrent requeue by tg_vm_resume is benign (queue_push
         * no-ops on an already-queued proc). */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&p->vm_quiesce, __ATOMIC_ACQUIRE))
            return;
        p->vm_quiesced = 0;
        p->state = PROC_READY;
    }
    uint32_t target = enq_target_for(p);
    struct percpu *cpu = smp_cpu_mut(target);
    if (!cpu) cpu = smp_cpu_mut(0);             /* should never happen */
    /* The "already queued?" test MUST be on on_rq and MUST be inside the lock.
     * It used to be `p->next_ready != NULL` evaluated BEFORE taking the lock:
     * wrong on both counts. The tail of the queue has next_ready == NULL, so
     * re-enqueuing the current tail sailed past the guard and self-cycled it
     * (ready_tail->next_ready = p with ready_tail == p), wedging the scheduler
     * for every process; and testing outside the lock races another CPU. */
    uint64_t flags = spin_lock_irqsave(&cpu->ready_lock);
    queue_push_locked(cpu, p);                  /* no-ops if already on_rq */
    spin_unlock_irqrestore(&cpu->ready_lock, flags);

    /* Slice 94: WAKE KICK. Work lands on the BSP queue; a halted AP would
     * otherwise sleep until its next LAPIC tick before stealing it --
     * [wlat] measured that pickup at avg 2-3 ms per wake, and every
     * futex/pipe hop of a chrome CDP round-trip paid it. Kick ONE halted
     * CPU so pickup is immediate. Claiming idle_halted before the IPI
     * bounds it to one kick per sleeper; the sti;hlt pair on the receiver
     * makes the race benign (a kick sent pre-hlt stays pending and breaks
     * the hlt as it lands). Scan cost with nobody halted: <= MAX_CPUS
     * relaxed loads. */
    if (g_ap_run_enabled) {
        uint32_t n = smp_cpu_count();
        for (uint32_t i = 0; i < n && i < MAX_CPUS; i++) {
            struct percpu *c = smp_cpu_mut(i);
            if (!c || !c->online) continue;
            if (__atomic_load_n(&c->idle_halted, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&c->idle_halted, 0, __ATOMIC_RELAXED);
                apic_sched_wake((uint8_t)c->apic_id);
#ifdef CHROMIUM_BOOT
                { extern uint64_t g_wkick;
                  __atomic_fetch_add(&g_wkick, 1, __ATOMIC_RELAXED); }
#endif
                break;
            }
        }
    }
}

/* Remove `p` from whatever ready queue it is on, if any. Safe to call on a proc
 * that is not queued. Scans every CPU because a proc can be enqueued on any of
 * them. Must be called before freeing a proc's kernel stack or marking its slot
 * UNUSED: a proc left linked in a ready queue after teardown gets popped and
 * switched into with a NULL kstack_top -> triple fault. */
void sched_dequeue(struct proc *p) {
    if (!p) return;
    uint32_t n = smp_cpu_count();
    if (n == 0) n = 1;
    for (uint32_t c = 0; c < n; c++) {
        struct percpu *cpu = smp_cpu_mut(c);
        if (!cpu) continue;
        uint64_t flags = spin_lock_irqsave(&cpu->ready_lock);
        bool removed = queue_remove_locked(cpu, p);
        spin_unlock_irqrestore(&cpu->ready_lock, flags);
        if (removed) return;      /* on at most one queue */
    }
}

void sched_boost_pid(int pid) {
    if (pid <= 0) return;
    struct proc *p = proc_lookup(pid);
    if (!p || p->state != PROC_READY) return;

    struct percpu *cpu = smp_cpu_mut(0);
    if (!cpu) return;

    uint64_t flags = spin_lock_irqsave(&cpu->ready_lock);
    (void)queue_remove_locked(cpu, p);
    queue_push_front_locked(cpu, p);
    spin_unlock_irqrestore(&cpu->ready_lock, flags);
}

int sched_set_prio(int pid, int prio) {
    struct proc *p = proc_lookup(pid);
    if (!p) return SCHED_PRIO_NONE;
    if (prio < PRIO_MIN) prio = PRIO_MIN;
    if (prio > PRIO_MAX) prio = PRIO_MAX;
    p->prio = prio;
    /* Re-base the quantum so a live raise/lower takes effect within a tick
     * rather than only at the next switch-in. Bounded to the new class slice. */
    if (p->quantum_left > sched_quantum_for(prio))
        p->quantum_left = sched_quantum_for(prio);
    return prio;
}

int sched_get_prio(int pid) {
    struct proc *p = proc_lookup(pid);
    return p ? p->prio : SCHED_PRIO_NONE;
}

/* ---- Slice 64 (perf tier 2): BKL-FREE sched_yield fast path ----------
 *
 * MEASURED (slice 64 [lx-top]): chrome issues ~401,000 sched_yield per 60s
 * -- 100x the next syscall -- and EVERY one took the BKL just to reach
 * sched_yield()'s fast path, whose entire body is `bkl_exit(); bkl_enter()`
 * (a full ticket-lock round trip: go to the back of the fair queue, spin,
 * re-acquire). That is the system-wide serialization point: cpu1/cpu2 each
 * showed ~400k acquisitions with ~20k contended waits per interval.
 *
 * When the caller is still RUNNING with an empty local ready queue, a yield
 * is semantically a NO-OP -- nothing can displace us -- so it needs no lock
 * at all. Served entirely before bkl_enter().
 *
 * Two things the old path got FOR FREE from being under the BKL, preserved:
 *   - the 10ms futex timeout sweep: driven here too (it serialises on
 *     g_futex_lock, never the BKL, so it is safe lock-free).
 *   - poll_tick(): needs the BKL, so instead of duplicating it we let the
 *     fast path DECLINE roughly every 2ms; that yield falls through to the
 *     normal BKL path and drives poll_tick exactly as before. 0.05% of
 *     yields keep the wake machinery healthy; the rest never touch the BKL.
 * Returns 1 if fully served (caller returns 0 to userspace). */
int sched_yield_fast(void) {
    struct proc   *cur = current_proc();
    struct percpu *me  = smp_this_cpu();
    if (!cur || !me) return 0;
    if (cur->state != PROC_RUNNING) return 0;
    if (__atomic_load_n(&me->ready_head, __ATOMIC_ACQUIRE) != 0) return 0;

    uint64_t nowns = perf_now_ns();
    static uint64_t last_slow;          /* let a yield through periodically */
    if (nowns - last_slow >= 2000000ull) { last_slow = nowns; return 0; }

    wdog_kick_sched();
    static uint64_t last_sweep;         /* the 10ms futex timeout sweep */
    if (nowns - last_sweep >= 10000000ull) {
        last_sweep = nowns;
        extern void futex_expire_timeouts(void);
        futex_expire_timeouts();
    }
    return 1;
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
        /* Slice 88: this early return used to skip the futex-timeout sweep
         * and poll_tick ENTIRELY -- they were only driven from the slow
         * path below and from sched_yield_fast's decline window. That held
         * as long as something kept the ready queue busy; the moment the
         * system settles into "everyone parked in futex WAIT, each waiter
         * looping sched_yield on an empty queue", every yield takes THIS
         * path and the wake machinery starves. Observed the moment the
         * slice-88 wake handoff resolved chrome's mutex ping-pong: [tick]
         * fxsweep froze at 2681 for 2+ minutes and six worker threads sat
         * 110s past their 60s futex deadlines. Drive the same rate-limited
         * sweeps here; both are cheap and correctly locked for this
         * context (sweep: g_futex_lock only; poll_tick: BKL, gated). */
        static uint64_t last_idle_sweep;
        uint64_t idlens = perf_now_ns();
        if (idlens - last_idle_sweep >= 10000000ull) {   /* 10 ms */
            last_idle_sweep = idlens;
            extern void futex_expire_timeouts(void);
            futex_expire_timeouts();
            /* poll_tick needs the BKL. Under load every yield can be
             * BKL-less (futex park loops drop it first) and pid 0 -- the
             * other driver -- never runs while chrome threads are READY,
             * so gating on holds_bkl starved poll_tick outright ([tick]
             * polltick frozen at 322 for 2 minutes while fxsweep ran).
             * Take the lock briefly at this 10ms cadence instead: the
             * yield path holds no other locks, so this is a safe spot. */
            extern void poll_tick(void);
            if (me->holds_bkl) { poll_tick(); }
            else { bkl_enter(); poll_tick(); bkl_exit(); }
        }
        /* Slice 39: do NOT keep the BKL across the fast path. An infinite
         * in-syscall wait loop -- lx_do_poll(timeout=-1) spinning `check
         * fds; sched_yield()` on an AP whose local queue is always empty
         * (enq_target_for sends everything to the BSP) -- otherwise never
         * releases it, and the ticket lock wedges EVERY other syscall on
         * every CPU. Observed as chrome freezing at ~4s: the pipe-reader
         * thread (tid 1) held the BKL forever on cpu3 at 650k poll
         * iterations/s while the main thread spun at bkl_lock ticket 931
         * on cpu0 with the whole Linux plane behind it (run39 [hb-x]).
         * Dropping + retaking hands the fair ticket queue to any waiter;
         * callers already tolerate this (the slow path below does it). */
        if (me->holds_bkl) { bkl_exit(); bkl_enter(); }
        return;     /* fast path: still running, nothing else here */
    }

    /* Interactivity credit (MLFQ "yield-before-slice-end stays high"): a proc
     * that gives up the CPU with quantum still left -- whether by BLOCKING
     * (about to sleep on a pipe/socket/key) or by voluntarily yielding
     * (nanosleep, cooperative SYS_YIELD, a GUI poll loop) -- is interactive /
     * I/O-bound, so grant it the io_boost that lifts its effective priority on
     * wakeup. CPU-bound procs only reach sched_yield via the quantum-expiry
     * tick (quantum_left == 0, after sched_tick already cleared the boost), so
     * they get no credit. We're past the fast path, so a RUNNING proc here is
     * genuinely ceding the core to a waiting peer. */
    if (cur && cur->quantum_left > 0 &&
        (cur->state == PROC_BLOCKED || cur->state == PROC_RUNNING))
        cur->io_boost = SCHED_IO_BOOST;

    /* Slow path: we may halt or switch away, so we must not keep the BKL.
     * Release it now; reacquire on every path where THIS proc keeps running
     * (YIELD_RETURN) or resumes after a switch (do_switch's reacquire flag).
     * The scheduling work below only touches per-CPU queues (own ready_lock)
     * and per-proc fields, so it's safe without the BKL. */
    /* Honour TIMED futex deadlines HERE, in normal kernel context (full stack,
     * valid GS) rather than in the timer IRQ. Running the sweep from sched_tick
     * (IRQ context) faulted in current_proc()/smp_this_cpu() under chrome's
     * heavy threading; the slow path of sched_yield runs constantly under load,
     * so timeouts still fire promptly. BSP-only + rate-limited. sched_enqueue
     * (taken by the sweep) does not call sched_yield, so no recursion. */
    /* Slice 56: was BSP-only -- which made the whole wake machinery a single
     * point of failure. When one thread sat inside a blocking syscall ON the
     * BSP (hlt-looping, never yielding), pid 0 was parked behind it (is_idle:
     * unstealable, and kernel mode is never tick-preempted), so NEITHER this
     * sweep NOR poll_tick ran anywhere for the whole wait: every chrome timer
     * and poller froze for ~50s at a time ([tick] counters flat, nextdue -26s).
     * Any CPU's yield slow path may drive both: the sweep serialises on
     * g_futex_lock, the poll list on the BKL (hence the holds_bkl gate); the
     * rate-limit statics racing across CPUs just means an occasional extra
     * sweep. */
    if (me) {
        static uint64_t last_futex_sweep;
        uint64_t nowns = perf_now_ns();
        if (nowns - last_futex_sweep >= 10000000ull) {   /* 10 ms */
            last_futex_sweep = nowns;
            extern void futex_expire_timeouts(void);
            futex_expire_timeouts();
        }
        /* slice 43: re-scan blocked poll/epoll/select waiters (self rate-
         * limited to ~1 ms). This is the under-load driver; pid 0's idle_loop
         * covers the all-blocked case. The poll wait list is BKL-serialised, so
         * only touch it when THIS yield is holding the BKL (the common case: a
         * user syscall yielding). BKL-less yields skip it -- idle_loop still
         * covers them. Runs before the bkl_exit below, like the futex sweep. */
        extern void poll_tick(void);
        if (me->holds_bkl) poll_tick();
    }

    bool had_bkl = me->holds_bkl;
    if (had_bkl) bkl_exit();
#define YIELD_RETURN() do { if (had_bkl) bkl_enter(); return; } while (0)

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

    /* Work-stealing: if our queue is empty, try to steal from other CPUs */
    if (!next) {
        uint32_t ncpus = smp_online_count();
        uint32_t my_idx = me->cpu_idx;
        for (uint32_t i = 1; i < ncpus && !next; i++) {
            struct percpu *other = smp_cpu_mut((my_idx + i) % ncpus);
            if (!other || !other->ready_head) continue;
            uint64_t f = spin_lock_irqsave(&other->ready_lock);
            next = queue_steal_locked(other);    /* skip idle procs */
            spin_unlock_irqrestore(&other->ready_lock, f);
        }
    }

    /* No one is ready. If the current proc can resume, just do that
     * (caller's intent was just "let someone else run if possible"). */
    if (!next) {
        if (cur && cur->state == PROC_READY) {
            cur->state = PROC_RUNNING;
            YIELD_RETURN();
        }
        /* cur is BLOCKED/TERMINATED with nothing else runnable. On an AP,
         * switch back to this CPU's idle proc (it steals work / halts). On
         * the BSP (no per-CPU idle proc) we halt in place until an IRQ makes
         * something runnable -- pid 0 drives the GUI and is never blocked, so
         * this only fires for genuinely-stuck waits. The BKL is already
         * released, so other cores run freely while we wait. */
        if (me->idle && cur != me->idle) {
            next = me->idle;
        } else {
            for (;;) {
                /* Slice 94: kickable while halted in place (see sched_idle). */
                __atomic_store_n(&me->idle_halted, 1, __ATOMIC_SEQ_CST);
                sti();
                hlt();
                __atomic_store_n(&me->idle_halted, 0, __ATOMIC_RELAXED);
                uint64_t f = spin_lock_irqsave(&me->ready_lock);
                next = queue_pop_locked(me);
                spin_unlock_irqrestore(&me->ready_lock, f);
                if (next) break;
                if (cur && cur->state == PROC_READY) {
                    cur->state = PROC_RUNNING;
                    YIELD_RETURN();
                }
            }
        }
    }

    /* If we just popped ourselves back off, no actual switch needed. */
    if (next == cur) {
        next->state = PROC_RUNNING;
        YIELD_RETURN();
    }

    do_switch(me, cur, next, had_bkl);   /* releases-already / reacquires BKL */
#undef YIELD_RETURN
}

/* ---------- AP idle loop ---------- */

void sched_idle(void) {
    /* AP scheduler loop, running as this CPU's idle proc (set by ap_entry).
     * Find a runnable proc (our queue first, then steal from other cores) and
     * context-switch into it. When that proc later blocks/exits with nothing
     * else for this CPU, sched_yield switches back here (me->idle) and we look
     * again; if there's genuinely no work anywhere, halt until the next IRQ
     * and retry. The idle proc never holds the BKL (user procs acquire it in
     * their own syscalls). */
    struct percpu *me   = smp_this_cpu();
    struct proc   *idle = me->current;     /* == me->idle */
    for (;;) {
        struct proc *p = g_ap_run_enabled ? steal_one(me) : 0;
        if (p) {
            do_switch(me, idle, p, false);
            me->current = idle;            /* p yielded/exited back to us */
        } else {
            /* Slice 94: advertise "kickable" before parking. A wake kick
             * sent between the store and the hlt stays pending across the
             * sti and breaks the hlt immediately (sti;hlt atomicity). */
            __atomic_store_n(&me->idle_halted, 1, __ATOMIC_SEQ_CST);
            sti();
            hlt();
            __atomic_store_n(&me->idle_halted, 0, __ATOMIC_RELAXED);
        }
    }
}

/* ---------- LAPIC timer tick ---------- */

#ifdef CHROMIUM_BOOT
/* Poor-man's sampling profiler (slice 20). chrome's renderer busy-churns without
 * ever completing a document load, and it has no VLOG, so sample the USER rip on
 * every ring-3 timer tick and report the hottest addresses per interval. Combined
 * with the [libmap] bases (main PIE @0x500000, ld.so @0x40000000) that says WHERE
 * it is spinning; symbolize with objdump --start-address=<rip-base>. */
#define PROF_SLOTS 64
static struct { uint64_t rip; uint32_t hits; int pid; } g_prof[PROF_SLOTS];
static uint32_t g_prof_total;

static void prof_sample(int pid, uint64_t rip) {
    g_prof_total++;
    for (int i = 0; i < PROF_SLOTS; i++)
        if (g_prof[i].hits && g_prof[i].rip == rip) { g_prof[i].hits++; return; }
    for (int i = 0; i < PROF_SLOTS; i++)
        if (!g_prof[i].hits) { g_prof[i].rip = rip; g_prof[i].hits = 1;
                               g_prof[i].pid = pid; return; }
    int w = 0;                              /* table full: evict the coldest */
    for (int i = 1; i < PROF_SLOTS; i++)
        if (g_prof[i].hits < g_prof[w].hits) w = i;
    g_prof[w].rip = rip; g_prof[w].hits = 1; g_prof[w].pid = pid;
}

void prof_dump_and_reset(void) {
    if (!g_prof_total) return;
    kprintf("[prof] %u ring-3 samples this interval; hottest user rips:\n",
            g_prof_total);
    for (int n = 0; n < 10; n++) {          /* selection-sort the top 10 */
        int b = -1;
        for (int i = 0; i < PROF_SLOTS; i++)
            if (g_prof[i].hits && (b < 0 || g_prof[i].hits > g_prof[b].hits)) b = i;
        if (b < 0) break;
        kprintf("  rip=0x%lx hits=%u pid=%d\n", (unsigned long)g_prof[b].rip,
                g_prof[b].hits, g_prof[b].pid);
        g_prof[b].hits = 0;
    }
    for (int i = 0; i < PROF_SLOTS; i++) g_prof[i].hits = 0;
    g_prof_total = 0;
}
#endif

#ifdef CHROMIUM_BOOT
/* [clkchk] slice 41: each CPU stamps its own CLOCK_MONOTONIC reading here every
 * timer tick; the BSP prints the whole array in the heartbeat. If the columns
 * disagree by more than the ~1 tick of sampling skew, the per-CPU TSCs are
 * unsynchronised (the WHPX-vs-TCG clock difference we are chasing) and
 * perf_now_ns() is lying to any thread that migrates cores. */
volatile uint64_t g_cpu_mono_ns[MAX_CPUS];
#endif

void sched_tick(struct regs *r) {
    struct percpu *me = smp_this_cpu();
    if (me) {
#ifdef CHROMIUM_BOOT
        if (me->cpu_idx < MAX_CPUS) g_cpu_mono_ns[me->cpu_idx] = perf_now_ns();
#endif
        me->timer_ticks++;
        /* Per-core usage sampler: this tick caught the core doing real work
         * unless the interrupted proc was its idle task (pid 0 / ap_idle).
         * Sampled here -- before the ring-3-only preempt guard below -- so
         * idle (kernel-mode) ticks are counted in the denominator too. */
        struct proc *c = me->current;
        if (c && !c->is_idle) me->sched_busy_ticks++;
    }
    /* M28C: count scheduler-tick events as heartbeats too. */
    wdog_kick_sched();

    /* (Futex deadline sweep moved OUT of this IRQ context into sched_yield's
     * slow path -- running it here faulted current_proc/smp_this_cpu under
     * chrome.) */

#ifdef CHROMIUM_BOOT
    /* Chromium bring-up (slice 8): every ~3s, dump every Linux (personality==1)
     * proc's scheduler state to catch WHAT the main thread is stuck on during
     * the ~60s freeze (it makes no syscalls, burns little CPU, isn't paging).
     * Placed BEFORE the ring-3-only early-return below so it fires even when the
     * tick interrupts kernel code (the freeze case). BSP-only + throttled so the
     * serial output stays sparse. Diagnostic read of g_proc without a lock is
     * fine for a heartbeat (worst case a garbled line). */
    if (me && me->cpu_idx == 0) {
        static uint64_t last_hb, last_deep;
        uint64_t now = perf_now_ns();
        if (now - last_hb > 3000000000ull) {
            last_hb = now;
            /* Slice 63c: the 3s cadence used to emit the FULL dump (proc
             * table + BKL/per-CPU + 384-entry syscall ring + prof + waitt,
             * ~450 lines) -- ~50k serial lines per 360s run, and every
             * serial byte is a VM EXIT under WHPX, so the watchdog itself
             * was a measurable drag on the thing it watched. Keep a 1-line
             * heartbeat at 3s (liveness + logdrop signal for the wedge
             * classes); run the deep dump every 60s. */
            kprintf("[hb %lu ms] alive logdrop=%lu\n",
                    now / 1000000ull, (unsigned long)serial_dropped());
            /* Slice 91: DEFERRED-REQUEUE BACKSTOP / INSTRUMENT.
             *
             * The -smp 4 freeze is real (slice 89 wrongly exonerated it):
             * six threads READY in clone3 for 293s with BKL acq=0 on ALL
             * four CPUs -- the whole kernel wedged. Prime suspect is the
             * slice-87 quiesce handoff: tg_vm_resume declines to requeue a
             * sibling that is still on_cpu and leaves vm_quiesced=1 for
             * sched_finish_switch to finish -- and if that never runs for
             * that proc, NOTHING ever puts it back on a run queue. Every
             * other lock in this machinery already has a timeout backstop
             * (cow_fork_lock steals after 20M spins, tg_vm_quiesce gives up
             * after 500k); the deferred requeue has none.
             *
             * This is an INSTRUMENT first and a mitigation second: if it
             * ever fires, the deferred-requeue path IS the freeze mechanism
             * and the real fix belongs at the handoff, not here. Runs from
             * the tick because the tick is the last thing still alive when
             * everything else is stuck. */
            {
                extern struct proc g_proc[];
                for (int i = 0; i < PROC_MAX; i++) {
                    struct proc *q = &g_proc[i];
                    if (q->state != PROC_BLOCKED || !q->vm_quiesced) continue;
                    if (__atomic_load_n(&q->vm_quiesce, __ATOMIC_ACQUIRE))
                        continue;                   /* still legitimately parked */
                    if (__atomic_load_n(&q->on_cpu, __ATOMIC_ACQUIRE))
                        continue;                   /* finish_switch still owes it */
                    kprintf("[qstuck] pid=%d parked with vm_quiesce CLEAR and "
                            "off-cpu -- deferred requeue never fired; "
                            "REQUEUEING\n", q->pid);
                    q->vm_quiesced = 0;
                    q->state = PROC_READY;
                    sched_enqueue(q);
                }
            }
            /* Slice 89: per-X-conn state at the 3s cadence. Must ride the
             * sched_tick heartbeat -- pid 0's idle loop never runs under
             * chrome load, so the same call inside xserver_tick printed
             * nothing for an entire 360s run. */
            { extern void xserver_debug_tick(void); xserver_debug_tick(); }
            if (now - last_deep > 60000000000ull) {
            last_deep = now;
            extern struct proc g_proc[];
            uint64_t nt = pit_ticks();
            kprintf("[hb %lu ms] chrome thread states (pit=%lu) logdrop=%lu:\n",
                    now / 1000000ull, nt, (unsigned long)serial_dropped());
            for (int i = 0; i < PROC_MAX; i++) {
                struct proc *p = &g_proc[i];
                if (p->state == PROC_UNUSED || p->personality != 1) continue;
                uint64_t waited = (nt > p->enq_tick) ? (nt - p->enq_tick) : 0;
                kprintf("  pid=%d %-8s prio=%d io=%d enq=%lu wait=%lu eff=%d "
                        "onq=%d oncpu=%d rsp=%p\n",
                        p->pid, proc_state_name(p->state), p->prio,
                        p->io_boost, p->enq_tick, waited, eff_prio(p, nt),
                        /* was `next_ready ? 1 : 0` -- which reports 0 for the
                         * queue TAIL, so this column used to lie. */
                        p->on_rq ? 1 : 0,
                        (int)__atomic_load_n(&p->on_cpu, __ATOMIC_RELAXED),
                        (void *)p->saved_rsp);
            }
            /* Slice 39 freeze triage: one line that discriminates the wedge
             * classes seen in run39 (both chrome threads RUNNING/onq=0
             * forever, ring frozen, no ring-3 samples):
             *   - bkl next>serving frozen  -> a BKL ticket was leaked; every
             *     later syscall spins in bkl_lock (ring-0, invisible).
             *   - pollit climbing          -> lx_do_poll spinner is alive.
             *   - percpu cur/tick          -> WHERE each thread is parked and
             *     whether that CPU still takes LAPIC timer interrupts. */
            {
                extern uint64_t g_lx_poll_iters; extern int g_lx_poll_last_pid;
                kprintf("  [hb-x] bkl next=%u serving=%u pollit=%lu lastpollpid=%d\n",
                        (unsigned)g_bkl.next, (unsigned)g_bkl.serving,
                        (unsigned long)g_lx_poll_iters, g_lx_poll_last_pid);
                for (uint32_t ci = 0; ci < smp_cpu_count() && ci < MAX_CPUS; ci++) {
                    const struct percpu *pc = smp_cpu(ci);
                    if (!pc || !pc->online) continue;
                    kprintf("  [hb-x] cpu%u cur=%d bkl=%d ticks=%lu mono=%lums\n",
                            ci, pc->current ? pc->current->pid : -1,
                            pc->holds_bkl ? 1 : 0,
                            (unsigned long)pc->timer_ticks,
                            (unsigned long)(g_cpu_mono_ns[ci] / 1000000ull));
                }
                /* [clkchk] BSP's own reading right now, to compare against the
                 * per-CPU mono columns above: a large spread == TSC desync. */
                kprintf("  [hb-x] bsp mono-now=%lums\n",
                        (unsigned long)(perf_now_ns() / 1000000ull));
            }
            /* slice 20: chrome busy-churns (pid burning CPU) but never navigates.
             * Dump the recent-syscall ring so we can see WHAT the hot threads
             * loop on at the stall (the livelock, not a blocking wait). */
            extern void lx_dump_recent_syscalls(void);
            lx_dump_recent_syscalls();
            prof_dump_and_reset();   /* hottest user rips this interval */
            { extern void waitt_dump(void); waitt_dump(); }  /* who blocks on what */
            /* Slice 64: BKL contention + syscall mix -- the tier-2 aiming
             * data. wait is in raw Mcycles (divide by ~4300 for ms on this
             * host); acq = acquisitions this interval. */
            for (uint32_t ci = 0; ci < smp_cpu_count() && ci < MAX_CPUS; ci++) {
                kprintf("  [bkl] cpu%u acq=%lu waits=%lu wait=%luMcyc "
                        "held=%luMcyc\n", ci,
                        (unsigned long)g_bkl_acqs[ci],
                        (unsigned long)g_bkl_waits[ci],
                        (unsigned long)(g_bkl_wait_tsc[ci] >> 20),
                        (unsigned long)(g_bkl_held_tsc[ci] >> 20));
                g_bkl_acqs[ci] = g_bkl_waits[ci] = g_bkl_wait_tsc[ci] = 0;
                g_bkl_held_tsc[ci] = 0;
            }
            { extern void lx_dump_syscall_top(void); lx_dump_syscall_top(); }
            /* Slice 92: on-CPU quiesce-park totals (kernel-#PF during a CoW
             * sweep). bkl= counts parks that ARRIVED holding the BKL -- they
             * now drop it for the wait; before slice 92 they kept it, which
             * serialized the whole kernel behind the ticket lock for the
             * sweep duration. */
            { extern uint64_t g_qpark_engaged, g_qpark_bkl;
              extern uint64_t g_fx_spurious;
              kprintf("  [qpark] engaged=%lu bkl=%lu fxspur=%lu\n",
                      (unsigned long)g_qpark_engaged,
                      (unsigned long)g_qpark_bkl,
                      (unsigned long)g_fx_spurious); }
            /* Slice 94: wake->run scheduler latency this interval + kicks. */
            { extern uint64_t g_wlat_sum, g_wlat_max, g_wlat_n, g_wkick;
              kprintf("  [wlat] avg=%luus max=%luus n=%lu wkick=%lu\n",
                      (unsigned long)(g_wlat_n ? g_wlat_sum / g_wlat_n / 1000
                                               : 0),
                      (unsigned long)(g_wlat_max / 1000),
                      (unsigned long)g_wlat_n,
                      (unsigned long)g_wkick);
              g_wlat_sum = g_wlat_max = g_wlat_n = 0; g_wkick = 0; }
            /* (deadlocked-stack dump now fires from signal_send at the renderer's
             * SIGKILL -- the only reliable moment; see bt_dump_group.) */
            }                        /* end 60s deep dump (slice 63c) */
        }
    }
#endif

    /* ---- Fair timeslicing (preemptive) ---------------------------------
     *
     * Only ever preempt when the timer interrupted ring-3 USER code. There the
     * interrupted proc holds no kernel spinlock and not the BKL, so forcing a
     * sched_yield() from this ISR is exactly as safe as the PIT's long-standing
     * ring-3 preemption (pit.c). A CPL-0 tick interrupted kernel code that may
     * hold a lock (heap, ready_lock, a driver lock) -- yielding there could
     * deadlock, so we just count those.
     *
     * This is what gives APs fair timeslicing: only the BSP receives the PIT
     * IRQ, so before this a CPU-bound proc stolen onto an AP ran until it
     * blocked or exited, monopolising that core. Now its per-CPU LAPIC timer
     * burns down its quantum and rotates it for the next runnable proc. The
     * quantum was sized by class in do_switch (sched_quantum_for), so higher-
     * priority procs get a proportionally longer slice. */
    if (!r || (r->cs & 3) != 3) return;

    struct proc *cur = current_proc();
    if (!cur || cur->is_idle) return;        /* never preempt pid 0 / ap_idle */

#ifdef CHROMIUM_BOOT
    /* We interrupted ring 3, so r->rip is this Linux proc's user pc: sample it. */
    if (cur->personality == 1) prof_sample(cur->pid, r->rip);
#endif

    /* Slice 88: CoW-fork stop-the-world. A sibling marked vm_quiesce must
     * leave user mode immediately so vmm_cow_fork can strip PTE_WRITABLE
     * without stale WRITABLE TLBs. Park as BLOCKED (not READY) so we are
     * not re-enqueued until tg_vm_resume. */
    if (__atomic_load_n(&cur->vm_quiesce, __ATOMIC_ACQUIRE)) {
        cur->state = PROC_BLOCKED;
        cur->vm_quiesced = 1;
        sched_yield();
        return;
    }

    /* Asynchronous-signal point for CPU-bound user code on THIS cpu. The
     * PIT only fires on the BSP, so before this a spinning proc running on
     * an AP could dodge a fatal/stop signal indefinitely (it never makes a
     * syscall, and the BSP's pit.c delivery never sees it). Same safety
     * argument as pit.c and the preemption below: we interrupted ring 3, so
     * the proc holds no kernel lock; default dispositions may proc_exit()
     * or stop (both sched_yield away), caught handlers stay pending for the
     * next syscall return. */
    signal_deliver_if_pending();

    if (cur->quantum_left > 0) cur->quantum_left--;
    if (cur->quantum_left > 0) return;        /* slice not spent yet */

    /* Full quantum burned without blocking -> this proc is CPU-bound, not
     * interactive: drop any interactivity boost it had so it competes at its
     * base class from here on (MLFQ demotion). It re-earns the boost only by
     * blocking before its next quantum is up. */
    cur->io_boost = 0;

    /* Slice spent: rotate. sched_yield() demotes cur to READY (re-enqueued on
     * this CPU, refreshing its aging stamp) and context-switches to the highest
     * effective-priority runnable proc -- which also lets a higher-priority
     * proc that became ready mid-slice preempt cur here. do_switch resets the
     * winner's quantum on the way in. The user trap frame is already saved on
     * this kstack; we resume here and iretq normally when cur is rescheduled. */
    sched_yield();
}
