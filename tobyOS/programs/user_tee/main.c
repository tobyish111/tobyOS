#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>

int main(int argc, char **argv) {
    int append = 0;
    int opt;
    while ((opt = getopt(argc, argv, "a")) != -1) {
        switch (opt) {
        case 'a': append = 1; break;
        default:
            fprintf(stderr, "usage: tee [-a] file\n");
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "usage: tee [-a] file\n");
        return 1;
    }

    const char *path = argv[optind];
    int flags = 1 | 0x040; /* O_WRONLY | O_CREAT */
    if (append)
        flags |= 0x400; /* O_APPEND */
    else
        flags |= 0x200; /* O_TRUNC */

    int fd = open(path, flags);
    if (fd < 0) {
        fprintf(stderr, "tee: cannot open '%s': %s\n", path, strerror(errno));
        return 1;
    }

    char buf[4096];
    long n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        write(1, buf, n);
        long off = 0;
        while (off < n) {
            long w = write(fd, buf + off, n - off);
            if (w <= 0) {
                fprintf(stderr, "tee: write error\n");
                close(fd);
                return 1;
            }
            off += w;
        }
    }

    close(fd);
    return 0;
}
