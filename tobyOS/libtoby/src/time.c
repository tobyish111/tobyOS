/* libtoby/src/time.c -- the clock, and the calendar on top of it.
 *
 * The kernel exposes three things:
 *   ABI_SYS_CLOCK_NS(clk)  -> nanoseconds: CLOCK_REALTIME since 1970-01-01
 *                             UTC (RTC-anchored, TSC-driven), or
 *                             CLOCK_MONOTONIC since boot
 *   ABI_SYS_CLOCK_MS()     -> milliseconds since boot (the old surface)
 *   ABI_SYS_NANOSLEEP(ns)  -> 0
 *
 * HISTORY, because the previous version of this file lied confidently.
 * Until 2026-09-03 time_t here was SECONDS SINCE BOOT, and gmtime_r faked
 * a 2025-01-01 base "so that strftime output looks plausible". On the real
 * EliteDesk, `date` printed `Wed Jan 1 00:01:11 UTC 2025` seventy-one
 * seconds after a boot whose RTC read 2026-09-01 -- while `busybox date`
 * on the same machine was right, because the Linux personality had a
 * realtime clock and the native ABI did not. The native ABI has one now,
 * and everything below is 1970-based through <tobyos/civiltime.h>, the
 * same calendar the shell's printf %(fmt)T uses.
 *
 * There are no time zones: localtime == gmtime, %Z is "UTC", %z "+0000". */

#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <errno.h>
#include "libtoby_internal.h"
#include <tobyos/civiltime.h>

/* ---- the clock ----------------------------------------------------- */

/* A negative answer means an older kernel without ABI_SYS_CLOCK_NS
 * (-ENOSYS); fall back to the millisecond uptime so a program still runs
 * on it, with the old (wrong) epoch rather than no clock at all. */
static uint64_t clock_ns(int clk) {
    long r = toby_sc1(ABI_SYS_CLOCK_NS, (long)clk);
    if (r >= 0) return (uint64_t)r;
    return (uint64_t)toby_sc0(ABI_SYS_CLOCK_MS) * 1000000ull;
}

static uint64_t realtime_ns(void)  { return clock_ns(ABI_CLOCK_REALTIME); }
static uint64_t monotonic_ns(void) { return clock_ns(ABI_CLOCK_MONOTONIC); }

/* clock(): CPU time of this process, approximated as wall time since the
 * first call (there is no per-process CPU accounting to ask for). Units are
 * CLOCKS_PER_SEC = 1000, i.e. milliseconds, which is what unported Unix code
 * expects of a "monotonic tick counter". */
static uint64_t g_clock_base_ns;
static int      g_clock_based;

clock_t clock(void) {
    uint64_t now = monotonic_ns();
    if (!g_clock_based) { g_clock_base_ns = now; g_clock_based = 1; }
    return (clock_t)((now - g_clock_base_ns) / 1000000ull);
}

time_t time(time_t *t) {
    time_t secs = (time_t)(realtime_ns() / 1000000000ull);
    if (t) *t = secs;
    return secs;
}

int clock_gettime(int clk, struct timespec *ts) {
    if (!ts) { errno = EFAULT; return -1; }
    uint64_t ns;
    switch (clk) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        ns = realtime_ns();
        break;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
        ns = monotonic_ns();
        break;
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
        /* same approximation clock() makes */
        ns = monotonic_ns();
        if (!g_clock_based) { g_clock_base_ns = ns; g_clock_based = 1; }
        ns -= g_clock_base_ns;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    ts->tv_sec  = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

int clock_getres(int clk, struct timespec *res) {
    struct timespec probe;
    if (clock_gettime(clk, &probe) != 0) return -1;   /* sets EINVAL */
    if (res) { res->tv_sec = 0; res->tv_nsec = 1; }
    return 0;
}

int gettimeofday(struct timeval *tv, struct timezone *tz) {
    if (tz) { tz->tz_minuteswest = 0; tz->tz_dsttime = 0; }
    if (tv) {
        uint64_t ns = realtime_ns();
        tv->tv_sec  = (time_t)(ns / 1000000000ull);
        tv->tv_usec = (long)(ns % 1000000000ull / 1000ull);
    }
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req) return 0;
    uint64_t ns = (uint64_t)req->tv_sec * 1000000000ull
                + (uint64_t)req->tv_nsec;
    toby_sc1(ABI_SYS_NANOSLEEP, (long)ns);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

double difftime(time_t end, time_t start) {
    return (double)(end - start);
}

void tzset(void) { /* UTC, always */ }

/* ---- the calendar ---------------------------------------------------- */

static struct tm g_tm;          /* the static result of gmtime/localtime */
static char      g_asc[32];     /* the static result of asctime/ctime */

static void civil_to_tm(const struct civil_time *ct, struct tm *tm) {
    tm->tm_sec   = ct->sec;
    tm->tm_min   = ct->min;
    tm->tm_hour  = ct->hour;
    tm->tm_mday  = ct->mday;
    tm->tm_mon   = ct->mon;
    tm->tm_year  = ct->year - 1900;
    tm->tm_wday  = ct->wday;
    tm->tm_yday  = ct->yday;
    tm->tm_isdst = 0;
}

static void tm_to_civil(const struct tm *tm, struct civil_time *ct) {
    ct->sec  = tm->tm_sec;
    ct->min  = tm->tm_min;
    ct->hour = tm->tm_hour;
    ct->mday = tm->tm_mday;
    ct->mon  = tm->tm_mon;
    ct->year = tm->tm_year + 1900;
    ct->wday = tm->tm_wday;
    ct->yday = tm->tm_yday;
}

struct tm *gmtime_r(const time_t *tp, struct tm *result) {
    if (!tp || !result) return 0;
    struct civil_time ct;
    civil_from_unix((int64_t)*tp, &ct);
    civil_to_tm(&ct, result);
    return result;
}

struct tm *gmtime(const time_t *tp)                       { return gmtime_r(tp, &g_tm); }
struct tm *localtime_r(const time_t *tp, struct tm *result) { return gmtime_r(tp, result); }
struct tm *localtime(const time_t *tp)                    { return gmtime(tp); }

/* mktime normalises its argument in place: out-of-range fields are carried
 * and tm_wday/tm_yday are recomputed, which is the part callers that step a
 * date forward with `tm_mday += 7` depend on. */
time_t timegm(struct tm *tm) {
    if (!tm) return (time_t)-1;
    struct civil_time ct;
    tm_to_civil(tm, &ct);
    int64_t t = civil_to_unix(&ct);
    civil_from_unix(t, &ct);
    civil_to_tm(&ct, tm);
    return (time_t)t;
}

time_t mktime(struct tm *tm) { return timegm(tm); }

size_t strftime(char *s, size_t maxsize, const char *fmt, const struct tm *tm) {
    if (!s || maxsize == 0 || !fmt || !tm) return 0;
    struct civil_time ct;
    tm_to_civil(tm, &ct);
    return civil_strftime(s, maxsize, fmt, &ct);
}

/* "Sun Sep 16 01:03:52 1973\n" -- 26 bytes including the NUL. */
char *asctime_r(const struct tm *tm, char *buf) {
    if (!tm || !buf) return 0;
    struct civil_time ct;
    tm_to_civil(tm, &ct);
    if (civil_strftime(buf, 26, "%a %b %e %H:%M:%S %Y\n", &ct) == 0) return 0;
    return buf;
}

char *asctime(const struct tm *tm) { return asctime_r(tm, g_asc); }

char *ctime_r(const time_t *tp, char *buf) {
    struct tm tm;
    if (!gmtime_r(tp, &tm)) return 0;
    return asctime_r(&tm, buf);
}

char *ctime(const time_t *tp) { return ctime_r(tp, g_asc); }

/* ---- utimes (2026-08-24) ------------------------------------------- *
 *
 * The kernel takes epoch SECONDS and reads (uint64_t)-1 as "now", so a
 * NULL `times` needs no clock read here. Sub-second precision is dropped:
 * struct vfs_stat stores whole seconds, and rounding silently would be
 * worse than not offering what cannot be stored. */

int utimes(const char *path, const struct timeval times[2])
{
    unsigned long long at = ~0ull, mt = ~0ull;   /* both mean "now" */
    if (times) {
        at = (unsigned long long)times[0].tv_sec;
        mt = (unsigned long long)times[1].tv_sec;
    }
    return (int)__toby_check(toby_sc3(ABI_SYS_UTIMES,
                                      (long)(uintptr_t)path,
                                      (long)mt, (long)at));
}
