/* bigwrite -- K4: which call makes bash's >64 KiB here-document fail?
 * bash 5.2's heredoc_write() does ONE write(2) of the whole document and
 * reports ENOSPC itself when the count comes back short with errno 0.
 * One line per candidate, as a Linux static ELF (raw syscalls). */
typedef long i64;
static inline i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d) {
    register i64 r10 __asm__("r10") = d; i64 ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}
static void put(const char *s) { i64 n = 0; while (s[n]) n++; sc(1, 1, (i64)s, n, 0); }
static void putn(i64 v) {
    char b[24]; int i = 23; b[i] = 0; int neg = v < 0; if (neg) v = -v;
    do { b[--i] = '0' + v % 10; v /= 10; } while (v);
    if (neg) b[--i] = '-';
    put(b + i);
}
static char big[200000];
__attribute__((force_align_arg_pointer)) void _start(void) {
    for (int i = 0; i < (int)sizeof big; i++) big[i] = 'a' + i % 26;
    /* open(O_CREAT|O_EXCL|O_RDWR), as mkstemp does */
    i64 fd = sc(2, (i64)"/tmp/bigwrite.tmp", 0x2 | 0x40 | 0x80, 0600, 0);
    put("open O_CREAT|O_EXCL: "); putn(fd); put("\n");
    i64 w = sc(1, fd, (i64)big, 70000, 0);
    put("write 70000 to a file: "); putn(w); put("\n");
    i64 w2 = sc(1, fd, (i64)big, 200000, 0);
    put("write 200000 to a file: "); putn(w2); put("\n");
    i64 end = sc(8, fd, 0, 2, 0);                        /* lseek SEEK_END */
    put("file size: "); putn(end); put("\n");
    sc(3, fd, 0, 0, 0);
    sc(87, (i64)"/tmp/bigwrite.tmp", 0, 0, 0);           /* unlink */
    /* the other candidates the handoff lists */
    i64 t = sc(2, (i64)"/tmp", 0x410000 | 0x2, 0600, 0); /* O_TMPFILE|O_RDWR */
    put("open O_TMPFILE: "); putn(t); put("\n");
    if (t >= 0) sc(3, t, 0, 0, 0);
    i64 m = sc(319, (i64)"x", 0, 0, 0);                  /* memfd_create */
    put("memfd_create: "); putn(m); put("\n");
    if (m >= 0) sc(3, m, 0, 0, 0);
    int pfd[2];
    sc(22, (i64)pfd, 0, 0, 0);                            /* pipe */
    i64 ps = sc(72, pfd[1], 1031, 131072, 0);             /* F_SETPIPE_SZ */
    put("F_SETPIPE_SZ 131072: "); putn(ps); put("\n");
    sc(60, 0, 0, 0, 0);
}
