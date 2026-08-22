/* unistd.h -- libtoby's POSIX-shape syscall surface.
 *
 * Most functions here are ~5-line wrappers over the kernel ABI in
 * <tobyos/abi/abi.h>: trap into the kernel, convert -ABI_E* to
 * errno + -1, return. The exceptions are sbrk() (which composes
 * brk() with bookkeeping of the previous break) and the *sleep
 * helpers (which translate units before invoking SYS_NANOSLEEP). */

#ifndef LIBTOBY_UNISTD_H
#define LIBTOBY_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

#define SEEK_SET       0
#define SEEK_CUR       1
#define SEEK_END       2

ssize_t read   (int fd, void *buf, size_t n);
ssize_t write  (int fd, const void *buf, size_t n);
int     close  (int fd);
off_t   lseek  (int fd, off_t off, int whence);

int     dup    (int oldfd);
int     dup2   (int oldfd, int newfd);

pid_t   getpid (void);
pid_t   getppid(void);

/* Scheduling priority (tobyOS extension). prio is HIGHER-runs-first, 0 ==
 * normal; see ABI_PRIO_* in <tobyos/abi/abi.h>. pid<=0 means the caller.
 * toby_setprio returns the applied priority, or <0 on error (e.g. -EPERM).
 * toby_getprio returns the priority, or a value <= -1000 if pid not found. */
int     toby_setprio(int pid, int prio);
int     toby_getprio(int pid);

int     chdir  (const char *path);
char   *getcwd (char *buf, size_t cap);
int     unlink (const char *path);
int     pipe   (int fds[2]);
int     rmdir  (const char *path);
int     access (const char *path, int mode);
int     truncate(const char *path, off_t length);
int     ftruncate(int fd, off_t length);
int     chmod  (const char *path, mode_t mode);
int     chown  (const char *path, uid_t owner, gid_t group);
int     rename (const char *oldpath, const char *newpath);
int     link   (const char *oldpath, const char *newpath);
int     symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);

int     setpgid(pid_t pid, pid_t pgid);
pid_t   getpgid(pid_t pid);
pid_t   getpgrp(void);
/* tty/pty control. Declared here rather than a separate <sys/ioctl.h>;
 * the request constants live with the caller. */
int     ioctl(int fd, unsigned long req, void *argp);
uid_t   getuid (void);
int     setuid (uid_t uid);      /* slice 14: real privilege drop */
int     setgid (gid_t gid);
uid_t   geteuid(void);
gid_t   getgid (void);
gid_t   getegid(void);

/* Socket API (UDP only). */
int     socket  (int domain, int type, int protocol);
int     bind    (int sockfd, unsigned short port_be);
ssize_t sendto  (int sockfd, const void *buf, size_t len,
                 unsigned int dst_ip_be, unsigned short dst_port_be);
ssize_t recvfrom(int sockfd, void *buf, size_t len, void *src_out);

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

void   *sbrk   (long inc);
int     brk    (void *addr);

unsigned int sleep (unsigned int seconds);
int          usleep(unsigned int usecs);

void    _exit  (int code) __attribute__((noreturn));

pid_t   fork   (void);

int     isatty (int fd);

/* ---- exec family (Milestone 25C) ---------------------------------- *
 *
 * The exec family replaces the current process image. Internally
 * this is modelled as "spawn-then-wait-then-exit" for compatibility
 * with programs that use the fork+exec pattern. From the parent's
 * view, the calling process "becomes" the child. */
int     execv  (const char *path, char *const argv[]);
int     execvp (const char *file, char *const argv[]);
int     execve (const char *path, char *const argv[], char *const envp[]);

/* Process & environment globals exported by crt0.S. Useful for tools
 * that want to walk envp directly (e.g. /usr/bin/env). environ is the
 * canonical POSIX symbol; setenv/unsetenv/putenv keep it consistent. */
extern int    __toby_argc;
extern char **__toby_argv;
extern char **__toby_envp;
extern char **environ;

/* ---- POSIX getopt (Milestone 25E) -------------------------------- *
 *
 * Single-character option parsing. The optstring grammar is the
 * subset every sbase-style port uses:
 *
 *     "abc:d"   -- a, b, d are flags; c takes an argument
 *
 * On return:
 *     option char    -- next option matched
 *     -1             -- no more options
 *     '?'            -- unrecognised option (or missing argument);
 *                       optopt holds the offending character.
 *     ':'            -- missing argument (only if optstring starts
 *                       with ':')
 *
 * No GNU long-option extensions, no permutation of non-option
 * args. Stops at the first non-option (or "--") so the caller
 * iterates `argv[optind .. argc-1]` afterwards.  */
extern char *optarg;
extern int   optind;
extern int   optopt;
extern int   opterr;

int getopt(int argc, char *const argv[], const char *optstring);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_UNISTD_H */
