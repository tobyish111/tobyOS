#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define O_WRONLY 1
#define O_CREAT  0x040

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: touch file...\n");
        return 1;
    }

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            fprintf(stderr, "touch: cannot touch '%s': %s\n", argv[i], strerror(errno));
            ret = 1;
        } else {
            close(fd);
        }
    }
    return ret;
}
