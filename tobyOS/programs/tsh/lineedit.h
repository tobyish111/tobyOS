/* lineedit.h -- interactive line editing for /bin/tsh.
 *
 * host.c's prompt loop read stdin one byte at a time in canonical mode, so
 * the terminal did the editing and the shell got a finished line. That gives
 * backspace and nothing else: no history, no cursor keys, none of the control
 * bindings anyone who uses bash has in their fingers.
 *
 * This is the readline subset that matters, kept OUT of src/shell.c on
 * purpose -- that file is the language, compiled into both the kernel shell
 * and /bin/tsh, and terminal state belongs to the host. */
#ifndef TSH_LINEEDIT_H
#define TSH_LINEEDIT_H
#include <stddef.h>

#define TSH_LE_EOF   (-1)   /* ^D on an empty line */
#define TSH_LE_INTR  (-2)   /* ^C */

/* True when stdin is a terminal we can drive. When false the caller must keep
 * using its plain read loop: a pipe or a file has no cursor to move, and
 * putting a non-tty into raw mode would break `echo cmd | tsh`. */
int  tsh_lineedit_available(void);

/* Read one line. Returns its length (buf NUL-terminated, no newline), or
 * TSH_LE_EOF / TSH_LE_INTR. Raw mode is entered and restored inside, so the
 * tty is in its normal state whenever a command runs -- job control depends
 * on the terminal delivering ^C/^Z to the foreground job, not to us. */
long tsh_readline(const char *prompt, char *buf, size_t cap);
#endif
