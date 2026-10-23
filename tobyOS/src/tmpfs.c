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
 * mount, defaults to half of RAM as Linux's (it was 16 MiB, which a large
 * archive unpacked into /tmp ran out of -- finding 231), and is settable by
 * the standard `size=` mount option; exceeding it is ENOSPC, which is a
 * state callers already handle.
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
#include <tobyos/proc.h>       /* a link's owner: the caller (tmpfs_symlink) */
#include <tobyos/pmm.h>        /* the default size: half of RAM (finding 231) */

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

#define TMPFS_MAX_MOUNTS      64    /* 8 for the uptime, never freed (233) */
#define TMPFS_DEFAULT_BYTES  (16u * 1024u * 1024u)   /* the least default */

/* A mount's size when size= does not say: half of RAM, as Linux's. */
static size_t tmpfs_default_bytes(void) {
    size_t half = pmm_total_pages() / 2 * 4096u;
    return half > TMPFS_DEFAULT_BYTES ? half : TMPFS_DEFAULT_BYTES;
}
#define TMPFS_MIN_GROW       512u

/* THE NODE TABLE GROWS, a chunk at a time, up to the mount's node limit.
 * It was one fixed array of 512: /tmp said "No space left on device" at its
 * 498th file with 16368 of its 16384 blocks free (finding 55, logs/b17
 * audit). A chunk never moves once it is allocated -- an open file holds its
 * node by ADDRESS and a hard link holds its owner by INDEX -- so growing adds
 * a chunk and never copies one. The limit is Linux's `nr_inodes=` option; by
 * default one node per KiB of the byte budget (16384 for 16 MiB),
 * and never fewer than the 512 the fixed table had. */
#define TMPFS_CHUNK_NODES    128u
#define TMPFS_MIN_NODES      512u
#define TMPFS_BYTES_PER_NODE 1024u
#define TMPFS_LIMIT_NODES    (1u << 24)   /* link_to is a 32-bit index */

struct tmpfs_node {
    bool          used;
    enum vfs_type type;
    char          path[VFS_PATH_MAX];   /* normalised, relative to the mount */
    uint8_t      *data;                 /* NULL for a directory              */
    size_t        size, cap;
    uint32_t      uid, gid, mode;
    uint64_t      mtime, atime;
    uint64_t      ctime;                /* the last change (finding 224) */
    /* POSIX: unlink() removes the NAME; the file itself survives until
     * the last open handle closes. Without this, `fd = open(t); unlink(t);
     * read(fd)` -- which is how every mkstemp()/tmpfile() user, GNU bash
     * here-documents included, makes a private temp file -- read back
     * nothing, because unlink freed the data out from under the fd. */
    int           open_count;
    bool          unlinked;
    /* Its creation number (a rename renumbers it): readdir lists a
     * directory's children newest first, as Linux's tmpfs (finding 221). */
    uint64_t      seq;
    /* Hard links. A tmpfs node is keyed by its PATH, so a second name for
     * the same bytes needs one level of indirection: `link_to` is 0 for an
     * entry that owns its data, or (owner index + 1) for an extra name.
     * Zero meaning "I am the owner" is deliberate -- every node is created
     * by memset, so the safe value is the one memset produces. */
    uint32_t      link_to;
    uint32_t      nlink;                /* names pointing at this owner */
    uint32_t      nent;                 /* a directory's entries (238) */
    size_t        pages;                /* its size's pages, charged (243) */
    uint32_t      rmaj, rmin;           /* a device node's numbers (214) */
};

/* The pages of N bytes, which Linux's tmpfs charges a file (243) */
#define TPAGES(n) (((size_t)(n) + 4095u) / 4096u)

/* A directory's size, as Linux's tmpfs counts it: BOGO_DIRENT_SIZE (20)
 * for each entry and for . and .. -- it was 0 (finding 238) */
#define TDIR_SIZE(n) (40u + 20u * (uint64_t)(n))

struct tmpfs_mount {
    bool                used;
    struct tmpfs_node **chunks;    /* max_nodes / TMPFS_CHUNK_NODES slots */
    size_t              nnodes;    /* nodes in the chunks allocated so far */
    size_t              max_nodes; /* nr_inodes, a whole number of chunks */
    size_t              bytes;     /* data bytes currently held */
    size_t              pages;     /* the files' pages: what size= limits (243) */
    size_t              max_bytes;
    bool                reported_full;  /* census printed once, not per create */
    bool                no_byte_limit;  /* size=0: none, statfs says 0 (233) */
    bool                no_node_limit;  /* nr_inodes=0: likewise */
    uint32_t            root_mode; /* the mount root's permissions (mode=) */
    uint64_t            root_time; /* when it was mounted: the root's times */
    uint64_t            root_mtime;/* its last entry change, 0 for none (238) */
    uint32_t            root_nent; /* the root's entries (238) */
    uint64_t            next_seq;  /* the next node's creation number */
};

static struct tmpfs_mount g_tmpfs[TMPFS_MAX_MOUNTS];

/* Node I of a mount; I < m->nnodes. */
static struct tmpfs_node *tnode(struct tmpfs_mount *m, size_t i)
{
    return &m->chunks[i / TMPFS_CHUNK_NODES][i % TMPFS_CHUNK_NODES];
}

/* A node's index in mount M, or (size_t)-1 when it is not one of M's. */
static size_t tindex(struct tmpfs_mount *m, const struct tmpfs_node *nd)
{
    for (size_t c = 0; c < m->nnodes / TMPFS_CHUNK_NODES; c++)
        if (nd >= m->chunks[c] && nd < m->chunks[c] + TMPFS_CHUNK_NODES)
            return c * TMPFS_CHUNK_NODES + (size_t)(nd - m->chunks[c]);
    return (size_t)-1;
}

/* The mount a node lives in, found by address: struct vfs_file carries the
 * node, not the mount. Cheap (a few mounts) and keeps vfs_file unchanged. */
static struct tmpfs_mount *tmount_of(const struct tmpfs_node *nd)
{
    for (size_t i = 0; i < TMPFS_MAX_MOUNTS; i++)
        if (g_tmpfs[i].used && tindex(&g_tmpfs[i], nd) != (size_t)-1)
            return &g_tmpfs[i];
    return NULL;
}

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

/* Find the NAME. Callers that manipulate the directory entry itself --
 * unlink, rename, link -- want this one. */
static struct tmpfs_node *tfind_name(struct tmpfs_mount *m, const char *norm)
{
    for (size_t i = 0; i < m->nnodes; i++) {
        struct tmpfs_node *nd = tnode(m, i);
        if (nd->used && !nd->unlinked && strcmp(nd->path, norm) == 0)
            return nd;
    }
    return NULL;
}

/* Follow a name to the node that actually owns the bytes. */
static struct tmpfs_node *tresolve(struct tmpfs_mount *m,
                                   struct tmpfs_node *nm)
{
    if (!nm || !nm->link_to) return nm;
    size_t idx = (size_t)nm->link_to - 1;
    if (idx >= m->nnodes) return nm;            /* corrupt: fail closed */
    struct tmpfs_node *ino = tnode(m, idx);
    return ino->used ? ino : nm;
}

/* Find the DATA. Everything that reads or writes a file wants this one,
 * so every existing caller keeps working through a link unchanged. */
static struct tmpfs_node *tfind(struct tmpfs_mount *m, const char *norm)
{
    return tresolve(m, tfind_name(m, norm));
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
 * found by address so callers that only hold the node can reap. */
static void treap(struct tmpfs_node *nd)
{
    if (!nd || !nd->used) return;
    struct tmpfs_mount *m = tmount_of(nd);
    if (m && nd->data) m->bytes -= nd->cap;
    if (m) m->pages -= nd->pages;
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
    for (size_t i = 0; i < m->nnodes; i++) {
        struct tmpfs_node *nd = tnode(m, i);
        if (!nd->used) continue;
        used++;
        if (nd->unlinked) orphan++;
        if (nd->open_count > 0) open_held++;
    }
    kprintf("[tmpfs] %s: %lu/%lu nodes used, %lu unlinked-but-held (leaked), "
            "%lu with open handles, %lu/%lu KiB\n",
            why, (unsigned long)used, (unsigned long)m->max_nodes,
            (unsigned long)orphan, (unsigned long)open_held,
            (unsigned long)(m->bytes / 1024u),
            (unsigned long)(m->max_bytes / 1024u));

    /* Name a few of them. "506 nodes leaked" says a leak exists; the PATHS say
     * which operation leaked them, and that is the difference between knowing
     * there is a bug and knowing where it is. Bounded -- this runs from an
     * allocation failure and must not become the next flood. */
    int shown = 0;
    for (size_t i = 0; i < m->nnodes && shown < 8; i++) {
        struct tmpfs_node *nd = tnode(m, i);
        if (!nd->used || !nd->unlinked) continue;
        kprintf("[tmpfs]   leaked: '%s' open_count=%d type=%d size=%lu\n",
                nd->path, nd->open_count, (int)nd->type,
                (unsigned long)nd->size);
        shown++;
    }
}

static struct tmpfs_node *talloc(struct tmpfs_mount *m)
{
    /* Slots past max_nodes in the last chunk are never handed out: the
     * limit is the one asked for, not rounded up to a chunk. */
    size_t lim = m->nnodes < m->max_nodes ? m->nnodes : m->max_nodes;
    /* the root is one of nr_inodes, as Linux's: N allows N - 1 (243) */
    if (lim >= m->max_nodes && lim) lim = m->max_nodes - 1;
    for (size_t i = 0; i < lim; i++)
        if (!tnode(m, i)->used) return tnode(m, i);
    /* Every slot is taken: one more chunk, while the limit allows. */
    bool room = m->nnodes < m->max_nodes;
    if (room) {
        size_t sz = sizeof(struct tmpfs_node) * TMPFS_CHUNK_NODES;
        struct tmpfs_node *c = kmalloc(sz);
        if (c) {
            memset(c, 0, sz);
            m->chunks[m->nnodes / TMPFS_CHUNK_NODES] = c;
            m->nnodes += TMPFS_CHUNK_NODES;
            return c;
        }
    }
    if (!m->reported_full) {
        m->reported_full = true;
        tmpfs_census(m, room ? "node table could not GROW (no memory)"
                             : "node table FULL");
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
    out->mode = (nd->mode & 0177777u) | VFS_MODE_VALID;
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

/* A READ MOVES atime, as Linux's relatime has it: to now when atime is not
 * newer than mtime or ctime, or is a day old. tmpfs never moved it, so a
 * file read after `touch -a -d 2019-...` still read as 2019 (find
 * -anewer, finding 242). */
static void taccessed(struct tmpfs_node *nd)
{
    uint64_t now = tmpfs_now_secs();
    if (nd->atime <= nd->mtime || nd->atime <= nd->ctime ||
        now >= nd->atime + 86400u)
        nd->atime = now;
}

static long tmpfs_read(struct vfs_file *f, void *buf, size_t n)
{
    struct tmpfs_node *nd = f->priv;
    if (!nd || !nd->used) return VFS_ERR_IO;
    taccessed(nd);
    if (f->pos >= nd->size) return 0;
    size_t avail = nd->size - f->pos;
    if (n > avail) n = avail;
    if (n && nd->data) memcpy(buf, nd->data + f->pos, n);
    f->pos += n;
    return (long)n;
}

/* The pages size= allows, rounded up as Linux's is (243) */
static size_t tmax_pages(const struct tmpfs_mount *m)
{
    return TPAGES(m->max_bytes);
}

/* Grow to at least `want` bytes, charging the mount's budget. Doubling rather
 * than exact-fit because a write loop that appends a byte at a time would
 * otherwise copy the whole file on every call. The budget is the size's
 * PAGES, as Linux's tmpfs charges -- not the doubled buffer, which made a
 * 100,000-byte file 128 KiB used and filled a mount early (finding 243). */
static int tgrow(struct tmpfs_mount *m, struct tmpfs_node *nd, size_t want)
{
    size_t np = TPAGES(want), was = nd->pages;
    if (np > was) {
        if (m->pages + (np - was) > tmax_pages(m)) {
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
        m->pages += np - was;
        nd->pages = np;
    }
    if (want <= nd->cap) return VFS_OK;
    size_t ncap = nd->cap ? nd->cap : TMPFS_MIN_GROW;
    while (ncap < want) ncap *= 2;
    size_t delta = ncap - nd->cap;
    uint8_t *nb = kmalloc(ncap);
    if (!nb) {
        m->pages -= nd->pages - was;          /* nothing grew after all */
        nd->pages = was;
        return VFS_ERR_NOMEM;
    }
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
    struct tmpfs_mount *m = tmount_of(nd);
    if (!m) return VFS_ERR_IO;

    if (f->append) f->pos = nd->size;              /* O_APPEND: the live end */
    size_t end = f->pos + n;
    int rc = tgrow(m, nd, end);
    if (rc != VFS_OK) return rc;
    /* A write that starts past EOF leaves a hole, and a hole reads as
     * zeroes. Same reasoning as ttrunc: the capacity may already cover
     * this range, in which case tgrow zeroed nothing and the gap would
     * expose whatever the file held before it was truncated. */
    if (f->pos > nd->size)
        memset(nd->data + nd->size, 0, f->pos - nd->size);
    memcpy(nd->data + f->pos, buf, n);
    if (end > nd->size) nd->size = end;
    f->pos = end;
    f->size = nd->size;
    nd->mtime = nd->ctime = tmpfs_now_secs();
    return (long)n;
}

/* A DIRECTORY CHANGES WHEN ITS ENTRIES DO: Linux's tmpfs sets the
 * parent's mtime and ctime at every create, link, unlink and rename. This
 * one's never moved -- tar --no-overwrite-dir left a directory @7 where
 * Linux's says the time its last entry was made (finding 238). */
static void tparent_touch(struct tmpfs_mount *m, const char *norm, int delta)
{
    uint64_t now = tmpfs_now_secs();
    const char *slash = trchr(norm, '/');
    if (!slash) {
        m->root_mtime = now;
        if (delta < 0 && m->root_nent) m->root_nent--;
        else if (delta > 0) m->root_nent++;
        return;
    }
    char parent[VFS_PATH_MAX];
    size_t pl = (size_t)(slash - norm);
    if (pl >= sizeof parent) return;
    memcpy(parent, norm, pl);
    parent[pl] = '\0';
    struct tmpfs_node *pd = tfind(m, parent);
    if (!pd) return;
    pd->mtime = pd->ctime = now;
    if (delta < 0 && pd->nent) pd->nent--;      /* and its size (238) */
    else if (delta > 0) pd->nent++;
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
    nd->seq = ++m->next_seq;
    nd->type = VFS_TYPE_FILE;
    nd->nlink = 1;
    tcopy(nd->path, norm, sizeof nd->path);
    nd->uid = uid; nd->gid = gid;
    /* the format bits too: a FIFO is a file whose mode says S_IFIFO
     * (vfs_mkfifo) */
    nd->mode = mode ? (mode & 0177777u) : 0644u;
    nd->mtime = nd->atime = nd->ctime = tmpfs_now_secs();
    tparent_touch(m, norm, 1);
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
    nd->seq = ++m->next_seq;
    nd->type = VFS_TYPE_DIR;
    nd->nlink = 1;
    tcopy(nd->path, norm, sizeof nd->path);
    nd->uid = uid; nd->gid = gid;
    nd->mode = mode ? (mode & 07777u) : 0755u;
    nd->mtime = nd->atime = nd->ctime = tmpfs_now_secs();
    tparent_touch(m, norm, 1);
    return VFS_OK;
}

/* Remove ONE name. The bytes survive while another name or an open handle
 * still refers to them -- that is what a hard link means, and it is also
 * what POSIX already required of unlink() on an open file. */
static void tdrop_name(struct tmpfs_mount *m, struct tmpfs_node *nm)
{
    struct tmpfs_node *ino = tresolve(m, nm);
    if (ino != nm) {                      /* an extra name: drop just it */
        memset(nm, 0, sizeof *nm);
        if (ino->nlink) ino->nlink--;
        ino->ctime = tmpfs_now_secs();
        if (ino->nlink == 0 && ino->open_count == 0) treap(ino);
        return;
    }
    if (ino->nlink > 1) {
        /* Other names point here BY INDEX, so the slot has to live on --
         * but this name is gone, so hide it from lookups and readdir. */
        ino->nlink--;
        ino->ctime = tmpfs_now_secs();
        ino->unlinked = true;
        return;
    }
    if (ino->nlink) ino->nlink--;
    /* Still open: drop the name now, keep the bytes until the last close.
     * The slot stays `used` so talloc() cannot hand it out underneath the
     * handles that still point at it. */
    if (ino->open_count > 0) { ino->unlinked = true; return; }
    treap(ino);
}

static int tmpfs_unlink(void *mnt, const char *path)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    struct tmpfs_node *nd = tfind_name(m, norm);
    if (!nd) return VFS_ERR_NOENT;
    /* A non-empty directory must not vanish with its contents still in the
     * table -- those entries would be unreachable but still charged. An
     * already-unlinked child does not count: its name is gone, so the
     * directory really is empty. */
    if (nd->type == VFS_TYPE_DIR)
        for (size_t i = 0; i < m->nnodes; i++) {
            struct tmpfs_node *c = tnode(m, i);
            if (c->used && !c->unlinked && tchild_of(norm, c->path))
                return VFS_ERR_EXIST;   /* ENOTEMPTY has no VFS code here */
        }
    tdrop_name(m, nd);
    tparent_touch(m, norm, -1);
    return VFS_OK;
}

/* A SYMBOLIC LINK IS A NODE OF ITS OWN, its target held as its data. tmpfs
 * had no symlink op, so a link made in /tmp went to the VFS's RAM table:
 * readlink found it, but `ls` never listed it, stat said it did not exist,
 * and `rm -rf` of its directory left it behind (logs/b15 lk). */
static int tmpfs_symlink(void *mnt, const char *path, const char *target)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    if (!*norm) return VFS_ERR_EXIST;
    if (tfind_name(m, norm)) return VFS_ERR_EXIST;
    if (!tparent_ok(m, norm)) return VFS_ERR_NOENT;
    size_t tl = strlen(target);
    if (tl >= VFS_PATH_MAX) return VFS_ERR_NAMETOOLONG;
    struct tmpfs_node *nd = talloc(m);
    if (!nd) return VFS_ERR_NOSPC;
    memset(nd, 0, sizeof *nd);
    nd->used = true;
    nd->seq = ++m->next_seq;
    nd->type = VFS_TYPE_SYMLINK;
    nd->nlink = 1;
    tcopy(nd->path, norm, sizeof nd->path);
    struct proc *cp = current_proc();
    nd->uid = cp ? (uint32_t)cp->uid : 0;
    nd->gid = cp ? (uint32_t)cp->gid : 0;
    nd->mode = 0777u;
    int rc = tgrow(m, nd, tl ? tl : 1);
    if (rc != VFS_OK) {
        memset(nd, 0, sizeof *nd);
        return rc;
    }
    memcpy(nd->data, target, tl);
    nd->size = tl;
    nd->mtime = nd->atime = nd->ctime = tmpfs_now_secs();
    tparent_touch(m, norm, 1);
    return VFS_OK;
}

static int tmpfs_readlink(void *mnt, const char *path, char *buf, size_t bufsz)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    struct tmpfs_node *nd = tfind(m, norm);
    if (!nd) return VFS_ERR_NOENT;
    if (nd->type != VFS_TYPE_SYMLINK) return VFS_ERR_INVAL;
    size_t n = nd->size < bufsz - 1 ? nd->size : bufsz - 1;
    memcpy(buf, nd->data, n);
    buf[n] = '\0';
    return VFS_OK;
}

/* Phase G: a second directory entry for the same bytes. */
static int tmpfs_link(void *mnt, const char *oldpath, const char *newpath)
{
    struct tmpfs_mount *m = mnt;
    char o[VFS_PATH_MAX], nw[VFS_PATH_MAX];
    if (!tnorm(oldpath, o, sizeof o) || !tnorm(newpath, nw, sizeof nw))
        return VFS_ERR_INVAL;
    struct tmpfs_node *nm = tfind_name(m, o);
    if (!nm) return VFS_ERR_NOENT;
    struct tmpfs_node *ino = tresolve(m, nm);
    /* POSIX forbids hard links to directories: they make the tree cyclic
     * and every walker that trusts it loops forever. */
    if (ino->type == VFS_TYPE_DIR) return VFS_ERR_ISDIR;
    if (tfind_name(m, nw)) return VFS_ERR_EXIST;
    if (!tparent_ok(m, nw)) return VFS_ERR_NOENT;

    struct tmpfs_node *nn = talloc(m);
    if (!nn) return VFS_ERR_NOSPC;
    memset(nn, 0, sizeof *nn);
    nn->used = true;
    nn->seq = ++m->next_seq;
    nn->type = ino->type;
    tcopy(nn->path, nw, sizeof nn->path);
    nn->path[sizeof nn->path - 1] = '\0';
    nn->link_to = (uint32_t)tindex(m, ino) + 1u;   /* talloc moved nothing */
    ino->nlink  = (ino->nlink ? ino->nlink : 1u) + 1u;
    ino->ctime  = tmpfs_now_secs();      /* a new link is a change (224) */
    tparent_touch(m, nw, 1);
    return VFS_OK;
}

static void tnode_stat(struct tmpfs_mount *m, struct tmpfs_node *nd,
                       struct vfs_stat *out);

static int tmpfs_stat(void *mnt, const char *path, struct vfs_stat *out)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    /* AN IDENTITY: the owning node's slot (shared by its hard links), and
     * the mount's number for the device. tmpfs reported none, so two names
     * for one file -- or $PWD and the cwd it names, through a link -- could
     * not be told the same (test -ef; tsh's logical pwd, logs/b15 cdl). */
    uint32_t dev = 0x7700u + (uint32_t)(m - g_tmpfs);
    if (!*norm) {                                  /* the mount root */
        memset(out, 0, sizeof *out);
        out->type = VFS_TYPE_DIR;
        out->mode = m->root_mode | VFS_MODE_VALID;
        /* its times are the mount's, as Linux's tmpfs root has (they
         * were 0, so `ls -ld /tmp` said Jan 1 1970) */
        out->atime = m->root_time;
        out->mtime = out->ctime = m->root_mtime ? m->root_mtime : m->root_time;
        out->size = TDIR_SIZE(m->root_nent);
        out->ino  = m->max_nodes + 1u;             /* past every node's */
        out->dev  = dev;
        return VFS_OK;
    }
    struct tmpfs_node *nd = tfind(m, norm);
    if (!nd) return VFS_ERR_NOENT;
    tnode_stat(m, nd, out);
    return VFS_OK;
}

/* What stat says of node ND (the owner of its bytes) -- shared by stat and
 * fstat, so the name and the open file are one file. */
static void tnode_stat(struct tmpfs_mount *m, struct tmpfs_node *nd,
                       struct vfs_stat *out)
{
    memset(out, 0, sizeof *out);
    out->ino   = (uint64_t)tindex(m, nd) + 1u;
    out->dev   = 0x7700u + (uint32_t)(m - g_tmpfs);
    out->type  = nd->type;
    out->size  = nd->type == VFS_TYPE_DIR ? TDIR_SIZE(nd->nent) : nd->size;
    out->uid   = nd->uid;
    out->gid   = nd->gid;
    /* VFS_MODE_VALID, and the same 07777 mask tmpfs_open already uses.
     *
     * Without the flag this stat says "this filesystem carries no permission
     * information", and every caller that gates on it skips its check. The
     * one that matters: BOTH exec paths test
     *     vfs_stat(path,&st) == VFS_OK && (st.mode & VFS_MODE_VALID)
     * before honouring S_ISUID -- so a setuid binary anywhere on tmpfs was
     * SILENTLY run unprivileged, while the identical binary on the initrd
     * ramfs (whose stat does set the flag) worked. tmpfs_open set it and
     * tmpfs_stat did not, so the two disagreed about the same file. */
    out->mode  = (nd->mode & 0177777u) | VFS_MODE_VALID;
    out->mtime = nd->mtime;
    out->atime = nd->atime;
    out->ctime = nd->ctime;
    out->nlink = nd->nlink ? nd->nlink : 1u;
    out->rmaj  = nd->rmaj;
    out->rmin  = nd->rmin;
}

/* A device node's numbers, on the node mknod has just made (finding 214):
 * tmpfs keeps them, as Linux's does, and open finds the device by them. */
static int tmpfs_setrdev(void *mnt, const char *path, uint32_t maj, uint32_t min)
{
    struct tmpfs_mount *m = mnt;
    char norm[VFS_PATH_MAX];
    if (!tnorm(path, norm, sizeof norm)) return VFS_ERR_INVAL;
    struct tmpfs_node *nd = tfind(m, norm);
    if (!nd) return VFS_ERR_NOENT;
    uint32_t fmt = nd->mode & 0170000u;
    if (fmt != 0020000u && fmt != 0060000u) return VFS_ERR_INVAL;
    nd->rmaj = maj;
    nd->rmin = min;
    return VFS_OK;
}

/* The open file's stat, through the node it holds -- renamed or unlinked
 * since, it is still itself (finding 106; vfs_fstat). */
static int tmpfs_fstat(struct vfs_file *f, struct vfs_stat *out)
{
    struct tmpfs_node *nd = f ? f->priv : 0;
    if (!nd || !nd->used) return VFS_ERR_IO;
    struct tmpfs_mount *m = tmount_of(nd);
    if (!m) return VFS_ERR_IO;
    tnode_stat(m, nd, out);
    return VFS_OK;
}

/* A directory handle: the children's slots as opendir found them, newest
 * first (finding 221), and how far readdir has come. */
struct tmpfs_dirh {
    struct tmpfs_mount *m;
    char dir[VFS_PATH_MAX];
    size_t i, n;
    size_t *slot;
};

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
    dh->m = m; dh->i = 0; dh->n = 0; dh->slot = 0;
    tcopy(dh->dir, norm, sizeof dh->dir);
    dh->dir[sizeof dh->dir - 1] = '\0';
    size_t cnt = 0;
    for (size_t i = 0; i < m->nnodes; i++) {
        struct tmpfs_node *nd = tnode(m, i);
        if (nd->used && !nd->unlinked && tchild_of(dh->dir, nd->path)) cnt++;
    }
    if (cnt) {
        dh->slot = kmalloc(cnt * sizeof *dh->slot);
        if (!dh->slot) { kfree(dh); return VFS_ERR_NOMEM; }
        for (size_t i = 0; i < m->nnodes && dh->n < cnt; i++) {
            struct tmpfs_node *nd = tnode(m, i);
            if (nd->used && !nd->unlinked && tchild_of(dh->dir, nd->path))
                dh->slot[dh->n++] = i;
        }
        /* newest first: by creation number, descending (insertion sort --
         * a directory's children, already nearly in slot order) */
        for (size_t a = 1; a < dh->n; a++) {
            size_t v = dh->slot[a];
            uint64_t sv = tnode(m, v)->seq;
            size_t c = a;
            while (c > 0 && tnode(m, dh->slot[c - 1])->seq < sv) {
                dh->slot[c] = dh->slot[c - 1];
                c--;
            }
            dh->slot[c] = v;
        }
    }
    out->priv = dh;
    return VFS_OK;
}

static int tmpfs_closedir(struct vfs_dir *d)
{
    struct tmpfs_dirh *dh = d->priv;
    if (dh && dh->slot) kfree(dh->slot);
    if (d->priv) kfree(d->priv);
    d->priv = NULL;
    return VFS_OK;
}

static int tmpfs_readdir(struct vfs_dir *d, struct vfs_dirent *out)
{
    struct tmpfs_dirh *dh = d->priv;
    if (!dh) return VFS_ERR_INVAL;
    for (; dh->i < dh->n; dh->i++) {
        size_t sl = dh->slot[dh->i];
        if (sl >= dh->m->nnodes) continue;
        struct tmpfs_node *nd = tnode(dh->m, sl);
        /* `unlinked` means the NAME is gone; the slot only survives to
         * keep open handles valid. Listing it made `ls` show files that
         * unlink() had already removed. A slot removed since opendir is
         * skipped the same way (and one reused is not its child). */
        if (!nd->used || nd->unlinked) continue;
        const char *name = tchild_of(dh->dir, nd->path);
        if (!name) continue;
        memset(out->name, 0, VFS_NAME_MAX);
        size_t nl = strlen(name);
        if (nl >= VFS_NAME_MAX) nl = VFS_NAME_MAX - 1;
        memcpy(out->name, name, nl);
        /* A hard link takes its size and ownership from the owner. */
        struct tmpfs_node *ino = tresolve(dh->m, nd);
        out->type = ino->type;
        out->size = ino->type == VFS_TYPE_DIR ? TDIR_SIZE(ino->nent) : ino->size;
        out->uid  = ino->uid;
        out->gid  = ino->gid;
        out->mode = ino->mode;
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
    nd->mode = (nd->mode & 0170000u) | (mode & 07777u);   /* keeps its type */
    nd->ctime = tmpfs_now_secs();
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
    nd->ctime = tmpfs_now_secs();
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
    nd->ctime = tmpfs_now_secs();        /* setting times is a change (224) */
    return VFS_OK;
}

static int ttrunc(struct tmpfs_mount *m, struct tmpfs_node *nd, uint64_t len)
{
    if (len > nd->size) {
        size_t from = nd->size;
        int rc = tgrow(m, nd, (size_t)len);
        if (rc != VFS_OK) return rc;
        /* Zero the hole HERE, not in tgrow. tgrow returns early when the
         * capacity already covers the request -- and after a shrink it
         * always does, because the buffer is never given back. So
         * truncate(f,3) then truncate(f,10) used to hand back bytes 3..9
         * of the file's PREVIOUS contents. That is a stale-data leak, and
         * POSIX says an extended file reads as zeroes. */
        if (nd->data && (size_t)len > from)
            memset(nd->data + from, 0, (size_t)len - from);
    }
    nd->size = (size_t)len;
    if (TPAGES(len) < nd->pages) {              /* a shrink gives pages back */
        m->pages -= nd->pages - TPAGES(len);
        nd->pages = TPAGES(len);
    }
    nd->mtime = nd->ctime = tmpfs_now_secs();
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
    struct tmpfs_mount *m = tmount_of(nd);
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
    /* Renaming moves a NAME, so both ends are looked up as names: renaming
     * a hard link must move that link, not the file it points at. */
    struct tmpfs_node *src = tfind_name(m, o);
    if (!src) return VFS_ERR_NOENT;
    if (!tparent_ok(m, nw)) return VFS_ERR_NOENT;
    struct tmpfs_node *dst = tfind_name(m, nw);
    bool replaced = false;
    if (dst && dst != src) {                    /* POSIX: replace */
        tdrop_name(m, dst);
        replaced = true;
    }
    size_t ol = strlen(o), nl = strlen(nw);
    if (src->type == VFS_TYPE_DIR) {
        for (size_t i = 0; i < m->nnodes; i++) {
            struct tmpfs_node *c = tnode(m, i);
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
    src->seq = ++m->next_seq;            /* newest where it now is (221) */
    src->ctime = tmpfs_now_secs();       /* a rename is a change (224) */
    tparent_touch(m, o, -1);
    tparent_touch(m, nw, replaced ? 0 : 1);
    return VFS_OK;
}

/* Phase H: real numbers -- the mount's byte budget and node table, not
 * the fabricated 4 GiB every statfs caller used to see. */
static int tmpfs_statfs(void *mnt, struct vfs_statfs *out)
{
    struct tmpfs_mount *m = (struct tmpfs_mount *)mnt;
    size_t files = 0;
    for (size_t i = 0; i < m->nnodes; i++)
        if (tnode(m, i)->used) files++;
    out->bsize      = 4096;
    /* pages, as Linux's: the files' sizes in whole pages, the limit too;
     * the root is an inode (finding 243) */
    out->blocks     = tmax_pages(m);
    out->bfree      = out->blocks > m->pages ? out->blocks - m->pages : 0;
    out->files      = m->max_nodes;
    out->ffree      = m->max_nodes > files + 1 ? m->max_nodes - files - 1 : 0;
    /* no limit: Linux's statfs says 0 of each */
    if (m->no_byte_limit) out->blocks = out->bfree = 0;
    if (m->no_node_limit) out->files = out->ffree = 0;
    out->type_magic = 0x01021994;           /* TMPFS_MAGIC */
    out->namelen    = VFS_NAME_MAX - 1;     /* Linux's 255, not the path limit */
    return VFS_OK;
}

/* UNMOUNTED: every file's bytes and the node table go back, and the slot is
 * free for the next mount. There was no umount op -- a tmpfs once mounted
 * kept its slot and its memory for good, and after eight mounts since boot
 * every tmpfs mount was ENOSPC (finding 233). The VFS calls this only for
 * the last mount of it in any namespace. A file still open in it keeps the
 * whole of it, as an open file keeps Linux's superblock. */
static int tmpfs_umount(void *mnt)
{
    struct tmpfs_mount *m = mnt;
    if (!m || !m->used) return VFS_OK;
    for (size_t i = 0; i < m->nnodes; i++) {
        struct tmpfs_node *nd = tnode(m, i);
        if (nd->used && nd->open_count > 0) {
            kprintf("[tmpfs] unmounted with '%s' open: its memory is kept\n",
                    nd->path);
            return VFS_OK;
        }
    }
    for (size_t i = 0; i < m->nnodes; i++) {
        struct tmpfs_node *nd = tnode(m, i);
        if (nd->used && nd->data) kfree(nd->data);
    }
    for (size_t c = 0; c < m->nnodes / TMPFS_CHUNK_NODES; c++)
        kfree(m->chunks[c]);
    kfree(m->chunks);
    memset(m, 0, sizeof *m);
    return VFS_OK;
}

static const struct vfs_ops tmpfs_ops = {
    .fsname   = "tmpfs",          /* batch 31: /proc/mounts */
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
    .link      = tmpfs_link,
    .symlink   = tmpfs_symlink,
    .readlink  = tmpfs_readlink,
    .umount    = tmpfs_umount,
    .statfs    = tmpfs_statfs,   /* Phase H */
    .fstat     = tmpfs_fstat,
    .setrdev   = tmpfs_setrdev,  /* finding 214 */
};

/* Parse a numeric mount option, `size=` or `nr_inodes=`. Accepts a plain
 * count or a k/m/g suffix, as mount(8) does. Returns 0 if absent, (size_t)-1
 * if present but unparseable -- the caller REFUSES rather than falling back to
 * the default, because silently giving a different size than was asked for is
 * the lie this whole file is trying not to tell. A value of 0 is NO LIMIT, as
 * Linux's tmpfs reads it: *ZERO says so, and the return is 0 (233). */
static size_t tmpfs_parse_num(const char *opts, const char *key, bool *zero)
{
    *zero = false;
    if (!opts) return 0;
    const char *p = tfind_opt(opts, key);
    if (!p) return 0;
    p += strlen(key);
    if (*p < '0' || *p > '9') return (size_t)-1;
    size_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10u + (size_t)(*p - '0'); p++; }
    switch (*p) {
    case 'k': case 'K': v *= 1024u;                     p++; break;
    case 'm': case 'M': v *= 1024u * 1024u;             p++; break;
    case 'g': case 'G': v *= 1024u * 1024u * 1024u;     p++; break;
    /* and t, p, e, and a percentage of RAM for size=, as Linux's tmpfs
     * takes them (size=1t was EINVAL; finding 244) */
    case 't': case 'T': v <<= 40;                       p++; break;
    case 'p': case 'P': v <<= 50;                       p++; break;
    case 'e': case 'E': v <<= 60;                       p++; break;
    case '%':
        if (key[0] != 's') return (size_t)-1;      /* nr_inodes= has no % */
        v = (size_t)(pmm_total_pages() / 100u * v) * 4096u;
        p++;
        break;
    default: break;
    }
    if (*p && *p != ',') return (size_t)-1;
    if (!v) *zero = true;
    return v;
}

int tmpfs_mount_at(const char *path, const char *opts)
{
    bool size0, nodes0;
    size_t want = tmpfs_parse_num(opts, "size=", &size0);
    if (want == (size_t)-1) {
        kprintf("[tmpfs] unparseable size= option -- refusing rather than "
                "mounting a different size than asked for\n");
        return VFS_ERR_INVAL;
    }
    size_t want_nodes = tmpfs_parse_num(opts, "nr_inodes=", &nodes0);
    if (want_nodes == (size_t)-1 || want_nodes > TMPFS_LIMIT_NODES) {
        kprintf("[tmpfs] unparseable or too large nr_inodes= option -- "
                "refusing rather than mounting a different limit than asked "
                "for\n");
        return VFS_ERR_INVAL;
    }

    /* mode=OCTAL, as mount(8) takes it: the root's permissions. 0777
     * without it, as every tmpfs here has been; /run and /var are 0755 */
    uint32_t root_mode = 0777u;
    {
        const char *mp = opts ? tfind_opt(opts, "mode=") : 0;
        if (mp) {
            mp += 5;
            uint32_t v = 0;
            int nd = 0;
            for (; *mp >= '0' && *mp <= '7'; mp++, nd++) v = v * 8u + (uint32_t)(*mp - '0');
            if (!nd || (*mp && *mp != ',') || v > 07777u) {
                kprintf("[tmpfs] unparseable mode= option -- refusing\n");
                return VFS_ERR_INVAL;
            }
            root_mode = v;
        }
    }

    struct tmpfs_mount *m = NULL;
    for (size_t i = 0; i < TMPFS_MAX_MOUNTS && !m; i++)
        if (!g_tmpfs[i].used) m = &g_tmpfs[i];
    if (!m) return VFS_ERR_NOSPC;

    size_t max_bytes = want ? want : tmpfs_default_bytes();
    if (size0) max_bytes = (size_t)-1 / 2;          /* no limit (233) */
    size_t max_nodes = nodes0 ? TMPFS_LIMIT_NODES : want_nodes;
    if (!max_nodes) {
        max_nodes = max_bytes / TMPFS_BYTES_PER_NODE;
        if (max_nodes < TMPFS_MIN_NODES)   max_nodes = TMPFS_MIN_NODES;
        if (max_nodes > TMPFS_LIMIT_NODES) max_nodes = TMPFS_LIMIT_NODES;
    }
    /* Only the chunk pointers now; talloc() adds the chunks as files come. */
    size_t nchunks = (max_nodes + TMPFS_CHUNK_NODES - 1) / TMPFS_CHUNK_NODES;
    m->chunks = kmalloc(sizeof(struct tmpfs_node *) * nchunks);
    if (!m->chunks) return VFS_ERR_NOMEM;
    memset(m->chunks, 0, sizeof(struct tmpfs_node *) * nchunks);
    m->nnodes = 0;
    m->max_nodes = max_nodes;
    m->bytes = 0;
    m->pages = 0;
    m->max_bytes = max_bytes;
    m->root_mode = root_mode;
    m->root_time = tmpfs_now_secs();
    m->no_byte_limit = size0;
    m->no_node_limit = nodes0;
    m->used = true;

    int rc = vfs_mount(path, &tmpfs_ops, m);
    if (rc != VFS_OK) {
        kfree(m->chunks);
        memset(m, 0, sizeof *m);
        return rc;
    }
    kprintf("[tmpfs] mounted at %s (%lu KiB max, %lu nodes max)\n", path,
            (unsigned long)(m->max_bytes / 1024u),
            (unsigned long)m->max_nodes);
    return VFS_OK;
}
