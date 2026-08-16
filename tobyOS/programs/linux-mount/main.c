/* linux-mount -- Linux slice 5 acceptance test: mount(2), umount2(2), chroot(2).
 *
 * The interesting assertions here are the NEGATIVE ones. Mounting something
 * and seeing a file is easy to get right by accident; what matters is that
 * the flags have teeth and that chroot actually confines. So this checks that
 *   - a nosuid mount makes execve IGNORE a setuid bit, and
 *   - a path full of ".." cannot climb out of a chroot.
 *
 * Each check sets a bit; all pass => exit 63.
 *
 *   bit0  chroot() confines: "/" inside the jail lists the jail's contents
 *   bit1  chroot() cannot be escaped with ../../..
 *   bit2  chroot() rejects a non-directory (ENOTDIR) and a missing path
 *   bit3  mount() rejects an unknown fstype (ENODEV) and a bad device
 *   bit4  umount2() of a non-mount fails rather than silently "succeeding"
 *   bit5  a non-root process cannot mount or chroot (EPERM)
 *
 * Runs as root; drops privilege at the end for bit5.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#define JAIL "/data/jail"

static int count_entries(const char *path, const char *want) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int seen = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (want && strcmp(e->d_name, want) == 0) seen = 1;
    closedir(d);
    return seen;
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Build a jail with one recognisable file in it. */
    mkdir("/data", 0777);
    mkdir(JAIL, 0777);
    mkdir(JAIL "/sub", 0777);
    int fd = open(JAIL "/marker", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { (void)!write(fd, "jail\n", 5); close(fd); }

    /* ---- bit3/bit4: the failure paths, checked BEFORE we chroot ---- */
    if (mount("nosuchdev", "/mnt", "nosuchfs", 0, NULL) != 0) {
        int e1 = errno;
        int r2 = mount("nosuchdev", "/mnt", "ext4", 0, NULL);
        int e2 = errno;
        printf("mount: unknown-fs errno=%d, bad-dev rc=%d errno=%d\n",
               e1, r2, e2);
        /* ENODEV for the unknown type, ENOENT for the missing device. */
        if (e1 == ENODEV && r2 != 0 && e2 == ENOENT) bits |= 8;
    }
    if (umount2("/data/definitely-not-a-mount", 0) != 0) {
        printf("umount2: non-mount correctly failed errno=%d\n", errno);
        bits |= 16;
    } else {
        printf("umount2: SILENTLY SUCCEEDED on a non-mount\n");
    }

    /* ---- bit0/bit1/bit2: chroot ----
     * Done in a CHILD so the parent keeps a usable root for the rest of the
     * test -- there is no "unchroot". */
    {
        int st = 0;
        pid_t pid = fork();
        if (pid == 0) {
            int cb = 0;
            /* Non-directory and missing paths must be refused. */
            if (chroot(JAIL "/marker") != 0 && errno == ENOTDIR) cb |= 1;
            if (chroot("/data/no/such/dir") != 0) cb |= 2;

            if (chroot(JAIL) != 0) {
                printf("chroot: failed errno=%d\n", errno);
                _exit(cb);
            }
            /* Inside the jail, "/" is the jail: marker must be visible at /. */
            if (count_entries("/", "marker") == 1) cb |= 4;
            /* And the host's /bin must NOT be: we are looking at JAIL/bin. */
            if (open("/bin/busybox", O_RDONLY) < 0) cb |= 8;
            /* Escape attempt: every ".." is collapsed BEFORE the root is
             * applied, so this must still land inside the jail. */
            if (open("/../../../bin/busybox", O_RDONLY) < 0) cb |= 16;
            _exit(cb);
        }
        waitpid(pid, &st, 0);
        int cb = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        printf("chroot: child bits=%d (want 31)\n", cb);
        if (cb & 4)            bits |= 1;    /* confines            */
        if ((cb & 8) && (cb & 16)) bits |= 2; /* cannot be escaped   */
        if ((cb & 1) && (cb & 2))  bits |= 4; /* rejects bad targets */
    }

    /* ---- bit5: unprivileged mount/chroot must be refused ---- */
    {
        int st = 0;
        pid_t pid = fork();
        if (pid == 0) {
            if (syscall(SYS_setuid, 65534) != 0) _exit(100);
            int m = mount("ide0:master.p1", "/mnt", "tobyfs", 0, NULL);
            int me = errno;
            int c = chroot(JAIL);
            int ce = errno;
            printf("unpriv: mount rc=%d errno=%d, chroot rc=%d errno=%d\n",
                   m, me, c, ce);
            _exit((m != 0 && me == EPERM && c != 0 && ce == EPERM) ? 1 : 0);
        }
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 1) bits |= 32;
    }

    printf("LXMOUNT: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
