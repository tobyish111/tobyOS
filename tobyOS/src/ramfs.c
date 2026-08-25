/* ramfs.c -- USTAR-backed read-only filesystem.
 *
 * Mount-time:
 *   - Walk every 512-byte block in the tar image.
 *   - For each USTAR header, parse `name`, `size`, `typeflag`.
 *   - Normalise the name (strip trailing '/' for directories), record
 *     a node entry pointing at the in-image data.
 *   - Two consecutive all-zero blocks terminate.
 *
 * Path canonicalisation:
 *   - Inputs from VFS callers always start with '/'. We strip the
 *     leading '/' and any trailing '/' (except the root, which becomes
 *     the empty string ""). Names stored in the node table use the
 *     same convention -- so lookups become a strcmp.
 *
 * Directory listing:
 *   - opendir() builds a deduplicated dirent array on the heap (stored
 *     in dir->priv). readdir() then just hands them out one at a time.
 *   - Implicit directories (where the tar contains "bin/hello" but no
 *     explicit "bin/" entry) are synthesised: any node whose path is
 *     `parent + "/" + leaf + "/" + rest` contributes a DIR dirent
 *     named `leaf`.
 */

#include <tobyos/ramfs.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define USTAR_BLOCK     512u
#define USTAR_NAME_LEN  100u
#define USTAR_MAGIC     "ustar"

struct ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};
_Static_assert(sizeof(struct ustar_header) == USTAR_BLOCK,
               "USTAR header must be exactly 512 bytes");

struct ramfs_node {
    char          name[VFS_NAME_MAX];   /* normalised: no leading/trailing '/' */
    enum vfs_type type;
    const void   *data;                 /* file bytes (NULL for DIR) */
    size_t        size;                 /* 0 for DIR */
    /* Ownership + permission bits, read from the USTAR header rather than
     * fabricated. Before this, ramfs reported a FIXED 00444 for every file
     * and 00555 for every directory at all three reporting sites (open,
     * stat, readdir) -- so nothing in the initrd could be marked executable,
     * setuid, or group-private, and `ls -l /bin` described a filesystem that
     * did not exist. mode carries VFS_MODE_PERMS (07777), so setuid/setgid/
     * sticky survive; execve reads S_ISUID from exactly this value. */
    uint32_t      mode;
    uint32_t      uid;
    uint32_t      gid;
    /* Copy-on-write state. `data` normally points INTO the initrd module
     * (zero-copy). After the first write it points at a kmalloc'd buffer of
     * `cap` bytes instead, and `owned` says so -- which is what makes it safe
     * to free/grow, and what keeps the shared initrd image itself immutable. */
    bool          owned;
    size_t        cap;
    /* Phase G: the writable root. A removed node is marked DEAD, never
     * compacted away -- open handles hold raw pointers into the node
     * tables, so slots must be address-stable for the mount's lifetime.
     * open_refs gates data teardown (unlink-while-open keeps the bytes
     * readable until the last close) and dead-slot reuse. */
    bool          dead;
    int           open_refs;
    /* Hard links. Like tmpfs, a ramfs node is keyed by its PATH, so a
     * second name for the same bytes needs one level of indirection:
     * `link_to` is 0 for an entry that owns its data, or (combined node
     * index + 1) for an extra name. Zero meaning "owner" is deliberate --
     * nodes are born from memset, so the safe value is the one it makes. */
    uint32_t      link_to;
    uint32_t      nlink;
    /* 2026-08-24: real timestamps. The tar carries an mtime per entry and
     * ramfs had been throwing it away -- worse, ramfs_stat never assigned
     * out->mtime/atime/ctime at all, so `ls -l /bin` printed whatever the
     * caller happened to have on its stack (the same defect the nlink
     * comment above records). Epoch seconds, like every other filesystem
     * here; 0 means "no answer", which stat emitters report as the epoch. */
    uint64_t      mtime;
    uint64_t      atime;
};

/* Epoch seconds, the way tmpfs gets them. Files created or written after
 * boot get a real time; tar-backed ones keep the archive's. */
static uint64_t ramfs_now_secs(void) {
    extern uint64_t lx_realtime_ns(uint64_t mono_ns);
    extern uint64_t perf_now_ns(void);
    return lx_realtime_ns(perf_now_ns()) / 1000000000ull;
}

/* Fallbacks for a tar whose mode field is missing or unparseable. These are
 * the OLD hardcoded values -- a malformed header degrades to the previous
 * behaviour rather than producing a 00000-mode file nobody can open. */
#define RAMFS_FALLBACK_FILE_MODE  00444u
#define RAMFS_FALLBACK_DIR_MODE   00555u
/* Synthesised parent directories (tar has "bin/hello" but no "bin/" entry)
 * have no header to read a mode from, so they get the conventional 0755. */
#define RAMFS_IMPLICIT_DIR_MODE   00755u

struct ramfs_mount {
    const void          *image;
    size_t               image_size;
    struct ramfs_node   *nodes;
    size_t               node_count;
};

static struct ramfs_mount g_mount;

/* Phase G: the SPILLOVER node table for files/dirs created after mount.
 * A separate static array (not a realloc of m->nodes) because open
 * handles hold raw node pointers -- growth must never move live slots.
 * 256 creatable names on / is a documented honest cap; real churn
 * belongs on /tmp (tmpfs) and /data (tobyfs), both unbounded. A dead
 * spillover slot with no open refs is reused; dead TAR slots are not
 * (their names stay tombstoned so the tar image itself is never
 * consulted again for them). */
#define RAMFS_EXTRA_MAX 256
static struct ramfs_node g_extra[RAMFS_EXTRA_MAX];
static size_t            g_extra_count;

/* ---- helpers ---- */

static size_t round_up_block(size_t n) {
    return (n + USTAR_BLOCK - 1) & ~(size_t)(USTAR_BLOCK - 1);
}

/* USTAR size field: up to 12 chars of ASCII octal, terminated by NUL or
 * space. Returns 0 on parse failure -- which is fine, since 0-byte
 * files are still valid (and indistinguishable from a parse error from
 * the caller's perspective; either way there's nothing to read). */
static size_t parse_octal(const char *s, size_t len) {
    size_t v = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == 0 || c == ' ') break;
        if (c < '0' || c > '7') return 0;
        v = (v << 3) | (size_t)(c - '0');
    }
    return v;
}

static bool is_zero_block(const uint8_t *p) {
    for (size_t i = 0; i < USTAR_BLOCK; i++) if (p[i]) return false;
    return true;
}

/* Copy USTAR's null-or-100-bounded `name` field into `out` and trim
 * any trailing '/' (we keep the trailing-slash-as-directory hint via
 * typeflag instead). Returns false if the name overflows VFS_NAME_MAX. */
static bool copy_name(const char *src, char *out, size_t out_max) {
    size_t n = 0;
    while (n < USTAR_NAME_LEN && src[n] != 0) n++;
    if (n >= out_max) return false;
    for (size_t i = 0; i < n; i++) out[i] = src[i];
    while (n > 0 && out[n - 1] == '/') n--;
    out[n] = 0;
    return true;
}

/* Normalise a VFS path: strip leading '/', strip trailing '/' (unless
 * the path is exactly "/", which becomes ""). Writes into `out`; on
 * overflow returns false. */
static bool normalise_path(const char *path, char *out, size_t out_max) {
    if (!path) return false;
    while (*path == '/') path++;
    size_t n = 0;
    while (path[n]) {
        if (n + 1 >= out_max) return false;
        out[n] = path[n];
        n++;
    }
    while (n > 0 && out[n - 1] == '/') n--;
    out[n] = 0;
    return true;
}

/* True if `s` starts with `prefix`. */
static bool starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return false;
    }
    return true;
}

/* ---- mount: walk the tar image once ---- */

/* Two passes so we can exact-size the node array (and avoid relying on
 * heap realloc, which we don't have). First pass counts entries we'd
 * accept; second pass actually fills the table. */
static size_t count_entries(const uint8_t *img, size_t size) {
    size_t off = 0;
    size_t count = 0;
    while (off + USTAR_BLOCK <= size) {
        const struct ustar_header *h = (const struct ustar_header *)(img + off);
        if (is_zero_block((const uint8_t *)h)) break;
        if (memcmp(h->magic, USTAR_MAGIC, 5) != 0) break;
        size_t fsize = parse_octal(h->size, sizeof(h->size));
        char tf = h->typeflag;
        /* '0' / NUL = regular file, '5' = directory, '1' = hard link (see
         * the resolver in the fill pass). Still skipped: '2' symlinks (the
         * VFS has no readlink hook for a driver to implement), char/block
         * devices, fifos, GNU extensions. */
        if (tf == '0' || tf == 0 || tf == '5' || tf == '1') count++;
        off += USTAR_BLOCK + round_up_block(fsize);
    }
    return count;
}

int ramfs_mount(const void *image, size_t size) {
    if (!image || size < USTAR_BLOCK) return VFS_ERR_INVAL;
    const uint8_t *img = (const uint8_t *)image;

    /* Sanity: the very first header must look like USTAR. We accept
     * trailing garbage (most tar producers append two zero blocks
     * plus padding), but the first block has to be a real header. */
    const struct ustar_header *h0 = (const struct ustar_header *)img;
    if (memcmp(h0->magic, USTAR_MAGIC, 5) != 0) {
        kprintf("[ramfs] reject: first block lacks 'ustar' magic\n");
        return VFS_ERR_INVAL;
    }

    size_t n = count_entries(img, size);
    if (n == 0) {
        kprintf("[ramfs] reject: tar contains no usable entries\n");
        return VFS_ERR_INVAL;
    }

    struct ramfs_node *nodes = kcalloc(n, sizeof(*nodes));
    if (!nodes) {
        kprintf("[ramfs] OOM allocating %lu nodes\n", (unsigned long)n);
        return VFS_ERR_NOMEM;
    }

    size_t idx = 0;
    size_t off = 0;
    while (off + USTAR_BLOCK <= size && idx < n) {
        const struct ustar_header *h = (const struct ustar_header *)(img + off);
        if (is_zero_block((const uint8_t *)h)) break;
        if (memcmp(h->magic, USTAR_MAGIC, 5) != 0) break;

        size_t fsize = parse_octal(h->size, sizeof(h->size));
        char tf = h->typeflag;
        size_t data_off = off + USTAR_BLOCK;

        if (tf == '0' || tf == 0 || tf == '5' || tf == '1') {
            struct ramfs_node *nd = &nodes[idx];
            if (!copy_name(h->name, nd->name, sizeof(nd->name))) {
                kprintf("[ramfs] WARN: skipping entry with overlong name\n");
            } else if (nd->name[0] == 0) {
                /* root entry "./" or similar -- skip silently */
            } else if (tf == '1') {
                /* Hard link ('1'): no data of its own -- `linkname` names an
                 * entry that appeared EARLIER in the archive, so a single
                 * forward pass can resolve it against what we have already
                 * parsed. Share the target's bytes; sharing a pointer into
                 * the tar image is exactly right, since a hard link IS the
                 * same data under a second name and the image is read-only.
                 *
                 * The self-referential case (name == linkname) is not a real
                 * link: GNU tar emits it when the SAME staged file is passed
                 * more than once in one archive's member list, which this
                 * build does via overlapping $EXTRA_FILES trees. The regular
                 * entry is already in the table, so these are pure
                 * duplicates -- drop them rather than add a shadowing node.
                 * All 14 hard links in the current initrd are this case. */
                char target[VFS_PATH_MAX];
                if (!copy_name(h->linkname, target, sizeof(target))) {
                    kprintf("[ramfs] WARN: hard link '%s' has an overlong "
                            "target\n", nd->name);
                } else if (strcmp(target, nd->name) == 0) {
                    /* tar's duplicate-member dedup -- silently ignore. */
                } else {
                    struct ramfs_node *tgt = 0;
                    for (size_t j = 0; j < idx; j++) {
                        if (strcmp(nodes[j].name, target) == 0) {
                            tgt = &nodes[j];
                            break;
                        }
                    }
                    if (!tgt) {
                        /* Forward reference or a target we skipped. Warn --
                         * silently dropping it is how a file goes missing
                         * with no evidence. */
                        kprintf("[ramfs] WARN: hard link '%s' -> '%s': target "
                                "not found, entry dropped\n",
                                nd->name, target);
                    } else {
                        nd->type = tgt->type;
                        nd->data = tgt->data;
                        nd->size = tgt->size;
                        /* A hard link shares the inode, so it shares the
                         * inode's mode and ownership too -- the link's own
                         * header fields are not authoritative. */
                        nd->mode = tgt->mode;
                        nd->uid  = tgt->uid;
                        nd->gid  = tgt->gid;
                        nd->mtime = tgt->mtime;
                        nd->atime = tgt->atime;
                        idx++;
                    }
                }
            } else {
                /* USTAR mode/uid/gid are ASCII octal. parse_octal returns 0
                 * for an unparseable field, and a 0 mode is indistinguishable
                 * from that -- so treat 0 as "no usable mode" and fall back.
                 * uid/gid legitimately ARE 0 (root), so they need no such
                 * guard: an absent field simply reads as root, which is the
                 * right answer for an initrd. */
                uint32_t tmode = (uint32_t)parse_octal(h->mode, sizeof(h->mode));
                nd->uid = (uint32_t)parse_octal(h->uid, sizeof(h->uid));
                nd->gid = (uint32_t)parse_octal(h->gid, sizeof(h->gid));
                /* USTAR mtime: octal epoch seconds, the archive's own
                 * record of when the file was last written. */
                nd->mtime = (uint64_t)parse_octal(h->mtime, sizeof(h->mtime));
                nd->atime = nd->mtime;

                if (tf == '5') {
                    nd->type = VFS_TYPE_DIR;
                    nd->data = 0;
                    nd->size = 0;
                    nd->mode = tmode ? (tmode & VFS_MODE_PERMS)
                                     : RAMFS_FALLBACK_DIR_MODE;
                } else {
                    nd->mode = tmode ? (tmode & VFS_MODE_PERMS)
                                     : RAMFS_FALLBACK_FILE_MODE;
                    if (data_off + fsize > size) {
                        kprintf("[ramfs] reject: '%s' file data runs off "
                                "image (%lu+%lu > %lu)\n",
                                nd->name, (unsigned long)data_off,
                                (unsigned long)fsize, (unsigned long)size);
                        kfree(nodes);
                        return VFS_ERR_INVAL;
                    }
                    nd->type = VFS_TYPE_FILE;
                    nd->data = img + data_off;
                    nd->size = fsize;
                }
                idx++;
            }
        }
        off += USTAR_BLOCK + round_up_block(fsize);
    }

    g_mount.image      = image;
    g_mount.image_size = size;
    g_mount.nodes      = nodes;
    g_mount.node_count = idx;

    extern const struct vfs_ops ramfs_ops;   /* defined below */
    int rc = vfs_mount("/", &ramfs_ops, &g_mount);
    if (rc != VFS_OK) {
        kfree(nodes);
        g_mount.nodes      = 0;
        g_mount.node_count = 0;
        return rc;
    }

    kprintf("[ramfs] mounted: %lu entries from %lu-byte tar at %p\n",
            (unsigned long)idx, (unsigned long)size, image);
    /* The per-file dump is ~240 lines on a full initrd -- on a real
     * framebuffer console that is ~20s of scrolling AND it pushes the
     * later, interesting boot stages (net/DHCP, desktop/login) past the
     * end of any practical serial capture. Keep it opt-in. */
#ifdef RAMFS_VERBOSE_LISTING
    for (size_t i = 0; i < idx; i++) {
        kprintf("  %s %-32s  %lu B  data@%p\n",
                nodes[i].type == VFS_TYPE_DIR ? "d" : "-",
                nodes[i].name,
                (unsigned long)nodes[i].size,
                nodes[i].data);
    }
#endif
    return VFS_OK;
}

size_t ramfs_node_count(void) { return g_mount.node_count; }

/* ---- vfs_ops impl ---- */

/* Phase G: one iteration space over BOTH node tables (tar + spillover).
 * Every walker below goes through these two, so a dead node disappears
 * from lookup/listing everywhere at once. */
static size_t node_total(struct ramfs_mount *m) {
    return m->node_count + g_extra_count;
}
static struct ramfs_node *node_at(struct ramfs_mount *m, size_t i) {
    if (i < m->node_count) return &m->nodes[i];
    return &g_extra[i - m->node_count];
}

/* Find the NAME -- what unlink, rename and link manipulate. */
static struct ramfs_node *find_node_name(struct ramfs_mount *m,
                                         const char *norm_path) {
    for (size_t i = 0; i < node_total(m); i++) {
        struct ramfs_node *nd = node_at(m, i);
        if (!nd->dead && strcmp(nd->name, norm_path) == 0) return nd;
    }
    return 0;
}

/* Follow a name to the node that owns the bytes. */
static struct ramfs_node *ramfs_resolve(struct ramfs_mount *m,
                                        struct ramfs_node *nm) {
    if (!nm || !nm->link_to) return nm;
    size_t idx = (size_t)nm->link_to - 1;
    if (idx >= node_total(m)) return nm;         /* corrupt: fail closed */
    struct ramfs_node *ino = node_at(m, idx);
    /* A dead owner that still has links is NOT gone -- its own name was
     * unlinked but the bytes belong to the remaining names. Only a dead
     * owner with no links left is a genuinely stale target. */
    return (ino->dead && ino->nlink == 0) ? nm : ino;
}

/* Find the DATA -- what everything that reads or writes wants, so every
 * existing caller works through a link with no change. */
static struct ramfs_node *find_node(struct ramfs_mount *m,
                                    const char *norm_path) {
    return ramfs_resolve(m, find_node_name(m, norm_path));
}

/* True if any node lives under `norm_path/`. Used to recognise implicit
 * directories (e.g. opendir("/bin") works even if no explicit "bin/"
 * entry was tar'd). */
static bool has_children(struct ramfs_mount *m, const char *norm_path) {
    size_t plen = strlen(norm_path);
    for (size_t i = 0; i < node_total(m); i++) {
        struct ramfs_node *nd = node_at(m, i);
        if (nd->dead) continue;
        const char *name = nd->name;
        if (plen == 0) return true;     /* root always has children */
        if (strncmp(name, norm_path, plen) == 0 && name[plen] == '/') {
            return true;
        }
    }
    return false;
}

static int ramfs_open(void *mnt, const char *path, struct vfs_file *out) {
    struct ramfs_mount *m = (struct ramfs_mount *)mnt;
    char norm[VFS_PATH_MAX];
    if (!normalise_path(path, norm, sizeof(norm))) return VFS_ERR_INVAL;

    struct ramfs_node *nd = find_node(m, norm);
    if (!nd) return VFS_ERR_NOENT;
    if (nd->type != VFS_TYPE_FILE) return VFS_ERR_ISDIR;

    out->priv = nd;
    out->pos  = 0;
    out->size = nd->size;
    /* The node's real identity, as recorded in the tar. MODE_VALID forces
     * the VFS to enforce these bits explicitly rather than treat the inode
     * as "no permission info". (This comment used to say ramfs left every
     * write-side op NULL so the mode bits could not matter -- true when it
     * was written, false since Phase G made the root writable and doubly so
     * now that the rest of the CRUD ops are wired below.) */
    out->uid  = nd->uid;
    out->gid  = nd->gid;
    out->mode = nd->mode | VFS_MODE_VALID;
    nd->open_refs++;                   /* Phase G: gates unlink teardown */
    return VFS_OK;
}

static int ramfs_close(struct vfs_file *f) {
    struct ramfs_node *nd = (struct ramfs_node *)f->priv;
    /* Phase G: last close of an unlinked node frees its owned bytes and
     * (for a spillover slot) makes the slot reusable. Tar-backed dead
     * nodes keep their tombstone -- the image bytes were never ours. */
    if (nd && nd->open_refs > 0 && --nd->open_refs == 0 && nd->dead &&
        nd->nlink == 0) {
        if (nd->owned && nd->data) kfree((void *)nd->data);
        nd->data  = 0;
        nd->size  = 0;
        nd->cap   = 0;
        nd->owned = false;
    }
    f->priv = 0;
    return VFS_OK;
}

/* Slice 112: hand out the open file's payload pointer (see ramfs.h).
 * The node was validated by ramfs_open; the tar bytes never move. */
int ramfs_file_data(struct vfs_file *f, const void **data, size_t *size) {
    struct ramfs_node *nd = f ? (struct ramfs_node *)f->priv : 0;
    if (!nd || nd->type != VFS_TYPE_FILE) return VFS_ERR_INVAL;
    if (data) *data = nd->data;
    if (size) *size = nd->size;
    return VFS_OK;
}

static long ramfs_read(struct vfs_file *f, void *buf, size_t n) {
    struct ramfs_node *nd = (struct ramfs_node *)f->priv;
    if (!nd) return VFS_ERR_INVAL;
    if (f->pos >= nd->size) return 0;
    size_t avail = nd->size - f->pos;
    if (n > avail) n = avail;
    memcpy(buf, (const uint8_t *)nd->data + f->pos, n);
    f->pos += n;
    return (long)n;
}

/* Per-opendir state: a heap array of pre-built dirents. The driver
 * builds it once at opendir time so readdir is a tight loop. */
struct ramfs_diriter {
    struct vfs_dirent *ents;
    size_t             count;
};

/* Helper used while building a dirent list: append `name` of `type`
 * unless we already have an entry with the same name. Returns false
 * on overflow (caller bumped the buffer too small). */
static bool dirent_push_unique(struct vfs_dirent *list, size_t *n,
                               size_t cap,
                               const char *name, enum vfs_type type,
                               size_t size,
                               uint32_t mode, uint32_t uid, uint32_t gid) {
    for (size_t i = 0; i < *n; i++) {
        if (strcmp(list[i].name, name) == 0) return true;   /* dedup */
    }
    if (*n >= cap) return false;
    struct vfs_dirent *e = &list[*n];
    size_t nl = strlen(name);
    if (nl >= sizeof(e->name)) nl = sizeof(e->name) - 1;
    memcpy(e->name, name, nl);
    e->name[nl] = 0;
    e->type = type;
    e->size = size;
    /* Carried from the node (or the caller's synthesised values for an
     * implicit directory) -- readdir must agree with stat, or `ls -l` and
     * stat() describe the same file differently. */
    e->uid  = uid;
    e->gid  = gid;
    e->mode = mode | VFS_MODE_VALID;
    (*n)++;
    return true;
}

static int ramfs_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    struct ramfs_mount *m = (struct ramfs_mount *)mnt;
    char norm[VFS_PATH_MAX];
    if (!normalise_path(path, norm, sizeof(norm))) return VFS_ERR_INVAL;

    /* Confirm the target really is a directory (explicit or implicit).
     * Root ("") is always valid. */
    if (norm[0] != 0) {
        struct ramfs_node *nd = find_node(m, norm);
        if (nd) {
            if (nd->type != VFS_TYPE_DIR) return VFS_ERR_NOTDIR;
        } else if (!has_children(m, norm)) {
            return VFS_ERR_NOENT;
        }
    }

    /* Worst case is one dirent per node, so allocate that many up front
     * -- node counts in the initrd are small (single digits). */
    struct ramfs_diriter *it = kmalloc(sizeof(*it));
    if (!it) return VFS_ERR_NOMEM;
    size_t cap = node_total(m) > 0 ? node_total(m) : 1;
    it->ents = kcalloc(cap, sizeof(*it->ents));
    if (!it->ents) { kfree(it); return VFS_ERR_NOMEM; }
    it->count = 0;

    size_t plen = strlen(norm);
    for (size_t i = 0; i < node_total(m); i++) {
        struct ramfs_node *ind = node_at(m, i);
        if (ind->dead) continue;       /* Phase G: unlinked names vanish */
        const char *name = ind->name;
        const char *rest;

        if (plen == 0) {
            rest = name;            /* root: every node is a candidate */
        } else {
            if (!starts_with(name, norm)) continue;
            if (name[plen] != '/') continue;
            rest = name + plen + 1;
        }
        if (*rest == 0) continue;   /* the dir itself, not a child */

        /* Find first '/' in `rest`. If absent, it's a direct child;
         * otherwise it's an implicit subdirectory. */
        const char *slash = rest;
        while (*slash && *slash != '/') slash++;

        if (*slash == 0) {
            /* Direct child entry -- report the node's own recorded mode.
             * A hard link carries no bytes of its own, so its size and
             * ownership come from the node it points at. */
            struct ramfs_node *ent = ramfs_resolve(m, ind);
            if (!dirent_push_unique(it->ents, &it->count, cap, rest,
                                    ent->type, ent->size,
                                    ent->mode, ent->uid,
                                    ent->gid)) {
                break;
            }
        } else {
            /* Implicit directory: emit the leading component. There is no
             * tar header for it, so it gets the conventional root-owned
             * 0755 -- matching what ramfs_stat() synthesises for the same
             * path, which is the pairing that keeps readdir and stat
             * consistent. */
            size_t leaf_len = (size_t)(slash - rest);
            char leaf[VFS_NAME_MAX];
            if (leaf_len >= sizeof(leaf)) leaf_len = sizeof(leaf) - 1;
            memcpy(leaf, rest, leaf_len);
            leaf[leaf_len] = 0;
            if (!dirent_push_unique(it->ents, &it->count, cap, leaf,
                                    VFS_TYPE_DIR, 0,
                                    RAMFS_IMPLICIT_DIR_MODE, 0, 0)) {
                break;
            }
        }
    }

    out->priv  = it;
    out->index = 0;
    return VFS_OK;
}

static int ramfs_closedir(struct vfs_dir *d) {
    struct ramfs_diriter *it = (struct ramfs_diriter *)d->priv;
    if (it) {
        if (it->ents) kfree(it->ents);
        kfree(it);
    }
    d->priv = 0;
    return VFS_OK;
}

static int ramfs_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    struct ramfs_diriter *it = (struct ramfs_diriter *)d->priv;
    if (!it) return VFS_ERR_INVAL;
    if (d->index >= it->count) return VFS_ERR_NOENT;   /* end of stream */
    *out = it->ents[d->index++];
    return VFS_OK;
}

static int ramfs_stat(void *mnt, const char *path, struct vfs_stat *out) {
    struct ramfs_mount *m = (struct ramfs_mount *)mnt;
    char norm[VFS_PATH_MAX];
    if (!normalise_path(path, norm, sizeof(norm))) return VFS_ERR_INVAL;

    /* nlink was never assigned on ANY of the three paths below, so `ls -l`
     * printed whatever junk the caller happened to have on its stack -- a
     * visible "-r--r--r-- 1919954296 ..." (those digits are ASCII bytes read
     * as an integer). Per struct vfs_stat, 0 means "this fs has no answer"
     * and the stat emitters substitute the conventional 1-for-files /
     * 2-for-directories, so setting 0 routes through that shared default
     * rather than duplicating the policy here. */
    out->nlink = 0;
    /* Same reasoning as nlink, and the same defect: mtime/atime/ctime were
     * never assigned on any arm, so every ramfs stat handed back whatever
     * the caller had on its stack. 0 is the honest answer for a path with
     * no recorded time; a real one is filled in below where we have it. */
    out->mtime = 0;
    out->atime = 0;
    out->ctime = 0;

    /* The mount root has no tar header of its own -- same synthesised 0755
     * as any other implicit directory. */
    if (norm[0] == 0) {
        out->type = VFS_TYPE_DIR;
        out->size = 0;
        out->uid  = 0;
        out->gid  = 0;
        out->mode = RAMFS_IMPLICIT_DIR_MODE | VFS_MODE_VALID;
        return VFS_OK;
    }

    struct ramfs_node *nd = find_node(m, norm);
    if (nd) {
        out->type = nd->type;
        out->size = nd->size;
        out->uid  = nd->uid;
        out->gid  = nd->gid;
        out->mode = nd->mode | VFS_MODE_VALID;
        out->mtime = nd->mtime;
        out->atime = nd->atime;
        out->ctime = nd->mtime;
        /* A real count once hard links exist; 0 still means "no answer"
         * for tar-loaded nodes, which routes through the shared default. */
        if (nd->nlink > 1) out->nlink = nd->nlink;
        return VFS_OK;
    }
    /* Implicit directory? */
    if (has_children(m, norm)) {
        out->type = VFS_TYPE_DIR;
        out->size = 0;
        out->uid  = 0;
        out->gid  = 0;
        out->mode = RAMFS_IMPLICIT_DIR_MODE | VFS_MODE_VALID;
        return VFS_OK;
    }
    return VFS_ERR_NOENT;
}

/* ---- writes: COPY-ON-WRITE onto the heap ------------------------------
 *
 * WHY THIS EXISTS. A file's bytes normally point straight INTO the Limine
 * initrd module (zero-copy: nd->data = img + off), which is why ramfs was
 * read-only. But a read-only root is a DEVIATION, not a design: Linux's
 * initramfs is a writable tmpfs, and the deviation had a real cost. DHCP
 * learns the nameserver and net_write_resolv_conf() rewrites
 * /etc/resolv.conf -- except the write silently failed here, so the resolver
 * kept using the address baked into the initrd. In QEMU that address is
 * accidentally correct (SLIRP hands out exactly the 10.0.2.3 the file names);
 * on real hardware every name lookup went to a host that does not exist and
 * Chromium reported ERR_NAME_NOT_RESOLVED with nothing in the log to explain
 * it.
 *
 * SCOPE: originally narrowed to overwrite-in-place only ("you cannot add
 * to /"). Phase G widened it -- create/unlink/mkdir now work through the
 * spillover table (see g_extra) -- so this CoW machinery serves both
 * pre-existing tar files and newly created ones.
 *
 * The first write to a tar-backed node kmallocs a private buffer and repoints
 * `data` at it; `owned` then says the buffer is ours to free and grow. The
 * initrd image itself is NEVER modified -- it is shared, and on a real boot it
 * is the module Limine loaded. */
static int ramfs_cow(struct ramfs_node *nd, size_t need) {
    if (nd->owned && need <= nd->cap) return VFS_OK;
    size_t cap = nd->owned ? nd->cap : 0;
    if (cap < need) cap = need < 64 ? 64 : need;
    uint8_t *buf = (uint8_t *)kmalloc(cap);
    if (!buf) return VFS_ERR_NOMEM;
    memset(buf, 0, cap);
    if (nd->data && nd->size) {
        size_t keep = nd->size < cap ? nd->size : cap;
        memcpy(buf, nd->data, keep);
    }
    if (nd->owned && nd->data) kfree((void *)nd->data);
    nd->data  = buf;
    nd->cap   = cap;
    nd->owned = true;
    return VFS_OK;
}

static long ramfs_write(struct vfs_file *f, const void *buf, size_t n) {
    struct ramfs_node *nd = (struct ramfs_node *)f->priv;
    if (!nd) return VFS_ERR_INVAL;
    if (nd->type != VFS_TYPE_FILE) return VFS_ERR_ISDIR;
    if (n == 0) return 0;
    size_t need = f->pos + n;
    if (need < f->pos) return VFS_ERR_INVAL;          /* offset overflow */
    int rc = ramfs_cow(nd, need);
    if (rc != VFS_OK) return rc;
    /* A write past the old end leaves a hole when f->pos > nd->size (lseek
     * beyond EOF then write). ramfs_cow zeroes a buffer it has just
     * allocated, but returns untouched when the node already owns enough
     * capacity -- so the gap has to be zeroed explicitly or it would read
     * back whatever the file used to hold there. */
    if (f->pos > nd->size)
        memset((uint8_t *)nd->data + nd->size, 0, f->pos - nd->size);
    memcpy((uint8_t *)nd->data + f->pos, buf, n);
    f->pos += n;
    if (need > nd->size) nd->size = need;
    f->size = nd->size;
    nd->mtime = ramfs_now_secs();
    return (long)n;
}

/* Set a node's length exactly, zero-filling any growth (POSIX). Shared by
 * truncate(2) and ftruncate(2). */
static int ramfs_set_len(struct ramfs_node *nd, uint64_t length) {
    size_t want = (size_t)length;
    if ((uint64_t)want != length) return VFS_ERR_NOSPC;   /* > SIZE_MAX */
    /* ramfs_cow(nd, 0) would kmalloc(0); ask for one byte instead. */
    int rc = ramfs_cow(nd, want ? want : 1);
    if (rc != VFS_OK) return rc;
    if (want > nd->size)
        memset((uint8_t *)nd->data + nd->size, 0, want - nd->size);
    nd->size  = want;
    nd->mtime = ramfs_now_secs();
    return VFS_OK;
}

/* ---- Phase G: the WRITABLE root ---------------------------------------
 *
 * Linux's initramfs is a writable tmpfs; ours was a read-only tar view
 * with a truncate-in-place patch (the resolv.conf slice above). That
 * deviation had a second cost the first one hid: nothing could CREATE a
 * file or directory anywhere ramfs is mounted, so /etc could not gain
 * files (ld.so.cache, machine-id, dhcp leases), and every "make a
 * scratch file next to the config" idiom failed with EROFS. Widened
 * here: create/unlink/mkdir work, backed by the spillover table --
 * address-stable, capped, reusable slots (see g_extra above). */

/* A spillover slot for a NEW name: a dead slot with no open refs is
 * recycled, else the table grows. NULL when the cap is hit. */
static struct ramfs_node *extra_alloc(void) {
    for (size_t i = 0; i < g_extra_count; i++)
        /* nlink > 0 means some other NAME still points at this slot by
         * index; handing it out would silently re-point that name at a
         * different file. */
        if (g_extra[i].dead && g_extra[i].open_refs == 0 &&
            g_extra[i].nlink == 0) {
            memset(&g_extra[i], 0, sizeof(g_extra[i]));
            return &g_extra[i];
        }
    if (g_extra_count >= RAMFS_EXTRA_MAX) return 0;
    return &g_extra[g_extra_count++];
}

/* The parent of `norm` must exist as a directory (explicit, implicit, or
 * the root). Rejecting orphans here keeps readdir's implicit-directory
 * inference sound -- a child under a never-created parent would conjure
 * the parent into listings. */
static int parent_dir_ok(struct ramfs_mount *m, const char *norm) {
    size_t plen = strlen(norm);
    while (plen > 0 && norm[plen - 1] != '/') plen--;
    if (plen == 0) return VFS_OK;                    /* direct child of / */
    char parent[VFS_PATH_MAX];
    memcpy(parent, norm, plen - 1);
    parent[plen - 1] = 0;
    struct ramfs_node *pd = find_node(m, parent);
    if (pd) return pd->type == VFS_TYPE_DIR ? VFS_OK : VFS_ERR_NOTDIR;
    return has_children(m, parent) ? VFS_OK : VFS_ERR_NOENT;
}

/* vfs_write_all() calls create() before open(), and treats VFS_ERR_EXIST as
 * "fine, carry on" -- so the EXIST arm TRUNCATES, matching the contract
 * vfs_write_all documents. Without the truncate a shorter replacement
 * would leave the tail of the old contents behind, which for resolv.conf
 * would mean two nameserver lines. New names allocate a spillover node. */
static int ramfs_create(void *mnt, const char *path,
                        uint32_t uid, uint32_t gid, uint32_t mode) {
    struct ramfs_mount *m = (struct ramfs_mount *)mnt;
    char norm[VFS_PATH_MAX];
    if (!normalise_path(path, norm, sizeof(norm))) return VFS_ERR_INVAL;
    if (norm[0] == 0) return VFS_ERR_ISDIR;
    if (strlen(norm) >= VFS_NAME_MAX) return VFS_ERR_NAMETOOLONG;
    struct ramfs_node *nd = find_node(m, norm);
    if (nd) {
        if (nd->type != VFS_TYPE_FILE) return VFS_ERR_ISDIR;
        int rc = ramfs_cow(nd, 1);
        if (rc != VFS_OK) return rc;
        nd->size = 0;                                        /* truncate */
        return VFS_ERR_EXIST;
    }
    int prc = parent_dir_ok(m, norm);
    if (prc != VFS_OK) return prc;
    nd = extra_alloc();
    if (!nd) return VFS_ERR_NOSPC;
    memcpy(nd->name, norm, strlen(norm) + 1);
    nd->type  = VFS_TYPE_FILE;
    nd->mode  = mode & VFS_MODE_PERMS;
    nd->uid   = uid;
    nd->gid   = gid;
    nd->nlink = 1;
    nd->mtime = ramfs_now_secs();
    nd->atime = nd->mtime;
    return VFS_OK;
}

/* Phase G: a second directory entry for the same bytes. */
static int ramfs_link(void *mnt, const char *oldpath, const char *newpath) {
    struct ramfs_mount *m = (struct ramfs_mount *)mnt;
    char o[VFS_PATH_MAX], nw[VFS_PATH_MAX];
    if (!normalise_path(oldpath, o, sizeof(o)) ||
        !normalise_path(newpath, nw, sizeof(nw))) return VFS_ERR_INVAL;
    if (o[0] == 0 || nw[0] == 0) return VFS_ERR_INVAL;
    if (strlen(nw) >= VFS_NAME_MAX) return VFS_ERR_NAMETOOLONG;

    struct ramfs_node *nm = find_node_name(m, o);
    if (!nm) return VFS_ERR_NOENT;
    struct ramfs_node *ino = ramfs_resolve(m, nm);
    /* POSIX forbids hard links to directories -- they make the tree
     * cyclic and every walker that trusts it loops forever. */
    if (ino->type == VFS_TYPE_DIR) return VFS_ERR_ISDIR;
    if (find_node_name(m, nw) || has_children(m, nw)) return VFS_ERR_EXIST;
    int prc = parent_dir_ok(m, nw);
    if (prc != VFS_OK) return prc;

    /* The owner's index in the combined (initrd + spillover) space. */
    size_t owner_idx = (size_t)-1;
    for (size_t i = 0; i < node_total(m); i++)
        if (node_at(m, i) == ino) { owner_idx = i; break; }
    if (owner_idx == (size_t)-1) return VFS_ERR_IO;

    struct ramfs_node *nn = extra_alloc();
    if (!nn) return VFS_ERR_NOSPC;
    memcpy(nn->name, nw, strlen(nw) + 1);
    nn->type    = ino->type;
    nn->mode    = ino->mode;
    nn->uid     = ino->uid;
    nn->gid     = ino->gid;
    nn->mtime   = ino->mtime;
    nn->atime   = ino->atime;
    nn->link_to = (uint32_t)owner_idx + 1u;
    ino->nlink  = (ino->nlink ? ino->nlink : 1u) + 1u;
    return VFS_OK;
}

static int ramfs_unlink(void *mnt, const char *path) {
    struct ramfs_mount *m = (struct ramfs_mount *)mnt;
    char norm[VFS_PATH_MAX];
    if (!normalise_path(path, norm, sizeof(norm))) return VFS_ERR_INVAL;
    if (norm[0] == 0) return VFS_ERR_INVAL;          /* not the root */
    struct ramfs_node *nm = find_node_name(m, norm);
    if (!nm) return VFS_ERR_NOENT;
    if (nm->type == VFS_TYPE_DIR && has_children(m, norm))
        return VFS_ERR_INVAL;                        /* non-empty dir */

    struct ramfs_node *nd = ramfs_resolve(m, nm);
    if (nd != nm) {
        /* Dropping an extra NAME: the bytes belong to another slot and
         * must outlive this entry. */
        nm->dead = true;
        nm->link_to = 0;
        if (nd->nlink) nd->nlink--;
        if (nd->nlink == 0 && nd->open_refs == 0 && nd->dead) {
            if (nd->owned && nd->data) kfree((void *)nd->data);
            nd->data = 0; nd->size = 0; nd->cap = 0; nd->owned = false;
        }
        return VFS_OK;
    }
    if (nd->nlink > 1) {
        /* Other names still point at this slot BY INDEX, so the bytes
         * stay; only this name goes away. */
        nd->nlink--;
        nd->dead = true;
        return VFS_OK;
    }
    if (nd->nlink) nd->nlink--;
    nd->dead = true;
    if (nd->open_refs == 0) {
        /* No handle outstanding: reclaim owned bytes now. With handles
         * open, ramfs_close's last-close arm does this instead --
         * unlink-while-open keeps the bytes readable, as POSIX wants. */
        if (nd->owned && nd->data) kfree((void *)nd->data);
        nd->data  = 0;
        nd->size  = 0;
        nd->cap   = 0;
        nd->owned = false;
    }
    return VFS_OK;
}

static int ramfs_mkdir(void *mnt, const char *path,
                       uint32_t uid, uint32_t gid, uint32_t mode) {
    struct ramfs_mount *m = (struct ramfs_mount *)mnt;
    char norm[VFS_PATH_MAX];
    if (!normalise_path(path, norm, sizeof(norm))) return VFS_ERR_INVAL;
    if (norm[0] == 0) return VFS_ERR_EXIST;
    if (strlen(norm) >= VFS_NAME_MAX) return VFS_ERR_NAMETOOLONG;
    if (find_node(m, norm) || has_children(m, norm)) return VFS_ERR_EXIST;
    int prc = parent_dir_ok(m, norm);
    if (prc != VFS_OK) return prc;
    struct ramfs_node *nd = extra_alloc();
    if (!nd) return VFS_ERR_NOSPC;
    memcpy(nd->name, norm, strlen(norm) + 1);
    nd->type = VFS_TYPE_DIR;
    nd->mode = mode & VFS_MODE_PERMS;
    nd->uid  = uid;
    nd->gid  = gid;
    nd->mtime = ramfs_now_secs();
    nd->atime = nd->mtime;
    return VFS_OK;
}

static int ramfs_truncate(void *mnt, const char *path, uint64_t length) {
    struct ramfs_mount *m = (struct ramfs_mount *)mnt;
    char norm[VFS_PATH_MAX];
    if (!normalise_path(path, norm, sizeof(norm))) return VFS_ERR_INVAL;
    struct ramfs_node *nd = find_node(m, norm);
    if (!nd) return VFS_ERR_NOENT;
    if (nd->type != VFS_TYPE_FILE) return VFS_ERR_ISDIR;
    return ramfs_set_len(nd, length);
}

static int ramfs_ftruncate(struct vfs_file *f, uint64_t length) {
    struct ramfs_node *nd = (struct ramfs_node *)f->priv;
    if (!nd) return VFS_ERR_INVAL;
    if (nd->type != VFS_TYPE_FILE) return VFS_ERR_ISDIR;
    int rc = ramfs_set_len(nd, length);
    if (rc == VFS_OK) f->size = nd->size;
    return rc;
}

/* chmod/chown/utimes need somewhere to PUT the value, and an implicit
 * directory (one the tar never named, inferred from its children) has no
 * node at all. Reporting ROFS there is the honest answer -- the
 * alternative, accepting the call and dropping the value, is the lie
 * utimensat used to tell across the whole VFS. */
static int ramfs_chmod(void *mnt, const char *path, uint32_t mode) {
    struct ramfs_mount *m = (struct ramfs_mount *)mnt;
    char norm[VFS_PATH_MAX];
    if (!normalise_path(path, norm, sizeof(norm))) return VFS_ERR_INVAL;
    struct ramfs_node *nd = find_node(m, norm);
    if (!nd) return has_children(m, norm) ? VFS_ERR_ROFS : VFS_ERR_NOENT;
    nd->mode = mode & VFS_MODE_PERMS;
    return VFS_OK;
}

static int ramfs_chown(void *mnt, const char *path, uint32_t uid, uint32_t gid) {
    struct ramfs_mount *m = (struct ramfs_mount *)mnt;
    char norm[VFS_PATH_MAX];
    if (!normalise_path(path, norm, sizeof(norm))) return VFS_ERR_INVAL;
    struct ramfs_node *nd = find_node(m, norm);
    if (!nd) return has_children(m, norm) ? VFS_ERR_ROFS : VFS_ERR_NOENT;
    if (uid != (uint32_t)-1) nd->uid = uid;
    if (gid != (uint32_t)-1) nd->gid = gid;
    return VFS_OK;
}

static int ramfs_utimes(void *mnt, const char *path,
                        uint64_t mtime, uint64_t atime) {
    struct ramfs_mount *m = (struct ramfs_mount *)mnt;
    char norm[VFS_PATH_MAX];
    if (!normalise_path(path, norm, sizeof(norm))) return VFS_ERR_INVAL;
    struct ramfs_node *nd = find_node(m, norm);
    if (!nd) return has_children(m, norm) ? VFS_ERR_ROFS : VFS_ERR_NOENT;
    nd->mtime = mtime;
    nd->atime = atime;
    return VFS_OK;
}

/* Rename within the mount. Paths are stored whole, so moving a DIRECTORY
 * means rewriting every descendant's name too -- otherwise `mv a b` would
 * strand every child under a parent that no longer exists (tmpfs_rename
 * learned the same lesson). */
static int ramfs_rename(void *mnt, const char *oldpath, const char *newpath) {
    struct ramfs_mount *m = (struct ramfs_mount *)mnt;
    char o[VFS_PATH_MAX], nw[VFS_PATH_MAX];
    if (!normalise_path(oldpath, o, sizeof(o)) ||
        !normalise_path(newpath, nw, sizeof(nw))) return VFS_ERR_INVAL;
    if (o[0] == 0 || nw[0] == 0) return VFS_ERR_INVAL;    /* not the root */
    if (strcmp(o, nw) == 0) return VFS_OK;
    /* Renaming moves a NAME. Resolving to the owner here would rename the
     * file a hard link points at instead of the link itself -- and if that
     * owner had already been unlinked, it would rename a node that lookups
     * deliberately skip, so the new name would not exist. */
    struct ramfs_node *src = find_node_name(m, o);
    if (!src) return VFS_ERR_NOENT;
    size_t ol = strlen(o), nl = strlen(nw);
    if (nl >= VFS_NAME_MAX) return VFS_ERR_NAMETOOLONG;
    /* Refuse to move a directory inside itself: the subtree rewrite below
     * would chase its own tail, and POSIX says EINVAL anyway. */
    if (src->type == VFS_TYPE_DIR &&
        strncmp(nw, o, ol) == 0 && nw[ol] == '/') return VFS_ERR_INVAL;

    int prc = parent_dir_ok(m, nw);
    if (prc != VFS_OK) return prc;

    struct ramfs_node *dst = find_node_name(m, nw);
    if (dst) {                                   /* POSIX: replace */
        if (dst == src) return VFS_OK;
        if (dst->type == VFS_TYPE_DIR && has_children(m, nw))
            return VFS_ERR_INVAL;                /* target dir not empty */
        int rc = ramfs_unlink(mnt, nw);
        if (rc != VFS_OK) return rc;
    }

    if (src->type == VFS_TYPE_DIR) {
        /* Bound-check every descendant BEFORE moving anything: a rewrite
         * that fails halfway would leave the tree split across two names. */
        for (size_t i = 0; i < node_total(m); i++) {
            struct ramfs_node *c = node_at(m, i);
            if (c->dead || c == src) continue;
            if (strncmp(c->name, o, ol) != 0 || c->name[ol] != '/') continue;
            if (nl + strlen(c->name + ol) >= VFS_NAME_MAX)
                return VFS_ERR_NAMETOOLONG;
        }
        for (size_t i = 0; i < node_total(m); i++) {
            struct ramfs_node *c = node_at(m, i);
            if (c->dead || c == src) continue;
            if (strncmp(c->name, o, ol) != 0 || c->name[ol] != '/') continue;
            char moved[VFS_NAME_MAX];
            size_t rest = strlen(c->name + ol);
            memcpy(moved, nw, nl);
            memcpy(moved + nl, c->name + ol, rest + 1);
            memcpy(c->name, moved, nl + rest + 1);
        }
    }
    memcpy(src->name, nw, nl + 1);
    return VFS_OK;
}

/* Phase H: honest numbers for the initrd view -- the image is the
 * "disk", the spillover table is the only growth room. */
static int ramfs_statfs(void *mnt, struct vfs_statfs *out) {
    struct ramfs_mount *m = (struct ramfs_mount *)mnt;
    size_t free_slots = RAMFS_EXTRA_MAX - g_extra_count;
    for (size_t i = 0; i < g_extra_count; i++)
        if (g_extra[i].dead && g_extra[i].open_refs == 0) free_slots++;
    out->bsize      = 4096;
    out->blocks     = (m->image_size + 4095) / 4096;
    out->bfree      = 0;                     /* the tar itself never grows */
    out->files      = node_total(m);
    out->ffree      = free_slots;
    out->type_magic = 0x858458f6;            /* RAMFS_MAGIC */
    out->namelen    = VFS_NAME_MAX - 1;
    return VFS_OK;
}

const struct vfs_ops ramfs_ops = {
    .open     = ramfs_open,
    .close    = ramfs_close,
    .read     = ramfs_read,
    .write    = ramfs_write,
    .create   = ramfs_create,
    .unlink   = ramfs_unlink,   /* Phase G: writable root */
    .mkdir    = ramfs_mkdir,    /* Phase G: writable root */
    .opendir  = ramfs_opendir,
    .closedir = ramfs_closedir,
    .readdir  = ramfs_readdir,
    .stat     = ramfs_stat,
    .statfs   = ramfs_statfs,   /* Phase H */
    /* 2026-08-24: the rest of the CRUD contract. Phase G gave the root
     * create/unlink/mkdir and stopped there, which left `mv`, `truncate`,
     * `chmod`, `chown` and `touch` on an existing file all answering EROFS
     * on the one filesystem every session starts in. A NULL op here is not
     * a stub -- the VFS turns it straight into VFS_ERR_ROFS -- so the gap
     * was invisible until something tried to use it. */
    .rename    = ramfs_rename,
    .chmod     = ramfs_chmod,
    .chown     = ramfs_chown,
    .utimes    = ramfs_utimes,
    .truncate  = ramfs_truncate,
    .ftruncate = ramfs_ftruncate,
    .link      = ramfs_link,
};
