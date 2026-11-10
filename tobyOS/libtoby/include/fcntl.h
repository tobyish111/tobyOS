/* fcntl.h -- libtoby's open() interface.
 *
 * Constants are defined here with values copied from
 * <tobyos/abi/abi.h>. We re-export them under POSIX names so user
 * programs (and ports) don't have to know about the ABI header. The
 * libc init code asserts O_RDONLY == ABI_O_RDONLY etc. so a future
 * drift between userland and kernel is loud, not silent. */

#ifndef LIBTOBY_FCNTL_H
#define LIBTOBY_FCNTL_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define O_RDONLY    0x0
#define O_WRONLY    0x1
#define O_RDWR      0x2
#define O_ACCMODE   0x3

#define O_CREAT     0x040
#define O_EXCL      0x080
#define O_TRUNC     0x200
#define O_APPEND    0x400
/* A directory, opened as one: the native open refuses a directory
 * unless asked this way, and then hands out the descriptor the Linux
 * personality gives -- getdents lists it, read(2) says EISDIR. Linux's
 * value (the kernel's SYS_O_DIRECTORY). */
#define O_DIRECTORY 0x10000
/* Linux's values for the rest (finding 235): the native open takes them
 * as the Linux personality's does, and fcntl(F_GETFL) says them back. */
#define O_NOCTTY    0400
#define O_NONBLOCK  04000
#define O_NDELAY    O_NONBLOCK
#define O_DSYNC     010000
#define O_NOFOLLOW  0400000
#define O_CLOEXEC   02000000
#define O_SYNC      04010000

/* fcntl(2): Linux's commands, the kernel's (ABI_SYS_FCNTL). It was a
 * stub that said 0 to everything (finding 235). */
#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_GETLK          5
#define F_SETLK          6
#define F_SETLKW         7
#define F_DUPFD_CLOEXEC  1030
#define F_SETPIPE_SZ     1031
#define F_GETPIPE_SZ     1032
#define FD_CLOEXEC       1

#define F_RDLCK          0
#define F_WRLCK          1
#define F_UNLCK          2

/* a record lock, Linux's x86-64 layout */
struct flock {
    short l_type;
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};

int open(const char *path, int flags, ...);
int fcntl(int fd, int cmd, ...);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_FCNTL_H */
