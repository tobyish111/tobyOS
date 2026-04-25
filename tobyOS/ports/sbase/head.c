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
