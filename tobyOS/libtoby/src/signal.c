#include <signal.h>
#include <unistd.h>
#include "libtoby_internal.h"

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
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

int sigemptyset(sigset_t *set)  { if (set) *set = 0; return 0; }
int sigfillset(sigset_t *set)   { if (set) *set = ~0UL; return 0; }
int sigaddset(sigset_t *set, int n) { if (set && n > 0 && n < 64) { *set |= (1UL << n); return 0; } return -1; }
int sigdelset(sigset_t *set, int n) { if (set && n > 0 && n < 64) { *set &= ~(1UL << n); return 0; } return -1; }
int sigismember(const sigset_t *set, int n) { if (set && n > 0 && n < 64) return (*set >> n) & 1; return -1; }
