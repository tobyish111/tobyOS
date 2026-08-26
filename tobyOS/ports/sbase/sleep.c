/* sleep -- suspend execution for an interval.
 *
 *     sleep seconds
 *
 * POSIX requires an integral number of seconds; a fractional part is a
 * common extension and is accepted here. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

int main(int argc, char *argv[]) {
    util_argv0_set(argc > 0 ? argv[0] : "sleep");
    if (argc != 2) eprintf("usage: %s seconds", argv0);

    const char *p = argv[1];
    unsigned long whole = 0, frac_ms = 0;
    if (*p < '0' || *p > '9') eprintf("invalid time interval '%s'", argv[1]);
    while (*p >= '0' && *p <= '9') whole = whole * 10 + (unsigned)(*p++ - '0');
    if (*p == '.') {
        p++;
        unsigned long scale = 100;
        while (*p >= '0' && *p <= '9') {
            frac_ms += (unsigned long)(*p++ - '0') * scale;
            scale /= 10;
            if (!scale) { while (*p >= '0' && *p <= '9') p++; break; }
        }
    }
    if (*p) eprintf("invalid time interval '%s'", argv[1]);

    unsigned long ms = whole * 1000UL + frac_ms;
    if (ms) usleep(ms * 1000UL);
    return 0;
}
