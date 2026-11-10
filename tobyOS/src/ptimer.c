/* ptimer.c -- POSIX interval timers: the timer_create(2) family
 * (2026-08-22). glibc's timer_* wrappers had no syscalls to land on --
 * timer_create fell through the dispatcher, so librt users (and every
 * profiler built on ITIMER-style periodic signals beyond the one alarm)
 * simply could not run.
 *
 * Firing rides signal_tick_alarms' existing ~10 ms perf_now_ns sweep --
 * the same cadence and clock the alarm(2) machinery already trusts, so a
 * timer cannot disagree with an alarm about when "now" is. SIGEV_SIGNAL
 * and SIGEV_NONE are supported; SIGEV_THREAD is glibc-internal (it builds
 * it on SIGEV_SIGNAL + a helper thread) so nothing extra is owed here.
 * All state is BKL-serialised (callers are syscall arms + the sweep). */

#include <tobyos/proc.h>
#include <tobyos/signal.h>
#include <tobyos/perf.h>
#include <tobyos/klibc.h>
#include <tobyos/abi/abi.h>

#define PTIMER_MAX 32

struct ptimer {
    bool     used;
    int      owner_pid;              /* tgid: timers are process-wide */
    int      signo;                  /* 0 = SIGEV_NONE */
    uint64_t next_ns;                /* absolute; 0 = disarmed */
    uint64_t interval_ns;            /* 0 = one-shot */
    int      overrun;                /* fires missed since last delivery */
};

static struct ptimer g_ptimers[PTIMER_MAX];

static struct ptimer *pt_of(int id, int owner) {
    if (id < 0 || id >= PTIMER_MAX) return 0;
    struct ptimer *t = &g_ptimers[id];
    if (!t->used || t->owner_pid != owner) return 0;
    return t;
}

long ptimer_create(int owner_pid, int signo) {
    for (int i = 0; i < PTIMER_MAX; i++) {
        if (!g_ptimers[i].used) {
            g_ptimers[i] = (struct ptimer){ true, owner_pid, signo, 0, 0, 0 };
            return i;
        }
    }
    return -ABI_ENOMEM;                /* EAGAIN on Linux; nearest honest */
}

long ptimer_settime(int id, int owner, uint64_t value_ns, uint64_t interval_ns,
                    uint64_t *old_rem_ns, uint64_t *old_interval_ns) {
    struct ptimer *t = pt_of(id, owner);
    if (!t) return -ABI_EINVAL;
    uint64_t now = perf_now_ns();
    if (old_rem_ns)
        *old_rem_ns = (t->next_ns && t->next_ns > now) ? t->next_ns - now : 0;
    if (old_interval_ns) *old_interval_ns = t->interval_ns;
    t->interval_ns = interval_ns;
    t->next_ns     = value_ns ? now + value_ns : 0;   /* 0 disarms */
    t->overrun     = 0;
    return 0;
}

long ptimer_gettime(int id, int owner, uint64_t *rem_ns, uint64_t *interval_ns) {
    struct ptimer *t = pt_of(id, owner);
    if (!t) return -ABI_EINVAL;
    uint64_t now = perf_now_ns();
    *rem_ns = (t->next_ns && t->next_ns > now) ? t->next_ns - now : 0;
    *interval_ns = t->interval_ns;
    return 0;
}

long ptimer_overrun(int id, int owner) {
    struct ptimer *t = pt_of(id, owner);
    if (!t) return -ABI_EINVAL;
    return t->overrun;
}

long ptimer_delete(int id, int owner) {
    struct ptimer *t = pt_of(id, owner);
    if (!t) return -ABI_EINVAL;
    t->used = false;
    return 0;
}

void ptimer_release_proc(int pid) {
    for (int i = 0; i < PTIMER_MAX; i++)
        if (g_ptimers[i].used && g_ptimers[i].owner_pid == pid)
            g_ptimers[i].used = false;
}

/* Driven from signal_tick_alarms (same sweep, same clock). A periodic
 * timer that fell several intervals behind fires ONCE and records the
 * misses as overrun -- Linux semantics; a burst of catch-up signals is
 * what naive implementations do and profilers explicitly do not want. */
void ptimer_tick(void) {
    uint64_t now = perf_now_ns();
    for (int i = 0; i < PTIMER_MAX; i++) {
        struct ptimer *t = &g_ptimers[i];
        if (!t->used || !t->next_ns || now < t->next_ns) continue;
        if (t->interval_ns) {
            uint64_t behind = now - t->next_ns;
            uint64_t missed = behind / t->interval_ns;
            t->overrun = (int)missed;
            t->next_ns += (missed + 1) * t->interval_ns;
        } else {
            t->next_ns = 0;            /* one-shot */
        }
        if (t->signo > 0) {
            struct proc *p = proc_lookup(t->owner_pid);
            if (p) signal_send(p, t->signo);
            else   t->used = false;    /* owner died without cleanup */
        }
    }
}
