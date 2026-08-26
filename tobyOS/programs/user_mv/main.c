/* mv -- move or rename.
 *
 * 2026-08-24: A REAL MOVE. This was copy-then-unlink, which meant
 *
 *   - `mv dir newname` was impossible: open() on a directory is EISDIR,
 *     so the copy failed and the message blamed the copy;
 *   - the file's storage was doubled before the original was freed, so
 *     moving a large file inside a full filesystem failed for no reason
 *     a user could see;
 *   - mode and ownership were dropped -- the destination came out with
 *     whatever open() defaulted to;
 *   - an interrupted copy left BOTH names present holding different
 *     bytes, and the exit code said failure without saying which half.
 *
 * rename(2) reached the native ABI on the same day (ABI_SYS_RENAME), so
 * the normal path is now one atomic call that carries the inode. The copy
 * fallback stays for exactly one case -- EXDEV, a rename across mounts,
 * which the VFS genuinely cannot do -- and `mv /tmp/x /data/x` is an
 * ordinary thing to want.
 *
 * Usage:  mv SRC DST
 *         mv SRC... DIRECTORY
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return (s && s[1]) ? s + 1 : p;
}

/* Copy SRC to DST preserving the permission bits, then verify the byte
 * count. Used only for a cross-mount move. */
static int copy_file(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) { errno = EISDIR; return -1; }

    int fdin = open(src, O_RDONLY);
    if (fdin < 0) return -1;
    int fdout = open(dst, O_WRONLY | O_CREAT | O_TRUNC, (int)(st.st_mode & 0777));
    if (fdout < 0) { close(fdin); return -1; }

    char buf[4096];
    long n, total = 0;
    while ((n = read(fdin, buf, sizeof buf)) > 0) {
        long off = 0;
        while (off < n) {
            long w = write(fdout, buf + off, (size_t)(n - off));
            if (w <= 0) { close(fdin); close(fdout); return -1; }
            off += w;
        }
        total += n;
    }
    close(fdin);
    close(fdout);
    if (n < 0) return -1;
    /* A short copy that reported success is how a move loses data. */
    if (total != (long)st.st_size) { errno = EIO; return -1; }
    return 0;
}

static int move_one(const char *src, const char *dst) {
    if (rename(src, dst) == 0) return 0;

    /* EXDEV is the ONLY error worth retrying differently: the two paths
     * are on different mounts, so there is no inode to relink and a copy
     * is the honest equivalent. Anything else is a real failure and gets
     * reported as itself rather than retried into a second message. */
    if (errno != EXDEV) {
        fprintf(stderr, "mv: cannot move '%s' to '%s': %s\n",
                src, dst, strerror(errno));
        return 1;
    }
    if (is_dir(src)) {
        fprintf(stderr, "mv: cannot move directory '%s' across filesystems\n",
                src);
        return 1;
    }
    if (copy_file(src, dst) != 0) {
        fprintf(stderr, "mv: cannot copy '%s' to '%s': %s\n",
                src, dst, strerror(errno));
        return 1;
    }
    if (unlink(src) != 0) {
        /* Say exactly what state the filesystem is in. "mv failed" after a
         * successful copy would leave the user guessing whether the
         * destination is safe to use. */
        fprintf(stderr, "mv: '%s' copied to '%s' but the original could not "
                        "be removed: %s\n", src, dst, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    /* Accept and ignore a lone "--", so `mv -- -weirdname dst` works. */
    int argi = 1;
    if (argc > 1 && strcmp(argv[1], "--") == 0) argi = 2;

    int n = argc - argi;
    if (n < 2) {
        fprintf(stderr, "usage: mv SRC DST\n"
                        "       mv SRC... DIRECTORY\n");
        return 1;
    }

    const char *dst = argv[argc - 1];
    int dst_is_dir = is_dir(dst);

    if (n > 2 && !dst_is_dir) {
        fprintf(stderr, "mv: target '%s' is not a directory\n", dst);
        return 1;
    }

    int rc = 0;
    for (int i = argi; i < argc - 1; i++) {
        const char *src = argv[i];
        if (dst_is_dir) {
            char full[512];
            snprintf(full, sizeof full, "%s/%s", dst, base_name(src));
            if (move_one(src, full) != 0) rc = 1;
        } else {
            if (move_one(src, dst) != 0) rc = 1;
        }
    }
    return rc;
}
