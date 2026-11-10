/* waitid probe -- a Linux static ELF exercising waitid(2) the way glibc's
 * waitid() and posix_spawn users call it. One line per case. */
typedef long i64;
static inline i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8 __asm__("r8") = e;
    i64 ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}
#define SYS_write 1
#define SYS_nanosleep 35
#define SYS_fork 57
#define SYS_exit 60
#define SYS_kill 62
#define SYS_setpgid 109
#define SYS_waitid 247
#define P_ALL 0
#define P_PID 1
#define P_PGID 2
#define WNOHANG 1
#define WEXITED 4
#define WNOWAIT 0x01000000

static void put(const char *s) { i64 n = 0; while (s[n]) n++; sc(SYS_write, 1, (i64)s, n, 0, 0); }
static void putn(i64 v) {
    char b[24]; int i = 23; b[i] = 0; int neg = v < 0; if (neg) v = -v;
    do { b[--i] = '0' + v % 10; v /= 10; } while (v);
    if (neg) b[--i] = '-';
    put(b + i);
}
static void msleep(i64 ms) { i64 ts[2] = { ms / 1000, (ms % 1000) * 1000000 }; sc(SYS_nanosleep, (i64)ts, 0, 0, 0, 0); }
static i64 child(int code, int ms) {
    i64 p = sc(SYS_fork, 0, 0, 0, 0, 0);
    if (p == 0) { msleep(ms); sc(SYS_exit, code, 0, 0, 0, 0); }
    return p;
}
static int si[32];      /* siginfo_t: signo@0 errno@4 code@8 pid@16 uid@20 status@24 */
static void clr(void) { for (int i = 0; i < 32; i++) si[i] = 0; }
static void show(const char *label, i64 r) {
    put(label); put(": ret="); putn(r);
    put(" signo="); putn(si[0]); put(" code="); putn(si[2]);
    put(" pid_ok="); putn(si[4] != 0); put(" status="); putn(si[6]); put("\n");
}

__attribute__((force_align_arg_pointer)) void _start(void) {
    i64 c1 = child(7, 100);
    clr(); i64 r = sc(SYS_waitid, P_ALL, 0, (i64)si, WEXITED, 0);
    show("P_ALL exited(7)", r); put("  pid matches: "); putn(si[4] == c1); put("\n");

    i64 c2 = child(3, 100);
    clr(); r = sc(SYS_waitid, P_PID, c2, (i64)si, WEXITED | WNOWAIT, 0);
    show("P_PID WNOWAIT exited(3)", r);
    clr(); r = sc(SYS_waitid, P_PID, c2, (i64)si, WEXITED, 0);
    show("P_PID again (still there after WNOWAIT)", r);

    i64 c3 = child(0, 5000);
    clr(); r = sc(SYS_waitid, P_PID, c3, (i64)si, WEXITED | WNOHANG, 0);
    show("P_PID WNOHANG running", r);
    sc(SYS_kill, c3, 9, 0, 0, 0);
    clr(); r = sc(SYS_waitid, P_PID, c3, (i64)si, WEXITED, 0);
    show("P_PID killed(9): code 2=CLD_KILLED", r);

    i64 c4 = sc(SYS_fork, 0, 0, 0, 0, 0);
    if (c4 == 0) { sc(SYS_setpgid, 0, 0, 0, 0, 0); msleep(100); sc(SYS_exit, 5, 0, 0, 0, 0); }
    msleep(20);
    clr(); r = sc(SYS_waitid, P_PGID, c4, (i64)si, WEXITED, 0);
    show("P_PGID exited(5)", r);

    clr(); r = sc(SYS_waitid, P_ALL, 0, (i64)si, WEXITED, 0);
    show("P_ALL no children: ret -10=ECHILD", r);
    sc(SYS_exit, 0, 0, 0, 0, 0);
}
