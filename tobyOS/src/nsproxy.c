/* nsproxy.c -- Linux namespaces: the mechanism.
 *
 * Phase 3 slice 8. See include/tobyos/nsproxy.h for the two design decisions
 * (NULL == initial namespace; per-object refcounts rather than a shared
 * nsproxy) and why they are the right shape for slices 9-12.
 *
 * This file owns:
 *   - `struct uts_ns` (the hostname/domainname payload),
 *   - the nsfs inode-number sequence every namespace type draws from,
 *   - the generic get/put/inum dispatch over namespace kinds,
 *   - unshare(2), setns(2), and the clone(2) CLONE_NEW* application,
 *   - FILE_KIND_NSFD, the openable /proc/PID/ns/<kind> descriptor.
 *
 * It does NOT own the IPC namespace (shm.c, next to the SysV tables) or the
 * mount namespace (vfs.c, since slice 5). Each payload's owner keeps its own
 * namespace struct; this file only ever holds void*.
 */

#include <tobyos/nsproxy.h>
#include <tobyos/proc.h>
#include <tobyos/file.h>
#include <tobyos/shm.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/spinlock.h>
#include <tobyos/seccomp.h>
#include <tobyos/abi/abi.h>

/* ===================================================================
 * nsfs inode numbers
 * =================================================================== */

static uint64_t   g_ns_next_inum = NS_INUM_DYNAMIC_BASE;
static spinlock_t g_ns_lock = SPINLOCK_INIT;

uint64_t ns_inum_alloc(void) {
    uint64_t f = spin_lock_irqsave(&g_ns_lock);
    uint64_t v = g_ns_next_inum++;
    spin_unlock_irqrestore(&g_ns_lock, f);
    return v;
}

/* ===================================================================
 * The UTS namespace
 * =================================================================== */

struct uts_ns {
    int      refs;
    uint64_t inum;
    char     nodename[UTS_FIELD_LEN];
    char     domainname[UTS_FIELD_LEN];
};

/* The initial UTS namespace. Its nodename is "tobyos" and its domainname is
 * "(none)" because those are the exact strings uname(2) has always returned --
 * this slice moves WHERE the values live, and deliberately does not change
 * WHAT an un-unshared process reads. (Syncing the initial nodename with the
 * `system.hostname` setting is a real improvement and a separate change: it
 * would alter uname output for every existing configuration, which is not
 * something a namespace slice should smuggle in.) */
static struct uts_ns g_init_uts_ns = {
    .refs = 1,
    .inum = NS_INUM_INIT_UTS,
    .nodename   = "tobyos",
    .domainname = "(none)",
};

static struct uts_ns *cur_uts_ns(void) {
    struct proc *p = current_proc();
    return (p && p->uts_ns) ? (struct uts_ns *)p->uts_ns : &g_init_uts_ns;
}

/* A new UTS namespace COPIES the creator's hostname. That is what Linux does,
 * and it is the opposite of the IPC namespace (which starts empty) for a
 * reason: a container that boots with an empty hostname is broken, whereas a
 * container that can see the host's shared-memory segments is not isolated. */
static struct uts_ns *uts_ns_clone(struct uts_ns *src) {
    struct uts_ns *ns = kmalloc(sizeof(*ns));
    if (!ns) return 0;
    memset(ns, 0, sizeof(*ns));
    ns->refs = 1;
    ns->inum = ns_inum_alloc();
    memcpy(ns->nodename,   src->nodename,   UTS_FIELD_LEN);
    memcpy(ns->domainname, src->domainname, UTS_FIELD_LEN);
    return ns;
}

static void uts_ns_get(void *p) {
    struct uts_ns *ns = (struct uts_ns *)p;
    if (!ns || ns == &g_init_uts_ns) return;
    uint64_t f = spin_lock_irqsave(&g_ns_lock);
    ns->refs++;
    spin_unlock_irqrestore(&g_ns_lock, f);
}

static void uts_ns_put(void *p) {
    struct uts_ns *ns = (struct uts_ns *)p;
    if (!ns || ns == &g_init_uts_ns) return;
    uint64_t f = spin_lock_irqsave(&g_ns_lock);
    int left = --ns->refs;
    spin_unlock_irqrestore(&g_ns_lock, f);
    if (left <= 0) kfree(ns);
}

const char *uts_nodename(void)   { return cur_uts_ns()->nodename; }
const char *uts_domainname(void) { return cur_uts_ns()->domainname; }

/* sethostname(2)/setdomainname(2). Linux requires CAP_SYS_ADMIN *in the user
 * namespace that owns the UTS namespace being modified*.
 *
 * Slice 11 enforces the capability, and scopes it with a deliberately
 * conservative proxy for ownership: a caller in a non-initial USER namespace may
 * only set the hostname if it also has its OWN (non-initial) UTS namespace.
 * `struct uts_ns` carries no owner field, so this is a proxy rather than the
 * exact Linux rule -- but it errs the safe way, because the only UTS namespace
 * such a caller can reach without one is the initial (shared) one, and changing
 * the machine's real hostname is precisely what must not be allowed. Root in
 * the initial user namespace is unaffected. */
static long uts_set_field(char *dst, const char *name, size_t len) {
    struct proc *p = current_proc();
    if (!p || !userns_capable(LCAP_SYS_ADMIN)) return -ABI_EPERM;
    if (p->user_ns && !p->uts_ns) {
        kprintf("[userns] sethostname refused: pid=%d is in a user namespace "
                "but shares the initial UTS namespace\n", p->pid);
        return -ABI_EPERM;
    }
    if (!name) return -ABI_EFAULT;
    if (len >= UTS_FIELD_LEN) return -ABI_EINVAL;
    memcpy(dst, name, len);
    dst[len] = '\0';
    return 0;
}

long uts_set_nodename(const char *name, size_t len) {
    return uts_set_field(cur_uts_ns()->nodename, name, len);
}
long uts_set_domainname(const char *name, size_t len) {
    return uts_set_field(cur_uts_ns()->domainname, name, len);
}

/* ===================================================================
 * Generic dispatch over namespace kinds
 *
 * Every kind Linux defines has an entry, so /proc/PID/ns lists the full set
 * and `ns_kind_implemented()` is the single place that says which ones are
 * real. Slices 9-12 flip entries here; nothing else needs to change.
 * =================================================================== */

static const char *const g_ns_names[NS_KIND_COUNT] = {
    [NS_KIND_IPC]  = "ipc",
    [NS_KIND_MNT]  = "mnt",
    [NS_KIND_NET]  = "net",
    [NS_KIND_PID]  = "pid",
    [NS_KIND_USER] = "user",
    [NS_KIND_UTS]  = "uts",
};

/* The CLONE_NEW* bit each kind corresponds to -- used by setns(2), whose
 * nstype argument is a CLONE_NEW* value rather than an index. */
static const uint32_t g_ns_clone_bit[NS_KIND_COUNT] = {
    [NS_KIND_IPC]  = CLONE_NEWIPC,
    [NS_KIND_MNT]  = CLONE_NEWNS,
    [NS_KIND_NET]  = CLONE_NEWNET,
    [NS_KIND_PID]  = CLONE_NEWPID,
    [NS_KIND_USER] = CLONE_NEWUSER,
    [NS_KIND_UTS]  = CLONE_NEWUTS,
};

const char *ns_kind_name(int kind) {
    if (kind < 0 || kind >= NS_KIND_COUNT) return "?";
    return g_ns_names[kind];
}

int ns_kind_from_name(const char *name) {
    if (!name) return -1;
    for (int k = 0; k < NS_KIND_COUNT; k++)
        if (strcmp(name, g_ns_names[k]) == 0) return k;
    return -1;
}

bool ns_kind_implemented(int kind) {
    if (kind < 0 || kind >= NS_KIND_COUNT) return false;
    return (g_ns_clone_bit[kind] & CLONE_NEW_SUPPORTED) != 0;
}

/* The void* a process holds for `kind`, or NULL for "the initial namespace".
 * The unimplemented kinds always report NULL, which is exactly right: every
 * process really is in the one and only namespace of that kind. */
static void *ns_ptr_of(struct proc *p, int kind) {
    if (!p) return 0;
    switch (kind) {
    case NS_KIND_UTS:  return p->uts_ns;
    case NS_KIND_IPC:  return p->ipc_ns;
    case NS_KIND_MNT:  return p->mnt_ns;
    case NS_KIND_PID:  return p->pid_ns;
    case NS_KIND_USER: return p->user_ns;
    case NS_KIND_NET:  return p->net_ns;
    default:           return 0;
    }
}

static void ns_get(int kind, void *ns) {
    switch (kind) {
    case NS_KIND_UTS: uts_ns_get(ns);   break;
    case NS_KIND_IPC: ipc_ns_get(ns);   break;
    case NS_KIND_PID: pid_ns_get(ns);   break;
    case NS_KIND_MNT: mount_ns_get(ns); break;   /* slice 9 */
    case NS_KIND_USER: user_ns_get(ns); break;   /* slice 11 */
    case NS_KIND_NET:  net_ns_get(ns);  break;   /* slice 12 */
    default: break;
    }
}

static void ns_put(int kind, void *ns) {
    switch (kind) {
    case NS_KIND_UTS: uts_ns_put(ns);   break;
    case NS_KIND_IPC: ipc_ns_put(ns);   break;
    case NS_KIND_PID: pid_ns_put(ns);   break;
    case NS_KIND_MNT: mount_ns_put(ns); break;   /* slice 9 */
    case NS_KIND_USER: user_ns_put(ns); break;   /* slice 11 */
    case NS_KIND_NET:  net_ns_put(ns);  break;   /* slice 12 */
    default: break;
    }
}

uint64_t ns_inum_of(struct proc *p, int kind) {
    void *ns = ns_ptr_of(p, kind);
    switch (kind) {
    case NS_KIND_UTS:
        return ns ? ((struct uts_ns *)ns)->inum : NS_INUM_INIT_UTS;
    case NS_KIND_IPC:
        return ipc_ns_inum(ns);
    case NS_KIND_PID:
        return pid_ns_inum(ns);
    case NS_KIND_MNT:
        return mount_ns_inum(ns);
    case NS_KIND_USER:
        return user_ns_inum(ns);
    case NS_KIND_NET:
        return net_ns_inum(ns);
    default:             return 0;
    }
}

/* ===================================================================
 * Process lifecycle
 * =================================================================== */

void nsproxy_fork_inherit(struct proc *child) {
    if (!child) return;
    /* The PCB memcpy already copied the pointers; all that is owed is a
     * reference for each. Kinds with no refcount (see ns_get) are no-ops. */
    for (int k = 0; k < NS_KIND_COUNT; k++)
        ns_get(k, ns_ptr_of(child, k));
    /* pid_ns_for_children is a SECOND pid-namespace pointer that no kind index
     * covers (see proc.h for why it exists), so it needs its own reference. */
    pid_ns_get(child->pid_ns_for_children);
}

void nsproxy_release(struct proc *p) {
    if (!p) return;
    /* NULL the field before dropping the reference, and re-read through the
     * proc each time, so a second call on the same proc is a no-op rather than
     * a double free. Two exit paths (proc_exit_group's direct thread teardown
     * and proc_reap) can both plausibly reach a given slot. */
    void *u = p->uts_ns; p->uts_ns = 0; uts_ns_put(u);
    void *i = p->ipc_ns; p->ipc_ns = 0; ipc_ns_put(i);
    /* Slice 10: drop the vpid mappings BEFORE releasing the namespace, or the
     * last put frees the pid_ns and pid_ns_forget then reads freed memory. */
    pid_ns_forget(p);
    void *pn = p->pid_ns;              p->pid_ns = 0;              pid_ns_put(pn);
    void *pc = p->pid_ns_for_children; p->pid_ns_for_children = 0; pid_ns_put(pc);
    void *mn = p->mnt_ns;              p->mnt_ns = 0;              mount_ns_put(mn);
    void *nn = p->net_ns;              p->net_ns = 0;              net_ns_put(nn);
    seccomp_release(p);   /* slice 13: same teardown funnel */
    /* user_ns LAST: mount_ns_put may consult it (owner bookkeeping). */
    void *un = p->user_ns;             p->user_ns = 0;             user_ns_put(un);
}

long nsproxy_apply_clone_flags(struct proc *child, uint32_t flags) {
    if (!child) return -ABI_EINVAL;
    uint32_t want = flags & CLONE_NEW_ANY;
    if (!want) return 0;

    uint32_t unsupported = want & ~(uint32_t)CLONE_NEW_SUPPORTED;
    if (unsupported) {
        kprintf("[ns] clone: namespace flags 0x%x not implemented -> EINVAL\n",
                (unsigned)unsupported);
        return -ABI_EINVAL;
    }
    /* Same privilege rule as unshare(2) below: CLONE_NEWUSER is free, anything
     * else needs CAP_SYS_ADMIN in the caller's user namespace -- UNLESS
     * CLONE_NEWUSER is part of the SAME request, in which case the user
     * namespace being created here is what authorises the rest.
     *
     * THAT LAST CLAUSE WAS THE COMMENT AND NOT THE CODE. The old test was
     *
     *     if ((want & ~CLONE_NEWUSER) && !userns_capable(LCAP_SYS_ADMIN))
     *
     * which masks NEWUSER out of the set being CHECKED -- it does not grant
     * anything. So an unprivileged `clone(CLONE_NEWUSER|CLONE_NEWPID|
     * CLONE_NEWNET)` was still measured against the PARENT's user namespace
     * and refused with EPERM. That is the only combination unprivileged
     * containers ever use, and it is exactly what Chromium's sandbox issues
     * (flags 0x70000011) once it gets past its credentials probe.
     *
     * ns_unshare() below has always been right, because it INSTALLS the user
     * namespace first and only then tests the capability. Two entry points to
     * one feature, and only one of them correct -- the same shape as the
     * clone/child_stack bug this same boot uncovered.
     *
     * Not an escalation: the child's capabilities are scoped to the namespace
     * it just created (userns_owns_mnt_ns and the gates in syscall.c), and
     * every namespace reachable this way is strictly a REMOVAL of reach -- an
     * empty net namespace, a private pid/uts/ipc table, a mount namespace
     * owned by the new user namespace. */
    struct proc *me = current_proc();
    if (!me) return -ABI_EPERM;
    if (!(want & CLONE_NEWUSER) && (want & ~(uint32_t)CLONE_NEWUSER) &&
        !userns_capable(LCAP_SYS_ADMIN))
        return -ABI_EPERM;

    if (want & CLONE_NEWUTS) {
        struct uts_ns *ns = uts_ns_clone(cur_uts_ns());
        if (!ns) return -ABI_ENOMEM;
        void *old = child->uts_ns;
        child->uts_ns = ns;
        uts_ns_put(old);              /* drop the inherited reference */
    }
    if (want & CLONE_NEWIPC) {
        void *ns = ipc_ns_create();
        if (!ns) return -ABI_ENOMEM;
        void *old = child->ipc_ns;
        child->ipc_ns = ns;
        ipc_ns_put(old);
    }
    /* Slice 11: CLONE_NEWUSER is applied BEFORE CLONE_NEWNS so a mount
     * namespace created in the same clone is OWNED by the new user namespace --
     * which is what makes `clone(CLONE_NEWUSER|CLONE_NEWNS)` usable
     * unprivileged. Same ordering as ns_unshare(). (UTS and IPC above have no
     * owner, so their position relative to this does not matter.) */
    if (want & CLONE_NEWUSER) {
        void *save = me->user_ns;
        void *ns = user_ns_create();          /* parented on the CALLER's ns */
        if (!ns) return -ABI_ENOMEM;
        void *old = child->user_ns;
        child->user_ns = ns;
        user_ns_put(old);
        child->lcap_eff = child->lcap_perm = child->lcap_inh = ~0ull;
        /* mount_ns_create() below reads current_proc()->user_ns to record the
         * owner, and we are running as the PARENT -- so borrow the child's new
         * namespace across that call and restore it immediately. Without this
         * the child's mount namespace would be owned by the parent's user
         * namespace and userns_owns_mnt_ns() would (correctly) refuse the
         * child's own mounts. */
        me->user_ns = ns;
        if (want & CLONE_NEWNS) {
            void *mns = mount_ns_create();
            me->user_ns = save;
            if (!mns) return -ABI_ENOMEM;
            void *oldm = child->mnt_ns;
            child->mnt_ns = mns;
            mount_ns_put(oldm);
        } else {
            me->user_ns = save;
        }
    } else if (want & CLONE_NEWNS) {
        /* mount_ns_create() clones the CALLER's table, and we are running in
         * the parent's context here, which is what the child should inherit. */
        void *ns = mount_ns_create();
        if (!ns) return -ABI_ENOMEM;
        void *old = child->mnt_ns;
        child->mnt_ns = ns;
        mount_ns_put(old);
    }
    if (want & CLONE_NEWNET) {            /* slice 12 */
        void *ns = net_ns_create();
        if (!ns) return -ABI_ENOMEM;
        void *old = child->net_ns;
        child->net_ns = ns;
        net_ns_put(old);
    }
    /* CLONE_NEWPID is deliberately NOT handled here. A pid namespace is not
     * just an object to swap in: the child also needs a vpid allocated in it
     * and in every ancestor, and which namespace it lands in depends on the
     * parent's pid_ns_for_children. All of that lives in pid_ns_place_child(),
     * which every fork path calls right after this. */
    kprintf("[ns] clone: pid=%d new namespaces%s%s%s%s%s%s\n", child->pid,
            (want & CLONE_NEWUTS)  ? " uts"  : "",
            (want & CLONE_NEWIPC)  ? " ipc"  : "",
            (want & CLONE_NEWPID)  ? " pid"  : "",
            (want & CLONE_NEWNS)   ? " mnt"  : "",
            (want & CLONE_NEWUSER) ? " user" : "",
            (want & CLONE_NEWNET)  ? " net"  : "");
    return 0;
}

/* ===================================================================
 * unshare(2)
 * =================================================================== */

long ns_unshare(uint32_t flags) {
    struct proc *p = current_proc();
    if (!p) return -ABI_EINVAL;
    if (flags == 0) return 0;                 /* Linux: a legal no-op */

    /* Refuse EVERY bit we do not implement, including the non-namespace
     * unshare flags (CLONE_FILES, CLONE_FS, CLONE_SYSVSEM). Those would need a
     * shared fd-table / shared-cwd refactor to mean anything, and this project
     * has been burned before by a syscall that accepted a flag and ignored it:
     * the caller then believes it has an isolation guarantee it does not have.
     * -EINVAL is also the literal answer Linux gives when the matching
     * CONFIG_*_NS is compiled out, so it is not merely honest, it is correct. */
    uint32_t bad = flags & ~(uint32_t)CLONE_NEW_SUPPORTED;
    if (bad) {
        kprintf("[ns] unshare(0x%x): unsupported bits 0x%x -> EINVAL\n",
                (unsigned)flags, (unsigned)bad);
        return -ABI_EINVAL;
    }
    /* ---- Slice 11: the privilege rules, and their ORDER ----------------
     *
     * CLONE_NEWUSER requires NO privilege. That is the entire point of user
     * namespaces and what makes rootless containers possible: you get a full
     * capability set INSIDE the new namespace, over nothing that was not
     * already yours.
     *
     * Every OTHER namespace requires CAP_SYS_ADMIN in the caller's user
     * namespace. THE ORDER BELOW IS THE FEATURE: when both are requested the
     * user namespace is created and INSTALLED FIRST, so the capabilities it
     * grants are what authorise the rest. That is exactly how `unshare -Urm`
     * works for a non-root user on Linux, and doing it in the other order would
     * make the combination useless to everyone except root.
     *
     * What stops this being an escalation is that the capabilities are scoped:
     * see userns_owns_mnt_ns() and the gates in syscall.c. A process that
     * unshares ONLY CLONE_NEWUSER can mount nothing real. */
    if (flags & CLONE_NEWUSER) {
        void *new_user = user_ns_create();
        if (!new_user) return -ABI_ENOMEM;
        void *old = p->user_ns;
        p->user_ns = new_user;
        user_ns_put(old);
        /* Full capability set in the namespace just created -- and only there. */
        p->lcap_eff = p->lcap_perm = p->lcap_inh = ~0ull;
        kprintf("[ns] unshare: pid=%d created user:[%lu] (uid %u is now root "
                "INSIDE it; caps are scoped to what it owns)\n", p->pid,
                (unsigned long)ns_inum_of(p, NS_KIND_USER), p->uid);
    }
    if (flags & ~(uint32_t)CLONE_NEWUSER) {
        if (!userns_capable(LCAP_SYS_ADMIN)) return -ABI_EPERM;
    }

    /* Build every new namespace BEFORE installing any, so an OOM partway
     * cannot leave the process half-unshared with no way to report it. */
    struct uts_ns *new_uts = 0;
    void          *new_ipc = 0;
    void          *new_pid = 0;
    if (flags & CLONE_NEWUTS) {
        new_uts = uts_ns_clone(cur_uts_ns());
        if (!new_uts) return -ABI_ENOMEM;
    }
    if (flags & CLONE_NEWIPC) {
        new_ipc = ipc_ns_create();
        if (!new_ipc) { uts_ns_put(new_uts); return -ABI_ENOMEM; }
    }
    if (flags & CLONE_NEWPID) {
        new_pid = pid_ns_create(p->pid_ns);
        if (!new_pid) { uts_ns_put(new_uts); ipc_ns_put(new_ipc);
                        return -ABI_ENOMEM; }
    }
    void *new_mnt = 0;
    if (flags & CLONE_NEWNS) {
        new_mnt = mount_ns_create();
        if (!new_mnt) { uts_ns_put(new_uts); ipc_ns_put(new_ipc);
                        pid_ns_put(new_pid); return -ABI_ENOMEM; }
    }
    void *new_net = 0;
    if (flags & CLONE_NEWNET) {
        new_net = net_ns_create();
        if (!new_net) { uts_ns_put(new_uts); ipc_ns_put(new_ipc);
                        pid_ns_put(new_pid); mount_ns_put(new_mnt);
                        return -ABI_ENOMEM; }
    }

    if (new_uts) { void *old = p->uts_ns; p->uts_ns = new_uts; uts_ns_put(old); }
    if (new_ipc) { void *old = p->ipc_ns; p->ipc_ns = new_ipc; ipc_ns_put(old); }
    if (new_pid) {
        /* THE ASYMMETRY THAT MATTERS: unshare(CLONE_NEWPID) does NOT move the
         * caller. `p->pid_ns` is untouched; only pid_ns_for_children changes,
         * so the caller's NEXT CHILD becomes init of the new namespace while
         * the caller keeps the identity everything else already knows it by.
         * A process cannot change its own pid namespace -- its number there
         * would have to appear from nowhere while every existing holder of the
         * old number carried on using it. This is Linux's behaviour, and the
         * acceptance test asserts the caller's own getpid() is unchanged. */
        void *old = p->pid_ns_for_children;
        p->pid_ns_for_children = new_pid;
        pid_ns_put(old);
    }
    if (new_mnt) {
        /* CONTRAST WITH CLONE_NEWPID ABOVE: a mount namespace DOES move the
         * caller. That is not an inconsistency -- a process's mount namespace is
         * only ever consulted to resolve its own paths, so switching it is well
         * defined, whereas its pid is a name others already hold. */
        void *old = p->mnt_ns;
        p->mnt_ns = new_mnt;
        mount_ns_put(old);
    }
    if (new_net) {
        /* Moves the caller, like the mount namespace. Note what this does NOT
         * do: already-open sockets keep working, because enforcement asks the
         * SOCKET's namespace (latched at creation), not the process's. Linux
         * behaves the same way, and it is what lets a sandbox unshare its
         * network while keeping an inherited socketpair to its parent. */
        void *old = p->net_ns;
        p->net_ns = new_net;
        net_ns_put(old);
    }

    kprintf("[ns] unshare: pid=%d now%s%s%s%s%s (uts=%lu ipc=%lu pid=%lu mnt=%lu"
            "%s)\n", p->pid,
            new_uts ? " uts" : "", new_ipc ? " ipc" : "",
            new_pid ? " pid-for-children" : "", new_mnt ? " mnt" : "",
            new_net ? " net" : "",
            (unsigned long)ns_inum_of(p, NS_KIND_UTS),
            (unsigned long)ns_inum_of(p, NS_KIND_IPC),
            (unsigned long)ns_inum_of(p, NS_KIND_PID),
            (unsigned long)ns_inum_of(p, NS_KIND_MNT),
            new_pid ? " -- caller stays put, children move" : "");
    return 0;
}

/* ===================================================================
 * FILE_KIND_NSFD -- /proc/PID/ns/<kind> as an openable descriptor
 *
 * setns(2) takes an fd, not a path, so the namespace has to be reachable
 * through a `struct file`. The descriptor holds a REFERENCE to the namespace:
 * that is what lets a namespace outlive its last member (the mechanism `ip
 * netns add` uses to pin one) and what makes setns able to re-enter it.
 * =================================================================== */

struct nsfd {
    int   refs;
    int   kind;
    void *ns;          /* NULL == the initial namespace of this kind */
};

struct file *ns_file_open(struct proc *p, int kind) {
    if (kind < 0 || kind >= NS_KIND_COUNT) return 0;
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return 0;
    struct nsfd *n = (struct nsfd *)kmalloc(sizeof(*n));
    if (!n) { kfree(f); return 0; }
    memset(f, 0, sizeof(*f));
    memset(n, 0, sizeof(*n));
    n->refs = 1;
    n->kind = kind;
    n->ns   = ns_ptr_of(p, kind);
    ns_get(kind, n->ns);
    f->kind      = FILE_KIND_NSFD;
    f->nsfd      = n;
    f->o_accmode = 0;                  /* O_RDONLY: an nsfd carries no data */
    return f;
}

void ns_file_clone_ref(struct file *dst, struct file *src) {
    if (!dst || !src) return;
    dst->nsfd = src->nsfd;
    if (dst->nsfd) dst->nsfd->refs++;
}

void ns_file_close(struct file *f) {
    if (!f || !f->nsfd) return;
    struct nsfd *n = f->nsfd;
    f->nsfd = 0;
    if (--n->refs > 0) return;
    ns_put(n->kind, n->ns);            /* the fd's reference on the namespace */
    kfree(n);
}

/* ===================================================================
 * setns(2)
 * =================================================================== */

long ns_setns(struct file *f, int nstype) {
    struct proc *p = current_proc();
    if (!p) return -ABI_EINVAL;
    if (!f || f->kind != FILE_KIND_NSFD || !f->nsfd) return -ABI_EINVAL;

    struct nsfd *n = f->nsfd;
    /* nstype 0 means "whatever kind this fd is"; anything else must match, so
     * a caller that means to enter a net namespace cannot silently be handed a
     * uts one. Note nstype is a CLONE_NEW* BIT, not our kind index. */
    if (nstype != 0 && (uint32_t)nstype != g_ns_clone_bit[n->kind])
        return -ABI_EINVAL;
    if (!ns_kind_implemented(n->kind)) return -ABI_EINVAL;
    if (p->uid != 0) return -ABI_EPERM;

    switch (n->kind) {
    case NS_KIND_UTS: {
        void *old = p->uts_ns;
        p->uts_ns = n->ns;
        uts_ns_get(n->ns);             /* the proc's own reference */
        uts_ns_put(old);
        break;
    }
    case NS_KIND_IPC: {
        void *old = p->ipc_ns;
        p->ipc_ns = n->ns;
        ipc_ns_get(n->ns);
        ipc_ns_put(old);
        break;
    }
    case NS_KIND_MNT: {
        /* Unlike the pid case below, this really does move the caller.
         *
         * Slice 11: a caller in a NON-INITIAL user namespace may only enter a
         * mount namespace its own user namespace owns. Otherwise the full
         * capability set it holds inside its user namespace would become
         * authority over someone else's mounts -- setns would be the way around
         * the guard in userns_owns_mnt_ns(). */
        if (p->user_ns && mount_ns_owner(n->ns) != p->user_ns) {
            kprintf("[ns] setns: pid=%d refused -- mnt ns not owned by this "
                    "user namespace\n", p->pid);
            return -ABI_EPERM;
        }
        void *old = p->mnt_ns;
        p->mnt_ns = n->ns;
        mount_ns_get(n->ns);
        mount_ns_put(old);
        break;
    }
    case NS_KIND_NET: {
        /* Moves the caller. Open sockets are unaffected -- see net_ns.c. */
        void *old = p->net_ns;
        p->net_ns = n->ns;
        net_ns_get(n->ns);
        net_ns_put(old);
        break;
    }
    case NS_KIND_USER: {
        /* Entering a user namespace grants its capability set. Linux refuses
         * when the caller is multithreaded or shares fs/files, because the
         * credentials would change under a sibling's feet; this kernel has no
         * per-thread credential split, so the same hazard is avoided by
         * refusing for a thread outright rather than pretending it is safe. */
        if (p->is_thread) return -ABI_EINVAL;
        void *old = p->user_ns;
        p->user_ns = n->ns;
        user_ns_get(n->ns);
        user_ns_put(old);
        /* Capabilities follow the namespace: full inside a non-initial one,
         * and back to whatever the uid implies in the initial one. */
        if (p->user_ns) p->lcap_eff = p->lcap_perm = p->lcap_inh = ~0ull;
        else if (p->uid != 0) p->lcap_eff = p->lcap_perm = p->lcap_inh = 0;
        break;
    }
    case NS_KIND_PID: {
        /* Same asymmetry as unshare: setns(CLONE_NEWPID) affects the caller's
         * CHILDREN, never the caller. Linux is explicit about this -- setns(2)
         * says a pid namespace change "does not change the caller's own pid
         * namespace", precisely because its pid there cannot be invented after
         * the fact. Reporting success while silently moving nobody would be the
         * lie; moving the children is the documented behaviour. */
        void *old = p->pid_ns_for_children;
        p->pid_ns_for_children = n->ns;
        pid_ns_get(n->ns);
        pid_ns_put(old);
        kprintf("[ns] setns: pid=%d children -> pid:[%lu]\n", p->pid,
                (unsigned long)pid_ns_inum(n->ns));
        return 0;
    }
    default:
        return -ABI_EINVAL;
    }
    kprintf("[ns] setns: pid=%d entered %s:[%lu]\n", p->pid,
            ns_kind_name(n->kind),
            (unsigned long)ns_inum_of(p, n->kind));
    return 0;
}
