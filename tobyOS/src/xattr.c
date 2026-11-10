/* Extended attributes -- the store (Phase E, 2026-08-22; finding 225).
 *
 * Real workloads that hit this: coreutils `cp -a` / `mv` across
 * filesystems (fgetxattr/flistxattr on the source fd, fsetxattr on the
 * destination fd), rsync -X, tar --xattrs, chcon and getfattr/setfattr.
 *
 * TWO HOMES (include/tobyos/xattr.h). tmpfs keeps a node's attributes on
 * the node (an xa_list), as Linux's tmpfs does: hard links share them, a
 * rename keeps them, they die with the node. Every other filesystem gets
 * the PATH STORE below:
 *   - On persistent filesystems (tobyfs /data) attributes survive rename
 *     and unlink correctly WITHIN a boot but are NOT written to disk, so
 *     they vanish on reboot. That is a documented divergence, chosen over
 *     returning ENOTSUP and failing every cp -a onto /data.
 *   - Path keying means identity follows the NAME, not the inode: the
 *     vfs_unlink hook drops attributes and the vfs_rename hook re-keys them
 *     (directory prefixes too), so a hard link there has its own, and an
 *     open-but-unlinked file loses them early.
 *
 * LINUX'S LIMITS: a name of up to 255 bytes, a value of up to 64 KiB, as
 * many as memory holds. The first store had room for 128 attributes in
 * all, 120-byte paths, 63-byte names and 256-byte values, and took the
 * user.* namespace alone -- chcon's security.selinux was ENOTSUP. Which
 * namespaces a caller may touch is the syscall layer's (xattr_policy), as
 * Linux's VFS decides it before a filesystem's handler is asked. */

#include <tobyos/xattr.h>
#include <tobyos/abi/abi.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>

struct xa_ent {
    struct xa_ent *next;
    char          *name;
    uint8_t       *val;
    size_t         vlen;
};

struct xa_list {
    struct xa_ent *first;          /* names descending, Linux tmpfs's order */
};

static struct xa_ent *xa_find(struct xa_list *l, const char *name) {
    for (struct xa_ent *e = l ? l->first : 0; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e;
    return 0;
}

static void xa_ent_free(struct xa_ent *e) {
    if (!e) return;
    kfree(e->name);
    if (e->val) kfree(e->val);
    kfree(e);
}

long xa_list_op(struct xa_list **head, int op, const char *name,
                void *buf, size_t sz, int flags) {
    if (!head) return -ABI_EINVAL;
    struct xa_list *l = *head;
    if (op == XA_LIST) {
        size_t need = 0;
        for (struct xa_ent *e = l ? l->first : 0; e; e = e->next)
            need += strlen(e->name) + 1;
        if (sz == 0) return (long)need;           /* the size, asked */
        if (sz < need) return -ABI_ERANGE;
        size_t off = 0;
        for (struct xa_ent *e = l ? l->first : 0; e; e = e->next) {
            size_t n = strlen(e->name) + 1;
            memcpy((char *)buf + off, e->name, n);
            off += n;
        }
        return (long)off;
    }
    if (!name) return -ABI_EINVAL;
    struct xa_ent *e = xa_find(l, name);
    if (op == XA_GET) {
        if (!e) return -ABI_ENODATA;
        if (sz == 0) return (long)e->vlen;        /* the size, asked */
        if (sz < e->vlen) return -ABI_ERANGE;
        if (e->vlen) memcpy(buf, e->val, e->vlen);
        return (long)e->vlen;
    }
    if (op == XA_REMOVE) {
        if (!e) return -ABI_ENODATA;
        struct xa_ent **pp = &l->first;
        while (*pp != e) pp = &(*pp)->next;
        *pp = e->next;
        xa_ent_free(e);
        return 0;
    }
    if (op != XA_SET) return -ABI_EINVAL;
    if (e && (flags & XA_CREATE))   return -ABI_EEXIST;
    if (!e && (flags & XA_REPLACE)) return -ABI_ENODATA;
    uint8_t *nv = 0;
    if (sz) {
        nv = kmalloc(sz);
        if (!nv) return -ABI_ENOMEM;
        memcpy(nv, buf, sz);
    }
    if (e) {                                     /* a new value, same place */
        if (e->val) kfree(e->val);
        e->val = nv;
        e->vlen = sz;
        return 0;
    }
    if (!l) {
        l = kmalloc(sizeof *l);
        if (!l) { if (nv) kfree(nv); return -ABI_ENOMEM; }
        l->first = 0;
        *head = l;
    }
    e = kmalloc(sizeof *e);
    size_t nl = strlen(name);
    char *nm = e ? kmalloc(nl + 1) : 0;
    if (!e || !nm) {
        if (e) kfree(e);
        if (nv) kfree(nv);
        return -ABI_ENOMEM;
    }
    memcpy(nm, name, nl + 1);
    e->name = nm;
    e->val = nv;
    e->vlen = sz;
    struct xa_ent **pp = &l->first;               /* names descending */
    while (*pp && strcmp((*pp)->name, name) > 0) pp = &(*pp)->next;
    e->next = *pp;
    *pp = e;
    return 0;
}

void xa_list_free(struct xa_list **head) {
    if (!head || !*head) return;
    struct xa_ent *e = (*head)->first;
    while (e) {
        struct xa_ent *nx = e->next;
        xa_ent_free(e);
        e = nx;
    }
    kfree(*head);
    *head = 0;
}

/* ---- the path store ------------------------------------------------- */

struct xa_path {
    struct xa_path *next;
    char           *path;
    struct xa_list *list;
};

static struct xa_path *g_paths;

static struct xa_path *xp_find(const char *kpath) {
    for (struct xa_path *p = g_paths; p; p = p->next)
        if (strcmp(p->path, kpath) == 0) return p;
    return 0;
}

static void xp_unlink(struct xa_path *dead) {
    struct xa_path **pp = &g_paths;
    while (*pp && *pp != dead) pp = &(*pp)->next;
    if (*pp) *pp = dead->next;
    xa_list_free(&dead->list);
    kfree(dead->path);
    kfree(dead);
}

long xattr_store(const char *kpath, int op, const char *name,
                 void *buf, size_t sz, int flags) {
    if (!kpath) return -ABI_EINVAL;
    struct xa_path *p = xp_find(kpath);
    if (!p) {
        if (op == XA_LIST) return 0;
        if (op != XA_SET) return -ABI_ENODATA;
        if (flags & XA_REPLACE) return -ABI_ENODATA;
        p = kmalloc(sizeof *p);
        size_t n = strlen(kpath);
        char *ps = p ? kmalloc(n + 1) : 0;
        if (!p || !ps) {
            if (p) kfree(p);
            return -ABI_ENOMEM;
        }
        memcpy(ps, kpath, n + 1);
        p->path = ps;
        p->list = 0;
        p->next = g_paths;
        g_paths = p;
    }
    long r = xa_list_op(&p->list, op, name, buf, sz, flags);
    if (!p->list || !p->list->first) xp_unlink(p);   /* nothing left there */
    return r;
}

void xattr_forget_path(const char *kpath) {
    if (!kpath) return;
    struct xa_path *p = xp_find(kpath);
    if (p) xp_unlink(p);
}

void xattr_rename_path(const char *oldp, const char *newp) {
    if (!oldp || !newp) return;
    size_t ol = strlen(oldp), nl = strlen(newp);
    xattr_forget_path(newp);                    /* the name replaced */
    for (struct xa_path *p = g_paths; p; p = p->next) {
        /* Exact match (the file itself) or directory-prefix match (a
         * renamed directory carries its children's attributes along). */
        bool exact = strcmp(p->path, oldp) == 0;
        bool child = !exact && strncmp(p->path, oldp, ol) == 0 && p->path[ol] == '/';
        if (!exact && !child) continue;
        size_t tail = exact ? 0 : strlen(p->path + ol);
        char *np = kmalloc(nl + tail + 1);
        if (!np) continue;                      /* kept under the old name */
        memcpy(np, newp, nl);
        memcpy(np + nl, p->path + ol, tail + 1);
        kfree(p->path);
        p->path = np;
    }
}
