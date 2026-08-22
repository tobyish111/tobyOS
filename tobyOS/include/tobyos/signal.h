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

/* 2026-08-22: SIG_MAX 32 -> 64. The realtime range (32..63) exists so that
 * glibc NPTL works AT ALL on the Linux personality: its internal SIGCANCEL
 * is 32 and SIGSETXID is 33, and with SIG_MAX at 32 both rt_sigaction and
 * tgkill answered EINVAL -- so pthread_cancel and setuid() from any
 * threaded process were structurally impossible, quietly. tobyOS's mask
 * convention keeps bit s for signal s (bit 0 unused), so 64 bits carry
 * signals 1..63; Linux's signal 64 (SIGRTMAX) is the one casualty, refused
 * with EINVAL rather than silently aliased. */
#define SIG_MAX  64
#define SIGMASK(s) ((uint64_t)1ull << (s))

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

typedef uint64_t sigset_t;   /* 64 signals since 2026-08-22; matches
                              * libtoby's unsigned long sigset_t for real
                              * now (the old u32 only matched by accident
                              * of ucontext padding). */

struct sigaction {
    void     (*sa_handler)(int);
    sigset_t   sa_mask;       /* signals to block during handler */
    int        sa_flags;
};

/* ---- siginfo_t + ucontext_t (SA_SIGINFO) ----
 *
 * tobyOS's own ABI layout (NOT Linux-binary-compatible) -- it MUST match
 * libtoby's <signal.h>. A 3-arg sa_sigaction handler receives
 *   void handler(int signo, siginfo_t *info, void *ucontext);
 * with `info` and `ucontext` pointing at structures the kernel builds on
 * the user stack alongside the sigreturn frame. */

/* si_code values */
#define SI_USER    0       /* kill()/raise()/tty-generated */
#define SI_KERNEL  0x80    /* kernel-synthesized */
#define SI_TKILL  (-6)     /* tkill/tgkill-generated (Linux) */

/* Synchronous-fault si_code values (Linux x86-64). Reported in the
 * siginfo_t a SIGSEGV/SIGFPE/SIGILL handler receives. */
#define SEGV_MAPERR 1      /* address not mapped to object */
#define SEGV_ACCERR 2      /* invalid permissions for mapped object */
#define FPE_INTDIV  1      /* integer divide by zero */
#define FPE_FLTDIV  3      /* floating-point divide by zero */
#define ILL_ILLOPC  1      /* illegal opcode */
#define TRAP_BRKPT  1      /* process breakpoint (int3 / #BP)          */
#define TRAP_TRACE  2      /* process trace trap (single-step / #DB)   */

typedef struct {
    int          si_signo;   /* signal number                       */
    int          si_code;    /* SI_USER / SI_KERNEL                  */
    int          si_pid;     /* sending pid (0 == kernel/tty)        */
    unsigned int si_uid;     /* sending uid                          */
    void        *si_addr;    /* fault address (unused for now, 0)    */
    int          si_status;  /* exit/signal status (unused for now)  */
    int          _pad;
} siginfo_t;

/* Minimal machine context. Register slots are populated from the saved
 * syscall trapframe; uc_mcontext is enough for a handler to read the
 * interrupted RIP/RSP/RFLAGS. Layout fixed -- keep libtoby in sync. */
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rsp, rflags;
} mcontext_t;

typedef struct ucontext {
    uint64_t          uc_flags;
    struct ucontext  *uc_link;
    sigset_t          uc_sigmask;   /* was u32 mask + u32 pad; an 8-byte
                                     * sigset_t occupies the same bytes, so
                                     * the layout libtoby reads is unchanged */
    mcontext_t        uc_mcontext;
} ucontext_t;

/* ---- Per-process signal state (stored in proc struct) ---- */

struct signal_state {
    struct sigaction actions[SIG_MAX];  /* per-signal disposition */
    sigset_t         mask;             /* blocked signals */
    sigset_t         pending;          /* pending signals */
    uint64_t         restorer;         /* sigreturn trampoline address */
    /* SA_SIGINFO: sender identity recorded per pending signal, set at
     * send time and read at delivery to populate siginfo_t. */
    int              si_pid[SIG_MAX];
    uint32_t         si_uid[SIG_MAX];
    /* si_code per pending signal (SI_USER=0 / SI_TKILL=-6). NOT cosmetic:
     * glibc's NPTL handlers for SIGCANCEL/SIGSETXID begin with
     *   if (si->si_pid != getpid() || si->si_code != SI_TKILL) return;
     * -- an anti-spoof check -- so a tgkill-generated signal delivered
     * with SI_USER is silently DISCARDED by the very handler it exists
     * for. Found live: the threaded-setuid broadcast's handler ran and
     * did nothing, forever (2026-08-22). */
    int8_t           si_code[SIG_MAX];
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
struct regs;   /* CPU exception trapframe (isr.h) */

/* Initialise signal state. */
void signal_init(void);

/* Initialize per-process signal state to defaults */
void signal_init_proc(struct signal_state *ss);

/* The HANDLER TABLE is a thread-group property (POSIX sighand): a handler
 * installed by any thread is visible to all. Threads reach the LEADER's
 * actions[] through this accessor -- the same indirection proc_fds() uses
 * for the fd table, and for the same reason: sys_clone_thread's whole-PCB
 * memcpy gives each thread a SNAPSHOT copy, and before this accessor
 * existed (2026-08-22) a handler installed after pthread_create was
 * invisible to every sibling. Masks and pending sets stay PER-THREAD --
 * that half of the split is correct POSIX, not a shortcut. */
struct sigaction *sig_actions_of(struct proc *p);

/* Foreground tracking */
int  signal_get_foreground(void);
void signal_set_foreground(int pid);

/* Send a signal. Safe from IRQ context. */
void signal_send(struct proc *p, int sig);
void signal_send_to_pid(int pid, int sig);
/* Same, but reports a pid that does not exist: 0 on delivery, -1 if there
 * is no such process. The shell's `kill` needs the answer for its status. */
int  signal_send_to_pid_checked(int pid, int sig);
void signal_send_to_foreground(int sig);
/* Group send for the tty/pty line discipline (^C/^Z to the foreground
 * group). Kernel-trusted; no permission model. */
void signal_send_to_pgrp(int pgrp, int sig);

/* Cooperative stop point for long-held syscall loops: applies one pending
 * default-disposition stop (parks until SIGCONT) and returns true; false
 * when the pending stop is caught (caller should EINTR instead). */
bool signal_take_pending_stop(void);

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

/* Deliver a SYNCHRONOUS CPU-fault signal (SIGSEGV/SIGFPE/SIGILL/...) to the
 * faulting user process from the exception path. `r` is the CPU exception
 * trapframe (rewritten in place so the trailing iretq enters the handler),
 * `si_code` is the fault sub-code (e.g. SEGV_MAPERR), and `fault_addr` is the
 * offending address (CR2 for #PF; 0 otherwise) reported as siginfo si_addr.
 * Returns true if a user handler was set up (caller should iretq); false if
 * the process has no catchable handler (caller takes the fatal path). */
bool signal_deliver_fault(struct regs *r, int sig, int si_code,
                          uint64_t fault_addr);

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

/* kill: send signal to process (process-directed: may deliver to any
 * group member with the signal unblocked) */
int sys_kill(int pid, int sig);

/* tkill/tgkill: thread-directed, never retargeted; tgid > 0 is verified
 * against the target's group, tgid <= 0 skips the check (tkill). */
int sys_tgkill(int tgid, int tid, int sig);

/* sigprocmask `how` values */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#endif /* TOBYOS_SIGNAL_H */
