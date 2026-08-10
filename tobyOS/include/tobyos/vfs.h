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
 * plus headroom for `mountfs /mnt ...` calls from the live shell.
 *
 * Slice 9: 8 -> 16. The table is now PER MOUNT NAMESPACE, and a container that
 * pivot_roots then mounts its own /proc, /sys, /dev, /dev/pts and /tmp needs
 * five slots of its own on top of the three the boot namespace already uses
 * (/, /proc, /data). At 8 that wall would have been hit by slice 16's container
 * runtime rather than by anything diagnosable. Costs ~288 bytes per extra slot
 * per namespace. */
#define VFS_MAX_MOUNTS 16

/* Slice 9: mount PROPAGATION type, per mount (Linux MS_PRIVATE/SHARED/SLAVE/
 * UNBINDABLE). PRIVATE is 0 so it stays the default for every mount that
 * existed before this slice, which is also Linux's default for a fresh mount. */
#define VFS_PROP_PRIVATE     0u
#define VFS_PROP_SHARED      1u
#define VFS_PROP_SLAVE       2u
#define VFS_PROP_UNBINDABLE  3u

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
#define VFS_ERR_PERM      -12   /* permission denied     -> EACCES */
/* Slice 11: "operation not permitted" (EPERM), which is NOT the same answer as
 * EACCES and cannot be folded into VFS_ERR_PERM.
 *
 * Found because /proc/PID/uid_map write refusals -- which Linux reports as
 * EPERM, and which container runtimes test for -- came out of the VFS as EACCES:
 * there was simply no code in this vocabulary that mapped to EPERM. That is the
 * same class of bug slice 6 recorded in the other direction (a raw VFS_ERR_*
 * escaping as an unrelated Linux errno); an error vocabulary that cannot express
 * the answer will silently substitute a plausible one. */
#define VFS_ERR_NOTPERM   -14   /* operation not permitted -> EPERM  */

/* Permission "want" bits, passed to vfs_perm_check(). They are the
 * same shape as the low 3 bits of a Unix mode: 4=read, 2=write,
 * 1=execute. */
#define VFS_WANT_READ   4
#define VFS_WANT_WRITE  2
#define VFS_WANT_EXEC   1

/* Mode bits stored in struct vfs_stat / struct vfs_dirent. We mirror
 * the on-disk tobyfs values so callers can use one set of constants
 * across both filesystems. */
/* Linux slice 2: widened 00777 -> 07777 so the setuid/setgid/sticky bits
 * survive the VFS. They were previously masked off at every filesystem
 * boundary, which meant execve could not see S_ISUID and a setuid binary was
 * indistinguishable from an ordinary one. ext2/ext4 already stored the full
 * i_mode -- only this mask was truncating it. Backward compatible: inodes
 * written before this simply read the new bits as 0. */
#define VFS_MODE_PERMS   07777u
#define VFS_MODE_SETUID  04000u
#define VFS_MODE_SETGID  02000u
#define VFS_MODE_STICKY  01000u
#define VFS_MODE_VALID   0x10000u

enum vfs_type {
    VFS_TYPE_FILE    = 1,
    VFS_TYPE_DIR     = 2,
    VFS_TYPE_SYMLINK = 3
};

#define VFS_SYMLINK_MAX   8   /* max symlink hops before ELOOP */
#define VFS_ERR_LOOP      -13 /* too many symlink hops */

struct vfs_stat {
    enum vfs_type type;
    /* Hard-link count. 0 = "unset", so stat emitters substitute the usual
     * default (1 for files, 2 for directories). Only filesystems with a real
     * answer set it -- procfs reports 2 + one-per-thread for /proc/<pid>/task/,
     * because chrome's sandbox asserts mono-threadedness by stat'ing that
     * directory and testing st_nlink == 3 (".", "..", one thread). See
     * sandbox/linux/services/thread_helpers.cc:41. */
    uint32_t      nlink;
    size_t        size;        /* 0 for directories */
    /* Owner identity + permission bits (milestone 15). mode == 0 (no
     * VFS_MODE_VALID bit) means "this fs/inode has no permission
     * info" -- callers and the perm checker treat that as fully
     * accessible. */
    uint32_t      uid;
    uint32_t      gid;
    uint32_t      mode;
    /* Linux slice 6: timestamps, in UNIX EPOCH SECONDS.
     *
     * Slice 1 shipped utimensat as accept-and-do-nothing because there was
     * nowhere to put a time: struct vfs_stat had no time fields at all, so
     * every file reported 1970 and `ls -l`, tar, cp -p and apk all saw the
     * same fixed date.
     *
     * EPOCH SECONDS, not tick counts: tobyfs stored `pit_ticks()` in its
     * on-disk mtime, which is time since BOOT and resets every reboot -- a
     * value that looks like a timestamp and is not one. Filesystems that have
     * no times leave these 0, which stat emitters report as the epoch. */
    uint64_t      mtime;       /* last data modification */
    uint64_t      atime;       /* last access (best effort) */
    uint64_t      ctime;       /* last inode change */
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
    /* Optional. Atomically re-link `oldpath` -> `newpath` within this mount,
     * replacing an existing `newpath`. If NULL, vfs_rename returns VFS_ERR_ROFS.
     * leveldb/SQLite (chrome's profile stores) depend on atomic rename. */
    int  (*rename)  (void *mnt, const char *oldpath, const char *newpath);
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
    /* Linux slice 6: set timestamps (epoch seconds). OPTIONAL -- a NULL entry
     * means the filesystem cannot store them, and vfs_utimes() reports
     * VFS_ERR_ROFS so utimensat can answer honestly instead of claiming a
     * success it did not achieve. */
    int  (*utimes)  (void *mnt, const char *path,
                     uint64_t mtime, uint64_t atime);
    /* Linux slice 6: set a file's length. OPTIONAL; NULL => VFS_ERR_ROFS.
     * Slice 1's truncate(2) could only validate its arguments because there
     * was no way to reach the on-disk size from a path. */
    int  (*truncate)(void *mnt, const char *path, uint64_t length);
    /* Linux slice 6: truncate an OPEN file. Needed separately from ->truncate
     * because busybox's `truncate` (and CPython's os.ftruncate) act on an fd,
     * not a path -- and the open handle is the only thing that knows which
     * inode it refers to after the name may have changed. */
    int  (*ftruncate)(struct vfs_file *f, uint64_t length);
    /* B20 (optional). Resolve a symlink that the driver synthesises
     * dynamically (e.g. procfs /proc/self, /proc/<pid>/exe), which the
     * static in-kernel symlink table can't represent. Gets the path
     * relative to the mount; writes up to bufsz-1 bytes + NUL into buf
     * and returns VFS_OK, or VFS_ERR_NOENT/INVAL. NULL => the driver has
     * no synthetic symlinks. */
    int  (*readlink)(void *mnt, const char *path, char *buf, size_t bufsz);
    /* Linux slice 7: create a PERSISTENT symlink on this filesystem. OPTIONAL;
     * when NULL, vfs_symlink() falls back to the legacy in-RAM table (which is
     * global, capped, and lost on reboot -- fine for the read-only initrd,
     * useless for a real rootfs). */
    int  (*symlink) (void *mnt, const char *path, const char *target);
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
    /* Stable per-file inode number, assigned by the fs driver's open hook
     * (0 = unknown -> fstat falls back to a per-handle synthetic inode). Two
     * opens of the SAME file must report the same value: Chromium's shm
     * PlatformSharedMemoryRegion::Create opens a temp file O_RDWR then O_RDONLY
     * by the same path and CHECKs both fds' fstat st_ino match ("Writable and
     * read-only inodes don't match; bailing"). Only fs with real inode numbers
     * (tobyfs) set this; ramfs et al. leave 0 and keep the node-pointer hash
     * (which is already stable per file there, so ld.so dedup is unaffected). */
    uint64_t              ino;
    /* Slice 23: which INCARNATION of `ino` this handle refers to. An inode
     * NUMBER is only an identity while the file is linked -- tobyfs reissues
     * the lowest free number immediately, so a number alone cannot tell two
     * successive files apart. The fs driver bumps a per-number counter in
     * alloc_inode and stamps it here at open, making (ino, ino_gen) a durable
     * identity for the file's lifetime. MAP_SHARED keys its page cache on the
     * pair: two opens of the same live file share (chrome opens each shm region
     * O_RDWR then O_RDONLY), while a later file that inherits the number does
     * NOT. 0 = fs without inode identity (ramfs et al.). */
    uint64_t              ino_gen;
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

/* ---- Per-mount security flags (Linux slice 2) -----------------------------
 *
 * Added BEFORE mount(2) exists (that is slice 5) and deliberately so. Slice 2
 * made execve honour the set-user-ID bit, which is safe only while root is the
 * only thing that can mount a filesystem. The moment userspace can mount, an
 * attacker mounts an image containing a root-owned setuid shell and is done.
 *
 * Landing the flag plumbing now means slice 5's mount(2) only has to pass the
 * user's flags through, rather than having to remember to invent this -- the
 * enforcement point in sys_execve already reads them. */
#define VFS_MNT_NOSUID   0x1u   /* ignore setuid/setgid bits on this mount */
#define VFS_MNT_NODEV    0x2u   /* ignore device nodes                     */
#define VFS_MNT_NOEXEC   0x4u   /* refuse to execute anything here         */
#define VFS_MNT_RDONLY   0x8u   /* refuse writes                           */

/* As vfs_mount(), but records mount flags. vfs_mount() is exactly this with
 * flags == 0, so all 13 existing callers keep their current behaviour. */
int  vfs_mount_flags(const char *mount_point, const struct vfs_ops *ops,
                     void *mount_data, uint32_t flags);

/* Look up the mount registered at EXACTLY `mount_point` (not the mount a path
 * resolves through -- that is vfs_path_mount_flags). Used by mount(2) to
 * re-register a mount with the caller's flags, and by MS_BIND to copy an
 * existing mount's ops/data to a second point. VFS_OK, else VFS_ERR_NOMOUNT. */
/* Linux slice 6: set a path's timestamps (epoch seconds). VFS_ERR_ROFS if the
 * filesystem has no ->utimes, so callers can tell "stored" from "ignored". */
int  vfs_utimes(const char *path, uint64_t mtime, uint64_t atime);

/* Linux slice 6: set a path's length. VFS_ERR_ROFS if unsupported. */
int  vfs_truncate(const char *path, uint64_t length);

/* Linux slice 6: truncate via an OPEN file handle. Updates f->size on success
 * so a subsequent fstat sees the new length. */
int  vfs_file_truncate(struct vfs_file *f, uint64_t length);

int  vfs_mount_lookup(const char *mount_point,
                      const struct vfs_ops **ops_out, void **data_out);

/* Flags of the mount that `path` resolves through (0 if unmounted/unknown).
 * The enforcement points -- execve's setuid transition today, open/write and
 * mknod later -- ask this rather than tracking mounts themselves. */
uint32_t vfs_path_mount_flags(const char *path);
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

/* ---- Slice 9: mount namespaces -------------------------------------------
 * `struct mount_ns` stays private to vfs.c (the payload's owner keeps its own
 * namespace struct -- same split as slice 8's uts/ipc). nsproxy.c drives the
 * lifecycle through these; NULL always means the initial namespace.
 *
 * mount_ns_create() CLONES the caller's mount table, and preserves peer groups
 * for SHARED mounts so propagation survives the unshare -- which is what makes
 * MS_SHARED mean anything at all. */
void    *mount_ns_create(void);
void     mount_ns_get(void *ns);
void     mount_ns_put(void *ns);
uint64_t mount_ns_inum(void *ns);

/* mount --make-{shared,private,slave,unbindable} [-R]; `prop` is VFS_PROP_*. */
int  vfs_set_propagation(const char *mount_point, uint32_t prop, bool recursive);
bool vfs_mount_is_unbindable(const char *mount_point);

/* pivot_root(2). Requires a non-initial mount namespace -- see vfs.c. */
int  vfs_pivot_root(const char *new_root, const char *put_old);

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
int  vfs_rename  (const char *oldpath, const char *newpath);
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

/* Slice 112: zero-copy variant. For a ramfs (initrd) file, *out_buf is a
 * BORROWED read-only pointer into the resident tar (*out_borrowed = 1;
 * do NOT kfree). Otherwise identical to vfs_read_all (*out_borrowed = 0,
 * caller owns and kfrees). */
int  vfs_read_all_ref(const char *path, void **out_buf, size_t *out_size,
                      int *out_borrowed);

/* Convenience: open(create-if-missing), write `n` bytes, close. */
int  vfs_write_all(const char *path, const void *buf, size_t n);

/* ---- symlink support (M1.5) ---- */

int  vfs_symlink (const char *path, const char *target);
int  vfs_readlink(const char *path, char *buf, size_t bufsz);
/* Slice 86: mkdir honouring the caller's mode. vfs_mkdir() keeps the old
 * 0755 default so existing in-kernel callers are unchanged. */
int  vfs_mkdir_mode(const char *path, uint32_t mode);
int  vfs_resolve_path(const char *path, char *resolved, size_t resolved_sz);

/* Resolve `path` through any SYMLINKS (including dynamically-synthesised ones
 * like procfs's /proc/self/exe, which vfs_resolve_path's static table cannot
 * see) and write the final target to `out`. Bounded hop count. Returns VFS_OK,
 * or the stat/readlink error. A non-symlink path is copied through unchanged. */
int  vfs_follow_link(const char *path, char *out, size_t outsz);

/* Pretty name for an error code (for diagnostics). */
const char *vfs_strerror(int err);

#endif /* TOBYOS_VFS_H */
