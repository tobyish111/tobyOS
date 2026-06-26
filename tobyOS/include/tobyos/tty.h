/* tty.h -- console TTY line discipline + Linux termios (Track B / B21).
 *
 * Before B21 the console was a raw byte pipe: reads returned keystrokes the
 * instant they arrived (no line editing, no echo) and `ioctl` always failed
 * with ENOTTY, so a Linux libc decided stdin/stdout were NOT a terminal
 * (`isatty()==0` -> fully buffered, no interactive behaviour). This file adds
 * a single kernel-side controlling TTY for the text console:
 *
 *   - a real `struct ktermios` (the 36-byte Linux TCGETS layout) with sane
 *     defaults (ICANON|ECHO|ISIG cooked mode), readable/writable via the
 *     TCGETS/TCSETS ioctls -- which is what makes `isatty()` return true and
 *     lets programs flip raw mode (editors, password prompts);
 *   - a cooked line discipline in tty_console_read(): canonical mode buffers a
 *     whole line with VERASE/VKILL editing + ECHO, VEOF gives EOF; raw mode
 *     (ICANON cleared) delivers bytes immediately under VMIN;
 *   - termios-driven signal generation: the keyboard ISIG path consults the
 *     live termios + VINTR/VQUIT/VSUSP control chars, so Ctrl-C is SIGINT only
 *     while ISIG is set (a raw-mode app sees the literal byte instead);
 *   - TIOCGWINSZ (terminal size from the console grid) + TIOCGPGRP/TIOCSPGRP
 *     (foreground process group) so size-aware / job-control software works.
 *
 * There is one console TTY (the kernel text console). PTYs are out of scope.
 */

#ifndef TOBYOS_TTY_H
#define TOBYOS_TTY_H

#include <tobyos/types.h>

/* Linux kernel `struct termios` (what TCGETS/TCSETS exchange): 36 bytes.
 * c_cc has NCCS==19 in the kernel ABI (musl's userspace struct is larger but
 * only relies on these leading fields). */
#define TTY_NCCS 19

struct ktermios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[TTY_NCCS];
};

/* c_lflag bits (Linux x86-64). */
#define TTY_ISIG    0x0001u
#define TTY_ICANON  0x0002u
#define TTY_ECHO    0x0008u
#define TTY_ECHOE   0x0010u
#define TTY_ECHOK   0x0020u
#define TTY_ECHONL  0x0040u

/* c_iflag bits. */
#define TTY_INLCR   0x0040u
#define TTY_IGNCR   0x0080u
#define TTY_ICRNL   0x0100u

/* c_oflag bits. */
#define TTY_OPOST   0x0001u
#define TTY_ONLCR   0x0004u

/* c_cc[] indices (Linux). */
#define TTY_VINTR   0
#define TTY_VQUIT   1
#define TTY_VERASE  2
#define TTY_VKILL   3
#define TTY_VEOF    4
#define TTY_VTIME   5
#define TTY_VMIN    6
#define TTY_VSUSP   10

/* ioctl request numbers (Linux x86-64). */
#define TTY_TCGETS      0x5401u
#define TTY_TCSETS      0x5402u
#define TTY_TCSETSW     0x5403u
#define TTY_TCSETSF     0x5404u
#define TTY_TCFLSH      0x540Bu
#define TTY_TIOCGPGRP   0x540Fu
#define TTY_TIOCSPGRP   0x5410u
#define TTY_TIOCGWINSZ  0x5413u
#define TTY_TIOCSWINSZ  0x5414u
#define TTY_FIONREAD    0x541Bu

struct kwinsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* One-time init: install the default cooked termios. Idempotent. */
void tty_init(void);

/* Keyboard ISIG hook. If `c` is a signal-generating control char under the
 * LIVE termios (ISIG set + c matches VINTR/VQUIT/VSUSP), sends the signal to
 * the foreground process group and returns true (the char is consumed, not
 * buffered). Otherwise returns false so the caller buffers the raw byte. */
bool tty_handle_isig(char c);

/* Cooked console read honouring the termios (canonical vs raw, ECHO, VERASE/
 * VKILL/VEOF). Writes into a KERNEL buffer (the caller copies to user).
 * Returns bytes read, 0 on EOF (canonical VEOF on an empty line), or a
 * negative errno-ish value (EINTR_RET) if interrupted by a signal. */
long tty_console_read(char *buf, size_t n);

/* ioctl on a console TTY fd. `arg` is the raw user pointer (ioctl arg3).
 * Returns 0 on success or a negative ABI errno. Unknown requests -> -ENOTTY. */
long tty_console_ioctl(unsigned long cmd, unsigned long arg);

#endif /* TOBYOS_TTY_H */
