/* programs/stdiotest/main.c -- /bin/stdiotest
 *
 * libtoby's BUFFERED stdio (K5, 2026-09-30), one line per case, each ending
 * in "ok" or "BAD"; the last line is "stdiotest: N ok, M bad".
 *
 * Buffering removed the per-character syscalls (589,185 for one xargs
 * reading `seq 100000`), but the native userland was written against
 * unbuffered stdio, so what it must NOT change is checked here: the order
 * of printf output against raw write(2) and against stderr, no output
 * duplicated into a fork child, nothing lost at exit or _exit, and the file
 * position a buffered reader leaves behind. The ordering cases run in a
 * child whose stdout AND stderr are one pipe, and the parent reads back the
 * exact byte sequence.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

static int g_ok, g_bad;

static void check(const char *what, int good) {
    printf("%-46s %s\n", what, good ? "ok" : "BAD");
    if (good) g_ok++; else g_bad++;
}

/* Run fn in a child with fd 1 and fd 2 on one pipe; return what it wrote. */
static int capture(void (*fn)(void), char *out, size_t cap) {
    int p[2];
    if (pipe(p) < 0) return -1;
    fflush(stdout);
    pid_t c = fork();
    if (c == 0) {
        close(p[0]);
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[1]);
        fn();
        exit(0);
    }
    close(p[1]);
    size_t n = 0;
    for (;;) {
        ssize_t r = read(p[0], out + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        if (n >= cap - 1) break;
    }
    out[n] = '\0';
    close(p[0]);
    int st = 0;
    waitpid(c, &st, 0);
    return (int)n;
}

static void w_printf_then_write(void) {
    printf("A");                 /* no newline: buffered (stdout is a pipe) */
    write(1, "B", 1);            /* raw: must come AFTER the A */
    printf("C\n");
}
static void w_stdout_then_stderr(void) {
    printf("out1 ");
    fprintf(stderr, "err1 ");
    printf("out2\n");
}
static void w_fork_no_dup(void) {
    printf("parent-before ");
    pid_t c = fork();
    if (c == 0) { printf("child "); _exit(0); }
    int st; waitpid(c, &st, 0);
    printf("parent-after\n");
}
static void w_exit_flushes(void) {
    printf("lost?");             /* only exit() can write this out */
}
static void w_underscore_exit(void) {
    printf("kept");
    _exit(0);
}

int main(void) {
    char buf[512], line[160];

    capture(w_printf_then_write, buf, sizeof buf);
    snprintf(line, sizeof line, "printf/write order: \"%.20s\"", buf);
    check(line, strcmp(buf, "ABC\n") == 0);

    capture(w_stdout_then_stderr, buf, sizeof buf);
    snprintf(line, sizeof line, "stdout/stderr order: \"%.24s\"", buf);
    check(line, strcmp(buf, "out1 err1 out2\n") == 0);

    capture(w_fork_no_dup, buf, sizeof buf);
    snprintf(line, sizeof line, "fork, no duplicate: \"%.30s\"", buf);
    check(line, strcmp(buf, "parent-before child parent-after\n") == 0);

    capture(w_exit_flushes, buf, sizeof buf);
    snprintf(line, sizeof line, "exit() flushes: \"%.10s\"", buf);
    check(line, strcmp(buf, "lost?") == 0);

    capture(w_underscore_exit, buf, sizeof buf);
    snprintf(line, sizeof line, "_exit() flushes too: \"%.10s\"", buf);
    check(line, strcmp(buf, "kept") == 0);

    /* Buffered reading and the file position. */
    const char *path = "/tmp/stdiotest.dat";
    FILE *f = fopen(path, "w");
    for (int i = 0; i < 2000; i++) fprintf(f, "line %d\n", i);
    fclose(f);

    f = fopen(path, "r");
    char l[64];
    int n = 0, okseq = 1;
    while (fgets(l, sizeof l, f)) {
        char want[32];
        snprintf(want, sizeof want, "line %d\n", n);
        if (strcmp(l, want) != 0) okseq = 0;
        n++;
    }
    fclose(f);
    snprintf(line, sizeof line, "fgets 2000 lines back: %d in order=%d", n, okseq);
    check(line, n == 2000 && okseq);

    f = fopen(path, "r");
    int c0 = fgetc(f), c1 = fgetc(f);
    long pos = ftell(f);
    ungetc(c1, f);
    long pos2 = ftell(f);
    int c1b = fgetc(f);
    snprintf(line, sizeof line, "fgetc/ftell/ungetc: %c%c pos %ld %ld", c0, c1, pos, pos2);
    check(line, c0 == 'l' && c1 == 'i' && pos == 2 && pos2 == 1 && c1b == 'i');

    fseek(f, 10, SEEK_SET);
    fgets(l, sizeof l, f);
    snprintf(line, sizeof line, "fseek(10) then fgets: \"%.12s\"", l);
    check(line, strcmp(l, "e 1\n") == 0);

    fseek(f, -4, SEEK_CUR);
    fgets(l, sizeof l, f);
    snprintf(line, sizeof line, "fseek(-4, SEEK_CUR): \"%.12s\"", l);
    check(line, strcmp(l, "e 1\n") == 0);

    fclose(f);

    char big[9000];
    f = fopen(path, "r");
    size_t got = fread(big, 1, sizeof big, f);
    snprintf(line, sizeof line, "fread 9000 bytes: %u", (unsigned)got);
    check(line, got == sizeof big && memcmp(big, "line 0\nline 1\n", 14) == 0);
    fclose(f);
    unlink(path);

    printf("stdiotest: %d ok, %d bad\n", g_ok, g_bad);
    return g_bad ? 1 : 0;
}
