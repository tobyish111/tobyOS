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
#define EINTR            4
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
#define EAGAIN          11
#define ESRCH            3
#define EDEADLK         35
#define EBUSY           16
#define ETIMEDOUT      110
/* 2026-08-24. Both were already reachable from the kernel and had no name
 * here, so callers could only compare against a bare number: EXDEV is what
 * vfs_rename answers for a cross-mount move (mv's copy fallback keys off
 * it) and ENOTEMPTY is what rmdir(2) answers for a populated directory. */
#define EXDEV           18
#define ENOTEMPTY       39
/* 2026-09-01: the loaders learned "#!". ENOEXEC is what execve answers for
 * a file that is neither script, ELF nor PE (execvp then runs it through
 * the shell, per POSIX); ELOOP is a #! chain deeper than the kernel will
 * follow. */
#define ENOEXEC          8
#define ELOOP           40
/* 2026-10-04: /dev/tty without a controlling terminal is ENXIO, and the
 * terminal ioctls on a non-terminal ENOTTY; ESPIPE is lseek on a pipe. */
#define ENXIO            6
#define ENOTTY          25
#define ESPIPE          29
/* 2026-10-12 (batch 25): a value too large for its type -- GNU truncate
 * says it of a size past the largest. Linux's number, musl's words in
 * strerror. */
#define EOVERFLOW       75

/* GNU-flavoured strerror_r returns the message pointer. */
const char *strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_ERRNO_H */
