/* ln -- make links.
 *
 *     ln [-sfnvT] target [linkname]
 *     ln [-sfnv] target... directory
 *
 * There was no ln at all: neither hard links nor symlinks could be made
 * from a shell, even though the VFS has both (the initrd itself is built
 * with hard links, and /proc is full of symlinks).
 *
 * With no linkname the link takes the target's basename in the current
 * directory, which is what makes `ln -s /usr/share/foo` do the obvious
 * thing. Linking into a directory appends the basename there.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static int sflag, fflag, vflag, nflag, Tflag;
static int status;

static const char *base_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

static int is_dir(const char *p) {
    /* -n asks for a symlink-to-a-directory to be treated as the LINK rather
     * than as the directory, which needs lstat(2). libtoby has no lstat, so
     * -n is accepted and has no effect: `ln -sfn new existing-link` will
     * put the link INSIDE the directory rather than replacing the symlink.
     * Said here rather than silently doing the wrong thing. */
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int link_one(const char *target, const char *linkname) {
    if (fflag) {
        /* Remove the existing name first; link()/symlink() will not
         * overwrite and -f promises to. Unconditional, and the result
         * ignored: a dangling symlink does not stat, but it does need
         * removing. */
        (void)unlink(linkname);
    }
    int rc = sflag ? symlink(target, linkname) : link(target, linkname);
    if (rc != 0) {
        if (!sflag && errno == ENOSYS)
            fprintf(stderr, "ln: hard links are not implemented by this "
                            "filesystem; use ln -s for a symbolic link\n");
        else
            fprintf(stderr, "ln: failed to create %s link '%s': %s\n",
                    sflag ? "symbolic" : "hard", linkname, strerror(errno));
        status = 1;
        return 1;
    }
    if (vflag) {
        if (sflag) printf("'%s' -> '%s'\n", linkname, target);
        else       printf("'%s' => '%s'\n", linkname, target);
    }
    return 0;
}

static void usage(void) {
    fprintf(stderr, "usage: ln [-sfnvT] target [linkname]\n"
                    "       ln [-sfnv] target... directory\n");
    exit(1);
}

int main(int argc, char **argv) {
    const char *args[256];
    int n = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--symbolic")) { sflag = 1; continue; }
        if (!strcmp(a, "--force"))    { fflag = 1; continue; }
        if (!strcmp(a, "--verbose"))  { vflag = 1; continue; }
        if (!strcmp(a, "--help"))     usage();
        if (a[0] == '-' && a[1]) {
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 's': sflag = 1; break;
                case 'f': fflag = 1; break;
                case 'v': vflag = 1; break;
                case 'n': nflag = 1; break;
                case 'T': Tflag = 1; break;
                case 'r': break;   /* -r (relative) needs realpath; treated as plain */
                default:
                    fprintf(stderr, "ln: invalid option -- '%c'\n", *p);
                    usage();
                }
            }
            continue;
        }
        if (n < 256) args[n++] = a;
    }

    if (n == 0) usage();

    if (n == 1) {
        /* ln -s TARGET  ->  a link named after the target, here. */
        return link_one(args[0], base_of(args[0]));
    }

    if (n == 2 && (Tflag || !is_dir(args[1])))
        return link_one(args[0], args[1]);

    /* Two or more names, the last a directory. */
    const char *dir = args[n - 1];
    if (!is_dir(dir)) {
        fprintf(stderr, "ln: target '%s' is not a directory\n", dir);
        return 1;
    }
    for (int i = 0; i < n - 1; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, base_of(args[i]));
        link_one(args[i], path);
    }
    return status;
}
