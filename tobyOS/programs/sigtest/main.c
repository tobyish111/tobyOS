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
 *   4. SA_RESTART: a blocking read() interrupted by a caught signal is
 *      transparently restarted (returns the data, not EINTR).
 *   5. Without SA_RESTART the same read() fails with errno == EINTR.
 *   6. Job control: SIGSTOP parks a CPU-spinning child (observed via
 *      /proc/<pid>/status), SIGCONT resumes it, SIGKILL then reaps it.
 *
 * Prints SIGTEST sentinels and returns 0 only if every check passes, so a
 * boot harness can `proc_wait` it and assert PASS. Run as `sigtest --boot`
 * for the same behaviour (the flag is accepted for symmetry with the other
 * boot harnesses; output is identical).
 */

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>

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

    /* ---- Test 4/5: SA_RESTART vs EINTR on an interrupted read ----
     *
     * The child signals us mid-read (the pipe is empty, so the parent's
     * read() blocks, bails out with EINTR when the signal lands, and the
     * handler runs at syscall return). With SA_RESTART the kernel rewinds
     * the saved user RIP onto the `syscall` instruction so the read
     * re-executes after the handler and returns the data the child writes
     * afterwards; without it the user code sees -1/EINTR. */
    for (int restart = 1; restart >= 0; restart--) {
        int fds[2];
        if (pipe(fds) != 0) {
            printf("SIGTEST: pipe() FAIL\n");
            fails++;
            break;
        }

        g_usr1_count = 0;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;
        sa.sa_flags   = restart ? SA_RESTART : 0;
        sigaction(SIGUSR1, &sa, 0);

        pid_t me  = getpid();
        pid_t pid = fork();
        if (pid == 0) {
            /* Child: let the parent block in read(), interrupt it, then
             * supply the byte its (possibly restarted) read is waiting on. */
            close(fds[0]);
            usleep(150 * 1000);
            kill(me, SIGUSR1);
            usleep(150 * 1000);
            write(fds[1], "R", 1);
            close(fds[1]);
            _exit(0);
        }
        if (pid < 0) {
            printf("SIGTEST: fork() FAIL\n");
            fails++;
            close(fds[0]); close(fds[1]);
            break;
        }

        close(fds[1]);
        char c = 0;
        errno = 0;
        ssize_t n = read(fds[0], &c, 1);

        if (restart) {
            if (n == 1 && c == 'R' && g_usr1_count == 1) {
                printf("SIGTEST: SA_RESTART restarted interrupted read OK\n");
            } else {
                printf("SIGTEST: SA_RESTART read n=%d c=%c handlers=%d FAIL\n",
                       (int)n, c ? c : '?', g_usr1_count);
                fails++;
            }
        } else {
            if (n == -1 && errno == EINTR && g_usr1_count == 1) {
                printf("SIGTEST: read without SA_RESTART -> EINTR OK\n");
            } else {
                printf("SIGTEST: no-restart read n=%d errno=%d handlers=%d "
                       "FAIL\n", (int)n, errno, g_usr1_count);
                fails++;
            }
            /* Drain the byte the child writes anyway (blocks until it
             * arrives) so the child can exit and be reaped cleanly. */
            read(fds[0], &c, 1);
        }

        close(fds[0]);
        waitpid(pid, 0, 0);
    }

    /* ---- Test 6: SIGSTOP/SIGCONT job control on a spinning child ----
     *
     * The child busy-spins in ring 3 and never makes a syscall, so the
     * stop has to land via the PIT-tick delivery path -- the strongest
     * version of the test. State is observed via /proc/<pid>/status. */
    {
        pid_t pid = fork();
        if (pid == 0) {
            for (;;) { /* pure CPU spin */ }
        }
        if (pid < 0) {
            printf("SIGTEST: fork() for stop test FAIL\n");
            fails++;
        } else {
            char path[64], buf[256];
            snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);

            usleep(100 * 1000);            /* let the child start spinning */
            kill(pid, SIGSTOP);

            int stopped = 0;
            for (int i = 0; i < 40 && !stopped; i++) {   /* <= ~2 s */
                usleep(50 * 1000);
                int fd = open(path, 0);
                if (fd < 0) continue;
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n <= 0) continue;
                buf[n] = '\0';
                if (strstr(buf, "STOPPED")) stopped = 1;
            }
            if (stopped) {
                printf("SIGTEST: SIGSTOP parked the spinning child OK\n");
            } else {
                printf("SIGTEST: child never reached STOPPED FAIL\n");
                fails++;
            }

            kill(pid, SIGCONT);
            int resumed = 0;
            for (int i = 0; i < 40 && !resumed; i++) {
                usleep(50 * 1000);
                int fd = open(path, 0);
                if (fd < 0) continue;
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n <= 0) continue;
                buf[n] = '\0';
                if (!strstr(buf, "STOPPED")) resumed = 1;
            }
            if (resumed) {
                printf("SIGTEST: SIGCONT resumed the child OK\n");
            } else {
                printf("SIGTEST: child still STOPPED after SIGCONT FAIL\n");
                fails++;
            }

            kill(pid, SIGKILL);
            if (waitpid(pid, 0, 0) == pid) {
                printf("SIGTEST: SIGKILL reaped the child OK\n");
            } else {
                printf("SIGTEST: waitpid after SIGKILL FAIL\n");
                fails++;
            }
        }
    }

    if (fails == 0) {
        printf("SIGTEST: ALL OK\n");
        return 0;
    }
    printf("SIGTEST: %d CHECK(S) FAILED\n", fails);
    return 1;
}
