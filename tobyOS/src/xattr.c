/* Extended attributes -- the user.* namespace (Phase E, 2026-08-22).
 *
 * Real workloads that hit this: coreutils `cp -a` / `mv` across
 * filesystems (fgetxattr/flistxattr on the source fd, fsetxattr on the
 * destination fd), rsync -X, tar --xattrs, and Python's os.getxattr.
 * Before this file existed all twelve syscalls fell into the ENOSYS
 * census and cp -a printed a warning per file.
 *
 * HONESTY CONTRACT. The store is kernel-resident and keyed by canonical
 * VFS path:
 *   - On tmpfs/ramfs this IS Linux semantics -- tmpfs xattrs live in
 *     memory and die with the filesystem.
 *   - On persistent filesystems (tobyfs /data) attributes survive
 *     rename and unlink correctly WITHIN a boot but are NOT written to
 *     disk, so they vanish on reboot. That is a documented divergence,
 *     chosen over the alternative of returning ENOTSUP and failing
 *     every cp -a onto /data.
 *   - Only the user.* namespace accepts writes; security./trusted./
 *     system. return ENOTSUP on set (Linux tmpfs without the matching
 *     LSM behaves the same way) and ENODATA on get.
 *
 * Path keying means identity follows the NAME, not the inode: the
 * vfs_unlink hook drops attributes and the vfs_rename hook re-keys them
 * (including directory-prefix re-keying), so the one observable
 * divergence from inode keying is that an open-but-unlinked file loses
 * its attributes early -- no real consumer reads xattrs through an
 * unlinked descriptor. */

#include <tobyos/xattr.h>
#include <tobyos/abi/abi.h>
#include <tobyos/klibc.h>

#define XA_MAX   128
#define XA_PATH  120
#define XA_NAME   64
#define XA_VAL   256

struct xa_ent {
    bool     used;
    char     path[XA_PATH];
    char     name[XA_NAME];
    uint16_t vlen;
    uint8_t  val[XA_VAL];
};

static struct xa_ent g_xa[XA_MAX];

/* Linux flag values (setxattr's fifth argument). */
#define LXATTR_CREATE  1
#define LXATTR_REPLACE 2

static bool xa_user_ns(const char *name) {
    return name[0] == 'u' && name[1] == 's' && name[2] == 'e' &&
           name[3] == 'r' && name[4] == '.' && name[5] != '\0';
}

static struct xa_ent *xa_find(const char *path, const char *name) {
    for (int i = 0; i < XA_MAX; i++)
        if (g_xa[i].used && strcmp(g_xa[i].path, path) == 0 &&
            strcmp(g_xa[i].name, name) == 0)
            return &g_xa[i];
    return 0;
}

int xattr_set(const char *kpath, const char *name,
              const void *val, size_t sz, int flags) {
    if (!kpath || !name) return -ABI_EINVAL;
    if (!xa_user_ns(name)) return -ABI_ENOTSUP;
    if (strlen(name) >= XA_NAME || strlen(kpath) >= XA_PATH)
        return -ABI_ERANGE;
    if (sz > XA_VAL) return -ABI_E2BIG;

    struct xa_ent *e = xa_find(kpath, name);
    if (e && (flags & LXATTR_CREATE))   return -ABI_EEXIST;
    if (!e && (flags & LXATTR_REPLACE)) return -ABI_ENODATA;
    if (!e) {
        for (int i = 0; i < XA_MAX; i++)
            if (!g_xa[i].used) { e = &g_xa[i]; break; }
        if (!e) return -ABI_ENOSPC;
        memset(e, 0, sizeof *e);
        memcpy(e->path, kpath, strlen(kpath) + 1);
        memcpy(e->name, name, strlen(name) + 1);
        e->used = true;
    }
    e->vlen = (uint16_t)sz;
    if (sz) memcpy(e->val, val, sz);
    return 0;
}

long xattr_get(const char *kpath, const char *name, void *val, size_t sz) {
    if (!kpath || !name) return -ABI_EINVAL;
    struct xa_ent *e = xa_find(kpath, name);
    if (!e) return -ABI_ENODATA;
    if (sz == 0) return e->vlen;          /* size probe */
    if (sz < e->vlen) return -ABI_ERANGE;
    memcpy(val, e->val, e->vlen);
    return e->vlen;
}

long xattr_list(const char *kpath, char *buf, size_t sz) {
    if (!kpath) return -ABI_EINVAL;
    size_t need = 0;
    for (int i = 0; i < XA_MAX; i++)
        if (g_xa[i].used && strcmp(g_xa[i].path, kpath) == 0)
            need += strlen(g_xa[i].name) + 1;
    if (sz == 0) return (long)need;       /* size probe */
    if (sz < need) return -ABI_ERANGE;
    size_t off = 0;
    for (int i = 0; i < XA_MAX; i++)
        if (g_xa[i].used && strcmp(g_xa[i].path, kpath) == 0) {
            size_t n = strlen(g_xa[i].name) + 1;
            memcpy(buf + off, g_xa[i].name, n);
            off += n;
        }
    return (long)off;
}

int xattr_remove(const char *kpath, const char *name) {
    if (!kpath || !name) return -ABI_EINVAL;
    struct xa_ent *e = xa_find(kpath, name);
    if (!e) return -ABI_ENODATA;
    e->used = false;
    return 0;
}

void xattr_forget_path(const char *kpath) {
    if (!kpath) return;
    for (int i = 0; i < XA_MAX; i++)
        if (g_xa[i].used && strcmp(g_xa[i].path, kpath) == 0)
            g_xa[i].used = false;
}

void xattr_rename_path(const char *oldp, const char *newp) {
    if (!oldp || !newp) return;
    size_t ol = strlen(oldp), nl = strlen(newp);
    if (nl >= XA_PATH) return;            /* new name unrepresentable: drop */
    for (int i = 0; i < XA_MAX; i++) {
        if (!g_xa[i].used) continue;
        const char *p = g_xa[i].path;
        /* Exact match (the file itself) or directory-prefix match (a
         * renamed directory carries its children's attributes along). */
        bool exact = strcmp(p, oldp) == 0;
        bool child = !exact && strncmp(p, oldp, ol) == 0 && p[ol] == '/';
        if (!exact && !child) continue;
        size_t tail = exact ? 0 : strlen(p + ol);
        if (nl + tail >= XA_PATH) { g_xa[i].used = false; continue; }
        char tmp[XA_PATH];
        memcpy(tmp, newp, nl);
        memcpy(tmp + nl, p + ol, tail + 1);
        memcpy(g_xa[i].path, tmp, nl + tail + 1);
    }
}
