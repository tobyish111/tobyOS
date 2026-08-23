/* tobyOS vDSO (2026-08-23). Built as a real x86-64 Linux shared object at
 * kernel build time, embedded in the kernel image, and mapped read-execute
 * into every Linux-personality process; glibc finds it through
 * AT_SYSINFO_EHDR and resolves the LINUX_2.6-versioned symbols below,
 * after which every clock_gettime/gettimeofday/time is a userspace read
 * of the shared data page instead of a syscall.
 *
 * RULES THIS FILE LIVES BY:
 *   - No relocations. The kernel maps the raw file image and nobody ever
 *     processes relocs: everything internal is static/hidden and reached
 *     pc-relative. __ehdr_start (linker-provided, hidden) is the module
 *     base; the data page sits one page BELOW it, exactly like Linux's
 *     vvar.
 *   - No libcalls. u64 multiply/divide only (single divq on x86-64), no
 *     memset/memcpy shapes the compiler could "optimise" into calls.
 *   - The time math is BIT-FOR-BIT the kernel's perf_now_ns():
 *     ns = (rdtsc - boot_tsc) * 1000000 / tsc_khz. Same formula, same
 *     published constants, so a vDSO read and a syscall read can never
 *     disagree by more than the rounding of one division.
 *   - Any clock we do not model falls back to the REAL SYSCALL from
 *     inside the vDSO (Linux's own convention), so unknown clock ids
 *     keep exactly their pre-vDSO behaviour.
 *
 * The data page layout is struct vdso_data in include/tobyos/vdso.h --
 * the kernel writes it (seqlock protocol), this file only reads. */

typedef int                i32;
typedef long long          i64;
typedef unsigned int       u32;
typedef unsigned long long u64;

struct vdso_data {
    u32 seq;            /* odd = writer active; retry */
    u32 tsc_khz;        /* 0 = not calibrated: always fall back */
    u64 boot_tsc;
    u64 epoch_base_ns;  /* CLOCK_REALTIME - CLOCK_MONOTONIC; 0 = no RTC */
};

struct ts { i64 sec; i64 nsec; };
struct tv { i64 sec; i64 usec; };

#define CLK_REALTIME         0
#define CLK_MONOTONIC        1
#define CLK_MONOTONIC_RAW    4
#define CLK_REALTIME_COARSE  5
#define CLK_MONOTONIC_COARSE 6
#define CLK_BOOTTIME         7

static inline u64 rdtsc_ordered(void) {
    u32 lo, hi;
    /* lfence keeps the TSC read from drifting ahead of the seq check. */
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return lo | ((u64)hi << 32);
}

static inline const volatile struct vdso_data *vd(void) {
    extern const char __ehdr_start[] __attribute__((visibility("hidden")));
    return (const volatile struct vdso_data *)(__ehdr_start - 4096);
}

static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "0"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

/* 0 on success with *out = nanoseconds; -1 = caller must syscall. */
static int read_clock(int clk, u64 *out) {
    const volatile struct vdso_data *d = vd();
    for (int tries = 0; tries < 100; tries++) {
        u32 s0 = d->seq;
        if (s0 & 1) continue;
        __asm__ volatile("" ::: "memory");
        u32 khz = d->tsc_khz;
        u64 bt  = d->boot_tsc;
        u64 ep  = d->epoch_base_ns;
        __asm__ volatile("" ::: "memory");
        if (d->seq != s0) continue;
        if (khz == 0) return -1;
        u64 mono = (rdtsc_ordered() - bt) * 1000000ull / khz;
        switch (clk) {
        case CLK_MONOTONIC:
        case CLK_MONOTONIC_RAW:
        case CLK_MONOTONIC_COARSE:
        case CLK_BOOTTIME:
            *out = mono;
            return 0;
        case CLK_REALTIME:
        case CLK_REALTIME_COARSE:
            *out = mono + ep;
            return 0;
        default:
            return -1;
        }
    }
    return -1;   /* writer wedged mid-update: the syscall is always right */
}

int __vdso_clock_gettime(int clk, struct ts *t) {
    u64 ns;
    if (read_clock(clk, &ns) != 0)
        return (int)sys3(228 /* clock_gettime */, clk, (long)t, 0);
    t->sec  = (i64)(ns / 1000000000ull);
    t->nsec = (i64)(ns % 1000000000ull);
    return 0;
}

int __vdso_gettimeofday(struct tv *v, void *tz) {
    if (v) {
        u64 ns;
        if (read_clock(CLK_REALTIME, &ns) != 0)
            return (int)sys3(96 /* gettimeofday */, (long)v, (long)tz, 0);
        v->sec  = (i64)(ns / 1000000000ull);
        v->usec = (i64)((ns % 1000000000ull) / 1000ull);
    }
    if (tz) {
        ((i32 *)tz)[0] = 0;   /* tz_minuteswest */
        ((i32 *)tz)[1] = 0;   /* tz_dsttime     */
    }
    return 0;
}

i64 __vdso_time(i64 *t) {
    u64 ns;
    if (read_clock(CLK_REALTIME, &ns) != 0)
        return sys3(201 /* time */, (long)t, 0, 0);
    i64 s = (i64)(ns / 1000000000ull);
    if (t) *t = s;
    return s;
}

long __vdso_getcpu(u32 *cpu, u32 *node, void *unused) {
    (void)unused;
    /* No per-cpu data is exported to userspace; the syscall keeps its
     * exact pre-vDSO behaviour rather than fabricating cpu 0. */
    return sys3(309 /* getcpu */, (long)cpu, (long)node, 0);
}

/* Linux exports each function under both names, same version. */
int clock_gettime(int, struct ts *)
    __attribute__((weak, alias("__vdso_clock_gettime")));
int gettimeofday(struct tv *, void *)
    __attribute__((weak, alias("__vdso_gettimeofday")));
i64 time(i64 *)
    __attribute__((weak, alias("__vdso_time")));
long getcpu(u32 *, u32 *, void *)
    __attribute__((weak, alias("__vdso_getcpu")));
