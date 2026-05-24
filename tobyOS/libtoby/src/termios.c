/* libtoby/src/termios.c -- terminal I/O control.
 *
 * Stores per-fd termios state in a small table. tcgetattr/tcsetattr
 * call the kernel's SYS_TCGETATTR/SYS_TCSETATTR if available, falling
 * back to a userspace-only shadow copy for older kernels. */

#include <termios.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "libtoby_internal.h"

/* Shadow termios table for up to 8 fds. Used when the kernel doesn't
 * expose termios syscalls (pre-M25E kernels). */
#define SHADOW_MAX 8

static struct termios g_shadow[SHADOW_MAX];
static int            g_shadow_init[SHADOW_MAX];

static struct termios default_termios(void) {
    struct termios t;
    memset(&t, 0, sizeof(t));
    t.c_iflag = ICRNL;
    t.c_oflag = OPOST | ONLCR;
    t.c_cflag = CS8 | CREAD | CLOCAL;
    t.c_lflag = ECHO | ECHOE | ICANON | ISIG;
    t.c_cc[VEOF]   = 4;    /* Ctrl-D */
    t.c_cc[VERASE] = 127;  /* DEL */
    t.c_cc[VINTR]  = 3;    /* Ctrl-C */
    t.c_cc[VKILL]  = 21;   /* Ctrl-U */
    t.c_cc[VQUIT]  = 28;   /* Ctrl-\ */
    t.c_cc[VSTART] = 17;   /* Ctrl-Q */
    t.c_cc[VSTOP]  = 19;   /* Ctrl-S */
    t.c_cc[VSUSP]  = 26;   /* Ctrl-Z */
    t.c_cc[VMIN]   = 1;
    t.c_cc[VTIME]  = 0;
    t.c_ispeed = B9600;
    t.c_ospeed = B9600;
    return t;
}

int tcgetattr(int fd, struct termios *t) {
    if (!t) { errno = EINVAL; return -1; }
    if (fd < 0) { errno = EBADF; return -1; }

    /* Try kernel syscall first (ABI_SYS_TCGETATTR = 80 by convention).
     * If it returns -ENOSYS, fall back to shadow table. */
#ifdef ABI_SYS_TCGETATTR
    long rv = toby_sc2(ABI_SYS_TCGETATTR, fd, (long)(uintptr_t)t);
    if (rv != -ENOSYS) {
        if (rv < 0) { errno = (int)(-rv); return -1; }
        return 0;
    }
#endif

    if (fd < SHADOW_MAX) {
        if (!g_shadow_init[fd]) {
            g_shadow[fd] = default_termios();
            g_shadow_init[fd] = 1;
        }
        memcpy(t, &g_shadow[fd], sizeof(*t));
        return 0;
    }
    errno = EBADF;
    return -1;
}

int tcsetattr(int fd, int action, const struct termios *t) {
    if (!t) { errno = EINVAL; return -1; }
    if (fd < 0) { errno = EBADF; return -1; }
    (void)action;

#ifdef ABI_SYS_TCSETATTR
    long rv = toby_sc3(ABI_SYS_TCSETATTR, fd, action, (long)(uintptr_t)t);
    if (rv != -ENOSYS) {
        if (rv < 0) { errno = (int)(-rv); return -1; }
        return 0;
    }
#endif

    if (fd < SHADOW_MAX) {
        memcpy(&g_shadow[fd], t, sizeof(*t));
        g_shadow_init[fd] = 1;
        return 0;
    }
    errno = EBADF;
    return -1;
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
