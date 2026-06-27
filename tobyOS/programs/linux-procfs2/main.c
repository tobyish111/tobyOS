/* programs/linux-procfs2/main.c
 *
 * Track B / B24 proof: a raw-syscall Linux x86-64 static ELF that exercises
 * the /proc + /sys breadth added in B24.
 *
 *   bit0 (1)  /proc/meminfo contains "MemTotal:" and a "kB" unit
 *   bit1 (2)  /proc/uptime parses as two space-separated fields
 *   bit2 (4)  /proc/mounts mentions the "/proc" mount point
 *   bit3 (8)  /proc/stat starts with "cpu " and has a "processes" line
 *   bit4 (16) /proc/self/fd lists at least fds 0,1,2; readlink(fd/0) nonempty
 *   bit5 (32) /sys/devices/system/cpu/online is nonempty (starts with '0')
 *
 * exit_group(bitmask). All six => exit 63.
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
#define SYS_getdents64 217
#define SYS_readlinkat 267
#define SYS_exit_group 231

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { sys(SYS_write, 1, (i64)s, (i64)slen(s), 0, 0); }

/* substring search */
static int contains(const char *hay, int hl, const char *needle) {
    int nl = (int)slen(needle);
    for (int i = 0; i + nl <= hl; i++) {
        int j = 0; while (j < nl && hay[i + j] == needle[j]) j++;
        if (j == nl) return 1;
    }
    return 0;
}

static int read_all(const char *path, char *buf, int cap) {
    int fd = (int)sys(SYS_open, (i64)path, 0 /*O_RDONLY*/, 0, 0, 0);
    if (fd < 0) return -1;
    int total = 0;
    for (;;) {
        i64 r = sys(SYS_read, fd, (i64)(buf + total), cap - 1 - total, 0, 0);
        if (r <= 0) break;
        total += (int)r;
        if (total >= cap - 1) break;
    }
    sys(SYS_close, fd, 0, 0, 0, 0);
    buf[total] = 0;
    return total;
}

struct linux_dirent64 {
    u64 d_ino, d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char d_name[];
};

__attribute__((force_align_arg_pointer))
void _start(void) {
    int flags = 0;
    char buf[4096];
    int n;

    /* bit0: meminfo */
    n = read_all("/proc/meminfo", buf, sizeof(buf));
    if (n > 0 && contains(buf, n, "MemTotal:") && contains(buf, n, "kB")) {
        flags |= 1; put("meminfo: OK\n");
    } else put("meminfo: FAIL\n");

    /* bit1: uptime has two fields */
    n = read_all("/proc/uptime", buf, sizeof(buf));
    {
        int spaces = 0, digits = 0;
        for (int i = 0; i < n; i++) { if (buf[i] == ' ') spaces++; if (buf[i] >= '0' && buf[i] <= '9') digits++; }
        if (n > 0 && spaces >= 1 && digits >= 2) { flags |= 2; put("uptime: OK\n"); }
        else put("uptime: FAIL\n");
    }

    /* bit2: mounts mentions /proc */
    n = read_all("/proc/mounts", buf, sizeof(buf));
    if (n > 0 && contains(buf, n, "/proc")) { flags |= 4; put("mounts: OK\n"); }
    else put("mounts: FAIL\n");

    /* bit3: stat */
    n = read_all("/proc/stat", buf, sizeof(buf));
    if (n > 4 && buf[0]=='c' && buf[1]=='p' && buf[2]=='u' &&
        contains(buf, n, "processes")) { flags |= 8; put("stat: OK\n"); }
    else put("stat: FAIL\n");

    /* bit4: /proc/self/fd lists 0,1,2 and readlink(fd/0) is nonempty */
    {
        int fd = (int)sys(SYS_open, (i64)"/proc/self/fd", 0x10000 /*O_DIRECTORY*/, 0, 0, 0);
        if (fd < 0) fd = (int)sys(SYS_open, (i64)"/proc/self/fd", 0, 0, 0, 0);
        int saw0 = 0, saw1 = 0, saw2 = 0;
        if (fd >= 0) {
            char db[2048];
            for (;;) {
                i64 r = sys(SYS_getdents64, fd, (i64)db, sizeof(db), 0, 0);
                if (r <= 0) break;
                int off = 0;
                while (off < (int)r) {
                    struct linux_dirent64 *de = (void *)(db + off);
                    const char *nm = de->d_name;
                    if (nm[0]=='0' && nm[1]==0) saw0 = 1;
                    if (nm[0]=='1' && nm[1]==0) saw1 = 1;
                    if (nm[0]=='2' && nm[1]==0) saw2 = 1;
                    off += de->d_reclen;
                }
            }
            sys(SYS_close, fd, 0, 0, 0, 0);
        }
        char lnk[128];
        i64 lr = sys(SYS_readlinkat, -100 /*AT_FDCWD*/, (i64)"/proc/self/fd/0",
                     (i64)lnk, sizeof(lnk) - 1, 0);
        if (saw0 && saw1 && saw2 && lr > 0) { flags |= 16; put("fd: OK\n"); }
        else put("fd: FAIL\n");
    }

    /* bit5: /sys cpu/online */
    n = read_all("/sys/devices/system/cpu/online", buf, sizeof(buf));
    if (n > 0 && buf[0] == '0') { flags |= 32; put("sysfs: OK\n"); }
    else put("sysfs: FAIL\n");

    sys(SYS_exit_group, flags, 0, 0, 0, 0);
    for (;;) {}
}
