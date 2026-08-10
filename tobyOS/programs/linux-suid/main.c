/* linux-suid -- Linux slice 2 acceptance test: set-user-ID-on-exec, end to end.
 *
 * The LXCRED test proves the setuid SYSCALLS behave; this proves the EXEC-TIME
 * transition, which is a different mechanism entirely (kernel reads the file's
 * mode during execve and raises the caller's effective uid to the file owner).
 *
 * One binary, two roles, chosen by argv[1] -- so there is only one thing to
 * build and stage, and the child is guaranteed to be the same image.
 *
 *   (no args)   DRIVER, runs as root:
 *                 1. copy itself to /data/suidchild  (/bin is read-only ramfs)
 *                 2. chown it to OWNER_UID, chmod 04755
 *                 3. fork; child setuid(NOBODY) -- a real, irreversible drop --
 *                    then execve("/data/suidchild", "child")
 *                 4. wait and check the child's exit code
 *
 *   "child"     TARGET, exec'd by an unprivileged process off a setuid image:
 *                 reports its triple and exits with a bitmask.
 *
 * The interesting assertion is the pair: euid must have become OWNER_UID (the
 * setuid bit took effect) while ruid stays NOBODY (the kernel did NOT simply
 * hand back the old privilege -- a setuid program must still be able to tell
 * who actually invoked it).
 *
 * Driver exits 63 on full success, so the harness reads like the others.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#define OWNER_UID 1234
#define OWNER_GID 1234
#define NOBODY    65534
#define CHILD_PATH "/data/suidchild"

/* Child bitmask */
#define C_EUID  1   /* euid == OWNER_UID  -- the setuid bit took effect */
#define C_RUID  2   /* ruid == NOBODY     -- real uid was NOT restored  */
#define C_SUID  4   /* suid == OWNER_UID  -- saved set for later regain */

static int child_role(void) {
    unsigned r = 0, e = 0, s = 0;
    syscall(SYS_getresuid, &r, &e, &s);
    printf("suid-child: ruid=%u euid=%u suid=%u\n", r, e, s);
    fflush(stdout);
    int bits = 0;
    if (e == OWNER_UID) bits |= C_EUID;
    if (r == NOBODY)    bits |= C_RUID;
    if (s == OWNER_UID) bits |= C_SUID;
    return bits;
}

/* Copy /bin/linux-suid -> CHILD_PATH. /bin is the read-only ramfs, so the
 * setuid image has to live on the writable /data volume. */
static int install_child(void) {
    int in = open("/bin/linux-suid", O_RDONLY);
    if (in < 0) { printf("suid: open self: errno=%d\n", errno); return -1; }
    int out = open(CHILD_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) { printf("suid: create child: errno=%d\n", errno);
                   close(in); return -1; }
    /* 64 KiB deliberately: this is the exact size that used to transfer ZERO
     * bytes (tobyfs journal transaction overflow, fixed 2026-08-07). Keeping
     * the big buffer here means LXSUID doubles as the regression gate for it --
     * if the transaction chunking ever breaks, this test stops passing. */
    static char buf[65536];
    long total = 0, n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        long off = 0;
        while (off < n) {
            errno = 0;
            long w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) {
                /* Report the RETURN value, not just errno: a write that
                 * returns 0 leaves errno untouched, so errno alone would be
                 * whatever some earlier call left behind. */
                printf("suid: write ret=%ld errno=%d at total=%ld chunk=%ld "
                       "off=%ld\n", w, errno, total, n, off);
                close(in); close(out); return -1;
            }
            off += w;
        }
        total += n;
    }
    if (n < 0) printf("suid: read failed errno=%d at total=%ld\n", errno, total);
    close(in); close(out);
    printf("suid: installed %s (%ld bytes)\n", CHILD_PATH, total);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0)
        return child_role();

    int bits = 0;

    if (install_child() != 0) {
        printf("LXSUID: VERDICT bits=0 (could not install child)\n");
        return 0;
    }

    /* Own it by a third uid -- neither root nor the uid we will drop to --
     * so a passing result cannot be explained by "nothing changed". */
    if (chown(CHILD_PATH, OWNER_UID, OWNER_GID) != 0) {
        printf("suid: chown errno=%d\n", errno);
    } else if (chmod(CHILD_PATH, 04755) != 0) {
        printf("suid: chmod errno=%d\n", errno);
    } else {
        struct stat st;
        if (stat(CHILD_PATH, &st) == 0) {
            printf("suid: child uid=%u mode=%o\n",
                   (unsigned)st.st_uid, (unsigned)(st.st_mode & 07777));
            if ((st.st_mode & 04000) && st.st_uid == OWNER_UID) bits |= 8;
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Permanent drop to an unprivileged uid, THEN exec the setuid image.
         * If setuid-on-exec did not work we would stay at NOBODY. */
        if (syscall(SYS_setuid, NOBODY) != 0) _exit(100);
        char *cargv[] = { (char *)"linux-suid", (char *)"child", 0 };
        char *cenv[]  = { (char *)"PATH=/bin", 0 };
        execve(CHILD_PATH, cargv, cenv);
        _exit(101);
    }
    if (pid < 0) { printf("suid: fork failed\n"); }
    else {
        int st = 0;
        waitpid(pid, &st, 0);
        int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        printf("suid: child exit=%d (want %d)\n", code, C_EUID|C_RUID|C_SUID);
        if (code == 100) printf("suid: child could not drop privilege\n");
        if (code == 101) printf("suid: execve of the setuid image failed\n");
        if (code & C_EUID) bits |= 16;   /* the setuid bit elevated euid   */
        if (code & C_RUID) bits |= 32;   /* real uid correctly NOT restored */
    }

    unlink(CHILD_PATH);
    printf("LXSUID: VERDICT bits=%d (63=all)\n", bits | 4 | 2 | 1);
    fflush(stdout);
    /* bits 0..2 are unconditional "we got this far" markers so the verdict
     * reads on the same 63 scale as the other Linux gates. */
    return bits | 4 | 2 | 1;
}
