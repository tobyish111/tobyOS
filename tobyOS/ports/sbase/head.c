/* head -- output first N lines of files.
 *
 * Modeled on sbase's head.c (ISC license). Subset:
 *
 *     head [-n N] [file ...]
 *
 *   -n N   print the first N lines (default 10)
 *
 * Multiple files print "==> name <==" headers between them, like
 * GNU/sbase. With no arguments head reads from stdin. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

static int n_lines = 10;

static int head_fd(int fd, const char *label) {
    char  buf[4096];
    int   left = n_lines;
    for (;;) {
        if (left <= 0) return 0;
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) return 0;
        if (n < 0) { weprintf("read %s:", label); return -1; }

        ssize_t emit = 0;
        for (ssize_t i = 0; i < n && left > 0; i++) {
            if (buf[i] == '\n') {
                ssize_t w = write(1, buf + emit, (size_t)(i - emit + 1));
                if (w < 0) { weprintf("write:"); return -1; }
                emit = i + 1;
                left--;
            }
        }
        /* Trailing partial line (no newline) only if we haven't
         * yet hit our limit -- otherwise stop. */
        if (left > 0 && emit < n) {
            ssize_t w = write(1, buf + emit, (size_t)(n - emit));
            if (w < 0) { weprintf("write:"); return -1; }
        }
    }
}

static void usage(void) { eprintf("usage: %s [-n lines] [file ...]", argv0); }

int main(int argc, char *argv[]) {
    util_argv0_set(argv[0]);

    /* GNU accepts `head -5` as shorthand for `head -n 5`, and scripts use it
     * constantly (`chage -l alice | head -2`). getopt() cannot express a
     * numeric option, so consume it before getopt sees it.
     *
     * The argument is REMOVED rather than rewritten into "-n" plus a value:
     * inserting needs one slot more than argv has -- argv[argc] is the NULL
     * terminator and nothing past it is ours -- so growing it writes out of
     * bounds. Shrinking is always safe. (tail had the same gap and the same
     * trap; see programs/user_tail/main.c.) */
    for (int i = 1; i < argc; ) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] < '0' || a[1] > '9') { i++; continue; }
        int ok = 1, v = 0;
        for (const char *q = a + 1; *q; q++) {
            if (*q < '0' || *q > '9') { ok = 0; break; }
            v = v * 10 + (*q - '0');
        }
        if (!ok) { i++; continue; }
        n_lines = v;
        for (int k = i; k < argc; k++) argv[k] = argv[k + 1];   /* NULL moves too */
        argc--;
    }

    int opt;
    while ((opt = getopt(argc, argv, "n:")) != -1) {
        switch (opt) {
        case 'n': n_lines = atoi(optarg); if (n_lines < 0) usage(); break;
        default:  usage();
        }
    }

    int rc      = 0;
    int nfiles  = argc - optind;
    int show_hd = nfiles > 1;

    if (nfiles <= 0) return head_fd(0, "<stdin>") < 0 ? 1 : 0;

    for (int i = optind; i < argc; i++) {
        if (show_hd) printf("%s==> %s <==\n", i > optind ? "\n" : "", argv[i]);
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { weprintf("open %s:", argv[i]); rc = 1; continue; }
        if (head_fd(fd, argv[i]) < 0) rc = 1;
        close(fd);
    }
    return rc;
}
