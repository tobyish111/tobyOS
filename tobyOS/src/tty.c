/* tty.c -- the console controlling TTY: termios + cooked line discipline.
 * See tty.h for the why. One global TTY backs the kernel text console; its
 * input source is the shared keyboard ring (kbd_trygetc), its echo sink is
 * kputc (console + serial). */

#include <tobyos/tty.h>
#include <tobyos/keyboard.h>
#include <tobyos/console.h>
#include <tobyos/signal.h>
#include <tobyos/proc.h>
#include <tobyos/printk.h>
#include <tobyos/uaccess.h>
#include <tobyos/cpu.h>
#include <tobyos/klibc.h>

/* The single console termios. */
static struct ktermios g_tio;
static bool g_inited;

/* Pending cooked line: a completed canonical line that did not fit in the
 * caller's buffer is held here and delivered across subsequent reads. */
#define TTY_LINE_MAX 1024
static char   g_line[TTY_LINE_MAX];
static size_t g_line_len;   /* bytes of a completed line waiting */
static size_t g_line_pos;   /* delivered so far */

/* Priority input-reply queue. Terminal query responses the TTY generates
 * itself (e.g. the cursor-position report answering ESC[6n) are pushed here
 * and consumed by reads BEFORE the keyboard ring, so an interactive line
 * editor that does "write ESC[6n; read the reply" gets its answer ahead of
 * any user keystrokes already queued. */
#define TTY_REPLY_MAX 32
static char   g_reply[TTY_REPLY_MAX];
static size_t g_reply_len;
static size_t g_reply_pos;

/* Output escape-sequence scanner state (persisted across console writes). */
static int  g_esc_state;            /* 0 normal, 1 saw ESC, 2 inside CSI */
static char g_csi[16];
static int  g_csi_len;

/* Next input byte: priority reply queue first, then the keyboard ring. */
static int tty_input_next(void) {
    if (g_reply_pos < g_reply_len) return (unsigned char)g_reply[g_reply_pos++];
    return kbd_trygetc();
}

void tty_init(void) {
    if (g_inited) return;
    memset(&g_tio, 0, sizeof(g_tio));
    /* ICRNL: a bare CR from the keyboard becomes NL so Enter ends a line. */
    g_tio.c_iflag = TTY_ICRNL;
    g_tio.c_oflag = TTY_OPOST | TTY_ONLCR;
    /* CS8 | CREAD | HUPCL-ish: value isn't load-bearing for our console, but
     * give it a plausible 8N1 shape so software that inspects it isn't
     * surprised. 0x1B = B9600 in CBAUD low bits + CS8 (0x30) | CREAD (0x80). */
    g_tio.c_cflag = 0x000000BFu;
    g_tio.c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE | TTY_ECHOK;
    g_tio.c_cc[TTY_VINTR]  = 0x03;   /* ^C */
    g_tio.c_cc[TTY_VQUIT]  = 0x1c;   /* ^\ */
    g_tio.c_cc[TTY_VERASE] = 0x7f;   /* DEL */
    g_tio.c_cc[TTY_VKILL]  = 0x15;   /* ^U */
    g_tio.c_cc[TTY_VEOF]   = 0x04;   /* ^D */
    g_tio.c_cc[TTY_VMIN]   = 1;
    g_tio.c_cc[TTY_VTIME]  = 0;
    g_tio.c_cc[TTY_VSUSP]  = 0x1a;   /* ^Z */
    g_line_len = g_line_pos = 0;
    g_inited = true;
}

static inline void ensure_init(void) { if (!g_inited) tty_init(); }

/* ---- echo ------------------------------------------------------------- */

static void echo_byte(char c) {
    if (c == '\n') {
        /* ONLCR: terminals expect CR before LF. */
        if (g_tio.c_oflag & TTY_ONLCR) kputc('\r');
        kputc('\n');
    } else if (c == '\t') {
        kputc('\t');
    } else if ((unsigned char)c < 0x20) {
        /* Control chars echo as ^X when ECHO is on (canonical convention). */
        kputc('^');
        kputc((char)(c + '@'));
    } else {
        kputc(c);
    }
}

static void echo_erase(void) {
    /* ECHOE: visually rub out the last char (backspace, space, backspace). */
    kputc('\b'); kputc(' '); kputc('\b');
}

/* ---- ISIG (called from the keyboard dispatch, possibly in IRQ ctx) ---- */

bool tty_handle_isig(char c) {
    ensure_init();
    if (!(g_tio.c_lflag & TTY_ISIG)) return false;
    int sig = 0;
    if (c == (char)g_tio.c_cc[TTY_VINTR])      sig = SIGINT;
    else if (c == (char)g_tio.c_cc[TTY_VQUIT]) sig = SIGQUIT;
    else if (c == (char)g_tio.c_cc[TTY_VSUSP]) sig = SIGTSTP;
    if (!sig) return false;
    int fg = signal_get_foreground();
    if (fg > 0) signal_send_to_pid(fg, sig);
    /* Consume the control char either way: a tty never delivers ^C as data
     * while ISIG is on, and we don't want it lingering in the input ring. */
    return true;
}

/* ---- blocking single-char fetch from the keyboard ring ---------------- */
/* Returns 0..255, or <0 to mean "interrupted by a signal" (EINTR). */
static int getc_block(void) {
    int c = tty_input_next();
    while (c < 0) {
        if (signal_pending_self()) return -1;
        sti();
        hlt();
        c = tty_input_next();
    }
    return c;
}

/* ---- cooked read ------------------------------------------------------ */

static long deliver_pending(char *buf, size_t n) {
    size_t avail = g_line_len - g_line_pos;
    size_t take = avail < n ? avail : n;
    memcpy(buf, g_line + g_line_pos, take);
    g_line_pos += take;
    if (g_line_pos >= g_line_len) g_line_len = g_line_pos = 0;
    return (long)take;
}

static long canonical_read(char *buf, size_t n) {
    /* Drain a previously-cooked line first. */
    if (g_line_pos < g_line_len) return deliver_pending(buf, n);

    bool echo = (g_tio.c_lflag & TTY_ECHO) != 0;
    bool echoe = (g_tio.c_lflag & TTY_ECHOE) != 0;
    size_t len = 0;

    for (;;) {
        int ci = getc_block();
        if (ci < 0) return EINTR_RET;
        char c = (char)ci;

        /* Input flag CR/NL mapping. */
        if (c == '\r') {
            if (g_tio.c_iflag & TTY_IGNCR) continue;
            if (g_tio.c_iflag & TTY_ICRNL) c = '\n';
        } else if (c == '\n') {
            if (g_tio.c_iflag & TTY_INLCR) c = '\r';
        }

        if (c == (char)g_tio.c_cc[TTY_VERASE]) {
            if (len > 0) { len--; if (echo && echoe) echo_erase(); }
            continue;
        }
        if (c == (char)g_tio.c_cc[TTY_VKILL]) {
            while (len > 0) { len--; if (echo && echoe) echo_erase(); }
            continue;
        }
        if (c == (char)g_tio.c_cc[TTY_VEOF]) {
            /* Ctrl-D: terminate the line. If nothing typed yet, this is EOF
             * (return 0); otherwise hand back what's buffered, no newline. */
            if (len == 0) return 0;
            break;
        }
        if (c == '\n') {
            if (len < TTY_LINE_MAX) g_line[len++] = '\n';
            if (echo) echo_byte('\n');
            break;
        }
        if (len < TTY_LINE_MAX) {
            g_line[len++] = c;
            if (echo) echo_byte(c);
        }
    }

    g_line_len = len;
    g_line_pos = 0;
    return deliver_pending(buf, n);
}

static long raw_read(char *buf, size_t n) {
    bool echo = (g_tio.c_lflag & TTY_ECHO) != 0;
    size_t vmin = g_tio.c_cc[TTY_VMIN];
    if (vmin == 0) vmin = 1;          /* we don't model VTIME timers */
    size_t got = 0;

    /* Serve any priority reply (e.g. a CPR answering ESC[6n) as its OWN read
     * burst -- don't spill into real keyboard input in the same call, so a
     * line editor parsing "ESC[...R" doesn't also swallow the user's typed
     * line. */
    if (g_reply_pos < g_reply_len) {
        while (got < n && g_reply_pos < g_reply_len) buf[got++] = g_reply[g_reply_pos++];
        return (long)got;
    }

    while (got < n) {
        int c = tty_input_next();
        if (c < 0) {
            if (got >= vmin) break;   /* VMIN satisfied */
            if (signal_pending_self()) return got ? (long)got : EINTR_RET;
            sti();
            hlt();
            continue;
        }
        buf[got++] = (char)c;
        if (echo) echo_byte((char)c);
    }
    return (long)got;
}

long tty_console_read(char *buf, size_t n) {
    ensure_init();
    if (n == 0) return 0;
    if (g_tio.c_lflag & TTY_ICANON) return canonical_read(buf, n);
    return raw_read(buf, n);
}

bool tty_console_readable(void) {
    /* For poll/select on a console fd: input is available if a terminal-query
     * reply is queued, a cooked line is pending delivery, or the keyboard ring
     * has bytes. (We report readiness on any byte even in canonical mode --
     * close enough for interactive use, which runs in raw mode anyway.) */
    if (g_reply_pos < g_reply_len) return true;
    if (g_line_pos < g_line_len) return true;
    return kbd_haschar();
}

/* ---- ioctl ------------------------------------------------------------ */

static void flush_input(void) {
    g_line_len = g_line_pos = 0;
    g_reply_len = g_reply_pos = 0;
    while (kbd_trygetc() >= 0) { /* drain the keyboard ring */ }
}

long tty_console_ioctl(unsigned long cmd, unsigned long arg) {
    ensure_init();
    switch (cmd) {
    case TTY_TCGETS:
        if (!arg) return -ABI_EFAULT;
        if (copy_to_user((void *)arg, &g_tio, sizeof(g_tio)) != 0)
            return -ABI_EFAULT;
        return 0;

    case TTY_TCSETS:
    case TTY_TCSETSW:
    case TTY_TCSETSF: {
        if (!arg) return -ABI_EFAULT;
        struct ktermios t;
        if (copy_from_user(&t, (const void *)arg, sizeof(t)) != 0)
            return -ABI_EFAULT;
        g_tio = t;
        if (cmd == TTY_TCSETSF) flush_input();   /* discard pending input */
        return 0;
    }

    case TTY_TCFLSH:
        flush_input();
        return 0;

    case TTY_TIOCGWINSZ: {
        if (!arg) return -ABI_EFAULT;
        uint32_t cols = 80, rows = 25;
        console_get_size(&cols, &rows);
        struct kwinsize ws = {
            .ws_row = (uint16_t)rows, .ws_col = (uint16_t)cols,
            .ws_xpixel = 0, .ws_ypixel = 0,
        };
        if (copy_to_user((void *)arg, &ws, sizeof(ws)) != 0)
            return -ABI_EFAULT;
        return 0;
    }

    case TTY_TIOCSWINSZ:
        /* Accept the new size but the console grid is fixed by the
         * framebuffer; nothing to store that matters here. */
        return 0;

    case TTY_TIOCGPGRP: {
        if (!arg) return -ABI_EFAULT;
        int pg = signal_get_foreground();
        if (pg <= 0) { struct proc *p = current_proc(); pg = p ? p->pid : 1; }
        if (copy_to_user((void *)arg, &pg, sizeof(pg)) != 0)
            return -ABI_EFAULT;
        return 0;
    }

    case TTY_TIOCSPGRP: {
        if (!arg) return -ABI_EFAULT;
        int pg = 0;
        if (copy_from_user(&pg, (const void *)arg, sizeof(pg)) != 0)
            return -ABI_EFAULT;
        if (pg > 0) signal_set_foreground(pg);
        return 0;
    }

    case TTY_FIONREAD: {
        if (!arg) return -ABI_EFAULT;
        /* Bytes available without blocking: a pending cooked line, else 0
         * (we can't cheaply peek the raw ring depth, and 0 is always safe). */
        int navail = (int)(g_line_len - g_line_pos);
        if (copy_to_user((void *)arg, &navail, sizeof(navail)) != 0)
            return -ABI_EFAULT;
        return 0;
    }

    default:
        return -ABI_ENOTTY;
    }
}

/* ---- console output processing (terminal query replies) --------------- */

/* Push the cursor-position report (CPR) answering an ESC[6n query into the
 * priority reply queue: ESC [ <row> ; <col> R, 1-based, from the console
 * cursor. An interactive line editor (busybox ash, readline, ...) does
 * "write ESC[6n; read until 'R'" to locate itself; without this reply it
 * blocks forever. */
static void enqueue_cpr(void) {
    uint32_t col = 0, row = 0;
    console_get_cursor(&col, &row);
    unsigned r = row + 1, c = col + 1;
    char rep[TTY_REPLY_MAX];
    int n = 0;
    rep[n++] = 0x1b; rep[n++] = '[';
    char tmp[8]; int t = 0;
    if (r == 0) tmp[t++] = '0'; else { unsigned v = r; while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; } }
    while (t > 0) rep[n++] = tmp[--t];
    rep[n++] = ';';
    t = 0;
    if (c == 0) tmp[t++] = '0'; else { unsigned v = c; while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; } }
    while (t > 0) rep[n++] = tmp[--t];
    rep[n++] = 'R';
    /* Overwrite any stale reply (a prior query is always fully consumed
     * before the next one is issued). */
    for (int i = 0; i < n; i++) g_reply[i] = rep[i];
    g_reply_len = (size_t)n;
    g_reply_pos = 0;
}

void tty_console_output(const char *buf, size_t n) {
    ensure_init();
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        kputc(c);   /* pass every byte through to the console + serial */

        switch (g_esc_state) {
        case 0:
            if (c == 0x1b) g_esc_state = 1;
            break;
        case 1:
            if (c == '[') { g_esc_state = 2; g_csi_len = 0; }
            else g_esc_state = 0;
            break;
        case 2:
            if (c >= '0' && c <= '9') {
                if (g_csi_len < (int)sizeof(g_csi)) g_csi[g_csi_len++] = c;
            } else {
                /* final byte: answer Device Status Report -> cursor position
                 * (ESC[6n). ESC[5n (device OK) -> ESC[0n. */
                if (c == 'n' && g_csi_len == 1 && g_csi[0] == '6') {
                    enqueue_cpr();
                }
                g_esc_state = 0;
            }
            break;
        }
    }
}
