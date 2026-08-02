/* programs/linux-qfreeze/main.c
 *
 * Permanent guard for the slice-92 -smp 4 kernel freeze: a sibling thread's
 * KERNEL-mode #PF (copy_to_user out-param) during a CoW-fork WP sweep used
 * to park on vm_quiesce while HOLDING the BKL; the forker re-takes the BKL
 * for mmap_cow_clone BEFORE tg_vm_resume, so on a FIFO ticket lock the whole
 * kernel deadlocked (READY threads never picked, [bkl] acq=0 on every CPU,
 * [qstuck] blind because the spinner is on_cpu with vm_quiesce still set).
 *
 * The window needs real parallelism (the sibling must be mid-syscall on
 * another CPU while the forker sweeps): run under WHPX with -smp >= 2 for a
 * true verdict; -smp 1 / TCG passes trivially, like linux-demrace.
 *
 * The test: NTHREADS hammer threads share the VM with main. Each loops
 *   clock_gettime(CLOCK_MONOTONIC, <pointer INTO a big touched region>)
 * so the KERNEL constantly writes user pages (the copy_to_user shape), plus
 * a user-mode store to re-dirty pages between forks. Main meanwhile fork()s
 * in a loop; every fork WP-sweeps the whole region (256 MB = 65536 PTEs, a
 * sweep long enough that hammer syscalls land inside it). On a broken kernel
 * the guest wedges on the first or second fork and the VERDICT line below
 * never prints -- the harness reads silence as the failure. On a fixed
 * kernel every fork completes and the threads' writes all survive.
 *
 * Result via exit_group():  3 = PASS   1 = FAIL (lost write)   2 = ERROR
 */

typedef unsigned long u64;
typedef long          i64;
typedef unsigned int  u32;

static inline i64 lx6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    register i64 r9  __asm__("r9")  = f;
    i64 ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}
#define SC1(n,a)           lx6(n,(i64)(a),0,0,0,0,0)
#define SC2(n,a,b)         lx6(n,(i64)(a),(i64)(b),0,0,0,0)
#define SC3(n,a,b,c)       lx6(n,(i64)(a),(i64)(b),(i64)(c),0,0,0)
#define SC4(n,a,b,c,d)     lx6(n,(i64)(a),(i64)(b),(i64)(c),(i64)(d),0,0)
#define SC6(n,a,b,c,d,e,f) lx6(n,(i64)(a),(i64)(b),(i64)(c),(i64)(d),(i64)(e),(i64)(f))

#define SYS_write          1
#define SYS_mmap           9
#define SYS_sched_yield    24
#define SYS_fork           57
#define SYS_exit           60
#define SYS_wait4          61
#define SYS_clock_gettime  228

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_PRIVATE 0x02
#define MAP_ANON    0x20

#define CLOCK_MONOTONIC 1

#define NTHREADS 3
#define NPAGES   65536              /* 256 MB: a WP sweep long enough to hit */
#define PGSZ     4096
#define FORKS    12

static void put(const char *s) {
    long n = 0; while (s[n]) n++;
    SC3(SYS_write, 1, s, n);
}
static void put_dec(const char *pfx, u64 v) {
    char b[96]; long n = 0;
    while (pfx[n]) { b[n] = pfx[n]; n++; }
    char d[20]; int nd = 0;
    do { d[nd++] = (char)('0' + (v % 10)); v /= 10; } while (v && nd < 20);
    while (nd) b[n++] = d[--nd];
    b[n++] = '\n';
    SC3(SYS_write, 1, b, n);
}

/* ---- shared state (CLONE_VM) ---- */
static volatile unsigned char *g_region;
static volatile int g_stop;
static volatile int g_fail;
static volatile int g_done[NTHREADS];        /* clean-exit handshake */
static volatile u64 g_iters[NTHREADS];       /* liveness: syscalls done */

/* Each thread owns slot t (8 bytes at offset 64+t*8) in every page it
 * visits and records what it last wrote there; verified at the end. A park
 * that ever breaks out mid-sweep (or a WP-race regression) loses writes. */
static u64 *g_last;                          /* [NTHREADS][NPAGES], mmap'd */

static int g_tid_slot[NTHREADS];

static void worker_body(int t) {
    u64 seq = 0x100000ull * (u64)(t + 1);
    u64 p = (u64)t * (NPAGES / NTHREADS);
    while (!g_stop) {
        p = (p + 613) % NPAGES;              /* co-prime stride: tour pages */
        volatile unsigned char *pg = g_region + p * PGSZ;
        /* KERNEL write into the region: clock_gettime's 16-byte timespec
         * copy_to_user is the exact shape that faulted holding the BKL. */
        SC2(SYS_clock_gettime, CLOCK_MONOTONIC, pg + 32);
        /* USER write: re-dirties the page so the next fork re-WPs it, and
         * feeds the end-of-run lost-write verification. */
        seq++;
        *(volatile u64 *)(pg + 64 + t * 8) = seq;
        g_last[(u64)t * NPAGES + p] = seq;
        g_iters[t]++;
    }
    g_done[t] = 1;
    SC1(SYS_exit, 0);
}

static int worker_entry(void) {
    for (int t = 0; t < NTHREADS; t++)
        if (__sync_bool_compare_and_swap(&g_tid_slot[t], 0, 1)) {
            worker_body(t);
            return 0;
        }
    return 0;
}

static unsigned char g_stacks[NTHREADS][16384] __attribute__((aligned(16)));

extern void _start(void);
__asm__(
  ".globl _start\n"
  "_start:\n"
  "  call main_c\n"
  "  mov %rax, %rdi\n"
  "  mov $231, %rax\n"     /* exit_group(result) */
  "  syscall\n");

/* clone trampoline (verbatim from linux-futex: fn kept in CALLEE-SAVED r12). */
extern long clone_thread(int (*fn)(void), void *child_stack_top, int flags);
__asm__(
  ".globl clone_thread\n"
  "clone_thread:\n"
  "  push %r12\n"
  "  mov %rdi, %r12\n"
  "  mov %rsi, %rsi\n"
  "  mov %edx, %edi\n"
  "  and $-16, %rsi\n"
  "  xor %edx, %edx\n"
  "  xor %r10, %r10\n"
  "  xor %r8, %r8\n"
  "  mov $56, %rax\n"
  "  syscall\n"
  "  test %rax, %rax\n"
  "  jnz 1f\n"
  "  call *%r12\n"
  "  mov $60, %rax\n"
  "  xor %edi, %edi\n"
  "  syscall\n"
  "1:\n"
  "  pop %r12\n"
  "  ret\n");

#define CLONE_VM      0x00000100
#define CLONE_FS      0x00000200
#define CLONE_FILES   0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD  0x00010000

int main_c(void);
int main_c(void) {
    put("[qfreeze] start\n");

    i64 base = SC6(SYS_mmap, 0, (u64)NPAGES * PGSZ, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base < 0 && base > -4096) { put("[qfreeze] mmap FAILED\n"); return 2; }
    g_region = (volatile unsigned char *)base;

    i64 shadow = SC6(SYS_mmap, 0, (u64)NTHREADS * NPAGES * 8,
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (shadow < 0 && shadow > -4096) { put("[qfreeze] mmap2 FAILED\n"); return 2; }
    g_last = (u64 *)shadow;

    /* Populate every PTE so each fork's WP sweep walks all of them. */
    for (u64 p = 0; p < NPAGES; p++)
        g_region[p * PGSZ] = (unsigned char)p;
    put("[qfreeze] region touched\n");

    for (int t = 0; t < NTHREADS; t++) {
        long tid = clone_thread(worker_entry,
                                g_stacks[t] + sizeof(g_stacks[t]),
                                CLONE_VM | CLONE_FS | CLONE_FILES |
                                CLONE_SIGHAND | CLONE_THREAD);
        if (tid < 0) { put("[qfreeze] clone FAILED\n"); g_stop = 1; return 2; }
    }

    /* GATE 1: the verdict is vacuous unless the hammers actually hammer.
     * (First cut of this guard "passed" with iters==0 on every thread --
     * the boot harness ran BSP-only and never scheduled them.) Wait until
     * every hammer proves it is live, bounded so a starved run errors out
     * instead of wedging the guard. */
    for (int t = 0; t < NTHREADS; t++) {
        long spins = 0;
        while (g_iters[t] < 2000) {
            SC1(SYS_sched_yield, 0);
            if (++spins > 5000000) {
                put("[qfreeze] VACUOUS: hammers never scheduled (no SMP?)\n");
                g_stop = 1;
                return 2;
            }
        }
    }
    put("[qfreeze] hammers live\n");

    u64 iters_before[NTHREADS];
    for (int t = 0; t < NTHREADS; t++) iters_before[t] = g_iters[t];

    for (int r = 0; r < FORKS; r++) {
        i64 pid = SC1(SYS_fork, 0);
        if (pid == 0) SC1(SYS_exit, 0);      /* child: vanish immediately */
        if (pid < 0) { put("[qfreeze] fork FAILED\n"); g_stop = 1; return 2; }
        int status = 0;
        SC4(SYS_wait4, pid, &status, 0, 0);
        put_dec("[qfreeze] fork round done: ", (u64)(r + 1));
        /* Give the hammers a beat between sweeps so every sweep runs against
         * live syscall traffic, not a parked queue. */
        for (int i = 0; i < 200; i++) SC1(SYS_sched_yield, 0);
    }

    /* GATE 2: hammers must have made progress DURING the fork phase --
     * otherwise no syscall ever overlapped a WP sweep and the run proved
     * nothing about the deadlock window. */
    int overlapped = 0;
    for (int t = 0; t < NTHREADS; t++)
        if (g_iters[t] > iters_before[t] + 1000) overlapped++;
    if (!overlapped) {
        put("[qfreeze] VACUOUS: no hammer advanced across the fork phase\n");
        g_stop = 1;
        return 2;
    }

    g_stop = 1;
    /* Wait for every hammer to finish its in-flight iteration, so the audit
     * below cannot race a half-recorded write. */
    for (int t = 0; t < NTHREADS; t++)
        while (!g_done[t]) SC1(SYS_sched_yield, 0);

    /* Lost-write audit: every recorded last-write must still be readable. */
    u64 checked = 0;
    for (int t = 0; t < NTHREADS; t++) {
        for (u64 p = 0; p < NPAGES; p++) {
            u64 want = g_last[(u64)t * NPAGES + p];
            if (!want) continue;
            u64 got = *(volatile u64 *)(g_region + p * PGSZ + 64 + t * 8);
            checked++;
            if (got != want) {
                put_dec("[qfreeze] LOST WRITE page ", p);
                g_fail = 1;
            }
        }
        put_dec("[qfreeze] thread iters: ", g_iters[t]);
    }
    put_dec("[qfreeze] slots verified: ", checked);

    if (g_fail) { put("[qfreeze] FAIL\n"); return 1; }
    put("[qfreeze] PASS\n");
    return 3;
}
