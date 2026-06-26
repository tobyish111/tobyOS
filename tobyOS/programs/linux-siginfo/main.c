/* programs/linux-siginfo/main.c
 *
 * A GENUINE Linux x86-64 static ELF (raw Linux syscalls, no libc) that proves
 * tobyOS's B15 signal-completeness work: SA_SIGINFO 3-arg handlers receiving
 * x86-64 *Linux-ABI-layout* siginfo_t / ucontext_t, AND catchable SYNCHRONOUS
 * CPU faults (SIGSEGV) delivered to a user handler with the right fault address.
 *
 * Two scenarios, both checked at the exact Linux struct byte-offsets (so a
 * native-layout struct would fail the checks):
 *
 *   1. ASYNC: rt_sigaction(SIGUSR1, SA_SIGINFO|SA_RESTORER) -> kill(self).
 *      The 3-arg handler verifies:
 *        - info->si_signo (off 0)  == SIGUSR1
 *        - info->si_code  (off 8)  == SI_USER (kill origin)
 *        - info->si_pid   (off 16) == our pid          (kill _kill union)
 *        - ucontext->uc_mcontext.gregs[REG_CSGSFS] low16 == 0x33  (user %cs)
 *        - ucontext->uc_mcontext.gregs[REG_RIP]  != 0
 *      then returns NORMALLY (restorer -> rt_sigreturn -> resume after kill).
 *
 *   2. SYNC FAULT: rt_sigaction(SIGSEGV, SA_SIGINFO|SA_RESTORER) -> deref a
 *      known-unmapped pointer. The handler verifies:
 *        - info->si_signo (off 0)  == SIGSEGV
 *        - info->si_addr  (off 16) == the faulting address
 *      then exit_group(42) iff BOTH scenarios passed (else a diagnostic code).
 *
 * On a real Linux kernel this program would behave identically. The only
 * build-time touch is the brandelf EI_OSABI=3 stamp.
 */

typedef unsigned long      u64;
typedef long               i64;
typedef unsigned int       u32;
typedef unsigned long      uintptr_t;

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
#define SIGSEGV           11
#define SI_USER            0
#define SA_SIGINFO  0x00000004
#define SA_RESTORER 0x04000000

#define BAD_ADDR  ((volatile int *)0x000000000DEAD000UL)

/* Linux rt_sigaction struct, x86-64 layout. */
struct ksigaction { u64 sa_handler, sa_flags, sa_restorer, sa_mask; };

/* Read Linux siginfo_t / ucontext_t fields by their fixed ABI byte offsets,
 * so the checks below actually pin down the LAYOUT (not just the values). */
static inline int  si_signo(void *info){ return *(int *)((char *)info + 0); }
static inline int  si_code (void *info){ return *(int *)((char *)info + 8); }
static inline int  si_pid  (void *info){ return *(int *)((char *)info + 16); }
static inline u64  si_addr (void *info){ return *(u64 *)((char *)info + 16); }
/* gregs[] starts at uc + 40; REG_CSGSFS=18, REG_RIP=16. */
static inline u64  greg(void *uc, int i){ return *(u64 *)((char *)uc + 40 + (i)*8); }
#define REG_RIP     16
#define REG_CSGSFS  18

static volatile int g_pid      = 0;
static volatile int g_usr1_ok  = 0;
static volatile int g_usr1_bits = 0;   /* per-check failure bitmask (0 == all OK) */

__attribute__((force_align_arg_pointer))
static void usr1_handler(int sig, void *info, void *uc) {
    int b = 0;
    if (sig != SIGUSR1)                          b |= 1;
    if (si_signo(info) != SIGUSR1)               b |= 2;
    if (si_code(info)  != SI_USER)               b |= 4;
    if (si_pid(info)   != g_pid)                 b |= 8;
    if ((greg(uc, REG_CSGSFS) & 0xffff) != 0x33) b |= 16;
    if (greg(uc, REG_RIP) == 0)                  b |= 32;
    g_usr1_bits = b;
    g_usr1_ok = (b == 0);
    /* returns normally -> restorer -> rt_sigreturn -> resume after kill() */
}

__attribute__((force_align_arg_pointer))
static void segv_handler(int sig, void *info, void *uc) {
    (void)uc;
    int segv_ok = 1;
    if (sig != SIGSEGV)                       segv_ok = 0;
    if (si_signo(info) != SIGSEGV)            segv_ok = 0;
    if (si_addr(info)  != (u64)(uintptr_t)BAD_ADDR) segv_ok = 0;

    int code;
    if (g_usr1_ok && segv_ok)      code = 42;   /* full PASS              */
    else if (!g_usr1_ok &&  segv_ok) code = 30; /* async siginfo failed   */
    else if ( g_usr1_ok && !segv_ok) code = 31; /* fault siginfo failed   */
    else                           code = 32;   /* both failed            */
    sc(SYS_exit_group, code, 0, 0, 0, 0);
    for (;;) {}
}

/* musl-style restorer: kernel sets it as the handler return address; it just
 * issues rt_sigreturn (syscall 15) to restore the interrupted context. */
extern void sig_restorer(void);
__asm__(".globl sig_restorer\n"
        "sig_restorer:\n"
        "  mov $15, %rax\n"
        "  syscall\n");

static u64 slen(const char *s){ u64 n=0; while(s[n]) n++; return n; }
static void put(const char *s){ sc(SYS_write, 1, (i64)s, (i64)slen(s), 0, 0); }

static int install(int signo, void *handler) {
    struct ksigaction sa = { 0 };
    sa.sa_handler  = (u64)(uintptr_t)handler;
    sa.sa_flags    = SA_SIGINFO | SA_RESTORER;
    sa.sa_restorer = (u64)&sig_restorer;
    sa.sa_mask     = 0;
    return (int)sc(SYS_rt_sigaction, signo, (i64)&sa, 0, 8, 0);
}

__attribute__((force_align_arg_pointer))
void _start(void) {
    g_pid = (int)sc(SYS_getpid, 0, 0, 0, 0, 0);

    if (install(SIGUSR1, (void *)&usr1_handler) != 0) {
        put("[b15] rt_sigaction SIGUSR1: FAIL\n");
        sc(SYS_exit_group, 2, 0, 0, 0, 0);
    }
    if (install(SIGSEGV, (void *)&segv_handler) != 0) {
        put("[b15] rt_sigaction SIGSEGV: FAIL\n");
        sc(SYS_exit_group, 3, 0, 0, 0, 0);
    }

    put("[b15] raising SIGUSR1 (async SA_SIGINFO)\n");
    sc(SYS_kill, g_pid, SIGUSR1, 0, 0, 0);
    /* handler already ran + returned here */
    if (g_usr1_ok) {
        put("[b15] async siginfo/ucontext layout: OK\n");
    } else {
        char msg[] = "[b15] async layout FAIL bits=0x00 "
                     "(2=signo 4=code 8=pid 16=csgsfs 32=rip)\n";
        /* fill the two hex nibbles of bits at fixed positions */
        const char *hx = "0123456789abcdef";
        int idx = 0; while (msg[idx] != 'x') idx++;
        msg[idx+1] = hx[(g_usr1_bits >> 4) & 0xf];
        msg[idx+2] = hx[g_usr1_bits & 0xf];
        put(msg);
    }

    put("[b15] dereferencing unmapped pointer (sync SIGSEGV)\n");
    *BAD_ADDR = 0x1234;          /* #PF -> catchable SIGSEGV -> segv_handler */

    /* Only reached if the fault was NOT delivered to our handler. */
    put("[b15] SIGSEGV not caught -- FAIL\n");
    sc(SYS_exit_group, 10, 0, 0, 0, 0);
    for (;;) {}
}
