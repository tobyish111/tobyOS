/* programs/linux-thread/main.c
 *
 * A genuine Linux x86-64 static ELF (raw syscalls, no libc) proving
 * tobyOS's Linux thread support: it spawns a real thread with
 * clone(CLONE_VM|...) (the same family of flags pthread_create uses), the
 * thread runs in the SHARED address space, sets a shared flag, and exits;
 * the parent yields until it sees the flag, then reports.
 *
 * Exit 0 iff the cloned thread actually ran.
 *
 * The clone + child path is done entirely in inline asm so the parent/child
 * divergence doesn't depend on C touching the (now-split) stack.
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

#define SYS_write       1
#define SYS_clone       56
#define SYS_exit        60          /* thread exit */
#define SYS_sched_yield 24
#define SYS_exit_group  231

/* pthread_create-style flag set; CLONE_VM is what makes it a thread. */
#define CLONE_VM        0x00000100
#define CLONE_FS        0x00000200
#define CLONE_FILES     0x00000400
#define CLONE_SIGHAND   0x00000800
#define CLONE_THREAD    0x00010000
#define THREAD_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD)

static volatile int g_ran = 0;
static char child_stack[16384] __attribute__((aligned(16)));

/* clone(THREAD_FLAGS, child_stack_top, 0, 0, 0). In the CHILD (rax==0) we
 * set g_ran and exit the thread, entirely in asm so no C stack use occurs
 * across the split. Returns the new tid to the parent (or <0 on error). */
static i64 spawn_thread(void) {
    void *stk = child_stack + sizeof(child_stack);   /* stacks grow down */
    i64 ret;
    register i64 r10 __asm__("r10") = 0;              /* child_tid */
    register i64 r8  __asm__("r8")  = 0;              /* tls */
    __asm__ volatile(
        "syscall\n\t"
        "test %%rax, %%rax\n\t"
        "jnz 1f\n\t"                 /* parent: rax = tid != 0 -> done */
        "movl $1, %[ran]\n\t"        /* child: shared flag = 1 */
        "movl $60, %%eax\n\t"        /* SYS_exit (this thread only) */
        "xor %%edi, %%edi\n\t"
        "syscall\n\t"                /* child exits; never returns */
        "1:\n\t"
        : "=a"(ret), [ran] "=m"(g_ran)
        : "a"((i64)SYS_clone), "D"((i64)THREAD_FLAGS), "S"(stk),
          "d"((i64)0 /*parent_tid*/), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { sc(SYS_write, 1, (i64)s, (i64)slen(s), 0, 0); }

__attribute__((force_align_arg_pointer))
void _start(void) {
    i64 tid = spawn_thread();
    if (tid < 0) {
        put("clone(CLONE_VM) FAILED\n");
        sc(SYS_exit_group, 2, 0, 0, 0, 0);
    }
    /* Wait for the thread (shares our memory via CLONE_VM) to set g_ran. */
    for (int i = 0; i < 2000000 && !g_ran; i++)
        sc(SYS_sched_yield, 0, 0, 0, 0, 0);

    put(g_ran ? "linux thread (clone CLONE_VM, shared address space) ran: OK\n"
              : "linux thread did NOT run: FAIL\n");
    sc(SYS_exit_group, g_ran ? 0 : 1, 0, 0, 0, 0);
    for (;;) { }
}
