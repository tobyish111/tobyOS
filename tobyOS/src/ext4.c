/* ext4.c -- read-only ext2/ext3/ext4 driver (milestone 23D).
 *
 * Mounts an ext4 partition and exposes it through the same `vfs_ops`
 * table that ramfs / tobyfs / fat32 use. The driver only knows about
 * `struct blk_dev *`, so it works on whole disks AND GPT partitions
 * AND USB-MSC LUNs without any glue code at the filesystem layer.
 *
 * What we read from the on-disk format
 * ------------------------------------
 *
 *   - Superblock at byte offset 1024 (sector 2 if 512-byte sectors).
 *     Validates magic, supported INCOMPAT bits, and a sane block size.
 *
 *   - Group Descriptor Table starting at block (s_first_data_block + 1).
 *     We read the entire GDT into memory at mount time. For our small
 *     test images that's at most a few KiB; for huge filesystems we
 *     would lazy-cache it, but RO mounts don't churn the GDT so a
 *     single read on mount is fine.
 *
 *   - Inode table per-group via the descriptor's inode_table_lo (+_hi if
 *     the s_desc_size field signals 64-byte descriptors).
 *
 *   - Directory entries via the linear ext4_dir_entry_2 walk -- htree
 *     fast-path is intentionally NOT implemented because a linear scan
 *     always finds the entry (htree just speeds it up).
 *
 *   - File data via either:
 *       * the extent tree (header + entries; depth 0 leaf or depth 1
 *         index pointing to leaves), OR
 *       * legacy block pointers (12 direct + single-indirect).
 *     Double + triple indirect aren't implemented (test files are
 *     small enough to never need them).
 *
 * What we DELIBERATELY skip
 * -------------------------
 *
 *   - Writes. All of vfs_create / vfs_unlink / vfs_write return
 *     VFS_ERR_ROFS.
 *   - Journal replay. We refuse to mount if EXT4_FEATURE_INCOMPAT_RECOVER
 *     is set (a needs-recovery filesystem can't be safely read).
 *   - Inline data + extended attributes + encryption.
 *   - Symlinks (the inode parses fine, but we don't follow them).
 *   - Hashed-tree directory index (htree). We linear-scan instead,
 *     which the spec mandates as the fallback path anyway.
 */

#include <tobyos/ext4.h>
#include <tobyos/blk.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

/* ---- in-memory state ---- */

struct ext4 {
    struct blk_dev *dev;

    uint32_t block_size;          /* 1024, 2048, 4096 */
    uint32_t sectors_per_block;   /* block_size / 512 */
    uint64_t total_blocks;
    uint32_t inode_size;          /* 128 or 256 */
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t first_ino;           /* usually 11 for ext4 */
    uint32_t feature_incompat;
    uint16_t desc_size;           /* 32 or 64 */
    uint32_t first_data_block;    /* 0 for >= 2 KiB blocks, 1 for 1 KiB */
    bool     desc_64bit;          /* read the high half of group desc */

    /* Cached scratch buffers (allocated at mount). */
    uint8_t *blk_buf;             /* one block */
    uint8_t *blk_buf2;            /* spare for indirect / extent walks */
    uint8_t *gdt_buf;             /* full GDT image */
    uint32_t gdt_bytes;
    uint32_t group_count;
};

/* Per-handle state for an open file. We snapshot the inode at open
 * time so subsequent reads don't have to chase the GDT/inode table
 * again on every byte. The driver is RO so the inode contents can't
 * change underneath us. */
struct ext4_filepriv {
    uint32_t          inode_no;
    struct ext4_inode in;
    uint64_t          file_size;
};

/* Per-handle state for an open directory. Fully materialised on
 * opendir, mirroring the tobyfs / fat32 pattern. */
struct ext4_diriter {
    struct vfs_dirent *ents;
    size_t             count;
};

/* ---- low-level helpers ---- */

static int read_block(struct ext4 *fs, uint64_t blk, void *buf) {
    if (blk >= fs->total_blocks) return VFS_ERR_INVAL;
    uint64_t lba = blk * fs->sectors_per_block;
    if (blk_read(fs->dev, lba, fs->sectors_per_block, buf) != 0) {
        return VFS_ERR_IO;
    }
    return VFS_OK;
}

/* Read `len` bytes starting at byte offset `byte_off` inside block `blk`.
 * `len` MUST fit inside the block (caller chunks larger reads). */
static int read_block_partial(struct ext4 *fs, uint64_t blk,
                              uint32_t byte_off, uint32_t len, void *out) {
    if (byte_off + len > fs->block_size) return VFS_ERR_INVAL;
    int rc = read_block(fs, blk, fs->blk_buf);
    if (rc != VFS_OK) return rc;
    memcpy(out, fs->blk_buf + byte_off, len);
    return VFS_OK;
}

/* ---- group descriptor + inode lookup ---- */

/* Pull the 64-bit "physical block number of inode table for group g"
 * out of our cached GDT, honouring 64-byte descriptors when present. */
static uint64_t group_inode_table(const struct ext4 *fs, uint32_t g) {
    const uint8_t *p = fs->gdt_buf + (uint64_t)g * fs->desc_size;
    const struct ext4_group_desc_32 *d32 =
        (const struct ext4_group_desc_32 *)p;
    uint64_t lo = d32->bg_inode_table_lo;
    if (fs->desc_64bit && fs->desc_size >= 64) {
        const struct ext4_group_desc_64 *d64 =
            (const struct ext4_group_desc_64 *)p;
        lo |= ((uint64_t)d64->bg_inode_table_hi) << 32;
    }
    return lo;
}

/* Read inode number `ino` (1-based) into `out`. Reads exactly the
 * first 128 bytes -- the extra ext4 fields beyond that aren't needed
 * for read-only operation. */
static int read_inode(struct ext4 *fs, uint32_t ino, struct ext4_inode *out) {
    if (ino == 0) return VFS_ERR_INVAL;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t idx   = (ino - 1) % fs->inodes_per_group;
    if (group >= fs->group_count) return VFS_ERR_INVAL;

    uint64_t itab = group_inode_table(fs, group);
    uint64_t byte_off_in_group = (uint64_t)idx * fs->inode_size;
    uint64_t blk = itab + byte_off_in_group / fs->block_size;
    uint32_t off = (uint32_t)(byte_off_in_group % fs->block_size);

    /* The on-disk inode is `inode_size` bytes; we only ever consume
     * the first sizeof(struct ext4_inode) = 128 bytes. */
    return read_block_partial(fs, blk, off, sizeof(*out), out);
}

/* ---- block resolution: extents OR legacy pointers ---- */

/* Walk one extent tree level. `node` points to a buffer that begins
 * with an ext4_extent_header. `logical_blk` is the file-relative
 * block we want. On success `*out_phys` gets the physical block
 * number. Recurses up to `EXT4_MAX_EXTENT_DEPTH` (5 is enough for any
 * real ext4; we cap at 4 here defensively). */
#define EXT4_MAX_EXTENT_DEPTH 4

static int extent_lookup(struct ext4 *fs, const uint8_t *node,
                         uint32_t logical_blk, uint64_t *out_phys,
                         int depth_left) {
    if (depth_left < 0) return VFS_ERR_INVAL;

    const struct ext4_extent_header *eh =
        (const struct ext4_extent_header *)node;
    if (eh->eh_magic != EXT4_EXT_MAGIC) return VFS_ERR_INVAL;
    if (eh->eh_entries == 0) return VFS_ERR_NOENT;

    if (eh->eh_depth == 0) {
        /* Leaf -- entries are ext4_extent. */
        const struct ext4_extent *e =
            (const struct ext4_extent *)(node + sizeof(*eh));
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            uint16_t len = e[i].ee_len;
            /* Uninitialized extent: high bit of ee_len means "zeros".
             * For RO we still return the block; callers either get a
             * real block (initialized) or zeros if the FS pre-allocated
             * but never wrote -- typical for sparse files we don't yet
             * support. Treat both the same way. */
            if (len > 32768) len = (uint16_t)(len - 32768);
            if (logical_blk >= e[i].ee_block &&
                logical_blk <  e[i].ee_block + len) {
                uint64_t base = ((uint64_t)e[i].ee_start_hi << 32) |
                                e[i].ee_start_lo;
                *out_phys = base + (logical_blk - e[i].ee_block);
                return VFS_OK;
            }
        }
        return VFS_ERR_NOENT;
    }

    /* Index node -- entries are ext4_extent_idx. Find the LARGEST
     * index whose ei_block <= logical_blk, then recurse. */
    const struct ext4_extent_idx *ei =
        (const struct ext4_extent_idx *)(node + sizeof(*eh));
    int chosen = -1;
    for (uint16_t i = 0; i < eh->eh_entries; i++) {
        if (ei[i].ei_block <= logical_blk) chosen = i;
        else break;
    }
    if (chosen < 0) return VFS_ERR_NOENT;

    uint64_t leaf_blk = ((uint64_t)ei[chosen].ei_leaf_hi << 32) |
                        ei[chosen].ei_leaf_lo;
    int rc = read_block(fs, leaf_blk, fs->blk_buf2);
    if (rc != VFS_OK) return rc;
    return extent_lookup(fs, fs->blk_buf2, logical_blk,
                         out_phys, depth_left - 1);
}

/* Map logical file block `lblk` to a physical disk block using
 * either the extent tree or the legacy block-pointer chain in the
 * inode's i_block[15] array. Sets *out_phys to 0 to indicate a hole
 * (caller should treat as zeros). */
static int inode_block_map(struct ext4 *fs, const struct ext4_inode *in,
                           uint32_t lblk, uint64_t *out_phys) {
    *out_phys = 0;

    if (in->i_flags & EXT4_EXTENTS_FL) {
        /* i_block IS the extent header + entries (60 bytes total). */
        return extent_lookup(fs, (const uint8_t *)&in->i_block[0],
                             lblk, out_phys, EXT4_MAX_EXTENT_DEPTH);
    }

    /* Legacy pointers: 12 direct, 1 single-indirect (slot 12), then
     * double + triple. We support up to single-indirect. */
    if (lblk < 12) {
        *out_phys = in->i_block[lblk];
        return VFS_OK;
    }

    uint32_t per_block = fs->block_size / 4;  /* 32-bit pointers */
    if (lblk < 12u + per_block) {
        uint32_t ind = in->i_block[12];
        if (ind == 0) return VFS_OK;          /* hole */
        int rc = read_block(fs, ind, fs->blk_buf2);
        if (rc != VFS_OK) return rc;
        const uint32_t *table = (const uint32_t *)fs->blk_buf2;
        *out_phys = table[lblk - 12];
        return VFS_OK;
    }

    /* Double / triple indirect not implemented -- our test files are
     * tiny. Return INVAL so the caller surfaces a clean error rather
     * than silently truncating. */
    return VFS_ERR_INVAL;
}

/* ---- read bytes from an inode ---- */

static long inode_read(struct ext4 *fs, const struct ext4_inode *in,
                       uint64_t file_size, uint64_t pos,
                       void *buf, size_t n) {
    if (pos >= file_size) return 0;
    uint64_t avail = file_size - pos;
    if (n > avail) n = (size_t)avail;
    if (n == 0) return 0;

    uint8_t *out = (uint8_t *)buf;
    size_t   total = 0;
    while (n > 0) {
        uint32_t lblk = (uint32_t)(pos / fs->block_size);
        uint32_t off  = (uint32_t)(pos % fs->block_size);
        uint32_t take = fs->block_size - off;
        if (take > n) take = (uint32_t)n;

        uint64_t phys = 0;
        int rc = inode_block_map(fs, in, lblk, &phys);
        if (rc != VFS_OK) return rc;
        if (phys == 0) {
            memset(out, 0, take);            /* hole reads as zero */
        } else {
            rc = read_block(fs, phys, fs->blk_buf);
            if (rc != VFS_OK) return rc;
            memcpy(out, fs->blk_buf + off, take);
        }
        out   += take;
        pos   += take;
        n     -= take;
        total += take;
    }
    return (long)total;
}

/* ---- directory iteration ---- */

/* Run `cb` against every dir entry in inode `in` (which must be a
 * directory). The callback receives (inode_no, file_type, name).
 * Returning VFS_ERR_NOENT from the callback stops the walk and is
 * treated as success (the "found-and-done" pattern matches fat32.c).
 * Any other negative return aborts. */
typedef int (*dir_cb_t)(void *user, uint32_t ino, uint8_t file_type,
                        const char *name, uint8_t name_len);

static int dir_walk(struct ext4 *fs, const struct ext4_inode *dir_inode,
                    dir_cb_t cb, void *user) {
    uint64_t dir_size = ((uint64_t)dir_inode->i_size_hi << 32) |
                        dir_inode->i_size_lo;
    uint32_t total_blocks = (uint32_t)((dir_size + fs->block_size - 1) /
                                       fs->block_size);

    uint8_t *blk = kmalloc(fs->block_size);
    if (!blk) return VFS_ERR_NOMEM;

    int rc = VFS_OK;
    for (uint32_t lblk = 0; lblk < total_blocks; lblk++) {
        uint64_t phys = 0;
        rc = inode_block_map(fs, dir_inode, lblk, &phys);
        if (rc != VFS_OK || phys == 0) {
            if (rc == VFS_OK) continue;       /* hole -- skip */
            goto out;
        }
        rc = read_block(fs, phys, blk);
        if (rc != VFS_OK) goto out;

        uint32_t off = 0;
        while (off + 8 <= fs->block_size) {
            const struct ext4_dir_entry_2 *de =
                (const struct ext4_dir_entry_2 *)(blk + off);
            uint16_t rec_len = de->rec_len;
            if (rec_len < 8 || off + rec_len > fs->block_size) {
                rc = VFS_ERR_IO;
                goto out;
            }
            if (de->inode != 0 && de->name_len > 0) {
                /* Stack-copy + NUL-terminate so callbacks can treat
                 * the name as a normal C string. */
                char name_buf[VFS_NAME_MAX];
                size_t cap = sizeof(name_buf) - 1;
                size_t cp  = de->name_len < cap ? de->name_len : cap;
                memcpy(name_buf, de->name, cp);
                name_buf[cp] = 0;
                int crc = cb(user, de->inode, de->file_type, name_buf,
                             (uint8_t)cp);
                if (crc == VFS_ERR_NOENT) { rc = VFS_OK; goto out; }
                if (crc != VFS_OK)        { rc = crc;    goto out; }
            }
            off += rec_len;
        }
    }
out:
    kfree(blk);
    return rc;
}

/* ---- path resolution ---- */

struct lookup_one_ctx {
    const char *want;
    uint32_t    out_ino;
    uint8_t     out_type;
    bool        found;
};

static int lookup_one_cb(void *user, uint32_t ino, uint8_t ftype,
                         const char *name, uint8_t nlen) {
    (void)nlen;
    struct lookup_one_ctx *ctx = (struct lookup_one_ctx *)user;
    if (strcmp(name, ctx->want) == 0) {
        ctx->out_ino  = ino;
        ctx->out_type = ftype;
        ctx->found    = true;
        return VFS_ERR_NOENT;        /* stop walking */
    }
    return VFS_OK;
}

/* Find the inode number for `path` (absolute, '/'-separated). On
 * success returns VFS_OK and fills *out_ino / *out_type with the
 * EXT4_FT_* file_type of the final component. */
static int path_to_inode(struct ext4 *fs, const char *path,
                         uint32_t *out_ino, uint8_t *out_type) {
    if (!path) return VFS_ERR_INVAL;
    while (*path == '/') path++;

    uint32_t cur = EXT4_ROOT_INODE;
    uint8_t  type = EXT4_FT_DIR;

    char comp[VFS_NAME_MAX];
    while (*path) {
        size_t i = 0;
        while (path[i] && path[i] != '/') {
            if (i + 1 >= sizeof(comp)) return VFS_ERR_NAMETOOLONG;
            comp[i] = path[i];
            i++;
        }
        comp[i] = 0;

        if (i == 0) break;            /* trailing '/' */

        struct ext4_inode dir;
        int rc = read_inode(fs, cur, &dir);
        if (rc != VFS_OK) return rc;
        if ((dir.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
            return VFS_ERR_NOTDIR;
        }
        struct lookup_one_ctx ctx = { .want = comp, .out_ino = 0,
                                       .out_type = 0, .found = false };
        rc = dir_walk(fs, &dir, lookup_one_cb, &ctx);
        if (rc != VFS_OK) return rc;
        if (!ctx.found) return VFS_ERR_NOENT;

        cur  = ctx.out_ino;
        type = ctx.out_type;
        path += i;
        while (*path == '/') path++;
    }

    *out_ino  = cur;
    *out_type = type;
    return VFS_OK;
}

/* ---- vfs_ops ---- */

static int ext4_open(void *mnt, const char *path, struct vfs_file *out) {
    struct ext4 *fs = (struct ext4 *)mnt;
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;

    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;

    if ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return VFS_ERR_ISDIR;
    if ((in.i_mode & EXT4_S_IFMT) != EXT4_S_IFREG) return VFS_ERR_NOENT;

    struct ext4_filepriv *fp = kcalloc(1, sizeof(*fp));
    if (!fp) return VFS_ERR_NOMEM;
    fp->inode_no  = ino;
    fp->in        = in;
    fp->file_size = ((uint64_t)in.i_size_hi << 32) | in.i_size_lo;

    out->priv = fp;
    out->size = (size_t)fp->file_size;
    out->pos  = 0;
    out->mode = in.i_mode & VFS_MODE_PERMS;
    if (in.i_mode != 0) out->mode |= VFS_MODE_VALID;
    out->uid  = in.i_uid;
    out->gid  = in.i_gid;
    return VFS_OK;
}

static int ext4_close(struct vfs_file *f) {
    if (f && f->priv) { kfree(f->priv); f->priv = NULL; }
    return VFS_OK;
}

static long ext4_read(struct vfs_file *f, void *buf, size_t n) {
    struct ext4 *fs = (struct ext4 *)f->mnt;
    struct ext4_filepriv *fp = (struct ext4_filepriv *)f->priv;
    if (!fp) return VFS_ERR_INVAL;
    long got = inode_read(fs, &fp->in, fp->file_size, f->pos, buf, n);
    if (got > 0) f->pos += (size_t)got;
    return got;
}

static long ext4_write(struct vfs_file *f, const void *buf, size_t n) {
    (void)f; (void)buf; (void)n;
    return VFS_ERR_ROFS;
}

static int ext4_create(void *mnt, const char *path,
                       uint32_t uid, uint32_t gid, uint32_t mode) {
    (void)mnt; (void)path; (void)uid; (void)gid; (void)mode;
    return VFS_ERR_ROFS;
}

static int ext4_unlink(void *mnt, const char *path) {
    (void)mnt; (void)path;
    return VFS_ERR_ROFS;
}

static int ext4_mkdir(void *mnt, const char *path,
                      uint32_t uid, uint32_t gid, uint32_t mode) {
    (void)mnt; (void)path; (void)uid; (void)gid; (void)mode;
    return VFS_ERR_ROFS;
}

/* ---- opendir / readdir / closedir ---- */

struct collect_ctx {
    struct ext4       *fs;
    struct vfs_dirent *out;
    size_t             cap;
    size_t             count;
    int                err;
};

static int collect_cb(void *user, uint32_t ino, uint8_t ftype,
                      const char *name, uint8_t nlen) {
    struct collect_ctx *ctx = (struct collect_ctx *)user;

    /* Skip "." and "..". The VFS contract is to expose only real
     * children; the ramfs / fat32 / tobyfs drivers all behave the
     * same way. */
    if (nlen == 1 && name[0] == '.') return VFS_OK;
    if (nlen == 2 && name[0] == '.' && name[1] == '.') return VFS_OK;

    if (ctx->count >= ctx->cap) {
        size_t new_cap = ctx->cap ? ctx->cap * 2 : 16;
        struct vfs_dirent *grown =
            kmalloc(new_cap * sizeof(struct vfs_dirent));
        if (!grown) { ctx->err = VFS_ERR_NOMEM; return VFS_ERR_NOMEM; }
        if (ctx->count) memcpy(grown, ctx->out,
                                ctx->count * sizeof(struct vfs_dirent));
        if (ctx->out) kfree(ctx->out);
        ctx->out = grown;
        ctx->cap = new_cap;
    }

    /* Look up the child inode so we can fill in size + mode. Cheap
     * enough at opendir time -- our test directories have a handful
     * of entries each. */
    struct ext4_inode child;
    int rc = read_inode(ctx->fs, ino, &child);
    if (rc != VFS_OK) { ctx->err = rc; return rc; }

    struct vfs_dirent *e = &ctx->out[ctx->count++];
    memset(e, 0, sizeof(*e));
    size_t cp = nlen < VFS_NAME_MAX - 1 ? nlen : VFS_NAME_MAX - 1;
    memcpy(e->name, name, cp);
    e->name[cp] = 0;

    if (ftype == EXT4_FT_DIR ||
        (child.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) {
        e->type = VFS_TYPE_DIR;
        e->size = 0;
    } else {
        e->type = VFS_TYPE_FILE;
        e->size = (size_t)(((uint64_t)child.i_size_hi << 32) |
                           child.i_size_lo);
    }
    e->uid  = child.i_uid;
    e->gid  = child.i_gid;
    e->mode = child.i_mode & VFS_MODE_PERMS;
    if (child.i_mode != 0) e->mode |= VFS_MODE_VALID;
    return VFS_OK;
}

static int ext4_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    struct ext4 *fs = (struct ext4 *)mnt;
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;

    struct ext4_inode dir;
    rc = read_inode(fs, ino, &dir);
    if (rc != VFS_OK) return rc;
    if ((dir.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return VFS_ERR_NOTDIR;

    struct collect_ctx ctx = { .fs = fs, .out = NULL, .cap = 0,
                                .count = 0, .err = VFS_OK };
    rc = dir_walk(fs, &dir, collect_cb, &ctx);
    if (rc != VFS_OK || ctx.err != VFS_OK) {
        if (ctx.out) kfree(ctx.out);
        return rc != VFS_OK ? rc : ctx.err;
    }

    struct ext4_diriter *it = kcalloc(1, sizeof(*it));
    if (!it) {
        if (ctx.out) kfree(ctx.out);
        return VFS_ERR_NOMEM;
    }
    it->ents  = ctx.out;
    it->count = ctx.count;

    out->priv  = it;
    out->index = 0;
    return VFS_OK;
}

static int ext4_closedir(struct vfs_dir *d) {
    if (d && d->priv) {
        struct ext4_diriter *it = (struct ext4_diriter *)d->priv;
        if (it->ents) kfree(it->ents);
        kfree(it);
        d->priv = NULL;
    }
    return VFS_OK;
}

static int ext4_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    struct ext4_diriter *it = (struct ext4_diriter *)d->priv;
    if (!it || d->index >= it->count) return VFS_ERR_NOENT;
    *out = it->ents[d->index++];
    return VFS_OK;
}

static int ext4_stat(void *mnt, const char *path, struct vfs_stat *out) {
    struct ext4 *fs = (struct ext4 *)mnt;
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;

    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;

    memset(out, 0, sizeof(*out));
    if ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) {
        out->type = VFS_TYPE_DIR;
        out->size = 0;
    } else {
        out->type = VFS_TYPE_FILE;
        out->size = (size_t)(((uint64_t)in.i_size_hi << 32) | in.i_size_lo);
    }
    out->uid  = in.i_uid;
    out->gid  = in.i_gid;
    out->mode = in.i_mode & VFS_MODE_PERMS;
    if (in.i_mode != 0) out->mode |= VFS_MODE_VALID;
    return VFS_OK;
}

static const struct vfs_ops ext4_ops = {
    .open     = ext4_open,
    .close    = ext4_close,
    .read     = ext4_read,
    .write    = ext4_write,
    .create   = ext4_create,
    .unlink   = ext4_unlink,
    .mkdir    = ext4_mkdir,
    .opendir  = ext4_opendir,
    .closedir = ext4_closedir,
    .readdir  = ext4_readdir,
    .stat     = ext4_stat,
    .chmod    = NULL,
    .chown    = NULL,
};

/* ---- probe + mount entry points ---- */

/* Read just the SB; check magic + that the block size is sane and
 * the incompat features don't include anything we refuse. */
int ext4_probe(struct blk_dev *dev) {
    if (!dev) return 0;
    uint8_t buf[1024];
    /* Superblock starts at byte offset 1024 -> sector 2 (assuming
     * 512-byte sectors, which is invariant in our blk layer). */
    if (blk_read(dev, 2, 2, buf) != 0) return 0;
    const struct ext4_super_block *sb = (const struct ext4_super_block *)buf;
    if (sb->s_magic != EXT4_SUPER_MAGIC) return 0;
    if (sb->s_log_block_size > 6) return 0;          /* > 64 KiB? bogus */

    uint32_t bs = 1024u << sb->s_log_block_size;
    if (bs != 1024 && bs != 2048 && bs != 4096) return 0;

    /* Refuse anything we can't safely mount RO. */
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) return 0;
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA) return 0;
    return 1;
}

int ext4_mount(const char *mount_point, struct blk_dev *dev) {
    if (!mount_point || !dev) return VFS_ERR_INVAL;

    uint8_t sb_buf[1024];
    if (blk_read(dev, 2, 2, sb_buf) != 0) {
        kprintf("[ext4] superblock read failed\n");
        return VFS_ERR_IO;
    }
    const struct ext4_super_block *sb =
        (const struct ext4_super_block *)sb_buf;
    if (sb->s_magic != EXT4_SUPER_MAGIC) {
        kprintf("[ext4] bad magic 0x%x (want 0xEF53)\n",
                (unsigned)sb->s_magic);
        return VFS_ERR_INVAL;
    }
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) {
        kprintf("[ext4] refuse mount: NEEDS_RECOVERY (journal replay required)\n");
        return VFS_ERR_INVAL;
    }
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA) {
        kprintf("[ext4] refuse mount: INLINE_DATA not supported\n");
        return VFS_ERR_INVAL;
    }

    struct ext4 *fs = kcalloc(1, sizeof(*fs));
    if (!fs) return VFS_ERR_NOMEM;
    fs->dev = dev;
    fs->block_size       = 1024u << sb->s_log_block_size;
    fs->sectors_per_block= fs->block_size / BLK_SECTOR_SIZE;
    fs->total_blocks     = sb->s_blocks_count_lo;
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        fs->total_blocks |= ((uint64_t)sb->s_blocks_count_hi) << 32;
    }
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->first_ino        = (sb->s_rev_level >= 1) ? sb->s_first_ino : 11;
    fs->inode_size       = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;
    fs->feature_incompat = sb->s_feature_incompat;
    fs->desc_size        = sb->s_desc_size;
    if (fs->desc_size < 32) fs->desc_size = 32;
    fs->desc_64bit = (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0
                     && fs->desc_size >= 64;
    fs->first_data_block = (fs->block_size == 1024) ? 1 : 0;

    if (fs->inodes_per_group == 0 || fs->blocks_per_group == 0 ||
        fs->inode_size < 128 || fs->total_blocks == 0) {
        kprintf("[ext4] superblock fields out of range\n");
        kfree(fs);
        return VFS_ERR_INVAL;
    }

    fs->group_count = (uint32_t)((fs->total_blocks + fs->blocks_per_group - 1) /
                                  fs->blocks_per_group);
    fs->gdt_bytes = fs->group_count * fs->desc_size;
    /* Round GDT up to a whole block. */
    uint32_t gdt_blocks = (fs->gdt_bytes + fs->block_size - 1) / fs->block_size;

    fs->blk_buf  = kmalloc(fs->block_size);
    fs->blk_buf2 = kmalloc(fs->block_size);
    fs->gdt_buf  = kmalloc(gdt_blocks * fs->block_size);
    if (!fs->blk_buf || !fs->blk_buf2 || !fs->gdt_buf) {
        if (fs->blk_buf)  kfree(fs->blk_buf);
        if (fs->blk_buf2) kfree(fs->blk_buf2);
        if (fs->gdt_buf)  kfree(fs->gdt_buf);
        kfree(fs);
        return VFS_ERR_NOMEM;
    }

    /* GDT lives at block (s_first_data_block + 1). */
    uint32_t gdt_first_blk = fs->first_data_block + 1;
    for (uint32_t i = 0; i < gdt_blocks; i++) {
        int rc = read_block(fs, gdt_first_blk + i,
                            fs->gdt_buf + i * fs->block_size);
        if (rc != VFS_OK) {
            kprintf("[ext4] GDT read failed at block %u\n", gdt_first_blk + i);
            kfree(fs->blk_buf);
            kfree(fs->blk_buf2);
            kfree(fs->gdt_buf);
            kfree(fs);
            return VFS_ERR_IO;
        }
    }

    /* Sanity-check the root inode actually decodes. */
    struct ext4_inode root;
    int rc = read_inode(fs, EXT4_ROOT_INODE, &root);
    if (rc != VFS_OK ||
        (root.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
        kprintf("[ext4] root inode (#2) is not a directory (mode=0x%x rc=%d)\n",
                (unsigned)root.i_mode, rc);
        kfree(fs->blk_buf);
        kfree(fs->blk_buf2);
        kfree(fs->gdt_buf);
        kfree(fs);
        return VFS_ERR_INVAL;
    }

    rc = vfs_mount(mount_point, &ext4_ops, fs);
    if (rc != VFS_OK) {
        kprintf("[ext4] vfs_mount('%s') failed: %d\n", mount_point, rc);
        kfree(fs->blk_buf);
        kfree(fs->blk_buf2);
        kfree(fs->gdt_buf);
        kfree(fs);
        return rc;
    }

    kprintf("[ext4] mounted '%s' on %s: %u blocks x %u B "
            "(%u KiB total, %u groups, inode_size=%u, first_ino=%u, %s)\n",
            mount_point, dev->name ? dev->name : "(anon)",
            (unsigned)fs->total_blocks, fs->block_size,
            (unsigned)((fs->total_blocks * fs->block_size) / 1024u),
            fs->group_count, fs->inode_size, fs->first_ino,
            (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) ?
                "extents" : "legacy-ptrs");
    return VFS_OK;
}
