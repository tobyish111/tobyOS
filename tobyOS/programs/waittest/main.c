/* programs/waittest/main.c -- /bin/waittest
 *
 * The NATIVE wait family, one line per case: wait(), waitpid(-1),
 * waitpid(0) and waitpid(-pgid) through libtoby, as a native program calls
 * them.
 *
 * WHY THIS EXISTS (K2, 2026-09-30). libtoby's wait() is waitpid(-1, ...),
 * and the kernel's native waitpid handled only pid > 0: -1 went to
 * proc_lookup(-1) and came back ENOENT. So EVERY native program that
 * called wait() failed, and nothing in the tree noticed -- the shells wait
 * for named pids. The Linux wait4 learned pid <= 0 in f63f50d; this is the
 * native half, and the test that keeps it.
 *
 * Each line prints what happened and ends in "ok" or "BAD"; the last line
 * is "waittest: N ok, M bad". Native status encoding is unchanged: a child
 * killed by SIGKILL reads WIFEXITED with WEXITSTATUS 137 (128 + 9).
 */

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

static int g_ok, g_bad;

static void check(const char *what, int good) {
    printf("%-44s %s\n", what, good ? "ok" : "BAD");
    if (good) g_ok++; else g_bad++;
}

static pid_t kid(int code, unsigned ms, int own_group) {
    pid_t p = fork();
    if (p == 0) {
        if (own_group) setpgid(0, 0);
        usleep(ms * 1000u);
        _exit(code);
    }
    return p;
}

int main(void) {
    char line[128];
    int st = 0;

    /* wait() twice: both children, the first to exit first. */
    pid_t a = kid(3, 300, 0), b = kid(5, 50, 0);
    pid_t w1 = wait(&st);
    int s1 = WEXITSTATUS(st);
    pid_t w2 = wait(&st);
    int s2 = WEXITSTATUS(st);
    snprintf(line, sizeof line, "wait x2: %s then %s, status %d %d",
             w1 == b ? "second" : w1 == a ? "first" : "?",
             w2 == a ? "first" : w2 == b ? "second" : "?", s1, s2);
    check(line, w1 == b && s1 == 5 && w2 == a && s2 == 3);

    errno = 0;
    pid_t w3 = wait(&st);
    snprintf(line, sizeof line, "wait, no children: %d errno=%d", (int)w3, errno);
    check(line, w3 == -1 && errno == ECHILD);

    /* WNOHANG on a running child: 0, and the child is still there. */
    pid_t c = kid(7, 200, 0);
    pid_t w4 = waitpid(-1, &st, WNOHANG);
    snprintf(line, sizeof line, "waitpid(-1, WNOHANG) running: %d", (int)w4);
    check(line, w4 == 0);
    pid_t w5 = waitpid(-1, &st, 0);
    snprintf(line, sizeof line, "waitpid(-1) then: %s status %d",
             w5 == c ? "that child" : "?", WEXITSTATUS(st));
    check(line, w5 == c && WEXITSTATUS(st) == 7);

    /* waitpid(0): a child in the caller's own group. */
    pid_t d = kid(9, 50, 0);
    pid_t w6 = waitpid(0, &st, 0);
    snprintf(line, sizeof line, "waitpid(0) own group: %s status %d",
             w6 == d ? "the child" : "?", WEXITSTATUS(st));
    check(line, w6 == d && WEXITSTATUS(st) == 9);

    /* waitpid(-pgid): a child that leads its own group -- and waitpid(0)
     * must NOT take it (it is not in our group). */
    pid_t e = kid(11, 150, 1);
    usleep(50000);
    errno = 0;
    pid_t w7 = waitpid(0, &st, WNOHANG);
    snprintf(line, sizeof line, "waitpid(0) skips other group: %d errno=%d",
             (int)w7, errno);
    check(line, w7 == -1 && errno == ECHILD);
    pid_t w8 = waitpid(-e, &st, 0);
    snprintf(line, sizeof line, "waitpid(-pgid): %s status %d",
             w8 == e ? "the child" : "?", WEXITSTATUS(st));
    check(line, w8 == e && WEXITSTATUS(st) == 11);

    /* Killed by a signal: still an exit with 128 + sig, as every native
     * program has always read it -- and the status also names the signal
     * (WIFSIGNALED / WTERMSIG, 2026-09-30). */
    pid_t f = kid(0, 5000, 0);
    usleep(50000);
    kill(f, SIGKILL);
    pid_t w9 = wait(&st);
    snprintf(line, sizeof line, "wait, SIGKILLed child: %s exited=%d code %d",
             w9 == f ? "the child" : "?", WIFEXITED(st) ? 1 : 0, WEXITSTATUS(st));
    check(line, w9 == f && WIFEXITED(st) && WEXITSTATUS(st) == 137);
    snprintf(line, sizeof line, "  ...and signaled=%d termsig=%d",
             WIFSIGNALED(st) ? 1 : 0, WTERMSIG(st));
    check(line, WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    pid_t g = kid(42, 10, 0);
    waitpid(g, &st, 0);
    snprintf(line, sizeof line, "exit(42): exited=%d code %d signaled=%d",
             WIFEXITED(st) ? 1 : 0, WEXITSTATUS(st), WIFSIGNALED(st) ? 1 : 0);
    check(line, WIFEXITED(st) && WEXITSTATUS(st) == 42 && !WIFSIGNALED(st));

    errno = 0;
    pid_t w10 = waitpid(-1, &st, WNOHANG);
    snprintf(line, sizeof line, "waitpid(-1, WNOHANG) no children: %d errno=%d",
             (int)w10, errno);
    check(line, w10 == -1 && errno == ECHILD);

    printf("waittest: %d ok, %d bad\n", g_ok, g_bad);
    return g_bad ? 1 : 0;
}
