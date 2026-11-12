/* programs/linux-mprot/main.c -- W7i (docs/roadmap-2026-11.md): mprotect's
 * PROT_NONE on pages that are THERE.
 *
 * A genuine Linux x86-64 static ELF (raw syscalls, no libc), run on WSL's
 * Linux first (the oracle: exit 63 there) and in lxposix's gate. Until W7i a
 * page that had been touched stayed readable after mprotect(PROT_NONE): an
 * x86 page-table entry with the user bit is always readable, and tobyOS kept
 * the bit -- only a never-touched page faulted. Each access that must fault
 * runs under a SIGSEGV handler that records the signal and the address and
 * opens the page (mprotect, as a guard-page user does), so that the access
 * is made again and goes through.
 *
 *   bit0  a page written, then PROT_NONE: a read is a SIGSEGV at its address
 *   bit1  ... and so is a write
 *   bit2  the contents are intact, the write's byte in its place
 *   bit3  PROT_NONE then PROT_READ: the contents read back, no SIGSEGV; a
 *         write SIGSEGVs
 *   bit4  a PROT_NONE mapping never touched: a read SIGSEGVs
 *   bit5  the kernel's own access: read(2) into a PROT_NONE page and
 *         write(2) from one (touched) are EFAULT, as on Linux
 *
 * Exit 63 iff all six. */

typedef unsigned long u64;
typedef long          i64;

static inline i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    register i64 r9  __asm__("r9")  = f;
    i64 r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
#define SYS_read          0
#define SYS_write         1
#define SYS_close         3
#define SYS_mmap          9
#define SYS_mprotect      10
#define SYS_rt_sigaction  13
#define SYS_pipe          22
#define SYS_exit          60
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define SIGSEGV 11
#define EFAULT  14
#define SA_SIGINFO  0x4
#define SA_RESTORER 0x04000000

/* the kernel enters _start with rsp 16-aligned, not as a call would: realign */
__asm__(".globl _start\n_start:\n\txor %ebp, %ebp\n\tand $-16, %rsp\n\tcall lmp_main\n\thlt\n");
__asm__(".globl lmp_restorer\nlmp_restorer:\n\tmov $15, %eax\n\tsyscall\n\thlt\n");
extern void lmp_restorer(void);

static volatile int g_sig, g_count;
static volatile u64 g_addr;
__attribute__((used)) static void on_segv(int sig, void *info, void *uc) {
    (void)uc;
    g_sig = sig;
    g_count++;
    g_addr = *(u64 *)((char *)info + 16);            /* si_addr */
    sc(SYS_mprotect, (i64)(g_addr & ~4095ul), 4096, PROT_READ | PROT_WRITE, 0, 0, 0);
}
/* exactly one SIGSEGV, at p, since the last call */
static int faulted(volatile unsigned char *p) {
    int ok = g_sig == SIGSEGV && g_count == 1 && g_addr == (u64)p;
    g_sig = 0;
    g_count = 0;
    g_addr = 0;
    return ok;
}

static void say(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    sc(SYS_write, 1, (i64)s, (i64)n, 0, 0, 0);
}
#define PAT(i) ((unsigned char)((i) * 7 + 1))

__attribute__((used, noreturn)) void lmp_main(void) {
    int bits = 0;
    struct { void *h; u64 flags; void *restorer; u64 mask; } act = {
        (void *)on_segv, SA_SIGINFO | SA_RESTORER, (void *)lmp_restorer, 0 };
    if (sc(SYS_rt_sigaction, SIGSEGV, (i64)&act, 0, 8, 0, 0) != 0) sc(SYS_exit, 101, 0, 0, 0, 0, 0);
    volatile unsigned char *m = (volatile unsigned char *)sc(SYS_mmap, 0, 4 * 4096, PROT_READ | PROT_WRITE,
                                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((i64)m < 0) sc(SYS_exit, 100, 0, 0, 0, 0, 0);
    for (int i = 0; i < 4 * 4096; i++) m[i] = PAT(i);

    /* bits 0-2: page 0 */
    sc(SYS_mprotect, (i64)m, 4096, PROT_NONE, 0, 0, 0);
    unsigned char v0 = m[8];
    if (faulted(m + 8) && v0 == PAT(8)) bits |= 1;
    sc(SYS_mprotect, (i64)m, 4096, PROT_NONE, 0, 0, 0);
    m[9] = 0x99;
    if (faulted(m + 9)) bits |= 2;
    int intact = 1;
    for (int i = 0; i < 4096; i++) if (m[i] != (i == 9 ? 0x99 : PAT(i))) intact = 0;
    if (intact && !g_count) bits |= 4;

    /* bit 3: page 1, PROT_NONE then PROT_READ */
    sc(SYS_mprotect, (i64)(m + 4096), 4096, PROT_NONE, 0, 0, 0);
    sc(SYS_mprotect, (i64)(m + 4096), 4096, PROT_READ, 0, 0, 0);
    unsigned char v = m[4096 + 5];
    int r_ok = !g_count && v == PAT(4096 + 5);
    m[4096 + 6] = 0x99;
    if (r_ok && faulted(m + 4096 + 6) && m[4096 + 6] == 0x99) bits |= 8;

    /* bit 4: a mapping never touched */
    volatile unsigned char *u = (volatile unsigned char *)sc(SYS_mmap, 0, 4096, PROT_NONE,
                                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((i64)u >= 0) {
        unsigned char z = u[3];
        if (faulted(u + 3) && z == 0) bits |= 16;
    }

    /* bit 5: the kernel's access -- page 2 (touched) PROT_NONE */
    sc(SYS_mprotect, (i64)(m + 8192), 4096, PROT_NONE, 0, 0, 0);
    int fd[2];
    if (sc(SYS_pipe, (i64)fd, 0, 0, 0, 0, 0) == 0) {
        i64 w = sc(SYS_write, fd[1], (i64)(m + 8192), 16, 0, 0, 0);     /* from it */
        sc(SYS_write, fd[1], (i64)"0123456789abcdef", 16, 0, 0, 0);
        i64 r = sc(SYS_read, fd[0], (i64)(m + 8192), 16, 0, 0, 0);      /* into it */
        if (w == -EFAULT && r == -EFAULT && !g_count) bits |= 32;
        sc(SYS_close, fd[0], 0, 0, 0, 0, 0);
        sc(SYS_close, fd[1], 0, 0, 0, 0, 0);
    }

    char line[] = "linux-mprot: bits=00\n";
    line[18] = (char)('0' + bits / 10);
    line[19] = (char)('0' + bits % 10);
    say(line);
    sc(SYS_exit, bits, 0, 0, 0, 0, 0);
    for (;;) {}
}
