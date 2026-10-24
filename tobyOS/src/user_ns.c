/* user_ns.c -- USER namespaces (Phase 3 slice 11).
 *
 * The namespace Chromium's sandbox actually depends on, and the one that makes
 * every other namespace available to an unprivileged process.
 *
 * ---------------------------------------------------------------------------
 * SLICE 2 ALREADY DECIDED THE HARD PART
 *
 * Slice 2's note said it outright: "The stored ids are HOST/initial-namespace
 * (Linux's kuid_t). A user namespace must NOT rewrite them; it interposes a
 * uid_map at the REPORTING boundary (getuid, stat, procfs status) and at ns
 * entry. That is what lets slice 11 land without touching every permission
 * check."
 *
 * That is exactly what this is, and it is the same shape slice 10 used for pids:
 * `p->uid` stays a kuid forever, `vfs_perm_check` / `cap_check_path` / oom
 * scoring / SO_PEERCRED keep comparing kuids, and only the ABI edge translates.
 * Rewriting p->uid instead would have meant auditing ~30 consumers, each of
 * which would then be comparing numbers from different namespaces.
 * ---------------------------------------------------------------------------
 *
 * THE CAPABILITY MODEL, and the escalation it must not allow:
 *
 * Creating a user namespace grants the creator a FULL capability set inside it
 * -- that is the whole point, and it is what lets an unprivileged process go on
 * to create mount/pid/net namespaces. The danger is obvious: those capabilities
 * must not reach back out. Linux's rule is that a privileged operation requires
 * the capability in the user namespace that OWNS the namespace being operated
 * on. So every namespace records its owning user_ns, and `mount(2)` from inside
 * a user namespace is allowed only when the caller's mount namespace is owned by
 * that same user namespace (see userns_owns_mnt_ns). A process that unshares
 * only CLONE_NEWUSER therefore gains nothing over the initial mount namespace,
 * which is precisely the intent.
 */

#include <tobyos/nsproxy.h>
#include <tobyos/proc.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/spinlock.h>
#include <tobyos/abi/abi.h>

/* Linux allows 340 extents per map; real containers use one or two, and the
 * classic unprivileged case ("map just my own id") uses exactly one. */
#define UIDMAP_MAX 5

/* Linux's overflowuid/overflowgid: what an unmapped id reports as. Reporting
 * this rather than the raw kuid is load-bearing -- it is what stops an
 * unmapped host id leaking into the namespace. */
#define OVERFLOW_ID 65534u

struct id_extent {
    uint32_t first;   /* first id INSIDE the namespace */
    uint32_t lower;   /* the kid it maps to OUTSIDE */
    uint32_t count;
};

struct user_ns {
    int              refs;
    uint64_t         inum;
    struct user_ns  *parent;        /* NULL == child of the initial ns */
    int              level;         /* 0 == initial */
    uint32_t         owner_kuid;    /* creator's kuid -- owns this namespace */
    uint32_t         owner_kgid;
    struct id_extent umap[UIDMAP_MAX]; int n_umap;
    struct id_extent gmap[UIDMAP_MAX]; int n_gmap;
    bool             setgroups_allowed;
    /* Was the CREATOR privileged in the PARENT namespace (host uid 0)?
     *
     * This has to be recorded at creation time and cannot be re-derived later,
     * which is subtle enough to be worth stating: creating a user namespace sets
     * the creator's capability set to full IN THE NEW NAMESPACE, so by the time
     * uid_map is written, `userns_capable(CAP_SETUID)` is true for everyone --
     * privileged or not. Using that to authorise an ARBITRARY id map would let
     * any user map any host uid, i.e. become root for real. Linux checks
     * CAP_SETUID in the PARENT namespace; this flag is that check, frozen at the
     * only moment it is still answerable. */
    bool             creator_was_root;
};

/* The initial user namespace: level 0, identity mapping, short-circuited in
 * every translation below so a system that never called CLONE_NEWUSER pays
 * nothing and behaves identically. */
static struct user_ns g_init_user_ns = {
    .refs = 1, .inum = NS_INUM_INIT_USER, .parent = 0, .level = 0,
    .owner_kuid = 0, .owner_kgid = 0,
    .umap = {{0,0,0}}, .n_umap = 0, .gmap = {{0,0,0}}, .n_gmap = 0,
    .setgroups_allowed = true,
};

static spinlock_t g_userns_lock = SPINLOCK_INIT;

static inline struct user_ns *as_uns(void *p) {
    return p ? (struct user_ns *)p : &g_init_user_ns;
}
static inline struct user_ns *proc_userns(struct proc *p) {
    return (p && p->user_ns) ? (struct user_ns *)p->user_ns : &g_init_user_ns;
}

/* ===================================================================
 * lifecycle
 * =================================================================== */

void *user_ns_create(void) {
    struct proc *me = current_proc();
    struct user_ns *par = proc_userns(me);
    struct user_ns *ns = kmalloc(sizeof(*ns));
    if (!ns) return 0;
    memset(ns, 0, sizeof(*ns));
    ns->refs  = 1;
    ns->inum  = ns_inum_alloc();
    ns->level = par->level + 1;
    ns->parent = (par == &g_init_user_ns) ? 0 : par;
    if (ns->parent) user_ns_get(ns->parent);
    ns->owner_kuid = me ? me->uid : 0;
    ns->owner_kgid = me ? me->gid : 0;
    /* Frozen NOW, before the caller's capabilities are widened -- see the field
     * comment for why this cannot be recomputed later. In a namespace below
     * the initial one, "root" is CAP_SETUID there: what root of an `unshare
     * -r` holds, and what lets it map its own 0 onward. */
    ns->creator_was_root = (!me || (par->level == 0 ? me->uid == 0
                                    : (me->lcap_eff & (1ull << LCAP_SETUID)) != 0));
    /* Linux: until uid_map is written, EVERY id reports as the overflow id.
     * n_umap == 0 means exactly that (see userns_k2v), so there is nothing to
     * initialise -- and no accidental identity mapping, which would have handed
     * the namespace the host's id space. setgroups is the parent's: a
     * namespace made inside one that denied it denies it too (Linux copies
     * the flag), or the deny could be undone by nesting. */
    ns->setgroups_allowed = par->setgroups_allowed;
    return ns;
}

void user_ns_get(void *p) {
    struct user_ns *ns = (struct user_ns *)p;
    if (!ns || ns == &g_init_user_ns) return;
    uint64_t f = spin_lock_irqsave(&g_userns_lock);
    ns->refs++;
    spin_unlock_irqrestore(&g_userns_lock, f);
}

void user_ns_put(void *p) {
    struct user_ns *ns = (struct user_ns *)p;
    while (ns && ns != &g_init_user_ns) {
        uint64_t f = spin_lock_irqsave(&g_userns_lock);
        int left = --ns->refs;
        spin_unlock_irqrestore(&g_userns_lock, f);
        if (left > 0) return;
        struct user_ns *par = ns->parent;   /* loop, don't recurse: depth is
                                             * userspace-controlled */
        kfree(ns);
        ns = par;
    }
}

uint64_t user_ns_inum(void *p) { return as_uns(p)->inum; }

/* ===================================================================
 * translation
 * =================================================================== */

static uint32_t map_k2v(const struct id_extent *e, int n, uint32_t kid) {
    for (int i = 0; i < n; i++)
        if (kid >= e[i].lower && kid - e[i].lower < e[i].count)
            return e[i].first + (kid - e[i].lower);
    return OVERFLOW_ID;
}

static uint32_t map_v2k(const struct id_extent *e, int n, uint32_t vid,
                        bool *ok) {
    for (int i = 0; i < n; i++)
        if (vid >= e[i].first && vid - e[i].first < e[i].count) {
            if (ok) *ok = true;
            return e[i].lower + (vid - e[i].first);
        }
    if (ok) *ok = false;
    return OVERFLOW_ID;
}

uint32_t userns_k2v_uid(void *nsp, uint32_t kuid) {
    struct user_ns *ns = as_uns(nsp);
    if (ns->level == 0) return kuid;                  /* identity */
    return map_k2v(ns->umap, ns->n_umap, kuid);
}
uint32_t userns_k2v_gid(void *nsp, uint32_t kgid) {
    struct user_ns *ns = as_uns(nsp);
    if (ns->level == 0) return kgid;
    return map_k2v(ns->gmap, ns->n_gmap, kgid);
}

/* Report `kuid` as the CURRENT process's namespace numbers it. */
uint32_t userns_cur_uid(uint32_t kuid) {
    return userns_k2v_uid(current_proc() ? current_proc()->user_ns : 0, kuid);
}
uint32_t userns_cur_gid(uint32_t kgid) {
    return userns_k2v_gid(current_proc() ? current_proc()->user_ns : 0, kgid);
}

/* Accept an id the CALLER supplied. `*ok` is false when the id has no mapping,
 * which callers must turn into EINVAL rather than silently substituting
 * something -- an unmapped id is not a valid argument, and quietly mapping it
 * to the overflow id would let a caller operate on nobody. */
uint32_t userns_v2k_uid(uint32_t vuid, bool *ok) {
    struct user_ns *ns = proc_userns(current_proc());
    if (ns->level == 0) { if (ok) *ok = true; return vuid; }
    return map_v2k(ns->umap, ns->n_umap, vuid, ok);
}
uint32_t userns_v2k_gid(uint32_t vgid, bool *ok) {
    struct user_ns *ns = proc_userns(current_proc());
    if (ns->level == 0) { if (ok) *ok = true; return vgid; }
    return map_v2k(ns->gmap, ns->n_gmap, vgid, ok);
}

bool userns_is_initial(struct proc *p) { return proc_userns(p)->level == 0; }

/* ===================================================================
 * capabilities
 * =================================================================== */

bool userns_capable(int cap) {
    struct proc *p = current_proc();
    if (!p) return true;                      /* kernel context */
    if (cap < 0 || cap > 63) return false;
    return (p->lcap_eff & (1ull << cap)) != 0;
}

/* Is the caller's user namespace the one that owns its mount namespace?
 *
 * THIS IS THE ESCALATION GUARD. A process that unshares only CLONE_NEWUSER has
 * a full capability set in its new namespace, but its mount namespace is still
 * the initial one -- owned by the initial user namespace -- so mount(2) must
 * refuse. Otherwise "unprivileged user namespace" would be a root exploit:
 * unshare(CLONE_NEWUSER) and then mount whatever you like over the real /.
 */
bool userns_owns_mnt_ns(struct proc *p) {
    if (!p) return true;
    void *uns = p->user_ns;
    if (!uns) return true;                    /* initial user ns: unchanged */
    return mount_ns_owner(p->mnt_ns) == uns;
}

/* ===================================================================
 * /proc/PID/{uid_map,gid_map,setgroups}
 * =================================================================== */

/* KID as namespace NS numbers it, (uint32_t)-1 when NS has no name for it --
 * what Linux's from_kuid() gives a map's reader */
static uint32_t map_k2v_raw(struct user_ns *ns, bool want_uid, uint32_t kid) {
    if (ns->level == 0) return kid;
    const struct id_extent *e = want_uid ? ns->umap : ns->gmap;
    int n = want_uid ? ns->n_umap : ns->n_gmap;
    for (int i = 0; i < n; i++)
        if (kid >= e[i].lower && kid - e[i].lower < e[i].count)
            return e[i].first + (kid - e[i].lower);
    return 0xFFFFFFFFu;
}

int userns_render_map(struct proc *p, bool want_uid, char *buf, size_t cap) {
    struct user_ns *ns = proc_userns(p);
    const struct id_extent *e = want_uid ? ns->umap : ns->gmap;
    int n = want_uid ? ns->n_umap : ns->n_gmap;
    size_t off = 0;
    /* The lower ids are kept as the kernel's (finding 241) and shown as the
     * READER's namespace numbers them -- its parent's, when the reader is in
     * the namespace itself: what Linux's uid_m_show does. */
    struct user_ns *rd = proc_userns(current_proc());
    if (rd == ns) rd = ns->parent ? ns->parent : &g_init_user_ns;

    /* The initial namespace's map is the whole id space, which is what Linux
     * shows for pid 1: "         0          0 4294967295". */
    if (ns->level == 0) {
        const char *s = "         0          0 4294967295\n";
        size_t l = strlen(s);
        if (l >= cap) return 0;
        memcpy(buf, s, l); buf[l] = '\0';
        return (int)l;
    }
    for (int i = 0; i < n; i++) {
        char line[64]; size_t li = 0;
        /* "%10u %10u %10u\n", Linux's columns (finding 241: unpadded) */
        #define PUTU(v) do { char t[16]; int tl = 0; uint32_t x = (v); \
            if (!x) t[tl++] = '0'; \
            while (x) { t[tl++] = (char)('0' + x % 10); x /= 10; } \
            for (int pd = tl; pd < 10; pd++) line[li++] = ' '; \
            while (tl) line[li++] = t[--tl]; } while (0)
        PUTU(e[i].first); line[li++] = ' ';
        PUTU(map_k2v_raw(rd, want_uid, e[i].lower)); line[li++] = ' ';
        PUTU(e[i].count); line[li++] = '\n';
        #undef PUTU
        if (off + li >= cap) break;
        memcpy(buf + off, line, li); off += li;
    }
    buf[off] = '\0';
    return (int)off;
}

int userns_render_setgroups(struct proc *p, char *buf, size_t cap) {
    struct user_ns *ns = proc_userns(p);
    const char *s = ns->setgroups_allowed ? "allow\n" : "deny\n";
    size_t l = strlen(s);
    if (l >= cap) return 0;
    memcpy(buf, s, l); buf[l] = '\0';
    return (int)l;
}

static const char *skip_ws(const char *s, const char *end) {
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    return s;
}
static const char *parse_u32(const char *s, const char *end, uint32_t *out,
                             bool *ok) {
    s = skip_ws(s, end);
    if (s >= end || *s < '0' || *s > '9') { *ok = false; return s; }
    uint64_t v = 0;
    while (s < end && *s >= '0' && *s <= '9') {
        v = v * 10 + (uint64_t)(*s - '0');
        if (v > 0xFFFFFFFFull) { *ok = false; return s; }
        s++;
    }
    *out = (uint32_t)v;
    *ok = true;
    return s;
}

/* Write /proc/PID/{uid_map,gid_map}. Returns bytes consumed or a negative ABI
 * errno.
 *
 * Linux's rules, and the reasons they matter:
 *   - WRITE ONCE. A second write is EPERM. Otherwise a process could widen its
 *     own mapping after dropping privilege.
 *   - Only the namespace's creator side may write it, and it must hold
 *     CAP_SETUID/CAP_SETGID in the PARENT namespace... except for the single
 *     unprivileged case below.
 *   - The UNPRIVILEGED case: one extent mapping exactly one id, whose `lower`
 *     is the writer's own id. This is what makes rootless containers possible,
 *     and it is safe because it grants no id the writer did not already have.
 *   - gid_map additionally requires setgroups to have been denied first when
 *     unprivileged, because supplementary groups can otherwise be used to drop
 *     a group that was restricting access. */
long userns_write_map(struct proc *target, bool want_uid,
                      const char *buf, size_t len) {
    struct proc *me = current_proc();
    if (!target || !me) return -ABI_EINVAL;
    struct user_ns *ns = proc_userns(target);
    if (ns->level == 0) return -ABI_EPERM;        /* the initial map is fixed */

    int *np = want_uid ? &ns->n_umap : &ns->n_gmap;
    struct id_extent *ex = want_uid ? ns->umap : ns->gmap;
    if (*np != 0) return -ABI_EPERM;              /* write once */

    /* Parse "first lower count" lines. */
    struct id_extent tmp[UIDMAP_MAX];
    int n = 0;
    const char *s = buf, *end = buf + len;
    while (s < end && n < UIDMAP_MAX) {
        s = skip_ws(s, end);
        while (s < end && (*s == '\n' || *s == '\r')) { s++; s = skip_ws(s, end); }
        if (s >= end) break;
        bool ok = false;
        uint32_t a, b, c;
        s = parse_u32(s, end, &a, &ok); if (!ok) return -ABI_EINVAL;
        s = parse_u32(s, end, &b, &ok); if (!ok) return -ABI_EINVAL;
        s = parse_u32(s, end, &c, &ok); if (!ok) return -ABI_EINVAL;
        if (c == 0) return -ABI_EINVAL;
        tmp[n].first = a; tmp[n].lower = b; tmp[n].count = c;
        n++;
    }
    if (n == 0) return -ABI_EINVAL;

    /* Each lower range is the PARENT namespace's numbers (finding 241: they
     * were taken as the kernel's, so the root of an `unshare -r` writing
     * "4242 0 1" for a namespace of its own named host uid 0). Kept as the
     * kernel's ids from here on; a range the parent has no single extent
     * for is EPERM, as Linux's map_id_range_down. */
    if (ns->parent) {
        struct user_ns *par = ns->parent;
        const struct id_extent *pe = want_uid ? par->umap : par->gmap;
        int pn = want_uid ? par->n_umap : par->n_gmap;
        for (int i = 0; i < n; i++) {
            int j;
            for (j = 0; j < pn; j++)
                if (tmp[i].lower >= pe[j].first &&
                    (uint64_t)tmp[i].lower + tmp[i].count <=
                    (uint64_t)pe[j].first + pe[j].count)
                    break;
            if (j == pn) return -ABI_EPERM;
            tmp[i].lower = pe[j].lower + (tmp[i].lower - pe[j].first);
        }
    }

    /* Privilege check -- against the PARENT namespace, via the flag frozen at
     * creation. NOT userns_capable(): that reports capabilities in the NEW
     * namespace, which every creator has, and using it here would let any
     * unprivileged process map any host uid and become genuinely root. */
    bool priv = ns->creator_was_root;
    if (!priv) {
        /* The unprivileged single-id case: exactly one extent, count 1, mapping
         * an id the writer already owns. */
        if (n != 1 || tmp[0].count != 1) return -ABI_EPERM;
        uint32_t own = want_uid ? me->uid : me->gid;
        if (tmp[0].lower != own) return -ABI_EPERM;
        if (!want_uid && ns->setgroups_allowed) {
            /* setgroups must be denied first -- see the comment above. */
            return -ABI_EPERM;
        }
    }

    for (int i = 0; i < n; i++) ex[i] = tmp[i];
    *np = n;
    kprintf("[userns] user:[%lu] %s_map written: %d extent(s), "
            "first=%u lower=%u count=%u\n",
            (unsigned long)ns->inum, want_uid ? "uid" : "gid", n,
            tmp[0].first, tmp[0].lower, tmp[0].count);
    return (long)len;
}

long userns_write_setgroups(struct proc *target, const char *buf, size_t len) {
    if (!target) return -ABI_EINVAL;
    struct user_ns *ns = proc_userns(target);
    if (ns->level == 0) return -ABI_EPERM;
    /* Linux's two rules (finding 241: the second was the other way round):
     * once denied, setgroups is never allowed again -- that would reopen the
     * hole denying it closed, in this namespace or one made inside it; and
     * it cannot be denied after gid_map is written, when what the map was
     * allowed to say depended on it. */
    if (len >= 4 && strncmp(buf, "deny", 4) == 0) {
        if (ns->n_gmap != 0) return -ABI_EPERM;
        ns->setgroups_allowed = false;
        return (long)len;
    }
    if (len >= 5 && strncmp(buf, "allow", 5) == 0) {
        if (!ns->setgroups_allowed) return -ABI_EPERM;
        return (long)len;
    }
    return -ABI_EINVAL;
}

bool userns_setgroups_allowed(struct proc *p) {
    return proc_userns(p)->setgroups_allowed;
}

/* May P's namespace hand out supplementary groups at all? Linux's
 * userns_may_setgroups: once gid_map is written, and while setgroups is
 * allowed (finding 241: setgroups(2) asked only "is the host uid 0") */
bool userns_may_setgroups(struct proc *p) {
    struct user_ns *ns = proc_userns(p);
    if (ns->level == 0) return true;
    return ns->n_gmap != 0 && ns->setgroups_allowed;
}

/* Is ANC the user namespace NS, or one it was made inside? What reaching
 * an object NS owns takes, capability in hand (Linux's ns_capable walks
 * the same line) -- NULL is the initial namespace, above every other. */
bool userns_covers(void *anc, void *nsp) {
    struct user_ns *a = as_uns(anc);
    for (struct user_ns *ns = as_uns(nsp); ; ns = ns->parent ? ns->parent : &g_init_user_ns) {
        if (ns == a) return true;
        if (ns == &g_init_user_ns) return false;
    }
}

/* CAP in the caller's user namespace, and that namespace OWNER_NS or one
 * above it: what acting on an object OWNER_NS owns takes (Linux's
 * ns_capable(owner, CAP)). Finding 257: the bare capability was asked,
 * and every process holds all of them in its own `unshare -U`. */
bool userns_capable_over(int cap, void *owner_ns) {
    struct proc *me = current_proc();
    if (!me) return true;                     /* kernel context */
    return userns_capable(cap) && userns_covers(me->user_ns, owner_ns);
}

/* CAP over a file owned by KUID:KGID: the capability in the caller's
 * namespace, and both ids named there (Linux's capable_wrt_inode_uidgid)
 * -- a namespace's root holds nothing over a file of someone it has no
 * name for */
bool userns_capable_wrt_ids(int cap, uint32_t kuid, uint32_t kgid) {
    struct proc *me = current_proc();
    if (!me) return true;
    struct user_ns *ns = proc_userns(me);
    return userns_capable(cap) &&
           map_k2v_raw(ns, true, kuid) != 0xFFFFFFFFu &&
           map_k2v_raw(ns, false, kgid) != 0xFFFFFFFFu;
}

/* May the caller make a user namespace inside its own? Only when that one
 * has names for the caller's uid and gid (Linux's create_user_ns: EPERM
 * otherwise) -- the new namespace's owner must be someone there. */
bool userns_may_create(void) {
    struct proc *me = current_proc();
    struct user_ns *ns = proc_userns(me);
    if (!me || ns->level == 0) return true;
    return map_k2v_raw(ns, true, (uint32_t)me->uid) != 0xFFFFFFFFu &&
           map_k2v_raw(ns, false, (uint32_t)me->gid) != 0xFFFFFFFFu;
}
