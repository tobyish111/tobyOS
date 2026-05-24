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

#endif /* TOBYOS_SHELL_H */
