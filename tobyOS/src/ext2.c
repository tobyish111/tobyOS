/* ext2.c -- read-write ext2 filesystem driver.
 *
 * Provides full read-write ext2 support using the same vfs_ops table
 * that ramfs / tobyfs / fat32 / ext4 use. The driver knows only about
 * `struct blk_dev *` so it works on GPT partitions, whole disks, or
 * USB-MSC LUNs without glue.
 *
 * Scope
 * -----
 *   READ:  open, read, readdir, stat, readlink
 *   WRITE: create, write, unlink, mkdir, chmod, chown, symlink
 *
 * Out of scope:
 *   - ext3/ext4 features (journal, extents, htree, inline data)
 *   - triple-indirect blocks (files > ~4 GB on 4K blocks)
 *   - hard link creation (existing hard links are followed correctly)
 *   - sparse file hole-punching
 */

#include <tobyos/ext2.h>
#include <tobyos/blk.h>
#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

/* ---- in-memory mount state ---- */

/* Concurrent open handles tracked per mount. Sized for "plenty";
 * exceeding it is reported, never silently ignored. */
#define EXT2_MAX_OPEN_INODES 64

struct ext2 {
    struct blk_dev *dev;

    uint32_t block_size;
    uint32_t sectors_per_block;
    uint64_t total_blocks;
    uint32_t total_inodes;
    uint32_t inode_size;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t first_ino;
    uint32_t first_data_block;
    uint32_t group_count;
    uint16_t desc_size;

    uint8_t *gdt_buf;
    uint32_t gdt_bytes;

    uint8_t *blk_buf;
    uint8_t *blk_buf2;
    uint8_t *blk_buf3;

    /* Open handles per inode, so unlink() can defer the release to the
     * last close the way POSIX requires. Same reasoning as ext4's. */
    struct ext2_openref {
        uint32_t ino;             /* 0 = free slot */
        int      refs;
        bool     orphan;          /* the last NAME went away while open */
    } openrefs[EXT2_MAX_OPEN_INODES];
};

/* Per-handle state for an open file. */
struct ext2_filepriv {
    uint32_t          inode_no;
    struct ext4_inode in;
    uint64_t          file_size;
    uint32_t          parent_ino;
};

/* Per-handle state for an open directory. */
struct ext2_diriter {
    struct vfs_dirent *ents;
    size_t             count;
};

/* ---- low-level block I/O ---- */

static int read_block(struct ext2 *fs, uint64_t blk, void *buf) {
    if (blk >= fs->total_blocks) return VFS_ERR_INVAL;
    uint64_t lba = blk * fs->sectors_per_block;
    if (blk_read(fs->dev, lba, fs->sectors_per_block, buf) != 0)
        return VFS_ERR_IO;
    return VFS_OK;
}

static int write_block(struct ext2 *fs, uint64_t blk, const void *buf) {
    if (blk >= fs->total_blocks) return VFS_ERR_INVAL;
    uint64_t lba = blk * fs->sectors_per_block;
    if (blk_write(fs->dev, lba, fs->sectors_per_block, buf) != 0)
        return VFS_ERR_IO;
    return VFS_OK;
}

static int read_block_partial(struct ext2 *fs, uint64_t blk,
                              uint32_t off, uint32_t len, void *out) {
    if (off + len > fs->block_size) return VFS_ERR_INVAL;
    int rc = read_block(fs, blk, fs->blk_buf);
    if (rc != VFS_OK) return rc;
    memcpy(out, fs->blk_buf + off, len);
    return VFS_OK;
}

/* ---- group descriptor access ---- */

static const struct ext4_group_desc_32 *gd32(const struct ext2 *fs, uint32_t g) {
    return (const struct ext4_group_desc_32 *)(fs->gdt_buf +
                                               (uint64_t)g * fs->desc_size);
}

static uint64_t group_inode_table(const struct ext2 *fs, uint32_t g) {
    return gd32(fs, g)->bg_inode_table_lo;
}

static uint64_t group_block_bitmap(const struct ext2 *fs, uint32_t g) {
    return gd32(fs, g)->bg_block_bitmap_lo;
}

static uint64_t group_inode_bitmap(const struct ext2 *fs, uint32_t g) {
    return gd32(fs, g)->bg_inode_bitmap_lo;
}

/* Flush the cached GDT back to disk. */
static int flush_gdt(struct ext2 *fs) {
    uint32_t gdt_blocks = (fs->gdt_bytes + fs->block_size - 1) / fs->block_size;
    uint32_t gdt_first = fs->first_data_block + 1;
    for (uint32_t i = 0; i < gdt_blocks; i++) {
        int rc = write_block(fs, gdt_first + i,
                             fs->gdt_buf + i * fs->block_size);
        if (rc != VFS_OK) return rc;
    }
    return VFS_OK;
}

/* ---- inode read/write ---- */

static uint64_t ext2_now_secs(void);

static int read_inode(struct ext2 *fs, uint32_t ino, struct ext4_inode *out) {
    if (ino == 0) return VFS_ERR_INVAL;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t idx   = (ino - 1) % fs->inodes_per_group;
    if (group >= fs->group_count) return VFS_ERR_INVAL;

    uint64_t itab = group_inode_table(fs, group);
    uint64_t byte_off = (uint64_t)idx * fs->inode_size;
    uint64_t blk = itab + byte_off / fs->block_size;
    uint32_t off = (uint32_t)(byte_off % fs->block_size);

    return read_block_partial(fs, blk, off, sizeof(*out), out);
}

static int write_inode(struct ext2 *fs, uint32_t ino, const struct ext4_inode *in) {
    if (ino == 0) return VFS_ERR_INVAL;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t idx   = (ino - 1) % fs->inodes_per_group;
    if (group >= fs->group_count) return VFS_ERR_INVAL;

    uint64_t itab = group_inode_table(fs, group);
    uint64_t byte_off = (uint64_t)idx * fs->inode_size;
    uint64_t blk = itab + byte_off / fs->block_size;
    uint32_t off = (uint32_t)(byte_off % fs->block_size);

    int rc = read_block(fs, blk, fs->blk_buf);
    if (rc != VFS_OK) return rc;
    memcpy(fs->blk_buf + off, in, sizeof(*in));
    return write_block(fs, blk, fs->blk_buf);
}

/* ---- block-pointer resolution (no extents) ---- */

static int inode_block_map(struct ext2 *fs, const struct ext4_inode *in,
                           uint32_t lblk, uint64_t *out_phys) {
    *out_phys = 0;
    uint32_t per_block = fs->block_size / 4;

    /* Direct blocks (0..11). */
    if (lblk < 12) {
        *out_phys = in->i_block[lblk];
        return VFS_OK;
    }

    /* Single indirect (12..12+per_block-1). */
    if (lblk < 12u + per_block) {
        uint32_t ind = in->i_block[12];
        if (ind == 0) return VFS_OK;
        int rc = read_block(fs, ind, fs->blk_buf2);
        if (rc != VFS_OK) return rc;
        const uint32_t *table = (const uint32_t *)fs->blk_buf2;
        *out_phys = table[lblk - 12];
        return VFS_OK;
    }

    /* Double indirect. */
    uint32_t rem = lblk - 12 - per_block;
    if (rem < per_block * per_block) {
        uint32_t dind = in->i_block[13];
        if (dind == 0) return VFS_OK;
        int rc = read_block(fs, dind, fs->blk_buf2);
        if (rc != VFS_OK) return rc;
        uint32_t l1_idx = rem / per_block;
        uint32_t l2_idx = rem % per_block;
        uint32_t ind = ((const uint32_t *)fs->blk_buf2)[l1_idx];
        if (ind == 0) return VFS_OK;
        rc = read_block(fs, ind, fs->blk_buf2);
        if (rc != VFS_OK) return rc;
        *out_phys = ((const uint32_t *)fs->blk_buf2)[l2_idx];
        return VFS_OK;
    }

    return VFS_ERR_INVAL;
}

/* ---- read bytes from an inode ---- */

static long inode_read(struct ext2 *fs, const struct ext4_inode *in,
                       uint64_t file_size, uint64_t pos,
                       void *buf, size_t n) {
    if (pos >= file_size) return 0;
    uint64_t avail = file_size - pos;
    if (n > avail) n = (size_t)avail;
    if (n == 0) return 0;

    uint8_t *out = (uint8_t *)buf;
    size_t total = 0;
    while (n > 0) {
        uint32_t lblk = (uint32_t)(pos / fs->block_size);
        uint32_t off  = (uint32_t)(pos % fs->block_size);
        uint32_t take = fs->block_size - off;
        if (take > n) take = (uint32_t)n;

        uint64_t phys = 0;
        int rc = inode_block_map(fs, in, lblk, &phys);
        if (rc != VFS_OK) return rc;
        if (phys == 0) {
            memset(out, 0, take);
        } else {
            uint8_t *tmp = fs->blk_buf3;
            rc = read_block(fs, phys, tmp);
            if (rc != VFS_OK) return rc;
            memcpy(out, tmp + off, take);
        }
        out   += take;
        pos   += take;
        n     -= take;
        total += take;
    }
    return (long)total;
}

/* ---- block allocation ---- */

static int alloc_block(struct ext2 *fs, uint32_t pref_group, uint32_t *out_blk) {
    uint8_t *bmp = kmalloc(fs->block_size);
    if (!bmp) return VFS_ERR_NOMEM;

    for (uint32_t gi = 0; gi < fs->group_count; gi++) {
        uint32_t g = (pref_group + gi) % fs->group_count;
        uint64_t bmp_blk = group_block_bitmap(fs, g);
        if (bmp_blk == 0) continue;
        int rc = read_block(fs, bmp_blk, bmp);
        if (rc != VFS_OK) { kfree(bmp); return rc; }

        for (uint32_t bit = 0; bit < fs->blocks_per_group; bit++) {
            uint32_t byte_idx = bit / 8;
            uint8_t  bit_mask = (uint8_t)(1u << (bit & 7));
            if (byte_idx >= fs->block_size) break;
            if (bmp[byte_idx] & bit_mask) continue;

            uint32_t abs_blk = g * fs->blocks_per_group +
                               fs->first_data_block + bit;
            if (abs_blk >= fs->total_blocks) continue;

            bmp[byte_idx] |= bit_mask;
            rc = write_block(fs, bmp_blk, bmp);
            if (rc != VFS_OK) { kfree(bmp); return rc; }

            /* Update free count in GDT. */
            struct ext4_group_desc_32 *gd =
                (struct ext4_group_desc_32 *)(fs->gdt_buf +
                                              (uint64_t)g * fs->desc_size);
            if (gd->bg_free_blocks_count_lo > 0)
                gd->bg_free_blocks_count_lo--;
            flush_gdt(fs);

            kfree(bmp);
            *out_blk = abs_blk;
            return VFS_OK;
        }
    }
    kfree(bmp);
    return VFS_ERR_NOSPC;
}

static int free_block(struct ext2 *fs, uint32_t blk_no) {
    if (blk_no < fs->first_data_block || blk_no >= fs->total_blocks)
        return VFS_ERR_INVAL;

    uint32_t rel = blk_no - fs->first_data_block;
    uint32_t g = rel / fs->blocks_per_group;
    uint32_t bit = rel % fs->blocks_per_group;
    if (g >= fs->group_count) return VFS_ERR_INVAL;

    uint8_t *bmp = kmalloc(fs->block_size);
    if (!bmp) return VFS_ERR_NOMEM;

    uint64_t bmp_blk = group_block_bitmap(fs, g);
    int rc = read_block(fs, bmp_blk, bmp);
    if (rc != VFS_OK) { kfree(bmp); return rc; }

    uint32_t byte_idx = bit / 8;
    uint8_t  bit_mask = (uint8_t)(1u << (bit & 7));
    bmp[byte_idx] &= (uint8_t)~bit_mask;

    rc = write_block(fs, bmp_blk, bmp);
    kfree(bmp);
    if (rc != VFS_OK) return rc;

    struct ext4_group_desc_32 *gd =
        (struct ext4_group_desc_32 *)(fs->gdt_buf +
                                      (uint64_t)g * fs->desc_size);
    gd->bg_free_blocks_count_lo++;
    return flush_gdt(fs);
}

/* Free all data blocks referenced by an inode (direct + indirect). */
static int free_inode_blocks(struct ext2 *fs, struct ext4_inode *in) {
    uint32_t per_block = fs->block_size / 4;

    for (int i = 0; i < 12; i++) {
        if (in->i_block[i]) {
            free_block(fs, in->i_block[i]);
            in->i_block[i] = 0;
        }
    }

    if (in->i_block[12]) {
        uint8_t *ind_buf = kmalloc(fs->block_size);
        if (ind_buf) {
            if (read_block(fs, in->i_block[12], ind_buf) == VFS_OK) {
                const uint32_t *ptrs = (const uint32_t *)ind_buf;
                for (uint32_t i = 0; i < per_block; i++) {
                    if (ptrs[i]) free_block(fs, ptrs[i]);
                }
            }
            kfree(ind_buf);
        }
        free_block(fs, in->i_block[12]);
        in->i_block[12] = 0;
    }

    if (in->i_block[13]) {
        uint8_t *dind_buf = kmalloc(fs->block_size);
        if (dind_buf) {
            if (read_block(fs, in->i_block[13], dind_buf) == VFS_OK) {
                const uint32_t *l1 = (const uint32_t *)dind_buf;
                uint8_t *ind_buf = kmalloc(fs->block_size);
                if (ind_buf) {
                    for (uint32_t i = 0; i < per_block; i++) {
                        if (l1[i] == 0) continue;
                        if (read_block(fs, l1[i], ind_buf) == VFS_OK) {
                            const uint32_t *l2 = (const uint32_t *)ind_buf;
                            for (uint32_t j = 0; j < per_block; j++) {
                                if (l2[j]) free_block(fs, l2[j]);
                            }
                        }
                        free_block(fs, l1[i]);
                    }
                    kfree(ind_buf);
                }
            }
            kfree(dind_buf);
        }
        free_block(fs, in->i_block[13]);
        in->i_block[13] = 0;
    }

    in->i_blocks_lo = 0;
    return VFS_OK;
}

/* ---- inode allocation ---- */

static int alloc_inode(struct ext2 *fs, uint32_t pref_group, uint32_t *out_ino) {
    uint8_t *bmp = kmalloc(fs->block_size);
    if (!bmp) return VFS_ERR_NOMEM;

    for (uint32_t gi = 0; gi < fs->group_count; gi++) {
        uint32_t g = (pref_group + gi) % fs->group_count;
        uint64_t bmp_blk = group_inode_bitmap(fs, g);
        if (bmp_blk == 0) continue;
        int rc = read_block(fs, bmp_blk, bmp);
        if (rc != VFS_OK) { kfree(bmp); return rc; }

        for (uint32_t bit = 0; bit < fs->inodes_per_group; bit++) {
            uint32_t byte_idx = bit / 8;
            uint8_t  bit_mask = (uint8_t)(1u << (bit & 7));
            if (byte_idx >= fs->block_size) break;
            if (bmp[byte_idx] & bit_mask) continue;

            uint32_t ino = g * fs->inodes_per_group + bit + 1;
            if (ino < fs->first_ino && ino != EXT4_ROOT_INODE) continue;

            bmp[byte_idx] |= bit_mask;
            rc = write_block(fs, bmp_blk, bmp);
            if (rc != VFS_OK) { kfree(bmp); return rc; }

            struct ext4_group_desc_32 *gd =
                (struct ext4_group_desc_32 *)(fs->gdt_buf +
                                              (uint64_t)g * fs->desc_size);
            if (gd->bg_free_inodes_count_lo > 0)
                gd->bg_free_inodes_count_lo--;
            flush_gdt(fs);

            kfree(bmp);
            *out_ino = ino;
            return VFS_OK;
        }
    }
    kfree(bmp);
    return VFS_ERR_NOSPC;
}

static int free_inode(struct ext2 *fs, uint32_t ino) {
    if (ino == 0 || ino > fs->total_inodes) return VFS_ERR_INVAL;
    uint32_t g   = (ino - 1) / fs->inodes_per_group;
    uint32_t bit = (ino - 1) % fs->inodes_per_group;

    uint8_t *bmp = kmalloc(fs->block_size);
    if (!bmp) return VFS_ERR_NOMEM;

    uint64_t bmp_blk = group_inode_bitmap(fs, g);
    int rc = read_block(fs, bmp_blk, bmp);
    if (rc != VFS_OK) { kfree(bmp); return rc; }

    uint32_t byte_idx = bit / 8;
    uint8_t  bit_mask = (uint8_t)(1u << (bit & 7));
    bmp[byte_idx] &= (uint8_t)~bit_mask;

    rc = write_block(fs, bmp_blk, bmp);
    kfree(bmp);
    if (rc != VFS_OK) return rc;

    struct ext4_group_desc_32 *gd =
        (struct ext4_group_desc_32 *)(fs->gdt_buf +
                                      (uint64_t)g * fs->desc_size);
    gd->bg_free_inodes_count_lo++;
    return flush_gdt(fs);
}

/* ---- set a block pointer in an inode (allocating indirect blocks) ---- */

static int inode_set_block(struct ext2 *fs, struct ext4_inode *in,
                           uint32_t lblk, uint32_t phys_blk) {
    uint32_t per_block = fs->block_size / 4;
    uint32_t ino_group = 0;

    if (lblk < 12) {
        in->i_block[lblk] = phys_blk;
        return VFS_OK;
    }

    /* Single indirect. */
    if (lblk < 12u + per_block) {
        if (in->i_block[12] == 0) {
            uint32_t ib;
            int rc = alloc_block(fs, ino_group, &ib);
            if (rc != VFS_OK) return rc;
            uint8_t *zero = kcalloc(1, fs->block_size);
            if (!zero) return VFS_ERR_NOMEM;
            write_block(fs, ib, zero);
            kfree(zero);
            in->i_block[12] = ib;
            in->i_blocks_lo += fs->block_size / 512;
        }
        uint8_t *ind = kmalloc(fs->block_size);
        if (!ind) return VFS_ERR_NOMEM;
        int rc = read_block(fs, in->i_block[12], ind);
        if (rc != VFS_OK) { kfree(ind); return rc; }
        ((uint32_t *)ind)[lblk - 12] = phys_blk;
        rc = write_block(fs, in->i_block[12], ind);
        kfree(ind);
        return rc;
    }

    /* Double indirect. */
    uint32_t rem = lblk - 12 - per_block;
    if (rem < per_block * per_block) {
        if (in->i_block[13] == 0) {
            uint32_t ib;
            int rc = alloc_block(fs, ino_group, &ib);
            if (rc != VFS_OK) return rc;
            uint8_t *zero = kcalloc(1, fs->block_size);
            if (!zero) return VFS_ERR_NOMEM;
            write_block(fs, ib, zero);
            kfree(zero);
            in->i_block[13] = ib;
            in->i_blocks_lo += fs->block_size / 512;
        }
        uint32_t l1_idx = rem / per_block;
        uint32_t l2_idx = rem % per_block;

        uint8_t *dind = kmalloc(fs->block_size);
        if (!dind) return VFS_ERR_NOMEM;
        int rc = read_block(fs, in->i_block[13], dind);
        if (rc != VFS_OK) { kfree(dind); return rc; }

        uint32_t *l1 = (uint32_t *)dind;
        if (l1[l1_idx] == 0) {
            uint32_t ib;
            rc = alloc_block(fs, ino_group, &ib);
            if (rc != VFS_OK) { kfree(dind); return rc; }
            uint8_t *zero = kcalloc(1, fs->block_size);
            if (!zero) { kfree(dind); return VFS_ERR_NOMEM; }
            write_block(fs, ib, zero);
            kfree(zero);
            l1[l1_idx] = ib;
            in->i_blocks_lo += fs->block_size / 512;
            write_block(fs, in->i_block[13], dind);
        }

        uint32_t ind_blk = l1[l1_idx];
        kfree(dind);

        uint8_t *ind = kmalloc(fs->block_size);
        if (!ind) return VFS_ERR_NOMEM;
        rc = read_block(fs, ind_blk, ind);
        if (rc != VFS_OK) { kfree(ind); return rc; }
        ((uint32_t *)ind)[l2_idx] = phys_blk;
        rc = write_block(fs, ind_blk, ind);
        kfree(ind);
        return rc;
    }

    return VFS_ERR_NOSPC;
}

/* ---- directory iteration ---- */

typedef int (*dir_cb_t)(void *user, uint32_t ino, uint8_t file_type,
                        const char *name, uint8_t name_len);

static int dir_walk(struct ext2 *fs, const struct ext4_inode *dir_inode,
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
            if (rc == VFS_OK) continue;
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
        return VFS_ERR_NOENT;
    }
    return VFS_OK;
}

static int path_to_inode(struct ext2 *fs, const char *path,
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
        if (i == 0) break;

        struct ext4_inode dir;
        int rc = read_inode(fs, cur, &dir);
        if (rc != VFS_OK) return rc;
        if ((dir.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR)
            return VFS_ERR_NOTDIR;

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

/* Resolve path to parent inode + leaf name. */
static int path_parent(struct ext2 *fs, const char *path,
                       uint32_t *parent_ino, char *leaf, size_t leaf_sz) {
    if (!path || path[0] != '/') return VFS_ERR_INVAL;

    const char *last_slash = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    size_t dir_len = (size_t)(last_slash - path);
    if (dir_len == 0) dir_len = 1;

    char dir_path[VFS_PATH_MAX];
    if (dir_len >= sizeof(dir_path)) return VFS_ERR_NAMETOOLONG;
    memcpy(dir_path, path, dir_len);
    dir_path[dir_len] = 0;
    if (dir_path[0] != '/') { dir_path[0] = '/'; dir_path[1] = 0; }

    const char *name = last_slash + 1;
    while (*name == '/') name++;
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen >= leaf_sz) return VFS_ERR_INVAL;
    memcpy(leaf, name, nlen + 1);

    uint32_t pino; uint8_t ptype;
    int rc = path_to_inode(fs, dir_path, &pino, &ptype);
    if (rc != VFS_OK) return rc;
    if (ptype != EXT4_FT_DIR) return VFS_ERR_NOTDIR;
    *parent_ino = pino;
    return VFS_OK;
}

/* ---- directory entry management ---- */

static int dir_add_entry(struct ext2 *fs, uint32_t dir_ino,
                         const char *name, uint32_t child_ino,
                         uint8_t file_type) {
    struct ext4_inode dir;
    int rc = read_inode(fs, dir_ino, &dir);
    if (rc != VFS_OK) return rc;
    if ((dir.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return VFS_ERR_NOTDIR;

    uint8_t nlen = (uint8_t)strlen(name);
    uint16_t need_rec = (uint16_t)((8 + nlen + 3) & ~3u);

    uint64_t dir_size = ((uint64_t)dir.i_size_hi << 32) | dir.i_size_lo;
    uint32_t total_blocks = (uint32_t)((dir_size + fs->block_size - 1) /
                                       fs->block_size);

    uint8_t *blk = kmalloc(fs->block_size);
    if (!blk) return VFS_ERR_NOMEM;

    for (uint32_t lblk = 0; lblk < total_blocks; lblk++) {
        uint64_t phys = 0;
        rc = inode_block_map(fs, &dir, lblk, &phys);
        if (rc != VFS_OK || phys == 0) continue;
        rc = read_block(fs, phys, blk);
        if (rc != VFS_OK) continue;

        uint32_t off = 0;
        while (off + 8 <= fs->block_size) {
            struct ext4_dir_entry_2 *de =
                (struct ext4_dir_entry_2 *)(blk + off);
            uint16_t rec_len = de->rec_len;
            if (rec_len < 8 || off + rec_len > fs->block_size) break;

            uint16_t used = (de->inode == 0) ? 0 :
                            (uint16_t)((8 + de->name_len + 3) & ~3u);
            uint16_t avail = (uint16_t)(rec_len - used);

            if (avail >= need_rec) {
                if (de->inode != 0) {
                    /* Shrink current entry. */
                    de->rec_len = used;
                    off += used;
                } else {
                    /* Reuse this empty slot. */
                }

                struct ext4_dir_entry_2 *ne =
                    (struct ext4_dir_entry_2 *)(blk + off);
                ne->inode     = child_ino;
                ne->rec_len   = (uint16_t)(rec_len - used);
                if (de->inode == 0) ne->rec_len = rec_len;
                ne->name_len  = nlen;
                ne->file_type = file_type;
                memcpy((char *)ne + 8, name, nlen);

                rc = write_block(fs, phys, blk);
                kfree(blk);
                return rc;
            }
            off += rec_len;
        }
    }

    /* No space in existing blocks -- allocate a new directory block. */
    uint32_t new_blk;
    uint32_t dir_group = (dir_ino - 1) / fs->inodes_per_group;
    rc = alloc_block(fs, dir_group, &new_blk);
    if (rc != VFS_OK) { kfree(blk); return rc; }

    memset(blk, 0, fs->block_size);
    struct ext4_dir_entry_2 *ne = (struct ext4_dir_entry_2 *)blk;
    ne->inode     = child_ino;
    ne->rec_len   = (uint16_t)fs->block_size;
    ne->name_len  = nlen;
    ne->file_type = file_type;
    memcpy((char *)ne + 8, name, nlen);

    rc = write_block(fs, new_blk, blk);
    kfree(blk);
    if (rc != VFS_OK) return rc;

    uint32_t new_lblk = total_blocks;
    rc = inode_set_block(fs, &dir, new_lblk, new_blk);
    if (rc != VFS_OK) return rc;

    dir.i_size_lo += fs->block_size;
    dir.i_blocks_lo += fs->block_size / 512;
    return write_inode(fs, dir_ino, &dir);
}

static int dir_remove_entry(struct ext2 *fs, uint32_t dir_ino,
                            const char *name) {
    struct ext4_inode dir;
    int rc = read_inode(fs, dir_ino, &dir);
    if (rc != VFS_OK) return rc;

    uint64_t dir_size = ((uint64_t)dir.i_size_hi << 32) | dir.i_size_lo;
    uint32_t total_blocks = (uint32_t)((dir_size + fs->block_size - 1) /
                                       fs->block_size);

    uint8_t *blk = kmalloc(fs->block_size);
    if (!blk) return VFS_ERR_NOMEM;

    for (uint32_t lblk = 0; lblk < total_blocks; lblk++) {
        uint64_t phys = 0;
        rc = inode_block_map(fs, &dir, lblk, &phys);
        if (rc != VFS_OK || phys == 0) continue;
        rc = read_block(fs, phys, blk);
        if (rc != VFS_OK) continue;

        uint32_t off = 0;
        struct ext4_dir_entry_2 *prev = NULL;
        while (off + 8 <= fs->block_size) {
            struct ext4_dir_entry_2 *de =
                (struct ext4_dir_entry_2 *)(blk + off);
            uint16_t rec_len = de->rec_len;
            if (rec_len < 8 || off + rec_len > fs->block_size) break;

            if (de->inode != 0 && de->name_len == (uint8_t)strlen(name)) {
                char nbuf[VFS_NAME_MAX];
                size_t cp = de->name_len < sizeof(nbuf) - 1 ?
                            de->name_len : sizeof(nbuf) - 1;
                memcpy(nbuf, (const char *)de + 8, cp);
                nbuf[cp] = 0;
                if (strcmp(nbuf, name) == 0) {
                    if (prev) {
                        prev->rec_len = (uint16_t)(prev->rec_len + de->rec_len);
                    } else {
                        de->inode = 0;
                    }
                    rc = write_block(fs, phys, blk);
                    kfree(blk);
                    return rc;
                }
            }
            prev = de;
            off += rec_len;
        }
    }
    kfree(blk);
    return VFS_ERR_NOENT;
}

/* ---- open-handle references, so unlink can defer the release --------
 *
 * POSIX: unlink() removes the NAME; the file survives until the last
 * descriptor closes, and THAT is when the blocks come back. This driver
 * freed them the moment i_links_count hit zero, leaving an open handle
 * reading blocks already returned to the allocator -- and, once the
 * inode number was reissued, a different file entirely. mkstemp(),
 * tmpfile() and every bash here-document depend on the correct
 * behaviour. Mirrors ext4's table. */
static struct ext2_openref *oref_find(struct ext2 *fs, uint32_t ino) {
    for (size_t i = 0; i < EXT2_MAX_OPEN_INODES; i++)
        if (fs->openrefs[i].ino == ino && fs->openrefs[i].refs > 0)
            return &fs->openrefs[i];
    return 0;
}

static int oref_get(struct ext2 *fs, uint32_t ino) {
    struct ext2_openref *r = oref_find(fs, ino);
    if (r) { r->refs++; return VFS_OK; }
    for (size_t i = 0; i < EXT2_MAX_OPEN_INODES; i++) {
        if (fs->openrefs[i].refs == 0) {
            fs->openrefs[i].ino    = ino;
            fs->openrefs[i].refs   = 1;
            fs->openrefs[i].orphan = false;
            return VFS_OK;
        }
    }
    /* Refuse rather than proceed untracked -- an untracked handle is the
     * exact bug this table exists to prevent. */
    kprintf("[ext2] open-inode table full (%d) -- refusing to open ino %u "
            "untracked\n", EXT2_MAX_OPEN_INODES, (unsigned)ino);
    return VFS_ERR_NOMEM;
}

static void ext2_release_inode(struct ext2 *fs, uint32_t ino) {
    struct ext4_inode in;
    if (read_inode(fs, ino, &in) != VFS_OK) return;
    if (in.i_links_count != 0) return;        /* re-linked in the meantime */
    free_inode_blocks(fs, &in);
    in.i_size_lo = 0;
    in.i_size_hi = 0;
    in.i_dtime   = (uint32_t)ext2_now_secs();
    write_inode(fs, ino, &in);
    free_inode(fs, ino);
}

static void oref_put(struct ext2 *fs, uint32_t ino) {
    struct ext2_openref *r = oref_find(fs, ino);
    if (!r) return;
    if (--r->refs > 0) return;
    bool orphan = r->orphan;
    r->ino    = 0;
    r->orphan = false;
    if (orphan) ext2_release_inode(fs, ino);   /* now the space comes back */
}

/* ---- vfs_ops implementation ---- */

static int ext2_open(void *mnt, const char *path, struct vfs_file *out) {
    struct ext2 *fs = (struct ext2 *)mnt;
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;

    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;

    if ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return VFS_ERR_ISDIR;
    if ((in.i_mode & EXT4_S_IFMT) != EXT4_S_IFREG &&
        (in.i_mode & EXT4_S_IFMT) != EXT4_S_IFLNK)
        return VFS_ERR_NOENT;

    struct ext2_filepriv *fp = kcalloc(1, sizeof(*fp));
    if (!fp) return VFS_ERR_NOMEM;
    /* Count this handle BEFORE anyone can unlink the name out from under
     * it -- that count is what makes the deferred release possible. */
    int orc = oref_get(fs, ino);
    if (orc != VFS_OK) { kfree(fp); return orc; }
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

static int ext2_close(struct vfs_file *f) {
    if (f && f->priv) {
        struct ext2 *fs = (struct ext2 *)f->mnt;
        struct ext2_filepriv *fp = (struct ext2_filepriv *)f->priv;
        uint32_t ino = fp->inode_no;
        kfree(fp);
        f->priv = NULL;
        if (fs) oref_put(fs, ino);
    }
    return VFS_OK;
}

/* An open handle keeps its OWN copy of the inode, and every mutation --
 * this handle's, another handle's, a truncate by path -- writes the inode
 * straight to disk. So disk is the shared truth and the cached copy goes
 * stale the moment anyone else touches the file. Same reasoning, and the
 * same fix, as ext4's fp_refresh(). */
static int fp_refresh(struct ext2 *fs, struct ext2_filepriv *fp) {
    struct ext4_inode in;
    int rc = read_inode(fs, fp->inode_no, &in);
    if (rc != VFS_OK) return rc;
    fp->in        = in;
    fp->file_size = ((uint64_t)in.i_size_hi << 32) | in.i_size_lo;
    return VFS_OK;
}

static long ext2_read(struct vfs_file *f, void *buf, size_t n) {
    struct ext2 *fs = (struct ext2 *)f->mnt;
    struct ext2_filepriv *fp = (struct ext2_filepriv *)f->priv;
    if (!fp) return VFS_ERR_INVAL;
    int frc = fp_refresh(fs, fp);          /* another handle may have grown it */
    if (frc != VFS_OK) return frc;
    f->size = (size_t)fp->file_size;
    long got = inode_read(fs, &fp->in, fp->file_size, f->pos, buf, n);
    if (got > 0) f->pos += (size_t)got;
    return got;
}

static long ext2_write(struct vfs_file *f, const void *buf, size_t n) {
    struct ext2 *fs = (struct ext2 *)f->mnt;
    struct ext2_filepriv *fp = (struct ext2_filepriv *)f->priv;
    if (!fp) return VFS_ERR_INVAL;

    int frc = fp_refresh(fs, fp);
    if (frc != VFS_OK) return frc;
    f->size = (size_t)fp->file_size;

    const uint8_t *src = (const uint8_t *)buf;
    size_t written = 0;
    uint32_t ino_group = (fp->inode_no - 1) / fs->inodes_per_group;

    while (n > 0) {
        uint32_t lblk = (uint32_t)(f->pos / fs->block_size);
        uint32_t off  = (uint32_t)(f->pos % fs->block_size);
        uint32_t take = fs->block_size - off;
        if (take > n) take = (uint32_t)n;

        uint64_t phys = 0;
        int rc = inode_block_map(fs, &fp->in, lblk, &phys);
        if (rc != VFS_OK && rc != VFS_ERR_NOENT) return rc;

        if (phys == 0) {
            uint32_t new_blk;
            rc = alloc_block(fs, ino_group, &new_blk);
            if (rc != VFS_OK) return written > 0 ? (long)written : rc;
            rc = inode_set_block(fs, &fp->in, lblk, new_blk);
            if (rc != VFS_OK) return written > 0 ? (long)written : rc;
            phys = new_blk;
            fp->in.i_blocks_lo += fs->block_size / 512;
        }

        uint8_t *tmp = kmalloc(fs->block_size);
        if (!tmp) return written > 0 ? (long)written : VFS_ERR_NOMEM;

        if (take < fs->block_size) {
            rc = read_block(fs, phys, tmp);
            if (rc != VFS_OK) { kfree(tmp); return written > 0 ? (long)written : rc; }
        }

        memcpy(tmp + off, src, take);
        rc = write_block(fs, phys, tmp);
        kfree(tmp);
        if (rc != VFS_OK) return written > 0 ? (long)written : rc;

        src     += take;
        f->pos  += take;
        n       -= take;
        written += take;
    }

    if (f->pos > fp->file_size) {
        fp->file_size = f->pos;
        fp->in.i_size_lo = (uint32_t)(fp->file_size & 0xFFFFFFFF);
        fp->in.i_size_hi = (uint32_t)(fp->file_size >> 32);
        f->size = (size_t)fp->file_size;
    }

    write_inode(fs, fp->inode_no, &fp->in);
    return (long)written;
}

static int ext2_create(void *mnt, const char *path,
                       uint32_t uid, uint32_t gid, uint32_t mode) {
    struct ext2 *fs = (struct ext2 *)mnt;

    uint32_t parent_ino;
    char leaf[VFS_NAME_MAX];
    int rc = path_parent(fs, path, &parent_ino, leaf, sizeof(leaf));
    if (rc != VFS_OK) return rc;

    /* Check if it already exists. */
    uint32_t ino; uint8_t ftype;
    rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc == VFS_OK) return VFS_ERR_EXIST;

    uint32_t pgroup = (parent_ino - 1) / fs->inodes_per_group;
    uint32_t new_ino;
    rc = alloc_inode(fs, pgroup, &new_ino);
    if (rc != VFS_OK) return rc;

    struct ext4_inode in;
    memset(&in, 0, sizeof(in));
    in.i_mode = EXT4_S_IFREG | (uint16_t)(mode & 0xFFF);
    in.i_uid  = (uint16_t)uid;
    in.i_gid  = (uint16_t)gid;
    in.i_links_count = 1;

    rc = write_inode(fs, new_ino, &in);
    if (rc != VFS_OK) { free_inode(fs, new_ino); return rc; }

    rc = dir_add_entry(fs, parent_ino, leaf, new_ino, EXT4_FT_REG_FILE);
    if (rc != VFS_OK) { free_inode(fs, new_ino); return rc; }

    return VFS_OK;
}

/* Hard links. ext2_unlink() below already decrements i_links_count and
 * only releases the blocks at zero, so the inode half was always right --
 * only the second-directory-entry half was missing. */
static int ext2_link(void *mnt, const char *oldpath, const char *newpath) {
    struct ext2 *fs = (struct ext2 *)mnt;

    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, oldpath, &ino, &ftype);
    if (rc != VFS_OK) return rc;

    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;
    /* POSIX forbids hard links to directories -- they make the tree
     * cyclic and every walker that trusts it loops forever. */
    if (ftype == EXT4_FT_DIR ||
        (in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return VFS_ERR_ISDIR;

    uint32_t np_ino;
    char np_leaf[VFS_NAME_MAX];
    rc = path_parent(fs, newpath, &np_ino, np_leaf, sizeof(np_leaf));
    if (rc != VFS_OK) return rc;

    uint32_t clash; uint8_t clash_ft;
    if (path_to_inode(fs, newpath, &clash, &clash_ft) == VFS_OK)
        return VFS_ERR_EXIST;

    rc = dir_add_entry(fs, np_ino, np_leaf, ino, ftype);
    if (rc != VFS_OK) return rc;

    in.i_links_count++;
    in.i_ctime = (uint32_t)ext2_now_secs();
    return write_inode(fs, ino, &in);
}

static int ext2_unlink(void *mnt, const char *path) {
    struct ext2 *fs = (struct ext2 *)mnt;

    uint32_t parent_ino;
    char leaf[VFS_NAME_MAX];
    int rc = path_parent(fs, path, &parent_ino, leaf, sizeof(leaf));
    if (rc != VFS_OK) return rc;

    uint32_t ino; uint8_t ftype;
    rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;

    if (ftype == EXT4_FT_DIR) return VFS_ERR_ISDIR;

    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;

    rc = dir_remove_entry(fs, parent_ino, leaf);
    if (rc != VFS_OK) return rc;

    in.i_links_count--;
    if (in.i_links_count == 0) {
        struct ext2_openref *r = oref_find(fs, ino);
        if (r) {
            /* Still open: the NAME went away above, but the bytes belong
             * to those handles until the last one closes. */
            r->orphan = true;
            return write_inode(fs, ino, &in);
        }
        free_inode_blocks(fs, &in);
        in.i_size_lo = 0;
        in.i_size_hi = 0;
        in.i_dtime = (uint32_t)ext2_now_secs();
        write_inode(fs, ino, &in);
        free_inode(fs, ino);
    } else {
        write_inode(fs, ino, &in);
    }
    return VFS_OK;
}

/* Phase H: rename within the mount, dirent-level -- add the new name
 * for the same inode, drop the old, unlinking a clobbered destination
 * the way rename(2) requires. leveldb/SQLite rename a temp file over a
 * live one constantly; on ext volumes that answered ROFS forever.
 * SCOPE, stated: a DIRECTORY may only be renamed within its parent --
 * moving one across directories would leave its ".." entry pointing at
 * the old parent, and silently corrupting a foreign filesystem's tree
 * is worse than refusing. Files move freely. */
static int ext2_rename(void *mnt, const char *oldpath, const char *newpath) {
    struct ext2 *fs = (struct ext2 *)mnt;
    uint32_t src_ino; uint8_t src_ft;
    int rc = path_to_inode(fs, oldpath, &src_ino, &src_ft);
    if (rc != VFS_OK) return rc;
    uint32_t op_ino, np_ino;
    char op_leaf[VFS_NAME_MAX], np_leaf[VFS_NAME_MAX];
    rc = path_parent(fs, oldpath, &op_ino, op_leaf, sizeof op_leaf);
    if (rc != VFS_OK) return rc;
    rc = path_parent(fs, newpath, &np_ino, np_leaf, sizeof np_leaf);
    if (rc != VFS_OK) return rc;
    if (src_ft == EXT4_FT_DIR && op_ino != np_ino) return VFS_ERR_INVAL;

    uint32_t dst_ino; uint8_t dst_ft;
    int drc = path_to_inode(fs, newpath, &dst_ino, &dst_ft);
    if (drc == VFS_OK) {
        if (dst_ino == src_ino) return VFS_OK;   /* same file: no-op */
        if (dst_ft == EXT4_FT_DIR) return VFS_ERR_INVAL;
        rc = ext2_unlink(mnt, newpath);
        if (rc != VFS_OK) return rc;
    } else if (drc != VFS_ERR_NOENT) {
        return drc;
    }
    rc = dir_add_entry(fs, np_ino, np_leaf, src_ino, src_ft);
    if (rc != VFS_OK) return rc;
    return dir_remove_entry(fs, op_ino, op_leaf);
}

static int ext2_mkdir(void *mnt, const char *path,
                      uint32_t uid, uint32_t gid, uint32_t mode) {
    struct ext2 *fs = (struct ext2 *)mnt;

    uint32_t parent_ino;
    char leaf[VFS_NAME_MAX];
    int rc = path_parent(fs, path, &parent_ino, leaf, sizeof(leaf));
    if (rc != VFS_OK) return rc;

    uint32_t ino; uint8_t ftype;
    rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc == VFS_OK) return VFS_ERR_EXIST;

    uint32_t pgroup = (parent_ino - 1) / fs->inodes_per_group;
    uint32_t new_ino;
    rc = alloc_inode(fs, pgroup, &new_ino);
    if (rc != VFS_OK) return rc;

    uint32_t dir_blk;
    rc = alloc_block(fs, pgroup, &dir_blk);
    if (rc != VFS_OK) { free_inode(fs, new_ino); return rc; }

    /* Populate the new directory block with "." and ".." entries. */
    uint8_t *blk = kcalloc(1, fs->block_size);
    if (!blk) { free_block(fs, dir_blk); free_inode(fs, new_ino); return VFS_ERR_NOMEM; }

    struct ext4_dir_entry_2 *dot = (struct ext4_dir_entry_2 *)blk;
    dot->inode     = new_ino;
    dot->rec_len   = 12;
    dot->name_len  = 1;
    dot->file_type = EXT4_FT_DIR;
    memcpy((char *)dot + 8, ".", 1);

    struct ext4_dir_entry_2 *dotdot =
        (struct ext4_dir_entry_2 *)(blk + 12);
    dotdot->inode     = parent_ino;
    dotdot->rec_len   = (uint16_t)(fs->block_size - 12);
    dotdot->name_len  = 2;
    dotdot->file_type = EXT4_FT_DIR;
    memcpy((char *)dotdot + 8, "..", 2);

    rc = write_block(fs, dir_blk, blk);
    kfree(blk);
    if (rc != VFS_OK) { free_block(fs, dir_blk); free_inode(fs, new_ino); return rc; }

    struct ext4_inode in;
    memset(&in, 0, sizeof(in));
    in.i_mode  = EXT4_S_IFDIR | (uint16_t)(mode & 0xFFF);
    in.i_uid   = (uint16_t)uid;
    in.i_gid   = (uint16_t)gid;
    in.i_links_count = 2;
    in.i_size_lo = fs->block_size;
    in.i_blocks_lo = fs->block_size / 512;
    in.i_block[0] = dir_blk;

    rc = write_inode(fs, new_ino, &in);
    if (rc != VFS_OK) { free_block(fs, dir_blk); free_inode(fs, new_ino); return rc; }

    rc = dir_add_entry(fs, parent_ino, leaf, new_ino, EXT4_FT_DIR);
    if (rc != VFS_OK) { free_block(fs, dir_blk); free_inode(fs, new_ino); return rc; }

    /* Increment parent link count (for ".."). */
    struct ext4_inode parent;
    if (read_inode(fs, parent_ino, &parent) == VFS_OK) {
        parent.i_links_count++;
        write_inode(fs, parent_ino, &parent);
    }

    /* Update used-dirs count in the block group descriptor. */
    struct ext4_group_desc_32 *gd =
        (struct ext4_group_desc_32 *)(fs->gdt_buf +
                                      (uint64_t)pgroup * fs->desc_size);
    gd->bg_used_dirs_count_lo++;
    flush_gdt(fs);

    return VFS_OK;
}

static int ext2_chmod(void *mnt, const char *path, uint32_t mode) {
    struct ext2 *fs = (struct ext2 *)mnt;

    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;

    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;

    in.i_mode = (in.i_mode & EXT4_S_IFMT) | (uint16_t)(mode & 0xFFF);
    return write_inode(fs, ino, &in);
}

static int ext2_chown(void *mnt, const char *path,
                      uint32_t uid, uint32_t gid) {
    struct ext2 *fs = (struct ext2 *)mnt;

    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;

    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;

    in.i_uid = (uint16_t)uid;
    in.i_gid = (uint16_t)gid;
    return write_inode(fs, ino, &in);
}

/* ---- opendir / readdir / closedir ---- */

struct collect_ctx {
    struct ext2       *fs;
    struct vfs_dirent *out;
    size_t             cap;
    size_t             count;
    int                err;
};

static int collect_cb(void *user, uint32_t ino, uint8_t ftype,
                      const char *name, uint8_t nlen) {
    struct collect_ctx *ctx = (struct collect_ctx *)user;

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
    } else if (ftype == EXT4_FT_SYMLINK ||
               (child.i_mode & EXT4_S_IFMT) == EXT4_S_IFLNK) {
        e->type = VFS_TYPE_SYMLINK;
        e->size = (size_t)(((uint64_t)child.i_size_hi << 32) |
                           child.i_size_lo);
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

static int ext2_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    struct ext2 *fs = (struct ext2 *)mnt;
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

    struct ext2_diriter *it = kcalloc(1, sizeof(*it));
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

static int ext2_closedir(struct vfs_dir *d) {
    if (d && d->priv) {
        struct ext2_diriter *it = (struct ext2_diriter *)d->priv;
        if (it->ents) kfree(it->ents);
        kfree(it);
        d->priv = NULL;
    }
    return VFS_OK;
}

static int ext2_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    struct ext2_diriter *it = (struct ext2_diriter *)d->priv;
    if (!it || d->index >= it->count) return VFS_ERR_NOENT;
    *out = it->ents[d->index++];
    return VFS_OK;
}

/* ================================================================
 * truncate / ftruncate / utimes.
 *
 * ext2 had chmod and chown but none of these three, and a NULL vfs op
 * becomes VFS_ERR_ROFS -- so `truncate`, `>` redirection onto an existing
 * file and `touch -d` all reported a read-only filesystem on a volume
 * that was writable. free_inode_blocks() above frees EVERY block; these
 * free only the tail, which is the part unlink never has to think about.
 * ================================================================ */

static uint64_t ext2_now_secs(void) {
    extern uint64_t lx_realtime_ns(uint64_t mono_ns);
    extern uint64_t perf_now_ns(void);
    return lx_realtime_ns(perf_now_ns()) / 1000000000ull;
}

/* Walk one indirect table, freeing the pointers whose logical block is at
 * or past `from`. `*out_still` receives how many pointers survive, so the
 * caller can drop the table itself when it empties. */
static int trim_one_indirect(struct ext2 *fs, uint32_t table_blk,
                             uint32_t base_lblk, uint32_t from,
                             uint32_t *out_still) {
    uint32_t per_block = fs->block_size / 4;
    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf) return VFS_ERR_NOMEM;
    int rc = read_block(fs, table_blk, buf);
    if (rc != VFS_OK) { kfree(buf); return rc; }

    uint32_t *t = (uint32_t *)buf;
    uint32_t still = 0;
    bool dirty = false;
    for (uint32_t i = 0; i < per_block; i++) {
        if (!t[i]) continue;
        if (base_lblk + i >= from) {
            free_block(fs, t[i]);
            t[i]  = 0;
            dirty = true;
        } else {
            still++;
        }
    }
    /* Only worth writing back if the table survives; if it does not, the
     * caller is about to free it. */
    if (dirty && still) rc = write_block(fs, table_blk, buf);
    kfree(buf);
    *out_still = still;
    return rc;
}

/* Free every block from logical block `from` onward, keeping the rest.
 * free_inode_blocks() is the `from == 0` case of this. */
static int trim_inode_blocks(struct ext2 *fs, struct ext4_inode *in,
                             uint32_t from) {
    uint32_t per_block = fs->block_size / 4;
    uint64_t kept = 0;

    for (uint32_t i = 0; i < 12; i++) {
        if (!in->i_block[i]) continue;
        if (i >= from) {
            free_block(fs, in->i_block[i]);
            in->i_block[i] = 0;
        } else {
            kept++;
        }
    }

    if (in->i_block[12]) {                    /* single indirect */
        uint32_t still = 0;
        int rc = trim_one_indirect(fs, in->i_block[12], 12, from, &still);
        if (rc != VFS_OK) return rc;
        if (still == 0) {
            free_block(fs, in->i_block[12]);
            in->i_block[12] = 0;
        } else {
            kept += still + 1;                /* +1: the table itself */
        }
    }

    if (in->i_block[13]) {                    /* double indirect */
        uint32_t base = 12 + per_block;
        uint8_t *l1buf = kmalloc(fs->block_size);
        if (!l1buf) return VFS_ERR_NOMEM;
        int rc = read_block(fs, in->i_block[13], l1buf);
        if (rc != VFS_OK) { kfree(l1buf); return rc; }

        uint32_t *l1 = (uint32_t *)l1buf;
        uint32_t live = 0;
        bool dirty = false;
        for (uint32_t i = 0; i < per_block; i++) {
            if (!l1[i]) continue;
            uint32_t still = 0;
            rc = trim_one_indirect(fs, l1[i], base + i * per_block, from,
                                   &still);
            if (rc != VFS_OK) { kfree(l1buf); return rc; }
            if (still == 0) {
                free_block(fs, l1[i]);
                l1[i] = 0;
                dirty = true;
            } else {
                live++;
                kept += still + 1;
            }
        }
        if (live == 0) {
            free_block(fs, in->i_block[13]);
            in->i_block[13] = 0;
        } else {
            kept += 1;                        /* the l1 table */
            if (dirty) rc = write_block(fs, in->i_block[13], l1buf);
        }
        kfree(l1buf);
        if (rc != VFS_OK) return rc;
    }

    in->i_blocks_lo = (uint32_t)(kept * (fs->block_size / 512));
    return VFS_OK;
}

static int ext2_do_truncate(struct ext2 *fs, uint32_t ino,
                            struct ext4_inode *in, uint64_t len) {
    if ((in->i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return VFS_ERR_ISDIR;
    uint64_t old = ((uint64_t)in->i_size_hi << 32) | in->i_size_lo;

    if (len < old) {
        /* Zero the tail of the last surviving block before freeing the
         * rest: those bytes live inside a block we keep, so growing the
         * file again would otherwise hand back its own old contents
         * instead of the zeroes POSIX promises. */
        uint32_t off = (uint32_t)(len % fs->block_size);
        if (off) {
            uint64_t phys = 0;
            int rc = inode_block_map(fs, in,
                                     (uint32_t)(len / fs->block_size), &phys);
            if (rc != VFS_OK) return rc;
            if (phys) {
                uint8_t *tmp = kmalloc(fs->block_size);
                if (!tmp) return VFS_ERR_NOMEM;
                rc = read_block(fs, phys, tmp);
                if (rc == VFS_OK) {
                    memset(tmp + off, 0, fs->block_size - off);
                    rc = write_block(fs, phys, tmp);
                }
                kfree(tmp);
                if (rc != VFS_OK) return rc;
            }
        }
        int rc = trim_inode_blocks(
            fs, in, (uint32_t)((len + fs->block_size - 1) / fs->block_size));
        if (rc != VFS_OK) return rc;
    }
    /* Growing allocates nothing: an unmapped block is a hole, and this
     * driver's read path already returns zeroes for one. */

    in->i_size_lo = (uint32_t)(len & 0xFFFFFFFFu);
    in->i_size_hi = (uint32_t)(len >> 32);
    uint32_t now = (uint32_t)ext2_now_secs();
    in->i_mtime = now;
    in->i_ctime = now;
    return write_inode(fs, ino, in);
}

static int ext2_truncate(void *mnt, const char *path, uint64_t length) {
    struct ext2 *fs = (struct ext2 *)mnt;
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;
    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;
    return ext2_do_truncate(fs, ino, &in, length);
}

static int ext2_ftruncate(struct vfs_file *f, uint64_t length) {
    struct ext2 *fs = (struct ext2 *)f->mnt;
    struct ext2_filepriv *fp = (struct ext2_filepriv *)f->priv;
    if (!fp) return VFS_ERR_INVAL;
    /* Critical here, not merely tidy: trimming with a stale block list
     * would free blocks the file no longer owns. */
    int rc = fp_refresh(fs, fp);
    if (rc != VFS_OK) return rc;
    rc = ext2_do_truncate(fs, fp->inode_no, &fp->in, length);
    if (rc == VFS_OK) {
        fp->file_size = length;
        f->size       = (size_t)length;
    }
    return rc;
}

static int ext2_utimes(void *mnt, const char *path, uint64_t mtime,
                       uint64_t atime) {
    struct ext2 *fs = (struct ext2 *)mnt;
    uint32_t ino; uint8_t ftype;
    int rc = path_to_inode(fs, path, &ino, &ftype);
    if (rc != VFS_OK) return rc;
    struct ext4_inode in;
    rc = read_inode(fs, ino, &in);
    if (rc != VFS_OK) return rc;
    in.i_mtime = (uint32_t)mtime;
    in.i_atime = (uint32_t)atime;
    return write_inode(fs, ino, &in);
}

static int ext2_stat(void *mnt, const char *path, struct vfs_stat *out) {
    struct ext2 *fs = (struct ext2 *)mnt;
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
    } else if ((in.i_mode & EXT4_S_IFMT) == EXT4_S_IFLNK) {
        out->type = VFS_TYPE_SYMLINK;
        out->size = (size_t)(((uint64_t)in.i_size_hi << 32) | in.i_size_lo);
    } else {
        out->type = VFS_TYPE_FILE;
        out->size = (size_t)(((uint64_t)in.i_size_hi << 32) | in.i_size_lo);
    }
    out->uid  = in.i_uid;
    out->gid  = in.i_gid;
    out->mtime = in.i_mtime;
    out->atime = in.i_atime;
    out->nlink = in.i_links_count;
    out->mode = in.i_mode & VFS_MODE_PERMS;
    if (in.i_mode != 0) out->mode |= VFS_MODE_VALID;
    return VFS_OK;
}

/* ---- unmount ---- */

static int ext2_umount(void *mnt) {
    struct ext2 *fs = (struct ext2 *)mnt;
    if (!fs) return VFS_OK;
    if (fs->blk_buf)  kfree(fs->blk_buf);
    if (fs->blk_buf2) kfree(fs->blk_buf2);
    if (fs->blk_buf3) kfree(fs->blk_buf3);
    if (fs->gdt_buf)  kfree(fs->gdt_buf);
    kfree(fs);
    return VFS_OK;
}

/* Phase H: real numbers from the group descriptors -- the same free
 * counters the allocators maintain, summed. */
static int ext2_statfs(void *mnt, struct vfs_statfs *out) {
    struct ext2 *fs = (struct ext2 *)mnt;
    uint64_t bfree = 0, ifree = 0;
    for (uint32_t g = 0; g < fs->group_count; g++) {
        struct ext4_group_desc_32 *gd =
            (struct ext4_group_desc_32 *)(fs->gdt_buf +
                                          (uint64_t)g * fs->desc_size);
        bfree += gd->bg_free_blocks_count_lo;
        ifree += gd->bg_free_inodes_count_lo;
    }
    out->bsize      = fs->block_size;
    out->blocks     = fs->total_blocks;
    out->bfree      = bfree;
    out->files      = fs->total_inodes;
    out->ffree      = ifree;
    out->type_magic = 0xEF53;               /* EXT4_SUPER_MAGIC */
    out->namelen    = 255;
    return VFS_OK;
}

/* ---- vfs_ops table ---- */

const struct vfs_ops ext2_ops = {
    .open     = ext2_open,
    .close    = ext2_close,
    .read     = ext2_read,
    .write    = ext2_write,
    .create   = ext2_create,
    .unlink   = ext2_unlink,
    .rename   = ext2_rename,   /* Phase H */
    .mkdir    = ext2_mkdir,
    .opendir  = ext2_opendir,
    .closedir = ext2_closedir,
    .readdir  = ext2_readdir,
    .stat     = ext2_stat,
    .chmod     = ext2_chmod,
    .chown     = ext2_chown,
    .utimes    = ext2_utimes,
    .truncate  = ext2_truncate,
    .ftruncate = ext2_ftruncate,
    .link      = ext2_link,
    .umount    = ext2_umount,
    .statfs    = ext2_statfs,   /* Phase H */
};

struct blk_dev *ext2_blkdev_of(void *mnt) {
    if (!mnt) return NULL;
    return ((struct ext2 *)mnt)->dev;
}

/* ---- probe + mount ---- */

int ext2_probe(struct blk_dev *dev) {
    if (!dev) return 0;
    uint8_t buf[1024];
    if (blk_read(dev, 2, 2, buf) != 0) return 0;
    const struct ext4_super_block *sb = (const struct ext4_super_block *)buf;
    if (sb->s_magic != EXT4_SUPER_MAGIC) return 0;
    if (sb->s_log_block_size > 6) return 0;

    uint32_t bs = 1024u << sb->s_log_block_size;
    if (bs != 1024 && bs != 2048 && bs != 4096) return 0;

    /* Refuse ext4-only features for this driver. */
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) return 0;
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) return 0;
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA) return 0;
    return 1;
}

int ext2_mount(const char *mount_point, struct blk_dev *dev) {
    if (!mount_point || !dev) return VFS_ERR_INVAL;

    uint8_t sb_buf[1024];
    if (blk_read(dev, 2, 2, sb_buf) != 0) {
        kprintf("[ext2] superblock read failed\n");
        return VFS_ERR_IO;
    }
    const struct ext4_super_block *sb =
        (const struct ext4_super_block *)sb_buf;
    if (sb->s_magic != EXT4_SUPER_MAGIC) {
        kprintf("[ext2] bad magic 0x%x (want 0xEF53)\n",
                (unsigned)sb->s_magic);
        return VFS_ERR_INVAL;
    }
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) {
        kprintf("[ext2] refuse: NEEDS_RECOVERY set\n");
        return VFS_ERR_INVAL;
    }
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) {
        kprintf("[ext2] refuse: EXTENTS set (use ext4 driver)\n");
        return VFS_ERR_INVAL;
    }

    struct ext2 *fs = kcalloc(1, sizeof(*fs));
    if (!fs) return VFS_ERR_NOMEM;
    fs->dev = dev;
    fs->block_size       = 1024u << sb->s_log_block_size;
    fs->sectors_per_block= fs->block_size / BLK_SECTOR_SIZE;
    fs->total_blocks     = sb->s_blocks_count_lo;
    fs->total_inodes     = sb->s_inodes_count;
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->first_ino        = (sb->s_rev_level >= 1) ? sb->s_first_ino : 11;
    fs->inode_size       = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;
    fs->desc_size        = sb->s_desc_size;
    if (fs->desc_size < 32) fs->desc_size = 32;
    fs->first_data_block = (fs->block_size == 1024) ? 1 : 0;

    if (fs->inodes_per_group == 0 || fs->blocks_per_group == 0 ||
        fs->inode_size < 128 || fs->total_blocks == 0) {
        kprintf("[ext2] superblock fields out of range\n");
        kfree(fs);
        return VFS_ERR_INVAL;
    }

    fs->group_count = (uint32_t)((fs->total_blocks + fs->blocks_per_group - 1) /
                                  fs->blocks_per_group);
    fs->gdt_bytes = fs->group_count * fs->desc_size;
    uint32_t gdt_blocks = (fs->gdt_bytes + fs->block_size - 1) / fs->block_size;

    fs->blk_buf  = kmalloc(fs->block_size);
    fs->blk_buf2 = kmalloc(fs->block_size);
    fs->blk_buf3 = kmalloc(fs->block_size);
    fs->gdt_buf  = kmalloc(gdt_blocks * fs->block_size);
    if (!fs->blk_buf || !fs->blk_buf2 || !fs->blk_buf3 || !fs->gdt_buf) {
        if (fs->blk_buf)  kfree(fs->blk_buf);
        if (fs->blk_buf2) kfree(fs->blk_buf2);
        if (fs->blk_buf3) kfree(fs->blk_buf3);
        if (fs->gdt_buf)  kfree(fs->gdt_buf);
        kfree(fs);
        return VFS_ERR_NOMEM;
    }

    uint32_t gdt_first_blk = fs->first_data_block + 1;
    for (uint32_t i = 0; i < gdt_blocks; i++) {
        int rc = read_block(fs, gdt_first_blk + i,
                            fs->gdt_buf + i * fs->block_size);
        if (rc != VFS_OK) {
            kprintf("[ext2] GDT read failed at block %u\n", gdt_first_blk + i);
            kfree(fs->blk_buf);
            kfree(fs->blk_buf2);
            kfree(fs->blk_buf3);
            kfree(fs->gdt_buf);
            kfree(fs);
            return VFS_ERR_IO;
        }
    }

    /* Verify root inode. */
    struct ext4_inode root;
    int rc = read_inode(fs, EXT4_ROOT_INODE, &root);
    if (rc != VFS_OK || (root.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
        kprintf("[ext2] root inode (#2) is not a directory (mode=0x%x rc=%d)\n",
                (unsigned)root.i_mode, rc);
        kfree(fs->blk_buf);
        kfree(fs->blk_buf2);
        kfree(fs->blk_buf3);
        kfree(fs->gdt_buf);
        kfree(fs);
        return VFS_ERR_INVAL;
    }

    rc = vfs_mount(mount_point, &ext2_ops, fs);
    if (rc != VFS_OK) {
        kprintf("[ext2] vfs_mount('%s') failed: %d\n", mount_point, rc);
        kfree(fs->blk_buf);
        kfree(fs->blk_buf2);
        kfree(fs->blk_buf3);
        kfree(fs->gdt_buf);
        kfree(fs);
        return rc;
    }

    kprintf("[ext2] mounted '%s' on %s: %u blocks x %u B "
            "(%u KiB total, %u groups, inode_size=%u, first_ino=%u, RW)\n",
            mount_point, dev->name ? dev->name : "(anon)",
            (unsigned)fs->total_blocks, fs->block_size,
            (unsigned)((fs->total_blocks * fs->block_size) / 1024u),
            fs->group_count, fs->inode_size, fs->first_ino);
    return VFS_OK;
}
