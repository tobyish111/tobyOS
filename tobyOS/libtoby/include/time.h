/* time.h -- libtoby's clock and calendar surface.
 *
 * time_t is seconds since 1970-01-01 00:00:00 UTC, the same clock the
 * kernel stamps files with and the Linux personality reports. (It was
 * seconds since BOOT until 2026-09-03; see src/time.c for the story.)
 * clock_gettime() has nanosecond resolution from ABI_SYS_CLOCK_NS.
 * There are no time zones: localtime is gmtime. */

#ifndef LIBTOBY_TIME_H
#define LIBTOBY_TIME_H

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCKS_PER_SEC  1000

/* Linux's numbers, so ported code that hardcodes them keeps working. */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7

#define TIME_UTC 1

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
int        clock_getres(int clk, struct timespec *res);
double     difftime(time_t end, time_t start);
struct tm *gmtime(const time_t *tp);
struct tm *gmtime_r(const time_t *tp, struct tm *result);
struct tm *localtime(const time_t *tp);
struct tm *localtime_r(const time_t *tp, struct tm *result);
time_t     mktime(struct tm *tm);
time_t     timegm(struct tm *tm);
size_t     strftime(char *s, size_t maxsize, const char *fmt,
                    const struct tm *tm);
char      *asctime(const struct tm *tm);
char      *asctime_r(const struct tm *tm, char *buf);
char      *ctime(const time_t *tp);
char      *ctime_r(const time_t *tp, char *buf);
void       tzset(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_TIME_H */
