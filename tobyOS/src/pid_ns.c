/* pid_ns.c -- PID namespaces (Phase 3 slice 10).
 *
 * See the long comment in nsproxy.h for the constraint that shapes everything
 * here: a pid in tobyOS IS an index into g_proc[PROC_MAX], so pids cannot be
 * reassigned. Namespace-local pids ("vpids") are therefore a TRANSLATION at the
 * syscall boundary over a stored host pid ("kpid") -- the same
 * store-global-translate-at-the-edge shape slice 2 chose for uids, and for the
 * same reason: the alternative is auditing every internal user of p->pid.
 *
 * ---------------------------------------------------------------------------
 * WHY THERE ARE TWO NAMESPACE POINTERS ON struct proc
 *
 * `pid_ns` is the namespace the process is IN. `pid_ns_for_children` is the
 * namespace its next child will be placed in. They differ because
 * unshare(CLONE_NEWPID) is the one namespace operation that does NOT move the
 * caller -- a process cannot change its own pid namespace, because its pid in
 * the new namespace would have to appear out of nowhere while everything
 * already holding the old number kept using it. Linux has exactly this split
 * (nsproxy->pid_ns_for_children), and collapsing the two is the single easiest
 * way to get CLONE_NEWPID subtly wrong.
 * ---------------------------------------------------------------------------
 */

#include <tobyos/nsproxy.h>
#include <tobyos/proc.h>
#include <tobyos/signal.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/spinlock.h>
#include <tobyos/abi/abi.h>

extern struct proc g_proc[PROC_MAX];

struct pid_ns {
    int             refs;
    uint64_t        inum;
    struct pid_ns  *parent;     /* NULL == the initial namespace is the parent */
    int             level;      /* 0 == the initial namespace itself */
    int             init_kpid;  /* kpid holding vpid 1 here; 0 until placed */
    int             next_v;     /* rotating allocation hint */
    /* Two directions, both O(1). The vpid space is bounded by PROC_MAX because
     * the whole machine is, so a flat array is both smaller and faster than a
     * hash -- 2 KiB per namespace. */
    int32_t         v_of_k[PROC_MAX];
    int32_t         k_of_v[PROC_MAX];
};

/* The initial pid namespace. Deliberately has NO maps: level 0 is the IDENTITY
 * mapping, short-circuited in every lookup below. That is what makes
 * `proc->pid_ns == NULL` (slice 8's convention) cost nothing and change
 * nothing -- on a system where nobody called CLONE_NEWPID, every translation
 * in this file is `return kpid`. */
static struct pid_ns g_init_pid_ns = {
    .refs = 1, .inum = NS_INUM_INIT_PID, .parent = 0, .level = 0,
    .init_kpid = 0, .next_v = 1, .v_of_k = {0}, .k_of_v = {0},
};

static spinlock_t g_pidns_lock = SPINLOCK_INIT;

static inline struct pid_ns *as_ns(void *p) {
    return p ? (struct pid_ns *)p : &g_init_pid_ns;
}

static inline struct pid_ns *proc_pidns(struct proc *p) {
    return (p && p->pid_ns) ? (struct pid_ns *)p->pid_ns : &g_init_pid_ns;
}

/* ===================================================================
 * refcounting
 * =================================================================== */

void *pid_ns_create(void *parent_ns) {
    struct pid_ns *ns = kmalloc(sizeof(*ns));
    if (!ns) return 0;
    memset(ns, 0, sizeof(*ns));
    struct pid_ns *par = as_ns(parent_ns);
    ns->refs   = 1;
    ns->inum   = ns_inum_alloc();
    ns->level  = par->level + 1;
    ns->next_v = 1;
    /* Store NULL for "the initial namespace is my parent" so the parent chain
     * uses the same NULL-means-initial convention as struct proc. */
    ns->parent = (par == &g_init_pid_ns) ? 0 : par;
    if (ns->parent) pid_ns_get(ns->parent);
    return ns;
}

void pid_ns_get(void *p) {
    struct pid_ns *ns = (struct pid_ns *)p;
    if (!ns || ns == &g_init_pid_ns) return;
    uint64_t f = spin_lock_irqsave(&g_pidns_lock);
    ns->refs++;
    spin_unlock_irqrestore(&g_pidns_lock, f);
}

void pid_ns_put(void *p) {
    struct pid_ns *ns = (struct pid_ns *)p;
    while (ns && ns != &g_init_pid_ns) {
        uint64_t f = spin_lock_irqsave(&g_pidns_lock);
        int left = --ns->refs;
        spin_unlock_irqrestore(&g_pidns_lock, f);
        if (left > 0) return;
        /* Freeing a namespace drops its reference on its parent, which can
         * cascade. Loop rather than recurse: nesting depth is unbounded from
         * userspace, and a recursive put would be a stack-depth attack. */
        struct pid_ns *par = ns->parent;
        kfree(ns);
        ns = par;
    }
}

uint64_t pid_ns_inum(void *p) { return as_ns(p)->inum; }

/* ===================================================================
 * translation
 * =================================================================== */

int pid_vnr_in(void *nsp, int kpid) {
    struct pid_ns *ns = as_ns(nsp);
    if (kpid <= 0 || kpid >= PROC_MAX) return 0;
    if (ns->level == 0) return kpid;            /* identity */
    return (int)ns->v_of_k[kpid];               /* 0 == not visible from here */
}

int pid_vnr_of_kpid(int kpid) {
    return pid_vnr_in(proc_pidns(current_proc()), kpid);
}

int pid_vnr(struct proc *target) {
    if (!target) return 0;
    return pid_vnr_of_kpid(target->pid);
}

int pid_knr(int vpid) {
    struct pid_ns *ns = proc_pidns(current_proc());
    if (vpid <= 0 || vpid >= PROC_MAX) return 0;
    if (ns->level == 0) return vpid;            /* identity */
    return (int)ns->k_of_v[vpid];
}

bool pid_ns_can_see(struct proc *observer, struct proc *target) {
    if (!observer || !target) return false;
    return pid_vnr_in(proc_pidns(observer), target->pid) != 0;
}

bool pid_ns_is_init(struct proc *p) {
    struct pid_ns *ns = proc_pidns(p);
    return ns->level > 0 && ns->init_kpid == p->pid;
}

/* ===================================================================
 * vpid allocation
 *
 * A process is visible in its own namespace and in every ancestor, so a vpid
 * is allocated in each. The initial namespace needs no entry (identity), which
 * is why the loop stops at level 0.
 * =================================================================== */

/* Smallest free vpid at or after the rotating hint. Rotating rather than
 * always-lowest so a ns does not immediately recycle the number of a process
 * that just exited -- the same reason Linux keeps a last_pid hint, and it
 * makes a stale vpid in a log far less likely to look like a live process. */
static int alloc_vpid_locked(struct pid_ns *ns, int kpid, int want) {
    if (want > 0) {                                   /* caller wants vpid 1 */
        if (ns->k_of_v[want]) return 0;
        ns->k_of_v[want] = kpid;
        ns->v_of_k[kpid] = want;
        return want;
    }
    for (int n = 0; n < PROC_MAX - 1; n++) {
        int v = ns->next_v + n;
        if (v >= PROC_MAX) v -= (PROC_MAX - 1);
        if (v < 1) v = 1;
        if (ns->k_of_v[v]) continue;
        ns->k_of_v[v] = kpid;
        ns->v_of_k[kpid] = v;
        ns->next_v = (v + 1 >= PROC_MAX) ? 1 : v + 1;
        return v;
    }
    return 0;
}

static void free_vpid_locked(struct pid_ns *ns, int kpid) {
    if (ns->level == 0) return;
    int v = (int)ns->v_of_k[kpid];
    if (v <= 0 || v >= PROC_MAX) return;
    if (ns->k_of_v[v] == kpid) ns->k_of_v[v] = 0;
    ns->v_of_k[kpid] = 0;
}

/* Register `kpid` in `ns` and every ancestor. `first` asks for vpid 1 in `ns`
 * itself (the namespace's init). All-or-nothing: on failure everything already
 * registered is rolled back, because a process half-visible in an ancestor
 * chain would be reported by procfs and unreachable by kill. */
static long pid_ns_register(struct pid_ns *ns, int kpid, bool first) {
    uint64_t f = spin_lock_irqsave(&g_pidns_lock);
    bool want_one = first;
    for (struct pid_ns *w = ns; w && w->level > 0; w = w->parent) {
        int got = alloc_vpid_locked(w, kpid, want_one ? 1 : 0);
        if (!got) {
            for (struct pid_ns *u = ns; u != w; u = u->parent)
                free_vpid_locked(u, kpid);
            spin_unlock_irqrestore(&g_pidns_lock, f);
            return -ABI_EAGAIN;
        }
        if (want_one) ns->init_kpid = kpid;
        want_one = false;      /* only the innermost ns hands out vpid 1 */
    }
    spin_unlock_irqrestore(&g_pidns_lock, f);
    return 0;
}

void pid_ns_forget(struct proc *p) {
    if (!p) return;
    struct pid_ns *ns = proc_pidns(p);
    if (ns->level == 0) return;                 /* identity: nothing stored */
    uint64_t f = spin_lock_irqsave(&g_pidns_lock);
    for (struct pid_ns *w = ns; w && w->level > 0; w = w->parent)
        free_vpid_locked(w, p->pid);
    spin_unlock_irqrestore(&g_pidns_lock, f);
}

/* ===================================================================
 * placement at fork/clone
 * =================================================================== */

long pid_ns_place_child(struct proc *child, struct proc *parent,
                        uint32_t clone_flags, bool is_thread) {
    if (!child) return -ABI_EINVAL;

    /* A thread joins its leader's namespace and gets its own vpid there (a tid
     * is namespace-local exactly like a pid). CLONE_NEWPID on a thread is
     * already refused at the syscall boundary. */
    if (is_thread) {
        struct pid_ns *ns = proc_pidns(child);
        if (ns->level == 0) return 0;
        return pid_ns_register(ns, child->pid, false);
    }

    struct pid_ns *target;
    bool becomes_init = false;

    if (clone_flags & CLONE_NEWPID) {
        struct pid_ns *base = parent ? proc_pidns(parent) : &g_init_pid_ns;
        void *fresh = pid_ns_create(base);
        if (!fresh) return -ABI_ENOMEM;
        /* Drop the reference memcpy'd from the parent before overwriting. */
        pid_ns_put(child->pid_ns);
        child->pid_ns = fresh;
        /* Its children stay in the same new namespace. Take a second reference:
         * both fields are independently released by nsproxy_release. */
        pid_ns_put(child->pid_ns_for_children);
        child->pid_ns_for_children = fresh;
        pid_ns_get(fresh);
        target = (struct pid_ns *)fresh;
        becomes_init = true;
    } else {
        /* THE unshare(CLONE_NEWPID) CASE: the child goes where the parent said
         * its children should go, which after an unshare is NOT the parent's
         * own namespace. */
        void *want = parent ? parent->pid_ns_for_children : 0;
        target = as_ns(want);
        if (child->pid_ns != want) {
            pid_ns_put(child->pid_ns);
            child->pid_ns = want;
            pid_ns_get(want);
        }
        if (child->pid_ns_for_children != want) {
            pid_ns_put(child->pid_ns_for_children);
            child->pid_ns_for_children = want;
            pid_ns_get(want);
        }
        /* First process to land in a namespace becomes its init. */
        becomes_init = (target->level > 0 && target->init_kpid == 0);
    }

    if (target->level == 0) return 0;           /* initial ns: identity */

    long rc = pid_ns_register(target, child->pid, becomes_init);
    if (rc != 0) return rc;

    kprintf("[ns] pid: kpid=%d -> pid:[%lu] vpid=%d%s\n",
            child->pid, (unsigned long)target->inum,
            pid_vnr_in(target, child->pid), becomes_init ? " (INIT)" : "");
    return 0;
}

/* ===================================================================
 * pid 1 semantics
 * =================================================================== */

void pid_ns_kill_members(struct proc *p) {
    struct pid_ns *ns = proc_pidns(p);
    if (ns->level == 0) return;         /* never tear down the initial ns */

    int killed = 0;
    for (int i = 1; i < PROC_MAX; i++) {
        struct proc *q = &g_proc[i];
        if (q == p) continue;
        if (q->state == PROC_UNUSED || q->state == PROC_EMBRYO ||
            q->state == PROC_TERMINATED) continue;
        /* Members of this namespace AND of any namespace nested inside it --
         * a container's init dying must not leave a nested container running,
         * and the visibility map already encodes exactly that relation. */
        if (!pid_vnr_in(ns, q->pid)) continue;
        signal_send(q, SIGKILL);
        killed++;
    }
    if (killed)
        kprintf("[ns] pid:[%lu] init (kpid=%d) exited -- SIGKILL %d member(s)\n",
                (unsigned long)ns->inum, p->pid, killed);
}

int pid_ns_reaper_kpid(struct proc *dead) {
    if (!dead) return 0;
    struct pid_ns *ns = proc_pidns(dead);
    /* Only namespaced processes get a reaper here. The initial namespace has
     * never reparented orphans in this kernel, and quietly changing that as a
     * side effect of a namespace slice would be a behaviour change nothing
     * asked for -- it is recorded as an open item instead. */
    if (ns->level == 0) return 0;
    if (ns->init_kpid == dead->pid) return 0;      /* init itself is dying */
    return ns->init_kpid;
}
