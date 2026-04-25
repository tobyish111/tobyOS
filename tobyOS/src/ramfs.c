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
};

struct ramfs_mount {
    const void          *image;
    size_t               image_size;
    struct ramfs_node   *nodes;
    size_t               node_count;
};

static struct ramfs_mount g_mount;

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
        /* '0' / NUL = regular file, '5' = directory. Skip everything
         * else (links, char/block dev, fifo, GNU extensions...). */
        if (tf == '0' || tf == 0 || tf == '5') count++;
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

        if (tf == '0' || tf == 0 || tf == '5') {
            struct ramfs_node *nd = &nodes[idx];
            if (!copy_name(h->name, nd->name, sizeof(nd->name))) {
                kprintf("[ramfs] WARN: skipping entry with overlong name\n");
            } else if (nd->name[0] == 0) {
                /* root entry "./" or similar -- skip silently */
            } else {
                if (tf == '5') {
                    nd->type = VFS_TYPE_DIR;
                    nd->data = 0;
                    nd->size = 0;
                } else {
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
    for (size_t i = 0; i < idx; i++) {
        kprintf("  %s %-32s  %lu B  data@%p\n",
                nodes[i].type == VFS_TYPE_DIR ? "d" : "-",
                nodes[i].name,
                (unsigned long)nodes[i].size,
                nodes[i].data);
    }
    return VFS_OK;
}

size_t ramfs_node_count(void) { return g_mount.node_count; }

/* ---- vfs_ops impl ---- */

static struct ramfs_node *find_node(struct ramfs_mount *m,
                                    const char *norm_path) {
    for (size_t i = 0; i < m->node_count; i++) {
        if (strcmp(m->nodes[i].name, norm_path) == 0) return &m->nodes[i];
    }
    return 0;
}

/* True if any node lives under `norm_path/`. Used to recognise implicit
 * directories (e.g. opendir("/bin") works even if no explicit "bin/"
 * entry was tar'd). */
static bool has_children(struct ramfs_mount *m, const char *norm_path) {
    size_t plen = strlen(norm_path);
    for (size_t i = 0; i < m->node_count; i++) {
        const char *name = m->nodes[i].name;
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
    /* ramfs is owned by root and world-readable but never writable;
     * MODE_VALID forces the VFS to enforce these bits explicitly. */
    out->uid  = 0;
    out->gid  = 0;
    out->mode = 00444u | VFS_MODE_VALID;
    return VFS_OK;
}

static int ramfs_close(struct vfs_file *f) {
    f->priv = 0;
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
                               size_t size) {
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
    /* Everything in the initrd is owned by root and read-only. */
    e->uid  = 0;
    e->gid  = 0;
    e->mode = ((type == VFS_TYPE_DIR) ? 00555u : 00444u) | VFS_MODE_VALID;
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
    size_t cap = m->node_count > 0 ? m->node_count : 1;
    it->ents = kcalloc(cap, sizeof(*it->ents));
    if (!it->ents) { kfree(it); return VFS_ERR_NOMEM; }
    it->count = 0;

    size_t plen = strlen(norm);
    for (size_t i = 0; i < m->node_count; i++) {
        const char *name = m->nodes[i].name;
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
            /* Direct child entry. */
            if (!dirent_push_unique(it->ents, &it->count, cap, rest,
                                    m->nodes[i].type, m->nodes[i].size)) {
                break;
            }
        } else {
            /* Implicit directory: emit the leading component. */
            size_t leaf_len = (size_t)(slash - rest);
            char leaf[VFS_NAME_MAX];
            if (leaf_len >= sizeof(leaf)) leaf_len = sizeof(leaf) - 1;
            memcpy(leaf, rest, leaf_len);
            leaf[leaf_len] = 0;
            if (!dirent_push_unique(it->ents, &it->count, cap, leaf,
                                    VFS_TYPE_DIR, 0)) {
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

    if (norm[0] == 0) {
        out->type = VFS_TYPE_DIR;
        out->size = 0;
        out->uid  = 0;
        out->gid  = 0;
        out->mode = 00555u | VFS_MODE_VALID;
        return VFS_OK;
    }

    struct ramfs_node *nd = find_node(m, norm);
    if (nd) {
        out->type = nd->type;
        out->size = nd->size;
        out->uid  = 0;
        out->gid  = 0;
        out->mode = ((nd->type == VFS_TYPE_DIR) ? 00555u : 00444u)
                    | VFS_MODE_VALID;
        return VFS_OK;
    }
    /* Implicit directory? */
    if (has_children(m, norm)) {
        out->type = VFS_TYPE_DIR;
        out->size = 0;
        out->uid  = 0;
        out->gid  = 0;
        out->mode = 00555u | VFS_MODE_VALID;
        return VFS_OK;
    }
    return VFS_ERR_NOENT;
}

/* Write-side ops are all NULL -- the VFS layer translates a NULL hook
 * into VFS_ERR_ROFS, so callers see "read-only filesystem" cleanly. */
const struct vfs_ops ramfs_ops = {
    .open     = ramfs_open,
    .close    = ramfs_close,
    .read     = ramfs_read,
    .write    = 0,
    .create   = 0,
    .unlink   = 0,
    .mkdir    = 0,
    .opendir  = ramfs_opendir,
    .closedir = ramfs_closedir,
    .readdir  = ramfs_readdir,
    .stat     = ramfs_stat,
};
