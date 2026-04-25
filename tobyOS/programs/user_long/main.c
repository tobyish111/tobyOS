/* user_long/main.c -- a long-running ring-3 program for milestone 8.
 *
 * Loops forever printing a heartbeat, with a SYS_YIELD between
 * iterations. Designed as the canonical "kill me with Ctrl+C" demo:
 *
 *   tobyOS> long_task &           <- spawn in background
 *   [1] 2  'long_task' &
 *   tobyOS> jobs
 *     [1]  pid=2  RUNNING     long_task
 *   tobyOS> fg 1                  <- bring to foreground
 *   fg: bringing [1] pid=2 'long_task' to foreground
 *   long_task tick #0
 *   long_task tick #1
 *   ...
 *   ^C                            <- Ctrl+C
 *   [signal] pid=2 'long_task' killed by signal 2
 *   fg: 'long_task' (pid=2) returned 130 (0x82)   <- 128 + SIGINT
 *
 * Every loop iteration is two syscalls (write + yield), each of which
 * passes through signal_deliver_if_pending() on its way back to user
 * mode -- so the latency between Ctrl+C and the kill is at most one
 * iteration. Even without the yield, the PIT IRQ checks pending
 * signals every 10 ms while ring 3 runs, so a CPU-bound loop is
 * killable too.
 */

typedef unsigned long size_t;
typedef long          ssize_t;

#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_YIELD  5

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

static inline void sys_yield(void) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"((long)SYS_YIELD)
        : "rcx", "r11", "memory"
    );
    (void)ret;
}

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

/* Append a small unsigned decimal to `buf` at offset `off`, return
 * the new offset. Just enough to print iteration counters. */
static size_t put_uint(char *buf, size_t off, unsigned v) {
    char tmp[16];
    int  n = 0;
    if (v == 0) tmp[n++] = '0';
    else { while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; } }
    while (n--) buf[off++] = tmp[n];
    return off;
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[64];
    for (unsigned i = 0; ; i++) {
        size_t off = 0;
        const char *prefix = "long_task tick #";
        size_t pl = my_strlen(prefix);
        for (size_t k = 0; k < pl; k++) buf[off++] = prefix[k];
        off = put_uint(buf, off, i);
        buf[off++] = '\n';
        sys_write(1, buf, off);

        /* Yield + a small busy delay so the heartbeat is human-paced
         * (~ a few per second) without us monopolising the CPU. */
        for (volatile int d = 0; d < 5000000; d++) { }
        sys_yield();
    }
}
