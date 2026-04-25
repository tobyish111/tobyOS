/* user_echo/main.c -- prints argv[1..] separated by single spaces,
 * followed by a single '\n'. Mirrors POSIX echo(1) without -n / -e.
 *
 * Bytes are emitted via SYS_WRITE on fd 1 (stdout). When run under a
 * shell pipeline ("echo hello | cat"), the shell rewires fd 1 to the
 * write end of a pipe before spawning us -- we do nothing special to
 * notice; write() Just Goes There.
 */

typedef unsigned long size_t;
typedef long          ssize_t;

#define SYS_EXIT   0
#define SYS_WRITE  1

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

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static void put_str(int fd, const char *s) {
    sys_write(fd, s, my_strlen(s));
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            sys_write(1, " ", 1);
        }
        put_str(1, argv[i]);
    }
    sys_write(1, "\n", 1);
    return 0;
}
