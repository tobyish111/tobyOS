/* programs/linux-join/main.c
 *
 * A genuine Linux x86-64 static ELF (raw syscalls, no libc) proving tobyOS's
 * B11 pthread_join support. It exercises the EXACT kernel contract glibc/musl
 * pthread_join relies on -- no libc internals, just the syscalls:
 *
 *   1. set_tid_address(&slot)          -> registers the main thread's exit futex
 *   2. clone(... CLONE_CHILD_CLEARTID, ctid=&tids[i])  spawns N joinable threads
 *      sharing this address space; each does `lock add` to a shared sum, then
 *      SYS_exit (thread-only).
 *   3. On each thread's exit the KERNEL writes 0 to its tids[i] and FUTEX_WAKEs
 *      it, so the main thread's futex(FUTEX_WAIT, &tids[i], tid) returns -- that
 *      wake + clear IS what pthread_join blocks on.
 *
 * Exit 0 iff all N threads ran (sum == N*7) AND the kernel cleared every tid
 * slot to 0 (the join rendezvous fired). Non-zero exit pinpoints the failure.
 *
 * Each clone + child path is inline asm so the parent/child split never depends
 * on C touching the (now-divided) stack.
 */

typedef unsigned long u64;
typedef long          i64;

static inline i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    i64 r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}

#define SYS_write           1
#define SYS_sched_yield     24
#define SYS_clone           56
#define SYS_exit            60
#define SYS_futex           202
#define SYS_set_tid_address 218
#define SYS_exit_group      231

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

/* clone flags a joinable pthread uses (CLONE_VM => thread; PARENT_SETTID +
 * CHILD_CLEARTID => the kernel publishes/clears the tid in the shared word). */
#define CLONE_VM            0x00000100
#define CLONE_FS            0x00000200
#define CLONE_FILES         0x00000400
#define CLONE_SIGHAND       0x00000800
#define CLONE_THREAD        0x00010000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define JOIN_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | \
                    CLONE_THREAD | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID)

#define NTHREADS 4
#define PER_THREAD_ADD 7

static volatile int g_sum = 0;
static volatile int tids[NTHREADS];
static int          main_tid_slot;
static char         stacks[NTHREADS][16384] __attribute__((aligned(16)));

/* clone one joinable thread on its own stack, ctid=tidp. CHILD adds to g_sum
 * and SYS_exits (asm-only, no C stack use across the split). Returns tid. */
static i64 spawn_one(void *stk, volatile int *tidp) {
    i64 ret;
    register i64 r10 __asm__("r10") = (i64)tidp;   /* ctid (arg4) */
    register i64 r8  __asm__("r8")  = 0;           /* tls  (arg5) */
    __asm__ volatile(
        "syscall\n\t"
        "test %%rax, %%rax\n\t"
        "jnz 1f\n\t"                       /* parent: rax = tid != 0 */
        "lock addl %[add], %[sum]\n\t"     /* child: shared sum += 7 */
        "movl $60, %%eax\n\t"              /* SYS_exit (this thread only) */
        "xor %%edi, %%edi\n\t"
        "syscall\n\t"
        "1:\n\t"
        : "=a"(ret), [sum] "=m"(g_sum)
        : "a"((i64)SYS_clone), "D"((i64)JOIN_FLAGS), "S"((i64)stk),
          "d"((i64)tidp) /* ptid (arg3) */, "r"(r10), "r"(r8),
          [add] "i"(PER_THREAD_ADD)
        : "rcx", "r11", "memory");
    return ret;
}

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { sc(SYS_write, 1, (i64)s, (i64)slen(s), 0, 0); }

__attribute__((noreturn))
static void done(int code) { sc(SYS_exit_group, code, 0, 0, 0, 0); for (;;) {} }

__attribute__((force_align_arg_pointer))
void _start(void) {
    /* (1) register the main thread's exit futex; return value must be our tid. */
    i64 mytid = sc(SYS_set_tid_address, (i64)&main_tid_slot, 0, 0, 0, 0);
    if (mytid <= 0) { put("set_tid_address bad tid\n"); done(2); }

    /* (2) spawn N joinable threads. */
    for (int i = 0; i < NTHREADS; i++) {
        void *top = stacks[i] + sizeof(stacks[i]);
        i64 tid = spawn_one(top, &tids[i]);
        if (tid < 0) { put("clone FAILED\n"); done(3); }
    }

    /* (3) join each: block in futex until the kernel zeroes tids[i] on exit. */
    for (int i = 0; i < NTHREADS; i++) {
        for (;;) {
            int v = tids[i];
            if (v == 0) break;                 /* kernel already cleared it */
            sc(SYS_futex, (i64)&tids[i], FUTEX_WAIT, v, 0, 0);
        }
    }

    /* verify: every join rendezvous fired AND every thread ran exactly once. */
    for (int i = 0; i < NTHREADS; i++)
        if (tids[i] != 0) { put("a tid was not cleared by the kernel\n"); done(4); }

    if (g_sum != NTHREADS * PER_THREAD_ADD) {
        put("sum mismatch: not all threads ran\n"); done(5);
    }

    put("linux pthread_join contract (clone CHILD_CLEARTID + futex wake) OK\n");
    done(0);
}
