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
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    /* Touch argv so the compiler keeps the packed strings live and we actually
     * dereference the user-stack argv pointers (the region the AP-first-run
     * SMEP fault was reported to land in -- keep exercising it). */
    volatile unsigned long sink = (unsigned long)argc;
    for (int a = 0; a < argc; a++) {
        const char *s = argv[a];
        while (s && *s) { sink += (unsigned char)*s; s++; }
    }

    /* Interactive mode (argv[1]=="int", argv[2]=window ms): the scheduler-
     * maturity io_boost proof. Repeatedly do a sliver of work then block in
     * usleep; measure how much longer each wake took than the requested sleep
     * -- that excess is the wake-to-run scheduling latency. Under CPU-bound
     * contention an interactive task with the io_boost should preempt the hogs
     * on wakeup, keeping average latency small; without it the wake would queue
     * behind a full quantum per competing hog. */
    if (argc >= 3 && strcmp(argv[1], "int") == 0) {
        long window = atol(argv[2]);
        if (window <= 0) window = 1500;
        const long nap = 5;                  /* requested sleep per cycle (ms) */
        long start = (long)clock();
        unsigned long cycles = 0, lat_sum = 0, lat_max = 0;
        volatile unsigned long x = sink;
        while (((long)clock() - start) < window) {
            for (int k = 0; k < 20000; k++) { x += k * 2654435761UL; x ^= x >> 13; }
            long t0 = (long)clock();
            usleep((unsigned)(nap * 1000));
            long elapsed = (long)clock() - t0;
            long lat = elapsed - nap;         /* scheduling delay beyond the nap */
            if (lat < 0) lat = 0;
            lat_sum += (unsigned long)lat;
            if ((unsigned long)lat > lat_max) lat_max = (unsigned long)lat;
            cycles++;
        }
        unsigned long avg = cycles ? lat_sum / cycles : 0;
        printf("MCTEST: interactive cycles=%lu avg_lat=%lums max_lat=%lums cksum=%lu\n",
               cycles, avg, lat_max, (unsigned long)x);
        /* Return avg wake latency (ms, capped) as the exit code so the boot
         * harness can read it cleanly via proc_wait_info -- serial printf from
         * 9 concurrent procs interleaves and can't be parsed reliably. */
        return (int)(avg > 250 ? 250 : avg);
    }

    /* Pipe-blocking interactivity proof (argv[1]=="pipeint", argv[2]=window
     * ms): the real io_boost test. We fork a feeder that mostly sleeps and,
     * every ~15 ms, writes the current clock into a pipe; the reader BLOCKS in
     * read() (-> PROC_BLOCKED, genuinely leaving its core, unlike a nanosleep
     * that hlt-retains the core) and on each wakeup computes how long it took
     * to run after the byte was sent -- the true wake-to-run latency while CPU
     * hogs saturate the cores. With io_boost the woken reader out-ranks the
     * NORMAL hogs at the next scheduling point; without it the reader waits its
     * FIFO turn behind them. Returns avg latency (ms, capped) as the exit code
     * so the boot harness reads it cleanly. */
    if (argc >= 3 && strcmp(argv[1], "pipeint") == 0) {
        long window = atol(argv[2]);
        if (window <= 0) window = 1500;
        int fds[2];
        if (pipe(fds) != 0) { printf("MCTEST: pipeint pipe FAIL\n"); return 255; }
        int pid = fork();
        if (pid == 0) {
            close(fds[0]);                       /* feeder: write end only */
            long start = (long)clock();
            while (((long)clock() - start) < window) {
                usleep(15000);                   /* pace; feeder ~idle */
                long t = (long)clock();
                if (write(fds[1], &t, sizeof(t)) != (long)sizeof(t)) break;
            }
            close(fds[1]);
            _exit(0);
        }
        if (pid < 0) { printf("MCTEST: pipeint fork FAIL\n"); return 254; }
        close(fds[1]);                           /* reader: read end only */
        unsigned long n = 0, sum = 0, mx = 0;
        for (;;) {
            long t; char *p = (char *)&t; size_t got = 0;
            while (got < sizeof(t)) {            /* read() BLOCKS when empty */
                long r = read(fds[0], p + got, sizeof(t) - got);
                if (r <= 0) { got = 0; break; }
                got += (size_t)r;
            }
            if (got != sizeof(t)) break;          /* feeder closed -> done */
            long lat = (long)clock() - t;
            if (lat < 0) lat = 0;
            sum += (unsigned long)lat;
            if ((unsigned long)lat > mx) mx = (unsigned long)lat;
            n++;
        }
        close(fds[0]);
        waitpid(pid, 0, 0);
        unsigned long avg = n ? sum / n : 0;
        printf("MCTEST: pipeint cycles=%lu avg_lat=%lums max_lat=%lums\n",
               n, avg, mx);
        return (int)(avg > 250 ? 250 : avg);
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
