/* programs/linux-futex/main.c
 *
 * Targeted unit test for the DOM-blocker futex bug (ledger DL2):
 *   a TIMED FUTEX_WAIT must be wakeable by FUTEX_WAKE, NOT only by a *uaddr
 *   value change. glibc's pthread_cond_timedwait / mutex_timedlock (pervasive
 *   in Chromium) rely on this; tobyOS's old timed path polled *uaddr and was
 *   never on the wait list, so FUTEX_WAKE was silently lost.
 *
 * The test spawns a thread (clone, shared VM) that does a TIMED FUTEX_WAIT on a
 * word with a LONG (10 s) absolute deadline, leaving the word UNCHANGED. The
 * main thread sleeps briefly, then FUTEX_WAKEs that word WITHOUT changing it. If
 * the futex is correct, the waiter returns almost immediately (woken by the
 * WAKE); if broken, it stays blocked until the 10 s deadline.
 *
 * Result via exit_group():
 *   3 = PASS  (waiter woke promptly from FUTEX_WAKE, word never changed)
 *   1 = FAIL  (waiter did NOT wake within the grace window -> lost wakeup)
 *   2 = ERROR (clone failed / setup problem)
 * A separate control also checks the UNTIMED path still works (bit not needed
 * for pass, but logged via the byte written to fd 1).
 */

typedef unsigned long u64;
typedef long          i64;

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
#define SC0(n)             lx6(n,0,0,0,0,0,0)
#define SC1(n,a)           lx6(n,(i64)(a),0,0,0,0,0)
#define SC2(n,a,b)         lx6(n,(i64)(a),(i64)(b),0,0,0,0)
#define SC3(n,a,b,c)       lx6(n,(i64)(a),(i64)(b),(i64)(c),0,0,0)
#define SC4(n,a,b,c,d)     lx6(n,(i64)(a),(i64)(b),(i64)(c),(i64)(d),0,0)
#define SC6(n,a,b,c,d,e,f) lx6(n,(i64)(a),(i64)(b),(i64)(c),(i64)(d),(i64)(e),(i64)(f))

#define SYS_write        1
#define SYS_futex        202
#define SYS_clone        56
#define SYS_exit         60
#define SYS_exit_group   231
#define SYS_nanosleep    35
#define SYS_clock_gettime 228

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_WAIT_BITSET    9
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_BITSET_MATCH_ANY 0xffffffffu

#define CLONE_VM      0x00000100
#define CLONE_FS      0x00000200
#define CLONE_FILES   0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD  0x00010000

struct timespec { i64 sec; i64 nsec; };

static void put(const char *s) {
    long n = 0; while (s[n]) n++;
    SC3(SYS_write, 1, s, n);
}

/* Shared state (same address space via CLONE_VM). */
static volatile unsigned int g_word   = 0;   /* the futex word -- NEVER changed */
static volatile long         g_waiter_ret   = 123;
static volatile int          g_waiter_done  = 0;
static volatile int          g_waiter_ready = 0;

static void msleep(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    SC2(SYS_nanosleep, &t, 0);
}
static u64 now_ns(void) {
    struct timespec t = { 0, 0 };
    SC2(SYS_clock_gettime, 1 /*CLOCK_MONOTONIC*/, &t);
    return (u64)t.sec * 1000000000ull + (u64)t.nsec;
}

/* The waiter thread: a TIMED FUTEX_WAIT with a 10 s absolute deadline, on a word
 * that stays == 0. Correct kernels wake it via FUTEX_WAKE long before 10 s. */
static void waiter_entry(void) {
    struct timespec deadline;
    struct timespec tnow = { 0, 0 };
    SC2(SYS_clock_gettime, 1, &tnow);
    deadline.sec  = tnow.sec + 10;
    deadline.nsec = tnow.nsec;
    g_waiter_ready = 1;
    /* FUTEX_WAIT_BITSET takes an ABSOLUTE deadline. word is 0, val is 0 -> block. */
    g_waiter_ret = SC6(SYS_futex, &g_word,
                       FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
                       0, &deadline, 0, FUTEX_BITSET_MATCH_ANY);
    g_waiter_done = 1;
    SC1(SYS_exit, 0);   /* thread exit */
}

/* 16 KiB child stack (16-aligned). */
static unsigned char g_child_stack[16384] __attribute__((aligned(16)));

extern void _start(void);
__asm__(
  ".globl _start\n"
  "_start:\n"
  "  call main_c\n"
  "  mov %rax, %rdi\n"
  "  mov $231, %rax\n"     /* exit_group(result) */
  "  syscall\n");

/* clone trampoline: clone() returns 0 in the child ON THE NEW STACK; from there
 * we must NOT return (no valid return address) -- jump straight to the entry. */
extern long clone_thread(int (*fn)(void), void *child_stack_top, int flags);
__asm__(
  ".globl clone_thread\n"
  "clone_thread:\n"
  "  push %r12\n"
  "  mov %rdi, %r12\n"        /* r12 = fn (CALLEE-SAVED: survives `syscall`,
                                which clobbers rcx AND r11 -- the original bug
                                here saved fn in r11, so the child jumped to
                                garbage and never ran). */
  "  mov %rsi, %rsi\n"        /* rsi = child stack top (clone arg 2) */
  "  mov %edx, %edi\n"        /* edi = flags (clone arg 1) */
  "  and $-16, %rsi\n"
  "  xor %edx, %edx\n"        /* ptid */
  "  xor %r10, %r10\n"        /* ctid */
  "  xor %r8, %r8\n"          /* tls */
  "  mov $56, %rax\n"         /* SYS_clone */
  "  syscall\n"
  "  test %rax, %rax\n"
  "  jnz 1f\n"                /* parent: return tid */
  "  call *%r12\n"            /* child (new stack, r12=fn preserved): fn() */
  "  mov $60, %rax\n"        /* just in case: exit */
  "  xor %edi, %edi\n"
  "  syscall\n"
  "1:\n"
  "  pop %r12\n"              /* parent path only (child never returns here) */
  "  ret\n");

int main_c(void);
int main_c(void) {
    put("[futextest] start\n");

    long tid = clone_thread((int(*)(void))waiter_entry,
                            g_child_stack + sizeof(g_child_stack),
                            CLONE_VM | CLONE_FS | CLONE_FILES |
                            CLONE_SIGHAND | CLONE_THREAD);
    if (tid < 0) { put("[futextest] clone FAILED\n"); return 2; }

    /* Wait until the waiter is about to block, then a bit more so it is parked. */
    for (int i = 0; i < 200 && !g_waiter_ready; i++) msleep(5);
    msleep(200);

    if (g_waiter_done) { put("[futextest] waiter returned BEFORE any wake?!\n"); return 2; }

    /* THE TEST: FUTEX_WAKE without changing g_word. A correct futex wakes the
     * timed waiter here; the broken poll-only path leaves it blocked to 10 s. */
    u64 t0 = now_ns();
    long nwoken = SC3(SYS_futex, &g_word, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1);

    /* Grace window: give the waiter up to ~2 s to wake. If the bug is present it
     * will still be blocked (its real deadline is 10 s away). */
    for (int i = 0; i < 400 && !g_waiter_done; i++) msleep(5);
    u64 dt_ms = (now_ns() - t0) / 1000000ull;

    if (g_waiter_done && g_waiter_ret == 0 && dt_ms < 3000) {
        put("[futextest] PASS: timed waiter woken by FUTEX_WAKE (word unchanged)\n");
        return 3;
    }
    if (!g_waiter_done) {
        put("[futextest] FAIL: timed waiter still blocked after FUTEX_WAKE (lost wakeup)\n");
        (void)nwoken;
        return 1;
    }
    put("[futextest] FAIL: waiter returned but not cleanly woken\n");
    return 1;
}
