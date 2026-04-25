/* user_bad/main.c -- a deliberately-misbehaving ring-3 program.
 *
 * Writes to a kernel-half address from CPL=3. The CPU traps with #PF
 * (vector 14) and supervisor=0, so the page tables refuse the access.
 * Our default exception handler notices (cs & 3) == 3, dumps regs, and
 * calls proc_exit(-1). The scheduler reaps us, the shell waiter wakes
 * up, and control returns to the prompt -- a smoke test for "user
 * faults do not crash the kernel".
 */

typedef unsigned long size_t;

#define SYS_EXIT   0
#define SYS_WRITE  1

/* Milestone 7 ABI: SYS_WRITE(fd, buf, len). */
static inline long sys_write(int fd, const char *buf, size_t len) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline __attribute__((noreturn)) void sys_exit(int code) {
    __asm__ volatile (
        "syscall" :
        : "a"((long)SYS_EXIT), "D"((long)code)
        : "rcx", "r11", "memory"
    );
    __builtin_unreachable();
}

void _start(void);
void _start(void) {
    static const char msg[] = "user_bad: about to write to a kernel address...\n";
    sys_write(1, msg, sizeof(msg) - 1);

    /* Try to scribble on the kernel image. CR0.WP=1 + page DPL=0 +
     * CPL=3 => #PF with err code 0x7 (user|write|present) on first
     * write. Kernel handles it gracefully; we never get to sys_exit. */
    volatile unsigned long *kernel_ptr = (unsigned long *)0xFFFFFFFF80000000ULL;
    *kernel_ptr = 0xDEADBEEF;

    /* Unreachable in practice -- here as belt-and-braces. */
    sys_exit(0);
}
