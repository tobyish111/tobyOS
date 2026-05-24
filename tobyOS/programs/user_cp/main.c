#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>

int main(int argc, char **argv) {
    int verbose = 0;
    int opt;
    while ((opt = getopt(argc, argv, "v")) != -1) {
        switch (opt) {
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr, "usage: cp [-v] src dst\n");
            return 1;
        }
    }

    if (optind + 2 > argc) {
        fprintf(stderr, "usage: cp [-v] src dst\n");
        return 1;
    }

    const char *src = argv[optind];
    const char *dst = argv[optind + 1];

    int fdin = open(src, 0);
    if (fdin < 0) {
        fprintf(stderr, "cp: cannot open '%s': %s\n", src, strerror(errno));
        return 1;
    }

    int fdout = open(dst, 1 | 0x040 | 0x200);
    if (fdout < 0) {
        fprintf(stderr, "cp: cannot create '%s': %s\n", dst, strerror(errno));
        close(fdin);
        return 1;
    }

    char buf[4096];
    long total = 0;
    long n;
    while ((n = read(fdin, buf, sizeof(buf))) > 0) {
        long written = 0;
        while (written < n) {
            long w = write(fdout, buf + written, n - written);
            if (w <= 0) {
                fprintf(stderr, "cp: write error: %s\n", strerror(errno));
                close(fdin);
                close(fdout);
                return 1;
            }
            written += w;
        }
        total += n;
    }

    close(fdin);
    close(fdout);

    if (verbose)
        printf("'%s' -> '%s' (%ld bytes)\n", src, dst, total);

    return 0;
}
