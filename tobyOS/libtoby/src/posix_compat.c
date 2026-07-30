/* posix_compat.c -- POSIX compatibility shim for libtoby (Phase 3 M3.1).
 *
 * Provides core POSIX/libc functions that ported applications expect,
 * implemented via tobyOS syscalls. This is the minimal surface needed
 * so ported programs (dash shell, coreutils) can link and run.
 *
 * All functions here are marked weak so the canonical implementations
 * in unistd.c, process.c, signal.c etc. take precedence when both
 * object files are linked together.
 */

#include "libtoby_internal.h"
#include <string.h>

/* ---- File descriptor operations ---- */

__attribute__((weak))
int open(const char *path, int flags, ...) {
    return (int)__toby_check(toby_sc2(ABI_SYS_OPEN, (long)path, flags));
}

__attribute__((weak))
int close(int fd) {
    return (int)__toby_check(toby_sc1(ABI_SYS_CLOSE, fd));
}

__attribute__((weak))
long sys_read(int fd, void *buf, unsigned long count) {
    return __toby_check(toby_sc3(ABI_SYS_READ, fd, (long)buf, (long)count));
}

__attribute__((weak))
long sys_write(int fd, const void *buf, unsigned long count) {
    return __toby_check(toby_sc3(ABI_SYS_WRITE, fd, (long)buf, (long)count));
}

__attribute__((weak))
long lseek(int fd, long offset, int whence) {
    return __toby_check(toby_sc3(ABI_SYS_LSEEK, fd, offset, whence));
}

__attribute__((weak))
int dup(int oldfd) {
    return (int)__toby_check(toby_sc1(ABI_SYS_DUP, oldfd));
}

__attribute__((weak))
int dup2(int oldfd, int newfd) {
    return (int)__toby_check(toby_sc2(ABI_SYS_DUP2, oldfd, newfd));
}

__attribute__((weak))
int pipe(int pipefd[2]) {
    return (int)__toby_check(toby_sc1(ABI_SYS_PIPE, (long)pipefd));
}

/* ---- Process control ---- */

__attribute__((weak))
int getpid(void) {
    return (int)toby_sc0(ABI_SYS_GETPID);
}

__attribute__((weak))
int getppid(void) {
    return (int)toby_sc0(ABI_SYS_GETPPID);
}

__attribute__((weak))
int fork(void) {
    return (int)__toby_check(toby_sc0(ABI_SYS_FORK));
}

__attribute__((weak))
int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)__toby_check(toby_sc3(ABI_SYS_EXECVE, (long)path, (long)argv, (long)envp));
}

__attribute__((weak))
int waitpid(int pid, int *status, int options) {
    return (int)__toby_check(toby_sc3(ABI_SYS_WAITPID, pid, (long)status, options));
}

/* ---- Memory management ---- */

__attribute__((weak))
void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long offset) {
    long rv = toby_sc6(ABI_SYS_MMAP, (long)addr, (long)len, prot, flags, fd, offset);
    if (rv < 0 && rv >= -4095) return (void *)-1L;
    return (void *)rv;
}

__attribute__((weak))
int munmap(void *addr, unsigned long len) {
    return (int)__toby_check(toby_sc2(ABI_SYS_MUNMAP, (long)addr, (long)len));
}

__attribute__((weak))
int mprotect(void *addr, unsigned long len, int prot) {
    return (int)__toby_check(toby_sc3(ABI_SYS_MPROTECT, (long)addr, (long)len, prot));
}

/* Simple sbrk using mmap as backing */
static char *g_brk_base = 0;
static char *g_brk_current = 0;
/* Slice 62: 16 MiB -> 64 MiB. chromewin's resizable display decodes
 * full-window JPEG frames (a 1278x697 maximized frame is a 3.56 MiB RGBA
 * plus stb temporaries, alongside the previous frame still held); the
 * 16 MiB pool fragmented under frame churn and every post-resize decode
 * failed with OOM -- 1006 silent "JPEG decode failed"s in one run. The
 * pool is a lazy mmap reservation: untouched pages cost nothing, so the
 * larger ceiling is free for every other libtoby program. */
#define SBRK_POOL_SIZE (64 * 1024 * 1024)

__attribute__((weak))
void *sbrk(long increment) {
    if (!g_brk_base) {
        g_brk_base = (char *)mmap(0, SBRK_POOL_SIZE, 3, 0x22, -1, 0);
        if (g_brk_base == (char *)-1L) return (void *)-1L;
        g_brk_current = g_brk_base;
    }
    if (increment == 0) return g_brk_current;
    char *old = g_brk_current;
    if (g_brk_current + increment > g_brk_base + SBRK_POOL_SIZE)
        return (void *)-1L;
    g_brk_current += increment;
    return old;
}

/* ---- Signal handling ---- */

__attribute__((weak))
int kill(int pid, int sig) {
    return (int)__toby_check(toby_sc2(ABI_SYS_KILL, pid, sig));
}

/* ---- Directory operations ---- */

__attribute__((weak))
int chdir(const char *path) {
    return (int)__toby_check(toby_sc1(ABI_SYS_CHDIR, (long)path));
}

__attribute__((weak))
char *getcwd(char *buf, unsigned long size) {
    long rv = toby_sc2(ABI_SYS_GETCWD, (long)buf, (long)size);
    if (rv < 0) return 0;
    return buf;
}

/* ---- Time ---- */

__attribute__((weak))
unsigned int sleep(unsigned int seconds) {
    toby_sc1(ABI_SYS_NANOSLEEP, (long)((uint64_t)seconds * 1000000000ULL));
    return 0;
}

__attribute__((weak))
int usleep(unsigned long usec) {
    toby_sc1(ABI_SYS_NANOSLEEP, (long)(usec * 1000UL));
    return 0;
}

/* ---- String utilities expected by ported software ---- */

__attribute__((weak))
int isatty(int fd) {
    (void)fd;
    return (fd >= 0 && fd <= 2) ? 1 : 0;
}

__attribute__((weak))
int fcntl(int fd, int cmd, ...) {
    (void)fd; (void)cmd;
    return 0;
}

__attribute__((weak))
int ioctl(int fd, unsigned long request, ...) {
    (void)fd; (void)request;
    return 0;
}
