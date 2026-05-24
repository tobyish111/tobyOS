/* user_cat/main.c -- concatenate and print files.
 *
 * Usage: cat [-n] [file ...]
 *
 * With no file arguments, reads from stdin (fd 0) until EOF.
 * With -n, prefixes each output line with a line number.
 *
 * This is a "pre-libc" style program that uses raw syscalls directly
 * for maximum compatibility with early boot and pipe scenarios.
 */

typedef unsigned long size_t;
typedef long          ssize_t;

#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_READ    2
#define SYS_CLOSE   4
#define SYS_OPEN   35

#define O_RDONLY 0

static inline ssize_t sys_write(int fd, const void *buf, size_t len) {
    ssize_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline ssize_t sys_read(int fd, void *buf, size_t len) {
    ssize_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"((long)SYS_READ), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int sys_open(const char *path, int flags, int mode) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"((long)SYS_OPEN), "D"(path), "S"((long)flags), "d"((long)mode)
        : "rcx", "r11", "memory"
    );
    return (int)ret;
}

static inline int sys_close(int fd) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"((long)SYS_CLOSE), "D"((long)fd)
        : "rcx", "r11", "memory"
    );
    return (int)ret;
}

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static void put_str(int fd, const char *s) {
    size_t len = my_strlen(s);
    size_t off = 0;
    while (off < len) {
        ssize_t w = sys_write(fd, s + off, len - off);
        if (w <= 0) return;
        off += (size_t)w;
    }
}

static void put_uint(int fd, unsigned v) {
    char buf[12];
    int i = 0;
    if (v == 0) { buf[i++] = '0'; }
    else {
        char tmp[12];
        int n = 0;
        while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
        while (n > 0) buf[i++] = tmp[--n];
    }
    sys_write(fd, buf, (size_t)i);
}

static int cat_fd(int fd, int number_lines) {
    char buf[4096];
    unsigned line_no = 1;
    int at_line_start = 1;

    for (;;) {
        ssize_t n = sys_read(fd, buf, sizeof(buf));
        if (n == 0) return 0;
        if (n < 0)  return 1;

        if (!number_lines) {
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = sys_write(1, buf + off, (size_t)(n - off));
                if (w <= 0) return 2;
                off += w;
            }
        } else {
            for (ssize_t i = 0; i < n; i++) {
                if (at_line_start) {
                    put_str(1, "     ");
                    put_uint(1, line_no);
                    put_str(1, "\t");
                    at_line_start = 0;
                }
                sys_write(1, &buf[i], 1);
                if (buf[i] == '\n') {
                    line_no++;
                    at_line_start = 1;
                }
            }
        }
    }
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    int number_lines = 0;
    int first_file = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0' && !first_file) {
            for (int j = 1; argv[i][j]; j++) {
                switch (argv[i][j]) {
                case 'n': number_lines = 1; break;
                default:
                    put_str(2, "cat: unknown option: -");
                    sys_write(2, &argv[i][j], 1);
                    put_str(2, "\n");
                    return 1;
                }
            }
        } else {
            if (!first_file) first_file = i;
        }
    }

    if (!first_file) {
        return cat_fd(0, number_lines);
    }

    int status = 0;
    for (int i = first_file; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '\0') {
            if (cat_fd(0, number_lines) != 0) status = 1;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') continue;

        int fd = sys_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            put_str(2, "cat: ");
            put_str(2, argv[i]);
            put_str(2, ": No such file or directory\n");
            status = 1;
            continue;
        }
        if (cat_fd(fd, number_lines) != 0) status = 1;
        sys_close(fd);
    }
    return status;
}
