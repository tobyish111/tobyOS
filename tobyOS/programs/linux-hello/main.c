/* programs/linux-hello/main.c
 *
 * A GENUINE Linux x86-64 static ELF -- raw Linux syscalls, no libc, no
 * tobyOS headers. It is byte-for-byte a Linux binary (it would run
 * unmodified on a real Linux kernel); the only build-time touch is the
 * standard FreeBSD-style "brandelf" stamp of e_ident[EI_OSABI]=Linux so
 * tobyOS's loader knows to run it under the Linux ABI personality.
 *
 * It deliberately walks the same startup path a real libc takes:
 *   - arch_prctl(ARCH_SET_FS, &tcb)   set up the thread pointer (TLS)
 *   - set_tid_address(&tid)           record the exit futex slot
 *   - read %fs:0 back                 prove the TLS base actually took
 *   - write(1, ...) + writev(1, ...)  the two stdout paths libc uses
 *   - exit_group(code)                code = 42 iff TLS verified, else 1
 *
 * Proving exit==42 proves the whole Linux-personality chain end to end:
 * personality latch -> Linux syscall-number translation -> arch_prctl TLS
 * -> writev scatter/gather -> exit_group.
 */

typedef unsigned long u64;
typedef long          i64;

static inline i64 lx_syscall(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    i64 ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

#define SYS_write           1
#define SYS_writev          20
#define SYS_arch_prctl      158
#define SYS_set_tid_address 218
#define SYS_exit_group      231
#define ARCH_SET_FS         0x1002

struct iovec { const void *iov_base; u64 iov_len; };

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char *s) {
    lx_syscall(SYS_write, 1, (i64)s, (i64)slen(s), 0, 0);
}

/* Minimal TCB: first word is a self-pointer (the x86-64 thread-pointer
 * ABI convention) so we can verify %fs:0 reads back our own address. */
static u64 tcb[16];

/* The kernel enters _start with RSP 16-byte aligned (the SysV/Linux
 * process-entry contract -- RSP%16==0, argc at [rsp]). A C function
 * compiled by clang instead assumes the *callee* contract (RSP%16==8, as
 * if reached via `call`), so its 16-byte SSE spills (movaps) would be
 * misaligned by 8 and #GP. force_align_arg_pointer emits an `and $-16`
 * prologue that re-establishes 16-alignment regardless of entry skew --
 * the standard fix for a freestanding _start. */
__attribute__((force_align_arg_pointer))
void _start(void) {
    tcb[0] = (u64)&tcb[0];
    lx_syscall(SYS_arch_prctl, ARCH_SET_FS, (i64)&tcb[0], 0, 0, 0);

    int tid;
    lx_syscall(SYS_set_tid_address, (i64)&tid, 0, 0, 0, 0);

    u64 tp = 0;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
    int tls_ok = (tp == (u64)&tcb[0]);

    put("hello from a Linux x86-64 binary running on tobyOS\n");
    put(tls_ok ? "arch_prctl + %fs thread pointer: OK\n"
               : "arch_prctl + %fs thread pointer: FAIL\n");

    struct iovec iov[2] = {
        { "writev: ", 8 },
        { "scatter/gather stdout OK\n", 25 },
    };
    lx_syscall(SYS_writev, 1, (i64)iov, 2, 0, 0);

    lx_syscall(SYS_exit_group, tls_ok ? 42 : 1, 0, 0, 0, 0);
    for (;;) { }   /* exit_group never returns; keep _start noreturn-clean */
}
