#include <signal.h>
#include <unistd.h>
#include "libtoby_internal.h"

/* sigreturn trampoline.
 *
 * When the kernel delivers a caught signal it pushes the address of this
 * stub as the handler's return address. The handler `ret`s here; we then
 * issue SYS_SIGRETURN, which the kernel handles by restoring the interrupted
 * context (so it never returns to us). The `ud2` is a guard: if SIGRETURN
 * ever does return, fault loudly instead of running off into the stack. */
__asm__(
    ".text\n"
    ".globl __toby_sigreturn\n"
    ".type __toby_sigreturn, @function\n"
    "__toby_sigreturn:\n"
    "    movl $119, %eax\n"          /* ABI_SYS_SIGRETURN */
    "    syscall\n"
    "    ud2\n"
    ".size __toby_sigreturn, .-__toby_sigreturn\n"
);
extern void __toby_sigreturn(void);

/* Register the trampoline with the kernel exactly once per process image.
 * Re-armed automatically after execve (fresh image -> g_restorer_armed==0)
 * and inherited as already-armed across fork (same address space). */
static int g_restorer_armed;

static void arm_restorer(void) {
    if (g_restorer_armed) return;
    toby_sc1(ABI_SYS_SIGRESTORER, (long)(uintptr_t)__toby_sigreturn);
    g_restorer_armed = 1;
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    arm_restorer();
    return (int)__toby_check(
        toby_sc3(ABI_SYS_SIGACTION, (long)signum, (long)(uintptr_t)act, (long)(uintptr_t)oldact));
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    return (int)__toby_check(
        toby_sc3(ABI_SYS_SIGPROCMASK, (long)how, (long)(uintptr_t)set, (long)(uintptr_t)oldset));
}

int kill(pid_t pid, int sig) {
    return (int)__toby_check(toby_sc2(ABI_SYS_KILL, (long)pid, (long)sig));
}

int raise(int sig) {
    return kill(getpid(), sig);
}

sighandler_t signal(int signum, sighandler_t handler) {
    struct sigaction sa = { .sa_handler = handler, .sa_mask = 0, .sa_flags = SA_RESTART };
    struct sigaction old = {0};
    if (sigaction(signum, &sa, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}

/* signal() WITHOUT SA_RESTART: a blocked syscall returns EINTR rather than
 * resuming transparently.
 *
 * 2026-08-25. signal()'s SA_RESTART is right for almost every handler and
 * exactly wrong for an interactive reader. /bin/tsh armed SIGINT with it,
 * so ^C at the prompt ran the handler and then RESUMED the blocked read():
 * the half-typed line stayed, no newline appeared, no fresh prompt was
 * drawn. The key did nothing observable, which is indistinguishable from
 * ^C not being wired at all. Anything that must REACT to a signal mid-read
 * -- a shell, a pager, an editor -- wants this one. */
sighandler_t signal_norestart(int signum, sighandler_t handler) {
    struct sigaction sa = { .sa_handler = handler, .sa_mask = 0, .sa_flags = 0 };
    struct sigaction old = {0};
    if (sigaction(signum, &sa, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}

int sigemptyset(sigset_t *set)  { if (set) *set = 0; return 0; }
int sigfillset(sigset_t *set)   { if (set) *set = ~0UL; return 0; }
int sigaddset(sigset_t *set, int n) { if (set && n > 0 && n < 64) { *set |= (1UL << n); return 0; } return -1; }
int sigdelset(sigset_t *set, int n) { if (set && n > 0 && n < 64) { *set &= ~(1UL << n); return 0; } return -1; }
int sigismember(const sigset_t *set, int n) { if (set && n > 0 && n < 64) return (*set >> n) & 1; return -1; }
