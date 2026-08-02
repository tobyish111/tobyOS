/* apic.c -- xAPIC (MMIO) Local APIC driver.
 *
 * MMIO register offsets we use (Intel SDM vol 3, table 11-1):
 *
 *   0x020  ID                                (R/W on some CPUs, read here)
 *   0x030  Version
 *   0x080  TPR  -- Task Priority Register
 *   0x0B0  EOI                               (write any value)
 *   0x0F0  SVR  -- Spurious-Interrupt Vector Register
 *                  bit 8       = LAPIC software enable
 *                  bits[7:0]   = spurious vector
 *   0x300  ICR_LOW
 *   0x310  ICR_HIGH                          (target APIC ID in [31:24])
 *   0x320  LVT Timer                         bit 16 = mask, bit 17 = mode
 *   0x380  Initial Count
 *   0x390  Current Count
 *   0x3E0  Divide Configuration Register     0x0B = divide-by-1
 *
 * ICR_LOW field cheat-sheet (the bits we actually set):
 *
 *   [10:8]  Delivery mode:  0=fixed, 4=NMI, 5=INIT, 6=startup
 *   [11]    Destination mode (0 = physical APIC ID)
 *   [12]    Delivery status (RO; 1 = pending)
 *   [14]    Level (1 = assert)
 *   [15]    Trigger (0 = edge, 1 = level)
 *   [19:18] Destination shorthand (0=use ICR_HI)
 *
 * IA32_APIC_BASE MSR (0x1B):
 *   bit 11 (EN)  -- LAPIC hardware enable (we set if not already)
 *   bit 8  (BSP) -- read-only "is this the BSP?"
 */

#include <tobyos/apic.h>
#include <tobyos/acpi.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/printk.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>
#include <tobyos/isr.h>
#include <tobyos/proc.h>     /* slice 57: cr3-targeted shootdowns */
#include <tobyos/percpu.h>
#include <tobyos/perf.h>     /* slice 95: time-bounded shootdown ack waits */
#include <tobyos/sched.h>
#include <tobyos/smp.h>      /* tlb_shootdown_remote: percpu walk */

/* Single virtual page mapped to LAPIC MMIO. Same kernel virt for every
 * CPU; the page resolves (in HW) to the per-CPU LAPIC. */
#define LAPIC_KVIRT 0xffffd20000000000ULL

#define LAPIC_REG_ID            0x020
#define LAPIC_REG_VERSION       0x030
#define LAPIC_REG_TPR           0x080
#define LAPIC_REG_EOI           0x0B0
#define LAPIC_REG_SVR           0x0F0
#define LAPIC_REG_ICR_LOW       0x300
#define LAPIC_REG_ICR_HIGH      0x310
#define LAPIC_REG_LVT_TIMER     0x320
#define LAPIC_REG_TIMER_INIT    0x380
#define LAPIC_REG_TIMER_CURR    0x390
#define LAPIC_REG_TIMER_DIV     0x3E0

#define IA32_APIC_BASE_MSR      0x1Bu
#define IA32_APIC_BASE_EN       (1ULL << 11)

#define ICR_DELIVERY_INIT       (5u << 8)
#define ICR_DELIVERY_STARTUP    (6u << 8)
#define ICR_LEVEL_ASSERT        (1u << 14)
#define ICR_TRIGGER_EDGE        (0u << 15)
#define ICR_DEST_PHYSICAL       (0u << 11)
#define ICR_DELIV_STATUS        (1u << 12)

#define SVR_SOFT_ENABLE         (1u << 8)

#define LVT_MASKED              (1u << 16)
#define LVT_TIMER_PERIODIC      (1u << 17)

static volatile uint8_t *g_lapic;     /* virt pointer to MMIO page */
static bool              g_ready;

bool apic_is_ready(void) { return g_ready; }

/* MMIO accessors -- LAPIC registers are dword-sized at dword offsets. */
static inline uint32_t lapic_read(uint32_t off) {
    return *(volatile uint32_t *)(g_lapic + off);
}
static inline void lapic_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(g_lapic + off) = val;
}

uint32_t apic_read_id(void) {
    /* APIC id lives in bits [31:24] of the ID register. */
    return lapic_read(LAPIC_REG_ID) >> 24;
}

void apic_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0);
}

/* ---- TLB shootdown (slice 38 iter 24) ------------------------------------
 *
 * The kernel had NO cross-CPU TLB invalidation. Any PTE downgrade (fork's
 * CoW write-protect, mprotect, munmap, madvise-drop) or frame CHANGE (CoW
 * copy-out) only flushed the CPU that executed it. Another CPU that had
 * the old translation cached kept using it -- observed as chrome's
 * SwiftShader JIT thread writing four spills through a stale WRITABLE
 * entry into the pre-CoW-copy GHOST frame, then (after the entry was
 * evicted mid-sequence) reading the real frame's pre-fork bytes back:
 * a deterministic "lost store" crash 10.8s after the fork that
 * write-protected the page (page-journal autopsy, run38 iter 24).
 *
 * Design: fixed-vector IPI; the handler reloads CR3 (flushes all
 * non-global entries) and bumps its per-CPU generation counter. The
 * sender snapshots every target's generation, sends the IPIs, and spins
 * until each target's counter moves (or a bounded timeout -- a target
 * parked with IRQs off can't ack; we warn instead of deadlocking). */
static volatile uint32_t g_tlb_gen[MAX_CPUS];

/* Slice 94: wake-kick receiver -- nothing to do; the interrupt itself
 * broke the target's hlt, and its idle loop re-scans the queues next. */
static void sched_wake_isr(struct regs *r) {
    (void)r;
    apic_eoi();
}

void apic_sched_wake(uint8_t target_apic_id) {
    apic_send_ipi(target_apic_id,
                  ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE |
                  ICR_DEST_PHYSICAL | SCHED_WAKE_VECTOR);
}

static void tlb_shootdown_isr(struct regs *r) {
    (void)r;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    uint32_t idx = smp_this_cpu()->cpu_idx;
    __atomic_fetch_add(&g_tlb_gen[idx], 1, __ATOMIC_SEQ_CST);
    apic_eoi();
}

/* Slice 57: cr3-filtered shootdown. cr3 == 0 -> broadcast (the historical
 * behaviour). Otherwise skip CPUs whose CURRENT proc is in a different
 * address space: user PTEs are non-global, and every context switch loads
 * CR3 (no PCID), so a CPU that switched away has already flushed -- only
 * CPUs running THIS space can hold its stale user translations. The racy
 * read of another CPU's ->current is safe: proc structs live in the static
 * pool, aligned 8-byte reads don't tear, and both race directions err
 * toward SENDING (a CPU switching INTO the space loads CR3 = flush; one
 * switching OUT gets a redundant IPI). ASSUMPTION (documented): kernel-side
 * code never touches user memory of an address space other than current's
 * (true today; the stage-12D HTTP worker predates chrome and is unused by
 * it). */
static bool cpu_may_hold_cr3(const struct percpu *c, uint64_t cr3) {
    if (cr3 == 0) return true;
    struct proc *p = c->current;
    if (!p) return true;                  /* unknown -> conservative send */
    return p->cr3 == cr3;
}

/* Slice 87: break the IRQ-off shootdown deadlock WITHOUT sti/BKL games.
 *
 * Two CPUs can take CoW faults concurrently with IRQs masked. Each then
 * calls tlb_shootdown_remote() and waits for the other to ack -- but with
 * IRQs off neither runs the shootdown ISR. Both time out, both proceed with
 * stale WRITABLE TLB entries, and PartitionAlloc freelist heads get
 * scribbled (full-chrome #GP at ThreadCache, rip in PA).
 *
 * An earlier attempt sti'd across the wait: that let peers ack, but also
 * let timer/scheduling run mid-fault and produced a kstack_top=0 SMEP
 * panic on thread-group teardown. Dropping the BKL raced VMA mutations
 * and aborted chrome at ~12s on an INT3/CHECK.
 *
 * Fix: while WE wait (possibly with IRQs off), periodically self-flush and
 * bump our own generation. That is exactly what the ISR would do, so any
 * peer waiting on us observes progress and completes -- deadlock broken,
 * no sti, no BKL drop. */
/* Slice 95: shootdown-latency visibility (printed by the deep dump as
 * [tlbto]). ack_timeouts = per-CPU 2M-spin round timeouts; giveups =
 * tlb_shootdown_remote returning after 2 un-acked rounds (see below). */
uint64_t g_tlb_ack_timeouts, g_tlb_giveups;

static void tlb_shootdown_self_ack(uint32_t me) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    __atomic_fetch_add(&g_tlb_gen[me], 1, __ATOMIC_SEQ_CST);
}

static bool tlb_shootdown_sync_filtered(uint64_t cr3) {
    if (!g_ready) return true;
    uint32_t n = smp_cpu_count();
    if (n <= 1) return true;

    uint32_t me = smp_this_cpu()->cpu_idx;
    uint32_t snap[MAX_CPUS];
    bool     sent[MAX_CPUS] = { false };

    for (uint32_t i = 0; i < n && i < MAX_CPUS; i++) {
        if (i == me) continue;
        const struct percpu *c = smp_cpu(i);
        if (!c || !c->online) continue;
        if (!cpu_may_hold_cr3(c, cr3)) continue;
        snap[i] = g_tlb_gen[i];
        apic_send_ipi((uint8_t)c->apic_id,
                      ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE |
                      ICR_DEST_PHYSICAL | TLB_SHOOTDOWN_VECTOR);
        sent[i] = true;
    }

    /* Bounded wait: a remote CPU spinning with IRQs masked cannot take the
     * IPI until it re-enables them; it WILL flush before touching user
     * translations again (the IPI stays pending). Don't deadlock on it --
     * but REPORT it: a caller about to free unmapped frames must NOT hand
     * them back to the PMM while any CPU could still hold the stale entry
     * (slice 50: it quarantines them until a later fully-acked shootdown).
     * Under WHPX a vCPU can be host-descheduled for >2M pauses, so timeouts
     * here are routine, not exceptional.
     *
     * Slice 87: self-ack periodically so an IRQ-off peer that is itself in
     * this wait loop can make OUR gen move (and we make theirs move). */
    bool all_acked = true;
    bool self_demoted = false;
    for (uint32_t i = 0; i < n && i < MAX_CPUS; i++) {
        if (!sent[i]) continue;
        /* Slice 95: TIME-bound the wait, don't iteration-bound it. The old
         * 2M-pause spin self-acked every 256 iterations, and each self-ack
         * is a CR3 reload = a VM EXIT under WHPX: ~7.8k exits made one
         * timed-out round cost ~4.5k Mcyc (~1 s) -- measured as the
         * constant [bklmax] hold behind every residual frame stall. An
         * AWAKE CPU acks in microseconds; 10 ms is two orders of margin.
         * A CPU that misses 10 ms is not executing, and its pending IPI
         * flushes at VM-entry before any user instruction (same argument
         * as the round-cap above). */
        uint64_t deadline = perf_now_ns() + 10000000ull;   /* 10 ms */
        uint32_t spins = 0;
        while (g_tlb_gen[i] == snap[i]) {
            __asm__ volatile("pause" ::: "memory");
            if ((spins & 0xfffu) == 0)
                tlb_shootdown_self_ack(me);
            /* Help CoW-fork STW: we are in-kernel with IRQs off, so demote
             * to BLOCKED while still on_cpu (ACK path). Otherwise the forker
             * waits forever for !PROC_RUNNING. */
            {
                struct proc *cp = current_proc();
                if (cp && __atomic_load_n(&cp->vm_quiesce, __ATOMIC_ACQUIRE)) {
                    cp->state = PROC_BLOCKED;
                    cp->vm_quiesced = 1;
                    self_demoted = true;
                }
            }
            if (((++spins & 0x3ffu) == 0) && perf_now_ns() > deadline) {
                static uint32_t warns;
                extern uint64_t g_tlb_ack_timeouts;
                __atomic_fetch_add(&g_tlb_ack_timeouts, 1, __ATOMIC_RELAXED);
                if (warns < 8) {
                    warns++;
                    kprintf("[tlb] WARN: cpu%u shootdown ack timeout\n", i);
                }
                all_acked = false;
                break;
            }
        }
    }
    /* Slice 92: UNDO the demotion before returning. The demote is a lie
     * told to the forker's quiesce wait ("I am parked") by a proc that in
     * fact KEEPS EXECUTING -- leaving state=BLOCKED + vm_quiesced=1 on a
     * running proc is the STALE FLAG that armed the -smp 4 freeze: the
     * proc's next futex park hit sched_finish_switch's deferred requeue
     * (BLOCKED + stale flag -> spurious READY while still on the futex
     * waiter list -> re-park -> CYCLIC list -> the expire sweep spins
     * forever holding g_futex_lock with IRQs off). If vm_quiesce is still
     * up, the tick / syscall-return / #PF park points will park us
     * properly; the quiesce wait re-times-out on us as in-kernel, which is
     * the truth. */
    if (self_demoted) {
        struct proc *cp = current_proc();
        if (cp && cp->state == PROC_BLOCKED) {
            cp->state = PROC_RUNNING;
            cp->vm_quiesced = 0;
        }
    }
    return all_acked;
}

bool tlb_shootdown_remote_sync(void) {
    /* Broadcast (cr3=0). Retry a handful of times so a single WHPX
     * deschedule does not force every free into quarantine. */
    for (int round = 0; round < 8; round++)
        if (tlb_shootdown_sync_filtered(0)) return true;
    return false;
}

void tlb_shootdown_remote(void) {
    /* Slice 57: RETRY until every relevant CPU acks. The old fire-and-forget
     * dropped all_acked=false on the floor -- tolerable for the FREE path
     * (slice-50 quarantine holds the frames back) but NOT for permission
     * tightening: fork's write-protect sweep, CoW copy-out and mprotect all
     * continued with a laggard CPU still holding stale WRITABLE
     * translations, so a parent thread on that CPU kept writing straight
     * into frames now shared with the fork child. Measured: run-11's 8-warn
     * cap saturated by 5.4s, then a fork at 15.6s + five seconds of CoW
     * faults ended in four processes crashing on zeroed/poisoned pointers
     * within 2s; with retry-until-acked (run 12) the crashes went to ZERO.
     * The naive broadcast retry was however catastrophically slow under
     * WHPX (bootstrap 267s: every round waits out host-descheduled vCPUs
     * running OTHER processes). Hence the cr3 filter: only CPUs currently
     * IN this address space can hold its stale user translations -- those
     * are awake-and-working CPUs that ack in microseconds.
     *
     * Slice 87: self-ack in the wait loop (see above) + higher round cap.
     *
     * Slice 95: round cap 64 -> 2. THE 64-ROUND STORM WAS THE LATENCY TAIL:
     * one round against a non-acking vCPU costs a 2M-pause spin (~45 ms
     * measured), so 64 rounds = ~2.9 s -- and mprotect holds the BKL across
     * this wait, so one host-descheduled vCPU turned into whole-kernel
     * 3-6 s convoys ([bklmax] hold=~12000Mcyc pid=chrome, [wtail] clusters
     * of threads pickable 5.9 s, the [cwif] multi-second frame stalls).
     * Correctness argument for giving up after 2 full rounds: an AWAKE CPU
     * in this address space acks in MICROSECONDS (round 1, always -- the
     * whole point of the cr3 filter). A CPU that has not acked after two
     * 2M-spin rounds is NOT EXECUTING (host-descheduled or IRQ-off in
     * kernel code that touches no user memory). A non-executing vCPU
     * cannot consume a stale translation, and its pending IPI is injected
     * at VM-entry before any further user instruction retires -- so the
     * flush still happens before the stale entry could be used. Spinning
     * longer under the BKL protected nothing. [tlbto]'s giveup counter in
     * the deep dump keeps the frequency visible. */
    struct proc *me = current_proc();
    uint64_t cr3 = me ? me->cr3 : 0;
    for (int round = 0; round < 2; round++)
        if (tlb_shootdown_sync_filtered(cr3)) return;
    extern uint64_t g_tlb_giveups;
    __atomic_fetch_add(&g_tlb_giveups, 1, __ATOMIC_RELAXED);
    static uint32_t hard_warns;
    if (hard_warns < 8) {
        hard_warns++;
        kprintf("[tlb] shootdown un-acked after 2 rounds -- returning on the "
                "pending-IPI argument (see slice 95)\n");
    }
}

void apic_send_ipi(uint8_t target_apic_id, uint32_t icr_low) {
    /* Spin until any prior delivery completes. */
    while (lapic_read(LAPIC_REG_ICR_LOW) & ICR_DELIV_STATUS) {
        __asm__ volatile ("pause" ::: "memory");
    }
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)target_apic_id << 24);
    /* Writing the LOW register triggers the actual send. */
    lapic_write(LAPIC_REG_ICR_LOW, icr_low);
    /* Optional: spin until the send has been accepted by the bus. */
    while (lapic_read(LAPIC_REG_ICR_LOW) & ICR_DELIV_STATUS) {
        __asm__ volatile ("pause" ::: "memory");
    }
}

void apic_send_init(uint8_t target_apic_id) {
    apic_send_ipi(target_apic_id,
                  ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT |
                  ICR_TRIGGER_EDGE  | ICR_DEST_PHYSICAL);
}

void apic_send_sipi(uint8_t target_apic_id, uint8_t vector) {
    apic_send_ipi(target_apic_id,
                  ICR_DELIVERY_STARTUP | ICR_LEVEL_ASSERT |
                  ICR_TRIGGER_EDGE     | ICR_DEST_PHYSICAL | vector);
}

void apic_init_local(void) {
    /* Hardware-enable via IA32_APIC_BASE if the firmware left it off. */
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    if ((base & IA32_APIC_BASE_EN) == 0) {
        wrmsr(IA32_APIC_BASE_MSR, base | IA32_APIC_BASE_EN);
    }

    /* Accept all priorities. */
    lapic_write(LAPIC_REG_TPR, 0);

    /* Software-enable + spurious vector. From this point on the LAPIC
     * will deliver any pending IPIs. */
    lapic_write(LAPIC_REG_SVR, SVR_SOFT_ENABLE | APIC_SPURIOUS_VECTOR);

    /* Configure the timer in a known-good state but keep it MASKED.
     * That way it won't fire (no scheduler yet) but a future milestone
     * can simply unmask + write TIMER_INIT to start ticking. */
    lapic_write(LAPIC_REG_TIMER_DIV,   0x0B);  /* divide by 1 */
    lapic_write(LAPIC_REG_LVT_TIMER,   LVT_MASKED | APIC_TIMER_VECTOR);
    lapic_write(LAPIC_REG_TIMER_INIT,  0);
}

/* Cached LAPIC timer rate from apic_timer_calibrate_global. Same on
 * every CPU because the LAPIC timer ticks off the bus clock, which
 * is shared across all logical CPUs in QEMU and on real x86_64 boxes
 * built around a single core complex. */
static uint32_t g_lapic_ticks_per_sec;

/* One-shot calibration: count LAPIC timer ticks during a 10 ms PIT
 * sleep. Purely informational -- we log the rate so the next milestone
 * has a starting point for "set timer to fire every N ms". */
static void apic_calibrate_timer(void) {
    /* Briefly unmask the counter (still no IRQ -- we don't even hit the
     * fire condition because we read the count before it expires). */
    lapic_write(LAPIC_REG_TIMER_DIV,  0x0B);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFu);
    pit_sleep_ms(10);
    uint32_t curr = lapic_read(LAPIC_REG_TIMER_CURR);
    uint32_t elapsed = 0xFFFFFFFFu - curr;
    /* Re-mask, leave init=0 so it stops counting. */
    lapic_write(LAPIC_REG_LVT_TIMER,  LVT_MASKED | APIC_TIMER_VECTOR);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);

    /* Stash for use by apic_timer_periodic_init. Multiply by 100 to
     * get per-second from per-10ms; saturate at UINT32_MAX-ish in
     * case calibration ever picks up a wildly fast clock. */
    uint64_t per_sec = (uint64_t)elapsed * 100ull;
    g_lapic_ticks_per_sec = per_sec > 0xFFFFFFFFull
        ? 0xFFFFFFFFu
        : (uint32_t)per_sec;

    kprintf("[apic] timer calibrated: ~%u ticks/10ms (~%u kHz, ~%u Hz/sec)\n",
            (unsigned)elapsed, (unsigned)(elapsed / 10u),
            (unsigned)g_lapic_ticks_per_sec);
}

void apic_timer_calibrate_global(void) {
    if (!g_ready) return;
    if (g_lapic_ticks_per_sec) return;          /* already calibrated */
    apic_calibrate_timer();
}

/* IDT-side ISR for vector APIC_TIMER_VECTOR. The same handler runs
 * on every CPU; sched_tick consults smp_this_cpu() internally so we
 * don't have to thread cpu_idx through the dispatch path. EOI is to
 * the LAPIC (apic_eoi) -- the LAPIC timer is delivered by the local
 * APIC, never via the PIC or IO APIC.
 *
 * EOI is sent BEFORE sched_tick(): when the timeslice is spent, sched_tick
 * preempts by switching away and does not return promptly, so the LAPIC must
 * already be acked or it would never deliver another timer to this core (same
 * ordering the PIT ISR uses for its preemption). The trap frame `r` is passed
 * through so sched_tick can check the interrupted privilege level -- it only
 * preempts when ring 3 was interrupted. */
static void apic_timer_isr(struct regs *r) {
    apic_eoi();
    sched_tick(r);
}

bool apic_timer_periodic_init(uint32_t hz) {
    if (!g_ready)             return false;
    if (hz == 0)              return false;
    if (g_lapic_ticks_per_sec == 0) {
        /* Late callers can ask us to calibrate on the fly. Only the
         * BSP should ever hit this branch, since we calibrate during
         * apic_init_bsp; APs run after that and inherit the cached
         * rate. */
        apic_timer_calibrate_global();
        if (g_lapic_ticks_per_sec == 0) return false;
    }
    uint32_t reload = g_lapic_ticks_per_sec / hz;
    if (reload < 16) reload = 16;               /* don't program a runaway */

    /* Periodic mode + our chosen vector + UNMASKED. The divider is
     * already 0x0B (divide-by-1) from apic_init_local; reset it just
     * to be defensive in case some future code touched it. */
    lapic_write(LAPIC_REG_TIMER_DIV,   0x0B);
    lapic_write(LAPIC_REG_LVT_TIMER,   LVT_TIMER_PERIODIC | APIC_TIMER_VECTOR);
    lapic_write(LAPIC_REG_TIMER_INIT,  reload);
    return true;
}

bool apic_init_bsp(void) {
    const struct acpi_info *info = acpi_get();
    uint64_t lapic_phys = info && info->ok ? info->lapic_phys : 0xFEE00000ULL;

    /* Some firmware leaves bit-0 / low bits set in the lapic_phys field
     * by accident. The MMIO must be page-aligned. */
    lapic_phys &= ~((uint64_t)PAGE_SIZE - 1);

    if (!vmm_map(LAPIC_KVIRT, lapic_phys, PAGE_SIZE,
                 VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_NOCACHE)) {
        kprintf("[apic] vmm_map for LAPIC MMIO at phys %p failed\n",
                (void *)lapic_phys);
        return false;
    }
    g_lapic = (volatile uint8_t *)LAPIC_KVIRT;

    apic_init_local();   /* enable on the BSP */
    g_ready = true;

    uint32_t id  = apic_read_id();
    uint32_t ver = lapic_read(LAPIC_REG_VERSION) & 0xFF;
    kprintf("[apic] BSP local APIC: phys=%p virt=%p id=%u ver=0x%x\n",
            (void *)lapic_phys, (void *)LAPIC_KVIRT,
            (unsigned)id, (unsigned)ver);
    kprintf("[apic] SVR=0x%x  spurious_vec=0x%x  timer_vec=0x%x (masked)\n",
            (unsigned)lapic_read(LAPIC_REG_SVR),
            APIC_SPURIOUS_VECTOR, APIC_TIMER_VECTOR);

    apic_calibrate_timer();                     /* fills g_lapic_ticks_per_sec */

    /* Register the timer ISR on the IDT. The timer is still MASKED
     * after calibration, so this is harmless until somebody calls
     * apic_timer_periodic_init() to unmask + program a reload value.
     * The IDT is shared across all CPUs (every AP runs lidt with the
     * same kernel IDTR via the trampoline) so registering here once
     * covers BSP + every AP. */
    isr_register(APIC_TIMER_VECTOR, apic_timer_isr);
    /* TLB shootdown IPI handler -- shared IDT, so once covers all CPUs. */
    isr_register(TLB_SHOOTDOWN_VECTOR, tlb_shootdown_isr);
    isr_register(SCHED_WAKE_VECTOR, sched_wake_isr);     /* slice 94 */
    return true;
}
