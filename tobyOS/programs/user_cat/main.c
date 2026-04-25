/* user_cat/main.c -- reads stdin (fd 0) until EOF, writes to stdout
 * (fd 1). Designed for shell pipelines: when a parent like /bin/echo
 * closes its write end of the pipe, our next read() returns 0, we exit
 * with status 0.
 *
 * We currently ignore argv -- file-path arguments are handled by the
 * shell's existing `cat` builtin, which uses VFS directly. cat-on-stdin
 * is the only case milestone 7 needs to demonstrate pipe IPC.
 */

typedef unsigned long size_t;
typedef long          ssize_t;

#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_READ   2

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

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[256];
    for (;;) {
        ssize_t n = sys_read(0, buf, sizeof(buf));
        if (n == 0) return 0;          /* EOF */
        if (n < 0)  return 1;          /* error */
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = sys_write(1, buf + off, (size_t)(n - off));
            if (w <= 0) return 2;      /* write error / EPIPE */
            off += w;
        }
    }
}
