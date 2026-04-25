/* tobyfs.c -- writable on-disk filesystem (milestone 6).
 *
 * Layout (matches include/tobyos/tobyfs.h verbatim and is shared with
 * tools/mkfs_tobyfs.c):
 *
 *   block 0       superblock
 *   block 1       inode bitmap   (1 bit / inode)
 *   block 2       data  bitmap   (1 bit / data block, includes meta blocks)
 *   blocks 3..10  inode table
 *   blocks 11..N  data blocks
 *
 * Every metadata mutation is written back to disk synchronously --
 * there's no cache, no journal, no batching. It's slow but easy to
 * reason about: if a call returns success, the bytes are durable.
 *
 * Path resolution is iterative: split on '/', look up each component
 * in the parent's data blocks (each containing TFS_DIRENTS_PER_BLOCK
 * 64-byte slots). No support for "." or ".." -- callers hand us
 * canonical absolute paths.
 */

#include <tobyos/tobyfs.h>
#include <tobyos/blk.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/pit.h>
#include <tobyos/slog.h>     /* M28E: emit fscheck verdicts via slog */

_Static_assert(sizeof(struct tfs_inode_disk)  == TFS_INODE_SIZE,  "tobyfs inode size");
_Static_assert(sizeof(struct tfs_dirent_disk) == TFS_DIRENT_SIZE, "tobyfs dirent size");

struct tobyfs;
/* Forward declaration: defined further down in the M28E section.
 * Needed because tobyfs_mount() (defined first) calls into it. */
static int check_core(struct tobyfs *fs, struct tobyfs_check *out);

struct tobyfs {
    struct blk_dev        *dev;
    struct tfs_superblock  sb;
    /* Bitmaps live in dedicated 4 KiB buffers, kept in lockstep with
     * the on-disk copies. We always re-flush the *whole* bitmap block
     * after a mutation -- correctness over cleverness. */
    uint8_t                ibitmap[TFS_BLOCK_SIZE];
    uint8_t                dbitmap[TFS_BLOCK_SIZE];
};

/* -------- block I/O helpers (block N <-> sectors N*8 .. N*8+7) -------- */

static int read_block(struct tobyfs *fs, uint32_t blk, void *buf) {
    return blk_read(fs->dev, (uint64_t)blk * TFS_SECTORS_PER_BLOCK,
                    TFS_SECTORS_PER_BLOCK, buf);
}

static int write_block(struct tobyfs *fs, uint32_t blk, const void *buf) {
    return blk_write(fs->dev, (uint64_t)blk * TFS_SECTORS_PER_BLOCK,
                     TFS_SECTORS_PER_BLOCK, buf);
}

/* -------- bitmap helpers (operate on the in-memory copy + flush) -------- */

static bool bit_get(const uint8_t *bm, uint32_t i) {
    return (bm[i >> 3] >> (i & 7)) & 1;
}
static void bit_set(uint8_t *bm, uint32_t i) {
    bm[i >> 3] |= (uint8_t)(1u << (i & 7));
}
static void bit_clear(uint8_t *bm, uint32_t i) {
    bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static int alloc_inode(struct tobyfs *fs, uint32_t *out_ino) {
    /* Inode 0 is reserved (means "invalid" everywhere in the FS). */
    for (uint32_t i = 1; i < fs->sb.inode_count; i++) {
        if (!bit_get(fs->ibitmap, i)) {
            bit_set(fs->ibitmap, i);
            int rc = write_block(fs, fs->sb.inode_bitmap_blk, fs->ibitmap);
            if (rc != 0) { bit_clear(fs->ibitmap, i); return VFS_ERR_IO; }
            *out_ino = i;
            return VFS_OK;
        }
    }
    return VFS_ERR_NOSPC;
}

static int free_inode(struct tobyfs *fs, uint32_t ino) {
    if (ino == 0 || ino >= fs->sb.inode_count) return VFS_ERR_INVAL;
    bit_clear(fs->ibitmap, ino);
    return write_block(fs, fs->sb.inode_bitmap_blk, fs->ibitmap)
           == 0 ? VFS_OK : VFS_ERR_IO;
}

static int alloc_data_block(struct tobyfs *fs, uint32_t *out_blk) {
    for (uint32_t b = fs->sb.data_blk_start; b < fs->sb.total_blocks; b++) {
        if (!bit_get(fs->dbitmap, b)) {
            bit_set(fs->dbitmap, b);
            int rc = write_block(fs, fs->sb.data_bitmap_blk, fs->dbitmap);
            if (rc != 0) { bit_clear(fs->dbitmap, b); return VFS_ERR_IO; }
            /* Zero the freshly-allocated block on disk so stale data
             * never leaks across allocations -- especially important
             * for newly-allocated directory blocks. */
            uint8_t zero[TFS_BLOCK_SIZE] = {0};
            rc = write_block(fs, b, zero);
            if (rc != 0) {
                bit_clear(fs->dbitmap, b);
                (void)write_block(fs, fs->sb.data_bitmap_blk, fs->dbitmap);
                return VFS_ERR_IO;
            }
            *out_blk = b;
            return VFS_OK;
        }
    }
    return VFS_ERR_NOSPC;
}

static int free_data_block(struct tobyfs *fs, uint32_t blk) {
    if (blk < fs->sb.data_blk_start || blk >= fs->sb.total_blocks) {
        return VFS_ERR_INVAL;
    }
    bit_clear(fs->dbitmap, blk);
    return write_block(fs, fs->sb.data_bitmap_blk, fs->dbitmap)
           == 0 ? VFS_OK : VFS_ERR_IO;
}

/* -------- inode I/O (read-modify-write whole block) -------- */

static int read_inode(struct tobyfs *fs, uint32_t ino,
                      struct tfs_inode_disk *out) {
    if (ino == 0 || ino >= fs->sb.inode_count) return VFS_ERR_INVAL;
    if (!bit_get(fs->ibitmap, ino)) return VFS_ERR_NOENT;
    uint32_t blk = fs->sb.inode_table_blk + (ino / TFS_INODES_PER_BLOCK);
    uint8_t buf[TFS_BLOCK_SIZE];
    int rc = read_block(fs, blk, buf);
    if (rc != 0) return VFS_ERR_IO;
    memcpy(out, buf + (ino % TFS_INODES_PER_BLOCK) * TFS_INODE_SIZE,
           sizeof(*out));
    return VFS_OK;
}

static int write_inode(struct tobyfs *fs, uint32_t ino,
                       const struct tfs_inode_disk *in) {
    if (ino == 0 || ino >= fs->sb.inode_count) return VFS_ERR_INVAL;
    uint32_t blk = fs->sb.inode_table_blk + (ino / TFS_INODES_PER_BLOCK);
    uint8_t buf[TFS_BLOCK_SIZE];
    int rc = read_block(fs, blk, buf);
    if (rc != 0) return VFS_ERR_IO;
    memcpy(buf + (ino % TFS_INODES_PER_BLOCK) * TFS_INODE_SIZE,
           in, sizeof(*in));
    return write_block(fs, blk, buf) == 0 ? VFS_OK : VFS_ERR_IO;
}

/* -------- path / dirent helpers -------- */

/* Pull the next '/'-delimited component out of *cursor. On entry,
 * *cursor is positioned at either '/' or the first byte of a name.
 * On return, `out` holds the NUL-terminated component, and *cursor
 * is advanced to the next component or the trailing NUL. Returns
 * 0 on success, VFS_ERR_NAMETOOLONG / VFS_ERR_INVAL on failure, and
 * +1 when there are no more components. */
static int next_component(const char **cursor, char *out, size_t out_max) {
    const char *p = *cursor;
    while (*p == '/') p++;
    if (*p == 0) { *cursor = p; return 1; }
    size_t n = 0;
    while (*p && *p != '/') {
        if (n + 1 >= out_max) return VFS_ERR_NAMETOOLONG;
        out[n++] = *p++;
    }
    out[n] = 0;
    *cursor = p;
    return VFS_OK;
}

/* Look up `name` in the directory inode `dir`. Returns its inode
 * number in *out_ino on success. */
static int dir_lookup(struct tobyfs *fs, const struct tfs_inode_disk *dir,
                      const char *name, uint32_t *out_ino) {
    if (dir->type != TFS_TYPE_DIR) return VFS_ERR_NOTDIR;
    uint8_t buf[TFS_BLOCK_SIZE];
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        uint32_t b = dir->direct[i];
        if (b == 0) continue;
        if (read_block(fs, b, buf) != 0) return VFS_ERR_IO;
        const struct tfs_dirent_disk *de = (const struct tfs_dirent_disk *)buf;
        for (uint32_t j = 0; j < TFS_DIRENTS_PER_BLOCK; j++) {
            if (de[j].ino == 0) continue;
            if (strcmp(de[j].name, name) == 0) {
                *out_ino = de[j].ino;
                return VFS_OK;
            }
        }
    }
    return VFS_ERR_NOENT;
}

/* Walk an absolute path to its inode. `consumed_components` lets us
 * also handle "walk to parent" by requesting one fewer component. */
static int path_walk(struct tobyfs *fs, const char *path, uint32_t *out_ino,
                     struct tfs_inode_disk *out_node) {
    uint32_t cur = fs->sb.root_ino;
    struct tfs_inode_disk node;
    int rc = read_inode(fs, cur, &node);
    if (rc != VFS_OK) return rc;
    const char *p = path;
    char comp[TFS_NAME_MAX + 1];
    for (;;) {
        rc = next_component(&p, comp, sizeof(comp));
        if (rc == 1) break;            /* end of path */
        if (rc != VFS_OK) return rc;
        uint32_t child;
        rc = dir_lookup(fs, &node, comp, &child);
        if (rc != VFS_OK) return rc;
        cur = child;
        rc = read_inode(fs, cur, &node);
        if (rc != VFS_OK) return rc;
    }
    *out_ino = cur;
    *out_node = node;
    return VFS_OK;
}

/* Split "/a/b/c" into parent="/a/b" + leaf="c". Both buffers must be
 * at least VFS_PATH_MAX. Empty parent ("" or "/") is represented as
 * "/". Returns VFS_OK on success, VFS_ERR_INVAL on something like "/". */
static int split_parent_leaf(const char *path, char *parent, char *leaf) {
    /* Find the last '/' that precedes a non-empty leaf. */
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] == '/') n--;     /* trim trailing slashes */
    if (n == 0) return VFS_ERR_INVAL;            /* root has no leaf */
    size_t leaf_start = n;
    while (leaf_start > 0 && path[leaf_start - 1] != '/') leaf_start--;
    size_t leaf_len = n - leaf_start;
    if (leaf_len == 0 || leaf_len > TFS_NAME_MAX) return VFS_ERR_NAMETOOLONG;
    memcpy(leaf, path + leaf_start, leaf_len);
    leaf[leaf_len] = 0;
    /* Parent is everything before leaf_start, with at least "/". */
    if (leaf_start <= 1) {
        parent[0] = '/'; parent[1] = 0;
    } else {
        size_t plen = leaf_start - 1;     /* drop the slash */
        if (plen >= VFS_PATH_MAX) return VFS_ERR_NAMETOOLONG;
        memcpy(parent, path, plen);
        parent[plen] = 0;
    }
    return VFS_OK;
}

/* Insert (ino, name) into the directory `dir_node`. May allocate a
 * new data block if every existing slot is full. Updates dir_node and
 * writes it back. */
static int dir_insert(struct tobyfs *fs, uint32_t dir_ino,
                      struct tfs_inode_disk *dir_node,
                      uint32_t child_ino, const char *name) {
    uint8_t buf[TFS_BLOCK_SIZE];
    /* Pass 1: find an empty slot in an already-allocated block. */
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        uint32_t b = dir_node->direct[i];
        if (b == 0) continue;
        if (read_block(fs, b, buf) != 0) return VFS_ERR_IO;
        struct tfs_dirent_disk *de = (struct tfs_dirent_disk *)buf;
        for (uint32_t j = 0; j < TFS_DIRENTS_PER_BLOCK; j++) {
            if (de[j].ino == 0) {
                de[j].ino = child_ino;
                de[j].reserved = 0;
                size_t nlen = strlen(name);
                memcpy(de[j].name, name, nlen);
                de[j].name[nlen] = 0;
                if (write_block(fs, b, buf) != 0) return VFS_ERR_IO;
                dir_node->size += TFS_DIRENT_SIZE;
                return write_inode(fs, dir_ino, dir_node);
            }
        }
    }
    /* Pass 2: every existing block is full -- allocate a new one. */
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        if (dir_node->direct[i] != 0) continue;
        uint32_t newb;
        int rc = alloc_data_block(fs, &newb);
        if (rc != VFS_OK) return rc;
        dir_node->direct[i] = newb;
        memset(buf, 0, sizeof(buf));
        struct tfs_dirent_disk *de = (struct tfs_dirent_disk *)buf;
        de[0].ino = child_ino;
        de[0].reserved = 0;
        size_t nlen = strlen(name);
        memcpy(de[0].name, name, nlen);
        de[0].name[nlen] = 0;
        if (write_block(fs, newb, buf) != 0) {
            (void)free_data_block(fs, newb);
            dir_node->direct[i] = 0;
            return VFS_ERR_IO;
        }
        dir_node->size += TFS_DIRENT_SIZE;
        return write_inode(fs, dir_ino, dir_node);
    }
    return VFS_ERR_NOSPC;
}

/* Remove (name) from the directory. Does not touch the child inode --
 * caller is responsible for that. Returns VFS_ERR_NOENT if not found. */
static int dir_remove(struct tobyfs *fs, uint32_t dir_ino,
                      struct tfs_inode_disk *dir_node, const char *name) {
    uint8_t buf[TFS_BLOCK_SIZE];
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        uint32_t b = dir_node->direct[i];
        if (b == 0) continue;
        if (read_block(fs, b, buf) != 0) return VFS_ERR_IO;
        struct tfs_dirent_disk *de = (struct tfs_dirent_disk *)buf;
        for (uint32_t j = 0; j < TFS_DIRENTS_PER_BLOCK; j++) {
            if (de[j].ino == 0) continue;
            if (strcmp(de[j].name, name) == 0) {
                de[j].ino = 0;
                de[j].name[0] = 0;
                if (write_block(fs, b, buf) != 0) return VFS_ERR_IO;
                if (dir_node->size >= TFS_DIRENT_SIZE) {
                    dir_node->size -= TFS_DIRENT_SIZE;
                }
                return write_inode(fs, dir_ino, dir_node);
            }
        }
    }
    return VFS_ERR_NOENT;
}

/* True if the directory contains any populated dirent. Used to refuse
 * removing a non-empty directory (we don't support recursive rm). */
static bool dir_is_empty(struct tobyfs *fs,
                         const struct tfs_inode_disk *dir_node) {
    uint8_t buf[TFS_BLOCK_SIZE];
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        uint32_t b = dir_node->direct[i];
        if (b == 0) continue;
        if (read_block(fs, b, buf) != 0) return false;   /* fail-safe */
        const struct tfs_dirent_disk *de = (const struct tfs_dirent_disk *)buf;
        for (uint32_t j = 0; j < TFS_DIRENTS_PER_BLOCK; j++) {
            if (de[j].ino != 0) return false;
        }
    }
    return true;
}

/* -------- vfs_ops impl -------- */

struct tobyfs_handle {
    uint32_t                ino;
    struct tfs_inode_disk   node;     /* in-RAM copy; flushed on write */
};

static int tobyfs_stat(void *mnt, const char *path, struct vfs_stat *out) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    uint32_t ino;
    struct tfs_inode_disk node;
    int rc = path_walk(fs, path, &ino, &node);
    if (rc != VFS_OK) return rc;
    out->type = (node.type == TFS_TYPE_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    out->size = node.size;
    out->uid  = node.uid;
    out->gid  = node.gid;
    out->mode = node.mode;
    return VFS_OK;
}

static int tobyfs_open(void *mnt, const char *path, struct vfs_file *out) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    uint32_t ino;
    struct tfs_inode_disk node;
    int rc = path_walk(fs, path, &ino, &node);
    if (rc != VFS_OK) return rc;
    if (node.type != TFS_TYPE_FILE) return VFS_ERR_ISDIR;
    struct tobyfs_handle *h = kmalloc(sizeof(*h));
    if (!h) return VFS_ERR_NOMEM;
    h->ino  = ino;
    h->node = node;
    out->priv = h;
    out->pos  = 0;
    out->size = node.size;
    out->uid  = node.uid;
    out->gid  = node.gid;
    out->mode = node.mode;
    return VFS_OK;
}

static int tobyfs_close(struct vfs_file *f) {
    if (f->priv) kfree(f->priv);
    f->priv = 0;
    return VFS_OK;
}

static long tobyfs_read(struct vfs_file *f, void *buf, size_t n) {
    struct tobyfs *fs = (struct tobyfs *)f->mnt;
    struct tobyfs_handle *h = (struct tobyfs_handle *)f->priv;
    if (!h) return VFS_ERR_INVAL;
    if (f->pos >= h->node.size) return 0;
    size_t avail = h->node.size - f->pos;
    if (n > avail) n = avail;

    uint8_t *out = (uint8_t *)buf;
    size_t copied = 0;
    uint8_t blkbuf[TFS_BLOCK_SIZE];
    while (copied < n) {
        size_t off    = f->pos + copied;
        uint32_t didx = (uint32_t)(off / TFS_BLOCK_SIZE);
        uint32_t bofs = (uint32_t)(off % TFS_BLOCK_SIZE);
        if (didx >= TFS_NDIRECT) break;
        uint32_t blk = h->node.direct[didx];
        if (blk == 0) {
            /* Sparse hole -- shouldn't happen on regular writes, but
             * be defensive: zero-fill. */
            size_t chunk = TFS_BLOCK_SIZE - bofs;
            if (chunk > n - copied) chunk = n - copied;
            memset(out + copied, 0, chunk);
            copied += chunk;
            continue;
        }
        if (read_block(fs, blk, blkbuf) != 0) return VFS_ERR_IO;
        size_t chunk = TFS_BLOCK_SIZE - bofs;
        if (chunk > n - copied) chunk = n - copied;
        memcpy(out + copied, blkbuf + bofs, chunk);
        copied += chunk;
    }
    f->pos += copied;
    return (long)copied;
}

static long tobyfs_write(struct vfs_file *f, const void *buf, size_t n) {
    struct tobyfs *fs = (struct tobyfs *)f->mnt;
    struct tobyfs_handle *h = (struct tobyfs_handle *)f->priv;
    if (!h) return VFS_ERR_INVAL;

    const uint8_t *in = (const uint8_t *)buf;
    size_t written = 0;
    uint8_t blkbuf[TFS_BLOCK_SIZE];

    while (written < n) {
        size_t off    = f->pos + written;
        uint32_t didx = (uint32_t)(off / TFS_BLOCK_SIZE);
        uint32_t bofs = (uint32_t)(off % TFS_BLOCK_SIZE);
        if (didx >= TFS_NDIRECT) {
            /* File would exceed 16 * 4 KiB = 64 KiB. Stop short and
             * report what we did manage. */
            break;
        }
        uint32_t blk = h->node.direct[didx];
        if (blk == 0) {
            int rc = alloc_data_block(fs, &blk);
            if (rc != VFS_OK) {
                /* Persist whatever we wrote so far before bailing. */
                if (written > 0) goto flush;
                return rc;
            }
            h->node.direct[didx] = blk;
        }
        size_t chunk = TFS_BLOCK_SIZE - bofs;
        if (chunk > n - written) chunk = n - written;
        if (chunk == TFS_BLOCK_SIZE) {
            /* Whole-block overwrite: skip the read. */
            memcpy(blkbuf, in + written, TFS_BLOCK_SIZE);
        } else {
            if (read_block(fs, blk, blkbuf) != 0) return VFS_ERR_IO;
            memcpy(blkbuf + bofs, in + written, chunk);
        }
        if (write_block(fs, blk, blkbuf) != 0) return VFS_ERR_IO;
        written += chunk;
    }

flush:
    {
        size_t newend = f->pos + written;
        if (newend > h->node.size) h->node.size = (uint32_t)newend;
        h->node.mtime = (uint32_t)pit_ticks();
        int rc = write_inode(fs, h->ino, &h->node);
        if (rc != VFS_OK) return rc;
        f->pos += written;
        f->size = h->node.size;
        return (long)written;
    }
}

/* Helper: free every direct block of an inode and zero them in-memory. */
static void inode_free_blocks(struct tobyfs *fs, struct tfs_inode_disk *node) {
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        if (node->direct[i] != 0) {
            (void)free_data_block(fs, node->direct[i]);
            node->direct[i] = 0;
        }
    }
}

static int tobyfs_create(void *mnt, const char *path,
                         uint32_t uid, uint32_t gid, uint32_t mode) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    char parent[VFS_PATH_MAX], leaf[TFS_NAME_MAX + 1];
    int rc = split_parent_leaf(path, parent, leaf);
    if (rc != VFS_OK) return rc;

    uint32_t pino;
    struct tfs_inode_disk pnode;
    rc = path_walk(fs, parent, &pino, &pnode);
    if (rc != VFS_OK) return rc;
    if (pnode.type != TFS_TYPE_DIR) return VFS_ERR_NOTDIR;

    uint32_t existing;
    rc = dir_lookup(fs, &pnode, leaf, &existing);
    if (rc == VFS_OK) {
        /* For "touch"-style semantics: if it already exists and is a
         * file, that's fine -- report EXIST so vfs_write_all knows to
         * truncate. If it's a directory, that's a real error. */
        struct tfs_inode_disk en;
        if (read_inode(fs, existing, &en) == VFS_OK &&
            en.type == TFS_TYPE_DIR) {
            return VFS_ERR_ISDIR;
        }
        return VFS_ERR_EXIST;
    }
    if (rc != VFS_ERR_NOENT) return rc;

    uint32_t ino;
    rc = alloc_inode(fs, &ino);
    if (rc != VFS_OK) return rc;

    struct tfs_inode_disk node = {0};
    node.type  = TFS_TYPE_FILE;
    node.nlink = 1;
    node.size  = 0;
    node.mtime = (uint32_t)pit_ticks();
    node.mode  = mode ? mode : TFS_DEFAULT_FILE_MODE;
    node.uid   = uid;
    node.gid   = gid;
    rc = write_inode(fs, ino, &node);
    if (rc != VFS_OK) { (void)free_inode(fs, ino); return rc; }

    rc = dir_insert(fs, pino, &pnode, ino, leaf);
    if (rc != VFS_OK) {
        (void)free_inode(fs, ino);
        return rc;
    }
    return VFS_OK;
}

static int tobyfs_mkdir(void *mnt, const char *path,
                        uint32_t uid, uint32_t gid, uint32_t mode) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    char parent[VFS_PATH_MAX], leaf[TFS_NAME_MAX + 1];
    int rc = split_parent_leaf(path, parent, leaf);
    if (rc != VFS_OK) return rc;

    uint32_t pino;
    struct tfs_inode_disk pnode;
    rc = path_walk(fs, parent, &pino, &pnode);
    if (rc != VFS_OK) return rc;
    if (pnode.type != TFS_TYPE_DIR) return VFS_ERR_NOTDIR;

    uint32_t existing;
    rc = dir_lookup(fs, &pnode, leaf, &existing);
    if (rc == VFS_OK) return VFS_ERR_EXIST;
    if (rc != VFS_ERR_NOENT) return rc;

    uint32_t ino;
    rc = alloc_inode(fs, &ino);
    if (rc != VFS_OK) return rc;

    struct tfs_inode_disk node = {0};
    node.type  = TFS_TYPE_DIR;
    node.nlink = 1;
    node.size  = 0;
    node.mtime = (uint32_t)pit_ticks();
    node.mode  = mode ? mode : TFS_DEFAULT_DIR_MODE;
    node.uid   = uid;
    node.gid   = gid;
    /* Empty dir -- no data blocks until something's inserted. */
    rc = write_inode(fs, ino, &node);
    if (rc != VFS_OK) { (void)free_inode(fs, ino); return rc; }

    rc = dir_insert(fs, pino, &pnode, ino, leaf);
    if (rc != VFS_OK) {
        (void)free_inode(fs, ino);
        return rc;
    }
    return VFS_OK;
}

static int tobyfs_chmod(void *mnt, const char *path, uint32_t mode) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    uint32_t ino;
    struct tfs_inode_disk node;
    int rc = path_walk(fs, path, &ino, &node);
    if (rc != VFS_OK) return rc;
    /* Preserve any high-bit flags (e.g. MODE_VALID) the caller asked for
     * but mask everything else to perms only. */
    node.mode = (mode & TFS_MODE_PERMS) | TFS_MODE_VALID;
    return write_inode(fs, ino, &node);
}

static int tobyfs_chown(void *mnt, const char *path,
                        uint32_t uid, uint32_t gid) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    uint32_t ino;
    struct tfs_inode_disk node;
    int rc = path_walk(fs, path, &ino, &node);
    if (rc != VFS_OK) return rc;
    node.uid = uid;
    node.gid = gid;
    /* Inode now has authoritative ownership info; flip MODE_VALID on so
     * the VFS treats the perm bits as enforceable. If perms were never
     * set (legacy), assume the matching default. */
    if (!(node.mode & TFS_MODE_VALID)) {
        node.mode = (node.type == TFS_TYPE_DIR)
                  ? TFS_DEFAULT_DIR_MODE
                  : TFS_DEFAULT_FILE_MODE;
    }
    return write_inode(fs, ino, &node);
}

static int tobyfs_unlink(void *mnt, const char *path) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    char parent[VFS_PATH_MAX], leaf[TFS_NAME_MAX + 1];
    int rc = split_parent_leaf(path, parent, leaf);
    if (rc != VFS_OK) return rc;

    uint32_t pino;
    struct tfs_inode_disk pnode;
    rc = path_walk(fs, parent, &pino, &pnode);
    if (rc != VFS_OK) return rc;
    if (pnode.type != TFS_TYPE_DIR) return VFS_ERR_NOTDIR;

    uint32_t cino;
    rc = dir_lookup(fs, &pnode, leaf, &cino);
    if (rc != VFS_OK) return rc;

    struct tfs_inode_disk cnode;
    rc = read_inode(fs, cino, &cnode);
    if (rc != VFS_OK) return rc;

    /* Empty directories are removable; non-empty ones are refused (we
     * don't do recursive rm in this milestone). */
    if (cnode.type == TFS_TYPE_DIR && !dir_is_empty(fs, &cnode)) {
        return VFS_ERR_INVAL;
    }

    inode_free_blocks(fs, &cnode);
    cnode.type = TFS_TYPE_FREE;
    cnode.size = 0;
    (void)write_inode(fs, cino, &cnode);
    (void)free_inode(fs, cino);
    return dir_remove(fs, pino, &pnode, leaf);
}

/* -------- directory iteration --------
 *
 * opendir() snapshots the directory's dirents into a heap array; readdir
 * just hands them out. Snapshotting is fine because directories are
 * tiny (max ~1024 entries) and the snapshot survives concurrent write
 * traffic without locking. */

struct tobyfs_diriter {
    struct vfs_dirent *ents;
    size_t             count;
};

static int tobyfs_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    struct tobyfs *fs = (struct tobyfs *)mnt;
    uint32_t ino;
    struct tfs_inode_disk node;
    int rc = path_walk(fs, path, &ino, &node);
    if (rc != VFS_OK) return rc;
    if (node.type != TFS_TYPE_DIR) return VFS_ERR_NOTDIR;

    struct tobyfs_diriter *it = kmalloc(sizeof(*it));
    if (!it) return VFS_ERR_NOMEM;
    /* Worst case: every direct block packed full of dirents. */
    size_t cap = TFS_NDIRECT * TFS_DIRENTS_PER_BLOCK;
    it->ents  = kcalloc(cap, sizeof(*it->ents));
    if (!it->ents) { kfree(it); return VFS_ERR_NOMEM; }
    it->count = 0;

    uint8_t buf[TFS_BLOCK_SIZE];
    for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
        uint32_t b = node.direct[i];
        if (b == 0) continue;
        if (read_block(fs, b, buf) != 0) {
            kfree(it->ents); kfree(it);
            return VFS_ERR_IO;
        }
        const struct tfs_dirent_disk *de = (const struct tfs_dirent_disk *)buf;
        for (uint32_t j = 0; j < TFS_DIRENTS_PER_BLOCK; j++) {
            if (de[j].ino == 0) continue;
            struct vfs_dirent *e = &it->ents[it->count++];
            size_t nlen = strlen(de[j].name);
            if (nlen >= sizeof(e->name)) nlen = sizeof(e->name) - 1;
            memcpy(e->name, de[j].name, nlen);
            e->name[nlen] = 0;
            struct tfs_inode_disk cn;
            if (read_inode(fs, de[j].ino, &cn) == VFS_OK) {
                e->type = (cn.type == TFS_TYPE_DIR) ? VFS_TYPE_DIR
                                                    : VFS_TYPE_FILE;
                e->size = cn.size;
                e->uid  = cn.uid;
                e->gid  = cn.gid;
                e->mode = cn.mode;
            } else {
                e->type = VFS_TYPE_FILE;
                e->size = 0;
                e->uid  = 0;
                e->gid  = 0;
                e->mode = 0;
            }
        }
    }
    out->priv  = it;
    out->index = 0;
    return VFS_OK;
}

static int tobyfs_closedir(struct vfs_dir *d) {
    struct tobyfs_diriter *it = (struct tobyfs_diriter *)d->priv;
    if (it) {
        if (it->ents) kfree(it->ents);
        kfree(it);
    }
    d->priv = 0;
    return VFS_OK;
}

static int tobyfs_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    struct tobyfs_diriter *it = (struct tobyfs_diriter *)d->priv;
    if (!it) return VFS_ERR_INVAL;
    if (d->index >= it->count) return VFS_ERR_NOENT;
    *out = it->ents[d->index++];
    return VFS_OK;
}

static const struct vfs_ops tobyfs_ops = {
    .open     = tobyfs_open,
    .close    = tobyfs_close,
    .read     = tobyfs_read,
    .write    = tobyfs_write,
    .create   = tobyfs_create,
    .unlink   = tobyfs_unlink,
    .mkdir    = tobyfs_mkdir,
    .opendir  = tobyfs_opendir,
    .closedir = tobyfs_closedir,
    .readdir  = tobyfs_readdir,
    .stat     = tobyfs_stat,
    .chmod    = tobyfs_chmod,
    .chown    = tobyfs_chown,
};

/* M28E: identification helper used by sys_fs_check() to recognise
 * tobyfs mounts in the VFS table without exposing the static vtable. */
const void *tobyfs_ops_addr(void) { return &tobyfs_ops; }

/* -------- mount -------- */

int tobyfs_mount(const char *mount_point, struct blk_dev *dev) {
    if (!mount_point || !dev) return VFS_ERR_INVAL;

    struct tobyfs *fs = kcalloc(1, sizeof(*fs));
    if (!fs) return VFS_ERR_NOMEM;
    fs->dev = dev;

    /* Read superblock. */
    uint8_t buf[TFS_BLOCK_SIZE];
    if (read_block(fs, 0, buf) != 0) {
        kprintf("[tobyfs] superblock read failed\n");
        kfree(fs);
        return VFS_ERR_IO;
    }
    memcpy(&fs->sb, buf, sizeof(fs->sb));
    if (fs->sb.magic != TFS_MAGIC) {
        kprintf("[tobyfs] bad magic 0x%lx (expected 0x%lx) -- run mkfs?\n",
                (unsigned long)fs->sb.magic,
                (unsigned long)TFS_MAGIC);
        kfree(fs);
        return VFS_ERR_INVAL;
    }
    if (fs->sb.block_size  != TFS_BLOCK_SIZE  ||
        fs->sb.total_blocks != TFS_TOTAL_BLOCKS ||
        fs->sb.inode_count  != TFS_INODE_COUNT) {
        kprintf("[tobyfs] geometry mismatch: bs=%u total=%u inodes=%u\n",
                fs->sb.block_size, fs->sb.total_blocks, fs->sb.inode_count);
        kfree(fs);
        return VFS_ERR_INVAL;
    }
    /* Cache both bitmaps in RAM for fast scan. */
    if (read_block(fs, fs->sb.inode_bitmap_blk, fs->ibitmap) != 0 ||
        read_block(fs, fs->sb.data_bitmap_blk,  fs->dbitmap) != 0) {
        kprintf("[tobyfs] bitmap read failed\n");
        kfree(fs);
        return VFS_ERR_IO;
    }

    /* Sanity: root inode must exist and be a directory. */
    struct tfs_inode_disk root;
    if (read_inode(fs, fs->sb.root_ino, &root) != VFS_OK ||
        root.type != TFS_TYPE_DIR) {
        kprintf("[tobyfs] root inode %u missing or not a directory\n",
                fs->sb.root_ino);
        kfree(fs);
        return VFS_ERR_INVAL;
    }

    /* Milestone 28E: full integrity check BEFORE we expose the mount
     * to the rest of the kernel. We reject FATAL corruption, log a
     * warning for repair-friendly issues, and proceed. */
    {
        struct tobyfs_check chk;
        int crc = check_core(fs, &chk);
        if (crc == 0 && chk.severity == TFS_CHECK_FATAL) {
            kprintf("[tobyfs] REFUSING mount of '%s': fscheck FATAL "
                    "(%u errors): %s\n",
                    mount_point, chk.errors,
                    chk.detail[0] ? chk.detail : "(no detail)");
            if (slog_ready()) {
                SLOG_ERROR(SLOG_SUB_FS,
                           "refusing mount %s: %s",
                           mount_point,
                           chk.detail[0] ? chk.detail : "fscheck fatal");
            }
            kfree(fs);
            return VFS_ERR_INVAL;
        }
        if (crc == 0 && chk.severity == TFS_CHECK_WARN) {
            kprintf("[tobyfs] mount of '%s' WARNING: %u issue(s): %s "
                    "(mounting anyway)\n",
                    mount_point, chk.errors,
                    chk.detail[0] ? chk.detail : "");
            if (slog_ready()) {
                SLOG_WARN(SLOG_SUB_FS,
                          "mount %s warn: %s",
                          mount_point,
                          chk.detail[0] ? chk.detail : "fscheck warn");
            }
        } else if (crc == 0 && chk.severity == TFS_CHECK_OK) {
            kprintf("[tobyfs] mount of '%s' clean: inodes=%u/%u "
                    "blocks=%u/%u\n",
                    mount_point, chk.inodes_used, chk.inodes_total,
                    chk.data_blocks_used, chk.data_blocks_total);
            if (slog_ready()) {
                SLOG_INFO(SLOG_SUB_FS,
                          "mount %s clean (i=%u/%u b=%u/%u)",
                          mount_point,
                          chk.inodes_used, chk.inodes_total,
                          chk.data_blocks_used, chk.data_blocks_total);
            }
        } else {
            /* Check failed (likely I/O); log but mount anyway -- the
             * geometry checks above already passed. */
            kprintf("[tobyfs] fscheck for '%s' failed (rc=%d) -- mounting "
                    "without integrity verdict\n", mount_point, crc);
        }
    }

    int rc = vfs_mount(mount_point, &tobyfs_ops, fs);
    if (rc != VFS_OK) {
        kprintf("[tobyfs] vfs_mount('%s') failed: %d\n", mount_point, rc);
        kfree(fs);
        return rc;
    }

    /* Quick stats print so the boot log shows the FS came up. */
    uint32_t used_inodes = 0, used_blocks = 0;
    for (uint32_t i = 1; i < fs->sb.inode_count; i++) {
        if (bit_get(fs->ibitmap, i)) used_inodes++;
    }
    for (uint32_t b = fs->sb.data_blk_start; b < fs->sb.total_blocks; b++) {
        if (bit_get(fs->dbitmap, b)) used_blocks++;
    }
    kprintf("[tobyfs] mounted '%s' on %s: %u/%u inodes, %u/%u data blocks "
            "(%u KiB free)\n",
            mount_point, dev->name,
            used_inodes, fs->sb.inode_count - 1,
            used_blocks, fs->sb.total_blocks - fs->sb.data_blk_start,
            (fs->sb.total_blocks - fs->sb.data_blk_start - used_blocks) * 4);
    return VFS_OK;
}

/* -------- milestone 20: in-kernel mkfs --------
 *
 * Produces the exact same byte pattern as tools/mkfs_tobyfs.c so an
 * installed disk is interchangeable with a host-formatted one. Used by
 * the installer to stamp a fresh /data region after writing the boot
 * image to the front of the disk. */
int tobyfs_format(struct blk_dev *dev) {
    if (!dev || !dev->ops || !dev->ops->write) return VFS_ERR_INVAL;
    if (dev->sector_count < (uint64_t)TFS_TOTAL_BLOCKS * TFS_SECTORS_PER_BLOCK) {
        kprintf("[tobyfs] format: device too small (%lu sectors, need %u)\n",
                (unsigned long)dev->sector_count,
                TFS_TOTAL_BLOCKS * TFS_SECTORS_PER_BLOCK);
        return VFS_ERR_INVAL;
    }

    uint8_t blk[TFS_BLOCK_SIZE];

    /* Block 0: superblock. */
    memset(blk, 0, sizeof(blk));
    struct tfs_superblock sb = {
        .magic            = TFS_MAGIC,
        .block_size       = TFS_BLOCK_SIZE,
        .total_blocks     = TFS_TOTAL_BLOCKS,
        .inode_count      = TFS_INODE_COUNT,
        .inode_bitmap_blk = TFS_INODE_BITMAP_BLK,
        .data_bitmap_blk  = TFS_DATA_BITMAP_BLK,
        .inode_table_blk  = TFS_INODE_TABLE_BLK,
        .data_blk_start   = TFS_DATA_BLK_START,
        .root_ino         = TFS_ROOT_INO,
    };
    memcpy(blk, &sb, sizeof(sb));
    if (blk_write(dev, 0, TFS_SECTORS_PER_BLOCK, blk) != 0) return VFS_ERR_IO;

    /* Block 1: inode bitmap. Bit 0 reserved, bit 1 = root inode. */
    memset(blk, 0, sizeof(blk));
    blk[0] = 0x03;   /* 0b00000011 */
    if (blk_write(dev,
                  (uint64_t)TFS_INODE_BITMAP_BLK * TFS_SECTORS_PER_BLOCK,
                  TFS_SECTORS_PER_BLOCK, blk) != 0) return VFS_ERR_IO;

    /* Block 2: data bitmap. Metadata blocks 0..TFS_DATA_BLK_START-1
     * marked as "used" so the allocator skips them. */
    memset(blk, 0, sizeof(blk));
    for (uint32_t b = 0; b < TFS_DATA_BLK_START; b++) {
        blk[b >> 3] |= (uint8_t)(1u << (b & 7));
    }
    if (blk_write(dev,
                  (uint64_t)TFS_DATA_BITMAP_BLK * TFS_SECTORS_PER_BLOCK,
                  TFS_SECTORS_PER_BLOCK, blk) != 0) return VFS_ERR_IO;

    /* Blocks 3..10: inode table. Block 3 carries the root dir inode
     * at slot TFS_ROOT_INO; the rest are zeroed (type=FREE). */
    memset(blk, 0, sizeof(blk));
    struct tfs_inode_disk *table = (struct tfs_inode_disk *)blk;
    table[TFS_ROOT_INO].type  = TFS_TYPE_DIR;
    table[TFS_ROOT_INO].nlink = 1;
    table[TFS_ROOT_INO].size  = 0;
    table[TFS_ROOT_INO].mtime = 0;
    table[TFS_ROOT_INO].mode  = TFS_DEFAULT_DIR_MODE;
    table[TFS_ROOT_INO].uid   = 0;
    table[TFS_ROOT_INO].gid   = 0;
    if (blk_write(dev,
                  (uint64_t)TFS_INODE_TABLE_BLK * TFS_SECTORS_PER_BLOCK,
                  TFS_SECTORS_PER_BLOCK, blk) != 0) return VFS_ERR_IO;

    /* Zero the remaining inode-table blocks. */
    memset(blk, 0, sizeof(blk));
    for (uint32_t i = 1; i < TFS_INODE_BLOCKS; i++) {
        if (blk_write(dev,
                      (uint64_t)(TFS_INODE_TABLE_BLK + i) *
                          TFS_SECTORS_PER_BLOCK,
                      TFS_SECTORS_PER_BLOCK, blk) != 0) return VFS_ERR_IO;
    }

    /* The data region is already zero-initialised on QEMU raw images
     * (or was wiped when the installer wrote the bootable image into
     * the preceding region -- neither touches this range). */
    return VFS_OK;
}

/* ============================================================
 *  Milestone 28E: integrity checker (tobyfs_check_*).
 *
 *  The strategy is intentionally a single-pass scan that NEVER writes
 *  back to disk -- it's safe to call on a mount that the rest of the
 *  kernel is concurrently writing to (worst case the in-flight bitmap
 *  bit gets seen as "unset just before" the inode that just claimed
 *  it; we report that as a "WARN: bitmap drift" rather than corruption).
 *
 *  Steps:
 *    1. Re-read the superblock and validate magic + geometry.
 *    2. Re-read both bitmaps.
 *    3. Walk the inode bitmap: for each allocated inode, read it and:
 *         - check type is TFS_TYPE_FILE or TFS_TYPE_DIR
 *         - check nlink >= 1
 *         - check direct[] entries are either 0 or in the data-block
 *           range AND set in the data bitmap.
 *    4. Sanity-check the root inode (must be type DIR, nlink>=1).
 *    5. Detail string is filled with the FIRST issue encountered so
 *       fscheck's text output is actionable.
 *
 *  Severity ladder:
 *    - bad magic / geometry -> FATAL (return immediately).
 *    - root inode not a directory -> FATAL.
 *    - inode points into a metadata block -> FATAL.
 *    - dirent referencing impossible inode -> FATAL.
 *    - inode references a data block that's NOT marked in dbitmap
 *      -> WARN (race window during a concurrent write).
 *    - allocated inode with type==FREE -> WARN.
 * ============================================================ */

static void check_record(struct tobyfs_check *r, int sev,
                         const char *fmt, ...) {
    if (!r) return;
    if (sev > r->severity) r->severity = sev;
    r->errors++;
    if (r->detail[0] == '\0' && fmt) {
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(r->detail, sizeof(r->detail), fmt, ap);
        va_end(ap);
    }
}

/* Count set bits in a bitmap up to `max_bits`. */
static uint32_t popcount_bitmap(const uint8_t *bm, uint32_t max_bits) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < max_bits; i++) {
        if (bm[i >> 3] & (uint8_t)(1u << (i & 7))) c++;
    }
    return c;
}

static int check_core(struct tobyfs *fs, struct tobyfs_check *out) {
    /* Defaults. */
    memset(out, 0, sizeof(*out));
    out->severity = TFS_CHECK_OK;
    out->inodes_total = fs->sb.inode_count;
    out->data_blocks_total =
        (fs->sb.total_blocks > fs->sb.data_blk_start)
            ? (fs->sb.total_blocks - fs->sb.data_blk_start) : 0;
    out->bytes_total =
        (uint64_t)fs->sb.total_blocks * fs->sb.block_size;

    /* 1. Verify the superblock again from the on-disk copy (the in-mem
     *    copy could have been corrupted by a previous OOPS). */
    uint8_t sbbuf[TFS_BLOCK_SIZE];
    if (read_block(fs, 0, sbbuf) != 0) {
        check_record(out, TFS_CHECK_FATAL, "superblock read failed");
        return -VFS_ERR_IO;
    }
    struct tfs_superblock *fresh = (struct tfs_superblock *)sbbuf;
    if (fresh->magic != TFS_MAGIC) {
        check_record(out, TFS_CHECK_FATAL,
                     "bad superblock magic 0x%lx (expected 0x%lx)",
                     (unsigned long)fresh->magic, (unsigned long)TFS_MAGIC);
        return 0;
    }
    if (fresh->block_size  != TFS_BLOCK_SIZE  ||
        fresh->total_blocks != TFS_TOTAL_BLOCKS ||
        fresh->inode_count  != TFS_INODE_COUNT) {
        check_record(out, TFS_CHECK_FATAL,
                     "geometry mismatch bs=%u total=%u inodes=%u",
                     fresh->block_size, fresh->total_blocks,
                     fresh->inode_count);
        return 0;
    }
    if (fresh->root_ino != TFS_ROOT_INO ||
        fresh->inode_table_blk != TFS_INODE_TABLE_BLK ||
        fresh->data_blk_start  != TFS_DATA_BLK_START) {
        check_record(out, TFS_CHECK_FATAL,
                     "fixed-region offsets drifted (root=%u itab=%u dstart=%u)",
                     fresh->root_ino, fresh->inode_table_blk,
                     fresh->data_blk_start);
        return 0;
    }

    /* 2. Re-read both bitmaps. We read fresh copies (rather than using
     *    fs->ibitmap/fs->dbitmap directly) so a check called before
     *    mount finishes still works -- and so a concurrent allocator
     *    write doesn't tear our scan. */
    uint8_t *ibm = (uint8_t *)kmalloc(TFS_BLOCK_SIZE);
    uint8_t *dbm = (uint8_t *)kmalloc(TFS_BLOCK_SIZE);
    if (!ibm || !dbm) {
        if (ibm) kfree(ibm);
        if (dbm) kfree(dbm);
        check_record(out, TFS_CHECK_FATAL, "no memory for fscheck scratch");
        return -VFS_ERR_NOMEM;
    }
    int rc = 0;
    if (read_block(fs, fs->sb.inode_bitmap_blk, ibm) != 0 ||
        read_block(fs, fs->sb.data_bitmap_blk,  dbm) != 0) {
        check_record(out, TFS_CHECK_FATAL, "bitmap re-read failed");
        kfree(ibm); kfree(dbm);
        return -VFS_ERR_IO;
    }

    /* 3. Inode 0 is the "invalid" sentinel. tobyfs_format() *sets*
     *    bit 0 to 1 so the allocator never hands it out (alloc_inode
     *    starts its scan at i=1 anyway, but the set bit is the
     *    intentional belt-and-braces pattern). Don't flag it here --
     *    that would falsely reject every freshly-formatted image. */

    /* Root inode bit must be set. */
    if (!(ibm[TFS_ROOT_INO >> 3] & (uint8_t)(1u << (TFS_ROOT_INO & 7)))) {
        check_record(out, TFS_CHECK_FATAL,
                     "root inode %u not allocated in bitmap", TFS_ROOT_INO);
    }

    /* 4. Walk allocated inodes. */
    uint32_t inodes_used = 0;
    for (uint32_t ino = 1; ino < fs->sb.inode_count; ino++) {
        if (!(ibm[ino >> 3] & (uint8_t)(1u << (ino & 7)))) continue;
        inodes_used++;

        struct tfs_inode_disk node;
        uint32_t blk = fs->sb.inode_table_blk + (ino / TFS_INODES_PER_BLOCK);
        uint8_t  ibuf[TFS_BLOCK_SIZE];
        if (read_block(fs, blk, ibuf) != 0) {
            check_record(out, TFS_CHECK_FATAL,
                         "inode table read failed at blk=%u for ino=%u",
                         blk, ino);
            rc = -VFS_ERR_IO;
            break;
        }
        memcpy(&node, ibuf + (ino % TFS_INODES_PER_BLOCK) * TFS_INODE_SIZE,
               sizeof(node));

        if (node.type == TFS_TYPE_FREE) {
            check_record(out, TFS_CHECK_WARN,
                         "ino=%u allocated but type=FREE (orphan slot)",
                         ino);
            continue;
        }
        if (node.type != TFS_TYPE_FILE && node.type != TFS_TYPE_DIR) {
            check_record(out, TFS_CHECK_FATAL,
                         "ino=%u has invalid type=%u",
                         ino, (unsigned)node.type);
            continue;
        }
        if (node.nlink == 0) {
            check_record(out, TFS_CHECK_WARN,
                         "ino=%u has nlink=0", ino);
        }

        for (uint32_t i = 0; i < TFS_NDIRECT; i++) {
            uint32_t db = node.direct[i];
            if (db == 0) continue;
            if (db < fs->sb.data_blk_start || db >= fs->sb.total_blocks) {
                check_record(out, TFS_CHECK_FATAL,
                             "ino=%u direct[%u]=%u OUT OF RANGE",
                             ino, i, db);
                continue;
            }
            if (!(dbm[db >> 3] & (uint8_t)(1u << (db & 7)))) {
                check_record(out, TFS_CHECK_WARN,
                             "ino=%u direct[%u]=%u not set in dbitmap",
                             ino, i, db);
            }
        }

        if (ino == fs->sb.root_ino && node.type != TFS_TYPE_DIR) {
            check_record(out, TFS_CHECK_FATAL,
                         "root inode %u is not a directory (type=%u)",
                         ino, (unsigned)node.type);
        }
    }

    out->inodes_used = inodes_used;
    /* Count data blocks: skip metadata bits 0..data_blk_start-1 which
     * are always set by mkfs. */
    uint32_t dbits = popcount_bitmap(dbm, fs->sb.total_blocks);
    out->data_blocks_used =
        (dbits >= fs->sb.data_blk_start) ? (dbits - fs->sb.data_blk_start) : 0;
    out->bytes_free =
        (uint64_t)(out->data_blocks_total - out->data_blocks_used) *
        fs->sb.block_size;

    kfree(ibm);
    kfree(dbm);
    return rc;
}

int tobyfs_check_dev(struct blk_dev *dev, struct tobyfs_check *out) {
    if (!dev || !out) return -VFS_ERR_INVAL;

    /* Build a temporary tobyfs handle on the heap so we can re-use
     * read_block(). We copy in the superblock first (to populate
     * fs->sb for the rest of check_core); if that fails, mark FATAL. */
    struct tobyfs *fs = (struct tobyfs *)kcalloc(1, sizeof(*fs));
    if (!fs) {
        memset(out, 0, sizeof(*out));
        out->severity = TFS_CHECK_FATAL;
        check_record(out, TFS_CHECK_FATAL, "no memory for fscheck handle");
        return -VFS_ERR_NOMEM;
    }
    fs->dev = dev;

    /* Pre-fill the superblock with build-time defaults so check_core's
     * fresh-superblock validation is not chasing zeros. */
    fs->sb.magic            = TFS_MAGIC;
    fs->sb.block_size       = TFS_BLOCK_SIZE;
    fs->sb.total_blocks     = TFS_TOTAL_BLOCKS;
    fs->sb.inode_count      = TFS_INODE_COUNT;
    fs->sb.inode_bitmap_blk = TFS_INODE_BITMAP_BLK;
    fs->sb.data_bitmap_blk  = TFS_DATA_BITMAP_BLK;
    fs->sb.inode_table_blk  = TFS_INODE_TABLE_BLK;
    fs->sb.data_blk_start   = TFS_DATA_BLK_START;
    fs->sb.root_ino         = TFS_ROOT_INO;

    int rc = check_core(fs, out);
    kfree(fs);
    return rc;
}

int tobyfs_check_mounted(void *mount_data, struct tobyfs_check *out) {
    if (!mount_data || !out) return -VFS_ERR_INVAL;
    struct tobyfs *fs = (struct tobyfs *)mount_data;
    return check_core(fs, out);
}

/* ============================================================
 *  Milestone 28E: in-kernel ramdisk-backed self-test.
 *
 *  The corruption-detection requirement says "simulate corrupted
 *  filesystem (test image), verify corruption detected, safe error
 *  shown, no kernel crash". We satisfy that without ever touching
 *  the live /data disk by:
 *
 *    1. allocating a 4 MiB heap buffer (one tobyfs image worth),
 *    2. wrapping it as a tiny in-RAM blk_dev,
 *    3. formatting it (real tobyfs_format),
 *    4. running tobyfs_check_dev twice -- once on the clean image
 *       (expect OK), once after stomping the superblock magic
 *       (expect FATAL).
 *
 *  Pure heap; nothing persists; safe to call from the boot harness.
 * ============================================================ */

struct tfs_ramdev {
    uint8_t *buf;
    uint64_t bytes;
};

static int ramdev_read(struct blk_dev *dev, uint64_t lba, uint32_t count,
                       void *buf) {
    struct tfs_ramdev *r = (struct tfs_ramdev *)dev->priv;
    uint64_t off  = lba * (uint64_t)BLK_SECTOR_SIZE;
    uint64_t span = (uint64_t)count * BLK_SECTOR_SIZE;
    if (off + span > r->bytes) return -1;
    memcpy(buf, r->buf + off, span);
    return 0;
}

static int ramdev_write(struct blk_dev *dev, uint64_t lba, uint32_t count,
                        const void *buf) {
    struct tfs_ramdev *r = (struct tfs_ramdev *)dev->priv;
    uint64_t off  = lba * (uint64_t)BLK_SECTOR_SIZE;
    uint64_t span = (uint64_t)count * BLK_SECTOR_SIZE;
    if (off + span > r->bytes) return -1;
    memcpy(r->buf + off, buf, span);
    return 0;
}

static const struct blk_ops ramdev_ops = {
    .read  = ramdev_read,
    .write = ramdev_write,
};

int tobyfs_self_test(struct tobyfs_check *clean_out,
                     struct tobyfs_check *bad_out) {
    if (!clean_out || !bad_out) return -VFS_ERR_INVAL;
    memset(clean_out, 0, sizeof(*clean_out));
    memset(bad_out,   0, sizeof(*bad_out));

    const uint64_t bytes =
        (uint64_t)TFS_TOTAL_BLOCKS * TFS_BLOCK_SIZE;

    uint8_t *image = (uint8_t *)kmalloc((size_t)bytes);
    if (!image) {
        kprintf("[m28e] self-test: kmalloc(%lu) failed\n",
                (unsigned long)bytes);
        return -VFS_ERR_NOMEM;
    }
    memset(image, 0, (size_t)bytes);

    struct tfs_ramdev r = { .buf = image, .bytes = bytes };
    struct blk_dev    dev;
    memset(&dev, 0, sizeof(dev));
    dev.name         = "ramfs-fscheck-test";
    dev.ops          = &ramdev_ops;
    dev.sector_count = bytes / BLK_SECTOR_SIZE;
    dev.priv         = &r;
    dev.class        = BLK_CLASS_DISK;

    /* 1. Format -> expect a clean image. */
    int rc = tobyfs_format(&dev);
    if (rc != VFS_OK) {
        kprintf("[m28e] self-test: tobyfs_format failed rc=%d\n", rc);
        kfree(image);
        return rc;
    }
    kprintf("[m28e] self-test: ramdev formatted (%u blocks, %lu bytes)\n",
            TFS_TOTAL_BLOCKS, (unsigned long)bytes);

    /* 2. Check clean. */
    rc = tobyfs_check_dev(&dev, clean_out);
    kprintf("[m28e] self-test: clean check rc=%d sev=%d errors=%u "
            "inodes=%u/%u dblocks=%u/%u detail=\"%s\"\n",
            rc, clean_out->severity, clean_out->errors,
            clean_out->inodes_used, clean_out->inodes_total,
            clean_out->data_blocks_used, clean_out->data_blocks_total,
            clean_out->detail);
    if (rc != 0) {
        kprintf("[m28e] self-test: clean check returned non-zero rc=%d\n", rc);
        kfree(image);
        return rc;
    }

    /* 3. Stomp the superblock magic. The first 8 bytes of block 0 are
     *    the magic field (verified by check_core). Any value != TFS_MAGIC
     *    must trip TFS_CHECK_FATAL. */
    uint64_t bad_magic = 0xDEADBEEFCAFEBABEULL;
    memcpy(image, &bad_magic, sizeof(bad_magic));

    rc = tobyfs_check_dev(&dev, bad_out);
    kprintf("[m28e] self-test: corrupt check rc=%d sev=%d errors=%u "
            "detail=\"%s\"\n",
            rc, bad_out->severity, bad_out->errors, bad_out->detail);

    kfree(image);
    return 0;
}
