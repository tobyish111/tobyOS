/* cgroup.h -- cgroup v2 (Phase 3 slice 15): core hierarchy + pids + cpu.
 *
 * The plan picked these two controllers because they map onto machinery that
 * already exists: `pids` is a counter, and `cpu.weight` maps onto the 5-class
 * priority scheduler in sched.c. Nothing here invents a new scheduling model.
 *
 * ---------------------------------------------------------------------------
 * WHAT "cgroup v2" MEANS HERE, STATED UP FRONT
 *
 * v2 (unified) rather than v1: ONE hierarchy, controllers enabled per-subtree,
 * and a process belongs to exactly one cgroup. That is both the modern interface
 * and much the simpler one -- v1's per-controller hierarchies would mean N trees
 * and N membership pointers per proc for no benefit to anything that runs here.
 *
 * The interface is a filesystem, mounted at /sys/fs/cgroup: directories ARE
 * cgroups (mkdir creates one, rmdir removes it) and the control files are read
 * and written. There is no cgroup syscall in Linux either -- the filesystem IS
 * the ABI, which is why slice 11's procfs write path was a prerequisite in
 * spirit: this needed the same thing for a new filesystem.
 * ---------------------------------------------------------------------------
 */
#ifndef TOBYOS_CGROUP_H
#define TOBYOS_CGROUP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct proc;

#define CGROUP_MAX        32    /* cgroups in the hierarchy (root is index 0) */
#define CGROUP_NAME_MAX   32
#define CGROUP_ROOT       0

/* cpu.weight range and default, straight from the v2 ABI. */
#define CGROUP_WEIGHT_MIN     1u
#define CGROUP_WEIGHT_DEFAULT 100u
#define CGROUP_WEIGHT_MAX     10000u

/* pids.max == -1 means the literal string "max" (no limit). */
#define CGROUP_PIDS_UNLIMITED (-1)

void cgroup_init(void);
/* Mount cgroup2 at /sys/fs/cgroup. Called after sysfs_init (it nests under
 * /sys, which the longest-prefix mount table handles natively). */
void cgroup_mount(void);

/* ---- membership -------------------------------------------------------- */
/* A process's cgroup index; 0 (root) for anything never assigned. */
int  cgroup_of(struct proc *p);
/* Move `p` into cgroup `cg`. Returns 0 or a negative ABI errno. */
long cgroup_attach(struct proc *p, int cg);

/* ---- the pids controller ----------------------------------------------
 * Called from the fork paths BEFORE a slot is committed. Returns 0 to allow,
 * or -EAGAIN when the child would exceed pids.max in its cgroup or any
 * ancestor -- the limit is hierarchical, so a child cgroup cannot escape its
 * parent's cap by raising its own. */
long cgroup_can_fork(struct proc *parent);

/* ---- the cpu controller ----------------------------------------------- */
/* The scheduler priority a cgroup's weight corresponds to. */
int  cgroup_prio_for(struct proc *p);
/* Accumulate CPU time against a cgroup (cpu.stat). */
void cgroup_account_cpu(struct proc *p, uint64_t ns);

/* Number of processes in `cg` and all its descendants. */
int  cgroup_nr_procs(int cg);

/* ---- the memory controller (slice 16) ----------------------------------
 *
 * CHARGED: anonymous user address-space pages (brk, demand fault, CoW, exec).
 * NOT CHARGED (Linux charges all of these; we do not, and say so): page cache,
 * memfd/shm backing pages, kernel allocations including page tables.
 *
 * Charging keeps a running counter instead of summing on read, because
 * memory.max is enforced at PAGE-FAULT time. The counter is kept honest by an
 * invariant the harness checks: mem_pages == sum of member p->user_pages.
 *
 * THE CHARGE FOLLOWS THE ADDRESS SPACE, NOT THE TASK. A user page belongs to an
 * mm, so mm_user_page_alloc resolves the charge to the mm OWNER (`proc_mm_pid`, the
 * same key mmap.c uses for the VMA table). Consequences, all of them wanted:
 *
 *   - Threads carry no charge of their own. Charging the faulting task instead
 *     under-reports every threaded program: a thread faults 100 pages, exits, and
 *     proc_reap releases them while all 100 are still mapped for the leader.
 *     Chromium runs ~30 threads per process, so this is not a corner case.
 *   - `sys_fork` FULL-copies the user half, so the child owns its pages and is
 *     bulk-charged its parent's count -- exact, not an approximation.
 *   - `sys_fork_share`/vfork SHARES the space, so the child is charged nothing;
 *     the parent already holds it. Charging both would double-count the whole
 *     address space on every vfork, and busybox vforks constantly.
 *   - Breaking CoW is charge-neutral (see cow_copy_page). */
#define CGROUP_MEM_UNLIMITED (-1)

/* THE FUNNEL. Every user page enters and leaves the system through these two,
 * which is what makes the accounting auditable by grep rather than by faith.
 * mm_user_page_alloc returns 0 on refusal OR on real OOM -- callers already
 * handle a 0 from pmm_alloc_page, so existing failure paths work unchanged. */
uint64_t mm_user_page_alloc(struct proc *p);
void     mm_user_page_free(struct proc *p, uint64_t phys);
/* Bulk release at teardown, using the process's own counter as source of truth. */
void     mm_user_pages_release_all(struct proc *p);

bool     cgroup_mem_charge(struct proc *p, uint64_t pages);
void     cgroup_mem_uncharge(struct proc *p, uint64_t pages);
void     cgroup_mem_migrate(struct proc *p, int from, int to);
void     cgroup_mem_note_oom_kill(struct proc *p);
/* Subtree total (memory.current) vs this cgroup's own charge. */
uint64_t cgroup_mem_pages(int cg);
uint64_t cgroup_mem_pages_own(int cg);
int64_t  cgroup_mem_max_pages(int cg);
uint64_t cgroup_mem_fail(int cg);

/* ---- the io controller (slice 16) --------------------------------------
 * Accounting only. There is deliberately NO io.max: throttling needs somewhere
 * to sleep inside a synchronous block path that runs under filesystem locks, and
 * a knob that accepts a limit and throttles nothing is the exact failure this arc
 * already caught in VFS_MNT_RDONLY and PR_SET_NO_NEW_PRIVS.
 *
 * The counters are UNLOCKED. Two CPUs completing block I/O for the same cgroup can
 * lose an update, so io.stat is accurate to within a few requests under concurrency
 * rather than exact. Stated because it is a real property: taking a lock on every
 * block completion would cost more than the number is worth, and no decision is
 * made from these figures -- unlike memory.current, which gates allocation and is
 * therefore maintained under the BKL like the rest of the charge path. */
void cgroup_io_charge(struct proc *p, bool write, uint64_t bytes);
void cgroup_io_stat(int cg, uint64_t *rb, uint64_t *wb, uint64_t *ri, uint64_t *wi);

#endif /* TOBYOS_CGROUP_H */
