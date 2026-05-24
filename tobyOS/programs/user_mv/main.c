#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int copy_file(const char *src, const char *dst) {
    int fdin = open(src, 0);
    if (fdin < 0) return -1;

    int fdout = open(dst, 1 | 0x040 | 0x200);
    if (fdout < 0) { close(fdin); return -1; }

    char buf[4096];
    long n;
    while ((n = read(fdin, buf, sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            long w = write(fdout, buf + off, n - off);
            if (w <= 0) { close(fdin); close(fdout); return -1; }
            off += w;
        }
    }

    close(fdin);
    close(fdout);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: mv src dst\n");
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    if (copy_file(src, dst) < 0) {
        fprintf(stderr, "mv: cannot copy '%s' to '%s': %s\n", src, dst, strerror(errno));
        return 1;
    }

    if (unlink(src) < 0) {
        fprintf(stderr, "mv: copied but cannot remove '%s': %s\n", src, strerror(errno));
        return 1;
    }

    return 0;
}
