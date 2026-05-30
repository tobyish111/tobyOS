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

/* ---- Signal action flags ---- */

#define SA_RESTART   0x01   /* restart interrupted syscall */
#define SA_NOCLDSTOP 0x02   /* don't notify on child stop */
#define SA_SIGINFO   0x04   /* use sa_sigaction instead of sa_handler */
#define SA_RESETHAND 0x08   /* reset to SIG_DFL after delivery */
#define SA_NODEFER   0x10   /* don't block signal during handler */

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
 * the syscall's return value. For a signal with a user handler this pushes a
 * signal frame onto the user stack and rewrites the saved trapframe so the
 * SYSRETQ lands in the handler; sys_sigreturn later restores the context. */
void signal_deliver_syscall(long rv);

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
