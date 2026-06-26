/* programs/linux-tty/main.c
 *
 * Track B / B21 proof: a genuine Linux x86-64 static ELF (raw syscalls, no
 * libc) that exercises the console TTY + termios. Before B21 ioctl(TCGETS)
 * failed with ENOTTY, so a libc would treat the console as a pipe.
 *
 *   1) ioctl(0, TCGETS, &t) succeeds            -> isatty(stdin) is true
 *   2) raw-mode roundtrip: clear ICANON|ECHO, TCSETS, TCGETS back, verify the
 *      flags actually changed; then restore the canonical termios
 *   3) ioctl(1, TIOCGWINSZ, &ws) succeeds with ws_col>0 && ws_row>0
 *   4) canonical cooked read: the boot harness injects "hello\n" into the
 *      keyboard before spawning us; read(0,..) must cook+return exactly
 *      "hello\n" (6 bytes) -- proving the line discipline, not raw bytes
 *
 * Each passing check sets a bit; exit_group(value) returns the bitmask.
 * All four => exit 15 (0b1111).
 */

typedef unsigned long u64;
typedef long          i64;

static inline i64 lx_syscall(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    i64 ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

#define SYS_read       0
#define SYS_write      1
#define SYS_ioctl      16
#define SYS_exit_group 231

/* Linux kernel struct termios (TCGETS layout, 36 bytes). */
struct termios {
    unsigned int  c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line;
    unsigned char c_cc[19];
};

struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TIOCGWINSZ  0x5413

#define ICANON 0x0002u
#define ECHO   0x0008u

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { lx_syscall(SYS_write, 1, (i64)s, (i64)slen(s), 0, 0); }

__attribute__((force_align_arg_pointer))
void _start(void) {
    int flags = 0;
    struct termios t, t2, saved;

    /* 1) TCGETS on stdin succeeds -> the console is a tty (isatty true). */
    if (lx_syscall(SYS_ioctl, 0, TCGETS, (i64)&t, 0, 0) == 0) {
        flags |= 1; put("tcgets: OK\n");
        saved = t;
    } else {
        put("tcgets: FAIL\n");
    }

    /* 2) Flip to raw mode and read the termios back to confirm it stuck. */
    if (flags & 1) {
        t2 = t;
        t2.c_lflag &= ~(ICANON | ECHO);
        if (lx_syscall(SYS_ioctl, 0, TCSETS, (i64)&t2, 0, 0) == 0 &&
            lx_syscall(SYS_ioctl, 0, TCGETS, (i64)&t2, 0, 0) == 0 &&
            (t2.c_lflag & ICANON) == 0 && (t2.c_lflag & ECHO) == 0) {
            flags |= 2; put("tcsetraw: OK\n");
        } else {
            put("tcsetraw: FAIL\n");
        }
        /* Restore canonical mode so check 4 cooks the injected line. */
        lx_syscall(SYS_ioctl, 0, TCSETS, (i64)&saved, 0, 0);
    }

    /* 3) Terminal size. */
    struct winsize ws;
    if (lx_syscall(SYS_ioctl, 1, TIOCGWINSZ, (i64)&ws, 0, 0) == 0 &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        flags |= 4; put("winsize: OK\n");
    } else {
        put("winsize: FAIL\n");
    }

    /* 4) Canonical cooked read of the harness-injected "hello\n". */
    char buf[64];
    i64 r = lx_syscall(SYS_read, 0, (i64)buf, sizeof(buf), 0, 0);
    if (r == 6 && buf[0]=='h' && buf[1]=='e' && buf[2]=='l' &&
        buf[3]=='l' && buf[4]=='o' && buf[5]=='\n') {
        flags |= 8; put("cookedline: OK\n");
    } else {
        put("cookedline: FAIL\n");
    }

    lx_syscall(SYS_exit_group, flags, 0, 0, 0, 0);
    for (;;) {}
}
