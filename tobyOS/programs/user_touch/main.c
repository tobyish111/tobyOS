/* touch -- create a file, or update its timestamps.
 *
 * 2026-08-24: it now does the second half. This program used to be one
 * open(O_WRONLY|O_CREAT) and a close(), which creates a missing file and
 * does NOTHING AT ALL to one that already exists -- so the single command
 * whose entire purpose is to move an mtime forward never moved one. The
 * native ABI had no way to set a timestamp until utimes(2) landed today;
 * the stub was honest about the era it was written in and stopped being
 * honest the moment `make` and `cp -u` started caring about mtimes.
 *
 *   touch FILE...      create if missing, set atime+mtime to now
 *   touch -c FILE...   never create; silently skip what is missing
 *   touch -a / -m      set only atime / only mtime
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

int main(int argc, char **argv) {
    int no_create = 0, want_a = 0, want_m = 0;
    int argi = 1;

    for (; argi < argc && argv[argi][0] == '-' && argv[argi][1]; argi++) {
        if (strcmp(argv[argi], "--") == 0) { argi++; break; }
        for (const char *f = argv[argi] + 1; *f; f++) {
            if      (*f == 'c') no_create = 1;
            else if (*f == 'a') want_a = 1;
            else if (*f == 'm') want_m = 1;
            else {
                fprintf(stderr, "touch: unknown option -%c\n", *f);
                fprintf(stderr, "usage: touch [-c] [-a] [-m] FILE...\n");
                return 1;
            }
        }
    }
    if (argi >= argc) {
        fprintf(stderr, "usage: touch [-c] [-a] [-m] FILE...\n");
        return 1;
    }
    /* Neither -a nor -m means both, as POSIX specifies. */
    if (!want_a && !want_m) { want_a = 1; want_m = 1; }

    int ret = 0;
    for (int i = argi; i < argc; i++) {
        const char *path = argv[i];
        struct stat st;
        int exists = (stat(path, &st) == 0);

        if (!exists) {
            if (no_create) continue;          /* -c: not an error */
            int fd = open(path, O_WRONLY | O_CREAT, 0644);
            if (fd < 0) {
                fprintf(stderr, "touch: cannot create '%s': %s\n",
                        path, strerror(errno));
                ret = 1;
                continue;
            }
            close(fd);
            /* A fresh file already carries the creation time, and -a/-m
             * have nothing older to preserve, so there is nothing more to
             * do for it. */
            continue;
        }

        /* Preserve whichever half was NOT asked for -- utimes sets both,
         * so `touch -m` has to hand back the file's existing atime or it
         * would silently zero it. st_atime/st_mtime carry those (added the
         * same day as utimes; before that stat had no times to give and
         * -a/-m could not have been honoured at all). */
        struct timeval now, tv[2];
        gettimeofday(&now, 0);
        tv[0].tv_sec  = want_a ? now.tv_sec : st.st_atime;   /* atime */
        tv[0].tv_usec = 0;
        tv[1].tv_sec  = want_m ? now.tv_sec : st.st_mtime;   /* mtime */
        tv[1].tv_usec = 0;

        if (utimes(path, tv) != 0) {
            fprintf(stderr, "touch: cannot set times on '%s': %s\n",
                    path, strerror(errno));
            ret = 1;
        }
    }
    return ret;
}
