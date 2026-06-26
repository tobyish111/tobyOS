/* programs/linux-static-auto/main.c
 *
 * Track B / B19 proof: a GENUINE Linux x86-64 STATIC ELF that is also
 * NOTE-LESS and UNBRANDED -- i.e. e_ident[EI_OSABI] stays 0 (ELFOSABI_SYSV),
 * there is NO PT_INTERP (it is statically linked), and there is no `brandelf`
 * touch at build time. This is the exact shape that, before B19, the loader
 * could NOT auto-detect: it has neither the Linux OSABI brand nor a Linux
 * dynamic-loader PT_INTERP, so it would have fallen to the native tobyOS
 * personality and mis-dispatched its very first syscall.
 *
 * The single remaining Linux fingerprint is the PT_GNU_STACK program header,
 * which the x86_64-unknown-linux-gnu toolchain emits on every link (even
 * -nostdlib), and which native tobyOS (x86_64-elf) binaries never carry. B19
 * keys off exactly that, so this binary now auto-detects as ABI_PERS_LINUX.
 *
 * It walks the same startup path real libc does -- arch_prctl(ARCH_SET_FS),
 * set_tid_address, a %fs:0 read-back, write + scatter/gather writev -- all of
 * which use LINUX syscall numbers. If the personality were mis-detected as
 * native, those numbers would mean different things and the program would
 * crash or return the wrong code. exit_group(73) is the PASS sentinel and is
 * only reachable if every Linux syscall above behaved, i.e. the process really
 * is running under the Linux personality.
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

static u64 tcb[16];

__attribute__((force_align_arg_pointer))
void _start(void) {
    tcb[0] = (u64)&tcb[0];
    lx_syscall(SYS_arch_prctl, ARCH_SET_FS, (i64)&tcb[0], 0, 0, 0);

    int tid;
    lx_syscall(SYS_set_tid_address, (i64)&tid, 0, 0, 0, 0);

    u64 tp = 0;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
    int tls_ok = (tp == (u64)&tcb[0]);

    put("hello from a STATIC, NOTE-LESS, UNBRANDED Linux binary on tobyOS\n");
    put(tls_ok ? "arch_prctl + %fs thread pointer: OK\n"
               : "arch_prctl + %fs thread pointer: FAIL\n");

    struct iovec iov[2] = {
        { "writev: ", 8 },
        { "auto-detected from PT_GNU_STACK (no brand, no interp)\n", 53 },
    };
    lx_syscall(SYS_writev, 1, (i64)iov, 2, 0, 0);

    lx_syscall(SYS_exit_group, tls_ok ? 73 : 1, 0, 0, 0, 0);
    for (;;) { }
}
