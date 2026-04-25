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
