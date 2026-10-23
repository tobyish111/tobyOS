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
#include <tobyos/blk.h>

#define DEV_S_IFDIR 0x4000u
#define DEV_S_IFCHR 0x2000u
#define DEV_S_IFBLK 0x6000u
#define DEV_S_IFLNK 0xA000u
#define DEV_GID_DISK 6u             /* the disk group, as on Linux */

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
    /* The RTC (finding 157): root's, as on a stock system, and the
     * /dev/rtc link udev makes. 249 is the major Linux gave it in WSL. */
    { "rtc0",           DEV_S_IFCHR | 0600, 249,   0, AV_ALWAYS, 0 },
    { "rtc",            DEV_S_IFLNK | 0777,   0,   0, AV_ALWAYS, "rtc0" },
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

/* THE DISKS AND THEIR PARTITIONS, under Linux's names (finding 198). /dev
 * had none: /proc/mounts named a disk filesystem's source by the driver's
 * own name ("ide0:master.p1"), and nothing that wants a device path --
 * mount by device, lsblk, dd -- had one. An ATA, AHCI or USB disk is
 * sd<letter> in the order the disks registered, a virtio disk vd<letter>,
 * an NVMe namespace nvme<H>n<N>; a partition is its disk's name and its
 * number (nvme0n1p1 for NVMe). The majors and minors are Linux's: sd 8
 * and vd 253, 16 minors a disk; NVMe 259, in the order met. The nodes are
 * root's and the disk group's, 0660. */
enum { DB_SD, DB_VD, DB_NVME };

static int devfs_blk_kind(const struct blk_dev *d) {
    const struct blk_dev *disk = d->parent ? d->parent : d;
    const char *n = disk->name ? disk->name : "";
    if (strncmp(n, "nvme", 4) == 0) return DB_NVME;
    if (strncmp(n, "virtio", 6) == 0 || strncmp(n, "vd", 2) == 0 ||
        strncmp(n, "vblk", 4) == 0) return DB_VD;     /* virtio_blk's "vblk0" */
    return DB_SD;
}

static bool devfs_blk_ident(const struct blk_dev *d, char *out, size_t cap,
                            uint32_t *maj, uint32_t *min) {
    if (!d || d->gone) return false;
    if (d->class != BLK_CLASS_DISK && d->class != BLK_CLASS_PARTITION) return false;
    const struct blk_dev *disk = (d->class == BLK_CLASS_PARTITION) ? d->parent : d;
    if (!disk || !disk->name) return false;
    int kind = devfs_blk_kind(d);
    size_t cnt = blk_count();
    int idx = -1, k = 0;
    for (size_t i = 0; i < cnt; i++) {
        struct blk_dev *x = blk_get(i);
        if (!x || x->class != BLK_CLASS_DISK || devfs_blk_kind(x) != kind) continue;
        if (x == disk) { idx = k; break; }
        k++;
    }
    if (idx < 0) return false;
    char base[32];
    if (kind == DB_NVME) {
        unsigned h = 0, ns = 1;
        const char *p = disk->name + 4;
        while (*p >= '0' && *p <= '9') h = h * 10 + (unsigned)(*p++ - '0');
        if (*p == ':') p++;
        if (*p == 'n') p++;
        if (*p >= '0' && *p <= '9') {
            ns = 0;
            while (*p >= '0' && *p <= '9') ns = ns * 10 + (unsigned)(*p++ - '0');
        }
        ksnprintf(base, sizeof base, "nvme%un%u", h, ns);
    } else {
        if (idx >= 26) return false;
        ksnprintf(base, sizeof base, "%s%c", kind == DB_VD ? "vd" : "sd", 'a' + idx);
    }
    uint32_t part = (d->class == BLK_CLASS_PARTITION) ? d->partition_index : 0;
    if (part) ksnprintf(out, cap, "%s%s%u", base, kind == DB_NVME ? "p" : "", part);
    else      ksnprintf(out, cap, "%s", base);
    uint32_t a, b;
    if (kind == DB_NVME) {
        b = 0;
        for (size_t i = 0; i < cnt; i++) {
            struct blk_dev *x = blk_get(i);
            if (!x || x->gone) continue;
            if (x->class != BLK_CLASS_DISK && x->class != BLK_CLASS_PARTITION) continue;
            if (devfs_blk_kind(x) != DB_NVME) continue;
            if (x == d) break;
            b++;
        }
        a = 259;
    } else {
        a = (kind == DB_VD) ? 253 : 8;
        b = (uint32_t)idx * 16 + part;
    }
    if (maj) *maj = a;
    if (min) *min = b;
    return true;
}

bool devfs_blk_path(const struct blk_dev *d, char *out, size_t cap) {
    char n[32];
    if (!devfs_blk_ident(d, n, sizeof n, 0, 0)) return false;
    ksnprintf(out, cap, "/dev/%s", n);
    return true;
}

static struct blk_dev *devfs_blk_by_rel(const char *rel, uint32_t *maj, uint32_t *min) {
    if (!rel || !rel[0]) return 0;
    for (const char *q = rel; *q; q++) if (*q == '/') return 0;
    size_t cnt = blk_count();
    for (size_t i = 0; i < cnt; i++) {
        struct blk_dev *x = blk_get(i);
        char n[32];
        if (x && devfs_blk_ident(x, n, sizeof n, maj, min) && strcmp(n, rel) == 0)
            return x;
    }
    return 0;
}

struct blk_dev *devfs_blk_by_path(const char *kpath, uint32_t *maj, uint32_t *min) {
    if (!kpath || strncmp(kpath, "/dev/", 5) != 0) return 0;
    return devfs_blk_by_rel(kpath + 5, maj, min);
}

/* Linux's /proc/partitions: a header, a blank line, then each disk and
 * partition -- major, minor, its size in 1 KiB blocks, its name. */
size_t devfs_partitions(char *buf, size_t cap) {
    size_t off = 0;
    if (!buf || cap == 0) return 0;
    int w = ksnprintf(buf, cap, "major minor  #blocks  name\n\n");
    if (w > 0) off = (size_t)w < cap ? (size_t)w : cap - 1;
    /* Each disk, then its own partitions, as Linux lists them: the
     * registry holds every disk before the first partition. */
    size_t cnt = blk_count();
    for (size_t i = 0; i < cnt && off + 1 < cap; i++) {
        struct blk_dev *disk = blk_get(i);
        if (!disk || disk->class != BLK_CLASS_DISK) continue;
        for (size_t j = i; j < cnt && off + 1 < cap; j++) {
            struct blk_dev *x = blk_get(j);
            char n[32];
            uint32_t a, b;
            if (!x || (x != disk && (x->class != BLK_CLASS_PARTITION ||
                                     x->parent != disk))) continue;
            if (!devfs_blk_ident(x, n, sizeof n, &a, &b)) continue;
            w = ksnprintf(buf + off, cap - off, "%4u  %7u %10llu %s\n", a, b,
                          (unsigned long long)(x->sector_count / 2), n);
            if (w <= 0) break;
            off += (size_t)w < cap - off ? (size_t)w : cap - off - 1;
        }
    }
    return off;
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
    if (devfs_blk_by_rel(rel, maj, min)) {             /* finding 198 */
        *mode = DEV_S_IFBLK | 0660;
        return true;
    }
    return false;
}

/* The /dev name of the device numbered MAJ:MIN of format FMT (S_IFCHR or
 * S_IFBLK): what a device node made elsewhere opens (finding 214). False
 * when no device has those numbers -- ENXIO, as Linux says. */
bool devfs_path_by_rdev(uint32_t fmt, uint32_t maj, uint32_t min,
                        char *out, size_t cap) {
    for (unsigned i = 0; i < DEVFS_N; i++) {
        const struct devfs_ent *e = &g_devfs[i];
        if ((e->mode & 0xF000u) != fmt || e->maj != maj || e->min != min) continue;
        if (!devfs_present(e)) continue;
        ksnprintf(out, cap, "/dev/%s", e->path);
        return true;
    }
    char n[32];
    int idx;
    if (fmt == DEV_S_IFCHR && maj == 4) {
        ksnprintf(n, sizeof n, "tty%u", min);
        if (devfs_vt_index(n, &idx)) { ksnprintf(out, cap, "/dev/%s", n); return true; }
    }
    if (fmt == DEV_S_IFCHR && maj == 136 && min < 10000 && pty_slave_live((int)min)) {
        ksnprintf(out, cap, "/dev/pts/%u", min);
        return true;
    }
    if (fmt == DEV_S_IFBLK) {
        size_t cnt = blk_count();
        for (size_t i = 0; i < cnt; i++) {
            uint32_t a, b;
            if (devfs_blk_ident(blk_get(i), n, sizeof n, &a, &b) && a == maj && b == min) {
                ksnprintf(out, cap, "/dev/%s", n);
                return true;
            }
        }
    }
    return false;
}

/* ---- the answer syscall.c asks (dev_synth_mode) ----------------------- */

uint32_t devfs_node_mode(const char *kpath, uint32_t *maj, uint32_t *min) {
    if (!kpath || strncmp(kpath, "/dev/", 5) != 0) return 0;
    uint32_t mode, a, b;
    if (!devfs_lookup(kpath + 5, &mode, &a, &b)) return 0;
    if ((mode & 0xF000u) != DEV_S_IFCHR &&
        (mode & 0xF000u) != DEV_S_IFBLK) return 0;      /* nodes only */
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
    if ((mode & 0xF000u) == DEV_S_IFBLK) out->gid = DEV_GID_DISK;   /* 198 */
    if ((mode & 0xF000u) == DEV_S_IFCHR || (mode & 0xF000u) == DEV_S_IFBLK) {
        out->rmaj = a;                       /* 214 */
        out->rmin = b;
    }
    {   /* made at boot, as devtmpfs's are (finding 202; src/syscall.c) */
        extern uint64_t dev_boot_time(void);
        out->atime = out->mtime = out->ctime = dev_boot_time();
    }
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
    if (dir[0] == '\0') {                          /* the disks (198) */
        size_t cnt = blk_count();
        for (size_t i = 0; i < cnt; i++) {
            char nb[32];
            if (!devfs_blk_ident(blk_get(i), nb, sizeof nb, 0, 0)) continue;
            if (idx++ == d->index) {
                devfs_fill(out, nb, DEV_S_IFBLK | 0660);
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
    .fsname   = "devtmpfs",          /* batch 31: /proc/mounts */
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
