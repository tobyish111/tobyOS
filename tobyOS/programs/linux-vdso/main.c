/* linux-vdso -- the userspace clock is real (2026-08-23).
 *
 *   bit0  AT_SYSINFO_EHDR is present and points at a mapped ELF image
 *   bit1  THE point of the exercise: 10,000 clock_gettime calls move the
 *         kernel's syscall counter (TobySyscalls in /proc/self/status,
 *         added for exactly this proof) by almost nothing
 *   bit2  vDSO reads and FORCED syscall reads interleave monotonically --
 *         the two compute the same function of the same TSC
 *   bit3  CLOCK_REALTIME agrees with gettimeofday, and the
 *         realtime-minus-monotonic offset is stable
 *   bit4  time(), MONOTONIC_RAW and both COARSE clocks are coherent
 *   bit5  the clock advances at wall rate (a 100 ms sleep measures >=80 ms)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/auxv.h>
#include <sys/syscall.h>

static long long read_syscall_count(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[128];
    long long v = -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "TobySyscalls: %lld", &v) == 1) break;
    fclose(f);
    return v;
}

static long long ns_of(const struct timespec *t) {
    return (long long)t->tv_sec * 1000000000ll + t->tv_nsec;
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- bit0: auxv + ELF magic at the advertised base ---- */
    unsigned long base = getauxval(AT_SYSINFO_EHDR);
    {
        int magic = 0;
        if (base) {
            const unsigned char *b = (const unsigned char *)base;
            magic = (b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' &&
                     b[3] == 'F');
        }
        printf("vd: auxv base=0x%lx elf=%d\n", base, magic);
        if (base && magic) bits |= 1;
    }

    /* ---- data-page sanity (address-stable: one page below base) ---- */
    if (base) {
        const volatile unsigned *vv =
            (const volatile unsigned *)(base - 4096);
        printf("vd: vvar seq=%u khz=%u\n", vv[0], vv[1]);
    }

    /* ---- bit1: clock_gettime stopped syscalling ---- */
    {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);   /* warm any glibc probing */
        long long c0 = read_syscall_count();
        for (int i = 0; i < 10000; i++)
            clock_gettime(CLOCK_MONOTONIC, &t);
        long long c1 = read_syscall_count();
        printf("vd: 10000 calls cost %lld syscalls (counter %lld->%lld)\n",
               c1 - c0, c0, c1);
        if (c0 >= 0 && c1 >= 0 && (c1 - c0) < 100) bits |= 2;
    }

    /* ---- bit2: vDSO and forced-syscall reads interleave monotonically ---- */
    {
        int mono_ok = 1;
        long long prev = 0;
        for (int i = 0; i < 1000 && mono_ok; i++) {
            struct timespec a, b;
            clock_gettime(CLOCK_MONOTONIC, &a);              /* vDSO    */
            syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &b); /* kernel  */
            long long na = ns_of(&a), nb = ns_of(&b);
            if (na < prev || nb < na) mono_ok = 0;
            prev = nb;
        }
        printf("vd: interleave monotonic=%d\n", mono_ok);
        if (mono_ok) bits |= 4;
    }

    /* ---- bit3: REALTIME vs gettimeofday + stable offset ---- */
    {
        struct timespec rt, mo, rt2, mo2;
        struct timeval tv;
        clock_gettime(CLOCK_REALTIME, &rt);
        gettimeofday(&tv, 0);
        clock_gettime(CLOCK_MONOTONIC, &mo);
        long long rtns = ns_of(&rt);
        long long tvns = (long long)tv.tv_sec * 1000000000ll +
                         (long long)tv.tv_usec * 1000ll;
        long long d1 = tvns - rtns;
        if (d1 < 0) d1 = -d1;
        long long off1 = rtns - ns_of(&mo);
        usleep(50000);
        clock_gettime(CLOCK_REALTIME, &rt2);
        clock_gettime(CLOCK_MONOTONIC, &mo2);
        long long off2 = ns_of(&rt2) - ns_of(&mo2);
        long long d2 = off2 - off1;
        if (d2 < 0) d2 = -d2;
        printf("vd: rt-vs-tod=%lldus offset-drift=%lldus\n",
               d1 / 1000, d2 / 1000);
        if (d1 < 50000000ll && d2 < 50000000ll) bits |= 8;
    }

    /* ---- bit4: time() + RAW + COARSE coherence ---- */
    {
        struct timespec rt, raw, com, cor;
        clock_gettime(CLOCK_REALTIME, &rt);
        time_t tt = time(0);
        int r1 = clock_gettime(CLOCK_MONOTONIC_RAW, &raw);
        int r2 = clock_gettime(CLOCK_MONOTONIC_COARSE, &com);
        int r3 = clock_gettime(CLOCK_REALTIME_COARSE, &cor);
        struct timespec mo;
        clock_gettime(CLOCK_MONOTONIC, &mo);
        long long dt = (long long)tt - (long long)rt.tv_sec;
        if (dt < 0) dt = -dt;
        long long draw = ns_of(&mo) - ns_of(&raw);
        if (draw < 0) draw = -draw;
        long long dcor = ns_of(&cor) - ns_of(&rt);
        if (dcor < 0) dcor = -dcor;
        printf("vd: time-dt=%llds raw-dt=%lldus rtc-dt=%lldus rc=%d/%d/%d\n",
               dt, draw / 1000, dcor / 1000, r1, r2, r3);
        if (dt <= 1 && r1 == 0 && r2 == 0 && r3 == 0 &&
            draw < 100000000ll && dcor < 100000000ll &&
            ns_of(&com) > 0)
            bits |= 16;
    }

    /* ---- bit5: wall-rate advance ---- */
    {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        usleep(100000);
        clock_gettime(CLOCK_MONOTONIC, &b);
        long long d = ns_of(&b) - ns_of(&a);
        printf("vd: slept 100ms measured %lldms\n", d / 1000000);
        if (d >= 80000000ll && d < 10000000000ll) bits |= 32;
    }

    printf("LXVDSO: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
