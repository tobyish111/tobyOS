/* programs/linux-procfs/main.c
 *
 * Track B / B20 proof: a genuine Linux x86-64 static ELF (raw syscalls, no
 * libc) that exercises the extended /proc filesystem -- the synthetic files
 * real Linux software (and glibc startup) read:
 *
 *   /proc/cpuinfo       must contain "processor"
 *   /proc/self/status   must contain "Pid:"   (proves /proc/self is live)
 *   /proc/self/maps     must contain "[stack]" (synthesized memory map)
 *   /proc/self/exe      readlink must equal this binary's path
 *
 * Each check that passes sets a bit; exit_group(value) returns the bitmask.
 * All four pass => exit 15 (0b1111). Run from /bin/linux-procfs so the
 * /proc/self/exe assertion has a known target.
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
#define SYS_open       2
#define SYS_close      3
#define SYS_readlink   89
#define SYS_exit_group 231

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { lx_syscall(SYS_write, 1, (i64)s, (i64)slen(s), 0, 0); }

/* naive substring search */
static int contains(const char *hay, i64 hlen, const char *needle) {
    u64 nl = slen(needle);
    if ((i64)nl > hlen) return 0;
    for (i64 i = 0; i + (i64)nl <= hlen; i++) {
        u64 j = 0;
        while (j < nl && hay[i + j] == needle[j]) j++;
        if (j == nl) return 1;
    }
    return 0;
}

static int streq(const char *a, const char *b, i64 alen) {
    u64 bl = slen(b);
    if ((i64)bl != alen) return 0;
    for (i64 i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* read whole file into buf (cap), return length or <0 */
static i64 read_file(const char *path, char *buf, i64 cap) {
    i64 fd = lx_syscall(SYS_open, (i64)path, 0 /*O_RDONLY*/, 0, 0, 0);
    if (fd < 0) return -1;
    i64 total = 0;
    for (;;) {
        i64 r = lx_syscall(SYS_read, fd, (i64)(buf + total), cap - total, 0, 0);
        if (r < 0) { lx_syscall(SYS_close, fd, 0, 0, 0, 0); return -2; }
        if (r == 0) break;
        total += r;
        if (total >= cap) break;
    }
    lx_syscall(SYS_close, fd, 0, 0, 0, 0);
    return total;
}

__attribute__((force_align_arg_pointer))
void _start(void) {
    static char buf[4096];
    int flags = 0;

    /* 1) /proc/cpuinfo contains "processor" */
    i64 n = read_file("/proc/cpuinfo", buf, sizeof(buf));
    if (n > 0 && contains(buf, n, "processor")) { flags |= 1; put("cpuinfo: OK\n"); }
    else put("cpuinfo: FAIL\n");

    /* 2) /proc/self/status contains "Pid:" -- proves /proc/self is the caller */
    n = read_file("/proc/self/status", buf, sizeof(buf));
    if (n > 0 && contains(buf, n, "Pid:")) { flags |= 2; put("self/status: OK\n"); }
    else put("self/status: FAIL\n");

    /* 3) /proc/self/maps contains "[stack]" */
    n = read_file("/proc/self/maps", buf, sizeof(buf));
    if (n > 0 && contains(buf, n, "[stack]")) { flags |= 4; put("self/maps: OK\n"); }
    else put("self/maps: FAIL\n");

    /* 4) readlink(/proc/self/exe) == /bin/linux-procfs */
    i64 rl = lx_syscall(SYS_readlink, (i64)"/proc/self/exe", (i64)buf, sizeof(buf), 0, 0);
    if (rl > 0 && streq(buf, "/bin/linux-procfs", rl)) { flags |= 8; put("self/exe: OK\n"); }
    else put("self/exe: FAIL\n");

    lx_syscall(SYS_exit_group, flags, 0, 0, 0, 0);
    for (;;) {}
}
