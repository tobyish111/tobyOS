/* linux-cont -- resume latency of a woken-from-stop process
 * (2026-08-23; the job-control arc measured 29 ms expected vs 1.4 s
 * actual and filed it as the scheduler's open starvation item).
 *
 *   bit0  sanity: a plain 50 ms sleep measures ~50 ms (the clock and
 *         the scheduler agree about an ordinary wake)
 *   bit1  THE item: with busy-spinners occupying every core, a
 *         SIGSTOPped child that receives SIGCONT must actually RUN
 *         promptly. The child stamps a shared timestamp at resume; the
 *         parent busy-waits (userspace only -- no yielding syscalls)
 *         and measures cont -> stamp. Asserts < 250 ms: generous
 *         against the ~30 ms two-tick ideal, an order of magnitude
 *         under the measured starvation.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- bit0: ordinary wake sanity ---- */
    {
        long long t0 = now_ms();
        usleep(50000);
        long long dt = now_ms() - t0;
        printf("ct: sleep50 measured=%lldms\n", dt);
        if (dt >= 40 && dt < 500) bits |= 1;
    }

    volatile long long *shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) { printf("ct: no shm\n"); return bits; }
    shm[0] = 0;   /* child resume stamp */

    /* ---- bit1: resume latency under full-core spinner load ---- */
    {
        /* Four pure-userspace spinners: one per core, no syscalls in
         * the loop, so only tick preemption ever takes the CPU back. */
        pid_t spin[4];
        for (int i = 0; i < 4; i++) {
            spin[i] = fork();
            if (spin[i] == 0) {
                for (volatile unsigned long v = 0;; v++) { }
            }
        }

        pid_t k = fork();
        if (k == 0) {
            raise(SIGSTOP);          /* parent CONTs us; stamp on resume */
            shm[0] = now_ms();
            _exit(0);
        }
        usleep(100000);              /* let the child reach its STOP */

        long long t_cont = now_ms();
        kill(k, SIGCONT);
        /* Busy-wait WITHOUT syscalls: the parent is itself one of the
         * competing spinners, which is the shape the shell had. */
        long long stamp = 0, deadline = t_cont + 5000;
        while ((stamp = shm[0]) == 0 && now_ms() < deadline) { }

        long long lat = stamp ? stamp - t_cont : -1;
        printf("ct: cont->run latency=%lldms (want <250)\n", lat);
        if (stamp && lat < 250) bits |= 2;

        for (int i = 0; i < 4; i++)
            if (spin[i] > 0) kill(spin[i], SIGKILL);
        int st;
        while (wait(&st) > 0) { }
    }

    printf("LXCONT: VERDICT bits=%d (3=all)\n", bits);
    return bits;
}
