/* dirname -- strip the last component from a pathname.
 *
 *     dirname string
 *
 * POSIX: no slash at all yields "."; a string of all slashes yields "/";
 * trailing slashes are ignored before the last component is removed. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

int main(int argc, char *argv[]) {
    util_argv0_set(argc > 0 ? argv[0] : "dirname");
    if (argc != 2) eprintf("usage: %s string", argv0);

    char buf[1024];
    size_t n = strlen(argv[1]);
    if (n + 1 > sizeof(buf)) eprintf("name too long");
    memcpy(buf, argv[1], n + 1);

    while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';
    if (n == 0) { printf(".\n"); return 0; }

    char *slash = 0;
    for (size_t k = 0; buf[k]; k++)
        if (buf[k] == '/') slash = buf + k;

    if (!slash) { printf(".\n"); return 0; }

    while (slash > buf && *slash == '/') slash--;
    if (slash == buf && buf[0] == '/') { printf("/\n"); return 0; }
    slash[1] = '\0';
    printf("%s\n", buf);
    return 0;
}
