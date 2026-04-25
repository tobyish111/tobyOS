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

int     chdir  (const char *path);
char   *getcwd (char *buf, size_t cap);
int     unlink (const char *path);

void   *sbrk   (long inc);
int     brk    (void *addr);

unsigned int sleep (unsigned int seconds);
int          usleep(unsigned int usecs);

void    _exit  (int code) __attribute__((noreturn));

int     isatty (int fd);

/* ---- exec family (Milestone 25C) ---------------------------------- *
 *
 * tobyOS does NOT have fork(), so the classic POSIX exec model
 * (replace this process's image) is approximated as
 *      pid = sys_spawn(...);
 *      sys_waitpid(pid, &status, 0);
 *      _exit(status);
 *
 * That is: the calling process spawns a child, waits for it, and
 * exits with the child's status. From the parent of *this* process
 * the result is indistinguishable from a real exec. The cost is one
 * extra PID slot during the lifetime of the child.
 *
 * On any failure (spawn returned -1, malloc failed building the
 * argv/envp arrays, ...) execv-family functions return -1 with errno
 * set; on success they DO NOT return. */
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
