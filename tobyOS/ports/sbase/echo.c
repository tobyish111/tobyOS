/* echo -- write arguments to standard output.
 *
 * Modeled on sbase's echo.c (ISC license). The semantics are the
 * POSIX/XSI subset:
 *
 *     echo [-n] [string ...]
 *
 *   -n   suppress the trailing newline.
 *
 * No backslash-escape interpretation (the GNU '-e' / SUSv4 XSI
 * variant) -- that's intentionally out of scope until we have a
 * use for it. Same as upstream sbase. */

#include <stdio.h>
#include <string.h>

#include "util.h"

static void usage(void) {
    eprintf("usage: %s [-n] [string ...]", argv0);
}

int main(int argc, char *argv[]) {
    int nflag = 0;
    int i;

    util_argv0_set(argv[0]);

    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        nflag = 1;
        argc--;
        argv++;
    }

    for (i = 1; i < argc; i++) {
        if (printf("%s%s", i > 1 ? " " : "", argv[i]) < 0) usage();
    }
    if (!nflag) putchar('\n');
    return 0;
}
