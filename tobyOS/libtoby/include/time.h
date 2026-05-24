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

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

clock_t    clock(void);
time_t     time(time_t *t);
int        nanosleep(const struct timespec *req, struct timespec *rem);
int        clock_gettime(int clk, struct timespec *ts);
double     difftime(time_t end, time_t start);
struct tm *gmtime(const time_t *tp);
struct tm *gmtime_r(const time_t *tp, struct tm *result);
struct tm *localtime(const time_t *tp);
struct tm *localtime_r(const time_t *tp, struct tm *result);
time_t     mktime(struct tm *tm);
size_t     strftime(char *s, size_t maxsize, const char *fmt,
                    const struct tm *tm);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_TIME_H */
