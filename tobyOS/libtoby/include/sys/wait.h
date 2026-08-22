/* sys/wait.h -- libtoby's process-wait surface.
 *
 * waitpid() blocks (or polls, with WNOHANG) until the named child has
 * exited, and returns its pid. The exit code is delivered via the
 * status_out int. WIFEXITED/WEXITSTATUS unpack it.
 *
 * The kernel does not yet model signals on processes (no SIGSEGV/...
 * surfaced to siblings), so WIFSIGNALED is provided but always false.
 * It exists so future M25E ports compile unmodified. */

#ifndef LIBTOBY_SYS_WAIT_H
#define LIBTOBY_SYS_WAIT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG     0x1
#define WUNTRACED   0x2

/* Stop encoding is 0x10000|sig -- NOT the Linux 0x7f low byte, which
 * would collide with a genuine `exit 127` (command-not-found's status).
 * WIFEXITED must exclude the stop bit. */
#define WIFEXITED(s)    (((s) & 0x10100) == 0)
#define WEXITSTATUS(s)  ((s) & 0xFF)
#define WIFSIGNALED(s)  (0)
#define WTERMSIG(s)     (0)
#define WIFSTOPPED(s)   (((s) & 0x10000) != 0)
#define WSTOPSIG(s)     ((s) & 0xFF)

pid_t waitpid(pid_t pid, int *status_out, int flags);
pid_t wait(int *wstatus);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_SYS_WAIT_H */
