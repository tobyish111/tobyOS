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

int open(const char *path, int flags, ...);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_FCNTL_H */
