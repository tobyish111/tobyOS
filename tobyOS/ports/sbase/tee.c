/* tee -- copy standard input to standard output and to files.
 *
 *     tee [-ai] [file ...]
 *
 *   -a   append to the files rather than truncating
 *   -i   ignore SIGINT
 *
 * A file that cannot be opened is a diagnostic and a non-zero exit, but
 * the remaining files and stdout still get the data. */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

#define TEE_MAX 16

int main(int argc, char *argv[]) {
    util_argv0_set(argc > 0 ? argv[0] : "tee");
    int aflag = 0, iflag = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'a') aflag = 1;
            else if (*f == 'i') iflag = 1;
            else eprintf("usage: %s [-ai] [file ...]", argv0);
        }
    }
    if (iflag) signal(SIGINT, SIG_IGN);

    int fds[TEE_MAX];
    int nfd = 0, rc = 0;
    for (; i < argc && nfd < TEE_MAX; i++) {
        int flags = O_WRONLY | O_CREAT | (aflag ? O_APPEND : O_TRUNC);
        int fd = open(argv[i], flags, 0666);
        if (fd < 0) { weprintf("open %s:", argv[i]); rc = 1; continue; }
        fds[nfd++] = fd;
    }

    char buf[4096];
    for (;;) {
        ssize_t n = read(0, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { weprintf("read:"); rc = 1; break; }
        if (write(1, buf, (size_t)n) < 0) { weprintf("write:"); rc = 1; }
        for (int k = 0; k < nfd; k++)
            if (write(fds[k], buf, (size_t)n) < 0) { weprintf("write:"); rc = 1; }
    }
    for (int k = 0; k < nfd; k++) close(fds[k]);
    return rc;
}
