/* oom.c -- Out-of-memory killer.
 *
 * Phase 1 Depth Pass: Virtual Memory Hardening.
 *
 * When physical memory is exhausted, the OOM killer picks the process
 * with the highest "badness" score and terminates it. Protected
 * processes (idle, init, login) are never victims.
 *
 * Scoring: (resident_pages * 1000) / total_pages, adjusted:
 *   +300 for non-root processes (uid != 0)
 *   -500 for essential services (session_id == 0 and name starts with "svc_")
 */

#include <tobyos/oom.h>
#include <tobyos/proc.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/signal.h>

#define OOM_PROTECTED_MAX 8

static int g_protected_pids[OOM_PROTECTED_MAX];
static int g_protected_count;
static int g_last_killed_pid = -1;
static bool g_oom_initialized;

extern struct proc g_proc[];

void oom_init(void) {
    g_protected_count = 0;
    g_last_killed_pid = -1;

    /* Protect essential system processes by default */
    oom_protect(0);  /* idle / kernel */
    oom_protect(1);  /* init */

    g_oom_initialized = true;
    kprintf("oom: initialized (protected: pid 0, pid 1)\n");
}

void oom_protect(int pid) {
    if (g_protected_count >= OOM_PROTECTED_MAX) return;
    for (int i = 0; i < g_protected_count; i++) {
        if (g_protected_pids[i] == pid) return;
    }
    g_protected_pids[g_protected_count++] = pid;
}

bool oom_is_protected(int pid) {
    for (int i = 0; i < g_protected_count; i++) {
        if (g_protected_pids[i] == pid) return true;
    }
    /* Also protect "login" by name */
    struct proc *p = proc_lookup(pid);
    if (p && strcmp(p->name, "login") == 0) return true;
    return false;
}

int oom_score(struct proc *p) {
    if (!p) return 0;
    if (p->state == PROC_UNUSED || p->state == PROC_TERMINATED) return 0;

    size_t total = pmm_total_pages();
    if (total == 0) return 0;

    /* Base score: proportion of memory used */
    int score = (int)((p->user_pages * 1000) / total);

    /* Non-root penalty */
    if (p->uid != 0) score += 300;

    /* Essential service bonus (lower score) */
    if (p->session_id == 0 && strncmp(p->name, "svc_", 4) == 0)
        score -= 500;

    if (score < 0) score = 0;
    return score;
}

void oom_kill(struct proc *p) {
    if (!p) return;

    kprintf("oom: killing pid %d (%s) -- score %d, %lu pages\n",
            p->pid, p->name, oom_score(p),
            (unsigned long)p->user_pages);

    g_last_killed_pid = p->pid;

    /* Send SIGKILL to the process */
    p->pending_signals |= (1u << 9); /* SIGKILL = signal 9 */

    /* If the process is blocked, wake it so it can die */
    if (p->state == PROC_BLOCKED) {
        p->state = PROC_READY;
        extern void sched_enqueue(struct proc *);
        sched_enqueue(p);
    }
}

void oom_check(void) {
    if (!g_oom_initialized) return;

    size_t free = pmm_free_pages();
    size_t total = pmm_total_pages();

    /* Only trigger if truly out of memory (< 1% free) */
    if (total > 0 && (free * 100 / total) > 1) return;

    kprintf("oom: memory critical (%lu/%lu pages free), selecting victim\n",
            (unsigned long)free, (unsigned long)total);

    struct proc *victim = NULL;
    int highest_score = -1;

    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *p = &g_proc[i];
        if (p->state == PROC_UNUSED || p->state == PROC_TERMINATED)
            continue;
        if (oom_is_protected(p->pid))
            continue;

        int score = oom_score(p);
        if (score > highest_score) {
            highest_score = score;
            victim = p;
        }
    }

    if (victim) {
        oom_kill(victim);
    } else {
        kprintf("oom: no eligible victim found -- system may hang\n");
    }
}

int oom_last_killed(void) {
    return g_last_killed_pid;
}
