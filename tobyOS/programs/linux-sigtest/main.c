/* programs/linux-sigtest/main.c
 *
 * A GENUINE Linux x86-64 static ELF (raw Linux syscalls, no libc) that
 * proves tobyOS's Linux signal-delivery path end to end:
 *
 *   rt_sigaction(SIGUSR1, handler, SA_RESTORER + musl-style restorer)
 *     -> kill(getpid(), SIGUSR1)
 *       -> kernel delivers: pushes a frame, enters `handler` with RDI=sig,
 *          return address = our restorer
 *       -> handler sets `got`, returns
 *       -> restorer issues rt_sigreturn
 *       -> kernel restores context, execution resumes after kill()
 *
 * Exit 0 iff the handler actually ran (got == 1); else non-zero. This is
 * the same brandelf'd "unmodified Linux binary" shape as linux-hello.
 */

typedef unsigned long u64;
typedef long          i64;

static inline i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    i64 r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}

#define SYS_write         1
#define SYS_rt_sigaction  13
#define SYS_getpid        39
#define SYS_kill          62
#define SYS_exit_group    231
#define SIGUSR1           10
#define SA_RESTORER       0x04000000

/* Linux rt_sigaction struct, x86-64 layout. */
struct ksigaction { u64 sa_handler, sa_flags, sa_restorer, sa_mask; };

static volatile int got = 0;

__attribute__((force_align_arg_pointer))
static void handler(int sig) { (void)sig; got = 1; }

/* musl-style signal restorer: the kernel sets this as the handler's return
 * address; it simply issues rt_sigreturn (syscall 15) to restore context. */
extern void sig_restorer(void);
__asm__(".globl sig_restorer\n"
        "sig_restorer:\n"
        "  mov $15, %rax\n"
        "  syscall\n");

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { sc(SYS_write, 1, (i64)s, (i64)slen(s), 0, 0); }

__attribute__((force_align_arg_pointer))
void _start(void) {
    struct ksigaction sa = { 0 };
    sa.sa_handler  = (u64)&handler;
    sa.sa_flags    = SA_RESTORER;
    sa.sa_restorer = (u64)&sig_restorer;
    sa.sa_mask     = 0;

    if (sc(SYS_rt_sigaction, SIGUSR1, (i64)&sa, 0, 8, 0) != 0) {
        put("rt_sigaction: FAIL\n");
        sc(SYS_exit_group, 2, 0, 0, 0, 0);
    }

    i64 pid = sc(SYS_getpid, 0, 0, 0, 0, 0);
    sc(SYS_kill, pid, SIGUSR1, 0, 0, 0);
    /* If delivery worked, `handler` has already run + returned by now. */

    put(got ? "linux signal handler ran (SIGUSR1): OK\n"
            : "linux signal handler: FAIL\n");
    sc(SYS_exit_group, got ? 0 : 1, 0, 0, 0, 0);
    for (;;) { }
}
