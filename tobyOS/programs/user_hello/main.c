/* user_hello/main.c -- the simplest possible ring-3 program for tobyOS.
 *
 * Calls SYS_WRITE to print a string, then SYS_EXIT with a sentinel
 * exit code so the kernel can verify both syscalls work end-to-end.
 *
 * ABI (matches Linux x86_64, see include/tobyos/syscall.h):
 *   rax = number, rdi/rsi/rdx/r10/r8/r9 = args, ret in rax,
 *   rcx and r11 clobbered.
 *
 * Built with -mcmodel=small (default) because the program is linked
 * at 0x400000 -- well within the 32-bit signed-immediate window.
 * No CRT, no glibc, no relocations: the linker resolves every symbol
 * statically against this single .c file.
 */

typedef unsigned long size_t;
typedef long          ssize_t;

#define SYS_EXIT   0
#define SYS_WRITE  1

/* Milestone 7 ABI: SYS_WRITE takes (int fd, const void *buf, size_t len).
 * stdout = fd 1. */
static inline ssize_t sys_write(int fd, const char *buf, size_t len) {
    ssize_t ret;
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
        "syscall"
        :
        : "a"((long)SYS_EXIT), "D"((long)code)
        : "rcx", "r11", "memory"
    );
    __builtin_unreachable();
}

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

/* The linker script anchors execution here. We have a real stack
 * (kernel set RSP via iretq) but no command-line args, no envp, and
 * no auxv -- pretend it's main(0, NULL). */
void _start(void);
void _start(void) {
    static const char msg[] = "hello from user mode\n";
    sys_write(1, msg, my_strlen(msg));

    /* Quick second write to prove syscalls can be reissued. */
    static const char tag[] = "user_hello: about to SYS_EXIT(0x55)\n";
    sys_write(1, tag, my_strlen(tag));

    sys_exit(0x55);
}
