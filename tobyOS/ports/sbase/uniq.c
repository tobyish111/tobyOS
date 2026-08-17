/* uniq -- report or filter out repeated adjacent lines.
 *
 *     uniq [-cdu] [input [output]]
 *
 *   -c   prefix each line with the number of occurrences
 *   -d   print only lines that were repeated
 *   -u   print only lines that were not repeated
 *
 * Only ADJACENT duplicates are considered, which is why uniq is usually
 * fed by sort. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

static int cflag, dflag, uflag;
static int out_fd = 1;

static void emit(const char *line, long count) {
    if (dflag && count < 2) return;
    if (uflag && count > 1) return;
    char buf[4200];
    int n;
    if (cflag) n = snprintf(buf, sizeof(buf), "%7ld %s\n", count, line);
    else       n = snprintf(buf, sizeof(buf), "%s\n", line);
    if (n > 0) (void)write(out_fd, buf, (size_t)n);
}

int main(int argc, char *argv[]) {
    util_argv0_set(argc > 0 ? argv[0] : "uniq");
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'c') cflag = 1;
            else if (*f == 'd') dflag = 1;
            else if (*f == 'u') uflag = 1;
            else eprintf("usage: %s [-cdu] [input [output]]", argv0);
        }
    }
    int fd = 0;
    if (i < argc && strcmp(argv[i], "-")) {
        fd = open(argv[i], O_RDONLY);
        if (fd < 0) eprintf("open %s:", argv[i]);
        i++;
    } else if (i < argc) i++;
    if (i < argc) {
        out_fd = open(argv[i], O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (out_fd < 0) eprintf("open %s:", argv[i]);
        i++;
    }

    char prev[4096], cur[4096];
    int have_prev = 0;
    long count = 0;
    size_t len = 0;
    char chunk[1024];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) { weprintf("read:"); return 1; }
        int eof = (n == 0);
        for (ssize_t k = 0; k < n; k++) {
            if (chunk[k] != '\n') {
                if (len + 1 < sizeof(cur)) cur[len++] = chunk[k];
                continue;
            }
            cur[len] = '\0';
            if (have_prev && strcmp(prev, cur) == 0) {
                count++;
            } else {
                if (have_prev) emit(prev, count);
                memcpy(prev, cur, len + 1);
                have_prev = 1;
                count = 1;
            }
            len = 0;
        }
        if (eof) break;
    }
    if (len) {
        cur[len] = '\0';
        if (have_prev && strcmp(prev, cur) == 0) count++;
        else { if (have_prev) emit(prev, count); memcpy(prev, cur, len + 1); have_prev = 1; count = 1; }
    }
    if (have_prev) emit(prev, count);
    return 0;
}
