/* cgroup.c -- cgroup v2: the hierarchy, cgroupfs, and the pids + cpu controllers.
 *
 * See cgroup.h for why v2 (unified) and why these two controllers.
 *
 * ---------------------------------------------------------------------------
 * THE TWO THINGS THAT MUST BE REAL, NOT DECORATIVE
 *
 * A cgroup interface is trivially easy to fake: the files are just text, so
 * `pids.max` can be written and read back perfectly while limiting nothing, and
 * `cpu.weight` can round-trip while changing no scheduling decision. Both would
 * pass a naive test. So:
 *
 *   - `pids.max` is enforced in the FORK PATH, hierarchically (a child cgroup
 *     cannot escape an ancestor's cap by raising its own), and the acceptance
 *     test asserts fork actually fails with EAGAIN at the limit and succeeds
 *     again once the limit is raised.
 *   - `cpu.weight` maps onto the real 5-class priority scheduler, and the test
 *     MEASURES CPU time from /proc rather than trusting the file.
 * ---------------------------------------------------------------------------
 */

#include <tobyos/cgroup.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/pmm.h>
#include <tobyos/printk.h>
#include <tobyos/abi/abi.h>

extern struct proc g_proc[PROC_MAX];

struct cgroup {
    bool     in_use;
    char     name[CGROUP_NAME_MAX];
    int      parent;                  /* index; -1 only for the root */
    int32_t  pids_max;                /* CGROUP_PIDS_UNLIMITED == "max" */
    uint32_t cpu_weight;
    uint64_t cpu_usage_ns;

    /* ---- slice 16: the memory controller ----------------------------------
     * `mem_pages` is maintained by charge/uncharge, NOT summed on read, because
     * memory.max has to be enforced at PAGE-FAULT time and walking every proc to
     * the root on each fault is not affordable.
     *
     * The invariant that keeps a counter honest is checkable, and the harness
     * checks it: for every cgroup, mem_pages == sum of member p->user_pages.
     * That is why charging goes through ONE funnel (mm_user_page_alloc). */
    uint64_t mem_pages;               /* charged user pages, this cgroup only */
    /* Subtree total (own + all descendants), maintained INCREMENTALLY.
     *
     * The first version summed the subtree on every charge by scanning all
     * CGROUP_MAX slots and walking each one's parent chain -- O(32 x depth) per
     * PAGE FAULT, paid even on a machine with no limits set at all. That is
     * exactly the allocator-hot-path cost the plan warned would make this slice
     * not worth shipping. Walking ancestors and adding the delta is O(depth) with
     * no inner loop, and makes memory.current an O(1) read. */
    uint64_t mem_subtree;
    uint64_t mem_peak_pages;          /* high-water mark (memory.peak) */
    int64_t  mem_max_pages;           /* CGROUP_MEM_UNLIMITED == "max" */
    uint64_t mem_fail;                /* refusals (memory.events: max) */
    uint64_t mem_oom_kill;            /* processes killed for this limit */

    /* ---- slice 16: the io controller ---------------------------------------
     * Charged in the blk_read/blk_write funnel, which is `static inline` in
     * blk.h and so is the ONE place every block operation in the kernel passes
     * through -- no driver can bypass it without bypassing the API. */
    uint64_t io_rbytes, io_wbytes;
    uint64_t io_rios,   io_wios;
};

static struct cgroup g_cg[CGROUP_MAX];

void cgroup_init(void) {
    memset(g_cg, 0, sizeof g_cg);
    g_cg[CGROUP_ROOT].in_use     = true;
    g_cg[CGROUP_ROOT].parent     = -1;
    g_cg[CGROUP_ROOT].pids_max   = CGROUP_PIDS_UNLIMITED;
    g_cg[CGROUP_ROOT].cpu_weight = CGROUP_WEIGHT_DEFAULT;
    g_cg[CGROUP_ROOT].mem_max_pages = CGROUP_MEM_UNLIMITED;
    g_cg[CGROUP_ROOT].name[0]    = '\0';          /* the root has no name */
    kprintf("[cgroup] v2 hierarchy ready (%d slots, root weight %u)\n",
            CGROUP_MAX, (unsigned)CGROUP_WEIGHT_DEFAULT);
}

int cgroup_of(struct proc *p) {
    if (!p) return CGROUP_ROOT;
    int cg = p->cgroup;
    if (cg < 0 || cg >= CGROUP_MAX || !g_cg[cg].in_use) return CGROUP_ROOT;
    return cg;
}

/* Is `cg` inside `anc` (or equal to it)? */
static bool cg_is_descendant(int cg, int anc) {
    for (int i = cg; i >= 0; i = g_cg[i].parent) {
        if (i == anc) return true;
        if (i == CGROUP_ROOT) break;
    }
    return false;
}

int cgroup_nr_procs(int cg) {
    if (cg < 0 || cg >= CGROUP_MAX) return 0;
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        if (g_proc[i].state == PROC_UNUSED ||
            g_proc[i].state == PROC_EMBRYO ||
            g_proc[i].state == PROC_TERMINATED) continue;
        if (cg_is_descendant(cgroup_of(&g_proc[i]), cg)) n++;
    }
    return n;
}

long cgroup_attach(struct proc *p, int cg) {
    if (!p) return -ABI_EINVAL;
    if (cg < 0 || cg >= CGROUP_MAX || !g_cg[cg].in_use) return -ABI_ENOENT;
    int from = cgroup_of(p);
    /* Slice 16: the charge follows the process. Migrating without this leaves the
     * pages counted against the cgroup the process LEFT -- and a container runtime
     * migrates as its very first act, so the bug would be present from the first
     * container onward, showing up as a limit that fires against the wrong group. */
    cgroup_mem_migrate(p, from, cg);
    p->cgroup = cg;
    /* Moving a process re-derives its scheduling priority from the new
     * cgroup's weight -- otherwise cpu.weight would apply only to processes
     * that happened to be created after it was set. */
    sched_set_prio(p->pid, cgroup_prio_for(p));
    return 0;
}

/* ---- the pids controller ---------------------------------------------- */

long cgroup_can_fork(struct proc *parent) {
    int cg = cgroup_of(parent);
    /* Walk to the root: the limit is HIERARCHICAL. Checking only the child's own
     * cgroup would let any container raise its own pids.max and escape the cap
     * its parent set, which is the entire point of the control. */
    for (int i = cg; i >= 0; i = g_cg[i].parent) {
        if (g_cg[i].pids_max != CGROUP_PIDS_UNLIMITED &&
            cgroup_nr_procs(i) >= g_cg[i].pids_max) {
            kprintf("[cgroup] fork denied: '%s' at pids.max=%d\n",
                    g_cg[i].name[0] ? g_cg[i].name : "/", g_cg[i].pids_max);
            return -ABI_EAGAIN;
        }
        if (i == CGROUP_ROOT) break;
    }
    return 0;
}

/* ---- the cpu controller ----------------------------------------------- */

/* cpu.weight (1..10000, default 100) onto the 5 priority classes.
 *
 * The mapping is coarse because the scheduler is coarse -- this is a class-based
 * scheduler with aging, not a virtual-runtime one, so weight cannot be honoured
 * proportionally. Stated plainly rather than implied: cpu.weight here selects a
 * PRIORITY BAND, it does not divide CPU in the ratio of the weights.
 *
 * PRIO_RT is deliberately NOT reachable from cpu.weight: it is for
 * latency-critical kernel-side work, and letting an unprivileged container claim
 * it by writing a number into a file would let it starve the desktop. The
 * scheduler's aging bounds the damage, but the gate is the right place to stop it. */
int cgroup_prio_for(struct proc *p) {
    uint32_t w = g_cg[cgroup_of(p)].cpu_weight;
    if (w <  10)  return PRIO_IDLE;
    if (w <  50)  return PRIO_LOW;
    if (w < 400)  return PRIO_NORMAL;      /* the default 100 lands here */
    return PRIO_HIGH;
}

void cgroup_account_cpu(struct proc *p, uint64_t ns) {
    g_cg[cgroup_of(p)].cpu_usage_ns += ns;
}

/* ---- the memory controller (slice 16) ----------------------------------
 *
 * WHAT IS CHARGED: anonymous user address-space pages -- brk growth, demand
 * faults, CoW copies, and the pages an exec/spawn installs. That is what makes a
 * container's footprint grow, and it is the thing a limit has to be able to stop.
 *
 * WHAT IS NOT CHARGED, deliberately and stated rather than implied: page cache,
 * memfd/shm backing pages, and kernel-side allocations (page tables, slab). Linux
 * charges all three. Saying "memory.current" while silently omitting them would
 * be the same class of lie as an unenforced pids.max, so the omission is named
 * here, in the harness output, and in the handoff doc.
 *
 * SHARED PAGES ARE COUNTED IN BOTH CGROUPS after fork, because a child inherits
 * its parent's page count and both processes' RSS include the shared CoW pages.
 * Linux charges such a page once, to whoever touched it first. Ours over-counts
 * a forked child that has not yet diverged. The trade is deliberate: it buys the
 * invariant `mem_pages == sum of member user_pages`, which the harness can and
 * does check -- a counter nobody can audit is how drift ships unnoticed. */

/* Would `pages` more exceed the limit anywhere from `cg` up to the root?
 * Hierarchical for the same reason pids.max is: otherwise a container raises its
 * own memory.max and walks out of its parent's cap. */
static int cg_mem_would_exceed(int cg, uint64_t pages) {
    for (int i = cg; i >= 0; i = g_cg[i].parent) {
        if (g_cg[i].mem_max_pages != CGROUP_MEM_UNLIMITED) {
            /* An ancestor's usage is its own plus every descendant's, so the
             * comparison has to be against the SUBTREE total, not g_cg[i]. */
            if (cgroup_mem_pages(i) + pages > (uint64_t)g_cg[i].mem_max_pages)
                return i;
        }
        if (i == CGROUP_ROOT) break;
    }
    return -1;
}

#ifdef CGMEM_TRACE
/* See the note in pmm.c. Only fires for a process whose cgroup carries a limit,
 * and only while inside pmm_alloc_page -- so a charged allocation shows up too,
 * and the two can be told apart by whether mm_user_page_alloc is on the path. */
static int g_cgmem_trace_n;
void cgmem_trace_alloc(void *ra) {
    struct proc *p = current_proc();
    if (!p) return;
    int cg = cgroup_of(p);
    if (cg == CGROUP_ROOT || g_cg[cg].mem_max_pages == CGROUP_MEM_UNLIMITED) return;
    if (g_cgmem_trace_n >= 40) return;
    g_cgmem_trace_n++;
    kprintf("[cgtrace] pid=%d cg%d alloc from ra=%p\n", p->pid, cg, ra);
}
#endif

uint64_t cgroup_mem_pages(int cg) {
    if (cg < 0 || cg >= CGROUP_MAX || !g_cg[cg].in_use) return 0;
    return g_cg[cg].mem_subtree;
}

/* Add `delta` to the subtree totals of `cg` and every ancestor, refreshing each
 * one's peak on the way up. One walk does both, and it is the only place either
 * number changes -- so memory.current and memory.peak cannot disagree. */
static void cg_subtree_add(int cg, int64_t delta) {
    for (int i = cg; i >= 0; i = g_cg[i].parent) {
        if (delta >= 0) {
            g_cg[i].mem_subtree += (uint64_t)delta;
            if (g_cg[i].mem_subtree > g_cg[i].mem_peak_pages)
                g_cg[i].mem_peak_pages = g_cg[i].mem_subtree;
        } else {
            uint64_t d = (uint64_t)(-delta);
            g_cg[i].mem_subtree = (g_cg[i].mem_subtree > d)
                                    ? g_cg[i].mem_subtree - d : 0;
        }
        if (i == CGROUP_ROOT) break;
    }
}

bool cgroup_mem_charge(struct proc *p, uint64_t pages) {
    if (!p || pages == 0) return true;
    int cg = cgroup_of(p);

    int over = cg_mem_would_exceed(cg, pages);
    if (over >= 0) {
        g_cg[over].mem_fail++;
        if (g_cg[over].mem_fail <= 3)
            kprintf("[cgmem] REFUSED %lu page(s) for pid=%d: '%s' at "
                    "memory.max=%ld (usage %lu)\n",
                    (unsigned long)pages, p->pid,
                    g_cg[over].name[0] ? g_cg[over].name : "/",
                    (long)g_cg[over].mem_max_pages,
                    (unsigned long)cgroup_mem_pages(over));
        return false;
    }
#ifdef CGMEM_TRACE
    /* Bring-up only: the first few charges into each NON-ROOT cgroup, so it is
     * visible which cgroup a given pid charges to. Bounded per cgroup. */
    if (cg != CGROUP_ROOT) {
        static uint8_t probed[CGROUP_MAX];
        if (probed[cg] < 3) { probed[cg]++;
            kprintf("[cgmem] charge %lu -> cg%d '%s' pid=%d usage %lu max=%ld\n",
                    (unsigned long)pages, cg,
                    g_cg[cg].name[0] ? g_cg[cg].name : "/", p->pid,
                    (unsigned long)cgroup_mem_pages(cg),
                    (long)g_cg[cg].mem_max_pages);
        }
    }
#endif
    g_cg[cg].mem_pages += pages;
    p->user_pages      += pages;
    cg_subtree_add(cg, (int64_t)pages);      /* O(depth), updates peaks too */
    return true;
}

void cgroup_mem_uncharge(struct proc *p, uint64_t pages) {
    if (!p || pages == 0) return;
    int cg = cgroup_of(p);
    /* Clamp rather than wrap. An underflow here would read as an ENORMOUS
     * memory.current and make the limit unhittable -- fail visible, not silent. */
    if (pages > g_cg[cg].mem_pages) {
        kprintf("[cgroup] WARN uncharge %lu > charged %lu in '%s' (clamping)\n",
                (unsigned long)pages, (unsigned long)g_cg[cg].mem_pages,
                g_cg[cg].name[0] ? g_cg[cg].name : "/");
        pages = g_cg[cg].mem_pages;
    }
    g_cg[cg].mem_pages -= pages;
    p->user_pages = (p->user_pages > pages) ? p->user_pages - pages : 0;
    cg_subtree_add(cg, -(int64_t)pages);
}

void cgroup_mem_note_oom_kill(struct proc *p) {
    if (p) g_cg[cgroup_of(p)].mem_oom_kill++;
}

/* Moving a process must move its charge, or the invariant breaks the first time
 * anything is migrated -- which is exactly what a container runtime does. */
void cgroup_mem_migrate(struct proc *p, int from, int to) {
    if (!p || from == to) return;
    uint64_t pages = p->user_pages;
    if (pages == 0) return;
    if (from >= 0 && from < CGROUP_MAX) {
        if (pages > g_cg[from].mem_pages) pages = g_cg[from].mem_pages;
        g_cg[from].mem_pages -= pages;
        cg_subtree_add(from, -(int64_t)pages);
    }
    if (to >= 0 && to < CGROUP_MAX) {
        g_cg[to].mem_pages += pages;
        cg_subtree_add(to, (int64_t)pages);
    }
}

/* ---- the io controller (slice 16) --------------------------------------
 *
 * Accounting only: io.stat is real, and there is NO io.max, because throttling
 * needs a place to sleep inside the block path and blk_read/blk_write are
 * synchronous inlines called from filesystem code holding locks. Exposing an
 * io.max that accepted a number and throttled nothing is the failure this arc
 * has already caught twice (VFS_MNT_RDONLY, PR_SET_NO_NEW_PRIVS), so the file
 * does not exist rather than existing as a lie. */
void cgroup_io_charge(struct proc *p, bool write, uint64_t bytes) {
    int cg = cgroup_of(p);
    if (write) { g_cg[cg].io_wbytes += bytes; g_cg[cg].io_wios++; }
    else       { g_cg[cg].io_rbytes += bytes; g_cg[cg].io_rios++; }
}

void cgroup_io_stat(int cg, uint64_t *rb, uint64_t *wb, uint64_t *ri, uint64_t *wi) {
    if (cg < 0 || cg >= CGROUP_MAX) cg = CGROUP_ROOT;
    if (rb) *rb = g_cg[cg].io_rbytes;
    if (wb) *wb = g_cg[cg].io_wbytes;
    if (ri) *ri = g_cg[cg].io_rios;
    if (wi) *wi = g_cg[cg].io_wios;
}

/* ---- THE FUNNEL ---------------------------------------------------------
 *
 * Every user page in the system is obtained here and released here. That is the
 * whole reason the accounting can be trusted: the alternative -- a counter bumped
 * at each of the eight scattered pmm_alloc_page() call sites -- is one missed site
 * away from a number that looks precise and drifts. One funnel is auditable by
 * grep: `pmm_alloc_page` must not appear in a user-page context anywhere else.
 *
 * Returns 0 both when the limit refuses and when the machine is genuinely out of
 * memory; callers already handle a 0 from pmm_alloc_page, so every existing
 * failure path works unchanged. The distinction is recorded in memory.events. */
/* A user page belongs to an ADDRESS SPACE, not to the task that happened to fault
 * it in. Threads share one address space, so charging the faulting task means:
 *
 *   leader + thread share an mm; the thread faults 100 pages (charged to the
 *   thread); the thread exits; proc_reap releases its 100 -- while all 100 pages
 *   are STILL MAPPED and in use by the leader.
 *
 * That under-reports memory.current for every threaded program, and chromium runs
 * ~30 threads per process. Resolving to the mm owner (the same key mmap.c uses for
 * the VMA table) makes the charge live exactly as long as the mapping does: threads
 * carry no charge of their own, and the leader's teardown releases the whole space.
 * It also puts the charge in the cgroup the process is in rather than in whichever
 * cgroup a thread was moved to. */
static struct proc *mm_charge_target(struct proc *p) {
    if (!p) return 0;
    int owner = proc_mm_pid(p);
    if (owner == p->pid) return p;
    struct proc *o = proc_lookup(owner);
    return o ? o : p;                  /* owner gone: charge the task, never lose it */
}

uint64_t mm_user_page_alloc(struct proc *p) {
    struct proc *t = mm_charge_target(p);
    if (!cgroup_mem_charge(t, 1)) return 0;         /* over memory.max */
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) { cgroup_mem_uncharge(t, 1); return 0; }
    return phys;
}

void mm_user_page_free(struct proc *p, uint64_t phys) {
    if (phys) pmm_free_page(phys);
    cgroup_mem_uncharge(mm_charge_target(p), 1);
}

/* Bulk uncharge at process teardown. The process's OWN counter is the source of
 * truth, so this cannot leave the cgroup counter behind however the pages were
 * freed -- including paths that free the whole user half in one walk. */
void mm_user_pages_release_all(struct proc *p) {
    if (p && p->user_pages) cgroup_mem_uncharge(p, p->user_pages);
}

/* For the harness's invariant check: mem_pages vs summed member user_pages. */
uint64_t cgroup_mem_pages_own(int cg) {
    if (cg < 0 || cg >= CGROUP_MAX) return 0;
    return g_cg[cg].mem_pages;
}
int64_t cgroup_mem_max_pages(int cg) {
    if (cg < 0 || cg >= CGROUP_MAX) return CGROUP_MEM_UNLIMITED;
    return g_cg[cg].mem_max_pages;
}
uint64_t cgroup_mem_fail(int cg) {
    if (cg < 0 || cg >= CGROUP_MAX) return 0;
    return g_cg[cg].mem_fail;
}

/* ===================================================================
 * cgroupfs
 *
 * Directories ARE cgroups. There is no cgroup syscall in Linux either: the
 * filesystem is the ABI.
 * =================================================================== */

struct cgfs_handle {
    char   data[256];
    size_t len, pos;
    int    cg;            /* which cgroup this file belongs to */
    int    which;         /* CGF_* */
};

enum { CGF_NONE = 0, CGF_PROCS, CGF_PIDS_MAX, CGF_PIDS_CURRENT,
       CGF_CPU_WEIGHT, CGF_CPU_STAT, CGF_CONTROLLERS, CGF_SUBTREE,
       /* slice 16 */
       CGF_MEM_CURRENT, CGF_MEM_MAX, CGF_MEM_PEAK, CGF_MEM_EVENTS, CGF_IO_STAT };

static int cg_find(int parent, const char *name) {
    for (int i = 1; i < CGROUP_MAX; i++)
        if (g_cg[i].in_use && g_cg[i].parent == parent &&
            strcmp(g_cg[i].name, name) == 0) return i;
    return -1;
}

/* Split "/foo/pids.max" into the cgroup it names and the control file.
 * Returns the cgroup index, or -1. `*file` gets a CGF_* (CGF_NONE = the
 * directory itself). */
static int cg_resolve(const char *path, int *file) {
    *file = CGF_NONE;
    int cg = CGROUP_ROOT;
    const char *s = path;
    if (*s == '/') s++;
    char comp[CGROUP_NAME_MAX];
    while (*s) {
        size_t n = 0;
        while (s[n] && s[n] != '/') n++;
        if (n >= sizeof comp) return -1;
        memcpy(comp, s, n); comp[n] = '\0';
        s += n;
        if (*s == '/') s++;
        /* A control file is always the LAST component and always contains a
         * '.', which is exactly how cgroup v2 keeps names unambiguous: a
         * control file may not contain '.', so no cgroup can shadow one. */
        bool has_dot = false;
        for (size_t i = 0; i < n; i++) if (comp[i] == '.') has_dot = true;
        if (has_dot || strcmp(comp, "cgroup.procs") == 0) {
            if (*s) return -1;                   /* nothing may follow a file */
            if      (strcmp(comp, "cgroup.procs") == 0)   *file = CGF_PROCS;
            else if (strcmp(comp, "pids.max") == 0)       *file = CGF_PIDS_MAX;
            else if (strcmp(comp, "pids.current") == 0)   *file = CGF_PIDS_CURRENT;
            else if (strcmp(comp, "cpu.weight") == 0)     *file = CGF_CPU_WEIGHT;
            else if (strcmp(comp, "cpu.stat") == 0)       *file = CGF_CPU_STAT;
            else if (strcmp(comp, "memory.current") == 0) *file = CGF_MEM_CURRENT;
            else if (strcmp(comp, "memory.max") == 0)     *file = CGF_MEM_MAX;
            else if (strcmp(comp, "memory.peak") == 0)    *file = CGF_MEM_PEAK;
            else if (strcmp(comp, "memory.events") == 0)  *file = CGF_MEM_EVENTS;
            else if (strcmp(comp, "io.stat") == 0)        *file = CGF_IO_STAT;
            else if (strcmp(comp, "cgroup.controllers") == 0) *file = CGF_CONTROLLERS;
            else if (strcmp(comp, "cgroup.subtree_control") == 0) *file = CGF_SUBTREE;
            else return -1;
            return cg;
        }
        int nx = cg_find(cg, comp);
        if (nx < 0) return -1;
        cg = nx;
    }
    return cg;
}

static size_t put_u(char *b, size_t off, uint64_t v) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) b[off++] = t[--n];
    return off;
}

static void cg_render(struct cgfs_handle *h) {
    size_t o = 0;
    switch (h->which) {
    /* Advertise exactly the controllers that ENFORCE something or report real
     * numbers. `io` is listed because io.stat is real; it has no io.max, and a
     * reader can tell because the file is absent -- which is the honest signal. */
    case CGF_CONTROLLERS:
    case CGF_SUBTREE:
        memcpy(h->data, "cpu io memory pids\n", 19); o = 19;
        break;
    case CGF_PIDS_MAX:
        if (g_cg[h->cg].pids_max == CGROUP_PIDS_UNLIMITED) {
            memcpy(h->data, "max\n", 4); o = 4;
        } else {
            o = put_u(h->data, 0, (uint64_t)g_cg[h->cg].pids_max);
            h->data[o++] = '\n';
        }
        break;
    case CGF_PIDS_CURRENT:
        o = put_u(h->data, 0, (uint64_t)cgroup_nr_procs(h->cg));
        h->data[o++] = '\n';
        break;
    case CGF_CPU_WEIGHT:
        o = put_u(h->data, 0, g_cg[h->cg].cpu_weight);
        h->data[o++] = '\n';
        break;
    case CGF_CPU_STAT:
        memcpy(h->data, "usage_usec ", 11); o = 11;
        o = put_u(h->data, o, g_cg[h->cg].cpu_usage_ns / 1000ull);
        h->data[o++] = '\n';
        break;
    /* memory.* are reported in BYTES, as the v2 ABI specifies -- the internal
     * unit is pages, and a controller that reported pages while claiming to be
     * cgroup v2 would silently mislead anything that parses it. */
    case CGF_MEM_CURRENT:
        o = put_u(h->data, 0, cgroup_mem_pages(h->cg) * 4096ull);
        h->data[o++] = '\n';
        break;
    case CGF_MEM_PEAK:
        o = put_u(h->data, 0, g_cg[h->cg].mem_peak_pages * 4096ull);
        h->data[o++] = '\n';
        break;
    case CGF_MEM_MAX:
        if (g_cg[h->cg].mem_max_pages == CGROUP_MEM_UNLIMITED) {
            memcpy(h->data, "max\n", 4); o = 4;
        } else {
            o = put_u(h->data, 0, (uint64_t)g_cg[h->cg].mem_max_pages * 4096ull);
            h->data[o++] = '\n';
        }
        break;
    case CGF_MEM_EVENTS:
        /* Linux's field names. `max` counts allocation refusals; `oom_kill`
         * counts processes killed. `low`/`high` are absent rather than
         * hard-zero: there is no memory.low or memory.high here to produce them. */
        memcpy(h->data, "max ", 4); o = 4;
        o = put_u(h->data, o, g_cg[h->cg].mem_fail);
        memcpy(h->data + o, "\noom_kill ", 10); o += 10;
        o = put_u(h->data, o, g_cg[h->cg].mem_oom_kill);
        h->data[o++] = '\n';
        break;
    case CGF_IO_STAT: {
        /* v2 format is one line per device, keyed by maj:min. This kernel has no
         * device-number registry, so the single aggregate line is keyed 0:0 --
         * a placeholder that is visibly a placeholder, not a fake device. */
        memcpy(h->data, "0:0 rbytes ", 11); o = 11;
        o = put_u(h->data, o, g_cg[h->cg].io_rbytes);
        memcpy(h->data + o, " wbytes ", 8); o += 8;
        o = put_u(h->data, o, g_cg[h->cg].io_wbytes);
        memcpy(h->data + o, " rios ", 6); o += 6;
        o = put_u(h->data, o, g_cg[h->cg].io_rios);
        memcpy(h->data + o, " wios ", 6); o += 6;
        o = put_u(h->data, o, g_cg[h->cg].io_wios);
        h->data[o++] = '\n';
        break;
    }
    case CGF_PROCS: {
        /* Only DIRECT members, as v2 specifies -- a process in a child cgroup
         * appears in that child's cgroup.procs, not here. */
        for (int i = 0; i < PROC_MAX && o < sizeof(h->data) - 12; i++) {
            if (g_proc[i].state == PROC_UNUSED ||
                g_proc[i].state == PROC_EMBRYO ||
                g_proc[i].state == PROC_TERMINATED) continue;
            if (cgroup_of(&g_proc[i]) != h->cg) continue;
            o = put_u(h->data, o, (uint64_t)g_proc[i].pid);
            h->data[o++] = '\n';
        }
        break;
    }
    default: break;
    }
    h->data[o] = '\0';
    h->len = o;
    h->pos = 0;
}

static int cgfs_open(void *mnt, const char *path, struct vfs_file *out) {
    (void)mnt;
    int which = CGF_NONE;
    int cg = cg_resolve(path, &which);
    if (cg < 0 || which == CGF_NONE) return VFS_ERR_NOENT;
    struct cgfs_handle *h = kmalloc(sizeof *h);
    if (!h) return VFS_ERR_NOMEM;
    memset(h, 0, sizeof *h);
    h->cg = cg; h->which = which;
    cg_render(h);
    out->priv = h;
    out->size = h->len;
    return VFS_OK;
}

static int cgfs_close(struct vfs_file *f) { kfree(f->priv); f->priv = 0; return VFS_OK; }

static long cgfs_read(struct vfs_file *f, void *buf, size_t n) {
    struct cgfs_handle *h = f->priv;
    if (!h) return VFS_ERR_INVAL;
    if (h->pos >= h->len) return 0;
    size_t avail = h->len - h->pos;
    if (n > avail) n = avail;
    memcpy(buf, h->data + h->pos, n);
    h->pos += n;
    f->pos = h->pos;
    return (long)n;
}

static long cgfs_write(struct vfs_file *f, const void *buf, size_t n) {
    struct cgfs_handle *h = f->priv;
    if (!h) return VFS_ERR_INVAL;
    char tmp[64];
    size_t k = n < sizeof(tmp) - 1 ? n : sizeof(tmp) - 1;
    memcpy(tmp, buf, k); tmp[k] = '\0';

    switch (h->which) {
    case CGF_PIDS_MAX: {
        if (strncmp(tmp, "max", 3) == 0) {
            g_cg[h->cg].pids_max = CGROUP_PIDS_UNLIMITED;
        } else {
            long v = 0; const char *s = tmp;
            while (*s == ' ') s++;
            if (*s < '0' || *s > '9') return VFS_ERR_INVAL;
            while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
            g_cg[h->cg].pids_max = (int32_t)v;
        }
        kprintf("[cgroup] '%s' pids.max = %d\n",
                g_cg[h->cg].name[0] ? g_cg[h->cg].name : "/",
                g_cg[h->cg].pids_max);
        return (long)n;
    }
    case CGF_CPU_WEIGHT: {
        uint32_t v = 0; const char *s = tmp;
        while (*s == ' ') s++;
        if (*s < '0' || *s > '9') return VFS_ERR_INVAL;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
        if (v < CGROUP_WEIGHT_MIN || v > CGROUP_WEIGHT_MAX) return VFS_ERR_INVAL;
        g_cg[h->cg].cpu_weight = v;
        /* Re-derive the priority of every process already in this cgroup --
         * otherwise the weight would only affect future members, which reads as
         * "the file works" while changing nothing for the running workload. */
        for (int i = 0; i < PROC_MAX; i++) {
            if (g_proc[i].state == PROC_UNUSED ||
                g_proc[i].state == PROC_EMBRYO) continue;
            if (cgroup_of(&g_proc[i]) != h->cg) continue;
            sched_set_prio(g_proc[i].pid, cgroup_prio_for(&g_proc[i]));
        }
        kprintf("[cgroup] '%s' cpu.weight = %u -> prio class %d\n",
                g_cg[h->cg].name[0] ? g_cg[h->cg].name : "/", v,
                cgroup_prio_for(&g_proc[0]) /* class for this weight */);
        return (long)n;
    }
    case CGF_MEM_MAX: {
        /* Written in BYTES (v2 ABI), stored in pages. Rounded UP: a limit of
         * 5000 bytes that permitted only one page would be a limit smaller than
         * the thing it measures, and every allocation would fail. */
        if (strncmp(tmp, "max", 3) == 0) {
            g_cg[h->cg].mem_max_pages = CGROUP_MEM_UNLIMITED;
        } else {
            uint64_t v = 0; const char *s = tmp;
            while (*s == ' ') s++;
            if (*s < '0' || *s > '9') return VFS_ERR_INVAL;
            while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; }
            g_cg[h->cg].mem_max_pages = (int64_t)((v + 4095ull) / 4096ull);
        }
        kprintf("[cgroup] '%s' memory.max = %ld pages (current %lu)\n",
                g_cg[h->cg].name[0] ? g_cg[h->cg].name : "/",
                (long)g_cg[h->cg].mem_max_pages,
                (unsigned long)cgroup_mem_pages(h->cg));
        return (long)n;
    }
    case CGF_PROCS: {
        int pid = 0; const char *s = tmp;
        while (*s == ' ') s++;
        if (*s < '0' || *s > '9') return VFS_ERR_INVAL;
        while (*s >= '0' && *s <= '9') { pid = pid * 10 + (*s - '0'); s++; }
        struct proc *t = proc_lookup(pid);
        if (!t) return VFS_ERR_NOENT;
        long rc = cgroup_attach(t, h->cg);
        if (rc != 0) return VFS_ERR_INVAL;
        kprintf("[cgroup] pid=%d attached to '%s'\n", pid,
                g_cg[h->cg].name[0] ? g_cg[h->cg].name : "/");
        return (long)n;
    }
    default:
        return VFS_ERR_ROFS;
    }
}

static int cgfs_mkdir(void *mnt, const char *path, uint32_t uid, uint32_t gid,
                      uint32_t mode) {
    (void)mnt; (void)uid; (void)gid; (void)mode;
    /* The parent must exist and the leaf must not. Split off the last part. */
    char buf[VFS_PATH_MAX];
    size_t l = strlen(path);
    if (l >= sizeof buf) return VFS_ERR_NAMETOOLONG;
    memcpy(buf, path, l + 1);
    while (l > 1 && buf[l - 1] == '/') buf[--l] = '\0';
    char *slash = 0;
    for (char *q = buf; *q; q++) if (*q == '/') slash = q;
    if (!slash) return VFS_ERR_INVAL;
    const char *leaf = slash + 1;
    if (!*leaf || strlen(leaf) >= CGROUP_NAME_MAX) return VFS_ERR_INVAL;
    /* A cgroup name may not contain '.', so it can never shadow a control
     * file -- the same rule cgroup v2 uses. */
    for (const char *q = leaf; *q; q++) if (*q == '.') return VFS_ERR_INVAL;
    *slash = '\0';
    int dummy = CGF_NONE;
    int parent = cg_resolve(buf[0] ? buf : "/", &dummy);
    if (parent < 0 || dummy != CGF_NONE) return VFS_ERR_NOENT;
    if (cg_find(parent, leaf) >= 0) return VFS_ERR_EXIST;

    for (int i = 1; i < CGROUP_MAX; i++) {
        if (g_cg[i].in_use) continue;
        memset(&g_cg[i], 0, sizeof g_cg[i]);
        g_cg[i].in_use = true;
        g_cg[i].parent = parent;
        size_t nl = strlen(leaf);
        memcpy(g_cg[i].name, leaf, nl + 1);
        /* A fresh cgroup inherits NO limit and the default weight, as v2
         * specifies -- limits are not inherited as values, they are enforced
         * hierarchically at fork time (see cgroup_can_fork). */
        g_cg[i].pids_max      = CGROUP_PIDS_UNLIMITED;
        g_cg[i].cpu_weight    = CGROUP_WEIGHT_DEFAULT;
        g_cg[i].mem_max_pages = CGROUP_MEM_UNLIMITED;
        kprintf("[cgroup] created '%s' (idx %d, parent %d)\n", leaf, i, parent);
        return VFS_OK;
    }
    return VFS_ERR_NOMEM;
}

static int cgfs_unlink(void *mnt, const char *path) {
    (void)mnt;
    int which = CGF_NONE;
    int cg = cg_resolve(path, &which);
    if (cg < 0) return VFS_ERR_NOENT;
    if (which != CGF_NONE) return VFS_ERR_PERM;   /* control files are permanent */
    if (cg == CGROUP_ROOT) return VFS_ERR_PERM;
    /* Refuse while populated or while it has children -- rmdir(2) on a cgroup
     * with members is EBUSY on Linux, and silently discarding the membership
     * would leave processes pointing at a freed cgroup. */
    if (cgroup_nr_procs(cg) > 0) return VFS_ERR_PERM;
    for (int i = 1; i < CGROUP_MAX; i++)
        if (g_cg[i].in_use && g_cg[i].parent == cg) return VFS_ERR_PERM;
    /* Slice 16: hand any residual charge back to the ancestors before zeroing.
     *
     * This is reachable even though a populated cgroup cannot be removed:
     * cgroup_nr_procs() skips PROC_TERMINATED, but a terminated process's charge is
     * only released in proc_reap(). So a cgroup whose last member has exited and not
     * yet been reaped counts as empty here while still holding pages. Without this,
     * every ancestor's mem_subtree stays inflated FOREVER -- and the later
     * mm_user_pages_release_all() would subtract from the root instead, because
     * cgroup_of() falls back to root once in_use goes false. */
    if (g_cg[cg].mem_pages) {
        kprintf("[cgroup] '%s' removed holding %lu charged page(s) (unreaped "
                "member); returning them to the parent\n",
                g_cg[cg].name, (unsigned long)g_cg[cg].mem_pages);
        cg_subtree_add(cg, -(int64_t)g_cg[cg].mem_pages);
    }
    kprintf("[cgroup] removed '%s'\n", g_cg[cg].name);
    memset(&g_cg[cg], 0, sizeof g_cg[cg]);
    return VFS_OK;
}

static int cgfs_stat(void *mnt, const char *path, struct vfs_stat *out) {
    (void)mnt;
    int which = CGF_NONE;
    int cg = cg_resolve(path, &which);
    if (cg < 0) return VFS_ERR_NOENT;
    memset(out, 0, sizeof *out);
    if (which == CGF_NONE) { out->type = VFS_TYPE_DIR;  out->mode = 0755; }
    else                   { out->type = VFS_TYPE_FILE; out->mode = 0644; }
    return VFS_OK;
}

struct cgfs_dir { int cg; int idx; };

static int cgfs_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    (void)mnt;
    int which = CGF_NONE;
    int cg = cg_resolve(path, &which);
    if (cg < 0 || which != CGF_NONE) return VFS_ERR_NOENT;
    struct cgfs_dir *d = kmalloc(sizeof *d);
    if (!d) return VFS_ERR_NOMEM;
    d->cg = cg; d->idx = 0;
    out->priv = d;
    return VFS_OK;
}
static int cgfs_closedir(struct vfs_dir *d) { kfree(d->priv); d->priv = 0; return VFS_OK; }

static int cgfs_readdir(struct vfs_dir *dd, struct vfs_dirent *out) {
    struct cgfs_dir *d = dd->priv;
    if (!d) return VFS_ERR_INVAL;
    static const char *files[] = { "cgroup.procs", "cgroup.controllers",
                                   "cgroup.subtree_control", "pids.max",
                                   "pids.current", "cpu.weight", "cpu.stat",
                                   /* slice 16 -- note there is deliberately no
                                    * io.max: it would enforce nothing. */
                                   "memory.current", "memory.max", "memory.peak",
                                   "memory.events", "io.stat" };
    const int nf = (int)(sizeof files / sizeof files[0]);
    memset(out, 0, sizeof *out);
    if (d->idx < nf) {
        size_t l = strlen(files[d->idx]);
        memcpy(out->name, files[d->idx], l);
        out->type = VFS_TYPE_FILE;
        d->idx++;
        return VFS_OK;
    }
    for (int i = d->idx - nf + 1; i < CGROUP_MAX; i++) {
        if (!g_cg[i].in_use || g_cg[i].parent != d->cg) continue;
        size_t l = strlen(g_cg[i].name);
        memcpy(out->name, g_cg[i].name, l);
        out->type = VFS_TYPE_DIR;
        d->idx = nf + i;
        return VFS_OK;
    }
    return VFS_ERR_NOENT;
}

static const struct vfs_ops cgroupfs_ops = {
    .open = cgfs_open, .close = cgfs_close, .read = cgfs_read,
    .write = cgfs_write, .create = 0, .unlink = cgfs_unlink,
    .mkdir = cgfs_mkdir, .opendir = cgfs_opendir, .closedir = cgfs_closedir,
    .readdir = cgfs_readdir, .stat = cgfs_stat, .chmod = 0, .chown = 0,
    .readlink = 0, .umount = 0,
};

void cgroup_mount(void) {
    int rc = vfs_mount("/sys/fs/cgroup", &cgroupfs_ops, 0);
    kprintf("[cgroup] cgroup2 mounted at /sys/fs/cgroup (rc=%d)\n", rc);
}

/* Mount cgroup2 at an arbitrary point, for mount(2) -t cgroup2. Same singleton
 * argument as procfs_mount_at: cgroupfs has no per-mount state (data==0) and
 * renders the ONE cgroup hierarchy, which is what cgroup v2 is -- so a second
 * mount point is a second view, not a second hierarchy.
 *
 * That also names the limit honestly: this is NOT a per-namespace cgroup view.
 * A container mounting cgroup2 sees the whole hierarchy, not a root rebased at
 * its own cgroup, because there is no cgroup namespace in this kernel. */
int cgroup_mount_at(const char *path) {
    return vfs_mount(path, &cgroupfs_ops, 0);
}
