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
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/proc.h>
#include <tobyos/cap.h>
#include <tobyos/sysprot.h>
#include <tobyos/slog.h>
#include <tobyos/perf.h>

struct vfs_mount {
    char                  point[VFS_PATH_MAX];   /* "/", "/data", ... no trailing '/' (except root) */
    size_t                point_len;             /* strlen(point) */
    const struct vfs_ops *ops;
    void                 *data;
};

static struct vfs_mount g_mounts[VFS_MAX_MOUNTS];
static size_t           g_mount_count;

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

int vfs_mount(const char *mount_point, const struct vfs_ops *ops, void *mount_data) {
    if (!ops || !mount_point) return VFS_ERR_INVAL;
    if (g_mount_count >= VFS_MAX_MOUNTS) return VFS_ERR_NOMEM;

    char norm[VFS_PATH_MAX];
    if (!normalise_mount(mount_point, norm)) return VFS_ERR_INVAL;

    /* Replace if a mount already lives at this exact point (handy for
     * re-mounts during testing -- not used at runtime). */
    for (size_t i = 0; i < g_mount_count; i++) {
        if (strcmp(g_mounts[i].point, norm) == 0) {
            g_mounts[i].ops  = ops;
            g_mounts[i].data = mount_data;
            return VFS_OK;
        }
    }

    struct vfs_mount *m = &g_mounts[g_mount_count++];
    size_t nlen = strlen(norm);
    memcpy(m->point, norm, nlen + 1);
    m->point_len = nlen;
    m->ops       = ops;
    m->data      = mount_data;
    return VFS_OK;
}

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

    if (snap.ops && snap.ops->umount) {
        drv_rc = snap.ops->umount(snap.data);
    }
    kprintf("[vfs] unmounted '%s' (driver rc=%d, %lu mount(s) remaining)\n",
            snap.point, drv_rc, (unsigned long)g_mount_count);
    return drv_rc;
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

int vfs_open(const char *path, struct vfs_file *out) {
    /* Milestone 19: wrap the whole open path in a perf zone. The
     * PERM/sandbox check below short-circuits before the driver call
     * but that still shows up as a sample -- useful for spotting
     * sandbox escape attempts in profiling. */
    uint64_t t_v = perf_rdtsc();
    if (!path || !out) { perf_zone_end(PERF_Z_VFS_OPEN, t_v); return VFS_ERR_INVAL; }
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
    return rc;
}

/* -------- write-side (each returns ROFS if the driver omits the op) -------- */

int vfs_create(const char *path) {
    if (!path) return VFS_ERR_INVAL;
    const char *rel; struct vfs_mount *m = resolve(path, &rel);
    if (!m) return VFS_ERR_NOMOUNT;
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

int vfs_mkdir(const char *path) {
    if (!path) return VFS_ERR_INVAL;
    const char *rel; struct vfs_mount *m = resolve(path, &rel);
    if (!m) return VFS_ERR_NOMOUNT;
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
                         00755u | VFS_MODE_VALID);
}

/* chmod / chown: only the owner or root may change ownership/perms. */
int vfs_chmod(const char *path, uint32_t mode) {
    if (!path) return VFS_ERR_INVAL;
    const char *rel; struct vfs_mount *m = resolve(path, &rel);
    if (!m) return VFS_ERR_NOMOUNT;
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

int vfs_read_all(const char *path, void **out_buf, size_t *out_size) {
    if (out_buf)  *out_buf  = 0;
    if (out_size) *out_size = 0;
    if (!path || !out_buf || !out_size) return VFS_ERR_INVAL;

    struct vfs_stat st;
    int rc = vfs_stat(path, &st);
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
    default:                    return "unknown error";
    }
}
