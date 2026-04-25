/* cat -- concatenate and print files.
 *
 * Modeled on sbase's cat.c (ISC license). Subset:
 *
 *     cat [file ...]
 *
 * No options -- not -u (POSIX line-buffered toggle), not -n (number
 * lines), not -s (squeeze blank lines). Add them when a port asks
 * for them. The file argument "-" reads stdin, matching POSIX. With
 * no arguments cat reads stdin to EOF.
 */

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include "util.h"

static int cat_one(const char *path) {
    int fd;
    int rc;
    if (strcmp(path, "-") == 0) {
        fd = 0;
        rc = concat_fd(fd, "<stdin>");
        return rc;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        weprintf("open %s:", path);
        return -1;
    }
    rc = concat_fd(fd, path);
    close(fd);
    return rc;
}

int main(int argc, char *argv[]) {
    int rc = 0;

    util_argv0_set(argv[0]);

    if (argc <= 1) {
        return concat_fd(0, "<stdin>") < 0 ? 1 : 0;
    }
    for (int i = 1; i < argc; i++) {
        if (cat_one(argv[i]) < 0) rc = 1;
    }
    return rc;
}
