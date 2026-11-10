/* spechelp/main.c -- the Oils spec suite's Python helpers, in C.
 *
 * The third-party corpus in third_party/oils-spec leans on four tiny Python 2
 * scripts (spec/bin/argv.py and friends). tobyOS ships CPython, but making the
 * shell gate depend on it would mean a Python bug could read as a shell bug,
 * and the host oracle would be running a DIFFERENT interpreter than the guest.
 * So they are reimplemented here once, in C, and the same source is compiled
 * for both the host oracle (logs/oilspec_host.sh) and the guest. One
 * implementation, two hosts -- the same arrangement src/shell.c already has.
 *
 * The binary dispatches on argv[0], so one object is installed under all four
 * names. tobyOS has no hard links, so those are copies.
 *
 * argv.py must reproduce PYTHON 2's repr() of a list of str byte-for-byte,
 * because that is what the recorded expectations in the corpus contain. That
 * only matters for the cross-check against those expectations -- the gate
 * itself is differential, and there both shells call THIS binary -- but a
 * helper that quietly disagreed with the corpus would make every expectation
 * mismatch look like a tsh bug.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Python 2 repr() of a str: single quotes unless the string contains a single
 * quote and no double quote; backslash, the active quote, \t \n \r escaped;
 * printable ASCII literal; everything else \xHH. */
static void repr_str(const char *s, size_t n, FILE *out) {
    int has_sq = 0, has_dq = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\'') has_sq = 1;
        if (s[i] == '"')  has_dq = 1;
    }
    char q = (has_sq && !has_dq) ? '"' : '\'';
    fputc(q, out);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\')      fputs("\\\\", out);
        else if (c == (unsigned char)q) { fputc('\\', out); fputc(q, out); }
        else if (c == '\t') fputs("\\t", out);
        else if (c == '\n') fputs("\\n", out);
        else if (c == '\r') fputs("\\r", out);
        else if (c >= 0x20 && c < 0x7f) fputc((char)c, out);
        else fprintf(out, "\\x%02x", c);
    }
    fputc(q, out);
}

static int do_argv(int argc, char **argv) {
    fputc('[', stdout);
    for (int i = 1; i < argc; i++) {
        if (i > 1) fputs(", ", stdout);
        repr_str(argv[i], strlen(argv[i]), stdout);
    }
    fputs("]\n", stdout);
    return 0;
}

static int do_printenv(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *v = getenv(argv[i]);
        /* Python prints the None object for a missing variable, not "". */
        puts(v ? v : "None");
    }
    return 0;
}

static int do_stdout_stderr(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "STDOUT";
    const char *err = argc > 2 ? argv[2] : "STDERR";
    int status      = argc > 3 ? atoi(argv[3]) : 0;
    printf("%s\n", out);
    fflush(stdout);
    fprintf(stderr, "%s\n", err);
    return status;
}

static int do_read_from_fd(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        int fd = atoi(argv[i]);
        char buf[1024];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            fprintf(stderr, "FATAL: Error reading from fd %d\n", fd);
            return 1;
        }
        printf("%d: ", fd);
        fwrite(buf, 1, (size_t)n, stdout);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "argv.py";
    const char *slash = strrchr(base, '/');
    if (slash) base = slash + 1;

    int rc;
    if      (!strcmp(base, "printenv.py"))      rc = do_printenv(argc, argv);
    else if (!strcmp(base, "stdout_stderr.py")) rc = do_stdout_stderr(argc, argv);
    else if (!strcmp(base, "read_from_fd.py"))  rc = do_read_from_fd(argc, argv);
    else                                        rc = do_argv(argc, argv);
    fflush(stdout);
    return rc;
}
