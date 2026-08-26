/* watchdog.c -- Milestone 28C kernel watchdog implementation.
 *
 * Three counters (kernel/sched/syscall) are maintained as plain
 * volatile uint64_t. The PIT IRQ calls wdog_kick_kernel() and
 * wdog_check(); sched_yield/sched_tick call wdog_kick_sched();
 * syscall_dispatch calls wdog_kick_proc(). All three updaters are
 * IRQ-safe: a torn 64-bit read on x86_64 is impossible at this
 * boundary (the increments are in IRQ context, the reads run from
 * either the IRQ check or a syscall body that can be preempted but
 * never sees a partial write of the same word).
 *
 * The check fires an "event" whenever:
 *   - sched heartbeat hasn't advanced for > timeout_ms
 *   - kernel heartbeat hasn't advanced for > timeout_ms
 *     (this is a really bad state: even PIT stopped firing -- we
 *     can't actually detect it in software since wdog_check itself
 *     runs from PIT, but a future HW path could)
 *
 * Per-process hangs are intentionally NOT detected from IRQ context
 * (would need to walk the proc table under an IRQ-unsafe spinlock).
 * They are detected from a "scrub" call that the boot harness or a
 * future kernel thread can invoke. For M28C we just ship the
 * scheduler-stall path; per-proc detection is wired through to the
 * userland tooling but not run automatically. */

#include <tobyos/watchdog.h>
#include <tobyos/printk.h>
#include <tobyos/slog.h>
#include <tobyos/pit.h>
#include <tobyos/perf.h>
#include <tobyos/cpu.h>
#include <tobyos/klibc.h>
#include <tobyos/spinlock.h>
#include <tobyos/proc.h>

static volatile uint64_t g_kernel_hb     = 0;
static volatile uint64_t g_sched_hb      = 0;
static volatile uint64_t g_syscall_hb    = 0;
static volatile uint64_t g_last_kernel_kick_ms = 0;
static volatile uint64_t g_last_sched_kick_ms  = 0;

/* M28C: when wdog_simulate_kernel_stall() is running it needs to
 * freeze the scheduler heartbeat so wdog_check() actually sees a
 * stale value. The LAPIC timer ISR fires sched_tick() at high freq
 * even while the BSP is in a busy/hlt loop, which would otherwise
 * keep g_last_sched_kick_ms perpetually fresh and prevent the
 * synthetic bite from firing. Setting g_stall_active turns
 * wdog_kick_sched() into a no-op for the duration of the stall. */
static volatile bool g_stall_active = false;

static uint32_t g_timeout_ms             = WDOG_DEFAULT_TIMEOUT_MS;
static bool     g_ready                  = false;

/* Throttle: only run the body of wdog_check() at WDOG_CHECK_HZ rate
 * even though it's invoked from every PIT tick. */
#define WDOG_CHECK_INTERVAL_MS 1000u
static volatile uint64_t g_last_check_ms = 0;

/* Event log -- single most-recent slot is enough for the userland
 * status syscall. The lock guards the slot during writes; reads
 * happen from syscall context with the lock held briefly. */
static spinlock_t g_evt_lock = SPINLOCK_INIT;
static struct {
    uint64_t   events;
    uint64_t   last_ms;
    uint32_t   last_kind;
    int32_t    last_pid;
    char       last_reason[ABI_WDOG_REASON_MAX];
} g_evt;

/* THE WATCHDOG'S CLOCK MUST NOT BE THE THING THAT DIES.
 *
 * This used to be (pit_ticks() * 1000) / pit_hz(). But pit_sleep_ms()'s own
 * comment says it plainly: "the scheduler moves the tick to the LAPIC timer
 * early in boot, and on headless QEMU/TCG (and defensively on real HW) the
 * IOAPIC->LAPIC PIT edge can be lost". So g_ticks stops advancing in a
 * normal boot -- and a watchdog whose clock has frozen never decides that
 * anything is late. Measured: two runs of the SAME binary, one where the
 * PIT reached 30 s in 30 s of wall time and one where it had not reached it
 * after 178 s.
 *
 * perf_now_ns() is TSC-based and keeps running regardless; it is what the
 * serial heartbeat has always used. */
static uint64_t now_ms(void) {
    return perf_now_ns() / 1000000ull;
}

static const char *kind_name(uint32_t kind) {
    switch (kind) {
    case ABI_WDOG_KIND_SCHED_STALL: return "sched_stall";
    case ABI_WDOG_KIND_IDLE_STALL:  return "idle_stall";
    case ABI_WDOG_KIND_KERNEL_HANG: return "kernel_hang";
    case ABI_WDOG_KIND_USER_HANG:   return "user_hang";
    case ABI_WDOG_KIND_MANUAL:      return "manual";
    default:                         return "none";
    }
}

void wdog_init(uint32_t timeout_ms) {
    if (timeout_ms == 0) timeout_ms = WDOG_DEFAULT_TIMEOUT_MS;
    g_timeout_ms = timeout_ms;
    g_kernel_hb = 0;
    g_sched_hb = 0;
    g_syscall_hb = 0;
    g_last_kernel_kick_ms = now_ms();
    g_last_sched_kick_ms  = g_last_kernel_kick_ms;
    g_last_check_ms       = g_last_kernel_kick_ms;
    spin_init(&g_evt_lock);
    g_evt.events = 0;
    g_evt.last_ms = 0;
    g_evt.last_kind = ABI_WDOG_KIND_NONE;
    g_evt.last_pid = -1;
    g_evt.last_reason[0] = '\0';
    g_ready = true;
    /* Defensive slog -- only safe if slog is up; tested via slog_ready. */
    if (slog_ready()) {
        SLOG_INFO(SLOG_SUB_WDOG, "watchdog ready (timeout=%u ms)",
                  (unsigned)g_timeout_ms);
    } else {
        kprintf("[wdog] ready (timeout=%u ms)\n", (unsigned)g_timeout_ms);
    }
}

bool     wdog_ready(void)       { return g_ready; }
uint32_t wdog_timeout_ms(void)  { return g_timeout_ms; }

void wdog_set_timeout_ms(uint32_t timeout_ms) {
    if (timeout_ms == 0) timeout_ms = WDOG_DEFAULT_TIMEOUT_MS;
    g_timeout_ms = timeout_ms;
}

void wdog_kick_kernel(void) {
    g_kernel_hb++;
    g_last_kernel_kick_ms = now_ms();
}

void wdog_kick_sched(void) {
    /* No-op while a synthetic stall is in flight (see g_stall_active
     * comment above). We deliberately don't even bump the counter --
     * the test verifies that wdog_check observes a stalled scheduler. */
    if (g_stall_active) return;
    g_sched_hb++;
    g_last_sched_kick_ms = now_ms();
}

void wdog_kick_proc(int pid) {
    (void)pid;  /* per-proc tracking lives in proc->syscall_count;
                 * we just maintain a global heartbeat for now. */
    g_syscall_hb++;
}

void wdog_record_event(uint32_t kind, int pid, const char *reason) {
    if (!g_ready) return;
    uint64_t flags = spin_lock_irqsave(&g_evt_lock);
    g_evt.events++;
    g_evt.last_ms   = now_ms();
    g_evt.last_kind = kind;
    g_evt.last_pid  = pid;
    if (reason) {
        size_t i = 0;
        while (i + 1 < sizeof(g_evt.last_reason) && reason[i]) {
            g_evt.last_reason[i] = reason[i];
            i++;
        }
        g_evt.last_reason[i] = '\0';
    } else {
        g_evt.last_reason[0] = '\0';
    }
    spin_unlock_irqrestore(&g_evt_lock, flags);
    /* Outside lock: emit slog so the test harness sees it. slog has
     * its own lock and is IRQ-safe. */
    if (slog_ready()) {
        SLOG_ERROR(SLOG_SUB_WDOG, "event #%lu kind=%s pid=%d reason='%s'",
                   (unsigned long)g_evt.events, kind_name(kind),
                   pid, g_evt.last_reason);
    }
    /* Also emit a kprintf line so the boot harness's serial-log
     * scanner can spot the bite even before slog persistence. */
    kprintf("[wdog] BITE event=%lu kind=%s pid=%d reason='%s'\n",
            (unsigned long)g_evt.events, kind_name(kind),
            pid, reason ? reason : "");
}


/* ---- pid 0 / idle-loop liveness -------------------------------------
 *
 * The serial heartbeat's own comment says "if pid 0 ever wedges, the beat
 * simply stops -- which is itself the signal". It was a signal nothing
 * consumed: on 2026-08-25 an EliteDesk froze at idle, the beat stopped,
 * and not one line came out. The sched-stall test above could not see it
 * because the OTHER CPUs kept scheduling happily -- a global stall is a
 * different thing from pid 0 being stuck, and only the global one was
 * being checked.
 *
 * This check runs from the PIT IRQ, which keeps firing while pid 0 is
 * wedged, so it can still speak when the machine otherwise cannot. It
 * reports the BKL owner and ticket numbers because "pid 0 blocked on a
 * lock somebody else holds" is the top hypothesis for that freeze, and
 * those three numbers distinguish it from every other cause.
 */
static volatile uint64_t g_idle_hb            = 0;
static volatile uint64_t g_last_idle_kick_ms  = 0;
static volatile bool     g_idle_bitten        = false;

void wdog_kick_idle(void) {
    g_idle_hb++;
    g_last_idle_kick_ms = now_ms();
    /* Recovered: arm the bite again so a LATER stall is reported too. */
    g_idle_bitten = false;
}

/* Owned by sched.c, but ONLY compiled under SMP_DIAG -- referencing it
 * unconditionally is a link error, and moving it out of that guard would
 * put a store on the BKL acquire path for the sake of a diagnostic. So:
 * report it when the build has it, say so plainly when it does not. */
#ifdef SMP_DIAG
extern volatile int g_bkl_owner;
#define WDOG_BKL_OWNER  (g_bkl_owner)
#else
#define WDOG_BKL_OWNER  (-2)        /* -2 == not built with SMP_DIAG */
#endif

static void wdog_check_idle(uint64_t now) {
    if (g_idle_hb == 0) return;            /* idle loop not started yet */
    if (g_idle_bitten) return;             /* one bite per stall episode */
    uint64_t since = now - g_last_idle_kick_ms;
    if (since <= g_timeout_ms) return;

    g_idle_bitten = true;
    char reason[ABI_WDOG_REASON_MAX];
    ksnprintf(reason, sizeof reason,
              "pid 0 idle loop stalled %lums (beats=%lu, bkl_owner=%d "
              "[-1 free, -2 needs SMP_DIAG])",
              (unsigned long)since, (unsigned long)g_idle_hb,
              WDOG_BKL_OWNER);
    wdog_record_event(ABI_WDOG_KIND_IDLE_STALL, 0, reason);
}

void wdog_check(void) {
    if (!g_ready) return;
    uint64_t now = now_ms();
    /* Rate-limit the body so we don't spam slog at PIT frequency. */
    if (now - g_last_check_ms < WDOG_CHECK_INTERVAL_MS) return;
    g_last_check_ms = now;

    /* Sched-stall test: the scheduler should be making progress at
     * least once per timeout window. Skip the check entirely until
     * the scheduler has actually started (g_sched_hb > 0) — during
     * early boot, the LAPIC timer and scheduler aren't running yet
     * and that's expected, not a stall. */
    if (g_sched_hb > 0 && (now - g_last_sched_kick_ms) > g_timeout_ms) {
        wdog_record_event(ABI_WDOG_KIND_SCHED_STALL, -1,
                          "scheduler heartbeat stalled");
    }
    /* pid 0 liveness -- see wdog_check_idle(). Separate from the sched
     * test above: the other CPUs can schedule normally while pid 0 is
     * wedged, and that is exactly the case that used to go unreported. */
    wdog_check_idle(now);

    /* Kernel-tick test: PIT itself should advance. Since wdog_check
     * runs from PIT, this is mostly a sanity assertion -- if PIT
     * stopped firing, we wouldn't be here in the first place. We do
     * still emit a one-line log so the boot harness can read the
     * "wdog active" cadence. */
}

void wdog_status(struct abi_wdog_status *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->enabled            = g_ready ? 1u : 0u;
    out->timeout_ms         = g_timeout_ms;
    out->kernel_heartbeats  = g_kernel_hb;
    out->sched_heartbeats   = g_sched_hb;
    out->syscall_heartbeats = g_syscall_hb;
    uint64_t now = now_ms();
    out->ms_since_kernel_kick = (now > g_last_kernel_kick_ms) ?
                                (now - g_last_kernel_kick_ms) : 0;
    out->ms_since_sched_kick  = (now > g_last_sched_kick_ms)  ?
                                (now - g_last_sched_kick_ms)  : 0;
    uint64_t flags = spin_lock_irqsave(&g_evt_lock);
    out->event_count        = g_evt.events;
    out->last_event_ms      = g_evt.last_ms;
    out->last_event_kind    = g_evt.last_kind;
    out->last_event_pid     = g_evt.last_pid;
    memcpy(out->last_event_reason, g_evt.last_reason,
           sizeof(out->last_event_reason));
    spin_unlock_irqrestore(&g_evt_lock, flags);
}

void wdog_simulate_kernel_stall(uint32_t ms) {
    /* This is only safe to call from the boot harness (kernel context,
     * IRQs enabled, no critical sections held). We freeze the sched
     * heartbeat for `ms` ms by busy-waiting -- the kernel heartbeat
     * keeps ticking via the PIT IRQ, but g_sched_hb does not advance
     * because we set g_stall_active=true and wdog_kick_sched() bails
     * early. wdog_check (also in PIT IRQ) sees sched_hb stuck and
     * fires the bite. */
    if (!g_ready) return;
    SLOG_WARN(SLOG_SUB_WDOG, "simulating kernel stall for %u ms", (unsigned)ms);
    /* Freeze the sched-kick clock to "now" once, then disable any
     * further kicks for the duration. wdog_check sees the timestamp
     * recede further into the past as real time advances. */
    g_last_sched_kick_ms = now_ms();
    g_stall_active       = true;
    /* Force wdog_check to actually run its body on the next PIT tick
     * even though it's normally rate-limited to 1 Hz: the throttle
     * uses g_last_check_ms, so we just back-date it. */
    g_last_check_ms = (g_last_sched_kick_ms > WDOG_CHECK_INTERVAL_MS) ?
                      (g_last_sched_kick_ms - WDOG_CHECK_INTERVAL_MS) : 0;
    uint64_t end = now_ms() + ms;
    while (now_ms() < end) {
        /* hlt is fine: PIT IRQ still wakes us. We deliberately do
         * NOT call sched_yield(). */
        sti();
        hlt();
    }
    /* Restore: re-enable kicks and bring the heartbeat current so
     * the box looks healthy again. */
    g_stall_active        = false;
    g_last_sched_kick_ms  = now_ms();
    g_sched_hb++;
    SLOG_INFO(SLOG_SUB_WDOG, "stall simulation complete");
}
