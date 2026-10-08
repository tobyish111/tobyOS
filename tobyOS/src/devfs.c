/* devfs.c -- /dev as a directory tree, answered by the VFS.
 *
 * WHAT WAS WRONG
 * --------------
 * /dev was never a directory. Its nodes are made by sys_open (the device
 * FILES -- null, the console, a pty slave -- have no VFS backing), and a
 * table in syscall.c taught the stat syscalls about the plainest of them.
 * Everything that asks the VFS instead saw nothing:
 *
 *     [ -d /dev ]        false          (bash, /bin/tsh, the kernel shell)
 *     cd /dev            "No such file or directory"
 *     ls /dev            "cannot access '/dev'"   -- ls stats first
 *     stat /dev/console  ENOENT, while open() of it worked
 *
 * and the getdents64 listing of /dev -- which did exist -- named entries
 * (dri, snd) that do not exist on a machine without those devices, and
 * nodes (console, fb0, input/...) that no stat would confirm.
 *
 * WHAT THIS IS
 * ------------
 * A small read-only filesystem mounted at /dev whose ONE table below says
 * what exists: every directory, every node, and the condition under which
 * each is present. The VFS then gives the same answer to every caller --
 * stat, chdir, access, both readdir syscalls, the in-kernel shell's own
 * test / cd / ls / globbing -- and syscall.c's dev_synth_mode() asks this
 * table too (devfs_node_mode), so the Linux stat arms, the listing and the
 * open path cannot drift apart again.
 *
 * Opening a NODE is still sys_open's job: it makes the device file. Opening
 * one through the VFS here (vfs_open, which has no device to hand back) is
 * refused with VFS_ERR_PERM -- "exists, cannot be read as a plain file",
 * which is what execve reports as EACCES. /dev/shm is a real tmpfs mount;
 * longest-prefix resolution sends everything under it there, and this
 * table only lists its name.
 */

#include <tobyos/types.h>
#include <tobyos/vfs.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pty.h>
#include <tobyos/linux_drm.h>
#include <tobyos/snd_pcm.h>
#include <tobyos/devfs.h>

#define DEV_S_IFDIR 0x4000u
#define DEV_S_IFCHR 0x2000u
#define DEV_S_IFLNK 0xA000u

enum dev_avail { AV_ALWAYS = 0, AV_DRM, AV_SND };

struct devfs_ent {
    const char *path;       /* relative to /dev, "" is /dev itself */
    uint32_t    mode;       /* S_IFDIR or S_IFCHR, plus permission bits */
    uint16_t    maj, min;   /* for a node: the Linux major/minor */
    uint8_t     avail;
    const char *target;     /* for a link: what it names */
};

/* The Linux values (major/minor and permissions) as on a stock system, so a
 * program comparing st_rdev against its expectations finds what it expects.
 * /proc/devices lists the same majors. */
static const struct devfs_ent g_devfs[] = {
    { "",               DEV_S_IFDIR | 0755,   0,   0, AV_ALWAYS, 0 },
    { "null",           DEV_S_IFCHR | 0666,   1,   3, AV_ALWAYS, 0 },
    { "zero",           DEV_S_IFCHR | 0666,   1,   5, AV_ALWAYS, 0 },
    { "full",           DEV_S_IFCHR | 0666,   1,   7, AV_ALWAYS, 0 },
    { "random",         DEV_S_IFCHR | 0666,   1,   8, AV_ALWAYS, 0 },
    { "urandom",        DEV_S_IFCHR | 0666,   1,   9, AV_ALWAYS, 0 },
    { "tty",            DEV_S_IFCHR | 0666,   5,   0, AV_ALWAYS, 0 },
    { "console",        DEV_S_IFCHR | 0600,   5,   1, AV_ALWAYS, 0 },
    { "ptmx",           DEV_S_IFCHR | 0666,   5,   2, AV_ALWAYS, 0 },
    { "fb0",            DEV_S_IFCHR | 0660,  29,   0, AV_ALWAYS, 0 },
    /* Linux's links to the caller's own descriptors. Opening one is
     * sys_open's (it dups the descriptor, 959b5346); these make them stat,
     * readlink and list as the links they are -- they did not exist to
     * anything but open (finding 70). */
    { "stdin",          DEV_S_IFLNK | 0777,   0,   0, AV_ALWAYS, "/proc/self/fd/0" },
    { "stdout",         DEV_S_IFLNK | 0777,   0,   0, AV_ALWAYS, "/proc/self/fd/1" },
    { "stderr",         DEV_S_IFLNK | 0777,   0,   0, AV_ALWAYS, "/proc/self/fd/2" },
    { "fd",             DEV_S_IFLNK | 0777,   0,   0, AV_ALWAYS, "/proc/self/fd" },
    { "pts",            DEV_S_IFDIR | 0755,   0,   0, AV_ALWAYS, 0 },
    { "input",          DEV_S_IFDIR | 0755,   0,   0, AV_ALWAYS, 0 },
    { "input/event0",   DEV_S_IFCHR | 0660,  13,  64, AV_ALWAYS, 0 },
    { "input/event1",   DEV_S_IFCHR | 0660,  13,  65, AV_ALWAYS, 0 },
    { "dri",            DEV_S_IFDIR | 0755,   0,   0, AV_DRM, 0 },
    { "dri/card0",      DEV_S_IFCHR | 0666, 226,   0, AV_DRM, 0 },
    { "dri/renderD128", DEV_S_IFCHR | 0666, 226, 128, AV_DRM, 0 },
    { "snd",            DEV_S_IFDIR | 0755,   0,   0, AV_SND, 0 },
    { "snd/controlC0",  DEV_S_IFCHR | 0666, 116,   0, AV_SND, 0 },
    { "snd/pcmC0D0p",   DEV_S_IFCHR | 0666, 116,  16, AV_SND, 0 },
    /* A real tmpfs mount (kernel.c): listed here, answered there. */
    { "shm",            DEV_S_IFDIR | 01777,  0,   0, AV_ALWAYS, 0 },
};
#define DEVFS_N (sizeof g_devfs / sizeof g_devfs[0])

static bool devfs_present(const struct devfs_ent *e) {
    switch (e->avail) {
    case AV_DRM: return lxdrm_available();
    case AV_SND: return lxsnd_available();
    default:     return true;
    }
}

/* "/pts/3" style relative path -> strip the leading slashes. */
static const char *devfs_rel(const char *path) {
    while (path && *path == '/') path++;
    return path ? path : "";
}

/* /dev/tty<N>, the virtual terminals: tty0..tty63, as on Linux (major 4).
 * sys_open answers every one with the kernel console; they were openable and
 * yet in no listing and no stat -- the asymmetry this table exists to end.
 * "tty" alone is the controlling terminal, a table entry of its own. No
 * leading zeros: /dev/tty01 does not exist on Linux either. */
#define DEVFS_VT_MAX 63
static bool devfs_vt_index(const char *rel, int *idx) {
    if (strncmp(rel, "tty", 3) != 0) return false;
    const char *q = rel + 3;
    if (!*q || (q[0] == '0' && q[1])) return false;
    int n = 0;
    for (; *q; q++) {
        if (*q < '0' || *q > '9') return false;
        n = n * 10 + (*q - '0');
        if (n > DEVFS_VT_MAX) return false;
    }
    *idx = n;
    return true;
}

/* A pty slave node, /dev/pts/<N>: present while its pair is live. */
static bool devfs_pts_index(const char *rel, int *idx) {
    if (strncmp(rel, "pts/", 4) != 0 || !rel[4]) return false;
    int n = 0;
    for (const char *q = rel + 4; *q; q++) {
        if (*q < '0' || *q > '9') return false;
        n = n * 10 + (*q - '0');
        if (n > 9999) return false;
    }
    *idx = n;
    return true;
}

static const struct devfs_ent *devfs_find(const char *rel) {
    for (unsigned i = 0; i < DEVFS_N; i++)
        if (strcmp(g_devfs[i].path, rel) == 0)
            return devfs_present(&g_devfs[i]) ? &g_devfs[i] : 0;
    return 0;
}

/* Mode, major and minor of a path under /dev ("null", "pts/2", "" for /dev
 * itself); false if nothing is there. */
static bool devfs_lookup(const char *rel, uint32_t *mode, uint32_t *maj,
                         uint32_t *min) {
    const struct devfs_ent *e = devfs_find(rel);
    if (e) {
        *mode = e->mode; *maj = e->maj; *min = e->min;
        return true;
    }
    int idx;
    if (devfs_pts_index(rel, &idx) && pty_slave_live(idx)) {
        *mode = DEV_S_IFCHR | 0620; *maj = 136; *min = (uint32_t)idx;
        return true;
    }
    if (devfs_vt_index(rel, &idx)) {
        *mode = DEV_S_IFCHR | 0620; *maj = 4; *min = (uint32_t)idx;
        return true;
    }
    return false;
}

/* ---- the answer syscall.c asks (dev_synth_mode) ----------------------- */

uint32_t devfs_node_mode(const char *kpath, uint32_t *maj, uint32_t *min) {
    if (!kpath || strncmp(kpath, "/dev/", 5) != 0) return 0;
    uint32_t mode, a, b;
    if (!devfs_lookup(kpath + 5, &mode, &a, &b)) return 0;
    if ((mode & 0xF000u) != DEV_S_IFCHR) return 0;     /* nodes only */
    if (maj) *maj = a;
    if (min) *min = b;
    return mode;
}

/* ---- VFS ops ---------------------------------------------------------- */

static int devfs_open(void *mnt, const char *path, struct vfs_file *out) {
    (void)mnt; (void)out;
    uint32_t mode, a, b;
    if (!devfs_lookup(devfs_rel(path), &mode, &a, &b)) return VFS_ERR_NOENT;
    if ((mode & 0xF000u) == DEV_S_IFDIR) return VFS_ERR_ISDIR;
    return VFS_ERR_PERM;         /* a device: sys_open makes those, not the VFS */
}

static int devfs_close(struct vfs_file *f) { (void)f; return VFS_OK; }

static int devfs_stat(void *mnt, const char *path, struct vfs_stat *out) {
    (void)mnt;
    uint32_t mode, a, b;
    if (!devfs_lookup(devfs_rel(path), &mode, &a, &b)) return VFS_ERR_NOENT;
    memset(out, 0, sizeof *out);
    out->type = ((mode & 0xF000u) == DEV_S_IFDIR) ? VFS_TYPE_DIR :
                ((mode & 0xF000u) == DEV_S_IFLNK) ? VFS_TYPE_SYMLINK :
                                                    VFS_TYPE_FILE;
    out->mode = mode | VFS_MODE_VALID;       /* S_IFCHR stays in the mode */
    if (out->type == VFS_TYPE_SYMLINK) {
        const struct devfs_ent *e = devfs_find(devfs_rel(path));
        out->size = (e && e->target) ? strlen(e->target) : 0;
        out->nlink = 1;
    }
    return VFS_OK;
}

static int devfs_readlink(void *mnt, const char *path, char *buf, size_t bufsz) {
    (void)mnt;
    const struct devfs_ent *e = devfs_find(devfs_rel(path));
    if (!e) return VFS_ERR_NOENT;
    if ((e->mode & 0xF000u) != DEV_S_IFLNK || !e->target) return VFS_ERR_INVAL;
    size_t n = strlen(e->target);
    if (n >= bufsz) n = bufsz - 1;
    memcpy(buf, e->target, n);
    buf[n] = '\0';
    return VFS_OK;
}

/* A directory handle remembers WHICH directory (a pointer into the table,
 * never a caller's buffer) and walks the table, then the pty pairs. */
static int devfs_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    (void)mnt;
    const char *rel = devfs_rel(path);
    const struct devfs_ent *e = devfs_find(rel);
    if (!e) return VFS_ERR_NOENT;
    if ((e->mode & 0xF000u) != DEV_S_IFDIR) return VFS_ERR_NOTDIR;
    out->priv  = (void *)e->path;
    out->index = 0;
    return VFS_OK;
}

static int devfs_closedir(struct vfs_dir *d) { (void)d; return VFS_OK; }

/* Is table entry `e` a DIRECT child of directory `dir`? */
static const char *devfs_child_name(const char *dir, const struct devfs_ent *e) {
    const char *p = e->path;
    if (!p[0]) return 0;                           /* /dev itself */
    size_t dl = strlen(dir);
    if (dl) {
        if (strncmp(p, dir, dl) != 0 || p[dl] != '/') return 0;
        p += dl + 1;
    }
    for (const char *q = p; *q; q++) if (*q == '/') return 0;
    return p;
}

static void devfs_fill(struct vfs_dirent *out, const char *name, uint32_t mode) {
    memset(out, 0, sizeof *out);
    size_t n = strlen(name);
    if (n >= VFS_NAME_MAX) n = VFS_NAME_MAX - 1;
    memcpy(out->name, name, n);
    out->name[n] = '\0';
    out->type = ((mode & 0xF000u) == DEV_S_IFDIR) ? VFS_TYPE_DIR :
                ((mode & 0xF000u) == DEV_S_IFLNK) ? VFS_TYPE_SYMLINK :
                                                    VFS_TYPE_FILE;
    out->mode = mode | VFS_MODE_VALID;
}

static int devfs_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    const char *dir = (const char *)d->priv;
    if (!dir) return VFS_ERR_NOENT;
    size_t idx = 0;
    for (unsigned i = 0; i < DEVFS_N; i++) {
        const struct devfs_ent *e = &g_devfs[i];
        const char *name = devfs_child_name(dir, e);
        if (!name || !devfs_present(e)) continue;
        if (idx++ == d->index) {
            devfs_fill(out, name, e->mode);
            d->index++;
            return VFS_OK;
        }
    }
    if (dir[0] == '\0') {                          /* /dev: tty0..tty63 */
        for (int n = 0; n <= DEVFS_VT_MAX; n++) {
            if (idx++ == d->index) {
                char nb[8];
                ksnprintf(nb, sizeof nb, "tty%d", n);
                devfs_fill(out, nb, DEV_S_IFCHR | 0620);
                d->index++;
                return VFS_OK;
            }
        }
    }
    if (strcmp(dir, "pts") == 0) {
        /* pty_slave_live() answers false past the end of the pair table. */
        for (int n = 0; n < 64; n++) {
            if (!pty_slave_live(n)) continue;
            if (idx++ == d->index) {
                char nb[8];
                ksnprintf(nb, sizeof nb, "%d", n);
                devfs_fill(out, nb, DEV_S_IFCHR | 0620);
                d->index++;
                return VFS_OK;
            }
        }
    }
    return VFS_ERR_NOENT;                           /* end of directory */
}

static const struct vfs_ops devfs_ops = {
    .open     = devfs_open,
    .close    = devfs_close,
    .read     = 0,
    .write    = 0,
    .create   = 0,
    .unlink   = 0,
    .mkdir    = 0,
    .opendir  = devfs_opendir,
    .closedir = devfs_closedir,
    .readdir  = devfs_readdir,
    .stat     = devfs_stat,
    .readlink = devfs_readlink,
    .chmod    = 0,
    .chown    = 0,
    .umount   = 0,
};

int devfs_init(void) {
    int rc = vfs_mount("/dev", &devfs_ops, 0);
    if (rc != VFS_OK)
        kprintf("[devfs] mount /dev failed rc=%d\n", rc);
    else
        kprintf("[devfs] /dev mounted (%u table entries + live pts)\n",
                (unsigned)DEVFS_N);
    return rc;
}
