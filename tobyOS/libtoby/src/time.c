/* libtoby/src/time.c -- minimal clock + sleep surface.
 *
 * The kernel exposes:
 *   SYS_NANOSLEEP(nsec)   -> 0
 *   SYS_CLOCK_MS()        -> milliseconds since boot
 *
 * Everything in this file is composed from those two. time() returns
 * seconds-since-boot today (we have no wall clock); when M25E adds
 * an RTC sync this is the spot to swap the underlying source.
 *
 * clock() returns milliseconds since boot of *this* process. We
 * approximate by seeding from CLOCK_MS at first call (lazy init);
 * this is wrong if the process is preempted by other CPU heavy work,
 * but matches what unported Unix code expects of clock() ("monotonic
 * tick counter, units = CLOCKS_PER_SEC"). */

#include <time.h>
#include "libtoby_internal.h"

static long g_clock_base_ms = -1;

clock_t clock(void) {
    long now = toby_sc0(ABI_SYS_CLOCK_MS);
    if (g_clock_base_ms < 0) g_clock_base_ms = now;
    return (clock_t)(now - g_clock_base_ms);
}

time_t time(time_t *t) {
    long ms = toby_sc0(ABI_SYS_CLOCK_MS);
    time_t secs = (time_t)(ms / 1000);
    if (t) *t = secs;
    return secs;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req) return 0;
    uint64_t ns = (uint64_t)req->tv_sec * 1000000000ull
                + (uint64_t)req->tv_nsec;
    toby_sc1(ABI_SYS_NANOSLEEP, (long)ns);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

int clock_gettime(int clk, struct timespec *ts) {
    (void)clk;
    if (!ts) return -1;
    long ms = toby_sc0(ABI_SYS_CLOCK_MS);
    ts->tv_sec  = ms / 1000;
    ts->tv_nsec = (long)(ms % 1000) * 1000000L;
    return 0;
}

double difftime(time_t end, time_t start) {
    return (double)(end - start);
}

/* ---- gmtime / localtime ----------------------------------------- *
 *
 * tobyOS has no timezone support; localtime == gmtime. We use a single
 * static struct tm (not thread-safe, but matches classic POSIX). The
 * epoch is seconds-since-boot, not Unix epoch; we pretend it's
 * 2025-01-01 00:00:00 UTC as a base so that strftime output looks
 * plausible. */

#include <string.h>

static struct tm g_tm;

static int is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static const int days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static int dim(int mon, int year) {
    if (mon == 1 && is_leap(year)) return 29;
    return days_in_month[mon];
}

struct tm *gmtime_r(const time_t *tp, struct tm *result) {
    if (!tp || !result) return 0;

    /* Base: 2025-01-01 00:00:00 UTC = epoch offset from boot. */
    time_t t = *tp;

    int sec  = (int)(t % 60); t /= 60;
    int min  = (int)(t % 60); t /= 60;
    int hour = (int)(t % 24); t /= 24;
    /* t is now days since base */

    /* Day of week: 2025-01-01 is a Wednesday (wday=3) */
    int wday = (int)((t + 3) % 7);
    if (wday < 0) wday += 7;

    int year = 2025;
    while (1) {
        int yd = is_leap(year) ? 366 : 365;
        if (t < yd) break;
        t -= yd;
        year++;
    }

    int yday = (int)t;
    int mon = 0;
    while (mon < 11 && t >= dim(mon, year)) {
        t -= dim(mon, year);
        mon++;
    }

    result->tm_sec   = sec;
    result->tm_min   = min;
    result->tm_hour  = hour;
    result->tm_mday  = (int)t + 1;
    result->tm_mon   = mon;
    result->tm_year  = year - 1900;
    result->tm_wday  = wday;
    result->tm_yday  = yday;
    result->tm_isdst = 0;
    return result;
}

struct tm *gmtime(const time_t *tp) {
    return gmtime_r(tp, &g_tm);
}

struct tm *localtime_r(const time_t *tp, struct tm *result) {
    return gmtime_r(tp, result);
}

struct tm *localtime(const time_t *tp) {
    return gmtime(tp);
}

time_t mktime(struct tm *tm) {
    if (!tm) return (time_t)-1;
    int year = tm->tm_year + 1900;
    int mon  = tm->tm_mon;
    int day  = tm->tm_mday - 1;

    time_t total = 0;
    for (int y = 2025; y < year; y++) {
        total += is_leap(y) ? 366 : 365;
    }
    for (int m = 0; m < mon; m++) {
        total += dim(m, year);
    }
    total += day;
    total = total * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
    return total;
}

/* ---- strftime --------------------------------------------------- */

static void emit_num(char **p, char *end, int val, int width) {
    char buf[16];
    int i = 0;
    int v = val < 0 ? -val : val;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i < width) buf[i++] = '0';
    for (int j = i - 1; j >= 0 && *p < end; j--)
        *(*p)++ = buf[j];
}

static void emit_str(char **p, char *end, const char *s) {
    while (*s && *p < end) *(*p)++ = *s++;
}

size_t strftime(char *s, size_t maxsize, const char *fmt,
                const struct tm *tm) {
    if (!s || maxsize == 0 || !fmt || !tm) return 0;

    static const char *wday_abbr[] = {
        "Sun","Mon","Tue","Wed","Thu","Fri","Sat"
    };
    static const char *mon_abbr[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };

    char *p = s;
    char *end = s + maxsize - 1;

    while (*fmt && p < end) {
        if (*fmt != '%') { *p++ = *fmt++; continue; }
        fmt++;
        switch (*fmt) {
        case 'Y': emit_num(&p, end, tm->tm_year + 1900, 4); break;
        case 'm': emit_num(&p, end, tm->tm_mon + 1, 2); break;
        case 'd': emit_num(&p, end, tm->tm_mday, 2); break;
        case 'H': emit_num(&p, end, tm->tm_hour, 2); break;
        case 'M': emit_num(&p, end, tm->tm_min, 2); break;
        case 'S': emit_num(&p, end, tm->tm_sec, 2); break;
        case 'j': emit_num(&p, end, tm->tm_yday + 1, 3); break;
        case 'a': emit_str(&p, end, wday_abbr[tm->tm_wday % 7]); break;
        case 'b': case 'h': emit_str(&p, end, mon_abbr[tm->tm_mon % 12]); break;
        case 'n': if (p < end) *p++ = '\n'; break;
        case 't': if (p < end) *p++ = '\t'; break;
        case '%': if (p < end) *p++ = '%'; break;
        case 'e':
            if (tm->tm_mday < 10) { if (p < end) *p++ = ' '; }
            emit_num(&p, end, tm->tm_mday, 1);
            break;
        case 'I': {
            int h = tm->tm_hour % 12;
            if (h == 0) h = 12;
            emit_num(&p, end, h, 2);
            break;
        }
        case 'p':
            emit_str(&p, end, tm->tm_hour < 12 ? "AM" : "PM");
            break;
        case 'Z':
            emit_str(&p, end, "UTC");
            break;
        case 'F': /* %Y-%m-%d */
            emit_num(&p, end, tm->tm_year + 1900, 4);
            if (p < end) *p++ = '-';
            emit_num(&p, end, tm->tm_mon + 1, 2);
            if (p < end) *p++ = '-';
            emit_num(&p, end, tm->tm_mday, 2);
            break;
        case 'T': /* %H:%M:%S */
            emit_num(&p, end, tm->tm_hour, 2);
            if (p < end) *p++ = ':';
            emit_num(&p, end, tm->tm_min, 2);
            if (p < end) *p++ = ':';
            emit_num(&p, end, tm->tm_sec, 2);
            break;
        default:
            if (p < end) *p++ = '%';
            if (p < end && *fmt) *p++ = *fmt;
            break;
        }
        if (*fmt) fmt++;
    }
    *p = '\0';
    return (size_t)(p - s);
}

/* ---- gettimeofday (M-JS: QuickJS Date.now / seeding) --------------- */

#include <sys/time.h>

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    (void)tz;
    if (tv) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        tv->tv_sec  = ts.tv_sec;
        tv->tv_usec = ts.tv_nsec / 1000;
    }
    return 0;
}
