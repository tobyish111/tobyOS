/* programs/tjoinchurn/main.c -- /bin/tjoinchurn
 *
 * Churn tobyOS's NATIVE thread join/detach paths, hard enough and for long
 * enough that a hold-and-wait deadlock in them shows up as a wedged box.
 *
 * WHY THIS EXISTS. proc_reap() used to spin on a dead child's on_cpu flag
 * while holding the BKL, which deadlocked the EliteDesk within a handful of
 * forks (commit 158cb7b). Auditing the rest of that primitive's callers
 * found the identical bug in three more places -- thread_join reaps a
 * terminated thread in two branches and thread_detach in a third -- and
 * 4c87a64 fixed all of them by making proc_wait_off_cpu() yield rather than
 * spin when it holds the lock.
 *
 * Those three had NO TEST. /bin/linux-join looks like it covers this and
 * does not: it is a Linux binary using the futex contract
 * (CLONE_CHILD_CLEARTID + FUTEX_WAIT), which is what glibc's pthread_join
 * rides and never enters thread.c at all. The native path is reached only
 * through libtoby's pthread_join -> ABI_SYS_THREAD_JOIN (109) and
 * pthread_detach -> ABI_SYS_THREAD_DETACH (112), and nothing in the tree
 * called either. So the fix for three of the four sites was argued, not
 * measured -- this closes that.
 *
 * WHAT IT AIMS AT. The dangerous branch is the one that reaps a thread
 * which has ALREADY terminated, because it reaches proc_wait_off_cpu()
 * immediately, while the corpse may still be loaded on another core:
 *
 *   thread_join   -> target->state == PROC_TERMINATED  -> reap at once
 *   thread_join   -> block, wake, then reap
 *   thread_detach -> target->state == PROC_TERMINATED  -> reap at once
 *
 * So the threads here do as little as possible and exit immediately, and
 * the joiner deliberately dawdles before joining -- maximising the odds
 * that the target is TERMINATED-but-still-on_cpu at the moment of the reap,
 * which is exactly the race that needs two real cores to lose.
 *
 * Progress prints every PROGRESS_EVERY rounds and is flushed, so a hang is
 * visible as output simply stopping. On a stall the kernel's watchdog bites
 * ten seconds later and prints the BKL picture (see bkl_dump_stall).
 *
 *   tjoinchurn [rounds]      default 250; 0 means run until killed
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#define THREADS_PER_ROUND  4
#define PROGRESS_EVERY     100

static volatile unsigned long g_ran;

static void *worker(void *arg) {
    /* Deliberately trivial: exit as fast as possible, so the joiner is
     * most likely to arrive while the corpse is still on_cpu elsewhere. */
    __atomic_fetch_add(&g_ran, 1, __ATOMIC_RELAXED);
    return arg;
}

/* A short spin, NOT a sleep: a sleep parks this thread and lets the other
 * core finish switching away, which is precisely the window we are trying
 * to still be inside when we call join. */
static void dawdle(void) {
    for (volatile int i = 0; i < 2000; i++) { }
}

int main(int argc, char **argv) {
    /* 250 rounds is 1000 threads: seven times past where the proc-slot
     * leak used to stop the run, and short enough to finish against a
     * 38400-baud console that prints three lines per thread. Pass a bigger
     * number for a soak; 0 runs until killed. */
    long rounds = (argc > 1) ? atol(argv[1]) : 250;
    int  forever = (rounds == 0);

    printf("[tjoinchurn] native thread join/detach churn, rounds=%s\n",
           forever ? "forever" : argv[1] ? argv[1] : "250");
    fflush(stdout);

    unsigned long joined = 0, detached = 0, failed = 0;

    for (long r = 0; forever || r < rounds; r++) {
        pthread_t t[THREADS_PER_ROUND];
        int made = 0;

        for (int i = 0; i < THREADS_PER_ROUND; i++) {
            if (pthread_create(&t[i], 0, worker, (void *)(long)i) == 0) made++;
            else { failed++; break; }
        }

        /* Let them die first, so the joins below take the
         * already-TERMINATED branch rather than the blocking one. */
        dawdle();

        for (int i = 0; i < made; i++) {
            /* Alternate the two reap paths: most rounds join (and so also
             * collect the exit code), every fourth detaches instead, which
             * is the third call site and is otherwise never exercised. */
            if ((r & 3) == 3) {
                if (pthread_detach(t[i]) == 0) detached++; else failed++;
            } else {
                void *rv = 0;
                if (pthread_join(t[i], &rv) == 0) joined++; else failed++;
            }
        }

        if (((r + 1) % PROGRESS_EVERY) == 0) {
            printf("[tjoinchurn] round=%ld ran=%lu joined=%lu detached=%lu "
                   "failed=%lu\n", r + 1,
                   __atomic_load_n(&g_ran, __ATOMIC_RELAXED),
                   joined, detached, failed);
            fflush(stdout);
        }
    }

    unsigned long ran = __atomic_load_n(&g_ran, __ATOMIC_RELAXED);
    printf("[tjoinchurn] DONE ran=%lu joined=%lu detached=%lu failed=%lu\n",
           ran, joined, detached, failed);

    /* WHAT CAN HONESTLY BE ASSERTED.
     *
     * No create, join or detach may fail -- that is the whole point, and it
     * is what caught both leaks: the kernel running out of proc slots and
     * libtoby running out of heap both surface here as a create failure.
     *
     * And every JOINED thread must have run, because joining it proves the
     * kernel saw it terminate.
     *
     * `ran == joined + detached` is NOT assertable, and the first version
     * of this asserted it anyway. A detached thread has no joiner by
     * definition, so nothing makes it run before main returns. A real run
     * on hardware reported ran=797 joined=600 detached=200 failed=0 and was
     * called a FAIL: the last round detaches, and three of its four threads
     * had not reached their counter yet. That is correct POSIX behaviour,
     * and the test was wrong about it. The shortfall is reported below
     * rather than hidden, since a LARGE one would mean something else. */
    int ok = (failed == 0) && (ran >= joined);
    if (ran < joined + detached)
        printf("[tjoinchurn] (%lu detached thread(s) had not run at exit -- "
               "expected, they have no joiner)\n",
               joined + detached - ran);
    printf("[tjoinchurn] VERDICT: %s\n", ok ? "PASS" : "FAIL");
    fflush(stdout);
    return ok ? 0 : 1;
}
