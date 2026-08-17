/* sort -- sort lines of text.
 *
 *     sort [-nru] [file ...]
 *
 *   -n   compare as numbers rather than as text
 *   -r   reverse the result
 *   -u   output only the first of an equal run
 *
 * The whole input is held in memory and insertion-sorted, which is fine
 * for the sizes this system deals in and keeps the port small. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

#define SORT_MAX 4096

static char *lines[SORT_MAX];
static int nlines;
static int nflag, rflag, uflag;

static long tonum(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static int cmp(const char *a, const char *b) {
    int r;
    if (nflag) {
        long x = tonum(a), y = tonum(b);
        r = x < y ? -1 : x > y ? 1 : 0;
    } else {
        r = strcmp(a, b);
    }
    return rflag ? -r : r;
}

static int read_fd(int fd, const char *label) {
    char buf[4096];
    size_t len = 0;
    char chunk[1024];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) { weprintf("read %s:", label); return -1; }
        int eof = (n == 0);
        for (ssize_t k = 0; k < n; k++) {
            if (chunk[k] == '\n') {
                buf[len] = '\0';
                if (nlines == SORT_MAX) { weprintf("too many lines"); return -1; }
                lines[nlines++] = estrdup(buf);
                len = 0;
            } else if (len + 1 < sizeof(buf)) {
                buf[len++] = chunk[k];
            }
        }
        if (eof) break;
    }
    if (len) {
        buf[len] = '\0';
        if (nlines == SORT_MAX) { weprintf("too many lines"); return -1; }
        lines[nlines++] = estrdup(buf);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    util_argv0_set(argc > 0 ? argv[0] : "sort");
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'n') nflag = 1;
            else if (*f == 'r') rflag = 1;
            else if (*f == 'u') uflag = 1;
            else eprintf("usage: %s [-nru] [file ...]", argv0);
        }
    }

    int rc = 0;
    if (i >= argc) {
        if (read_fd(0, "<stdin>") < 0) rc = 1;
    } else {
        for (; i < argc; i++) {
            int fd = strcmp(argv[i], "-") ? open(argv[i], O_RDONLY) : 0;
            if (fd < 0) { weprintf("open %s:", argv[i]); rc = 1; continue; }
            if (read_fd(fd, argv[i]) < 0) rc = 1;
            if (fd) close(fd);
        }
    }

    for (int a = 1; a < nlines; a++) {
        char *key = lines[a];
        int b = a - 1;
        while (b >= 0 && cmp(lines[b], key) > 0) { lines[b + 1] = lines[b]; b--; }
        lines[b + 1] = key;
    }

    for (int k = 0; k < nlines; k++) {
        if (uflag && k > 0 && cmp(lines[k - 1], lines[k]) == 0) continue;
        printf("%s\n", lines[k]);
    }
    return rc;
}
