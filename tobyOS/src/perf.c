/* perf.c -- milestone 19 profiling + metrics + structured logging.
 *
 * Layout mirrors perf.h. Every subsystem here is designed to be
 * effectively free when it's disabled:
 *
 *   - perf_zone_end() -> early-out bit test, then one table update
 *   - klog(cat, ...)  -> early-out bit test, then kprintf
 *
 * TSC calibration happens in perf_init() by rdtsc-ing across a known
 * duration of PIT ticks. That's single-digit accuracy on QEMU (the
 * instruction-rate fluctuates with dispatch) but good enough for
 * kernel-grade profiling where we care about relative cost.
 */

#include <tobyos/perf.h>
#include <tobyos/pit.h>
#include <tobyos/printk.h>
#include <tobyos/proc.h>
#include <tobyos/klibc.h>

/* ==== 1. TIMING ================================================== */

static uint64_t g_boot_tsc       = 0;
static uint32_t g_tsc_khz        = 0;   /* 0 => calibration not done */
static bool     g_perf_enabled   = true;

/* kHz-based nanosecond conversion. Input is a TSC delta.
 *
 *   ns = dtsc * 1e6 / khz
 *
 * Using 64x64 math keeps accuracy for any plausible dtsc (up to
 * ~0.5 year at 3 GHz before overflow). */
uint64_t perf_tsc_to_ns(uint64_t dtsc) {
    if (g_tsc_khz == 0) return 0;
    return (dtsc * 1000000ull) / g_tsc_khz;
}

uint64_t perf_now_ns(void) {
    if (g_tsc_khz == 0) return 0;
    return perf_tsc_to_ns(rdtsc() - g_boot_tsc);
}

uint32_t perf_tsc_khz(void) { return g_tsc_khz; }

void perf_init(void) {
    if (g_tsc_khz) return;      /* already calibrated */

    uint32_t hz = pit_hz();
    if (hz == 0) {
        /* PIT isn't running yet -- nothing we can do. Leave khz=0 so
         * perf_now_ns() returns 0 until the user re-calls us. */
        kprintf("[perf] cannot calibrate: PIT not initialised\n");
        return;
    }

    /* Sample TSC across N PIT ticks. At 100 Hz and N=5 we get a 50 ms
     * calibration window, which is long enough to average over cache
     * warm-up but short enough that boot stays snappy. */
    const int N = 5;
    uint64_t t0_ticks = pit_ticks();
    while (pit_ticks() == t0_ticks) { /* align to tick edge */ }
    uint64_t tsc_start = rdtsc();
    uint64_t pit_start = pit_ticks();

    while ((pit_ticks() - pit_start) < (uint64_t)N) {
        /* Busy wait. hlt() would also work but makes the test longer
         * due to wake latency. 50 ms is a boot-time cost anyway. */
    }
    uint64_t tsc_end   = rdtsc();
    uint64_t pit_elap  = pit_ticks() - pit_start;

    /* ns elapsed = pit_elap * 1e9 / hz */
    uint64_t ns_elap = pit_elap * 1000000000ull / hz;
    uint64_t cycles  = tsc_end - tsc_start;

    /* khz = cycles * 1e6 / ns */
    uint64_t khz = (cycles * 1000000ull) / ns_elap;
    if (khz == 0) khz = 1;      /* avoid divide-by-zero downstream */

    g_tsc_khz  = (uint32_t)khz;
    g_boot_tsc = tsc_end;       /* treat "end of calibration" as t=0 */

    kprintf("[perf] TSC calibrated: %u MHz (%lu cycles in %lu ns)\n",
            (unsigned)(g_tsc_khz / 1000),
            (unsigned long)cycles, (unsigned long)ns_elap);
}

/* ==== 2. INSTRUMENTATION ZONES =================================== */

static struct perf_zone_stats g_zones[PERF_Z_COUNT] = {
    [PERF_Z_SCHED_SWITCH ] = { .name = "sched_switch"  },
    [PERF_Z_SYSCALL      ] = { .name = "syscall"       },
    [PERF_Z_VFS_OPEN     ] = { .name = "vfs_open"      },
    [PERF_Z_VFS_READ     ] = { .name = "vfs_read"      },
    [PERF_Z_VFS_WRITE    ] = { .name = "vfs_write"     },
    [PERF_Z_VFS_STAT     ] = { .name = "vfs_stat"      },
    [PERF_Z_GUI_COMPOSITE] = { .name = "gui_composite" },
    [PERF_Z_GUI_FLIP     ] = { .name = "gui_flip"      },
    [PERF_Z_ELF_LOAD     ] = { .name = "elf_load"      },
    [PERF_Z_PROC_SPAWN   ] = { .name = "proc_spawn"    },
    [PERF_Z_NET_RX       ] = { .name = "net_rx"        },
    [PERF_Z_NET_TX       ] = { .name = "net_tx"        },
};

bool perf_enabled(void) { return g_perf_enabled; }

void perf_set_enabled(bool on) {
    g_perf_enabled = on;
    kprintf("[perf] profiling %s\n", on ? "enabled" : "disabled");
}

void perf_zone_end(enum perf_zone z, uint64_t t_start) {
    if (!g_perf_enabled)                      return;
    if ((unsigned)z >= PERF_Z_COUNT)          return;
    uint64_t ns = perf_tsc_to_ns(rdtsc() - t_start);

    struct perf_zone_stats *s = &g_zones[z];
    s->count    += 1;
    s->total_ns += ns;
    if (s->count == 1) {
        s->min_ns = s->max_ns = ns;
    } else {
        if (ns < s->min_ns) s->min_ns = ns;
        if (ns > s->max_ns) s->max_ns = ns;
    }
}

const struct perf_zone_stats *perf_zone_get(enum perf_zone z) {
    if ((unsigned)z >= PERF_Z_COUNT) return 0;
    return &g_zones[z];
}

/* ==== 3. SYSCALL HISTOGRAM ======================================= */

static uint64_t g_sys_count   [PERF_SYS_MAX];
static uint64_t g_sys_total_ns[PERF_SYS_MAX];

void perf_syscall_enter(int num, uint64_t *out_t0) {
    if (out_t0) *out_t0 = rdtsc();
    (void)num;
}

void perf_syscall_exit(int num, uint64_t t0) {
    if (!g_perf_enabled)                    return;
    if ((unsigned)num >= PERF_SYS_MAX)      return;
    g_sys_count   [num] += 1;
    g_sys_total_ns[num] += perf_tsc_to_ns(rdtsc() - t0);
}

uint64_t perf_syscall_count   (int num) {
    if ((unsigned)num >= PERF_SYS_MAX) return 0;
    return g_sys_count[num];
}

uint64_t perf_syscall_total_ns(int num) {
    if ((unsigned)num >= PERF_SYS_MAX) return 0;
    return g_sys_total_ns[num];
}

/* ==== 4. SYSTEM COUNTERS ========================================= */

static struct perf_sys g_sys;

void perf_sys_snapshot(struct perf_sys *out) {
    if (!out) return;
    *out = g_sys;
    out->boot_ns = perf_now_ns();
}

void perf_count_ctx_switch  (void) { g_sys.context_switches++; }
void perf_count_gui_frame   (void) { g_sys.gui_frames++;       }
void perf_count_proc_spawn  (void) { g_sys.proc_spawns++;      }
void perf_count_proc_exit   (void) { g_sys.proc_exits++;       }
void perf_inc_total_syscalls(void) { g_sys.total_syscalls++;   }

void perf_reset(void) {
    for (int i = 0; i < PERF_Z_COUNT; i++) {
        g_zones[i].count    = 0;
        g_zones[i].total_ns = 0;
        g_zones[i].min_ns   = 0;
        g_zones[i].max_ns   = 0;
    }
    memset(g_sys_count,    0, sizeof(g_sys_count));
    memset(g_sys_total_ns, 0, sizeof(g_sys_total_ns));
    g_sys.total_syscalls   = 0;
    g_sys.context_switches = 0;
    g_sys.gui_frames       = 0;
    g_sys.proc_spawns      = 0;
    g_sys.proc_exits       = 0;
    kprintf("[perf] counters reset\n");
}

/* ==== 5. PER-PROC ACCOUNTING ==================================== */

void perf_proc_account_out(struct proc *p, uint64_t tsc_now) {
    if (!p) return;
    if (p->last_switch_tsc) {
        uint64_t dtsc = tsc_now - p->last_switch_tsc;
        p->cpu_ns += perf_tsc_to_ns(dtsc);
    }
    p->last_switch_tsc = 0;
}

void perf_proc_account_in(struct proc *p, uint64_t tsc_now) {
    if (!p) return;
    p->last_switch_tsc = tsc_now;
}

void perf_proc_print_summary(const struct proc *p,
                             uint64_t wall_ns, uint64_t cpu_ns,
                             uint64_t syscalls) {
    if (!p) return;
    /* Fixed-point ms with 3 decimals using integer math. */
    uint64_t wall_ms_i = wall_ns / 1000000ull;
    uint64_t wall_us_r = (wall_ns / 1000ull) % 1000ull;
    uint64_t cpu_ms_i  = cpu_ns  / 1000000ull;
    uint64_t cpu_us_r  = (cpu_ns  / 1000ull) % 1000ull;
    kprintf("time: pid=%d '%s'  wall=%lu.%03lu ms  cpu=%lu.%03lu ms"
            "  syscalls=%lu\n",
            p->pid, p->name,
            (unsigned long)wall_ms_i, (unsigned long)wall_us_r,
            (unsigned long)cpu_ms_i,  (unsigned long)cpu_us_r,
            (unsigned long)syscalls);
}

/* ==== 6. STRUCTURED LOGGING ==================================== */

/* Start conservatively: all categories off so the serial log stays
 * readable during boot. The boot flow enables LOG_CAT_PROC once the
 * scheduler is up so you still see spawn/exit lines; the shell
 * `log enable <cat>` builtin flips everything else. */
static uint32_t g_log_mask = 0;

bool     log_enabled(uint32_t cat) { return (g_log_mask & cat) != 0; }
void     log_enable (uint32_t cat) { g_log_mask |= cat; }
void     log_disable(uint32_t cat) { g_log_mask &= ~cat; }
uint32_t log_mask(void)            { return g_log_mask; }

const char *log_cat_name(uint32_t cat) {
    switch (cat) {
    case LOG_CAT_SCHED:   return "sched";
    case LOG_CAT_SYSCALL: return "syscall";
    case LOG_CAT_PROC:    return "proc";
    case LOG_CAT_VFS:     return "vfs";
    case LOG_CAT_GUI:     return "gui";
    case LOG_CAT_PERF:    return "perf";
    case LOG_CAT_NET:     return "net";
    default:              return 0;
    }
}

uint32_t log_cat_from_name(const char *name) {
    if (!name)                        return 0;
    if (strcmp(name, "sched"  ) == 0) return LOG_CAT_SCHED;
    if (strcmp(name, "syscall") == 0) return LOG_CAT_SYSCALL;
    if (strcmp(name, "proc"   ) == 0) return LOG_CAT_PROC;
    if (strcmp(name, "vfs"    ) == 0) return LOG_CAT_VFS;
    if (strcmp(name, "gui"    ) == 0) return LOG_CAT_GUI;
    if (strcmp(name, "perf"   ) == 0) return LOG_CAT_PERF;
    if (strcmp(name, "net"    ) == 0) return LOG_CAT_NET;
    if (strcmp(name, "all"    ) == 0) return 0xFFFFFFFFu;
    return 0;
}

/* Printf shim. We format via kvprintf (printk.h) so we don't duplicate
 * the converters. Prefix with a short tag so serial.log is grep-able:
 *   [log sched] yield: 1 -> 0 (... ns)
 */
#include <stdarg.h>
#include <tobyos/printk.h>
void klog(uint32_t cat, const char *fmt, ...) {
    if (!log_enabled(cat)) return;
    const char *tag = log_cat_name(cat);
    if (tag) kprintf("[log %s] ", tag);
    else     kprintf("[log ??] ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
}

/* ==== 7. DUMPS FOR THE `perf` BUILTIN =========================== */

static void print_ns(uint64_t ns, const char *tag) {
    uint64_t ms  = ns / 1000000ull;
    uint64_t us  = (ns / 1000ull) % 1000ull;
    kprintf("%s=%lu.%03lu ms", tag, (unsigned long)ms, (unsigned long)us);
}

void perf_dump_zones(void) {
    kprintf("perf zones%s:\n",
            g_perf_enabled ? "" : " (PROFILING OFF)");
    kprintf("  %-16s  %8s  %14s  %10s  %10s  %10s\n",
            "zone", "count", "total_ns", "avg_ns", "min_ns", "max_ns");
    for (int i = 0; i < PERF_Z_COUNT; i++) {
        struct perf_zone_stats *s = &g_zones[i];
        uint64_t avg = s->count ? (s->total_ns / s->count) : 0;
        kprintf("  %-16s  %8lu  %14lu  %10lu  %10lu  %10lu\n",
                s->name ? s->name : "?",
                (unsigned long)s->count, (unsigned long)s->total_ns,
                (unsigned long)avg,
                (unsigned long)s->min_ns, (unsigned long)s->max_ns);
    }
}

void perf_dump_syscalls(void) {
    kprintf("syscall histogram:\n");
    kprintf("  %-4s  %10s  %14s  %12s\n",
            "num", "count", "total_ns", "avg_ns");
    int shown = 0;
    for (int i = 0; i < PERF_SYS_MAX; i++) {
        if (g_sys_count[i] == 0) continue;
        uint64_t avg = g_sys_total_ns[i] / g_sys_count[i];
        kprintf("  %-4d  %10lu  %14lu  %12lu\n",
                i, (unsigned long)g_sys_count[i],
                (unsigned long)g_sys_total_ns[i],
                (unsigned long)avg);
        shown++;
    }
    if (shown == 0) kprintf("  (no syscalls observed)\n");
}

void perf_dump_sys(void) {
    uint64_t up_ns = perf_now_ns();
    kprintf("system metrics:\n");
    kprintf("  uptime       : "); print_ns(up_ns, "ns");  kprintf("\n");
    kprintf("  tsc_khz      : %u\n",  (unsigned)g_tsc_khz);
    kprintf("  ctx_switches : %lu\n", (unsigned long)g_sys.context_switches);
    kprintf("  syscalls     : %lu\n", (unsigned long)g_sys.total_syscalls);
    kprintf("  gui_frames   : %lu\n", (unsigned long)g_sys.gui_frames);
    kprintf("  proc_spawns  : %lu\n", (unsigned long)g_sys.proc_spawns);
    kprintf("  proc_exits   : %lu\n", (unsigned long)g_sys.proc_exits);
}
