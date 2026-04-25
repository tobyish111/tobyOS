/* term.h -- minimal "terminal session" abstraction (milestone 13).
 *
 * A term_session is the kernel-side backend of a GUI terminal window.
 * It is NOT a real PTY -- there is no foreground process group, no
 * termios, and no line discipline. Just two byte streams plus a tiny
 * built-in shell:
 *
 *   user keystrokes  --SYS_TERM_WRITE-->  session.in_line (line editor)
 *                                         |
 *                                         v  on '\n': dispatch builtin
 *                                         |
 *   terminal grid    <--SYS_TERM_READ---  session.out_ring  <-- echoes
 *                                                              + cmd output
 *
 * The session echoes every printable input byte into the output ring so
 * the terminal window can render what the user typed without the app
 * having to second-guess the editor state. Backspace echoes BS+SP+BS so
 * the rendering grid can handle it as a simple cell rewrite.
 *
 * Built-in commands (implemented in term.c):
 *   help, clear, about, echo, pwd, cd <path>, ls [path], cat <path>,
 *   run <path> [arg]                       (queues via gui_launch_enqueue_arg)
 *
 * `run` does NOT capture stdout -- the spawned process still writes to
 * the kernel console / serial (FILE_KIND_CONSOLE default). The terminal
 * just prints "[term] spawned pid N". This keeps the plumbing minimal;
 * full stdout-to-session redirection is a future milestone.
 *
 * Sessions are pooled (TERM_MAX_SESSIONS = 4) and wrapped in a
 * FILE_KIND_TERM fd so they follow the usual close-on-exit lifecycle.
 */

#ifndef TOBYOS_TERM_H
#define TOBYOS_TERM_H

#include <tobyos/types.h>

#define TERM_MAX_SESSIONS   4
#define TERM_CMD_MAX        256
#define TERM_OUT_RING       4096
#define TERM_CWD_MAX        128

struct term_session;

/* One-time init -- zeroes the session pool. Safe to call multiple times
 * (but only the first call does anything). */
void term_init(void);

/* Allocate a session from the pool. Writes a welcome banner + first
 * prompt into the output ring so the terminal app has something to
 * paint immediately. Returns NULL if the pool is exhausted. */
struct term_session *term_session_create(void);

/* Release a session back to the pool. Safe with NULL. The struct file
 * close path (FILE_KIND_TERM) is the normal caller. */
void term_session_close(struct term_session *s);

/* Push `n` input bytes from userspace into the session. Each byte:
 *   - '\r' or '\n'   -> finalize line, run builtin, print prompt
 *   - 0x08 / 0x7F    -> backspace (erase last cmd char, echo BS SP BS)
 *   - 0x20..0x7E     -> append to cmd buffer + echo
 *   - otherwise      -> ignored
 * Returns bytes consumed (always == n for now). */
long term_session_write_input(struct term_session *s,
                              const char *buf, size_t n);

/* Drain up to `cap` output bytes into `buf`. Returns the number of
 * bytes copied (0 if the ring is empty -- the app should yield and
 * retry). */
long term_session_read_output(struct term_session *s,
                              char *buf, size_t cap);

#endif /* TOBYOS_TERM_H */
