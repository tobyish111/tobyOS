/* vfs.h -- minimal virtual filesystem layer.
 *
 * The VFS hides every on-disk format behind a small operation table and
 * a longest-prefix mount table. Today there are two mounts:
 *   "/"      -> ramfs    (read-only, USTAR initrd)
 *   "/data"  -> tobyfs   (read-write, ATA-backed disk)
 *
 * Every wrapper resolves a path to (mount, relative_path) and forwards
 * the call. The driver only ever sees the relative path -- e.g. opening
 * "/data/docs/x" calls tobyfs->open with "/docs/x".
 *
 * Operations supported:
 *   open      - look up a path, return a read/write cursor
 *   read      - copy bytes out, advance cursor
 *   write     - copy bytes in, advance cursor (ROFS on ramfs)
 *   close     - release the cursor
 *   create    - create an empty regular file (ROFS on ramfs)
 *   unlink    - delete a regular file (ROFS on ramfs)
 *   mkdir     - create an empty directory (ROFS on ramfs)
 *   opendir   - look up a directory path
 *   readdir   - return one dirent at a time, ENOENT-style sentinel at end
 *   closedir  - release the dir cursor
 *   stat      - size + type without opening
 *
 * Paths are absolute, '/'-separated. We do not resolve "." / ".."
 * entries -- callers are expected to hand us canonical paths.
 */

#ifndef TOBYOS_VFS_H
#define TOBYOS_VFS_H

#include <tobyos/types.h>

#define VFS_NAME_MAX 128
#define VFS_PATH_MAX 256
/* M23D: bumped from 4 -> 8. We can simultaneously hold:
 *   /          ramfs (initrd)
 *   /data      tobyfs (GPT slot 2)
 *   /fat       FAT32  (GPT slot 3)
 *   /ext       ext4   (GPT slot 4, RO)
 *   /usb       FAT32  (USB MSC)
 * plus headroom for `mountfs /mnt ...` calls from the live shell. */
#define VFS_MAX_MOUNTS 8

/* Result codes -- negative on error, 0 on success unless noted. */
#define VFS_OK             0
#define VFS_ERR_NOENT     -1   /* path not found */
#define VFS_ERR_NOTDIR    -2   /* opendir on a regular file */
#define VFS_ERR_ISDIR     -3   /* open on a directory */
#define VFS_ERR_NOMEM     -4   /* heap exhausted */
#define VFS_ERR_INVAL     -5   /* bad arguments */
#define VFS_ERR_NOMOUNT   -6   /* no filesystem mounted at that path */
#define VFS_ERR_IO        -7   /* underlying read/write failed */
#define VFS_ERR_EXIST     -8   /* file/directory already exists */
#define VFS_ERR_ROFS      -9   /* mount is read-only */
#define VFS_ERR_NOSPC     -10  /* filesystem is full */
#define VFS_ERR_NAMETOOLONG -11
#define VFS_ERR_PERM      -12   /* permission denied (milestone 15) */

/* Permission "want" bits, passed to vfs_perm_check(). They are the
 * same shape as the low 3 bits of a Unix mode: 4=read, 2=write,
 * 1=execute. */
#define VFS_WANT_READ   4
#define VFS_WANT_WRITE  2
#define VFS_WANT_EXEC   1

/* Mode bits stored in struct vfs_stat / struct vfs_dirent. We mirror
 * the on-disk tobyfs values so callers can use one set of constants
 * across both filesystems. */
#define VFS_MODE_PERMS   00777u
#define VFS_MODE_VALID   0x10000u

enum vfs_type {
    VFS_TYPE_FILE = 1,
    VFS_TYPE_DIR  = 2
};

struct vfs_stat {
    enum vfs_type type;
    size_t        size;        /* 0 for directories */
    /* Owner identity + permission bits (milestone 15). mode == 0 (no
     * VFS_MODE_VALID bit) means "this fs/inode has no permission
     * info" -- callers and the perm checker treat that as fully
     * accessible. */
    uint32_t      uid;
    uint32_t      gid;
    uint32_t      mode;
};

struct vfs_dirent {
    char          name[VFS_NAME_MAX];   /* leaf name only, no slashes */
    enum vfs_type type;
    size_t        size;
    uint32_t      uid;
    uint32_t      gid;
    uint32_t      mode;
};

/* Forward decls; concrete layout is opaque to callers and lives in
 * vfs.c. The fs driver gets to stash its own per-handle state in the
 * `priv` slot; everything else is bookkeeping the VFS owns. */
struct vfs_file;
struct vfs_dir;

struct vfs_ops {
    int  (*open)    (void *mnt, const char *path, struct vfs_file *out);
    int  (*close)   (struct vfs_file *f);
    long (*read)    (struct vfs_file *f, void *buf, size_t n);
    long (*write)   (struct vfs_file *f, const void *buf, size_t n);
    /* `create` and `mkdir` get the new inode's owner/mode passed in
     * directly so the driver doesn't have to reach into the proc
     * subsystem. The VFS wrappers fill these in from current_proc()
     * and the appropriate default mode. */
    int  (*create)  (void *mnt, const char *path,
                     uint32_t uid, uint32_t gid, uint32_t mode);
    int  (*unlink)  (void *mnt, const char *path);
    int  (*mkdir)   (void *mnt, const char *path,
                     uint32_t uid, uint32_t gid, uint32_t mode);
    int  (*opendir) (void *mnt, const char *path, struct vfs_dir *out);
    int  (*closedir)(struct vfs_dir *d);
    int  (*readdir) (struct vfs_dir *d, struct vfs_dirent *out);
    int  (*stat)    (void *mnt, const char *path, struct vfs_stat *out);
    /* Optional. If NULL, vfs_chmod / vfs_chown return VFS_ERR_ROFS. */
    int  (*chmod)   (void *mnt, const char *path, uint32_t mode);
    int  (*chown)   (void *mnt, const char *path,
                     uint32_t uid, uint32_t gid);
    /* M26E (optional). Called by vfs_unmount AFTER the slot is removed
     * from the mount table. Drivers free their per-mount state here
     * (cluster buffers, FS scratch, etc). NULL => the VFS just drops
     * the entry; the underlying state is leaked. */
    int  (*umount)  (void *mnt);
};

/* Per-handle state. The driver stuffs whatever it likes into `priv`
 * (ramfs uses it for a node pointer + read cursor). The owner +
 * permission cache lets vfs_write enforce W access without having to
 * stat the path again every call. */
struct vfs_file {
    const struct vfs_ops *ops;
    void                 *mnt;
    void                 *priv;
    size_t                pos;
    size_t                size;
    uint32_t              uid;
    uint32_t              gid;
    uint32_t              mode;
    /* Milestone 34E: set by vfs_open if the path matched a sysprot
     * protected prefix. vfs_write consults it so a sandboxed proc
     * can't write through a read-opened handle to a protected file
     * (defence-in-depth: vfs_create is already gated, but a process
     * that successfully opened a pre-existing protected file in
     * read mode would otherwise be able to write through it if it
     * also held CAP_FILE_WRITE). */
    bool                  sysprot;
};

struct vfs_dir {
    const struct vfs_ops *ops;
    void                 *mnt;
    void                 *priv;
    size_t                index;   /* driver-defined; usually next-entry idx */
};

/* ---- mount table ----
 *
 * Mount a driver at an absolute path. mount_point="/" registers the
 * root mount; anything else (e.g. "/data") creates a sub-mount. Path
 * resolution always picks the longest matching mount-point prefix. */
int  vfs_mount(const char *mount_point, const struct vfs_ops *ops, void *mount_data);
bool vfs_is_mounted(void);
void vfs_dump_mounts(void);

/* M26E: tear a mount slot back down. Looks up the mount by its
 * normalised path (same rules as vfs_mount), invokes the driver's
 * optional `umount` hook so it can free its per-mount state, then
 * removes the slot from the mount table. Subsequent path lookups
 * that would have resolved through `mount_point` fall back to the
 * next-longest matching prefix (or VFS_ERR_NOMOUNT if there is none).
 *
 * Returns VFS_OK on success, VFS_ERR_NOMOUNT if no mount lives at
 * `mount_point`, VFS_ERR_INVAL on a malformed path. The driver's
 * umount callback's return value is forwarded if non-zero. */
int  vfs_unmount(const char *mount_point);

/* M26E: walk every entry in the mount table. Stops early if `cb`
 * returns false. The callback gets a NUL-terminated normalised mount
 * path, the driver's vfs_ops vtable pointer (so it can identify which
 * fs is mounted), and the opaque mount-data pointer the driver handed
 * to vfs_mount(). Used by usb_msc_unbind to find any mounts that
 * still reference a yanked block device. */
typedef bool (*vfs_mount_walk_cb)(const char *mount_point,
                                  const struct vfs_ops *ops,
                                  void *mount_data,
                                  void *cookie);
void vfs_iter_mounts(vfs_mount_walk_cb cb, void *cookie);

/* ---- per-call wrappers ---- */

int  vfs_open    (const char *path, struct vfs_file *out);
int  vfs_close   (struct vfs_file *f);
long vfs_read    (struct vfs_file *f, void *buf, size_t n);
long vfs_write   (struct vfs_file *f, const void *buf, size_t n);
int  vfs_create  (const char *path);
int  vfs_unlink  (const char *path);
int  vfs_mkdir   (const char *path);
int  vfs_opendir (const char *path, struct vfs_dir *out);
int  vfs_closedir(struct vfs_dir *d);
int  vfs_readdir (struct vfs_dir *d, struct vfs_dirent *out);
int  vfs_stat    (const char *path, struct vfs_stat *out);

/* Permission management (milestone 15). Both require either uid 0 or
 * the calling process to own the file. Return VFS_OK on success,
 * VFS_ERR_PERM on access denied, VFS_ERR_ROFS if the underlying
 * filesystem doesn't support it (e.g. the read-only ramfs root). */
int  vfs_chmod   (const char *path, uint32_t mode);
int  vfs_chown   (const char *path, uint32_t uid, uint32_t gid);

/* Permission check used internally by the VFS wrappers and by the
 * proc subsystem before exec'ing a binary. `want` is a bitwise OR of
 * VFS_WANT_READ / VFS_WANT_WRITE / VFS_WANT_EXEC. Returns VFS_OK or
 * VFS_ERR_PERM (or a propagated stat error). uid 0 always passes. */
int  vfs_perm_check(const char *path, int want);

/* Convenience: read the entire file into a kmalloc'd buffer. The
 * caller owns *out_buf and must kfree() it. Returns VFS_OK on success;
 * on any error returns the error code AND leaves *out_buf == NULL. */
int  vfs_read_all(const char *path, void **out_buf, size_t *out_size);

/* Convenience: open(create-if-missing), write `n` bytes, close. */
int  vfs_write_all(const char *path, const void *buf, size_t n);

/* Pretty name for an error code (for diagnostics). */
const char *vfs_strerror(int err);

#endif /* TOBYOS_VFS_H */
