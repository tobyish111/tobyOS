/* linux-xattr -- extended attributes, fd identity, mknod, fexecve
 * (Phase E, 2026-08-22).
 *
 *   bit0  setxattr/getxattr round-trip (path forms) + size probe
 *   bit1  listxattr contains the name; removexattr really removes (ENODATA)
 *   bit2  fsetxattr/fgetxattr through a descriptor -- the exact calls
 *         cp -a makes, and what forced struct file to learn its path
 *   bit3  rename(2) carries attributes along; unlink drops them (a fresh
 *         same-path file starts clean)
 *   bit4  /proc/self/fd/N readlink answers the real opened path (it was
 *         "/" forever) + mknod(S_IFREG) creates a stat-able regular file
 *   bit5  fexecve() re-executes this binary (glibc's /proc/self/fd/N
 *         execve fallback, powered by bit4's identity)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/xattr.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern char **environ;

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "kid") == 0) _exit(42);

    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *fa = "/tmp/xa-file";
    int fd = open(fa, O_CREAT | O_RDWR | O_TRUNC, 0644);
    (void)!write(fd, "x", 1);

    /* ---- bit0: set/get round-trip + size probe (path forms) ---- */
    {
        int sr = setxattr(fa, "user.color", "teal", 4, 0);
        char v[16];
        memset(v, 0, sizeof v);
        ssize_t gr = getxattr(fa, "user.color", v, sizeof v);
        ssize_t probe = getxattr(fa, "user.color", NULL, 0);
        printf("xa: set=%d get=%zd val=%.4s probe=%zd\n", sr, gr, v, probe);
        if (sr == 0 && gr == 4 && memcmp(v, "teal", 4) == 0 && probe == 4)
            bits |= 1;
    }

    /* ---- bit1: list contains it; remove really removes ---- */
    {
        char lst[128];
        ssize_t lr = listxattr(fa, lst, sizeof lst);
        int found = 0;
        for (ssize_t o = 0; o < lr; o += (ssize_t)strlen(lst + o) + 1)
            if (strcmp(lst + o, "user.color") == 0) found = 1;
        int rr = removexattr(fa, "user.color");
        errno = 0;
        ssize_t after = getxattr(fa, "user.color", NULL, 0);
        int gone = (after < 0 && errno == ENODATA);
        printf("xa: list=%zd found=%d rm=%d after-errno=%d\n",
               lr, found, rr, errno);
        if (lr > 0 && found && rr == 0 && gone) bits |= 2;
    }

    /* ---- bit2: the f* forms (cp -a's shape) ---- */
    {
        int sr = fsetxattr(fd, "user.mark", "byfd", 4, 0);
        char v[16];
        memset(v, 0, sizeof v);
        ssize_t gr = fgetxattr(fd, "user.mark", v, sizeof v);
        printf("xa: fset=%d fget=%zd val=%.4s\n", sr, gr, v);
        if (sr == 0 && gr == 4 && memcmp(v, "byfd", 4) == 0) bits |= 4;
    }

    /* ---- bit3: rename carries, unlink drops ---- */
    {
        const char *fb = "/tmp/xa-moved";
        int ok_carry = 0, ok_drop = 0;
        if (rename(fa, fb) == 0) {
            char v[16];
            ssize_t gr = getxattr(fb, "user.mark", v, sizeof v);
            ok_carry = (gr == 4 && memcmp(v, "byfd", 4) == 0);
            unlink(fb);
            int f2 = open(fb, O_CREAT | O_RDWR, 0644);
            errno = 0;
            ssize_t g2 = getxattr(fb, "user.mark", NULL, 0);
            ok_drop = (g2 < 0 && errno == ENODATA);
            if (f2 >= 0) close(f2);
            unlink(fb);
        }
        printf("xa: rename-carry=%d unlink-drop=%d\n", ok_carry, ok_drop);
        if (ok_carry && ok_drop) bits |= 8;
    }

    /* ---- bit4: fd link identity + mknod(REG) ---- */
    {
        const char *fc = "/tmp/xa-ident";
        int f3 = open(fc, O_CREAT | O_RDWR, 0644);
        char lnk[64], tgt[128];
        snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", f3);
        memset(tgt, 0, sizeof tgt);
        ssize_t rl = readlink(lnk, tgt, sizeof tgt - 1);
        int ident = (rl > 0 && strcmp(tgt, fc) == 0);
        if (f3 >= 0) close(f3);
        unlink(fc);

        struct stat st;
        int mr = mknod("/tmp/xa-node", S_IFREG | 0644, 0);
        int ms = stat("/tmp/xa-node", &st);
        int node = (mr == 0 && ms == 0 && S_ISREG(st.st_mode));
        unlink("/tmp/xa-node");
        printf("xa: fdlink=%d (%s) mknod=%d\n", ident, tgt, node);
        if (ident && node) bits |= 16;
    }

    /* ---- bit5: fexecve re-executes this binary ---- */
    {
        int ok = 0;
        /* Under the LXPOSIX harness argv[0] is the bare program name, not
         * a path -- open it absolutely or the bit fails before the fork. */
        const char *self = (argv[0] && argv[0][0] == '/')
                               ? argv[0] : "/bin/linux-xattr";
        int xfd = open(self, O_RDONLY);
        if (xfd >= 0) {
            pid_t k = fork();
            if (k == 0) {
                char *kargv[] = { argv[0], (char *)"kid", NULL };
                fexecve(xfd, kargv, environ);
                _exit(9);              /* fexecve came back: fail loudly */
            }
            int wst = 0;
            if (k > 0 && waitpid(k, &wst, 0) == k)
                ok = (WIFEXITED(wst) && WEXITSTATUS(wst) == 42);
            close(xfd);
        }
        printf("xa: fexecve=%d\n", ok);
        if (ok) bits |= 32;
    }

    if (fd >= 0) close(fd);
    unlink(fa);

    printf("LXXATTR: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
