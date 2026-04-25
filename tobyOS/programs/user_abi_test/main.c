/* user_abi_test/main.c -- exercises every Milestone 25A syscall.
 *
 * This program is the canonical "does the ABI still hold?" smoke test.
 * It only depends on the kernel/userspace ABI -- no libc, no shared
 * library, no fancy linker tricks. Every syscall is invoked via inline
 * assembly so a regression in the libc port (milestone 25B) can't
 * mask a regression in the kernel-side syscall handler.
 *
 * Output is a sequence of `[abi-test] ...` lines on stdout. The test
 * driver looks for the trailing `[abi-test] ALL OK` line to declare
 * success. Any deviation from the expected line list is a failure.
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
typedef unsigned long long uint64_t;
typedef unsigned int       uint32_t;
typedef long long          int64_t;
typedef unsigned long      uintptr_t;

/* -------- syscall numbers (must match include/tobyos/abi/abi.h) ------ */
#define SYS_EXIT          0
#define SYS_WRITE         1
#define SYS_GETPID       31
#define SYS_GETPPID      32
#define SYS_SPAWN        33
#define SYS_WAITPID      34
#define SYS_OPEN         35
#define SYS_LSEEK        36
#define SYS_STAT         37
#define SYS_FSTAT        38
#define SYS_DUP          39
#define SYS_DUP2         40
#define SYS_UNLINK       41
#define SYS_MKDIR        42
#define SYS_BRK          43
#define SYS_GETCWD       44
#define SYS_CHDIR        45
#define SYS_GETENV       46
#define SYS_NANOSLEEP    47
#define SYS_CLOCK_MS     48
#define SYS_ABI_VERSION  49

/* -------- syscall trampolines (0..6 args) ---------------------------- */
static inline long sc0(long n) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "0"(n) : "rcx", "r11", "memory");
    return r;
}
static inline long sc1(long n, long a) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "0"(n), "D"(a)
                      : "rcx", "r11", "memory");
    return r;
}
static inline long sc2(long n, long a, long b) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b)
                      : "rcx", "r11", "memory");
    return r;
}
static inline long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return r;
}

/* -------- minimal helpers (no libc) ---------------------------------- */
static size_t u_strlen(const char *s) {
    const char *p = s; while (*p) p++; return (size_t)(p - s);
}
static int u_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
static void u_write(int fd, const char *s) {
    sc3(SYS_WRITE, fd, (long)s, (long)u_strlen(s));
}
static void u_writeln(int fd, const char *s) {
    u_write(fd, s); u_write(fd, "\n");
}
/* unsigned-long -> decimal string (caller-owned 32-byte buffer) */
static void u_uitoa(uint64_t v, char *buf) {
    char tmp[32]; int n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (v) { tmp[n++] = (char)('0' + v % 10ull); v /= 10ull; }
    int i = 0;
    while (n) buf[i++] = tmp[--n];
    buf[i] = '\0';
}
/* signed-long -> decimal string (caller-owned 32-byte buffer) */
static void u_itoa(long v, char *buf) {
    if (v < 0) { buf[0] = '-'; u_uitoa((uint64_t)(-v), buf + 1); return; }
    u_uitoa((uint64_t)v, buf);
}
static void log_kv(const char *key, long v) {
    char nbuf[32]; u_itoa(v, nbuf);
    u_write(1, "[abi-test] "); u_write(1, key);
    u_write(1, " = ");        u_writeln(1, nbuf);
}
static void log_msg(const char *m) {
    u_write(1, "[abi-test] "); u_writeln(1, m);
}
static void fail(const char *m) {
    u_write(1, "[abi-test] FAIL: "); u_writeln(1, m);
    sc1(SYS_EXIT, 1);
}

/* abi_stat layout matches struct abi_stat in <tobyos/abi/abi.h>. */
struct abi_stat {
    uint64_t size;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t _r0;
    uint64_t _r1[2];
};

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argv;

    log_msg("starting Milestone 25A ABI surface check");
    log_kv("argc", argc);

    /* --- abi version + clock + pid bookkeeping ---------------- */
    long ver = sc0(SYS_ABI_VERSION);
    log_kv("abi_version", ver);
    if (ver != 1) fail("expected abi version 1");

    long pid = sc0(SYS_GETPID);
    long ppid = sc0(SYS_GETPPID);
    log_kv("pid",  pid);
    log_kv("ppid", ppid);
    if (pid <= 0)  fail("pid must be > 0 for a user process");
    if (ppid < 0)  fail("ppid must be >= 0");

    long ms_a = sc0(SYS_CLOCK_MS);
    sc1(SYS_NANOSLEEP, 1000000); /* 1 ms */
    long ms_b = sc0(SYS_CLOCK_MS);
    log_kv("clock_ms_delta", ms_b - ms_a);
    if (ms_b < ms_a) fail("clock went backwards");

    /* --- cwd / chdir ----------------------------------------- */
    char cwd[256];
    long cn = sc2(SYS_GETCWD, (long)cwd, sizeof(cwd));
    if (cn < 0) fail("getcwd failed");
    log_kv("cwd_len", cn);
    u_write(1, "[abi-test] cwd = "); u_writeln(1, cwd);

    /* --- brk: grow then poke memory ------------------------- */
    long base = sc1(SYS_BRK, 0);
    log_kv("brk_initial", base);
    long want = base + 16384;       /* 4 pages */
    long got  = sc1(SYS_BRK, want);
    log_kv("brk_after_grow", got);
    if (got < want) fail("brk grow returned less than requested");

    /* Touch the new pages -- must be writable + zero-filled. */
    volatile char *heap = (volatile char *)base;
    for (long i = 0; i < 16384; i += 4096) {
        if (heap[i] != 0) fail("fresh heap page wasn't zero");
        heap[i] = (char)(i & 0x7f);
        if (heap[i] != (char)(i & 0x7f)) fail("heap page not writable");
    }
    log_msg("heap pages writable + zero-initialised OK");

    /* --- open / write / lseek / read / fstat -------------- */
    /* /data is the writable tobyfs mount. We use a unique name so
     * a stale file from a previous boot doesn't confuse us. */
    const char *path = "/data/abi_test.tmp";
    sc1(SYS_UNLINK, (long)path);    /* ignore error */

    long fd = sc3(SYS_OPEN, (long)path, /*O_RDWR|O_CREAT*/ 0x42, 0644);
    log_kv("open_fd", fd);
    if (fd < 0) fail("open(/data/abi_test.tmp, O_RDWR|O_CREAT) failed");

    const char *payload = "hello-25A\n";
    long w = sc3(SYS_WRITE, fd, (long)payload, (long)u_strlen(payload));
    log_kv("write_bytes", w);
    if (w != (long)u_strlen(payload)) fail("short write");

    long lr = sc3(SYS_LSEEK, fd, 0, /*SEEK_SET*/ 0);
    log_kv("lseek_set", lr);
    if (lr != 0) fail("lseek SEEK_SET=0 failed");

    char rbuf[64] = {0};
    long r = sc3(2 /* SYS_READ */, fd, (long)rbuf, sizeof(rbuf) - 1);
    log_kv("read_bytes", r);
    if (r < 0) fail("read failed");
    if (!u_streq(rbuf, payload)) fail("readback mismatch");

    struct abi_stat st;
    long sr = sc2(SYS_FSTAT, fd, (long)&st);
    log_kv("fstat_rc",   sr);
    log_kv("fstat_size", (long)st.size);
    log_kv("fstat_mode", (long)st.mode);
    if (sr != 0) fail("fstat failed");

    long sr2 = sc2(SYS_STAT, (long)path, (long)&st);
    log_kv("stat_rc",   sr2);
    log_kv("stat_size", (long)st.size);
    if (sr2 != 0) fail("stat failed");

    /* --- dup / dup2 ----------------------------------------- */
    long fd2 = sc1(SYS_DUP, fd);
    log_kv("dup_fd", fd2);
    if (fd2 < 0) fail("dup failed");

    long fd3 = sc2(SYS_DUP2, fd, 9);
    log_kv("dup2_fd", fd3);
    if (fd3 != 9) fail("dup2 to fd 9 failed");

    /* close all three (SYS_CLOSE = 4) */
    sc1(4, fd);
    sc1(4, fd2);
    sc1(4, fd3);

    long ur = sc1(SYS_UNLINK, (long)path);
    log_kv("unlink_rc", ur);

    /* --- mkdir + chdir ------------------------------------- */
    const char *dir = "/data/abi_test_dir";
    sc2(SYS_MKDIR, (long)dir, 0755);   /* ignore EEXIST */

    long cdr = sc1(SYS_CHDIR, (long)dir);
    log_kv("chdir_rc", cdr);
    if (cdr != 0) fail("chdir failed");

    char cwd2[256];
    long cn2 = sc2(SYS_GETCWD, (long)cwd2, sizeof(cwd2));
    u_write(1, "[abi-test] new cwd = "); u_writeln(1, cwd2);
    if (cn2 < 0) fail("getcwd after chdir failed");
    if (!u_streq(cwd2, dir)) fail("cwd doesn't match the dir we chdir'd to");

    /* getenv: kernel always returns 0 ("not set") in M25A. The libc
     * port (M25B) will satisfy these from envp directly. */
    char ebuf[64];
    long er = sc3(SYS_GETENV, (long)"PATH", (long)ebuf, sizeof(ebuf));
    log_kv("getenv_PATH_rc", er);

    log_msg("ALL OK");
    return 0;
}
