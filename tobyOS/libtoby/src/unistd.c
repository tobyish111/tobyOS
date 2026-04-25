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

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
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

int brk(void *addr) {
    long r = toby_sc1(ABI_SYS_BRK, (long)(uintptr_t)addr);
    if (r == 0 && addr != 0) { errno = ENOMEM; return -1; }
    return 0;
}

void *sbrk(long inc) {
    /* Read current. */
    long cur = toby_sc1(ABI_SYS_BRK, 0);
    if (cur == 0) { errno = ENOMEM; return (void *)-1; }
    if (inc == 0) return (void *)(uintptr_t)cur;
    long want = cur + inc;
    long got  = toby_sc1(ABI_SYS_BRK, want);
    if (got != want) { errno = ENOMEM; return (void *)-1; }
    return (void *)(uintptr_t)cur;     /* POSIX: return PREVIOUS break */
}

/* ---- _exit ------------------------------------------------------- *
 *
 * Bypasses atexit handlers (see exit() in stdlib.c for the full path). */

/* DEBUG (M25C): trace each _exit() call with caller RIP + code so we
 * can pin down a mystery exit(2) coming out of the env test program.
 * Uses raw write() so no stdio buffering can swallow the line. */
static void __toby_exit_trace(int code, void *caller) {
    char buf[80];
    static const char hex[] = "0123456789abcdef";
    int  i = 0;
    buf[i++] = '[';  buf[i++] = '_';  buf[i++] = 'e'; buf[i++] = 'x';
    buf[i++] = 'i';  buf[i++] = 't';  buf[i++] = ']';
    buf[i++] = ' ';  buf[i++] = 'c';  buf[i++] = 'o'; buf[i++] = 'd';
    buf[i++] = 'e';  buf[i++] = '=';
    /* Decimal code (handles negative). */
    int c = code;
    if (c < 0) { buf[i++] = '-'; c = -c; }
    char tmp[12]; int ti = 0;
    if (c == 0) { tmp[ti++] = '0'; }
    while (c) { tmp[ti++] = (char)('0' + (c % 10)); c /= 10; }
    while (ti) buf[i++] = tmp[--ti];
    buf[i++] = ' '; buf[i++] = 'c'; buf[i++] = 'a'; buf[i++] = 'l';
    buf[i++] = 'l'; buf[i++] = 'e'; buf[i++] = 'r'; buf[i++] = '=';
    buf[i++] = '0'; buf[i++] = 'x';
    unsigned long p = (unsigned long)(uintptr_t)caller;
    for (int s = 60; s >= 0; s -= 4) {
        buf[i++] = hex[(p >> s) & 0xf];
    }
    buf[i++] = '\n';
    /* Direct write to fd=2 (stderr); ignore failures. */
    (void)toby_sc3(ABI_SYS_WRITE, 2, (long)(uintptr_t)buf, (long)i);
}

void _exit(int code) {
    __toby_exit_trace(code, __builtin_return_address(0));
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
 * The kernel exposes no "is this fd a tty?" syscall yet (M25A scope).
 * We approximate by treating stdin/stdout/stderr as ttys and everything
 * else as not. Good enough for the M25E-era ports of cat/ls which only
 * use isatty to decide whether to colourise output. */
int isatty(int fd) {
    if (fd >= 0 && fd <= 2) return 1;
    return 0;
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
    return 0;
}

int stat(const char *path, struct stat *out) {
    if (!out) { errno = EFAULT; return -1; }
    struct abi_stat k;
    long r = toby_sc2(ABI_SYS_STAT, (long)(uintptr_t)path, (long)(uintptr_t)&k);
    if (__toby_check(r) < 0) return -1;
    return abi_stat_to_user(&k, out);
}

int fstat(int fd, struct stat *out) {
    if (!out) { errno = EFAULT; return -1; }
    struct abi_stat k;
    long r = toby_sc2(ABI_SYS_FSTAT, fd, (long)(uintptr_t)&k);
    if (__toby_check(r) < 0) return -1;
    return abi_stat_to_user(&k, out);
}

int mkdir(const char *path, mode_t mode) {
    return (int)__toby_check(
        toby_sc2(ABI_SYS_MKDIR, (long)(uintptr_t)path, (long)mode));
}
