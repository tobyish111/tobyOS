/* libtoby/src/termios.c -- terminal I/O control.
 *
 * tcgetattr/tcsetattr are the kernel's TCGETS/TCSETS (src/tty.c for the
 * console, src/pty.c for ptys); anything else is not a terminal. */

#include <termios.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "libtoby_internal.h"

/* NOT A TERMINAL IS AN ERROR (batch 25). A failed TCGETS/TCSETS used to
 * fall back to a userspace "shadow" table for fds 0-7 and report success:
 * tcgetattr said a file, a pipe or /dev/null was a terminal with made-up
 * settings, and `stty < /dev/null` listed them where Linux's says "Not a
 * tty". The kernel answers for its terminals (the console and ptys) and
 * says ENOTTY for everything else; that answer is now the caller's. */

/* The kernel's half of `struct termios`: exactly the 36 bytes TCGETS and
 * TCSETS exchange. Our public struct starts with these same fields in the
 * same order, so the ioctl can point straight at it. */
#define TCGETS_REQ 0x5401UL
#define TCSETS_REQ 0x5402UL

int tcgetattr(int fd, struct termios *t) {
    if (!t) { errno = EINVAL; return -1; }
    if (fd < 0) { errno = EBADF; return -1; }

    /* THE REAL TERMINAL, via TCGETS.
     *
     * This used to be guarded on ABI_SYS_TCGETATTR -- a macro that is
     * DEFINED NOWHERE IN THE TREE, so the block compiled to nothing and
     * every call fell through to a userspace shadow table (gone). The
     * effect was that tcsetattr RETURNED SUCCESS WITHOUT EVER REACHING
     * THE KERNEL: callers believed they had raw mode while the tty stayed
     * canonical with echo on. It went unnoticed because the only programs
     * that set raw mode here were ported ones using musl's real syscalls;
     * the first libtoby program to need it was the native editor.
     *
     * The kernel has implemented TCGETS/TCSETS for the console and for
     * ptys all along (src/tty.c, src/pty.c). */
    return ioctl(fd, TCGETS_REQ, t) == 0 ? 0 : -1;    /* errno: the kernel's */
}

int tcsetattr(int fd, int action, const struct termios *t) {
    if (!t) { errno = EINVAL; return -1; }
    if (fd < 0) { errno = EBADF; return -1; }
    (void)action;

    /* TCSETS for all three actions. TCSANOW/TCSADRAIN/TCSAFLUSH differ
     * only in what they do with pending I/O, and this kernel's ttys have
     * nothing to drain that would change the result. Applying the
     * settings is the part callers depend on -- and the part that was
     * missing entirely. */
    return ioctl(fd, TCSETS_REQ, (void *)(unsigned long)(uintptr_t)t) == 0 ? 0 : -1;
}

speed_t cfgetispeed(const struct termios *t) {
    return t ? t->c_ispeed : 0;
}

speed_t cfgetospeed(const struct termios *t) {
    return t ? t->c_ospeed : 0;
}

int cfsetispeed(struct termios *t, speed_t speed) {
    if (!t) { errno = EINVAL; return -1; }
    t->c_ispeed = speed;
    return 0;
}

int cfsetospeed(struct termios *t, speed_t speed) {
    if (!t) { errno = EINVAL; return -1; }
    t->c_ospeed = speed;
    return 0;
}

int tcdrain(int fd) {
    (void)fd;
    return 0;
}

int tcflush(int fd, int queue_selector) {
    (void)fd; (void)queue_selector;
    return 0;
}
