#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static inline long sys_chmod(const char *path, int mode) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"(29L), "D"((long)path), "S"((long)mode)
        : "rcx", "r11", "memory");
    return r;
}

static int parse_mode(const char *s) {
    int mode = 0;
    while (*s) {
        if (*s < '0' || *s > '7') return -1;
        mode = (mode << 3) | (*s - '0');
        s++;
    }
    return mode;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: chmod mode path\n");
        fprintf(stderr, "  mode: octal (e.g. 755)\n");
        return 1;
    }

    int mode = parse_mode(argv[1]);
    if (mode < 0) {
        fprintf(stderr, "chmod: invalid mode '%s'\n", argv[1]);
        return 1;
    }

    long r = sys_chmod(argv[2], mode);
    if (r < 0) {
        fprintf(stderr, "chmod: cannot chmod '%s': error %ld\n", argv[2], -r);
        return 1;
    }

    return 0;
}
