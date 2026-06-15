/* programs/linux-mmaptest/main.c
 *
 * A genuine Linux x86-64 static ELF (raw syscalls, no libc) that proves
 * tobyOS's file-backed mmap (B6):
 *
 *   - mmap a file at offset 0 and verify the mapped bytes equal what
 *     read() returns,
 *   - mmap a larger file at a PAGE OFFSET (4096) and verify again -- this
 *     exercises the 6th syscall arg (mmap offset = user r9), which the
 *     dispatch reads back from the saved register block.
 *
 * Exit 0 iff both mappings match their read() contents.
 */

typedef unsigned long u64;
typedef long          i64;

/* 6-arg syscall wrapper (mmap needs fd in arg5 + offset in arg6/r9). */
static inline i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    register i64 r9  __asm__("r9")  = f;
    i64 r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

#define SYS_read       0
#define SYS_write      1
#define SYS_open       2
#define SYS_close      3
#define SYS_lseek      8
#define SYS_mmap       9
#define SYS_exit_group 231
#define PROT_READ      1
#define MAP_PRIVATE    2
#define SEEK_SET       0

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { sc(SYS_write, 1, (i64)s, (i64)slen(s), 0, 0, 0); }
static int  eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* mmap `path` at `off`, read the same window with read(), and compare. */
static int check(const char *path, u64 off, int n) {
    i64 fd = sc(SYS_open, (i64)path, 0 /*O_RDONLY*/, 0, 0, 0, 0);
    if (fd < 0) return 0;
    char rbuf[64];
    if (n > (int)sizeof(rbuf)) n = sizeof(rbuf);
    sc(SYS_lseek, fd, (i64)off, SEEK_SET, 0, 0, 0);
    i64 got = sc(SYS_read, fd, (i64)rbuf, n, 0, 0, 0);
    if (got <= 0) { sc(SYS_close, fd, 0, 0, 0, 0, 0); return 0; }

    /* mmap maps file[off .. off+len), so mapping[0] == file[off]. */
    i64 m = sc(SYS_mmap, 0, 4096, PROT_READ, MAP_PRIVATE, fd, (i64)off);
    int ok = (m > 0) && eq((const char *)m, rbuf, (int)got);
    sc(SYS_close, fd, 0, 0, 0, 0, 0);
    return ok;
}

__attribute__((force_align_arg_pointer))
void _start(void) {
    int ok = 1;
    /* offset 0: a small file. */
    if (!check("/etc/motd", 0, 48)) ok = 0;
    /* page offset: a larger file (proves the mmap offset arg is honoured). */
    if (!check("/bin/c_hello", 4096, 32)) ok = 0;

    put(ok ? "file-backed mmap (offset 0 + 4096): OK\n"
           : "file-backed mmap: FAIL\n");
    sc(SYS_exit_group, ok ? 0 : 1, 0, 0, 0, 0, 0);
    for (;;) { }
}
