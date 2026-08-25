#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <sys/types.h>

#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGSEGV   11
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGUSR1   10
#define SIGUSR2   12

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

/* Linux-compatible values; the kernel's <tobyos/signal.h> mirrors these
 * exactly (sa_flags crosses the syscall ABI raw -- keep in lockstep). */
#define SA_RESTART   0x10000000
#define SA_SIGINFO   0x00000004
#define SA_NOCLDSTOP 0x00000001
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

#define SIGTTIN   21
#define SIGTTOU   22

/* sigprocmask `how` values */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

typedef unsigned long sigset_t;

/* siginfo_t + ucontext_t for SA_SIGINFO 3-arg handlers. MUST match the
 * kernel's <tobyos/signal.h> layout (tobyOS's own ABI, not Linux-binary). */
#define SI_USER    0
#define SI_KERNEL  0x80

typedef struct {
    int          si_signo;
    int          si_code;
    int          si_pid;
    unsigned int si_uid;
    void        *si_addr;
    int          si_status;
    int          _si_pad;
} siginfo_t;

typedef struct {
    unsigned long rax, rbx, rcx, rdx, rsi, rdi, rbp;
    unsigned long r8, r9, r10, r11, r12, r13, r14, r15;
    unsigned long rip, rsp, rflags;
} mcontext_t;

typedef struct ucontext {
    unsigned long     uc_flags;
    struct ucontext  *uc_link;
    sigset_t          uc_sigmask;
    unsigned int      _uc_pad;
    mcontext_t        uc_mcontext;
} ucontext_t;

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
};

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int kill(pid_t pid, int sig);
int raise(int sig);

int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigdelset(sigset_t *set, int signum);
int sigismember(const sigset_t *set, int signum);

typedef void (*sighandler_t)(int);
sighandler_t signal(int signum, sighandler_t handler);
/* signal() WITHOUT SA_RESTART: a blocked syscall returns EINTR instead of
 * resuming. For anything that must REACT to a signal mid-read -- a shell
 * at its prompt, a pager, an editor. plain signal() sets SA_RESTART, which
 * makes ^C during a blocking read do nothing observable at all. */
sighandler_t signal_norestart(int signum, sighandler_t handler);

#endif
