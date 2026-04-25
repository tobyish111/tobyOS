/* errno.h -- libtoby's POSIX-shape errno surface.
 *
 * The integer codes are deliberately byte-identical to the kernel's
 * ABI_E* values in <tobyos/abi/abi.h>. The libc init code asserts the
 * equivalence at startup (see libtoby/src/init.c) so a future drift
 * between userland and kernel is caught loudly, not silently.
 *
 * `errno` is a single int per process. tobyOS doesn't have user threads
 * yet, so no TLS gymnastics are needed -- a plain global suffices. The
 * wrapper layer in libtoby/src/syscall.c is the only writer. */

#ifndef LIBTOBY_ERRNO_H
#define LIBTOBY_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

extern int errno;

#define EPERM            1
#define ENOENT           2
#define EIO              5
#define E2BIG            7
#define EBADF            9
#define ECHILD          10
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EBUSY           16
#define EEXIST          17
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define EMFILE          24
#define ENOSPC          28
#define EROFS           30
#define EPIPE           32
#define ERANGE          34
#define ENAMETOOLONG    36
#define ENOSYS          38

/* GNU-flavoured strerror_r returns the message pointer. */
const char *strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_ERRNO_H */
