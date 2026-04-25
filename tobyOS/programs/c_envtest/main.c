/* programs/c_envtest/main.c -- libtoby environment + exec smoke test.
 *
 * Exercises everything Milestone 25C added on top of M25B's libc:
 *
 *   1. argc/argv from kernel-packed user stack
 *   2. environ walk + getenv()
 *   3. setenv/getenv round-trip (overwrite=1)
 *   4. setenv with overwrite=0 leaves an existing var untouched
 *   5. unsetenv removes a key (getenv -> NULL after)
 *   6. putenv with a stack buffer
 *   7. clearenv leaves environ empty
 *   8. system("c_hello") spawns a real child, returns its status
 *   9. recursive execvp via "--exec-child <prog>" mode (PATH search)
 *
 * Run modes:
 *   c_envtest                 -- full self-test, exits 0 on PASS
 *   c_envtest --exec-child P  -- execvp(P, {P, NULL}) and never return
 *
 * Failures are printed as "[c_envtest] FAIL: <reason>" and the program
 * exits non-zero so the boot harness can grade it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static int fails = 0;

#define EXPECT(cond, msg) do {                                       \
    if (!(cond)) {                                                   \
        printf("[c_envtest] FAIL: %s  (line %d)\n", (msg), __LINE__); \
        fails++;                                                     \
    }                                                                \
} while (0)

static size_t environ_count(void) {
    size_t n = 0;
    if (!environ) return 0;
    while (environ[n]) n++;
    return n;
}

int main(int argc, char **argv) {
    /* DEBUG (M25C): build marker -- if this prints we know the new
     * c_envtest+libtoby image actually got loaded. */
    fprintf(stderr, "[c_envtest][build] M25C-debug-rev3 alive\n");

    /* ------------------------------------------------------------ *
     *  Mode dispatch: --exec-child re-execs into argv[2] via execvp
     * ------------------------------------------------------------ */
    if (argc >= 3 && strcmp(argv[1], "--exec-child") == 0) {
        printf("[c_envtest] (child) execvp -> '%s'\n", argv[2]);
        char *child_argv[2] = { argv[2], 0 };
        execvp(argv[2], child_argv);
        printf("[c_envtest] FAIL: execvp returned (errno=%d)\n", errno);
        return 99;
    }

    /* ------------------------------------------------------------ *
     *  1. argc/argv
     * ------------------------------------------------------------ */
    printf("[c_envtest] argc=%d argv[0]='%s'\n",
           argc, argc > 0 ? argv[0] : "(none)");
    EXPECT(argc >= 1,        "argc < 1 (crt0 didn't pack argv)");
    EXPECT(argv != NULL,     "argv is NULL");
    EXPECT(argv[argc] == 0,  "argv not NULL-terminated");

    /* ------------------------------------------------------------ *
     *  2. environ walk + getenv basics
     * ------------------------------------------------------------ */
    size_t initial_envc = environ_count();
    printf("[c_envtest] environ has %u initial entries\n",
           (unsigned)initial_envc);
    EXPECT(environ != NULL, "environ is NULL (crt0 didn't seed it)");

    const char *path = getenv("PATH");
    printf("[c_envtest] getenv(PATH) = %s\n", path ? path : "(unset)");
    EXPECT(path != NULL, "PATH not in environ");

    /* ------------------------------------------------------------ *
     *  3. setenv with overwrite=1 + round-trip
     * ------------------------------------------------------------ */
    int rc = setenv("M25C_KEY", "alpha", 1);
    EXPECT(rc == 0, "setenv(M25C_KEY=alpha,1) failed");
    const char *v = getenv("M25C_KEY");
    EXPECT(v != NULL && strcmp(v, "alpha") == 0,
           "getenv(M25C_KEY) didn't return 'alpha'");

    rc = setenv("M25C_KEY", "beta", 1);
    EXPECT(rc == 0, "setenv(M25C_KEY=beta,1) failed");
    v = getenv("M25C_KEY");
    EXPECT(v != NULL && strcmp(v, "beta") == 0,
           "getenv(M25C_KEY) didn't update to 'beta'");

    /* ------------------------------------------------------------ *
     *  4. setenv with overwrite=0 keeps the existing value
     * ------------------------------------------------------------ */
    rc = setenv("M25C_KEY", "gamma", 0);
    EXPECT(rc == 0, "setenv(M25C_KEY=gamma,0) failed");
    v = getenv("M25C_KEY");
    EXPECT(v != NULL && strcmp(v, "beta") == 0,
           "setenv overwrite=0 wrongly replaced existing value");

    /* ------------------------------------------------------------ *
     *  5. unsetenv removes the key
     * ------------------------------------------------------------ */
    rc = unsetenv("M25C_KEY");
    EXPECT(rc == 0, "unsetenv(M25C_KEY) failed");
    v = getenv("M25C_KEY");
    EXPECT(v == NULL, "M25C_KEY still set after unsetenv");

    /* ------------------------------------------------------------ *
     *  6. putenv -- stores the caller's exact pointer
     * ------------------------------------------------------------ */
    static char putbuf[] = "M25C_PUT=fortytwo";
    rc = putenv(putbuf);
    EXPECT(rc == 0, "putenv failed");
    v = getenv("M25C_PUT");
    EXPECT(v != NULL && strcmp(v, "fortytwo") == 0,
           "getenv(M25C_PUT) didn't return 'fortytwo'");

    /* ------------------------------------------------------------ *
     *  7. clearenv -- environ becomes empty
     * ------------------------------------------------------------ */
    rc = clearenv();
    EXPECT(rc == 0, "clearenv failed");
    EXPECT(environ_count() == 0, "environ not empty after clearenv");
    EXPECT(getenv("PATH") == NULL, "PATH still resolves after clearenv");

    /* Re-seed PATH so the upcoming system()/execvp tests can find
     * /bin/c_hello via PATH search (the kernel passed PATH=/bin in
     * the boot-harness envp; the shell's setenv path uses the same). */
    rc = setenv("PATH", "/bin", 1);
    EXPECT(rc == 0, "setenv(PATH=/bin) failed after clearenv");

    /* ------------------------------------------------------------ *
     *  8. system("c_hello") -- in-process synchronous spawn+wait
     * ------------------------------------------------------------ */
    fprintf(stderr, "[c_envtest][diag] BEFORE system\n");
    printf("[c_envtest] system(\"c_hello\") starting...\n");
    int sr = system("c_hello");
    fprintf(stderr, "[c_envtest][diag] AFTER system, sr=%d\n", sr);
    printf("[c_envtest] system(\"c_hello\") raw status=0x%x\n", sr);
    fprintf(stderr, "[c_envtest][diag] AFTER printf raw status\n");
    EXPECT(sr >= 0,                    "system() returned -1");
    EXPECT(WIFEXITED(sr),              "child didn't exit normally");
    EXPECT(WEXITSTATUS(sr) == 0,       "c_hello exited non-zero");
    fprintf(stderr, "[c_envtest][diag] AFTER system EXPECTs\n");

    /* ------------------------------------------------------------ *
     *  9. exec emulation via recursive --exec-child
     *
     *  We spawn ourselves with [argv0, "--exec-child", "c_hello"].
     *  The child will execvp("c_hello", ...) -- which itself spawns
     *  /bin/c_hello, waits, and _exits with c_hello's exit code (0).
     *  From the parent's view the recursive PID exits 0 if and only
     *  if the whole chain works. */
    printf("[c_envtest] recursive execvp test starting...\n");
    {
        extern pid_t toby_spawn(const char *path, char *const argv[],
                                char *const envp[],
                                int fd0, int fd1, int fd2);
        char *recurse_argv[4] = {
            (char *)"c_envtest", (char *)"--exec-child",
            (char *)"c_hello",   0
        };
        pid_t pid = toby_spawn("/bin/c_envtest", recurse_argv, environ,
                               0, 0, 0);
        EXPECT(pid > 0, "recursive toby_spawn failed");
        if (pid > 0) {
            int status = 0;
            pid_t rc2 = waitpid(pid, &status, 0);
            printf("[c_envtest] recursive child pid=%d status=0x%x\n",
                   (int)pid, status);
            EXPECT(rc2 == pid,            "waitpid(recursive) failed");
            EXPECT(WIFEXITED(status),     "recursive child didn't exit normally");
            EXPECT(WEXITSTATUS(status) == 0,
                   "recursive c_hello via execvp returned non-zero");
        }
    }

    /* ------------------------------------------------------------ *
     *  Verdict
     * ------------------------------------------------------------ */
    if (fails == 0) {
        printf("[c_envtest] ALL OK\n");
        return 0;
    }
    printf("[c_envtest] FAILED (%d check(s))\n", fails);
    return 1;
}
