/* oom.h -- Out-of-memory killer.
 *
 * Phase 1 Depth Pass: Virtual Memory Hardening.
 *
 * When the PMM cannot satisfy an allocation, the OOM killer selects
 * the "worst offender" process and terminates it to reclaim memory.
 * Protected processes (pid 0/idle, pid 1/init, login) are never killed.
 */

#ifndef TOBYOS_OOM_H
#define TOBYOS_OOM_H

#include <tobyos/types.h>

struct proc;

/* Initialize OOM subsystem. Call once during boot. */
void oom_init(void);

/* Check memory pressure and kill a process if needed.
 * Called when pmm_alloc_page() fails. */
void oom_check(void);

/* Compute an OOM score for a process.
 * Higher score = more likely to be killed.
 * Score factors: resident pages, non-root penalty, essential bonus. */
int oom_score(struct proc *p);

/* Terminate a process and free all its pages.
 * Sends SIGKILL-equivalent, frees VMAs and page tables. */
void oom_kill(struct proc *p);

/* Mark a PID as OOM-protected (never killed). */
void oom_protect(int pid);

/* Query whether a process is OOM-protected. */
bool oom_is_protected(int pid);

/* Return the PID of the last process killed by OOM, or -1 if none. */
int oom_last_killed(void);

#endif /* TOBYOS_OOM_H */
