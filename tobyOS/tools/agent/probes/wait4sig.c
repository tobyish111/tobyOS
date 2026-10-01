/* wait4sig -- is a child killed by a signal reported as SIGNALED by wait4?
 * Linux: status 0x9 for SIGKILL (WIFSIGNALED, WTERMSIG 9); 0x700 for exit(7). */
typedef long i64;
static inline i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d) {
    register i64 r10 __asm__("r10") = d; i64 ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}
static void put(const char *s) { i64 n = 0; while (s[n]) n++; sc(1, 1, (i64)s, n, 0); }
static void hex(unsigned v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v); b[--i] = 'x'; b[--i] = '0'; put(b + i); }
__attribute__((force_align_arg_pointer)) void _start(void) {
    int st = 0;
    i64 c = sc(57, 0, 0, 0, 0);
    if (c == 0) { i64 ts[2] = { 5, 0 }; sc(35, (i64)ts, 0, 0, 0); sc(60, 0, 0, 0, 0); }
    i64 ts2[2] = { 0, 100000000 }; sc(35, (i64)ts2, 0, 0, 0);
    sc(62, c, 9, 0, 0);
    sc(61, c, (i64)&st, 0, 0); put("killed by SIGKILL: status "); hex((unsigned)st); put(" (Linux 0x9)\n");
    c = sc(57, 0, 0, 0, 0);
    if (c == 0) sc(60, 7, 0, 0, 0);
    st = 0; sc(61, c, (i64)&st, 0, 0); put("exit(7): status "); hex((unsigned)st); put(" (Linux 0x700)\n");
    c = sc(57, 0, 0, 0, 0);
    if (c == 0) { sc(60, 137, 0, 0, 0); }
    st = 0; sc(61, c, (i64)&st, 0, 0); put("exit(137): status "); hex((unsigned)st); put(" (Linux 0x8900)\n");
    sc(60, 0, 0, 0, 0);
}
