/* perf.h -- milestone 19: profiling, metrics, and structured logging.
 *
 * Three small subsystems share this module because they interlock:
 *
 *  1. TIMING
 *     perf_init() calibrates the TSC against the PIT at boot, so the
 *     rest of the kernel gets a cheap ~1-cycle wall clock via
 *     perf_now_ns()/perf_tsc_to_ns().
 *
 *  2. INSTRUMENTATION ZONES
 *     A small fixed table of hot-path "zones" (scheduler switch,
 *     syscall dispatch, VFS ops, GUI compositor). Each zone keeps a
 *     {count, total_ns, min_ns, max_ns} tuple. Hot paths wrap their
 *     body with:
 *
 *         uint64_t t0 = perf_rdtsc();
 *         ... work ...
 *         perf_zone_end(PERF_Z_XXX, t0);
 *
 *     When profiling is disabled (perf_enabled() == false), the helper
 *     is a one-line early-out so the overhead is a branch + TSC read
 *     the kernel already pays anyway for other purposes.
 *
 *  3. STRUCTURED LOGGING
 *     klog(LOG_CAT_*, fmt, ...) is the "stream" API. Each category
 *     is one bit in a global mask. `log enable sched` / `log disable
 *     syscall` shell builtins flip bits. Disabled categories are a
 *     single bit-test early-out before any formatting work.
 *
 * There are also per-process and system-wide metrics -- those live
 * directly on struct proc + on struct perf_sys respectively, but the
 * helpers to read/update them live here.
 */

#ifndef TOBYOS_PERF_H
#define TOBYOS_PERF_H

#include <tobyos/types.h>
#include <tobyos/cpu.h>

/* ==== 1. TIMING =================================================== */

/* Calibrate TSC against PIT. Must be called AFTER pit_init() has the
 * tick IRQ firing. Prints the measured MHz on the serial log. Safe to
 * call exactly once; subsequent calls are a no-op. */
void     perf_init(void);

/* Raw TSC read, re-exported so callers don't need <tobyos/cpu.h>. */
static inline uint64_t perf_rdtsc(void) { return rdtsc(); }

/* Nanoseconds since boot. Uses the calibrated TSC rate -- before
 * perf_init() completes, returns 0. */
uint64_t perf_now_ns(void);

/* Convert a TSC delta (typically `rdtsc_end - rdtsc_start`) into
 * nanoseconds using the calibrated rate. */
uint64_t perf_tsc_to_ns(uint64_t dtsc);

/* Measured TSC rate (in kHz). 0 until perf_init completes. */
uint32_t perf_tsc_khz(void);

/* ==== 2. INSTRUMENTATION ZONES ==================================== */

enum perf_zone {
    PERF_Z_SCHED_SWITCH = 0,
    PERF_Z_SYSCALL,
    PERF_Z_VFS_OPEN,
    PERF_Z_VFS_READ,
    PERF_Z_VFS_WRITE,
    PERF_Z_VFS_STAT,
    PERF_Z_GUI_COMPOSITE,
    PERF_Z_GUI_FLIP,
    PERF_Z_ELF_LOAD,
    PERF_Z_PROC_SPAWN,
    PERF_Z_NET_RX,
    PERF_Z_NET_TX,
    PERF_Z_COUNT
};

struct perf_zone_stats {
    const char *name;
    uint64_t    count;
    uint64_t    total_ns;
    uint64_t    min_ns;
    uint64_t    max_ns;
};

/* Global on/off switch. The zone/syscall recorders are a one-line
 * early-out when this is false, so wrapping any function with
 * perf_zone_end() is ~free outside profiling sessions. Default = on;
 * `perf off` in the shell flips it. */
bool     perf_enabled(void);
void     perf_set_enabled(bool on);

/* Record a zone sample. t_start was obtained via perf_rdtsc(). */
void     perf_zone_end(enum perf_zone z, uint64_t t_start);

/* Read a zone's stats (copy). Returns NULL if out of range. */
const struct perf_zone_stats *perf_zone_get(enum perf_zone z);

/* Clear all zone + syscall counters. Useful between experiments; the
 * `perf reset` shell builtin calls this. System-wide cumulative
 * counters (total_syscalls, context_switches) are also cleared. */
void     perf_reset(void);

/* Per-syscall histogram. One slot per SYS_* number (size PERF_SYS_MAX).
 * Updated in syscall.c:syscall_dispatch. */
#define PERF_SYS_MAX 40
void         perf_syscall_enter(int num, uint64_t *out_t0);
void         perf_syscall_exit (int num, uint64_t  t0);
uint64_t     perf_syscall_count(int num);
uint64_t     perf_syscall_total_ns(int num);

/* ==== 3. SYSTEM METRICS ========================================== */

struct perf_sys {
    uint64_t boot_ns;
    uint64_t total_syscalls;
    uint64_t context_switches;
    uint64_t gui_frames;
    uint64_t proc_spawns;
    uint64_t proc_exits;
};

/* Take a snapshot of the global counters. Cheap (word-copy). */
void     perf_sys_snapshot(struct perf_sys *out);

/* Incrementers called from the relevant subsystems. Inlined for the
 * hot paths; implementations in perf.c wrap them in klog() when the
 * right category is enabled. */
void     perf_count_ctx_switch(void);
void     perf_count_gui_frame(void);
void     perf_count_proc_spawn(void);
void     perf_count_proc_exit(void);
void     perf_inc_total_syscalls(void);

/* ==== 4. PER-PROCESS HELPERS ==================================== */

/* Called from sched_yield on switch-out / switch-in. They live here
 * so the logic (how "cpu_ns" accumulates) is documented next to the
 * rest of the profiling code. */
struct proc;
void     perf_proc_account_out(struct proc *p, uint64_t tsc_now);
void     perf_proc_account_in (struct proc *p, uint64_t tsc_now);

/* One-line summary a caller can log after a command completes. Used
 * by the `time` builtin. */
void     perf_proc_print_summary(const struct proc *p,
                                 uint64_t wall_ns, uint64_t cpu_ns,
                                 uint64_t syscalls);

/* ==== 5. STRUCTURED LOGGING ==================================== */

enum log_cat {
    LOG_CAT_SCHED   = 1u << 0,
    LOG_CAT_SYSCALL = 1u << 1,
    LOG_CAT_PROC    = 1u << 2,
    LOG_CAT_VFS     = 1u << 3,
    LOG_CAT_GUI     = 1u << 4,
    LOG_CAT_PERF    = 1u << 5,
    LOG_CAT_NET     = 1u << 6,
};

bool     log_enabled(uint32_t cat);
void     log_enable (uint32_t cat);
void     log_disable(uint32_t cat);
uint32_t log_mask(void);

/* Printf-style logger gated by `cat`. Prefixes with a short category
 * tag so you can grep serial.log. Disabled categories are a single
 * bit-test, then return. Format uses the same converters as kprintf.
 */
void     klog(uint32_t cat, const char *fmt, ...);

/* Translate a LOG_CAT_* bit (must be a single bit) to its name; used
 * by the `log` shell builtin to print the current mask. Returns NULL
 * if cat is zero or >1 bit. */
const char *log_cat_name(uint32_t cat);
/* Lookup by name for the toggle builtin. Returns 0 on unknown. */
uint32_t    log_cat_from_name(const char *name);

/* ==== 6. PRINT HELPERS (for the shell) ========================== */

/* One-line per zone, indented for the `perf` builtin. Walks all
 * zones in enum order. */
void     perf_dump_zones(void);

/* One-line per non-zero syscall slot. */
void     perf_dump_syscalls(void);

/* Uptime + cumulative counters. */
void     perf_dump_sys(void);

#endif /* TOBYOS_PERF_H */
