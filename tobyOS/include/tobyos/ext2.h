/* ext2.h -- read-write ext2 filesystem driver.
 *
 * Companion to the read-only ext4 driver (ext4.h / ext4.c). This driver
 * targets the simpler ext2 on-disk format (no journal, no extents) and
 * adds full write support: file creation, writes, deletion, mkdir, chmod,
 * chown, and symbolic links.
 *
 * The on-disk structures (superblock, inode, group descriptor, directory
 * entry) are byte-identical between ext2 and ext4. We reuse the packed
 * struct definitions from ext4.h and restrict ourselves to the subset
 * of features that ext2 actually uses:
 *
 *   - Legacy block pointers (12 direct + single/double/triple indirect)
 *   - 1024 / 2048 / 4096 byte block sizes
 *   - 128-byte or 256-byte inodes
 *   - Linear directory entry walk (ext4_dir_entry_2 format)
 *   - Block and inode bitmaps for allocation
 *
 * The driver consumes a generic `struct blk_dev *` and presents itself
 * to the VFS through the standard vfs_ops table, just like tobyfs,
 * FAT32, and ext4.
 */

#ifndef TOBYOS_EXT2_H
#define TOBYOS_EXT2_H

#include <tobyos/types.h>
#include <tobyos/ext4.h>

struct blk_dev;
struct vfs_ops;

/* Quick sniff: read the superblock and check magic + no unsupported
 * INCOMPAT features (extents, journal recover, inline data are all
 * refused). Returns 1 if it looks like a mountable ext2 volume. */
int ext2_probe(struct blk_dev *dev);

/* Mount the ext2 filesystem on `dev` at `mount_point` (e.g. "/ext2").
 * Returns VFS_OK on success, VFS_ERR_* on failure. Unlike the ext4
 * driver, this mount is read-write. */
int ext2_mount(const char *mount_point, struct blk_dev *dev);

/* Map an opaque vfs mount-data pointer back to the underlying blk_dev.
 * Returns NULL if `mnt` is NULL. */
struct blk_dev *ext2_blkdev_of(void *mnt);

/* Address-of-vtable identity. Compare a mount's vfs_ops* against this
 * before calling ext2_blkdev_of(). */
extern const struct vfs_ops ext2_ops;

#endif /* TOBYOS_EXT2_H */
