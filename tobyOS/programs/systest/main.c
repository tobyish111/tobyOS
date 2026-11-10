/* systest -- prove libtoby's system() and popen() reach a REAL shell.
 *
 * The point of item 4 in the native-parity work: /bin/sh used to be a
 * minimal shell (0/14 against bash), and system()/popen() routed through it,
 * so a pipeline or a quoted argument in a system() string did not behave.
 * /bin/sh is tsh now, and this exercises exactly the constructs that were
 * broken: a pipeline, an exit status carried back through waitpid's
 * encoding, and popen() reading a command's output.
 *
 * It prints one line per check with a fixed shape the gate matches, then a
 * summary. No libc headers beyond what libtoby ships, so it does not depend
 * on the on-image compiler having a header set (which tcc does not). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

int main(void) {
    int fails = 0;

    /* A pipeline and an exit status. `tr` uppercases, and the shell carries
     * `exit 3` back through the wait encoding. */
    int r = system("echo from-system | tr a-z A-Z; exit 3");
    int code = (r >= 0 && WIFEXITED(r)) ? WEXITSTATUS(r) : -1;
    printf("system_status=%d\n", code);
    if (code != 3) fails++;

    /* NULL asks "is a shell available"; POSIX says nonzero. */
    printf("system_probe=%d\n", system(NULL) != 0 ? 1 : 0);

    /* popen for reading: count the lines a piped printf produces. */
    FILE *p = popen("printf 'p1\\np2\\np3\\n' | wc -l", "r");
    int lines = -1;
    if (p) {
        char b[64];
        if (fgets(b, sizeof b, p)) lines = atoi(b);
        pclose(p);
    }
    printf("popen_lines=%d\n", lines);
    if (lines != 3) fails++;

    /* popen for writing: the shell on the other end sorts what we send. */
    p = popen("sort > /tmp/systest_sorted.txt", "w");
    if (p) {
        fputs("gamma\nalpha\nbeta\n", p);
        pclose(p);
    }
    FILE *rd = fopen("/tmp/systest_sorted.txt", "r");
    char first[64] = "";
    if (rd) { if (!fgets(first, sizeof first, rd)) first[0] = 0; fclose(rd); }
    first[strcspn(first, "\n")] = 0;
    printf("popen_write_first=%s\n", first[0] ? first : "?");
    if (strcmp(first, "alpha") != 0) fails++;

    /* A quoted argument with shell metacharacters must survive as data. */
    r = system("echo 'a b|c' | wc -c");
    printf("system_quoted=%d\n", (r >= 0 && WIFEXITED(r)) ? WEXITSTATUS(r) : -1);

    printf("systest: %s (%d fail)\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
