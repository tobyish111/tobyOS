/* sys/wait.h -- libtoby's process-wait surface.
 *
 * waitpid() blocks (or polls, with WNOHANG) until the named child has
 * exited, and returns its pid. The exit code is delivered via the
 * status_out int. WIFEXITED/WEXITSTATUS unpack it.
 *
 * A child KILLED BY A SIGNAL reads as an exit with status 128+N -- that is
 * what every native program has always been given, and still is -- and
 * since 2026-09-30 the status ALSO carries the signal, in bits the other
 * macros never test: WIFSIGNALED is true and WTERMSIG names it. (So, unlike
 * POSIX, WIFEXITED is true for such a child too; native code that wants the
 * difference asks WIFSIGNALED first.) */

#ifndef LIBTOBY_SYS_WAIT_H
#define LIBTOBY_SYS_WAIT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG     0x1
#define WUNTRACED   0x2
#define WCONTINUED  0x4

/* Stop encoding is 0x10000|sig, continue is 0x20000 -- NOT the Linux low
 * bytes, which would collide with genuine exit codes (`exit 127` is
 * command-not-found's status). WIFEXITED must exclude both bits. */
#define WIFEXITED(s)    (((s) & 0x30100) == 0)
#define WEXITSTATUS(s)  ((s) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x40000) != 0)
#define WTERMSIG(s)     (((s) >> 20) & 0x7F)
#define WIFSTOPPED(s)   (((s) & 0x10000) != 0)
#define WSTOPSIG(s)     ((s) & 0xFF)
#define WIFCONTINUED(s) (((s) & 0x20000) != 0)

pid_t waitpid(pid_t pid, int *status_out, int flags);
pid_t wait(int *wstatus);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_SYS_WAIT_H */
