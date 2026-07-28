/* programs/linux-demrace/main.c
 *
 * Permanent guard for the slice-50b corruption bug: concurrent DEMAND FAULTS
 * on the SAME page must not lose writes.
 *
 * The bug: the #PF resolver runs BKL-free; map_4k silently REPLACED a present
 * PTE. Two threads first-touching the same freshly-madvise(MADV_DONTNEED)'d
 * page both saw it absent (TOCTOU), both allocated a zero frame, and the
 * SECOND install replaced the first -- discarding every byte the first thread
 * had already written through its frame (chrome's renderer read NULL out of a
 * live Vec-of-pointers => the 0x208c13a YouTube crashes). Fixed by
 * vmm_map_page_if_absent (atomic check+install under the vmm lock).
 *
 * The test: NTHREADS threads + main run lock-step rounds over a private anon
 * region. Each round: main MADV_DONTNEEDs the whole region (drops every page),
 * then ALL threads concurrently first-touch every page (each writes a magic
 * value at its own slot inside the SAME page -- maximal install-race
 * pressure), then every thread verifies BOTH its own slot and the NEXT
 * thread's slot in every page. Cross-verification matters: a thread's own
 * read-back can be satisfied by its own stale TLB entry for a replaced frame,
 * masking the loss; a peer's view goes through the surviving PTE.
 *
 * On a broken kernel the writes scatter across replaced frames and verify
 * sees zeros within a few rounds under real parallelism (WHPX). On a fixed
 * kernel exactly one frame per page survives per round; losers back off and
 * re-fault onto it ([pfrace] counts the absorbed races).
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
#define SC6(n,a,b,c,d,e,f) lx6(n,(i64)(a),(i64)(b),(i64)(c),(i64)(d),(i64)(e),(i64)(f))

#define SYS_write       1
#define SYS_mmap        9
#define SYS_madvise     28
#define SYS_sched_yield 24
#define SYS_exit        60

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_PRIVATE 0x02
#define MAP_ANON    0x20
#define MADV_DONTNEED 4

#define NTHREADS 4
#define NPAGES   16
#define ROUNDS   1500
#define PGSZ     4096

static void put(const char *s) {
    long n = 0; while (s[n]) n++;
    SC3(SYS_write, 1, s, n);
}
static void put_hex(u64 v) {
    char b[19]; b[0]='0'; b[1]='x';
    for (int i = 0; i < 16; i++)
        b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
    b[18] = '\n';
    SC3(SYS_write, 1, b, 19);
}

/* ---- shared state (CLONE_VM) ---- */
static volatile unsigned char *g_region;
static volatile int  g_fail;
static volatile u64  g_fail_detail;          /* first failure: r<<32|p<<8|slot */
static volatile int  g_stop;

/* Sense-counting barrier for NTHREADS+1 participants. sched_yield in the
 * spin so oversubscribed vCPUs (5 participants, 4 CPUs) always make
 * progress under WHPX. */
static volatile int g_bar_count, g_bar_gen;
static void barrier(void) {
    int g = g_bar_gen;
    if (__sync_add_and_fetch(&g_bar_count, 1) == NTHREADS + 1) {
        g_bar_count = 0;
        __sync_synchronize();
        g_bar_gen = g + 1;
    } else {
        while (g_bar_gen == g && !g_stop) SC1(SYS_sched_yield, 0);
    }
}

static u64 magic(int r, int p, int t) {
    return 0x5A5A000000000000ull | ((u64)(u32)r << 16) | ((u64)p << 8) | (u64)t;
}

static int g_tid_slot[NTHREADS];             /* thread index handoff */

static void worker_body(int t) {
    for (int r = 0; r < ROUNDS && !g_stop; r++) {
        barrier();                            /* A: round start            */
        barrier();                            /* B: main finished madvise  */
        /* Concurrent first-touch: ONE page per round, all threads released
         * from the barrier straight onto it -- the tightest same-page fault
         * collision (touring all pages, even in the same order, MEASURED 0
         * [pfrace] hits: barrier-exit skew already spread the threads past
         * the ~1us translate->alloc->zero->install window). */
        {
            int p = r % NPAGES;
            *(volatile u64 *)(g_region + p * PGSZ + 64 + t * 8) = magic(r, p, t);
        }
        barrier();                            /* C: all writes done        */
        {
            int p = r % NPAGES;
            int u = (t + 1) % NTHREADS;       /* cross-check the peer slot */
            u64 own  = *(volatile u64 *)(g_region + p * PGSZ + 64 + t * 8);
            u64 peer = *(volatile u64 *)(g_region + p * PGSZ + 64 + u * 8);
            if (own != magic(r, p, t) || peer != magic(r, p, u)) {
                if (__sync_fetch_and_add(&g_fail, 1) == 0)
                    g_fail_detail = ((u64)(u32)r << 32) | ((u64)p << 8) |
                                    (u64)(own != magic(r, p, t) ? t : u);
                g_stop = 1;
            }
        }
        barrier();                            /* D: verified               */
    }
    SC1(SYS_exit, 0);
}

static int worker_entry(void) {
    /* claim an index */
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
    put("[demrace] start\n");

    i64 base = SC6(SYS_mmap, 0, NPAGES * PGSZ, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base < 0 && base > -4096) { put("[demrace] mmap FAILED\n"); return 2; }
    g_region = (volatile unsigned char *)base;

    for (int t = 0; t < NTHREADS; t++) {
        long tid = clone_thread(worker_entry,
                                g_stacks[t] + sizeof(g_stacks[t]),
                                CLONE_VM | CLONE_FS | CLONE_FILES |
                                CLONE_SIGHAND | CLONE_THREAD);
        if (tid < 0) { put("[demrace] clone FAILED\n"); g_stop = 1; return 2; }
    }

    for (int r = 0; r < ROUNDS && !g_stop; r++) {
        barrier();                            /* A */
        SC3(SYS_madvise, g_region, NPAGES * PGSZ, MADV_DONTNEED);
        barrier();                            /* B */
        barrier();                            /* C (main writes nothing)   */
        barrier();                            /* D */
    }
    g_stop = 1;   /* release any thread still parked in a barrier on FAIL */

    if (g_fail) {
        put("[demrace] FAIL: lost write detected (round<<32|page<<8|thread):\n");
        put_hex(g_fail_detail);
        return 1;
    }
    put("[demrace] PASS: no lost writes across concurrent demand faults\n");
    return 3;
}
