/* libtoby/include/sys/time.h -- gettimeofday + struct timeval.
 * Added for the QuickJS port (Date.now / Math.random seeding).
 * Implemented in src/time.c over clock_gettime(CLOCK_REALTIME). */

#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timeval {
    time_t      tv_sec;
    long        tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval *tv, struct timezone *tz);
int settimeofday(const struct timeval *tv, const struct timezone *tz);   /* root only */

/* adjtime(3): slew the clock by DELTA (root only), the adjustment still
 * outstanding before it in OLDDELTA; DELTA null only reads that. Over
 * adjtimex's single-shot mode, as glibc's (finding 280). */
int adjtime(const struct timeval *delta, struct timeval *olddelta);

/* utimes(2). times[0] = atime, times[1] = mtime; NULL means "now", which
 * is touch(1)'s default and the only form it needs. Added 2026-08-24 --
 * before it, the native ABI had no way to set a timestamp at all, so
 * `touch existingfile` opened and closed the file and changed nothing. */
int utimes(const char *path, const struct timeval times[2]);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_TIME_H */
