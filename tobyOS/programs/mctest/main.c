/* programs/mctest/main.c -- CPU-bound worker for the multi-core tests.
 *
 * Spends time computing entirely in user mode (just a couple of syscalls: the
 * final write + exit, plus clock() polls in timed mode), so when several copies
 * run at once they parallelize across cores rather than serializing in the
 * kernel.
 *
 * Two modes:
 *   - legacy (no numeric argv[1]): run a FIXED amount of work then exit. Used by
 *     the MCTEST_BOOT parallelism benchmark and the MCARGV_BOOT SMEP repro
 *     (which passes non-numeric argv like "argone", so it stays in this mode).
 *   - timed (argv[1] is a positive integer = duration in ms): spin until the
 *     uptime clock has advanced that many ms, then exit. Used by the
 *     SCHEDPRIO_BOOT priority/timeslicing test: every worker runs the same
 *     WALL window, so a worker that the scheduler favours accumulates more CPU
 *     (higher cpu_ns + more iterations) than a deprioritised peer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv) {
    /* Touch argv so the compiler keeps the packed strings live and we actually
     * dereference the user-stack argv pointers (the region the AP-first-run
     * SMEP fault was reported to land in -- keep exercising it). */
    volatile unsigned long sink = (unsigned long)argc;
    for (int a = 0; a < argc; a++) {
        const char *s = argv[a];
        while (s && *s) { sink += (unsigned char)*s; s++; }
    }

    long dur_ms = (argc >= 2) ? atol(argv[1]) : 0;

    if (dur_ms > 0) {
        /* Timed mode: burn CPU until the wall clock advances dur_ms, counting
         * how many work units we completed -- a direct proxy for the CPU share
         * the scheduler gave us over the window. */
        unsigned long iters = 0;
        volatile unsigned long x = sink;
        long start = (long)clock();          /* ms since boot (CLOCKS_PER_SEC=1000) */
        while (((long)clock() - start) < dur_ms) {
            /* Coarse batch (~tens of ms of compute) so the clock() syscall is
             * polled only a few dozen times across the window -- keeps BKL /
             * syscall overhead negligible vs the CPU-share signal we measure. */
            for (int k = 0; k < 2000000; k++) {
                x += iters * 2654435761UL;
                x ^= (x >> 13);
                iters++;
            }
        }
        printf("MCTEST: timed worker pid done (dur=%ldms iters=%lu cksum=%lu)\n",
               dur_ms, iters, (unsigned long)x);
        return 0;
    }

    /* Legacy fixed-work mode. */
    volatile unsigned long x = sink;
    for (unsigned long i = 0; i < 400000000UL; i++) {
        x += i * 2654435761UL;
        x ^= (x >> 13);
    }
    printf("MCTEST: worker done (checksum=%lu)\n", (unsigned long)x);
    return 0;
}
