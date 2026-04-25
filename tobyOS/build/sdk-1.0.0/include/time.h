/* time.h -- libtoby's clock surface.
 *
 * Minimal -- just the pieces our sample programs and the future ports
 * (M25E) actually exercise. clock_gettime() is implemented in terms
 * of SYS_CLOCK_MS so it has millisecond resolution, which is plenty
 * for "sleep N seconds and check the clock advanced". */

#ifndef LIBTOBY_TIME_H
#define LIBTOBY_TIME_H

#include <sys/types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCKS_PER_SEC  1000

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

clock_t clock(void);
time_t  time(time_t *t);
int     nanosleep(const struct timespec *req, struct timespec *rem);
int     clock_gettime(int clk, struct timespec *ts);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_TIME_H */
