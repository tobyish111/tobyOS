#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <getopt.h>
#include <errno.h>

static int recursive = 0;
static int verbose = 0;
/* -f was missing entirely, and `rm -f` is one of the commonest lines in
 * any shell script: it means "remove it, and a file that is not there is
 * not an error". Without it every `rm -f x` printed a diagnostic and
 * exited 1 when x did not exist -- precisely the case it exists to make
 * quiet. */
static int force = 0;

static int remove_path(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        if (force) return 0;      /* not there is not a failure under -f */
        fprintf(stderr, "rm: cannot stat '%s': %s\n", path, strerror(errno));
        return 1;
    }

    /* S_ISDIR, not `st.st_mode & S_IFDIR`. The bit test happens to give the
     * right answer while only S_IFREG and S_IFDIR exist, because their
     * values share no bits -- but it is testing one bit of a 4-bit type
     * FIELD, so the first type that includes 0x4000 in its encoding (a
     * symlink is 0xA000; a socket 0xC000) would read as a directory and
     * `rm` would recurse into it. */
    if (S_ISDIR(st.st_mode) && recursive) {
        DIR *d = opendir(path);
        if (!d) {
            fprintf(stderr, "rm: cannot open dir '%s': %s\n", path, strerror(errno));
            return 1;
        }
        struct dirent *ent;
        int err = 0;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            if (remove_path(child) != 0) err = 1;
        }
        closedir(d);
        if (rmdir(path) < 0) {
            fprintf(stderr, "rm: cannot remove dir '%s': %s\n", path, strerror(errno));
            return 1;
        }
        if (verbose) printf("removed directory '%s'\n", path);
        return err;
    } else if (S_ISDIR(st.st_mode)) {
        fprintf(stderr, "rm: '%s' is a directory (use -r)\n", path);
        return 1;
    }

    if (unlink(path) < 0) {
        if (force) return 0;
        fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (verbose) printf("removed '%s'\n", path);
    return 0;
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "rRfvi")) != -1) {
        switch (opt) {
        case 'r': case 'R': recursive = 1; break;
        case 'f': force = 1; break;
        case 'i': break;   /* prompting needs a terminal we may not have */
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr, "usage: rm [-rRfvi] path...\n");
            return 1;
        }
    }

    if (optind >= argc) {
        /* `rm -f` with nothing to remove succeeds, which is what lets
         * `rm -f $MAYBE_EMPTY` appear unguarded in a script. */
        if (force) return 0;
        fprintf(stderr, "usage: rm [-rRfvi] path...\n");
        return 1;
    }

    int ret = 0;
    for (int i = optind; i < argc; i++) {
        if (remove_path(argv[i]) != 0) ret = 1;
    }
    return ret;
}
