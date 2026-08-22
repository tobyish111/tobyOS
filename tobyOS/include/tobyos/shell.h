/* shell.h -- minimal interactive shell.
 *
 * Drains the keyboard ring buffer one character at a time, runs an
 * in-place line editor, and dispatches whitespace-tokenised commands
 * against a static command table. No history, no pipes, no globbing --
 * just enough to drive the kernel from a real keyboard.
 */

#ifndef TOBYOS_SHELL_H
#define TOBYOS_SHELL_H

#include <tobyos/types.h>

typedef void (*shell_write_fn_t)(const char *s, void *ctx);

void shell_set_output(shell_write_fn_t fn, void *ctx);
void shell_write(const char *s);
void shell_printf(const char *fmt, ...);
/* Print the welcome line and the first prompt. Call once after every
 * other subsystem is up. */
void shell_init(void);

/* Pull every queued key from the keyboard, update the editor, and run
 * the line if the user pressed Enter. Returns immediately if there is
 * nothing to do. Safe (and intended) to call from the idle loop. */
void shell_poll(void);

/* Milestone 25C: synchronously run a single command line through the
 * exact same dispatch path used by the keyboard-driven editor. Used
 * by the boot harness to validate the shell launch flow (PATH lookup,
 * env propagation, foreground wait) without needing a real keyboard.
 *
 * `line` is copied internally so the caller's buffer is untouched.
 * Lines longer than the shell's editor buffer are rejected. */
void shell_run_test_line(const char *line);

/* Deliver an asynchronous signal to the shell (called from interrupt context
 * or the signal subsystem). The trap handler will run at the next safe point. */
void shell_deliver_signal(int sig);

/* True when `pid` is the process the shell itself runs as. Used by signal
 * delivery to route a signal aimed at the shell to its trap dispatcher. */
bool shell_owns_pid(int pid);

/* ---- hosted entry points ------------------------------------------------
 *
 * shell.c is compiled twice: into the kernel (driven by shell_poll above) and
 * into userspace /bin/tsh (driven by programs/tsh/host.c). One language, two
 * hosts -- so the shell's expansion, arithmetic, globbing and control flow
 * exist exactly once and cannot drift between the two.
 *
 * The host provides the other half of the seam: kmalloc, vfs_*, proc_spawn,
 * file_*, kprintf. Real subsystems in the kernel; libtoby syscall wrappers in
 * userspace. See programs/tsh/host.c. */

/* Initialise state without printing a banner or prompt (scripts must emit
 * nothing the oracle would not). `argv0` becomes $0. */
void shell_init_hosted(const char *argv0);

/* Declare whether this is a terminal session. Defaults to true, because the
 * kernel shell always is; a script or `-c` must clear it. Alias expansion is
 * keyed on this -- bash expands aliases interactively and ignores them in a
 * script unless `shopt -s expand_aliases` says otherwise. */
void shell_set_interactive_hosted(bool on);

/* Run a script file / a single -c line. Both return the exit status. */
int  shell_run_script_hosted(const char *path);
int  shell_run_line_hosted(const char *text);

/* Is `set -o ignoreeof` on? The interactive read loop asks at each EOF. */
bool shell_opt_ignoreeof_hosted(void);

/* Read a shell variable (PS1/PS2 are rarely exported, so getenv cannot see
 * them). NULL when unset. */
const char *shell_get_var_hosted(const char *name);

/* Continuation test for the interactive loop: 0 = complete, 1 = incomplete
 * (join with a newline), 2 = backslash continuation (drop the backslash). */
int shell_line_incomplete_hosted(const char *s);

/* True after a line whose execution asked the shell to exit; *status gets
 * the exit status. The interactive loop must check this after every line. */
bool shell_wants_exit_hosted(int *status);

/* Set $1..$n before running. */
int  shell_set_args_hosted(int argc, char **argv);

#endif /* TOBYOS_SHELL_H */
