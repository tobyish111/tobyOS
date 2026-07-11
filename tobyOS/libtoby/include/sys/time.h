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

#ifdef __cplusplus
}
#endif

#endif /* _SYS_TIME_H */
