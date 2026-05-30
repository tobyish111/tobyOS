/* programs/sigtest/main.c -- user-space signal delivery regression test.
 *
 * Exercises the real kernel signal machinery added when handler delivery
 * went from "logged but ignored" to "frame pushed + sigreturn":
 *
 *   1. A SIGUSR1 handler actually runs, sees the right signum, and the
 *      interrupted code resumes afterwards (the canary is untouched).
 *   2. sigprocmask blocking holds a signal pending, and unblocking then
 *      delivers it (proves the mask + pending-at-syscall-return path).
 *   3. Registers are preserved across handler entry/return (a value held
 *      in a local across raise() survives -- catches a broken sigframe).
 *
 * Prints SIGTEST sentinels and returns 0 only if every check passes, so a
 * boot harness can `proc_wait` it and assert PASS. Run as `sigtest --boot`
 * for the same behaviour (the flag is accepted for symmetry with the other
 * boot harnesses; output is identical).
 */

#include <stdio.h>
#include <signal.h>
#include <string.h>

static volatile int g_usr1_count;
static volatile int g_usr1_signum;
static volatile int g_usr2_count;

static void on_usr1(int sig) {
    g_usr1_count++;
    g_usr1_signum = sig;
}

static void on_usr2(int sig) {
    (void)sig;
    g_usr2_count++;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int fails = 0;

    /* ---- Test 1: a caught handler runs and we resume after it ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    if (sigaction(SIGUSR1, &sa, 0) != 0) {
        printf("SIGTEST: sigaction(SIGUSR1) FAIL\n");
        fails++;
    }

    /* A value the handler must not be allowed to corrupt -- if the signal
     * frame mis-saves/restores callee state, this canary changes. */
    volatile unsigned canary = 0xC0FFEE42u;

    raise(SIGUSR1);   /* synchronous self-signal; handler runs at syscall ret */

    if (g_usr1_count != 1) {
        printf("SIGTEST: handler not invoked (count=%d) FAIL\n", g_usr1_count);
        fails++;
    } else {
        printf("SIGTEST: SIGUSR1 handler ran (count=1) OK\n");
    }
    if (g_usr1_signum != SIGUSR1) {
        printf("SIGTEST: handler saw wrong signum %d FAIL\n", g_usr1_signum);
        fails++;
    }
    if (canary != 0xC0FFEE42u) {
        printf("SIGTEST: canary clobbered 0x%x -- bad sigframe FAIL\n", canary);
        fails++;
    } else {
        printf("SIGTEST: context preserved across handler OK\n");
    }

    /* ---- Test 2: blocked signal stays pending until unblocked ---- */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr2;
    sigaction(SIGUSR2, &sa, 0);

    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    sigprocmask(SIG_BLOCK, &block, &old);

    raise(SIGUSR2);
    if (g_usr2_count != 0) {
        printf("SIGTEST: SIGUSR2 delivered while blocked FAIL\n");
        fails++;
    } else {
        printf("SIGTEST: SIGUSR2 held pending while blocked OK\n");
    }

    sigprocmask(SIG_SETMASK, &old, 0);   /* unblock -> should deliver now */
    if (g_usr2_count != 1) {
        printf("SIGTEST: SIGUSR2 not delivered after unblock (count=%d) FAIL\n",
               g_usr2_count);
        fails++;
    } else {
        printf("SIGTEST: SIGUSR2 delivered after unblock OK\n");
    }

    if (fails == 0) {
        printf("SIGTEST: ALL OK\n");
        return 0;
    }
    printf("SIGTEST: %d CHECK(S) FAILED\n", fails);
    return 1;
}
