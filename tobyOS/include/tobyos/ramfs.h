/* ramfs.h -- read-only USTAR-backed in-memory filesystem.
 *
 * The ramfs driver parses a POSIX USTAR archive once at mount time,
 * builds a flat node table on the kernel heap, and serves vfs_ops
 * directly out of the original tar payload (no copy). Files are read
 * by memcpy'ing from the tar bytes; directories are enumerated by
 * filtering the node table for entries whose parent matches.
 *
 * Limits (compile-time):
 *   - filename length: 100 chars (USTAR `name` field; we ignore the
 *     `prefix` extension since our initrd contents are short)
 *   - max nodes: capped by the heap; we kmalloc one node struct per
 *     tar entry and reject mounts that don't fit.
 *
 * The tar bytes (`image`) must remain valid for the lifetime of the
 * mount. For our use case Limine modules sit in BOOTLOADER_RECLAIMABLE
 * memory which is permanently mapped via HHDM, so this is fine.
 */

#ifndef TOBYOS_RAMFS_H
#define TOBYOS_RAMFS_H

#include <tobyos/types.h>
#include <tobyos/vfs.h>

/* Mount the USTAR archive `image[0..size)` and register it as the root
 * VFS. Returns VFS_OK on success, or a negative error code (the
 * mount slot is left unchanged on failure). */
int ramfs_mount(const void *image, size_t size);

/* Number of entries the ramfs is currently serving (for diagnostics). */
size_t ramfs_node_count(void);

/* Slice 112: zero-copy image borrow for execve.
 *
 * The tar bytes are resident and permanently mapped (see the header
 * comment), and every file's payload is CONTIGUOUS inside them -- so a
 * reader that only needs the bytes for the duration of a syscall can
 * borrow a pointer instead of paying a full-copy vfs_read_all (~320 ms
 * for chrome's 188 MiB image, under the BKL, per exec'd child).
 *
 * `ramfs_file_data` returns the payload pointer/size for an OPEN ramfs
 * file handle (i.e. after vfs_open ran every cap/sandbox/perm gate).
 * Callers identify a ramfs handle by `f->ops == &ramfs_ops`. The
 * returned pointer is read-only and valid for the mount's lifetime;
 * it must NEVER be kfree'd. */
extern const struct vfs_ops ramfs_ops;
int ramfs_file_data(struct vfs_file *f, const void **data, size_t *size);

#endif /* TOBYOS_RAMFS_H */
