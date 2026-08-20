#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <getopt.h>
#include <errno.h>

static int mkpath(const char *path, int mode) {
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    int len = strlen(tmp);

    if (len > 0 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    return mkdir(tmp, mode);
}

int main(int argc, char **argv) {
    int parents = 0;
    int opt;
    while ((opt = getopt(argc, argv, "p")) != -1) {
        switch (opt) {
        case 'p': parents = 1; break;
        default:
            fprintf(stderr, "usage: mkdir [-p] path...\n");
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "usage: mkdir [-p] path...\n");
        return 1;
    }

    int ret = 0;
    for (int i = optind; i < argc; i++) {
        int r;
        if (parents)
            r = mkpath(argv[i], 0755);
        else
            r = mkdir(argv[i], 0755);

        if (r < 0) {
            /* `-p` IS IDEMPOTENT. POSIX XCU: with -p, "no error shall be
             * reported if the directory named by the operand already
             * exists". Reporting one made every `mkdir -p` in a script that
             * had run before fail -- and in the conformance gate, where bash
             * runs each case first and leaves directories outside the wiped
             * scratch behind, it made tsh fail cases bash had just passed. */
            if (parents) {
                struct stat st;
                if (stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode)) continue;
            }
            fprintf(stderr, "mkdir: cannot create '%s': %s\n", argv[i], strerror(errno));
            ret = 1;
        }
    }
    return ret;
}
