/* libtoby/include/sys/timex.h -- adjtimex(2), the kernel's clock
 * discipline: struct timex in Linux's x86-64 layout and the mode and
 * status bits a time service sets. An offset is SLEWED at 500 ppm, never
 * stepped; ADJ_SETOFFSET steps (finding 280). */

#ifndef _SYS_TIMEX_H
#define _SYS_TIMEX_H

#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timex {
    int    modes;
    long   offset;
    long   freq;
    long   maxerror;
    long   esterror;
    int    status;
    long   constant;
    long   precision;
    long   tolerance;
    struct timeval time;
    long   tick;
    long   ppsfreq;
    long   jitter;
    int    shift;
    long   stabil;
    long   jitcnt;
    long   calcnt;
    long   errcnt;
    long   stbcnt;
    int    tai;
    int    reserved[11];
};

#define ADJ_OFFSET            0x0001
#define ADJ_FREQUENCY         0x0002
#define ADJ_MAXERROR          0x0004
#define ADJ_ESTERROR          0x0008
#define ADJ_STATUS            0x0010
#define ADJ_TIMECONST         0x0020
#define ADJ_TAI               0x0080
#define ADJ_SETOFFSET         0x0100
#define ADJ_MICRO             0x1000
#define ADJ_NANO              0x2000
#define ADJ_TICK              0x4000
#define ADJ_OFFSET_SINGLESHOT 0x8001
#define ADJ_OFFSET_SS_READ    0xa001

#define STA_PLL       0x0001
#define STA_UNSYNC    0x0040
#define STA_NANO      0x2000

#define TIME_OK       0
#define TIME_ERROR    5

int adjtimex(struct timex *tx);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_TIMEX_H */
