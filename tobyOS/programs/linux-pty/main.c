/* programs/linux-pty/main.c
 *
 * Track B / B23 proof: a genuine Linux x86-64 static ELF (raw syscalls, no
 * libc) that creates a pseudoterminal exactly the way glibc/musl openpty()
 * does, drives both ends, and finally runs a REAL busybox shell on the slave.
 *
 *   bit0 (1)  open("/dev/ptmx"); ioctl(TIOCSPTLCK,0); ioctl(TIOCGPTN)->N;
 *             open("/dev/pts/N") -- the full openpty handshake succeeds
 *   bit1 (2)  ioctl(slave, TCGETS) works (isatty), TCSETS clears ECHO, reads back
 *   bit2 (4)  master->slave canonical line: write "hello\n" to master, the
 *             cooked read on the slave returns exactly "hello\n"
 *   bit3 (8)  slave->master with ONLCR: write "world\n" on the slave, the
 *             master reads "world\r\n" (the line discipline mapped NL->CRNL)
 *   bit4 (16) TIOCSWINSZ on master then TIOCGWINSZ on slave round-trips 40x120
 *   bit5 (32) fork a child whose stdio is the slave and execve busybox
 *             `sh -c 'echo PTY-SHELL'`; the master reads "PTY-SHELL" back --
 *             a real program running on the pseudoterminal
 *
 * exit_group(bitmask). All six => exit 63 (0b111111).
 */

typedef unsigned long u64;
typedef long          i64;

static inline i64 sys(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    i64 ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

#define SYS_read       0
#define SYS_write      1
#define SYS_open       2
#define SYS_close      3
#define SYS_ioctl      16
#define SYS_dup2       33
#define SYS_fork       57
#define SYS_execve     59
#define SYS_wait4      61
#define SYS_exit_group 231

struct termios {
    unsigned int  c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line;
    unsigned char c_cc[19];
};
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCSPTLCK  0x40045431
#define TIOCGPTN    0x80045430
#define ECHO        0x0008u

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { sys(SYS_write, 1, (i64)s, (i64)slen(s), 0, 0); }
static int eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

__attribute__((force_align_arg_pointer))
void _start(void) {
    int flags = 0;

    /* ---- bit0: openpty handshake ---- */
    int mfd = (int)sys(SYS_open, (i64)"/dev/ptmx", 2 /*O_RDWR*/, 0, 0, 0);
    int n = -1, sfd = -1;
    if (mfd >= 0) {
        int unlock = 0;
        sys(SYS_ioctl, mfd, TIOCSPTLCK, (i64)&unlock, 0, 0);
        if (sys(SYS_ioctl, mfd, TIOCGPTN, (i64)&n, 0, 0) == 0 && n >= 0) {
            char path[16]; int p = 0;
            const char *pre = "/dev/pts/";
            for (const char *q = pre; *q; q++) path[p++] = *q;
            if (n == 0) path[p++] = '0';
            else { char tmp[8]; int t = 0, v = n; while (v) { tmp[t++] = '0'+v%10; v/=10; }
                   while (t) path[p++] = tmp[--t]; }
            path[p] = 0;
            sfd = (int)sys(SYS_open, (i64)path, 2, 0, 0, 0);
        }
    }
    if (mfd >= 0 && sfd >= 0) { flags |= 1; put("openpty: OK\n"); }
    else { put("openpty: FAIL\n"); sys(SYS_exit_group, flags, 0,0,0,0); for(;;){} }

    /* ---- bit1: TCGETS/TCSETS on the slave (clear ECHO for clean data path) ---- */
    struct termios t;
    if (sys(SYS_ioctl, sfd, TCGETS, (i64)&t, 0, 0) == 0) {
        t.c_lflag &= ~ECHO;
        struct termios t2;
        if (sys(SYS_ioctl, sfd, TCSETS, (i64)&t, 0, 0) == 0 &&
            sys(SYS_ioctl, sfd, TCGETS, (i64)&t2, 0, 0) == 0 &&
            (t2.c_lflag & ECHO) == 0) {
            flags |= 2; put("termios: OK\n");
        } else put("termios: FAIL\n");
    } else put("termios: FAIL\n");

    /* ---- bit2: master -> slave canonical line ---- */
    sys(SYS_write, mfd, (i64)"hello\n", 6, 0, 0);
    char buf[64];
    i64 r = sys(SYS_read, sfd, (i64)buf, sizeof(buf), 0, 0);
    if (r == 6 && eq(buf, "hello\n", 6)) { flags |= 4; put("m2s: OK\n"); }
    else put("m2s: FAIL\n");

    /* ---- bit3: slave -> master with ONLCR ---- */
    sys(SYS_write, sfd, (i64)"world\n", 6, 0, 0);
    r = sys(SYS_read, mfd, (i64)buf, sizeof(buf), 0, 0);
    if (r == 7 && eq(buf, "world\r\n", 7)) { flags |= 8; put("s2m: OK\n"); }
    else put("s2m: FAIL\n");

    /* ---- bit4: window size propagation ---- */
    struct winsize ws = { 40, 120, 0, 0 };
    struct winsize wr;
    if (sys(SYS_ioctl, mfd, TIOCSWINSZ, (i64)&ws, 0, 0) == 0 &&
        sys(SYS_ioctl, sfd, TIOCGWINSZ, (i64)&wr, 0, 0) == 0 &&
        wr.ws_row == 40 && wr.ws_col == 120) {
        flags |= 16; put("winsz: OK\n");
    } else put("winsz: FAIL\n");

    /* ---- bit5: run a REAL busybox shell on the pseudoterminal ---- */
    i64 pid = sys(SYS_fork, 0, 0, 0, 0, 0);
    if (pid == 0) {
        /* child: stdio = slave, then become the shell */
        sys(SYS_dup2, sfd, 0, 0, 0, 0);
        sys(SYS_dup2, sfd, 1, 0, 0, 0);
        sys(SYS_dup2, sfd, 2, 0, 0, 0);
        sys(SYS_close, mfd, 0, 0, 0, 0);
        char *argv[] = { (char*)"busybox", (char*)"sh", (char*)"-c",
                         (char*)"echo PTY-SHELL", 0 };
        char *envp[] = { (char*)"PATH=/bin", 0 };
        sys(SYS_execve, (i64)"/bin/busybox", (i64)argv, (i64)envp, 0, 0);
        sys(SYS_exit_group, 127, 0, 0, 0, 0);
        for (;;) {}
    } else if (pid > 0) {
        /* parent: read the shell's output off the master until we see the token */
        char acc[128]; int al = 0;
        for (int it = 0; it < 64 && al < (int)sizeof(acc) - 1; it++) {
            r = sys(SYS_read, mfd, (i64)(acc + al), sizeof(acc) - 1 - al, 0, 0);
            if (r <= 0) break;
            al += (int)r;
            int found = 0;
            for (int i = 0; i + 9 <= al; i++) if (eq(acc + i, "PTY-SHELL", 9)) { found = 1; break; }
            if (found) { flags |= 32; put("ptyshell: OK\n"); break; }
        }
        int st = 0;
        sys(SYS_wait4, pid, (i64)&st, 0, 0, 0);
        if (!(flags & 32)) put("ptyshell: FAIL\n");
    } else {
        put("ptyshell: FAIL(fork)\n");
    }

    sys(SYS_exit_group, flags, 0, 0, 0, 0);
    for (;;) {}
}
