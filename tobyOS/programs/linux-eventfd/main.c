/* programs/linux-eventfd/main.c
 *
 * Targeted unit test for the remaining DOM blocker (ledger: the renderer's IO
 * thread parks in epoll_wait on an eventfd -- efd 14 -- that is never woken).
 * Question in isolation: does epoll_wait on an eventfd wake when ANOTHER THREAD
 * writes that eventfd? (Same address space, the renderer's case.)
 *
 *   thread A: epoll_wait(epfd with the eventfd registered EPOLLIN, timeout 10s)
 *   main:     sleep briefly, then write 1 to the eventfd
 *   A must return promptly with the eventfd readable (NOT wait out the 10s).
 *
 * exit_group():
 *   3 = PASS  (epoll_wait woke from the cross-thread eventfd write)
 *   1 = FAIL  (epoll_wait did NOT wake -> tobyOS eventfd/epoll wakeup bug = DOM)
 *   2 = ERROR (setup/clone problem)
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
#define SC1(n,a)         lx6(n,(i64)(a),0,0,0,0,0)
#define SC2(n,a,b)       lx6(n,(i64)(a),(i64)(b),0,0,0,0)
#define SC3(n,a,b,c)     lx6(n,(i64)(a),(i64)(b),(i64)(c),0,0,0)
#define SC4(n,a,b,c,d)   lx6(n,(i64)(a),(i64)(b),(i64)(c),(i64)(d),0,0)

#define SYS_write        1
#define SYS_read         0
#define SYS_exit         60
#define SYS_exit_group   231
#define SYS_nanosleep    35
#define SYS_clock_gettime 228
#define SYS_eventfd2     290
#define SYS_epoll_create1 291
#define SYS_epoll_ctl    233
#define SYS_epoll_wait   232
#define SYS_clone        56

#define EPOLL_CTL_ADD 1
#define EPOLLIN       0x001

#define CLONE_VM      0x00000100
#define CLONE_FS      0x00000200
#define CLONE_FILES   0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD  0x00010000

struct timespec { i64 sec; i64 nsec; };
struct epoll_event { unsigned int events; unsigned long data; } __attribute__((packed));

static void put(const char *s){ long n=0; while(s[n])n++; SC3(SYS_write,1,s,n); }
static void msleep(long ms){ struct timespec t={ms/1000,(ms%1000)*1000000L}; SC2(SYS_nanosleep,&t,0); }
static u64 now_ns(void){ struct timespec t={0,0}; SC2(SYS_clock_gettime,1,&t); return (u64)t.sec*1000000000ull+(u64)t.nsec; }

static volatile int  g_efd  = -1;
static volatile int  g_epfd = -1;
static volatile int  g_a_ready = 0;
static volatile int  g_a_done  = 0;
static volatile long g_a_ret   = -123;
static volatile unsigned g_a_events = 0;

/* Thread A: block in epoll_wait for up to 10 s. eventfd starts at 0, so it must
 * NOT be readable until main writes it. */
static void thread_a(void) {
    struct epoll_event ev[4];
    g_a_ready = 1;
    g_a_ret = SC4(SYS_epoll_wait, g_epfd, ev, 4, 10000 /* ms */);
    if (g_a_ret > 0) g_a_events = ev[0].events;
    g_a_done = 1;
    SC1(SYS_exit, 0);
}

static unsigned char g_stack[16384] __attribute__((aligned(16)));

extern void _start(void);
__asm__(".globl _start\n_start:\n  call main_c\n  mov %rax,%rdi\n  mov $231,%rax\n  syscall\n");

/* clone trampoline. fn saved in r12 (CALLEE-SAVED: survives `syscall`, which
 * clobbers rcx AND r11 -- saving fn in r11 makes the child jump to garbage). */
extern long clone_thread(void (*fn)(void), void *stack_top, int flags);
__asm__(
  ".globl clone_thread\nclone_thread:\n"
  "  push %r12\n  mov %rdi,%r12\n  mov %edx,%edi\n  and $-16,%rsi\n"
  "  xor %edx,%edx\n  xor %r10,%r10\n  xor %r8,%r8\n  mov $56,%rax\n  syscall\n"
  "  test %rax,%rax\n  jnz 1f\n  call *%r12\n  mov $60,%rax\n  xor %edi,%edi\n  syscall\n"
  "1:\n  pop %r12\n  ret\n");

int main_c(void);
int main_c(void) {
    put("[efdtest] start\n");

    g_efd = (int)lx6(SYS_eventfd2, 0, 0, 0, 0, 0, 0);   /* initval 0, flags 0 */
    if (g_efd < 0) { put("[efdtest] eventfd2 FAILED\n"); return 2; }
    g_epfd = (int)SC1(SYS_epoll_create1, 0);
    if (g_epfd < 0) { put("[efdtest] epoll_create1 FAILED\n"); return 2; }

    struct epoll_event add = { EPOLLIN, 0x1234 };
    if (SC4(SYS_epoll_ctl, g_epfd, EPOLL_CTL_ADD, g_efd, &add) < 0) {
        put("[efdtest] epoll_ctl ADD FAILED\n"); return 2;
    }

    long tid = clone_thread(thread_a, g_stack + sizeof(g_stack),
                            CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD);
    if (tid < 0) { put("[efdtest] clone FAILED\n"); return 2; }

    for (int i = 0; i < 200 && !g_a_ready; i++) msleep(5);
    msleep(200);   /* let A actually enter epoll_wait */
    if (g_a_done) { put("[efdtest] A returned before any write?!\n"); return 2; }

    /* THE TEST: write the eventfd from THIS thread; A must wake. */
    u64 t0 = now_ns();
    unsigned long one = 1;
    long w = SC3(SYS_write, g_efd, &one, 8);
    (void)w;

    for (int i = 0; i < 400 && !g_a_done; i++) msleep(5);  /* up to ~2 s grace */
    u64 dt_ms = (now_ns() - t0) / 1000000ull;

    if (g_a_done && g_a_ret >= 1 && (g_a_events & EPOLLIN) && dt_ms < 3000) {
        put("[efdtest] PASS: epoll_wait woke from cross-thread eventfd write\n");
        return 3;
    }
    if (!g_a_done) {
        put("[efdtest] FAIL: epoll_wait NOT woken by eventfd write (lost wakeup)\n");
        return 1;
    }
    put("[efdtest] FAIL: epoll_wait returned but not cleanly (no EPOLLIN)\n");
    return 1;
}
