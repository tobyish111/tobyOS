/* signal.h -- POSIX signal subsystem (Phase 1 M1.3).
 *
 * Extended from the minimal milestone 8 version to support:
 *   - Full set of POSIX signals (SIGKILL, SIGCHLD, SIGSEGV, etc.)
 *   - User-installed signal handlers via sigaction()
 *   - Signal masks (sigprocmask)
 *   - Proper signal delivery via user-space trampoline
 *   - SA_RESTART semantics for interrupted syscalls
 */

#ifndef TOBYOS_SIGNAL_H
#define TOBYOS_SIGNAL_H

#include <tobyos/types.h>

/* ---- Signal numbers (POSIX-compatible) ---- */

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGIO    29
#define SIGSYS   31

#define SIG_MAX  32
#define SIGMASK(s) ((uint32_t)1u << (s))

/* ---- Signal action flags ----
 *
 * VALUES ARE ABI: userland passes these through sys_sigaction's raw
 * sa_flags, so the kernel constants MUST match libtoby's <signal.h> (which
 * uses the Linux values, easing ports). The original kernel-private values
 * (0x01..0x10) silently collided -- e.g. libtoby's SA_NOCLDSTOP (0x1) was
 * the kernel's SA_RESTART -- the same class of footgun as the GUI_EV_*
 * event-type mismatch. Keep both headers in lockstep. */

#define SA_NOCLDSTOP 0x00000001  /* don't notify on child stop */
#define SA_SIGINFO   0x00000004  /* use sa_sigaction instead of sa_handler */
#define SA_RESTART   0x10000000  /* restart interrupted syscall */
#define SA_NODEFER   0x40000000  /* don't block signal during handler */
#define SA_RESETHAND 0x80000000  /* reset to SIG_DFL after delivery */

/* Special handler values */
#define SIG_DFL  ((void (*)(int))0)
#define SIG_IGN  ((void (*)(int))1)

/* ---- sigaction structure ---- */

typedef uint32_t sigset_t;

struct sigaction {
    void     (*sa_handler)(int);
    sigset_t   sa_mask;       /* signals to block during handler */
    int        sa_flags;
};

/* ---- Per-process signal state (stored in proc struct) ---- */

struct signal_state {
    struct sigaction actions[SIG_MAX];  /* per-signal disposition */
    sigset_t         mask;             /* blocked signals */
    sigset_t         pending;          /* pending signals */
    uint64_t         restorer;         /* sigreturn trampoline address */
};

/* errno-ish return value for interrupted blocking primitives */
#define EINTR_RET ((long)-4)

/* Mirror of the register block syscall_entry.S leaves on the per-process
 * kernel syscall stack. Listed in ASCENDING address order, which is the
 * REVERSE of the push order. The base address of this block is
 * `proc->kstack_top - sizeof(struct syscall_regs)` because the scheduler
 * keeps the per-CPU syscall_rsp == current kstack_top and syscall_entry
 * pushes downward from there. Shared by signal delivery (frame rewrite)
 * and fork (the child's resume frame is a copy of the parent's).
 *
 * THIS MUST STAY IN SYNC WITH THE PUSH SEQUENCE IN syscall_entry.S. */
struct syscall_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx;   /* callee-saved          */
    uint64_t r9, r8, r10, rdx, rsi, rdi;     /* user syscall arg regs */
    uint64_t r11;        /* user RFLAGS (SYSRETQ reloads from here)   */
    uint64_t rcx;        /* user RIP    (SYSRETQ reloads from here)   */
    uint64_t pad;        /* 16-byte alignment pad                     */
    uint64_t user_rsp;   /* saved user RSP                            */
};

struct proc;

/* Initialise signal state. */
void signal_init(void);

/* Initialize per-process signal state to defaults */
void signal_init_proc(struct signal_state *ss);

/* Foreground tracking */
int  signal_get_foreground(void);
void signal_set_foreground(int pid);

/* Send a signal. Safe from IRQ context. */
void signal_send(struct proc *p, int sig);
void signal_send_to_pid(int pid, int sig);
void signal_send_to_foreground(int sig);

/* Check and deliver pending signals from an IRQ (PIT) context, where no
 * saved syscall trapframe exists. Handles fatal/default dispositions;
 * caught (user-handler) signals are left pending for the next syscall
 * return to deliver. */
void signal_deliver_if_pending(void);

/* Check and deliver pending signals from the SYSCALL return path. `rv` is
 * the syscall's return value and `num` its number (for SA_RESTART: an
 * EINTR'd syscall whose handler asks for restart is re-executed by rewinding
 * the saved user RIP to the `syscall` instruction with RAX = num). For a
 * signal with a user handler this pushes a signal frame onto the user stack
 * and rewrites the saved trapframe so the SYSRETQ lands in the handler;
 * sys_sigreturn later restores the context. */
void signal_deliver_syscall(long rv, long num);

/* Quick non-destructive query */
bool signal_pending_self(void);

/* ---- Syscall implementations ---- */

/* sigaction: install/query signal handler. act/oldact point at the user
 * `struct sigaction` (libtoby ABI: 64-bit sa_mask), marshalled internally. */
int sys_sigaction(int sig, const void *act, void *oldact);

/* sigprocmask: modify signal mask. set/oldset point at a user 64-bit
 * sigset_t. */
int sys_sigprocmask(int how, const void *set, void *oldset);

/* sigreturn: restore the context saved when a handler was entered. Returns
 * the value to leave in the user's RAX (the interrupted syscall's result). */
long sys_sigreturn(void);

/* sigrestorer: register the user-space sigreturn trampoline address. */
void sys_sigrestorer(uint64_t addr);

/* kill: send signal to process */
int sys_kill(int pid, int sig);

/* sigprocmask `how` values */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#endif /* TOBYOS_SIGNAL_H */
