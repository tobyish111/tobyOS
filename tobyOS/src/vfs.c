/* vfs.c -- multi-mount VFS dispatch + read/write helpers.
 *
 * The VFS owns a tiny mount table (VFS_MAX_MOUNTS slots). On every
 * call, resolve_mount() walks the table picking the longest prefix
 * match -- so "/data/docs/x" finds the /data mount before falling back
 * to the / mount. The driver only ever sees the path *relative* to
 * its own mount point, e.g. "/docs/x" (always begins with '/').
 *
 * Driver vtables can leave write-side ops NULL; in that case the VFS
 * returns VFS_ERR_ROFS instead of dereferencing a null pointer.
 */

#include <tobyos/vfs.h>
#include <tobyos/ramfs.h>   /* slice 112: ramfs_ops identity + payload borrow */
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/proc.h>
#include <tobyos/cap.h>
#include <tobyos/sysprot.h>
#include <tobyos/slog.h>
#include <tobyos/perf.h>
#include <tobyos/nsproxy.h>   /* ns_inum_alloc, NS_INUM_INIT_MNT (slice 9) */

struct vfs_mount {
    char                  point[VFS_PATH_MAX];   /* "/", "/data", ... no trailing '/' (except root) */
    size_t                point_len;             /* strlen(point) */
    const struct vfs_ops *ops;
    void                 *data;
    uint32_t              flags;   /* VFS_MNT_* -- Linux slice 2 */
    /* Slice 9: propagation. `prop` is VFS_PROP_*; `peer_id` names the peer
     * group a SHARED mount belongs to (0 = none), and `master_id` the group a
     * SLAVE receives events from. Propagation is between NAMESPACES, so these
     * ids are global and are what makes a mount in one namespace findable from
     * another. */
    uint32_t              prop;
    uint32_t              peer_id;
    uint32_t              master_id;
};

/* ---- Linux slice 5: the mount NAMESPACE seam ------------------------------
 *
 * The mount table now lives inside a `struct mount_ns` rather than being a
 * bare global. Today there is exactly one namespace and every process shares
 * it, so behaviour is identical -- this is deliberately a SEAM, not a feature.
 *
 * It is built now because slice 9 (CLONE_NEWNS) only has to allocate a second
 * mount_ns and point a proc at it; doing the flat version first and
 * namespacing later would mean rewriting every resolution site twice.
 *
 * Namespacing is cheap here precisely because tobyOS resolves paths by
 * longest-matching-prefix over a small array rather than by walking a
 * dentry/vfsmount graph: cloning a namespace is a struct copy.
 *
 * The two accessor macros keep the ~29 existing `g_mounts`/`g_mount_count`
 * uses in this file untouched, so the seam adds no churn and no opportunity
 * to miss a site. */
struct mount_ns {
    struct vfs_mount m[VFS_MAX_MOUNTS];
    size_t           count;
    int              refs;      /* namespaces are shared until unshared */
    /* ---- slice 9 ---- */
    uint64_t         inum;      /* nsfs inode number, i.e. the N in mnt:[N] */
    struct mount_ns *next;      /* global registry; see g_mnt_ns_list */
    /* ---- slice 11 ----
     * The USER namespace that created this mount namespace. NULL == the initial
     * user namespace. This is what makes unprivileged mounting safe: holding
     * CAP_SYS_ADMIN in a user namespace only authorises mounting in namespaces
     * that user namespace OWNS, so unshare(CLONE_NEWUSER) alone grants nothing
     * over the real mount table. See userns_owns_mnt_ns(). */
    void            *owner_user_ns;
};

static struct mount_ns g_init_mnt_ns;

/* Slice 9: every namespace, threaded on ->next, initial namespace first.
 *
 * Propagation is the reason this exists. A mount event under a SHARED mount has
 * to reach that mount's peers, and peers live in OTHER namespaces -- so there
 * must be a way to enumerate namespaces from inside a mount call. Linux walks
 * its vfsmount tree instead; we have a flat table per namespace, so a list of
 * namespaces is the equivalent structure. */
static struct mount_ns *g_mnt_ns_list = &g_init_mnt_ns;

/* Peer-group id allocator. 0 means "not in a peer group". */
static uint32_t g_peer_next = 1;

static inline struct mount_ns *cur_mnt_ns(void) {
    struct proc *p = current_proc();
    return (p && p->mnt_ns) ? (struct mount_ns *)p->mnt_ns : &g_init_mnt_ns;
}

#define g_mounts      (cur_mnt_ns()->m)
#define g_mount_count (cur_mnt_ns()->count)

/* Normalise a mount point: ensure leading '/', strip trailing '/'
 * unless it's exactly the root. Writes into out (size VFS_PATH_MAX). */
static bool normalise_mount(const char *in, char *out) {
    if (!in || in[0] != '/') return false;
    size_t n = 0;
    while (in[n]) {
        if (n + 1 >= VFS_PATH_MAX) return false;
        out[n] = in[n];
        n++;
    }
    while (n > 1 && out[n - 1] == '/') n--;
    out[n] = 0;
    return true;
}

/* ===================================================================
 * Slice 9: mount namespaces and propagation
 *
 * Propagation is the only part of this slice with real semantic content, and
 * the only part that could be faked by accepting MS_SHARED and doing nothing.
 * So it is implemented for real: a mount under a SHARED mount appears in that
 * mount's peers, which live in OTHER namespaces, and the acceptance test checks
 * both that it does and that a PRIVATE mount does not.
 *
 * The model maps onto tobyOS's flat longest-prefix table like this:
 *   - a mount's "parent" is the longest-prefix mount strictly above it;
 *   - "peers" are mounts (in any namespace) sharing a peer_id;
 *   - propagating means adding/removing the same table entry in each peer's
 *     namespace.
 * Linux walks a vfsmount tree to do the same thing.
 * =================================================================== */

static struct vfs_mount *ns_find_exact(struct mount_ns *ns, const char *norm) {
    for (size_t i = 0; i < ns->count; i++)
        if (strcmp(ns->m[i].point, norm) == 0) return &ns->m[i];
    return 0;
}

/* The mount `norm` sits under: longest prefix STRICTLY shorter than norm. */
static struct vfs_mount *ns_find_parent(struct mount_ns *ns, const char *norm) {
    struct vfs_mount *best = 0;
    size_t nl = strlen(norm);
    for (size_t i = 0; i < ns->count; i++) {
        struct vfs_mount *m = &ns->m[i];
        if (m->point_len >= nl) continue;
        if (strncmp(norm, m->point, m->point_len) != 0) continue;
        /* "/" is a prefix of everything; otherwise the next char must be '/'
         * so that /datax is not treated as living under /data. */
        if (m->point_len > 1 && norm[m->point_len] != '/') continue;
        if (!best || m->point_len > best->point_len) best = m;
    }
    return best;
}

/* Insert without any propagation or driver call. Returns VFS_OK or an error. */
static int ns_add_raw(struct mount_ns *ns, const char *norm,
                      const struct vfs_ops *ops, void *data, uint32_t flags,
                      uint32_t prop, uint32_t peer_id, uint32_t master_id) {
    struct vfs_mount *ex = ns_find_exact(ns, norm);
    if (!ex) {
        if (ns->count >= VFS_MAX_MOUNTS) return VFS_ERR_NOMEM;
        ex = &ns->m[ns->count++];
        size_t nlen = strlen(norm);
        memcpy(ex->point, norm, nlen + 1);
        ex->point_len = nlen;
    }
    ex->ops       = ops;
    ex->data      = data;
    ex->flags     = flags;
    ex->prop      = prop;
    ex->peer_id   = peer_id;
    ex->master_id = master_id;
    return VFS_OK;
}

/* Remove without calling the driver's umount hook. Propagated copies SHARE the
 * (ops, data) of the original, so the driver must be torn down exactly once --
 * see the shared-data guard in vfs_unmount. */
static void ns_remove_raw(struct mount_ns *ns, const char *norm) {
    for (size_t i = 0; i < ns->count; i++) {
        if (strcmp(ns->m[i].point, norm) != 0) continue;
        for (size_t j = i + 1; j < ns->count; j++) ns->m[j - 1] = ns->m[j];
        ns->count--;
        memset(&ns->m[ns->count], 0, sizeof(ns->m[ns->count]));
        return;
    }
}

/* Is (ops,data) still referenced by any mount in any namespace? Used to decide
 * whether vfs_unmount may call the driver's umount hook.
 *
 * This also closes a PRE-EXISTING hazard: slice 5's MS_BIND already registers a
 * second table entry sharing one (ops,data), so unmounting either end called
 * ops->umount(data) and left the other pointing at torn-down state. */
static bool data_still_referenced(const struct vfs_ops *ops, void *data) {
    for (struct mount_ns *ns = g_mnt_ns_list; ns; ns = ns->next)
        for (size_t i = 0; i < ns->count; i++)
            if (ns->m[i].ops == ops && ns->m[i].data == data) return true;
    return false;
}

/* Propagate a new mount at `norm` outward from `origin`. Returns the peer group
 * the new mount should join (0 when nothing propagated). */
static uint32_t propagate_mount(struct mount_ns *origin, const char *norm,
                                const struct vfs_ops *ops, void *data,
                                uint32_t flags) {
    struct vfs_mount *parent = ns_find_parent(origin, norm);
    if (!parent || parent->prop != VFS_PROP_SHARED || !parent->peer_id) return 0;

    uint32_t group = g_peer_next++;
    int n = 0;
    for (struct mount_ns *ns = g_mnt_ns_list; ns; ns = ns->next) {
        if (ns == origin) continue;
        /* Does this namespace hold a peer (or a slave) of the parent? */
        for (size_t i = 0; i < ns->count; i++) {
            struct vfs_mount *m = &ns->m[i];
            bool is_peer  = (m->peer_id   == parent->peer_id);
            bool is_slave = (m->master_id == parent->peer_id);
            if (!is_peer && !is_slave) continue;
            /* A peer receives a SHARED copy (so it propagates onward); a slave
             * receives a SLAVE copy (receives, never sends). */
            (void)ns_add_raw(ns, norm, ops, data, flags,
                             is_peer ? VFS_PROP_SHARED : VFS_PROP_SLAVE,
                             is_peer ? group : 0,
                             is_peer ? 0 : group);
            n++;
            break;                       /* one copy per namespace */
        }
    }
    if (n) kprintf("[mntns] propagated mount '%s' to %d peer namespace(s) "
                   "(group %u)\n", norm, n, group);
    return group;
}

static void propagate_umount(struct mount_ns *origin, const struct vfs_mount *gone) {
    if (!gone->peer_id && !gone->master_id) return;
    uint32_t g = gone->peer_id ? gone->peer_id : gone->master_id;
    for (struct mount_ns *ns = g_mnt_ns_list; ns; ns = ns->next) {
        if (ns == origin) continue;
        struct vfs_mount *m = ns_find_exact(ns, gone->point);
        if (!m) continue;
        if (m->peer_id != g && m->master_id != g) continue;
        ns_remove_raw(ns, gone->point);
    }
}

/* ---- the namespace object, driven by nsproxy.c ---- */

void *mount_ns_create(void) {
    struct mount_ns *src = cur_mnt_ns();
    struct mount_ns *ns = kmalloc(sizeof(*ns));
    if (!ns) return 0;
    memcpy(ns, src, sizeof(*ns));           /* a namespace CLONES the mounts */
    ns->refs = 1;
    ns->inum = ns_inum_alloc();
    /* Peer relationships across the clone, per Linux:
     *   SHARED  -> the copy is a PEER of the original (same peer_id), which is
     *              exactly what makes propagation survive an unshare;
     *   SLAVE   -> the copy is still a slave of the same master;
     *   PRIVATE -> the copy is independent.
     * Getting the SHARED case wrong (clearing peer_id) would silently turn
     * every propagation test into a no-op that still looked isolated. */
    for (size_t i = 0; i < ns->count; i++) {
        if (ns->m[i].prop == VFS_PROP_PRIVATE ||
            ns->m[i].prop == VFS_PROP_UNBINDABLE) {
            ns->m[i].peer_id = 0;
            ns->m[i].master_id = 0;
        }
    }
    /* Slice 11: record the creator's user namespace as the owner. */
    { struct proc *cp = current_proc();
      ns->owner_user_ns = cp ? cp->user_ns : 0; }
    ns->next = g_mnt_ns_list;
    g_mnt_ns_list = ns;
    kprintf("[mntns] created mnt:[%lu] with %lu mount(s) cloned (owner user ns "
            "%p)\n", (unsigned long)ns->inum, (unsigned long)ns->count,
            ns->owner_user_ns);
    return ns;
}

void *mount_ns_owner(void *p) {
    struct mount_ns *ns = p ? (struct mount_ns *)p : &g_init_mnt_ns;
    return ns->owner_user_ns;
}

void mount_ns_get(void *p) {
    struct mount_ns *ns = (struct mount_ns *)p;
    if (!ns || ns == &g_init_mnt_ns) return;
    ns->refs++;
}

void mount_ns_put(void *p) {
    struct mount_ns *ns = (struct mount_ns *)p;
    if (!ns || ns == &g_init_mnt_ns) return;
    if (--ns->refs > 0) return;
    /* Unlink from the registry before tearing down, so no propagation walk can
     * still reach it. */
    struct mount_ns **pp = &g_mnt_ns_list;
    while (*pp && *pp != ns) pp = &(*pp)->next;
    if (*pp) *pp = ns->next;
    /* Release any filesystem whose last reference was in this namespace. Its
     * mounts are going away with it, so the driver hook is owed exactly here --
     * and only for (ops,data) nothing else still names. */
    for (size_t i = 0; i < ns->count; i++) {
        const struct vfs_ops *ops = ns->m[i].ops;
        void *data = ns->m[i].data;
        ns->m[i].ops = 0;                   /* so the guard below ignores us */
        ns->m[i].data = 0;
        if (ops && ops->umount && !data_still_referenced(ops, data))
            (void)ops->umount(data);
    }
    kprintf("[mntns] destroyed mnt:[%lu]\n", (unsigned long)ns->inum);
    kfree(ns);
}

uint64_t mount_ns_inum(void *p) {
    struct mount_ns *ns = p ? (struct mount_ns *)p : &g_init_mnt_ns;
    if (!ns->inum) ns->inum = NS_INUM_INIT_MNT;   /* lazily stamp the initial ns */
    return ns->inum;
}

/* mount --make-{shared,private,slave,unbindable} [-R]. Linux spells these as a
 * mount(2) call carrying only a propagation flag, so this is not a new syscall. */
int vfs_set_propagation(const char *mount_point, uint32_t prop, bool recursive) {
    char norm[VFS_PATH_MAX];
    if (!normalise_mount(mount_point, norm)) return VFS_ERR_INVAL;
    struct mount_ns *ns = cur_mnt_ns();
    struct vfs_mount *m = ns_find_exact(ns, norm);
    if (!m) return VFS_ERR_NOMOUNT;

    size_t nl = strlen(norm);
    for (size_t i = 0; i < ns->count; i++) {
        struct vfs_mount *t = &ns->m[i];
        if (t != m) {
            if (!recursive) continue;
            /* Under norm? Same prefix rule as ns_find_parent. */
            if (t->point_len <= nl) continue;
            if (strncmp(t->point, norm, nl) != 0) continue;
            if (nl > 1 && t->point[nl] != '/') continue;
        }
        switch (prop) {
        case VFS_PROP_SHARED:
            if (!t->peer_id) t->peer_id = g_peer_next++;
            t->master_id = 0;
            t->prop = VFS_PROP_SHARED;
            break;
        case VFS_PROP_SLAVE:
            /* A shared mount becoming a slave keeps receiving from the group it
             * used to be a peer of -- that is what "slave" means. */
            t->master_id = t->peer_id ? t->peer_id : t->master_id;
            t->peer_id = 0;
            t->prop = VFS_PROP_SLAVE;
            break;
        case VFS_PROP_UNBINDABLE:
            t->peer_id = 0; t->master_id = 0;
            t->prop = VFS_PROP_UNBINDABLE;
            break;
        default:
            t->peer_id = 0; t->master_id = 0;
            t->prop = VFS_PROP_PRIVATE;
            break;
        }
    }
    kprintf("[mntns] '%s' propagation -> %u%s\n", norm, prop,
            recursive ? " (recursive)" : "");
    return VFS_OK;
}

/* True when `mount_point` is an existing mount whose propagation forbids
 * bind-mounting it (MS_UNBINDABLE). */
bool vfs_mount_is_unbindable(const char *mount_point) {
    char norm[VFS_PATH_MAX];
    if (!normalise_mount(mount_point, norm)) return false;
    struct vfs_mount *m = ns_find_exact(cur_mnt_ns(), norm);
    return m && m->prop == VFS_PROP_UNBINDABLE;
}

int vfs_mount_flags(const char *mount_point, const struct vfs_ops *ops,
                    void *mount_data, uint32_t flags) {
    if (!ops || !mount_point) return VFS_ERR_INVAL;
    if (g_mount_count >= VFS_MAX_MOUNTS) return VFS_ERR_NOMEM;

    char norm[VFS_PATH_MAX];
    if (!normalise_mount(mount_point, norm)) return VFS_ERR_INVAL;

    /* Replace if a mount already lives at this exact point (handy for
     * re-mounts during testing -- not used at runtime). */
    for (size_t i = 0; i < g_mount_count; i++) {
        if (strcmp(g_mounts[i].point, norm) == 0) {
            g_mounts[i].ops   = ops;
            g_mounts[i].data  = mount_data;
            g_mounts[i].flags = flags;
            return VFS_OK;
        }
    }

    struct vfs_mount *m = &g_mounts[g_mount_count++];
    size_t nlen = strlen(norm);
    memcpy(m->point, norm, nlen + 1);
    m->point_len = nlen;
    m->ops       = ops;
    m->data      = mount_data;
    m->flags     = flags;
    /* Slice 9: a fresh mount is PRIVATE unless it inherits a peer group by
     * propagating to the peers of the mount it sits under. Note the ordering --
     * propagate_mount() reads the table to find the parent, so the entry above
     * must already exist, and it must not yet claim a peer group. */
    m->prop      = VFS_PROP_PRIVATE;
    m->peer_id   = 0;
    m->master_id = 0;
    uint32_t group = propagate_mount(cur_mnt_ns(), norm, ops, mount_data, flags);
    if (group) {
        /* The new mount and every copy it just produced are peers. */
        m = ns_find_exact(cur_mnt_ns(), norm);   /* re-find: table may have moved */
        if (m) { m->prop = VFS_PROP_SHARED; m->peer_id = group; }
    }
    return VFS_OK;
}

int vfs_mount(const char *mount_point, const struct vfs_ops *ops, void *mount_data) {
    /* Kernel-internal mounts are trusted: flags 0. Only mount(2) (slice 5)
     * will pass anything else, and that is exactly the untrusted path. */
    return vfs_mount_flags(mount_point, ops, mount_data, 0);
}

/* vfs_path_mount_flags() lives just after resolve()'s definition below --
 * it needs the resolver, which is static and defined later in this file. */

bool vfs_is_mounted(void) { return g_mount_count > 0; }

int vfs_unmount(const char *mount_point) {
    if (!mount_point) return VFS_ERR_INVAL;
    char norm[VFS_PATH_MAX];
    if (!normalise_mount(mount_point, norm)) return VFS_ERR_INVAL;

    /* Find the slot. Exact-prefix match -- this is *not* the same as
     * vfs_resolve(), which picks longest-prefix; vfs_unmount only
     * affects the slot you actually mounted at. */
    size_t slot = (size_t)-1;
    for (size_t i = 0; i < g_mount_count; i++) {
        if (strcmp(g_mounts[i].point, norm) == 0) { slot = i; break; }
    }
    if (slot == (size_t)-1) return VFS_ERR_NOMOUNT;

    /* Snapshot before we wipe -- the driver's umount hook may want to
     * inspect ops/data, and we still want to print a clean kernel log
     * after we've shrunk the table. */
    struct vfs_mount snap = g_mounts[slot];
    int drv_rc = VFS_OK;

    /* Compact the table: shift the tail one slot left so the surviving
     * entries stay densely packed. Order matters since longest-prefix
     * resolution scans the array linearly -- preserving registration
     * order keeps the deterministic walk. */
    for (size_t i = slot + 1; i < g_mount_count; i++) {
        g_mounts[i - 1] = g_mounts[i];
    }
    g_mount_count--;
    /* Zero the trailing slot so a stale ops pointer doesn't survive. */
    memset(&g_mounts[g_mount_count], 0, sizeof(g_mounts[g_mount_count]));

    /* Slice 9: remove the peer copies in other namespaces BEFORE deciding
     * whether the driver may be torn down -- they hold references to the same
     * (ops,data). */
    propagate_umount(cur_mnt_ns(), &snap);

    /* Slice 9: only the LAST reference to a filesystem may call its umount
     * hook. Propagated mounts and MS_BIND mounts share one (ops,data) across
     * several table entries -- and bind already did before this slice, so
     * unmounting either end tore the filesystem down under the other. */
    if (snap.ops && snap.ops->umount &&
        !data_still_referenced(snap.ops, snap.data)) {
        drv_rc = snap.ops->umount(snap.data);
    }
    kprintf("[vfs] unmounted '%s' (driver rc=%d, %lu mount(s) remaining)\n",
            snap.point, drv_rc, (unsigned long)g_mount_count);
    return drv_rc;
}

/* ===================================================================
 * pivot_root(new_root, put_old) -- the item slice 5 deliberately left ENOSYS
 *
 * Slice 5 refused to alias this to chroot, correctly: chroot only changes name
 * resolution and leaves the old tree reachable through any fd or any path that
 * escapes, whereas pivot_root MOVES the root mount so the old root is genuinely
 * gone from the namespace. Aliasing them would hand callers a false guarantee.
 *
 * In tobyOS's flat longest-prefix table, "moving the root" is exactly a rewrite
 * of mount POINTS, which is why this needed the mount namespace underneath it
 * and nothing more: the old "/" becomes `put_old`, `new_root` becomes "/", and
 * every path then resolves through the new root's filesystem. That is a real
 * implementation of the semantic, not a redirect.
 *
 * Requires a NON-INITIAL mount namespace. Linux permits it in the initial one
 * (initramfs switch_root relies on that), but here it would rewrite the mount
 * table of the running system with no way back, and every caller that wants it
 * -- a container runtime -- is in its own namespace anyway.
 * =================================================================== */
int vfs_pivot_root(const char *new_root, const char *put_old) {
    struct proc *p = current_proc();
    if (!p || !p->mnt_ns) return VFS_ERR_PERM;    /* initial ns: refused */
    if (!new_root || !put_old) return VFS_ERR_INVAL;

    char nr[VFS_PATH_MAX], po[VFS_PATH_MAX];
    if (!normalise_mount(new_root, nr)) return VFS_ERR_INVAL;
    if (!normalise_mount(put_old, po))  return VFS_ERR_INVAL;
    if (strcmp(nr, "/") == 0) return VFS_ERR_INVAL;  /* must not be the root */

    struct mount_ns *ns = cur_mnt_ns();
    struct vfs_mount *nrm = ns_find_exact(ns, nr);
    if (!nrm) return VFS_ERR_NOMOUNT;             /* new_root must be a mount */
    struct vfs_mount *root = ns_find_exact(ns, "/");
    if (!root) return VFS_ERR_NOMOUNT;

    /* put_old must lie underneath new_root, as Linux requires -- otherwise the
     * old root would land somewhere still reachable from the new one by a
     * shorter path, which defeats the point. */
    size_t nrl = strlen(nr);
    if (strncmp(po, nr, nrl) != 0 || po[nrl] != '/') return VFS_ERR_INVAL;

    /* Rewrite in place. Order matters: stash the two descriptors first, because
     * renaming shifts nothing but re-pointing both entries at once through the
     * live table is easy to get wrong. */
    struct vfs_mount newroot_snap = *nrm;
    struct vfs_mount oldroot_snap = *root;

    /* old "/" -> put_old, WITH THE new_root PREFIX STRIPPED.
     *
     * `put_old` is supplied as a path in the PRE-pivot tree (Linux requires it
     * to be under new_root), but after the pivot new_root IS "/" -- so the old
     * root lands at put_old minus that prefix. Storing `po` verbatim would park
     * it at "/newroot/oldroot", a path that no longer resolves because
     * "/newroot" is not a mount point any more: the old root would be silently
     * unreachable, which looks exactly like a working pivot_root until someone
     * tries to unmount the old root. */
    { const char *rel = po + nrl;            /* nrl = strlen(new_root) */
      size_t l = strlen(rel);
      memcpy(root->point, rel, l + 1); root->point_len = l;
      root->ops = oldroot_snap.ops; root->data = oldroot_snap.data;
      root->flags = oldroot_snap.flags; }
    /* new_root -> "/" */
    { nrm->point[0] = '/'; nrm->point[1] = '\0'; nrm->point_len = 1;
      nrm->ops = newroot_snap.ops; nrm->data = newroot_snap.data;
      nrm->flags = newroot_snap.flags; }

    /* Everything that lived under the old new_root prefix has to follow it up to
     * the new root, or those mounts would dangle at paths that no longer exist. */
    for (size_t i = 0; i < ns->count; i++) {
        struct vfs_mount *t = &ns->m[i];
        if (t == nrm || t == root) continue;
        if (t->point_len <= nrl) continue;
        if (strncmp(t->point, nr, nrl) != 0 || t->point[nrl] != '/') continue;
        char moved[VFS_PATH_MAX];
        size_t tl = strlen(t->point + nrl);
        if (tl >= VFS_PATH_MAX) continue;
        memcpy(moved, t->point + nrl, tl + 1);
        memcpy(t->point, moved, tl + 1);
        t->point_len = tl;
    }

    kprintf("[mntns] pivot_root: '%s' is now / (old / -> '%s') in mnt:[%lu]\n",
            nr, po, (unsigned long)ns->inum);
    return VFS_OK;
}

void vfs_iter_mounts(vfs_mount_walk_cb cb, void *cookie) {
    if (!cb) return;
    /* Snapshot count up-front so a callback that triggers an unmount
     * doesn't make us walk past the (newly-shrunk) end of the table. */
    size_t n = g_mount_count;
    for (size_t i = 0; i < n && i < g_mount_count; i++) {
        if (!cb(g_mounts[i].point, g_mounts[i].ops, g_mounts[i].data, cookie)) {
            return;
        }
    }
}

void vfs_dump_mounts(void) {
    if (g_mount_count == 0) {
        kprintf("[vfs] (no mounts)\n");
        return;
    }
    kprintf("[vfs] mount table (%lu entries):\n",
            (unsigned long)g_mount_count);
    for (size_t i = 0; i < g_mount_count; i++) {
        kprintf("  %-12s -> ops=%p mnt=%p\n",
                g_mounts[i].point,
                (void *)g_mounts[i].ops,
                g_mounts[i].data);
    }
}

/* Pick the mount whose point is the longest prefix of `path`. The
 * returned `relative` always starts with '/' (so the driver sees a
 * familiar absolute-looking path) and never ends with '/'. */
static struct vfs_mount *resolve(const char *path, const char **relative) {
    if (!path || path[0] != '/') return 0;
    struct vfs_mount *best = 0;
    size_t best_len = 0;
    for (size_t i = 0; i < g_mount_count; i++) {
        struct vfs_mount *m = &g_mounts[i];
        size_t pl = m->point_len;
        if (strncmp(path, m->point, pl) != 0) continue;
        /* The character right after the match must be either end-of-
         * string or a '/' -- otherwise "/data" would match "/database". */
        char next = path[pl];
        if (m->point[0] == '/' && pl == 1) {
            /* Root mount always matches; pick if nothing longer hit. */
        } else if (next != 0 && next != '/') {
            continue;
        }
        if (pl >= best_len) {
            best = m;
            best_len = pl;
        }
    }
    if (!best) return 0;
    /* Compute the relative path. Root: hand back the whole path. Sub-
     * mount: skip the prefix; if nothing's left, hand back "/". */
    const char *rel = path + best->point_len;
    if (best->point_len == 1) rel = path;       /* root */
    if (rel[0] == 0) rel = "/";
    *relative = rel;
    return best;
}

/* Linux slice 2: the mount flags a path resolves through. Defined here rather
 * than beside vfs_mount_flags() because it needs the static resolver above. */
int vfs_utimes(const char *path, uint64_t mtime, uint64_t atime) {
    if (!path) return VFS_ERR_INVAL;
    const char *rel; struct vfs_mount *m = resolve(path, &rel);
    if (!m) return VFS_ERR_NOMOUNT;
    if (m->flags & VFS_MNT_RDONLY) return VFS_ERR_ROFS;
    if (!m->ops->utimes) return VFS_ERR_ROFS;   /* fs cannot store times */
    if (!cap_check_path(current_proc(), path, CAP_FILE_WRITE, "vfs_utimes"))
        return VFS_ERR_PERM;
    return m->ops->utimes(m->data, rel, mtime, atime);
}

int vfs_truncate(const char *path, uint64_t length) {
    if (!path) return VFS_ERR_INVAL;
    const char *rel; struct vfs_mount *m = resolve(path, &rel);
    if (!m) return VFS_ERR_NOMOUNT;
    if (m->flags & VFS_MNT_RDONLY) return VFS_ERR_ROFS;
    if (!m->ops->truncate) return VFS_ERR_ROFS;
    if (!cap_check_path(current_proc(), path, CAP_FILE_WRITE, "vfs_truncate"))
        return VFS_ERR_PERM;
    return m->ops->truncate(m->data, rel, length);
}

int vfs_file_truncate(struct vfs_file *f, uint64_t length) {
    if (!f || !f->ops) return VFS_ERR_INVAL;
    if (!f->ops->ftruncate) return VFS_ERR_ROFS;
    int rc = f->ops->ftruncate(f, length);
    if (rc == VFS_OK) f->size = (size_t)length;
    return rc;
}

int vfs_mount_lookup(const char *mount_point,
                     const struct vfs_ops **ops_out, void **data_out) {
    if (!mount_point) return VFS_ERR_INVAL;
    char norm[VFS_PATH_MAX];
    if (!normalise_mount(mount_point, norm)) return VFS_ERR_INVAL;
    for (size_t i = 0; i < g_mount_count; i++) {
        if (strcmp(g_mounts[i].point, norm) == 0) {
            if (ops_out)  *ops_out  = g_mounts[i].ops;
            if (data_out) *data_out = g_mounts[i].data;
            return VFS_OK;
        }
    }
    return VFS_ERR_NOMOUNT;
}

uint32_t vfs_path_mount_flags(const char *path) {
    const char *rel;
    struct vfs_mount *m = resolve(path, &rel);
    return m ? m->flags : 0;
}

/* -------- permission helpers (milestone 15) --------
 *
 * The model is intentionally tiny:
 *   - mode bits without VFS_MODE_VALID  -> "no permission info" -> allow
 *   - current process uid == 0          -> root bypass -> allow
 *   - uid matches owner                  -> use owner bits
 *   - else                                -> use other bits
 *
 * Group bits are stored on disk for forward compatibility but no
 * caller in tobyOS today uses GIDs to grant access. */

static int current_uid(void) {
    struct proc *p = current_proc();
    return p ? p->uid : 0;
}

static int current_gid(void) {
    struct proc *p = current_proc();
    return p ? p->gid : 0;
}

static bool mode_allows(uint32_t mode, uint32_t uid, uint32_t gid, int want) {
    if (!(mode & VFS_MODE_VALID)) return true;        /* legacy */
    int who_uid = current_uid();
    if (who_uid == 0) return true;                    /* root bypass */

    int bits;
    if ((uint32_t)who_uid == uid) {
        /* owner: bits 6..8 */
        bits = (int)((mode >> 6) & 7u);
    } else if ((uint32_t)current_gid() == gid) {
        /* group: bits 3..5 */
        bits = (int)((mode >> 3) & 7u);
    } else {
        /* other: bits 0..2 */
        bits = (int)(mode & 7u);
    }
    return (bits & want) == want;
}

/* Path of `path`'s parent directory. parent="/" if path itself is "/x". */
static int parent_path(const char *path, char *out, size_t out_max) {
    if (!path || path[0] != '/') return VFS_ERR_INVAL;
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] == '/') n--;          /* trim trailing '/' */
    while (n > 0 && path[n - 1] != '/') n--;          /* eat the leaf */
    if (n == 0) {
        if (out_max < 2) return VFS_ERR_INVAL;
        out[0] = '/'; out[1] = '\0';
        return VFS_OK;
    }
    /* Drop the trailing slash unless we're at root. */
    if (n > 1) n--;
    if (n + 1 > out_max) return VFS_ERR_NAMETOOLONG;
    memcpy(out, path, n);
    out[n] = '\0';
    return VFS_OK;
}

int vfs_perm_check(const char *path, int want) {
    if (!path) return VFS_ERR_INVAL;
    /* Root short-circuit: skip the stat entirely. */
    if (current_uid() == 0) return VFS_OK;
    struct vfs_stat st;
    int rc = vfs_stat(path, &st);
    if (rc != VFS_OK) return rc;
    if (mode_allows(st.mode, st.uid, st.gid, want)) return VFS_OK;
    return VFS_ERR_PERM;
}

/* -------- read-side -------- */


#ifdef PATHFAIL_TRACE
/* Name the path behind an ENOENT.
 *
 * The syscall ring records ARGUMENTS, and a path argument is a user pointer --
 * so a log full of `openat a1=29407312 = -2` says a file is missing without
 * saying which. That is the exact wall Chromium's zygote hit
 * (zygote_host_impl_linux.cc:221, "No such file or directory"), and guessing
 * the filename from Chromium's source would be a hypothesis, not a diagnosis.
 *
 * Capped, because a normal boot probes for dozens of absent paths (every
 * ld.so search directory) and an uncapped trace buries the interesting one. */
void pathfail_trace(const char *what, const char *path, int rc) {
    static int n;
    if (rc != VFS_ERR_NOENT) return;
    if (n >= 400) return;
    n++;
    kprintf("[enoent] %s '%s'\n", what, path ? path : "(null)");
}
#endif

int vfs_open(const char *path, struct vfs_file *out) {
    /* Milestone 19: wrap the whole open path in a perf zone. The
     * PERM/sandbox check below short-circuits before the driver call
     * but that still shows up as a sample -- useful for spotting
     * sandbox escape attempts in profiling. */
    uint64_t t_v = perf_rdtsc();
    if (!path || !out) { perf_zone_end(PERF_Z_VFS_OPEN, t_v); return VFS_ERR_INVAL; }

    /* Linux slice 7: FOLLOW SYMLINKS on open.
     *
     * Nothing did before, because until on-disk symlinks existed the only
     * links were procfs's synthetic ones (resolved by their own mount) and a
     * RAM table that path resolution consulted separately. With a real rootfs
     * this is load-bearing: Alpine's ld.so opens /lib/libz.so.1 and
     * /lib/libc.musl-x86_64.so.1, both symlinks, so every dynamically-linked
     * binary needing more than the loader itself fails to start without it.
     *
     * Deliberately NOT done in resolve_user_path(): readlink(2) and
     * symlink(2) must act on the LINK, not its target, and they share that
     * resolver. Following belongs to the operations that mean "the thing the
     * name refers to" -- open being the main one. */
    char followed[VFS_PATH_MAX];
    {
        struct vfs_stat lst;
        if (vfs_stat(path, &lst) == VFS_OK && lst.type == VFS_TYPE_SYMLINK) {
            if (vfs_follow_link(path, followed, sizeof followed) == VFS_OK)
                path = followed;
        }
    }
    const char *rel; struct vfs_mount *m = resolve(path, &rel);
    if (!m) { perf_zone_end(PERF_Z_VFS_OPEN, t_v); return VFS_ERR_NOMOUNT; }
    /* Milestone 18: capability + path-sandbox gate FIRST. Must come
     * before any stat/open call so a sandboxed proc can't even
     * *observe* a file outside its root via probing error codes. */
    if (!cap_check_path(current_proc(), path, CAP_FILE_READ, "vfs_open")) {
        perf_zone_end(PERF_Z_VFS_OPEN, t_v);
        return VFS_ERR_PERM;
    }
    /* Require read permission to open. We don't take an O_WRONLY-style
     * flag in this VFS, so "open" is always at least a read intent. */
    int prc = vfs_perm_check(path, VFS_WANT_READ);
    if (prc != VFS_OK) return prc;
    /* Cache owner + mode on the handle so vfs_write can enforce W
     * without re-stat'ing every call. The driver may overwrite these
     * with the authoritative inode values inside its open hook (it
     * usually has them already in hand). */
    out->ops     = m->ops;
    out->mnt     = m->data;
    out->priv    = 0;
    out->pos     = 0;
    out->size    = 0;
    out->uid     = 0;
    out->gid     = 0;
    out->mode    = 0;
    out->ino     = 0;    /* driver open hook may set a stable inode number */
    /* Milestone 34E: stamp protected-prefix bit on the handle so
     * vfs_write can re-check WITHOUT having to remember the path. */
    out->sysprot = sysprot_is_protected(path);
    {
        struct vfs_stat st;
        if (vfs_stat(path, &st) == VFS_OK) {
            out->uid = st.uid; out->gid = st.gid; out->mode = st.mode;
        }
    }
    if (!m->ops->open) { perf_zone_end(PERF_Z_VFS_OPEN, t_v); return VFS_ERR_INVAL; }
    int rc = m->ops->open(m->data, rel, out);
    perf_zone_end(PERF_Z_VFS_OPEN, t_v);
#ifdef PATHFAIL_TRACE
    pathfail_trace("open", path, rc);
#endif
    return rc;
}

int vfs_close(struct vfs_file *f) {
    if (!f || !f->ops) return VFS_ERR_INVAL;
    int rc = f->ops->close(f);
    f->ops = 0;
    return rc;
}

long vfs_read(struct vfs_file *f, void *buf, size_t n) {
    if (!f || !f->ops || !buf) return VFS_ERR_INVAL;
    if (n == 0) return 0;
    if (!f->ops->read) return VFS_ERR_INVAL;
    /* Milestone 19: one sample per read. For files served entirely out
     * of RAM (initrd / ramfs) this is ~a few microseconds; for tobyfs
     * it's disk-bound and dwarfs every other zone. */
    uint64_t t_v = perf_rdtsc();
    long rc = f->ops->read(f, buf, n);
    perf_zone_end(PERF_Z_VFS_READ, t_v);
    return rc;
}

long vfs_write(struct vfs_file *f, const void *buf, size_t n) {
    if (!f || !f->ops || !buf) return VFS_ERR_INVAL;
    if (n == 0) return 0;
    if (!f->ops->write) return VFS_ERR_ROFS;
    /* Milestone 18: writing requires CAP_FILE_WRITE regardless of
     * who opened the handle. This closes the door on a privileged
     * parent opening a file R/W, then passing the fd to a sandboxed
     * child and having the child write through it. */
    if (!cap_check(current_proc(), CAP_FILE_WRITE, "vfs_write")) {
        return VFS_ERR_PERM;
    }
    /* Milestone 34E: same defence on protected paths. The flag is
     * stamped at open time. We can't print the path here (we don't
     * have it on the handle), but the audit line names the proc and
     * the op so a tail of the audit log is still actionable. */
    if (f->sysprot) {
        struct proc *p = current_proc();
        if (!(p && p->sysprot_priv > 0) && (!p || p->pid != 0 || sysprot_get_test_strict())) {
            SLOG_WARN(SLOG_SUB_SYSPROT,
                      "deny pid=%d uid=%d '%s' op=vfs_write (protected handle)",
                      p ? p->pid : -1, p ? p->uid : -1,
                      p ? p->name : "(kernel)");
            kprintf("[sysprot] deny pid=%d '%s' op=vfs_write (protected handle)\n",
                    p ? p->pid : -1, p ? p->name : "(kernel)");
            return VFS_ERR_PERM;
        }
    }
    /* Enforce W permission off the cached handle metadata. Skip if no
     * MODE_VALID bit (legacy inode) or if running as root. */
    if (current_uid() != 0 && (f->mode & VFS_MODE_VALID)) {
        if (!mode_allows(f->mode, f->uid, f->gid, VFS_WANT_WRITE)) {
            return VFS_ERR_PERM;
        }
    }
    uint64_t t_v = perf_rdtsc();
    long rc = f->ops->write(f, buf, n);
    perf_zone_end(PERF_Z_VFS_WRITE, t_v);
    return rc;
}

int vfs_opendir(const char *path, struct vfs_dir *out) {
    if (!path || !out) return VFS_ERR_INVAL;
    const char *rel; struct vfs_mount *m = resolve(path, &rel);
    if (!m) return VFS_ERR_NOMOUNT;
    /* Milestone 18: cap + sandbox gate. Same rationale as vfs_open. */
    if (!cap_check_path(current_proc(), path, CAP_FILE_READ, "vfs_opendir")) {
        return VFS_ERR_PERM;
    }
    int prc = vfs_perm_check(path, VFS_WANT_READ | VFS_WANT_EXEC);
    if (prc != VFS_OK) return prc;
    out->ops   = m->ops;
    out->mnt   = m->data;
    out->priv  = 0;
    out->index = 0;
    if (!m->ops->opendir) return VFS_ERR_INVAL;
    return m->ops->opendir(m->data, rel, out);
}

int vfs_closedir(struct vfs_dir *d) {
    if (!d || !d->ops) return VFS_ERR_INVAL;
    int rc = d->ops->closedir(d);
    d->ops = 0;
    return rc;
}

int vfs_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    if (!d || !d->ops || !out) return VFS_ERR_INVAL;
    return d->ops->readdir(d, out);
}

int vfs_stat(const char *path, struct vfs_stat *out) {
    uint64_t t_v = perf_rdtsc();
    if (!path || !out) { perf_zone_end(PERF_Z_VFS_STAT, t_v); return VFS_ERR_INVAL; }
    const char *rel; struct vfs_mount *m = resolve(path, &rel);
    if (!m) { perf_zone_end(PERF_Z_VFS_STAT, t_v); return VFS_ERR_NOMOUNT; }
    /* Milestone 18: even existence checks are gated -- otherwise a
     * sandboxed proc could probe the filesystem layout by stat'ing
     * every path and distinguishing NOENT from PERM. */
    if (!cap_check_path(current_proc(), path, CAP_FILE_READ, "vfs_stat")) {
        perf_zone_end(PERF_Z_VFS_STAT, t_v);
        return VFS_ERR_PERM;
    }
    if (!m->ops->stat) { perf_zone_end(PERF_Z_VFS_STAT, t_v); return VFS_ERR_INVAL; }
    int rc = m->ops->stat(m->data, rel, out);
    perf_zone_end(PERF_Z_VFS_STAT, t_v);
#ifdef PATHFAIL_TRACE
    pathfail_trace("stat", path, rc);
#endif
    return rc;
}

/* -------- write-side (each returns ROFS if the driver omits the op) -------- */

int vfs_create(const char *path) {
    if (!path) return VFS_ERR_INVAL;
    const char *rel; struct vfs_mount *m = resolve(path, &rel);
    if (!m) return VFS_ERR_NOMOUNT;
    /* Linux slice 5: a read-only mount refuses mutation. VFS_MNT_RDONLY was
     * defined in slice 2 and passed through by mount(2), but nothing enforced
     * it -- a flag that is accepted and ignored is the same class of lie as a
     * no-op syscall that reports success. */
    if (m->flags & VFS_MNT_RDONLY) return VFS_ERR_ROFS;
    if (!m->ops->create) return VFS_ERR_ROFS;
    if (!cap_check_path(current_proc(), path, CAP_FILE_WRITE, "vfs_create")) {
        return VFS_ERR_PERM;
    }
    /* Milestone 34E: protected-prefix gate. Returns VFS_OK if the
     * path isn't protected or the caller is in a privileged scope;
     * otherwise emits an SLOG_SUB_SYSPROT audit line and denies. */
    {
        int sp = sysprot_check_write(current_proc(), path, "vfs_create");
        if (sp != VFS_OK) return sp;
    }
    /* Need W on the parent directory to add a new entry. */
    char par[VFS_PATH_MAX];
    if (parent_path(path, par, sizeof(par)) == VFS_OK) {
        int prc = vfs_perm_check(par, VFS_WANT_WRITE | VFS_WANT_EXEC);
        if (prc != VFS_OK) return prc;
    }
    /* Default new-file mode: owner-rw + world-r. Owner = current uid.
     * Root spawning during boot leaves uid 0 / gid 0, which is what we
     * want for kernel-installed system files (settings.conf, users). */
    return m->ops->create(m->data, rel,
                          (uint32_t)current_uid(), (uint32_t)current_gid(),
                          00644u | VFS_MODE_VALID);
}

int vfs_unlink(const char *path) {
    if (!path) return VFS_ERR_INVAL;
    const char *rel; struct vfs_mount *m = resolve(path, &rel);
    if (!m) return VFS_ERR_NOMOUNT;
    /* Linux slice 5: a read-only mount refuses mutation. VFS_MNT_RDONLY was
     * defined in slice 2 and passed through by mount(2), but nothing enforced
     * it -- a flag that is accepted and ignored is the same class of lie as a
     * no-op syscall that reports success. */
    if (m->flags & VFS_MNT_RDONLY) return VFS_ERR_ROFS;
    if (!m->ops->unlink) return VFS_ERR_ROFS;
    if (!cap_check_path(current_proc(), path, CAP_FILE_WRITE, "vfs_unlink")) {
        return VFS_ERR_PERM;
    }
    {
        int sp = sysprot_check_write(current_proc(), path, "vfs_unlink");
        if (sp != VFS_OK) return sp;
    }
    /* Need W on the parent directory. (Classic Unix would also let an
     * owner with W on the parent unlink a file they don't own; we
     * follow that.) */
    char par[VFS_PATH_MAX];
    if (parent_path(path, par, sizeof(par)) == VFS_OK) {
        int prc = vfs_perm_check(par, VFS_WANT_WRITE | VFS_WANT_EXEC);
        if (prc != VFS_OK) return prc;
    }
    return m->ops->unlink(m->data, rel);
}

int vfs_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return VFS_ERR_INVAL;
    const char *orel; struct vfs_mount *om = resolve(oldpath, &orel);
    const char *nrel; struct vfs_mount *nm = resolve(newpath, &nrel);
    if (!om || !nm) return VFS_ERR_NOMOUNT;
    /* Linux slice 5: a read-only mount refuses mutation. VFS_MNT_RDONLY was
     * defined in slice 2 and passed through by mount(2), but nothing enforced
     * it -- a flag that is accepted and ignored is the same class of lie as a
     * no-op syscall that reports success. */
    if ((om->flags | nm->flags) & VFS_MNT_RDONLY) return VFS_ERR_ROFS;
    if (om != nm)   return VFS_ERR_INVAL;    /* cross-mount rename (EXDEV) -- n/s */
    if (!om->ops->rename) return VFS_ERR_ROFS;
    /* Need write on both old and new (source is unlinked, dest is created). */
    if (!cap_check_path(current_proc(), oldpath, CAP_FILE_WRITE, "vfs_rename") ||
        !cap_check_path(current_proc(), newpath, CAP_FILE_WRITE, "vfs_rename"))
        return VFS_ERR_PERM;
    {
        int sp = sysprot_check_write(current_proc(), oldpath, "vfs_rename");
        if (sp != VFS_OK) return sp;
        sp = sysprot_check_write(current_proc(), newpath, "vfs_rename");
        if (sp != VFS_OK) return sp;
    }
    return om->ops->rename(om->data, orel, nrel);
}

/* Slice 86: mkdir with the CALLER's mode. This used to hardcode 0755 for
 * every directory ever created, which is not a cosmetic default -- callers
 * that create a private directory and then verify it is private treat the
 * mismatch as a security failure. Full chrome's ProcessSingleton does
 *     CHECK(GetPosixFilePermissions(dir, &mode) && mode == 0700)
 * immediately after mkdir(dir, 0700); reading back 0755 fails that CHECK
 * and takes IMMEDIATE_CRASH() -- the int3/ud2 pair that had been showing
 * up as an unexplained SIGTRAP and exit 191. */
int vfs_mkdir_mode(const char *path, uint32_t mode) {
    if (!path) return VFS_ERR_INVAL;
    const char *rel; struct vfs_mount *m = resolve(path, &rel);
    if (!m) return VFS_ERR_NOMOUNT;
    /* Linux slice 5: a read-only mount refuses mutation. VFS_MNT_RDONLY was
     * defined in slice 2 and passed through by mount(2), but nothing enforced
     * it -- a flag that is accepted and ignored is the same class of lie as a
     * no-op syscall that reports success. */
    if (m->flags & VFS_MNT_RDONLY) return VFS_ERR_ROFS;
    if (!m->ops->mkdir) return VFS_ERR_ROFS;
    if (!cap_check_path(current_proc(), path, CAP_FILE_WRITE, "vfs_mkdir")) {
        return VFS_ERR_PERM;
    }
    {
        int sp = sysprot_check_write(current_proc(), path, "vfs_mkdir");
        if (sp != VFS_OK) return sp;
    }
    char par[VFS_PATH_MAX];
    if (parent_path(path, par, sizeof(par)) == VFS_OK) {
        int prc = vfs_perm_check(par, VFS_WANT_WRITE | VFS_WANT_EXEC);
        if (prc != VFS_OK) return prc;
    }
    return m->ops->mkdir(m->data, rel,
                         (uint32_t)current_uid(), (uint32_t)current_gid(),
                         (mode & 07777u) | VFS_MODE_VALID);
}

int vfs_mkdir(const char *path) {
    return vfs_mkdir_mode(path, 00755u);      /* historical default */
}

/* chmod / chown: only the owner or root may change ownership/perms. */
int vfs_chmod(const char *path, uint32_t mode) {
    if (!path) return VFS_ERR_INVAL;
    const char *rel; struct vfs_mount *m = resolve(path, &rel);
    if (!m) return VFS_ERR_NOMOUNT;
    /* Linux slice 5: a read-only mount refuses mutation. VFS_MNT_RDONLY was
     * defined in slice 2 and passed through by mount(2), but nothing enforced
     * it -- a flag that is accepted and ignored is the same class of lie as a
     * no-op syscall that reports success. */
    if (m->flags & VFS_MNT_RDONLY) return VFS_ERR_ROFS;
    if (!m->ops->chmod) return VFS_ERR_ROFS;
    if (!cap_check_path(current_proc(), path, CAP_FILE_WRITE, "vfs_chmod")) {
        return VFS_ERR_PERM;
    }
    {
        int sp = sysprot_check_write(current_proc(), path, "vfs_chmod");
        if (sp != VFS_OK) return sp;
    }
    if (current_uid() != 0) {
        struct vfs_stat st;
        int rc = vfs_stat(path, &st);
        if (rc != VFS_OK) return rc;
        if ((st.mode & VFS_MODE_VALID) &&
            (uint32_t)current_uid() != st.uid) {
            return VFS_ERR_PERM;
        }
    }
    /* Always set MODE_VALID so the caller can't accidentally "downgrade"
     * to legacy mode and grant world access. */
    return m->ops->chmod(m->data, rel,
                        (mode & VFS_MODE_PERMS) | VFS_MODE_VALID);
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid) {
    if (!path) return VFS_ERR_INVAL;
    const char *rel; struct vfs_mount *m = resolve(path, &rel);
    if (!m) return VFS_ERR_NOMOUNT;
    /* Linux slice 5: a read-only mount refuses mutation. VFS_MNT_RDONLY was
     * defined in slice 2 and passed through by mount(2), but nothing enforced
     * it -- a flag that is accepted and ignored is the same class of lie as a
     * no-op syscall that reports success. */
    if (m->flags & VFS_MNT_RDONLY) return VFS_ERR_ROFS;
    if (!m->ops->chown) return VFS_ERR_ROFS;
    if (!cap_check_path(current_proc(), path, CAP_FILE_WRITE, "vfs_chown")) {
        return VFS_ERR_PERM;
    }
    {
        int sp = sysprot_check_write(current_proc(), path, "vfs_chown");
        if (sp != VFS_OK) return sp;
    }
    /* Only root may change ownership in this milestone. */
    if (current_uid() != 0) return VFS_ERR_PERM;
    return m->ops->chown(m->data, rel, uid, gid);
}

/* -------- helpers -------- */

int vfs_follow_link(const char *path, char *out, size_t outsz) {
    if (!path || !out || outsz == 0) return VFS_ERR_INVAL;

    char scratch[2][VFS_PATH_MAX];
    const char *cur = path;

    for (int depth = 0; ; depth++) {
        struct vfs_stat st;
        int rc = vfs_stat(cur, &st);
        if (rc != VFS_OK) return rc;
        if (st.type != VFS_TYPE_SYMLINK) break;
        if (depth >= 8) return VFS_ERR_INVAL;       /* symlink loop / too deep */
        char *next = scratch[depth & 1];            /* never readlink into `cur` */
        rc = vfs_readlink(cur, next, VFS_PATH_MAX);
        if (rc != VFS_OK) return rc;

        /* A RELATIVE target resolves against the LINK'S OWN DIRECTORY, not
         * against the root. This was missing, so `lib/libc.musl-x86_64.so.1
         * -> ld-musl-x86_64.so.1` resolved to "/ld-musl-x86_64.so.1" and
         * landed on whatever (if anything) sat there -- during the Alpine
         * extraction it hit a directory and tar reported "write: Is a
         * directory".
         *
         * It went unnoticed until now because the only previous symlinks were
         * procfs's synthetic ones and a RAM table, both of which store
         * ABSOLUTE targets. Real distributions use relative targets heavily
         * (Alpine's rootfs is full of them), so this is the first workload
         * that could expose it. */
        /* An ABSOLUTE target is absolute in the CALLER'S root, not the
         * kernel's. Inside a chroot, Alpine's `/bin/sh -> /bin/busybox`
         * otherwise resolves to the HOST's /bin/busybox -- so `su -s /bin/sh`
         * reported "can't execute '/bin/sh': No such file or directory" while
         * the link and its target both plainly existed inside the jail.
         * Re-apply the process root that resolve_user_path() already applied
         * to the link's own path. */
        if (next[0] == '/') {
            struct proc *cp = current_proc();
            if (cp && cp->fs_root[0]) {
                char joined[VFS_PATH_MAX];
                size_t rl = strlen(cp->fs_root), tl = strlen(next);
                if (rl + tl + 1 <= sizeof(joined)) {
                    memcpy(joined, cp->fs_root, rl);
                    memcpy(joined + rl, next, tl + 1);
                    memcpy(next, joined, strlen(joined) + 1);
                }
            }
        }
        if (next[0] != '/') {
            char joined[VFS_PATH_MAX];
            size_t cl = strlen(cur);
            while (cl > 0 && cur[cl - 1] != '/') cl--;   /* keep trailing '/' */
            size_t tl = strlen(next);
            if (cl + tl + 1 > sizeof(joined)) return VFS_ERR_NAMETOOLONG;
            memcpy(joined, cur, cl);
            memcpy(joined + cl, next, tl + 1);
            /* Copy back into the scratch slot so `cur` stays valid next loop. */
            memcpy(next, joined, strlen(joined) + 1);
        }
        cur = next;
    }

    size_t n = strlen(cur);
    if (n >= outsz) return VFS_ERR_NAMETOOLONG;
    memcpy(out, cur, n + 1);
    return VFS_OK;
}

int vfs_read_all(const char *path, void **out_buf, size_t *out_size) {
    if (out_buf)  *out_buf  = 0;
    if (out_size) *out_size = 0;
    if (!path || !out_buf || !out_size) return VFS_ERR_INVAL;

    /* Follow symlinks. POSIX read/exec paths are symlink-transparent (only
     * O_NOFOLLOW/lstat see the link itself), but this helper used to reject
     * anything that wasn't VFS_TYPE_FILE outright. That broke execve of
     * /proc/self/exe -- which procfs correctly reports as a SYMLINK -- with
     * VFS_ERR_ISDIR (-3). Chrome re-execs itself that way to spawn every typed
     * child process (gpu, renderer, utility), so all of them died _exit(127). */
    char realpath_buf[VFS_PATH_MAX];
    int rc = vfs_follow_link(path, realpath_buf, sizeof(realpath_buf));
    if (rc != VFS_OK) return rc;
    path = realpath_buf;

    struct vfs_stat st;
    rc = vfs_stat(path, &st);
    if (rc != VFS_OK) return rc;
    if (st.type != VFS_TYPE_FILE) return VFS_ERR_ISDIR;

    void *buf = kmalloc(st.size + 1);
    if (!buf) return VFS_ERR_NOMEM;

    struct vfs_file f;
    rc = vfs_open(path, &f);
    if (rc != VFS_OK) { kfree(buf); return rc; }

    size_t total = 0;
    while (total < st.size) {
        long got = vfs_read(&f, (uint8_t *)buf + total, st.size - total);
        if (got < 0) { vfs_close(&f); kfree(buf); return (int)got; }
        if (got == 0) break;
        total += (size_t)got;
    }
    vfs_close(&f);

    if (total != st.size) {
        kprintf("[vfs] read_all('%s'): short read %lu/%lu\n",
                path, (unsigned long)total, (unsigned long)st.size);
        kfree(buf);
        return VFS_ERR_IO;
    }
    ((uint8_t *)buf)[st.size] = 0;
    *out_buf  = buf;
    *out_size = st.size;
    return VFS_OK;
}

/* Slice 112: like vfs_read_all, but for a ramfs-backed file hand out a
 * BORROWED pointer into the resident initrd tar instead of a full copy
 * (*out_borrowed = 1; the caller must NOT kfree it). Falls back to the
 * copying vfs_read_all everywhere else (*out_borrowed = 0). All the
 * open-path gates (cap, sandbox, perm) run either way -- the borrow is
 * taken from an OPEN handle. Motivation: execve of chrome's 188 MiB
 * image paid a ~320 ms memcpy under the BKL, five times per bootstrap,
 * for bytes that already sit in RAM. */
int vfs_read_all_ref(const char *path, void **out_buf, size_t *out_size,
                     int *out_borrowed) {
    if (out_borrowed) *out_borrowed = 0;
    if (!path || !out_buf || !out_size) return VFS_ERR_INVAL;

    char realpath_buf[VFS_PATH_MAX];
    int rc = vfs_follow_link(path, realpath_buf, sizeof(realpath_buf));
    if (rc != VFS_OK) return rc;

    struct vfs_file f;
    if (vfs_open(realpath_buf, &f) == VFS_OK) {
        if (f.ops == &ramfs_ops) {
            const void *d = 0; size_t n = 0;
            if (ramfs_file_data(&f, &d, &n) == VFS_OK) {
                vfs_close(&f);
                *out_buf = (void *)d;
                *out_size = n;
                if (out_borrowed) *out_borrowed = 1;
                return VFS_OK;
            }
        }
        vfs_close(&f);
    }
    return vfs_read_all(realpath_buf, out_buf, out_size);
}

int vfs_write_all(const char *path, const void *buf, size_t n) {
    if (!path || !buf) return VFS_ERR_INVAL;
    /* create() on a file that already exists is OK -- we then truncate
     * by re-opening: tobyfs's create() truncates existing files. */
    int rc = vfs_create(path);
    if (rc != VFS_OK && rc != VFS_ERR_EXIST) return rc;
    struct vfs_file f;
    rc = vfs_open(path, &f);
    if (rc != VFS_OK) return rc;
    long w = vfs_write(&f, buf, n);
    vfs_close(&f);
    if (w < 0) return (int)w;
    if ((size_t)w != n) return VFS_ERR_IO;
    return VFS_OK;
}

const char *vfs_strerror(int err) {
    switch (err) {
    case VFS_OK:                return "ok";
    case VFS_ERR_NOENT:         return "no such file or directory";
    case VFS_ERR_NOTDIR:        return "not a directory";
    case VFS_ERR_ISDIR:         return "is a directory";
    case VFS_ERR_NOMEM:         return "out of memory";
    case VFS_ERR_INVAL:         return "invalid argument";
    case VFS_ERR_NOMOUNT:       return "no filesystem mounted at that path";
    case VFS_ERR_IO:            return "i/o error";
    case VFS_ERR_EXIST:         return "file exists";
    case VFS_ERR_ROFS:          return "read-only filesystem";
    case VFS_ERR_NOSPC:         return "no space left on device";
    case VFS_ERR_NAMETOOLONG:   return "name too long";
    case VFS_ERR_PERM:          return "permission denied";
    case VFS_ERR_LOOP:          return "too many levels of symbolic links";
    default:                    return "unknown error";
    }
}

/* -------- symlink layer (M1.5) --------
 *
 * In-kernel symlink table: a simple array of (path, target) pairs.
 * Real filesystems would store symlinks as inodes; this lightweight
 * table covers the VFS layer so procfs /proc/self and user-created
 * symlinks work without modifying every on-disk format. */

#define VFS_MAX_SYMLINKS 128

struct vfs_symlink_entry {
    char path[VFS_PATH_MAX];
    char target[VFS_PATH_MAX];
    bool used;
};

static struct vfs_symlink_entry g_symlinks[VFS_MAX_SYMLINKS];

static struct vfs_symlink_entry *symlink_find(const char *path) {
    for (size_t i = 0; i < VFS_MAX_SYMLINKS; i++) {
        if (g_symlinks[i].used && strcmp(g_symlinks[i].path, path) == 0)
            return &g_symlinks[i];
    }
    return 0;
}

int vfs_symlink(const char *path, const char *target) {
    if (!path || !target) return VFS_ERR_INVAL;
    if (path[0] != '/') return VFS_ERR_INVAL;
    if (strlen(path) >= VFS_PATH_MAX || strlen(target) >= VFS_PATH_MAX)
        return VFS_ERR_NAMETOOLONG;
    if (symlink_find(path)) return VFS_ERR_EXIST;

    /* Linux slice 7: prefer a PERSISTENT, per-filesystem symlink.
     *
     * The table below is global to the machine, capped at VFS_MAX_SYMLINKS,
     * and lives only in RAM. Extracting Alpine's minirootfs (335 symlinks,
     * nearly all busybox applet links) both overflowed it -- `tar: can't
     * create symlink` partway through -- and would have left every link
     * dangling after a reboot. Filesystems that can store a link do; the
     * table remains for those that cannot (the read-only initrd ramfs). */
    {
        const char *rel = 0;
        struct vfs_mount *m = resolve(path, &rel);
        if (m && m->ops && m->ops->symlink) {
            if (m->flags & VFS_MNT_RDONLY) return VFS_ERR_ROFS;
            return m->ops->symlink(m->data, rel, target);
        }
    }

    for (size_t i = 0; i < VFS_MAX_SYMLINKS; i++) {
        if (!g_symlinks[i].used) {
            size_t plen = strlen(path);
            size_t tlen = strlen(target);
            memcpy(g_symlinks[i].path, path, plen + 1);
            memcpy(g_symlinks[i].target, target, tlen + 1);
            g_symlinks[i].used = true;
            return VFS_OK;
        }
    }
    return VFS_ERR_NOSPC;
}

int vfs_readlink(const char *path, char *buf, size_t bufsz) {
    if (!path || !buf || bufsz == 0) return VFS_ERR_INVAL;
    struct vfs_symlink_entry *e = symlink_find(path);
    if (!e) {
        /* B20: not in the static table -- give the owning mount a chance
         * to resolve a dynamically-synthesised symlink (procfs
         * /proc/self, /proc/<pid>/exe). */
        const char *rel = 0;
        struct vfs_mount *m = resolve(path, &rel);
        if (m && m->ops && m->ops->readlink)
            return m->ops->readlink(m->data, rel, buf, bufsz);
        return VFS_ERR_NOENT;
    }
    size_t tlen = strlen(e->target);
    if (tlen >= bufsz) tlen = bufsz - 1;
    memcpy(buf, e->target, tlen);
    buf[tlen] = '\0';
    return VFS_OK;
}

int vfs_resolve_path(const char *path, char *resolved, size_t resolved_sz) {
    if (!path || !resolved || resolved_sz < 2) return VFS_ERR_INVAL;

    char buf_a[VFS_PATH_MAX], buf_b[VFS_PATH_MAX];
    size_t plen = strlen(path);
    if (plen >= VFS_PATH_MAX) return VFS_ERR_NAMETOOLONG;
    memcpy(buf_a, path, plen + 1);

    for (int hops = 0; hops < VFS_SYMLINK_MAX; hops++) {
        struct vfs_symlink_entry *e = symlink_find(buf_a);
        if (!e) {
            /* Check prefix-based symlinks: if any symlink path is a
             * prefix of buf_a, substitute it. E.g. /proc/self/status
             * resolves /proc/self -> /proc/<pid>, yielding
             * /proc/<pid>/status. */
            size_t best_len = 0;
            struct vfs_symlink_entry *best = 0;
            for (size_t i = 0; i < VFS_MAX_SYMLINKS; i++) {
                if (!g_symlinks[i].used) continue;
                size_t sp = strlen(g_symlinks[i].path);
                if (sp > strlen(buf_a)) continue;
                if (strncmp(buf_a, g_symlinks[i].path, sp) != 0) continue;
                char next = buf_a[sp];
                if (next != '\0' && next != '/') continue;
                if (sp > best_len) { best_len = sp; best = &g_symlinks[i]; }
            }
            if (!best) {
                /* No more symlinks to resolve. */
                plen = strlen(buf_a);
                if (plen >= resolved_sz) return VFS_ERR_NAMETOOLONG;
                memcpy(resolved, buf_a, plen + 1);
                return VFS_OK;
            }
            /* Substitute the prefix. */
            const char *tail = buf_a + best_len;
            size_t tlen = strlen(best->target);
            size_t tail_len = strlen(tail);
            if (tlen + tail_len >= VFS_PATH_MAX) return VFS_ERR_NAMETOOLONG;
            memcpy(buf_b, best->target, tlen);
            memcpy(buf_b + tlen, tail, tail_len + 1);
            memcpy(buf_a, buf_b, tlen + tail_len + 1);
            continue;
        }
        /* Exact match: replace with target. */
        plen = strlen(e->target);
        if (plen >= VFS_PATH_MAX) return VFS_ERR_NAMETOOLONG;
        memcpy(buf_a, e->target, plen + 1);
    }
    return VFS_ERR_LOOP;
}

/* ================================================================== *
 * Slice 127: recursive delete, shared by every caller that needs it.
 *
 * `rm` in the kernel shell could remove a file or an EMPTY directory and
 * nothing else, and the GUI terminal (src/term.c, its own much smaller
 * builtin set) had no delete at all -- so a directory TREE could not be
 * removed from this OS by any route. Found the practical way: a corrupt
 * chrome profile at /data/cr2, thousands of files deep, needed clearing.
 *
 * It lives HERE rather than in either shell because both need it and the
 * file manager will too; duplicating a recursive unlink into three command
 * tables is how they drift.
 *
 * Two things this gets right that a naive version does not:
 *
 *  - The readdir cursor is an index into a directory whose entries are
 *    being deleted underneath it, so HOLDING it across an unlink skips
 *    entries. Remove one child, close, reopen, repeat until a full pass
 *    finds nothing left. Slower, correct, obvious.
 *  - Recursion carries ONE path buffer, appended to and truncated on the
 *    way back out. A VFS_PATH_MAX buffer per frame is what would overflow
 *    the kernel stack on a deep tree; depth is capped at VFS_RMTREE_MAX
 *    besides, with a real error rather than a fault.
 *
 * `failed` (optional) receives the path of the first thing that could not
 * be removed, so a caller can report something better than an errno.
 * ================================================================== */
#define VFS_RMTREE_MAX_DEPTH 32

static int vfs_rmtree_walk(char *path, size_t len, size_t cap, int depth,
                           bool force, char *failed, size_t failed_cap) {
    struct vfs_stat st;
    int rc = vfs_stat(path, &st);
    if (rc != VFS_OK)
        return (force && rc == VFS_ERR_NOENT) ? VFS_OK : rc;

    if (st.type == VFS_TYPE_DIR) {
        if (depth >= VFS_RMTREE_MAX_DEPTH) {
            if (failed && failed_cap) {
                size_t n = 0;
                while (path[n] && n + 1 < failed_cap) { failed[n] = path[n]; n++; }
                failed[n] = 0;
            }
            return VFS_ERR_NAMETOOLONG;
        }
        for (;;) {
            struct vfs_dir d;
            if (vfs_opendir(path, &d) != VFS_OK) break;
            struct vfs_dirent ent;
            bool removed_one = false;
            while (vfs_readdir(&d, &ent) == VFS_OK) {
                if (ent.name[0] == '.' &&
                    (ent.name[1] == 0 ||
                     (ent.name[1] == '.' && ent.name[2] == 0)))
                    continue;
                size_t nlen = 0;
                while (ent.name[nlen]) nlen++;
                if (len + 1 + nlen + 1 > cap) continue;      /* skip: too long */
                size_t save = len;
                if (len == 0 || path[len - 1] != '/') path[len++] = '/';
                for (size_t i = 0; i < nlen; i++) path[len + i] = ent.name[i];
                len += nlen;
                path[len] = 0;

                int sub = vfs_rmtree_walk(path, len, cap, depth + 1, force,
                                          failed, failed_cap);
                len = save; path[len] = 0;
                if (sub != VFS_OK) { vfs_closedir(&d); return sub; }
                removed_one = true;
                break;                                       /* rewind */
            }
            vfs_closedir(&d);
            if (!removed_one) break;
        }
    }

    rc = vfs_unlink(path);
    if (rc != VFS_OK) {
        if (force && rc == VFS_ERR_NOENT) return VFS_OK;
        if (failed && failed_cap) {
            size_t n = 0;
            while (path[n] && n + 1 < failed_cap) { failed[n] = path[n]; n++; }
            failed[n] = 0;
        }
        return rc;
    }
    return VFS_OK;
}

int vfs_rmtree(const char *path, bool force, char *failed, size_t failed_cap) {
    if (!path || !path[0]) return VFS_ERR_INVAL;
    /* Refuse to recurse from the root: it would walk into /proc and /sys and
     * fail confusingly on synthesised nodes the caller never meant to touch. */
    if (path[0] == '/' && path[1] == 0) return VFS_ERR_INVAL;
    char buf[VFS_PATH_MAX];
    size_t len = 0;
    while (path[len] && len + 1 < sizeof buf) { buf[len] = path[len]; len++; }
    if (path[len]) return VFS_ERR_NAMETOOLONG;
    buf[len] = 0;
    if (failed && failed_cap) failed[0] = 0;
    return vfs_rmtree_walk(buf, len, sizeof buf, 0, force, failed, failed_cap);
}
