/* tmpfs -- a WRITABLE in-memory filesystem, mountable at any point.
 *
 * ===========================================================================
 * WHY THIS EXISTS
 *
 * This kernel had no mountable in-memory filesystem at all. ramfs is the
 * initrd: one global instance, backed by a USTAR image, with no write path
 * (`.write = 0, .create = 0, .unlink = 0, .mkdir = 0`). So `mount -t tmpfs`
 * answered ENODEV, and that answer was deliberately kept honest rather than
 * quietly satisfied by mounting something else -- see the fstype switch in
 * syscall.c.
 *
 * The cost of not having it was concrete: slice 16's OCI runtime reports
 * "NOT APPLIED: /dev (tmpfs)" on every container it starts, because the bundle
 * asks for a tmpfs /dev the way every real OCI config does. A container also
 * has no writable /tmp, which an enormous amount of ordinary Linux software
 * simply assumes.
 *
 * ===========================================================================
 * SHAPE, AND WHY IT IS THIS SHAPE
 *
 * A FLAT, PATH-KEYED node table, exactly like ramfs. That is not laziness: the
 * VFS resolves by longest-prefix string match and has no dentry graph, so a
 * tree of inodes would be a structure nothing else here speaks. Flat keeps
 * lookup, readdir and rename expressible in terms the rest of the VFS already
 * uses -- and it is why slice 5 could namespace the mount table with a struct
 * copy.
 *
 * IT IS SIZE-CAPPED, and that is the load-bearing decision. An in-memory
 * filesystem with no limit is a way for any process to consume all of RAM and
 * take the machine down -- `cat /dev/zero > /tmp/x` is enough. Linux caps
 * tmpfs at half of RAM by default for exactly this reason. The cap here is per
 * mount, defaults to TMPFS_DEFAULT_BYTES, and is settable by the standard
 * `size=` mount option; exceeding it is ENOSPC, which is a state callers
 * already handle.
 *
 * `size=` IS PARSED rather than accepted and ignored. A mount that silently
 * gives you a different size than you asked for is the same class of lie as a
 * flag that is stored and never enforced, which this arc has now found in
 * VFS_MNT_RDONLY, PR_SET_NO_NEW_PRIVS and io.max.
 * ===========================================================================
 */
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* Same source tobyfs uses for a wall-clock timestamp (see tfs_now_secs). */
static uint64_t tmpfs_now_secs(void) {
    extern uint64_t lx_realtime_ns(uint64_t mono_ns);
    extern uint64_t perf_now_ns(void);
    return lx_realtime_ns(perf_now_ns()) / 1000000000ull;
}

/* klibc deliberately provides only strlen/strcmp/strncmp and the mem* family,
 * so the handful of string operations below are local. Same constraint ramfs
 * works under; keeping it means this file adds no new libc surface. */
static const char *tchr(const char *s, char c)
{
    for (; *s; s++) if (*s == c) return s;
    return 0;
}
static const char *trchr(const char *s, char c)
{
    const char *last = 0;
    for (; *s; s++) if (*s == c) last = s;
    return last;
}
static void tcopy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}
static const char *tfind_opt(const char *hay, const char *needle)
{
    if (!hay || !needle) return 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncmp(p, needle, nl) == 0) return p;
    return 0;
}

#define TMPFS_MAX_NODES     512
#define TMPFS_MAX_MOUNTS      8
#define TMPFS_DEFAULT_BYTES  (16u * 1024u * 1024u)
#define TMPFS_MIN_GROW       512u

struct tmpfs_node {
    bool          used;
    enum vfs_type type;
    char          path[VFS_PATH_MAX];   /* normalised, relative to the mount */
    uint8_t      *data;                 /* NULL for a directory              */
    size_t        size, cap;
    uint32_t      uid, gid, mode;
    uint64_t      mtime, atime;
    /* POSIX: unlink() removes the NAME; the file itself survives until
     * the last open handle closes. Without this, `fd = open(t); unlink(t);
     * read(fd)` -- which is how every mkstemp()/tmpfile() user, GNU bash
     * here-documents included, makes a private temp file -- read back
     * nothing, because unlink freed the data out from under the fd. */
    int           open_count;
    bool          unlinked;
};

struct tmpfs_mount {
    bool               used;
    struct tmpfs_node *nodes;
    size_t             bytes;      /* data bytes currently held */
    size_t             max_bytes;
    bool               reported_full;  /* census printed once, not per create */
};

static struct tmpfs_mount g_tmpfs[TMPFS_MAX_MOUNTS];

/* ---- path helpers -------------------------------------------------------
 * The VFS hands drivers a path RELATIVE to the mount point, with a leading
 * '/'. Normalise to "no leading slash, no trailing slash", so the mount root
 * is the empty string and every other node is "a/b/c". */
static bool tnorm(const char *path, char *out, size_t cap)
{
    if (!path) return false;
    while (*path == '/') path++;
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] == '/') n--;
    if (n >= cap) return false;
    memcpy(out, path, n);
    out[n] = '\0';
    return true;
}

static struct tmpfs_node *tfind(struct tmpfs_mount *m, const char *norm)
{
    for (size_t i = 0; i < TMPFS_MAX_NODES; i++)
        if (m->nodes[i].used && !m->nodes[i].unlinked &&
            strcmp(m->nodes[i].path, norm) == 0)
            return &m->nodes[i];
    return NULL;
}

/* Is `norm` a DIRECT child of directory `dir`? Returns the child's own name. */
static const char *tchild_of(const char *dir, const char *norm)
{
    size_t dl = strlen(dir);
    if (dl == 0) {                       /* the mount root */
        if (!*norm) return NULL;
        return tchr(norm, '/') ? NULL : norm;
    }
    if (strncmp(norm, dir, dl) != 0 || norm[dl] != '/') return NULL;
    const char *rest = norm + dl + 1;
    if (!*rest) return NULL;
    return tchr(rest, '/') ? NULL : rest;
}

/* Free a node and its data, returning the slot to talloc(). The mount is
 * found by pointer range so callers that only hold the node can reap. */
static void treap(struct tmpfs_node *nd)
{
    if (!nd || !nd->used) return;
    for (size_t i = 0; i < TMPFS_MAX_MOUNTS; i++) {
        if (!g_tmpfs[i].used) continue;
        if (nd >= g_tmpfs[i].nodes && nd < g_tmpfs[i].nodes + TMPFS_MAX_NODES) {
            if (nd->data) g_tmpfs[i].bytes -= nd->cap;
            break;
        }
    }
    if (nd->data) kfree(nd->data);
    memset(nd, 0, sizeof *nd);
}

/* Running out of nodes used to be a silent NULL: every create returned an
 * error the caller reported as a generic failure, and a filesystem that has
 * simply filled up looked identical to one that is broken. The third-party
 * shell gate spent a whole run reporting "could not run" for 2,600 cases
 * because of it. A full table now says so, once per mount, with the census
 * that distinguishes the two ways it can happen:
 *
 *   used but reachable      -- genuinely full, the caller should clean up
 *   used AND unlinked       -- LEAKED: the name is gone but the slot is held
 *                              by an open_count that never came back to zero
 */
static void tmpfs_census(struct tmpfs_mount *m, const char *why)
{
    size_t used = 0, orphan = 0, open_held = 0;
    for (size_t i = 0; i < TMPFS_MAX_NODES; i++) {
        if (!m->nodes[i].used) continue;
        used++;
        if (m->nodes[i].unlinked) orphan++;
        if (m->nodes[i].open_count > 0) open_held++;
    }
    kprintf("[tmpfs] %s: %lu/%d nodes used, %lu unlinked-but-held (leaked), "
            "%lu with open handles, %lu/%lu KiB\n",
            why, (unsigned long)used, TMPFS_MAX_NODES,
            (unsigned long)orphan, (unsigned long)open_held,
            (unsigned long)(m->bytes / 1024u),
            (unsigned long)(m->max_bytes / 1024u));

    /* Name a few of them. "506 nodes leaked" says a leak exists; the PATHS say
     * which operation leaked them, and that is the difference between knowing
     * there is a bug and knowing where it is. Bounded -- this runs from an
     * allocation failure and must not become the next flood. */
    int shown = 0;
    for (size_t i = 0; i < TMPFS_MAX_NODES && shown < 8; i++) {
        if (!m->nodes[i].used || !m->nodes[i].unlinked) continue;
        kprintf("[tmpfs]   leaked: '%s' open_count=%d type=%d size=%lu\n",
                m->nodes[i].path, m->nodes[i].open_count,
                (int)m->nodes[i].type, (unsigned long)m->nodes[i].size);
        shown++;
    }
}

static struct tmpfs_node *talloc(struct tmpfs_mount *m)
{
    for (size_t i = 0; i < TMPFS_MAX_NODES; i++)
        if (!m->nodes[i].used) return &m->nodes[i];
    if (!m->reported_full) {
        m->reported_full = true;
        tmpfs_census(m, "node table FULL");
    }
    return NULL;
}

/* The parent directory must exist, as it must on any real filesystem: without
 * this, create("a/b/c") on an empty tmpfs would silently succeed and produce a
 * file nothing can reach by walking. */
static bool tparent_ok(struct tmpfs_mount *m, const char *norm)
{
    const char *slash = trchr(norm, '/');
    if (!slash) return true;                    /* directly in the root */
    char parent[VFS_PATH_MAX];
    size_t pl = (size_t)(slash - norm);
    if (pl >= sizeof parent) return false;
    memcpy(parent, norm, pl);
    parent[pl] = '\0';
    struct tmpfs_node *p = tfind(m, parent);
    return p && p->type == VFS_TYPE_DIR;
}

/* ---- the ops ------------------------------------------------------------ */

static int tmpfs_open(void *mnt, const char *path, struct vfs_file *out)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    struct tmpfs_node *nd = tfind(m, norm);
    if (!nd) return VFS_ERR_NOENT;
    if (nd->type != VFS_TYPE_FILE) return VFS_ERR_ISDIR;
    nd->open_count++;
    out->priv = nd;
    out->pos  = 0;
    out->size = nd->size;
    out->uid  = nd->uid;
    out->gid  = nd->gid;
    out->mode = (nd->mode & 07777u) | VFS_MODE_VALID;
    return VFS_OK;
}

/* Releasing the last handle on an unlinked node is what finally frees it. */
static int tmpfs_close(struct vfs_file *f)
{
    struct tmpfs_node *nd = f ? f->priv : 0;
    if (!nd || !nd->used) return VFS_OK;
    if (nd->open_count > 0) nd->open_count--;
    if (nd->unlinked && nd->open_count == 0) treap(nd);
    return VFS_OK;
}

static long tmpfs_read(struct vfs_file *f, void *buf, size_t n)
{
    struct tmpfs_node *nd = f->priv;
    if (!nd || !nd->used) return VFS_ERR_IO;
    if (f->pos >= nd->size) return 0;
    size_t avail = nd->size - f->pos;
    if (n > avail) n = avail;
    if (n && nd->data) memcpy(buf, nd->data + f->pos, n);
    f->pos += n;
    return (long)n;
}

/* Grow to at least `want` bytes, charging the mount's budget. Doubling rather
 * than exact-fit because a write loop that appends a byte at a time would
 * otherwise copy the whole file on every call. */
static int tgrow(struct tmpfs_mount *m, struct tmpfs_node *nd, size_t want)
{
    if (want <= nd->cap) return VFS_OK;
    size_t ncap = nd->cap ? nd->cap : TMPFS_MIN_GROW;
    while (ncap < want) ncap *= 2;
    size_t delta = ncap - nd->cap;
    if (m->bytes + delta > m->max_bytes) {
        /* Try an exact fit before giving up: the doubling, not the request,
         * may be what crossed the limit. */
        ncap = want;
        delta = ncap - nd->cap;
        if (m->bytes + delta > m->max_bytes) {
            /* Say so. A tmpfs that has simply filled up returned a bare
             * NOSPC that every caller reported as a generic write failure,
             * which is indistinguishable from a broken filesystem -- the
             * shell gate lost 2,600 cases to exactly that ambiguity. */
            if (!m->reported_full) {
                m->reported_full = true;
                tmpfs_census(m, "byte budget EXHAUSTED");
            }
            return VFS_ERR_NOSPC;
        }
    }
    uint8_t *nb = kmalloc(ncap);
    if (!nb) return VFS_ERR_NOMEM;
    if (nd->size && nd->data) memcpy(nb, nd->data, nd->size);
    if (ncap > nd->size) memset(nb + nd->size, 0, ncap - nd->size);
    if (nd->data) kfree(nd->data);
    nd->data = nb;
    nd->cap  = ncap;
    m->bytes += delta;
    return VFS_OK;
}

static long tmpfs_write(struct vfs_file *f, const void *buf, size_t n)
{
    struct tmpfs_node *nd = f->priv;
    if (!nd || !nd->used) return VFS_ERR_IO;
    if (!n) return 0;
    /* The mount is found by walking the table: struct vfs_file carries the
     * node, not the mount. Cheap (8 slots) and keeps vfs_file unchanged. */
    struct tmpfs_mount *m = NULL;
    for (size_t i = 0; i < TMPFS_MAX_MOUNTS && !m; i++)
        if (g_tmpfs[i].used && nd >= g_tmpfs[i].nodes &&
            nd <  g_tmpfs[i].nodes + TMPFS_MAX_NODES)
            m = &g_tmpfs[i];
    if (!m) return VFS_ERR_IO;

    size_t end = f->pos + n;
    int rc = tgrow(m, nd, end);
    if (rc != VFS_OK) return rc;
    memcpy(nd->data + f->pos, buf, n);
    if (end > nd->size) nd->size = end;
    f->pos = end;
    f->size = nd->size;
    nd->mtime = tmpfs_now_secs();
    return (long)n;
}

static int tmpfs_create(void *mnt, const char *path, uint32_t uid,
                        uint32_t gid, uint32_t mode)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    if (!*norm) return VFS_ERR_EXIST;              /* the root */
    if (tfind(m, norm)) return VFS_ERR_EXIST;
    if (!tparent_ok(m, norm)) return VFS_ERR_NOENT;
    struct tmpfs_node *nd = talloc(m);
    if (!nd) return VFS_ERR_NOSPC;
    memset(nd, 0, sizeof *nd);
    nd->used = true;
    nd->type = VFS_TYPE_FILE;
    tcopy(nd->path, norm, sizeof nd->path);
    nd->uid = uid; nd->gid = gid;
    nd->mode = mode ? (mode & 07777u) : 0644u;
    nd->mtime = nd->atime = tmpfs_now_secs();
    return VFS_OK;
}

static int tmpfs_mkdir(void *mnt, const char *path, uint32_t uid,
                       uint32_t gid, uint32_t mode)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    if (!*norm) return VFS_ERR_EXIST;
    if (tfind(m, norm)) return VFS_ERR_EXIST;
    if (!tparent_ok(m, norm)) return VFS_ERR_NOENT;
    struct tmpfs_node *nd = talloc(m);
    if (!nd) return VFS_ERR_NOSPC;
    memset(nd, 0, sizeof *nd);
    nd->used = true;
    nd->type = VFS_TYPE_DIR;
    tcopy(nd->path, norm, sizeof nd->path);
    nd->uid = uid; nd->gid = gid;
    nd->mode = mode ? (mode & 07777u) : 0755u;
    nd->mtime = nd->atime = tmpfs_now_secs();
    return VFS_OK;
}

static int tmpfs_unlink(void *mnt, const char *path)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    struct tmpfs_node *nd = tfind(m, norm);
    if (!nd) return VFS_ERR_NOENT;
    /* A non-empty directory must not vanish with its contents still in the
     * table -- those entries would be unreachable but still charged. */
    if (nd->type == VFS_TYPE_DIR)
        for (size_t i = 0; i < TMPFS_MAX_NODES; i++)
            if (m->nodes[i].used && tchild_of(norm, m->nodes[i].path))
                return VFS_ERR_EXIST;   /* ENOTEMPTY has no VFS code here */
    /* Still open: drop the name now, keep the bytes until the last close.
     * The slot stays `used` so talloc() cannot hand it out underneath the
     * handles that still point at it. */
    if (nd->open_count > 0) {
        nd->unlinked = true;
        return VFS_OK;
    }
    treap(nd);
    return VFS_OK;
}

static int tmpfs_stat(void *mnt, const char *path, struct vfs_stat *out)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    if (!*norm) {                                  /* the mount root */
        memset(out, 0, sizeof *out);
        out->type = VFS_TYPE_DIR;
        out->mode = 0777u;
        return VFS_OK;
    }
    struct tmpfs_node *nd = tfind(m, norm);
    if (!nd) return VFS_ERR_NOENT;
    memset(out, 0, sizeof *out);
    out->type  = nd->type;
    out->size  = nd->size;
    out->uid   = nd->uid;
    out->gid   = nd->gid;
    out->mode  = nd->mode;
    out->mtime = nd->mtime;
    out->atime = nd->atime;
    return VFS_OK;
}

struct tmpfs_dirh { struct tmpfs_mount *m; char dir[VFS_PATH_MAX]; size_t i; };

static int tmpfs_opendir(void *mnt, const char *path, struct vfs_dir *out)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    if (*norm) {
        struct tmpfs_node *nd = tfind(m, norm);
        if (!nd) return VFS_ERR_NOENT;
        if (nd->type != VFS_TYPE_DIR) return VFS_ERR_NOTDIR;
    }
    struct tmpfs_dirh *dh = kmalloc(sizeof *dh);
    if (!dh) return VFS_ERR_NOMEM;
    dh->m = m; dh->i = 0;
    tcopy(dh->dir, norm, sizeof dh->dir);
    dh->dir[sizeof dh->dir - 1] = '\0';
    out->priv = dh;
    return VFS_OK;
}

static int tmpfs_closedir(struct vfs_dir *d)
{
    if (d->priv) kfree(d->priv);
    d->priv = NULL;
    return VFS_OK;
}

static int tmpfs_readdir(struct vfs_dir *d, struct vfs_dirent *out)
{
    struct tmpfs_dirh *dh = d->priv;
    if (!dh) return VFS_ERR_INVAL;
    for (; dh->i < TMPFS_MAX_NODES; dh->i++) {
        struct tmpfs_node *nd = &dh->m->nodes[dh->i];
        if (!nd->used) continue;
        const char *name = tchild_of(dh->dir, nd->path);
        if (!name) continue;
        memset(out->name, 0, VFS_NAME_MAX);
        size_t nl = strlen(name);
        if (nl >= VFS_NAME_MAX) nl = VFS_NAME_MAX - 1;
        memcpy(out->name, name, nl);
        out->type = nd->type;
        out->size = nd->size;
        out->uid  = nd->uid;
        out->gid  = nd->gid;
        out->mode = nd->mode;
        dh->i++;
        return VFS_OK;
    }
    return VFS_ERR_NOENT;
}

static int tmpfs_chmod(void *mnt, const char *path, uint32_t mode)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    struct tmpfs_node *nd = tfind(m, norm);
    if (!nd) return VFS_ERR_NOENT;
    nd->mode = mode & 07777u;
    return VFS_OK;
}

static int tmpfs_chown(void *mnt, const char *path, uint32_t uid, uint32_t gid)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    struct tmpfs_node *nd = tfind(m, norm);
    if (!nd) return VFS_ERR_NOENT;
    if (uid != (uint32_t)-1) nd->uid = uid;
    if (gid != (uint32_t)-1) nd->gid = gid;
    return VFS_OK;
}

static int tmpfs_utimes(void *mnt, const char *path, uint64_t mtime,
                        uint64_t atime)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    struct tmpfs_node *nd = tfind(m, norm);
    if (!nd) return VFS_ERR_NOENT;
    nd->mtime = mtime; nd->atime = atime;
    return VFS_OK;
}

static int ttrunc(struct tmpfs_mount *m, struct tmpfs_node *nd, uint64_t len)
{
    if (len > nd->size) {
        int rc = tgrow(m, nd, (size_t)len);
        if (rc != VFS_OK) return rc;
        /* tgrow zeroes the region past size, so the hole reads as zeroes --
         * which is what extending a file must do. */
    }
    nd->size = (size_t)len;
    nd->mtime = tmpfs_now_secs();
    return VFS_OK;
}

static int tmpfs_truncate(void *mnt, const char *path, uint64_t length)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    struct tmpfs_node *nd = tfind(m, norm);
    if (!nd) return VFS_ERR_NOENT;
    if (nd->type != VFS_TYPE_FILE) return VFS_ERR_ISDIR;
    return ttrunc(m, nd, length);
}

static int tmpfs_ftruncate(struct vfs_file *f, uint64_t length)
{
    struct tmpfs_node *nd = f->priv;
    if (!nd || !nd->used) return VFS_ERR_IO;
    struct tmpfs_mount *m = NULL;
    for (size_t i = 0; i < TMPFS_MAX_MOUNTS && !m; i++)
        if (g_tmpfs[i].used && nd >= g_tmpfs[i].nodes &&
            nd <  g_tmpfs[i].nodes + TMPFS_MAX_NODES)
            m = &g_tmpfs[i];
    if (!m) return VFS_ERR_IO;
    int rc = ttrunc(m, nd, length);
    if (rc == VFS_OK) f->size = nd->size;
    return rc;
}

/* Rename within the mount. A directory takes its whole subtree with it --
 * without that, `mv a b` on a directory would strand every child at a path
 * whose parent no longer exists. */
static int tmpfs_rename(void *mnt, const char *oldpath, const char *newpath)
{
    struct tmpfs_mount *m = mnt;
    char o[VFS_PATH_MAX], nw[VFS_PATH_MAX];
    if (!tnorm(oldpath, o, sizeof o) || !tnorm(newpath, nw, sizeof nw))
        return VFS_ERR_INVAL;
    struct tmpfs_node *src = tfind(m, o);
    if (!src) return VFS_ERR_NOENT;
    if (!tparent_ok(m, nw)) return VFS_ERR_NOENT;
    struct tmpfs_node *dst = tfind(m, nw);
    if (dst) {                                  /* POSIX: replace */
        if (dst->data) { m->bytes -= dst->cap; kfree(dst->data); }
        memset(dst, 0, sizeof *dst);
    }
    size_t ol = strlen(o), nl = strlen(nw);
    if (src->type == VFS_TYPE_DIR) {
        for (size_t i = 0; i < TMPFS_MAX_NODES; i++) {
            struct tmpfs_node *c = &m->nodes[i];
            if (!c->used || c == src) continue;
            if (strncmp(c->path, o, ol) != 0 || c->path[ol] != '/') continue;
            char moved[VFS_PATH_MAX];
            size_t rest = strlen(c->path + ol);
            if (nl + rest >= sizeof moved) return VFS_ERR_NAMETOOLONG;
            memcpy(moved, nw, nl);
            memcpy(moved + nl, c->path + ol, rest + 1);
            memcpy(c->path, moved, nl + rest + 1);
        }
    }
    tcopy(src->path, nw, sizeof src->path);
    src->path[sizeof src->path - 1] = '\0';
    return VFS_OK;
}

/* Phase H: real numbers -- the mount's byte budget and node table, not
 * the fabricated 4 GiB every statfs caller used to see. */
static int tmpfs_statfs(void *mnt, struct vfs_statfs *out)
{
    struct tmpfs_mount *m = (struct tmpfs_mount *)mnt;
    size_t files = 0;
    for (size_t i = 0; i < TMPFS_MAX_NODES; i++)
        if (m->nodes[i].used) files++;
    out->bsize      = 4096;
    out->blocks     = (m->max_bytes + 4095) / 4096;
    out->bfree      = (m->max_bytes > m->bytes)
                          ? (m->max_bytes - m->bytes) / 4096 : 0;
    out->files      = TMPFS_MAX_NODES;
    out->ffree      = TMPFS_MAX_NODES - files;
    out->type_magic = 0x01021994;           /* TMPFS_MAGIC */
    out->namelen    = VFS_PATH_MAX - 1;
    return VFS_OK;
}

static const struct vfs_ops tmpfs_ops = {
    .open      = tmpfs_open,
    .close     = tmpfs_close,
    .read      = tmpfs_read,
    .write     = tmpfs_write,
    .create    = tmpfs_create,
    .unlink    = tmpfs_unlink,
    .rename    = tmpfs_rename,
    .mkdir     = tmpfs_mkdir,
    .opendir   = tmpfs_opendir,
    .closedir  = tmpfs_closedir,
    .readdir   = tmpfs_readdir,
    .stat      = tmpfs_stat,
    .chmod     = tmpfs_chmod,
    .chown     = tmpfs_chown,
    .utimes    = tmpfs_utimes,
    .truncate  = tmpfs_truncate,
    .ftruncate = tmpfs_ftruncate,
    .readlink  = 0,
    .umount    = 0,
    .statfs    = tmpfs_statfs,   /* Phase H */
};

/* Parse the `size=` mount option. Accepts a plain byte count or a k/m/g
 * suffix, as mount(8) does. Returns 0 if absent, (size_t)-1 if present but
 * unparseable -- the caller REFUSES rather than falling back to the default,
 * because silently giving a different size than was asked for is the lie this
 * whole file is trying not to tell. */
static size_t tmpfs_parse_size(const char *opts)
{
    if (!opts) return 0;
    const char *p = tfind_opt(opts, "size=");
    if (!p) return 0;
    p += 5;
    if (*p < '0' || *p > '9') return (size_t)-1;
    size_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10u + (size_t)(*p - '0'); p++; }
    switch (*p) {
    case 'k': case 'K': v *= 1024u;                     p++; break;
    case 'm': case 'M': v *= 1024u * 1024u;             p++; break;
    case 'g': case 'G': v *= 1024u * 1024u * 1024u;     p++; break;
    default: break;
    }
    if (*p && *p != ',') return (size_t)-1;
    return v ? v : (size_t)-1;
}

int tmpfs_mount_at(const char *path, const char *opts)
{
    size_t want = tmpfs_parse_size(opts);
    if (want == (size_t)-1) {
        kprintf("[tmpfs] unparseable size= option -- refusing rather than "
                "mounting a different size than asked for\n");
        return VFS_ERR_INVAL;
    }

    struct tmpfs_mount *m = NULL;
    for (size_t i = 0; i < TMPFS_MAX_MOUNTS && !m; i++)
        if (!g_tmpfs[i].used) m = &g_tmpfs[i];
    if (!m) return VFS_ERR_NOSPC;

    m->nodes = kmalloc(sizeof(struct tmpfs_node) * TMPFS_MAX_NODES);
    if (!m->nodes) return VFS_ERR_NOMEM;
    memset(m->nodes, 0, sizeof(struct tmpfs_node) * TMPFS_MAX_NODES);
    m->bytes = 0;
    m->max_bytes = want ? want : TMPFS_DEFAULT_BYTES;
    m->used = true;

    int rc = vfs_mount(path, &tmpfs_ops, m);
    if (rc != VFS_OK) {
        kfree(m->nodes);
        memset(m, 0, sizeof *m);
        return rc;
    }
    kprintf("[tmpfs] mounted at %s (%lu KiB max, %d nodes)\n", path,
            (unsigned long)(m->max_bytes / 1024u), TMPFS_MAX_NODES);
    return VFS_OK;
}
