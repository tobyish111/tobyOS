/* basename -- strip directory and suffix from a pathname.
 *
 *     basename string [suffix]
 *
 * POSIX: a string of all slashes is "/"; trailing slashes are removed
 * before the last component is taken; the suffix is only removed when it
 * is not the whole remaining name. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

int main(int argc, char *argv[]) {
    util_argv0_set(argc > 0 ? argv[0] : "basename");
    if (argc < 2 || argc > 3)
        eprintf("usage: %s string [suffix]", argv0);

    char buf[1024];
    size_t n = strlen(argv[1]);
    if (n + 1 > sizeof(buf)) eprintf("name too long");
    memcpy(buf, argv[1], n + 1);

    if (n == 0) { printf("\n"); return 0; }

    /* All slashes collapses to a single "/". */
    size_t i = 0;
    while (buf[i] == '/') i++;
    if (buf[i] == '\0') { printf("/\n"); return 0; }

    while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';

    char *base = buf;
    for (size_t k = 0; buf[k]; k++)
        if (buf[k] == '/') base = buf + k + 1;

    if (argc == 3) {
        size_t bl = strlen(base), sl = strlen(argv[2]);
        /* Never strip the suffix down to nothing. */
        if (sl && bl > sl && strcmp(base + bl - sl, argv[2]) == 0)
            base[bl - sl] = '\0';
    }
    printf("%s\n", base);
    return 0;
}
