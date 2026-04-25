/* libtoby/src/assert.c -- assertion failure handler.
 *
 * Writes a single human-readable line to fd 2 (stderr) and aborts.
 * We use the raw write helper directly rather than fprintf(stderr) so
 * an assertion firing from inside printf() (which it can, indirectly)
 * doesn't recurse forever. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "libtoby_internal.h"

void __libtoby_assert_fail(const char *expr,
                           const char *file,
                           unsigned    line,
                           const char *func) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "[libtoby] assert failed: %s at %s:%u (%s)\n",
                     expr ? expr : "?",
                     file ? file : "?",
                     line,
                     func ? func : "?");
    if (n < 0) n = 0;
    if ((size_t)n > sizeof(buf)) n = (int)sizeof(buf);
    __toby_raw_write(2, buf, (size_t)n);
    abort();
}
