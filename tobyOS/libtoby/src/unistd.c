/* libtoby/src/unistd.c -- POSIX-shape wrappers for the kernel ABI.
 *
 * Each function is a 1-2 line trampoline: pack arguments into the
 * SysV register order expected by toby_sc*, invoke the syscall, and
 * funnel the negative return through __toby_check so errno + -1 is
 * the user-visible failure mode.
 *
 * This file is the "everything POSIX" home for syscall wrappers --
 * fcntl.h's open() lives here (rather than in a separate fcntl.c)
 * because there's no good reason to split it out. */

#include <sys/sysmacros.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <errno.h>
#include <stdarg.h>
#include "libtoby_internal.h"

/* Legacy-return helper.
 *
 * sys_read / sys_write / sys_close still return the pre-25A "-1 means
 * any error" sentinel rather than -ABI_E* codes. For libc we surface
 * that as the most-likely errno so user code gets a meaningful value
 * via perror() etc.
 *
 * (Cleaning up the kernel-side return convention for these legacy
 * syscalls is a M25C cleanup item; deliberately not changing it here
 * because /bin/hello and other pre-libc programs depend on the
 * existing -1 behaviour.) */
static ssize_t legacy_io_check(long rv, int default_errno) {
    if (rv < 0) {
        /* Kernel may have already returned a -ABI_E* (>= -4095). Prefer
         * that if so; otherwise fall back to the supplied default. */
        if ((unsigned long)rv > (unsigned long)-4096L) {
            errno = (int)(-rv);
        } else {
            errno = default_errno;
        }
        return -1;
    }
    return rv;
}

ssize_t read(int fd, void *buf, size_t n) {
    return legacy_io_check(
        toby_sc3(ABI_SYS_READ, fd, (long)(uintptr_t)buf, (long)n), EIO);
}

ssize_t write(int fd, const void *buf, size_t n) {
    return legacy_io_check(
        toby_sc3(ABI_SYS_WRITE, fd, (long)(uintptr_t)buf, (long)n), EIO);
}

int close(int fd) {
    long rv = toby_sc1(ABI_SYS_CLOSE, fd);
    return (int)legacy_io_check(rv, EBADF);
}

off_t lseek(int fd, off_t off, int whence) {
    return (off_t)__toby_check(toby_sc3(ABI_SYS_LSEEK, fd, (long)off, whence));
}

int dup(int oldfd) {
    return (int)__toby_check(toby_sc1(ABI_SYS_DUP, oldfd));
}

int dup2(int oldfd, int newfd) {
    return (int)__toby_check(toby_sc2(ABI_SYS_DUP2, oldfd, newfd));
}

pid_t getpid(void) {
    return (pid_t)toby_sc0(ABI_SYS_GETPID);
}

pid_t getppid(void) {
    return (pid_t)toby_sc0(ABI_SYS_GETPPID);
}

int toby_setprio(int pid, int prio) {
    return (int)toby_sc2(ABI_SYS_SETPRIORITY, (long)pid, (long)prio);
}

int toby_getprio(int pid) {
    return (int)toby_sc1(ABI_SYS_GETPRIORITY, (long)pid);
}

int chdir(const char *path) {
    return (int)__toby_check(toby_sc1(ABI_SYS_CHDIR, (long)(uintptr_t)path));
}

char *getcwd(char *buf, size_t cap) {
    long r = toby_sc2(ABI_SYS_GETCWD, (long)(uintptr_t)buf, (long)cap);
    if (r < 0) { errno = (int)(-r); return 0; }
    return buf;
}

int unlink(const char *path) {
    return (int)__toby_check(toby_sc1(ABI_SYS_UNLINK, (long)(uintptr_t)path));
}

/* ---- brk / sbrk -------------------------------------------------- *
 *
 * Kernel SYS_BRK semantics:
 *   - new_brk == 0  -> return current break (query)
 *   - new_brk != 0  -> set break to new_brk; return new break, or 0
 *                      on failure (out of range / OOM).
 *
 * POSIX semantics:
 *   - brk(addr)     -> 0 on success, -1 + errno=ENOMEM on failure.
 *   - sbrk(0)       -> current break.
 *   - sbrk(inc)     -> previous break, or (void*)-1 + errno=ENOMEM. */

/* MEMORY IS NOT AN EFFECT: the break moves without flushing stdio first
 * (K5, syscall.c, writes pending output before every other syscall). A
 * program's output has nothing to be ordered against here, and flushing
 * made WHERE a buffered line broke depend on whether malloc happened to
 * grow the heap: /bin/tsh's printf put half a line out ahead of its own
 * warning, where bash's stdio holds the line (logs/b13 q53). */
int brk(void *addr) {
    long r = toby_sc_raw3(ABI_SYS_BRK, (long)(uintptr_t)addr, 0, 0);
    if (r == 0 && addr != 0) { errno = ENOMEM; return -1; }
    return 0;
}

void *sbrk(long inc) {
    /* Read current. */
    long cur = toby_sc_raw3(ABI_SYS_BRK, 0, 0, 0);
    if (cur == 0) { errno = ENOMEM; return (void *)-1; }
    if (inc == 0) return (void *)(uintptr_t)cur;
    long want = cur + inc;
    long got  = toby_sc_raw3(ABI_SYS_BRK, want, 0, 0);
    if (got != want) { errno = ENOMEM; return (void *)-1; }
    return (void *)(uintptr_t)cur;     /* POSIX: return PREVIOUS break */
}

/* ---- _exit ------------------------------------------------------- *
 *
 * Bypasses atexit handlers (see exit() in stdlib.c for the full path). */

/* THE M25C EXIT TRACE IS GONE (2026-09-13).
 *
 * It wrote "[_exit] code=<n> caller=0x<rip>" to fd 2 on every _exit(), to
 * pin down one mystery exit(2) coming out of the env test program. That
 * question was answered long ago; the line outlived it and became a tax on
 * every harness in the tree.
 *
 * It fired precisely when stderr was NOT a tty -- which is the definition
 * of a captured stream, so it polluted exactly the transcripts that get
 * compared and never the screen where a human might have noticed it. Its
 * own comment claimed it stayed "for captured streams (harnesses read
 * it)", and no harness ever read it: programs/oilspec/main.c skips the
 * line in two places, and logs/kernstrip.sh strips it for both distro
 * gates. Three filters, zero consumers.
 *
 * It also cost a real verdict. On the real EliteDesk the shred section
 * graded FAIL with
 *
 *     ...|/tmp/shr: removed|[_exit] code=0 caller=0x4070a4|gone|
 *
 * against an expectation identical but for that middle field. A diagnostic
 * that no one reads, which corrupts the streams that ARE read, is not a
 * diagnostic -- it is noise with a build dependency. Filtering it in three
 * places was the wrong repair; not emitting it is the right one.
 *
 * (oilspec's two filters are left alone: they cost nothing and still guard
 * against a stale binary built before this commit.) */
void _exit(int code) {
    toby_sc1(ABI_SYS_EXIT, code);
    for (;;) { __asm__ volatile ("hlt"); }   /* unreachable */
}

/* ---- sleep / usleep ---------------------------------------------- */

unsigned int sleep(unsigned int seconds) {
    uint64_t ns = (uint64_t)seconds * 1000000000ull;
    toby_sc1(ABI_SYS_NANOSLEEP, (long)ns);
    return 0;
}

int usleep(unsigned int usecs) {
    uint64_t ns = (uint64_t)usecs * 1000ull;
    toby_sc1(ABI_SYS_NANOSLEEP, (long)ns);
    return 0;
}

/* ---- isatty ------------------------------------------------------ *
 *
 * ASK THE KERNEL. This used to return 1 for any of fd 0/1/2 whatever they
 * were pointing at, under a comment saying no syscall could tell -- while
 * _exit() twenty lines above was already using the right probe: TCGETS
 * succeeds on a console or a pty and returns ENOTTY on anything else.
 *
 * The stub was not harmless. Every tool that changes behaviour for a
 * terminal was taking the terminal branch when redirected: `ls` printed
 * COLUMNS into a pipe instead of one name per line, `more` paged instead
 * of copying its input through, `nohup` always redirected to nohup.out,
 * and `watch` would have looped forever in a script. */
int isatty(int fd) {
    if (fd < 0) return 0;
    char tio_probe[64];
    return toby_sc3(ABI_SYS_IOCTL, fd, 0x5401 /* TCGETS */,
                    (long)(uintptr_t)tio_probe) >= 0;
}

/* ---- open ------------------------------------------------------- *
 *
 * open() takes a variadic third arg (mode) only when O_CREAT is set.
 * We always pull it (with a sane default of 0644 if absent) to keep
 * the call shape uniform inside the kernel. */
int open(const char *path, int flags, ...) {
    int mode = 0644;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return (int)__toby_check(
        toby_sc3(ABI_SYS_OPEN, (long)(uintptr_t)path, flags, mode));
}

/* ---- stat / fstat / mkdir --------------------------------------- *
 *
 * The kernel returns its own struct abi_stat. We translate field-by-
 * field into libtoby's struct stat so userland is decoupled from the
 * kernel-internal layout. */

static int abi_stat_to_user(const struct abi_stat *k, struct stat *u) {
    u->st_size = (off_t)k->size;
    u->st_mode = (mode_t)k->mode;
    u->st_uid  = (uid_t)k->uid;
    u->st_gid  = (gid_t)k->gid;
    u->st_mtime = (time_t)k->mtime;
    u->st_atime = (time_t)k->atime;
    u->st_nlink = (nlink_t)k->nlink;
    u->st_ino   = (ino_t)k->ino;
    /* the mount's number split as the Linux personality splits it, 12 bits
     * of major over 20 of minor, so a native `stat -c %d` and a Linux one
     * agree; 0 stays 0, "no answer" */
    u->st_dev   = k->dev ? (dev_t)makedev((unsigned)(k->dev >> 20) & 0xfffu,
                                          (unsigned)k->dev & 0xfffffu) : 0;
    u->st_ctime   = (time_t)k->ctime;
    u->st_blocks  = (blkcnt_t)k->blocks;
    u->st_blksize = (blksize_t)k->blksize;
    u->st_rdev    = (k->rdev_major || k->rdev_minor)
                  ? (dev_t)makedev(k->rdev_major, k->rdev_minor) : 0;
    return 0;
}

/* stat, lstat and fstat ask through ABI_SYS_XSTAT, which writes the whole
 * struct abi_stat this libtoby was built with (batch 25); the old calls
 * write only its first ABI_STAT_V1_SIZE bytes. KIND 0 stat, 1 lstat, 2
 * fstat. */
static int xstat(int kind, long what, struct stat *out) {
    if (!out) { errno = EFAULT; return -1; }
    struct abi_stat k;
    memset(&k, 0, sizeof k);
    long r = toby_sc4(ABI_SYS_XSTAT, kind, what, (long)(uintptr_t)&k, (long)sizeof k);
    if (__toby_check(r) < 0) return -1;
    return abi_stat_to_user(&k, out);
}

int stat(const char *path, struct stat *out) {
    return xstat(0, (long)(uintptr_t)path, out);
}

int fstat(int fd, struct stat *out) {
    return xstat(2, fd, out);
}

/* A FIFO at PATH, MODE less the umask (ABI_SYS_MKFIFO). */
int mkfifo(const char *path, mode_t mode) {
    return (int)__toby_check(toby_sc2(ABI_SYS_MKFIFO, (long)(uintptr_t)path,
                                      (long)mode));
}

/* mknod(2) and mknodat(2), the kernel's in Linux's order of answers: a
 * device node for root alone, as on Linux (finding 214; there was none) */
int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev) {
    return (int)__toby_check(toby_sc4(ABI_SYS_MKNOD, dirfd, (long)(uintptr_t)path,
                                      (long)mode, (long)dev));
}
int mknod(const char *path, mode_t mode, dev_t dev) {
    return mknodat(-100, path, mode, dev);           /* AT_FDCWD */
}

/* chroot(2), the kernel's (finding 214; there was none) */
int chroot(const char *path) {
    return (int)__toby_check(toby_sc1(ABI_SYS_CHROOT, (long)(uintptr_t)path));
}

/* unshare(2), the kernel's (finding 241; there was none): <sched.h> */
int unshare(int flags) {
    return (int)__toby_check(toby_sc1(ABI_SYS_UNSHARE, (long)(unsigned)flags));
}

/* The twelve extended-attribute calls (finding 225; there were none):
 * ABI_SYS_XATTR carries Linux's number for each -- 188 setxattr, then
 * lsetxattr, fsetxattr, getxattr (191), lgetxattr, fgetxattr, listxattr
 * (194), llistxattr, flistxattr, removexattr (197), lremovexattr,
 * fremovexattr -- and a set's flags above its size. */
static long xa_call(long nr, long a, long b, long c, long d) {
    return __toby_check(toby_sc5(ABI_SYS_XATTR, nr, a, b, c, d));
}
static long xa_set(long nr, long target, const char *name, const void *value,
                   size_t size, int flags) {
    return xa_call(nr, target, (long)(uintptr_t)name, (long)(uintptr_t)value,
                   (long)((uint64_t)size | (uint64_t)(uint32_t)flags << 32));
}
int setxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    return (int)xa_set(188, (long)(uintptr_t)path, name, value, size, flags);
}
int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    return (int)xa_set(189, (long)(uintptr_t)path, name, value, size, flags);
}
int fsetxattr(int fd, const char *name, const void *value, size_t size, int flags) {
    return (int)xa_set(190, fd, name, value, size, flags);
}
ssize_t getxattr(const char *path, const char *name, void *value, size_t size) {
    return xa_call(191, (long)(uintptr_t)path, (long)(uintptr_t)name, (long)(uintptr_t)value, (long)size);
}
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size) {
    return xa_call(192, (long)(uintptr_t)path, (long)(uintptr_t)name, (long)(uintptr_t)value, (long)size);
}
ssize_t fgetxattr(int fd, const char *name, void *value, size_t size) {
    return xa_call(193, fd, (long)(uintptr_t)name, (long)(uintptr_t)value, (long)size);
}
ssize_t listxattr(const char *path, char *list, size_t size) {
    return xa_call(194, (long)(uintptr_t)path, (long)(uintptr_t)list, (long)size, 0);
}
ssize_t llistxattr(const char *path, char *list, size_t size) {
    return xa_call(195, (long)(uintptr_t)path, (long)(uintptr_t)list, (long)size, 0);
}
ssize_t flistxattr(int fd, char *list, size_t size) {
    return xa_call(196, fd, (long)(uintptr_t)list, (long)size, 0);
}
int removexattr(const char *path, const char *name) {
    return (int)xa_call(197, (long)(uintptr_t)path, (long)(uintptr_t)name, 0, 0);
}
int lremovexattr(const char *path, const char *name) {
    return (int)xa_call(198, (long)(uintptr_t)path, (long)(uintptr_t)name, 0, 0);
}
int fremovexattr(int fd, const char *name) {
    return (int)xa_call(199, fd, (long)(uintptr_t)name, 0, 0);
}

/* The link itself, S_IFLNK; stat() follows it. There was no lstat, and
 * stat called a link a regular file, so nothing native could see one. */
int lstat(const char *path, struct stat *out) {
    return xstat(1, (long)(uintptr_t)path, out);
}

int mkdir(const char *path, mode_t mode) {
    return (int)__toby_check(
        toby_sc2(ABI_SYS_MKDIR, (long)(uintptr_t)path, (long)mode));
}

/* ---- pipe ------------------------------------------------------- *
 *
 * The kernel's SYS_PIPE takes a pointer to an int[2] and fills it
 * with [read_fd, write_fd]. Returns 0 on success, -1 on failure. */

int pipe(int fds[2]) {
    long rv = toby_sc1(ABI_SYS_PIPE, (long)(uintptr_t)fds);
    return (int)__toby_check(rv);
}

/* ---- truncate / ftruncate --------------------------------------- *
 *
 * 2026-08-24: real syscalls. These were ENOSYS stubs with the comment
 * "the kernel doesn't expose these yet" -- which stopped being true when
 * the Linux personality got truncate(2)/ftruncate(2) against the same
 * VFS, so busybox could shorten a file and a native tobyOS program could
 * not. ABI_SYS_TRUNCATE/FTRUNCATE close that. */

int truncate(const char *path, off_t length) {
    if (length < 0) { errno = EINVAL; return -1; }
    return (int)__toby_check(
        toby_sc2(ABI_SYS_TRUNCATE, (long)(uintptr_t)path, (long)length));
}

int ftruncate(int fd, off_t length) {
    if (length < 0) { errno = EINVAL; return -1; }
    return (int)__toby_check(toby_sc2(ABI_SYS_FTRUNCATE, (long)fd, (long)length));
}

/* ---- rmdir ------------------------------------------------------ *
 *
 * 2026-08-24: a real rmdir. This used to be `return unlink(path)`, so
 * `rmdir somefile` DELETED THE FILE rather than reporting ENOTDIR --
 * every script that probes with rmdir before falling back to unlink was
 * quietly destroying data. The kernel does the type check. */

int rmdir(const char *path) {
    return (int)__toby_check(toby_sc1(ABI_SYS_RMDIR, (long)(uintptr_t)path));
}

/* ---- access ----------------------------------------------------- *
 *
 * Check file accessibility using stat() and permission bits. */

int access(const char *pathname, int mode) {
    struct stat st;
    if (stat(pathname, &st) < 0) return -1;
    if (mode == F_OK) return 0;
    if ((mode & R_OK) && !(st.st_mode & 0444)) { errno = EACCES; return -1; }
    if ((mode & W_OK) && !(st.st_mode & 0222)) { errno = EACCES; return -1; }
    if ((mode & X_OK) && !(st.st_mode & 0111)) { errno = EACCES; return -1; }
    return 0;
}

/* ---- chmod / chown ---------------------------------------------- */

int chmod(const char *path, mode_t mode) {
    return (int)__toby_check(
        toby_sc2(ABI_SYS_CHMOD, (long)(uintptr_t)path, (long)mode));
}

int chown(const char *path, uid_t owner, gid_t group) {
    return (int)__toby_check(
        toby_sc3(ABI_SYS_CHOWN, (long)(uintptr_t)path, (long)owner, (long)group));
}

/* getgroups(2): the supplementary groups, as a login set them */
int getgroups(int size, gid_t list[]) {
    return (int)__toby_check(
        toby_sc2(ABI_SYS_GETGROUPS, (long)size, (long)(uintptr_t)list));
}

/* setgroups(2): privileged, as Linux's (finding 203) */
int setgroups(size_t size, const gid_t *list) {
    return (int)__toby_check(
        toby_sc2(ABI_SYS_SETGROUPS, (long)size, (long)(uintptr_t)list));
}

/* initgroups(3): GROUP and every /etc/group entry that names USER, as
 * glibc's -- what a login and su give the account (finding 203) */
int initgroups(const char *user, gid_t group) {
    gid_t g[64];
    size_t n = 0;
    g[n++] = group;
    int fd = open("/etc/group", O_RDONLY);
    if (fd >= 0) {
        static char buf[16384];
        ssize_t r, len = 0;
        while (len < (ssize_t)sizeof buf - 1 &&
               (r = read(fd, buf + len, sizeof buf - 1 - (size_t)len)) > 0)
            len += r;
        close(fd);
        buf[len > 0 ? len : 0] = '\0';
        size_t ul = user ? strlen(user) : 0;
        for (char *s = buf; ul && *s && n < 64; ) {
            char *e = strchr(s, '\n');
            if (e) *e = '\0';
            /* NAME:PASSWORD:GID:MEMBER,MEMBER... */
            char *c1 = strchr(s, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : 0;
            char *c3 = c2 ? strchr(c2 + 1, ':') : 0;
            if (c3 && c2[1] >= '0' && c2[1] <= '9') {
                gid_t gid = 0;
                for (char *q = c2 + 1; q < c3 && *q >= '0' && *q <= '9'; q++)
                    gid = gid * 10 + (gid_t)(*q - '0');
                for (char *m = c3 + 1; *m; ) {
                    size_t ml = strcspn(m, ",\r");
                    if (ml == ul && !strncmp(m, user, ul)) {
                        size_t k = 0;
                        while (k < n && g[k] != gid) k++;
                        if (k == n) g[n++] = gid;
                        break;
                    }
                    m += ml;
                    if (*m) m++;
                }
            }
            if (!e) break;
            s = e + 1;
        }
    }
    return setgroups(n, g);
}

/* lchown(2): a symbolic link itself, where chown follows one */
int lchown(const char *path, uid_t owner, gid_t group) {
    return (int)__toby_check(
        toby_sc3(ABI_SYS_LCHOWN, (long)(uintptr_t)path, (long)owner, (long)group));
}

/* Always succeeds; the kernel answers with the previous mask. */
mode_t umask(mode_t mask) {
    return (mode_t)toby_sc1(ABI_SYS_UMASK, (long)mask);
}

/* ---- rename ----------------------------------------------------- *
 *
 * 2026-08-24: the real thing. This was a read-copy-write-unlink
 * emulation, which is not a rename: it could not move a DIRECTORY (it
 * returned EISDIR), it was not atomic, it doubled the file's storage
 * mid-flight, and a failure part-way left both names present with
 * different contents. The kernel had vfs_rename all along -- renameat(2)
 * in the Linux personality already used it. */

int rename(const char *oldpath, const char *newpath) {
    return (int)__toby_check(toby_sc2(ABI_SYS_RENAME,
                                      (long)(uintptr_t)oldpath,
                                      (long)(uintptr_t)newpath));
}

/* ---- link / symlink / readlink ---------------------------------- *
 *
 * link() reaches ABI_SYS_LINK (native-parity batch, 2026-09-04).
 * symlink/readlink use the kernel's SYS_SYMLINK/SYS_READLINK. */

int link(const char *oldpath, const char *newpath) {
    /* ABI_SYS_LINK (2026-09-04): a real hard link where the filesystem has
     * inode indirection (tobyfs, tmpfs); -EPERM where it cannot, which is
     * exactly Linux's answer on FAT. This stubbed ENOSYS for years. */
    return (int)__toby_check(
        toby_sc2(ABI_SYS_LINK, (long)(uintptr_t)oldpath, (long)(uintptr_t)newpath));
}

int symlink(const char *target, const char *linkpath) {
    return (int)__toby_check(
        toby_sc2(ABI_SYS_SYMLINK, (long)(uintptr_t)target, (long)(uintptr_t)linkpath));
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    return (ssize_t)__toby_check(
        toby_sc3(ABI_SYS_READLINK, (long)(uintptr_t)path, (long)(uintptr_t)buf, (long)bufsiz));
}

/* ---- getuid / geteuid / getgid / getegid ------------------------ */

/* THE REAL IDS for getuid/getgid, the effective for geteuid/getegid
 * (finding 206): all four said the effective ones, so a setuid-root
 * program could not tell who ran it -- sudo, asking getuid(), let anyone
 * be root. */
uid_t getuid(void)  { return (uid_t)toby_sc0(ABI_SYS_GETRUID); }
uid_t geteuid(void) { return (uid_t)toby_sc0(ABI_SYS_GETUID); }
gid_t getgid(void)  { return (gid_t)toby_sc0(ABI_SYS_GETRGID); }
gid_t getegid(void) { return (gid_t)toby_sc0(ABI_SYS_GETGID); }

/* Slice 14: the native ABI could READ identity but never change it, which is why
 * chromewin (a native app) could not stop being root before exec'ing Chromium --
 * and Chromium refuses to enable its sandbox as root. Errors come back as a
 * negative return; mapped to -1/errno the way the rest of this file does it. */
int setuid(uid_t uid) {
    long r = toby_sc1(ABI_SYS_SETUID, (long)(unsigned)uid);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}
int setgid(gid_t gid) {
    long r = toby_sc1(ABI_SYS_SETGID, (long)(unsigned)gid);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* Job control: real process groups (the shell's `set -m`). */
int setpgid(pid_t pid, pid_t pgid) {
    long r = toby_sc2(ABI_SYS_SETPGID, (long)pid, (long)pgid);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}
pid_t getpgid(pid_t pid) {
    long r = toby_sc1(ABI_SYS_GETPGID, (long)pid);
    if (r < 0) { errno = (int)-r; return -1; }
    return (pid_t)r;
}
pid_t getpgrp(void) { return getpgid(0); }

/* setsid(2): a new session and group led by the caller, with no
 * controlling terminal; EPERM for a process-group leader (fork first). */
pid_t setsid(void) {
    long r = toby_sc0(ABI_SYS_SETSID);
    if (r < 0) { errno = (int)-r; return -1; }
    return (pid_t)r;
}

int ioctl(int fd, unsigned long req, void *argp) {
    long r = toby_sc3(ABI_SYS_IOCTL, fd, (long)req, (long)argp);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

/* ---- socket API ------------------------------------------------- */

int socket(int domain, int type, int protocol) {
    /* The kernel's socket() takes only (domain, type), so a raw socket's
     * protocol rides in the top 16 bits of `type` -- see SOCK_RAW_PROTO_SHIFT
     * in tobyos/socket.h. Discarding it (which this did) meant every raw
     * socket asked for protocol 0 and got every packet on the machine. */
    if ((type & 0xFFFF) == 3 /* SOCK_RAW */ && protocol > 0)
        type |= (protocol & 0xFF) << 16;
    return (int)__toby_check(toby_sc2(ABI_SYS_SOCKET, domain, type));
}

int bind(int sockfd, uint16_t port_be) {
    return (int)__toby_check(toby_sc2(ABI_SYS_BIND, sockfd, port_be));
}

ssize_t sendto(int sockfd, const void *buf, size_t len,
               uint32_t dst_ip_be, uint16_t dst_port_be) {
    return (ssize_t)__toby_check(
        toby_sc5(ABI_SYS_SENDTO, sockfd, (long)(uintptr_t)buf,
                 (long)len, (long)dst_ip_be, (long)dst_port_be));
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, void *src_out) {
    return (ssize_t)__toby_check(
        toby_sc4(ABI_SYS_RECVFROM, sockfd, (long)(uintptr_t)buf,
                 (long)len, (long)(uintptr_t)src_out));
}

int toby_sockopt(int fd, int opt, long val) {
    return (int)__toby_check(toby_sc3(ABI_SYS_SOCKOPT, (long)fd, (long)opt, val));
}

int toby_arp_resolve(unsigned int ip_be, unsigned char mac[6]) {
    return (int)__toby_check(toby_sc3(ABI_SYS_ARP, 0, (long)ip_be,
                                      (long)(uintptr_t)mac));
}

int toby_arp_request(unsigned int ip_be) {
    return (int)__toby_check(toby_sc3(ABI_SYS_ARP, 1, (long)ip_be, 0));
}

/* ---- niceness (batch 25): getpriority/setpriority/nice -------------------
 * The raw call answers 20 less the niceness, as Linux's does, so a
 * niceness of -1 is never mistaken for an error there. */
int getpriority(int which, int who) {
    long r = toby_sc2(ABI_SYS_GETNICE, which, who);
    if (r < 0) { errno = (int)-r; return -1; }
    return 20 - (int)r;
}

int setpriority(int which, int who, int prio) {
    long r = toby_sc3(ABI_SYS_SETNICE, which, who, prio);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* nice(3): add INC to the caller's niceness; the new one, or -1 with errno
 * (EPERM for a lower one without the privilege -- POSIX's word for it). */
int nice(int inc) {
    errno = 0;
    int cur = getpriority(0, 0);
    if (cur == -1 && errno) return -1;
    if (setpriority(0, 0, cur + inc) != 0) {
        if (errno == EACCES) errno = EPERM;
        return -1;
    }
    return getpriority(0, 0);
}
