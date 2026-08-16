/* sysfs.c -- /sys virtual filesystem exposing kernel/hardware state.
 *
 * Provides a read-only virtual tree at /sys with subtrees:
 *   /sys/cpu/count       - number of online CPUs
 *   /sys/cpu/model       - CPU model string
 *   /sys/mem/total       - total physical RAM in bytes
 *   /sys/mem/free        - free physical RAM in bytes
 *   /sys/kernel/version  - kernel version string
 *   /sys/kernel/uptime   - uptime in milliseconds
 *   /sys/devices/...     - enumerated devices
 *
 * Mounts via the VFS as a standard filesystem driver at /sys.
 */

#include <tobyos/vfs.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pmm.h>
#include <tobyos/types.h>

extern uint64_t pit_ticks(void);
extern uint32_t pit_hz(void);
extern uint32_t smp_cpu_count(void);

#define SYSFS_MAX_NODES 96   /* slice 100: + the DRM device tree */
#define SYSFS_BUF_SIZE  256

struct sysfs_node {
    char path[VFS_PATH_MAX];
    enum vfs_type type;
    /* For files: function to generate content dynamically */
    int (*generate)(char *buf, size_t bufsz);
    /* Slice 100: for VFS_TYPE_SYMLINK, the literal target. Sysfs served
     * only files and dirs, but libdrm READLINKS
     * /sys/dev/char/<maj>:<min>/device/subsystem and keys the bus type on
     * the target's basename -- a regular file cannot answer that. */
    const char *link;
};

static struct sysfs_node g_sysfs_nodes[SYSFS_MAX_NODES];
static int g_sysfs_count;

/* ---- content generators ---- */

static int gen_cpu_count(char *buf, size_t sz) {
    uint32_t n = smp_cpu_count();
    int len = 0;
    char tmp[32];
    uint32_t v = n;
    if (v == 0) { tmp[0] = '0'; len = 1; }
    else {
        while (v > 0) { tmp[len++] = '0' + (v % 10); v /= 10; }
        for (int i = 0; i < len / 2; i++) {
            char c = tmp[i]; tmp[i] = tmp[len-1-i]; tmp[len-1-i] = c;
        }
    }
    tmp[len++] = '\n';
    if ((size_t)len >= sz) len = (int)sz - 1;
    memcpy(buf, tmp, (size_t)len);
    buf[len] = '\0';
    return len;
}

static int gen_mem_total(char *buf, size_t sz) {
    extern size_t pmm_total_pages(void);
    size_t total = pmm_total_pages() * 4096;
    char tmp[32]; int len = 0;
    size_t v = total;
    if (v == 0) { tmp[0] = '0'; len = 1; }
    else {
        while (v > 0) { tmp[len++] = '0' + (char)(v % 10); v /= 10; }
        for (int i = 0; i < len / 2; i++) {
            char c = tmp[i]; tmp[i] = tmp[len-1-i]; tmp[len-1-i] = c;
        }
    }
    tmp[len++] = '\n';
    if ((size_t)len >= sz) len = (int)sz - 1;
    memcpy(buf, tmp, (size_t)len);
    buf[len] = '\0';
    return len;
}

static int gen_mem_free(char *buf, size_t sz) {
    extern size_t pmm_free_pages(void);
    size_t fr = pmm_free_pages() * 4096;
    char tmp[32]; int len = 0;
    size_t v = fr;
    if (v == 0) { tmp[0] = '0'; len = 1; }
    else {
        while (v > 0) { tmp[len++] = '0' + (char)(v % 10); v /= 10; }
        for (int i = 0; i < len / 2; i++) {
            char c = tmp[i]; tmp[i] = tmp[len-1-i]; tmp[len-1-i] = c;
        }
    }
    tmp[len++] = '\n';
    if ((size_t)len >= sz) len = (int)sz - 1;
    memcpy(buf, tmp, (size_t)len);
    buf[len] = '\0';
    return len;
}

static int gen_kernel_version(char *buf, size_t sz) {
    const char *ver = "tobyOS 1.0.0\n";
    size_t len = strlen(ver);
    if (len >= sz) len = sz - 1;
    memcpy(buf, ver, len);
    buf[len] = '\0';
    return (int)len;
}

static int gen_kernel_uptime(char *buf, size_t sz) {
    uint32_t hz = pit_hz();
    uint64_t ms = (hz > 0) ? (pit_ticks() * 1000ULL) / (uint64_t)hz : 0;
    char tmp[32]; int len = 0;
    uint64_t v = ms;
    if (v == 0) { tmp[0] = '0'; len = 1; }
    else {
        while (v > 0) { tmp[len++] = '0' + (char)(v % 10); v /= 10; }
        for (int i = 0; i < len / 2; i++) {
            char c = tmp[i]; tmp[i] = tmp[len-1-i]; tmp[len-1-i] = c;
        }
    }
    tmp[len++] = '\n';
    if ((size_t)len >= sz) len = (int)sz - 1;
    memcpy(buf, tmp, (size_t)len);
    buf[len] = '\0';
    return len;
}

/* B24: Linux-compatible nodes. get_nprocs()/sysconf(_SC_NPROCESSORS_ONLN)
 * read /sys/devices/system/cpu/online (the cpulist: "0" for one CPU,
 * "0-<n-1>" for several); uname-ish probes read /sys/kernel/{ostype,osrelease}. */
static int gen_cpu_online(char *buf, size_t sz) {
    uint32_t n = smp_cpu_count();
    if (n == 0) n = 1;
    int len = 0;
    buf[len++] = '0';
    if (n > 1) {
        buf[len++] = '-';
        char tmp[16]; int tl = 0; uint32_t v = n - 1;
        if (v == 0) tmp[tl++] = '0';
        else { while (v) { tmp[tl++] = (char)('0' + v % 10); v /= 10; } }
        while (tl > 0 && (size_t)len < sz - 2) buf[len++] = tmp[--tl];
    }
    buf[len++] = '\n';
    buf[len] = '\0';
    return len;
}

static int gen_ostype(char *buf, size_t sz) {
    const char *s = "Linux\n";
    size_t len = strlen(s);
    if (len >= sz) len = sz - 1;
    memcpy(buf, s, len); buf[len] = '\0';
    return (int)len;
}

static int gen_osrelease(char *buf, size_t sz) {
    const char *s = "6.1.0-tobyos\n";
    size_t len = strlen(s);
    if (len >= sz) len = sz - 1;
    memcpy(buf, s, len); buf[len] = '\0';
    return (int)len;
}

/* Slice 100: PCI identity of the virtio-gpu device, in the exact shapes
 * libdrm parses. sysfs reports these as "0x%04x\n" text. */
static int gen_fixed(char *buf, size_t sz, const char *s) {
    size_t n = strlen(s);
    if (n >= sz) n = sz - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    return (int)n;
}
static int gen_pci_vendor(char *b, size_t s)   { return gen_fixed(b, s, "0x1af4\n"); }
static int gen_pci_device(char *b, size_t s)   { return gen_fixed(b, s, "0x1050\n"); }
static int gen_pci_revision(char *b, size_t s) { return gen_fixed(b, s, "0x01\n"); }
static int gen_pci_subdev(char *b, size_t s)   { return gen_fixed(b, s, "0x1100\n"); }
/* Linux libdrm does NOT read the text vendor/device files -- on Linux
 * drmParsePciDeviceInfo() opens .../device/config and reads the first 64
 * BINARY bytes of PCI config space. Synthesize that header for the
 * virtio-gpu function (1af4:1050, class 0x030000 display, subsystem
 * 1af4:1100). Returns an explicit 64, so the NUL bytes inside are fine. */
static int gen_pci_config(char *b, size_t s) {
    if (s < 64) return 0;
    memset(b, 0, 64);
    b[0x00] = 0xf4; b[0x01] = 0x1a;          /* vendor 1af4 */
    b[0x02] = 0x50; b[0x03] = 0x10;          /* device 1050 */
    b[0x04] = 0x07; b[0x05] = 0x00;          /* command: io+mem+busmaster */
    b[0x06] = 0x10; b[0x07] = 0x00;          /* status: cap list */
    b[0x08] = 0x01;                          /* revision 01 */
    b[0x09] = 0x00; b[0x0a] = 0x00; b[0x0b] = 0x03;  /* class 030000 */
    b[0x0e] = 0x00;                          /* header type 0 */
    b[0x2c] = 0xf4; b[0x2d] = 0x1a;          /* subsystem vendor 1af4 */
    b[0x2e] = 0x00; b[0x2f] = 0x11;          /* subsystem device 1100 */
    b[0x34] = 0x40;                          /* capabilities pointer */
    return 64;
}

static int gen_drm_uevent(char *b, size_t s) {
    return gen_fixed(b, s,
        "DRIVER=virtio_gpu\n"
        "PCI_CLASS=30000\n"
        "PCI_ID=1AF4:1050\n"
        "PCI_SUBSYS_ID=1AF4:1100\n"
        "PCI_SLOT_NAME=0000:00:04.0\n"
        "MODALIAS=pci:v00001AF4d00001050sv00001AF4sd00001100bc03sc00i00\n");
}

/* ---- registration ---- */

static void sysfs_add_dir(const char *path) {
    if (g_sysfs_count >= SYSFS_MAX_NODES) return;
    struct sysfs_node *n = &g_sysfs_nodes[g_sysfs_count++];
    size_t len = strlen(path);
    if (len >= VFS_PATH_MAX) len = VFS_PATH_MAX - 1;
    memcpy(n->path, path, len);
    n->path[len] = '\0';
    n->type = VFS_TYPE_DIR;
    n->generate = 0;
}

static void sysfs_add_file(const char *path, int (*gen)(char *, size_t)) {
    if (g_sysfs_count >= SYSFS_MAX_NODES) return;
    struct sysfs_node *n = &g_sysfs_nodes[g_sysfs_count++];
    size_t len = strlen(path);
    if (len >= VFS_PATH_MAX) len = VFS_PATH_MAX - 1;
    memcpy(n->path, path, len);
    n->path[len] = '\0';
    n->type = VFS_TYPE_FILE;
    n->generate = gen;
}

static void sysfs_add_link(const char *path, const char *target) {
    if (g_sysfs_count >= SYSFS_MAX_NODES) return;
    struct sysfs_node *n = &g_sysfs_nodes[g_sysfs_count++];
    size_t len = strlen(path);
    if (len >= VFS_PATH_MAX) len = VFS_PATH_MAX - 1;
    memcpy(n->path, path, len);
    n->path[len] = '\0';
    n->type = VFS_TYPE_SYMLINK;
    n->generate = 0;
    n->link = target;
}

/* ---- VFS ops ---- */

struct sysfs_file_priv {
    char buf[SYSFS_BUF_SIZE];
    size_t len;
    size_t pos;
};

static struct sysfs_node *sysfs_find(const char *path) {
    for (int i = 0; i < g_sysfs_count; i++) {
        if (strcmp(g_sysfs_nodes[i].path, path) == 0)
            return &g_sysfs_nodes[i];
    }
    return 0;
}

static int sysfs_open(void *mnt, const char *path, struct vfs_file *out) {
    (void)mnt;
    struct sysfs_node *n = sysfs_find(path);
    if (!n) return VFS_ERR_NOENT;
    if (n->type == VFS_TYPE_DIR) return VFS_ERR_ISDIR;

    struct sysfs_file_priv *priv = kmalloc(sizeof(*priv));
    if (!priv) return VFS_ERR_NOMEM;
    memset(priv, 0, sizeof(*priv));

    if (n->generate) {
        int len = n->generate(priv->buf, SYSFS_BUF_SIZE);
        priv->len = (size_t)(len > 0 ? len : 0);
    }
    priv->pos = 0;
    out->priv = priv;
    out->size = priv->len;
    return VFS_OK;
}

static int sysfs_close(struct vfs_file *f) {
    if (f->priv) kfree(f->priv);
    return VFS_OK;
}

static long sysfs_read(struct vfs_file *f, void *buf, size_t n) {
    struct sysfs_file_priv *priv = f->priv;
    if (!priv) return 0;
    if (priv->pos >= priv->len) return 0;
    size_t avail = priv->len - priv->pos;
    if (n > avail) n = avail;
    memcpy(buf, priv->buf + priv->pos, n);
    priv->pos += n;
    f->pos = priv->pos;
    return (long)n;
}

static int sysfs_stat(void *mnt, const char *path, struct vfs_stat *out) {
    (void)mnt;
    struct sysfs_node *n = sysfs_find(path);
    if (!n) return VFS_ERR_NOENT;
    out->type = n->type;
    out->size = 0;
    out->uid = 0;
    out->gid = 0;
    out->mode = 00444u | VFS_MODE_VALID;
    if (n->type == VFS_TYPE_FILE && n->generate) {
        char tmp[SYSFS_BUF_SIZE];
        int len = n->generate(tmp, SYSFS_BUF_SIZE);
        out->size = (size_t)(len > 0 ? len : 0);
    }
    return VFS_OK;
}

static int sysfs_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    (void)mnt;
    struct sysfs_node *n = sysfs_find(path);
    if (!n && strcmp(path, "/") != 0) return VFS_ERR_NOENT;
    out->index = 0;
    out->priv = (void *)(uintptr_t)(path[0] ? (uintptr_t)path : (uintptr_t)"/");
    return VFS_OK;
}

static int sysfs_closedir(struct vfs_dir *d) {
    (void)d;
    return VFS_OK;
}

static int sysfs_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    const char *parent = "/";
    if (d->priv) parent = (const char *)(uintptr_t)d->priv;

    size_t plen = strlen(parent);
    size_t idx = 0;

    for (int i = 0; i < g_sysfs_count; i++) {
        const char *npath = g_sysfs_nodes[i].path;
        if (strncmp(npath, parent, plen) != 0) continue;
        if (plen > 1 && npath[plen] != '/') continue;

        const char *rest = npath + plen;
        if (plen == 1 && npath[0] == '/') rest = npath + 1;
        else if (rest[0] == '/') rest++;

        if (rest[0] == '\0') continue;

        /* Only direct children (no nested '/' in the rest) */
        const char *slash = rest;
        while (*slash && *slash != '/') slash++;
        if (*slash == '/') continue;

        if (idx == d->index) {
            size_t nlen = (size_t)(slash - rest);
            if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
            memcpy(out->name, rest, nlen);
            out->name[nlen] = '\0';
            out->type = g_sysfs_nodes[i].type;
            out->size = 0;
            out->uid = 0;
            out->gid = 0;
            out->mode = 00444u | VFS_MODE_VALID;
            d->index++;
            return VFS_OK;
        }
        idx++;
    }
    return VFS_ERR_NOENT;
}

/* Slice 100: readlink for the symlink nodes (libdrm's bus-type probe). */
static int sysfs_readlink(void *mnt, const char *path, char *buf,
                          size_t bufsz) {
    (void)mnt;
    struct sysfs_node *n = sysfs_find(path);
    if (!n) return VFS_ERR_NOENT;
    if (n->type != VFS_TYPE_SYMLINK || !n->link) return VFS_ERR_INVAL;
    size_t len = strlen(n->link);
    if (len >= bufsz) len = bufsz ? bufsz - 1 : 0;
    memcpy(buf, n->link, len);
    if (bufsz) buf[len] = '\0';
    /* VFS_OK, NOT the length: vfs_readlink's callers test `rc != VFS_OK`
     * and measure the buffer themselves (see procfs). Returning the
     * length here made every successful readlink look like an error --
     * which is exactly what the first run reported. */
    return VFS_OK;
}

static const struct vfs_ops sysfs_ops = {
    .open    = sysfs_open,
    .close   = sysfs_close,
    .read    = sysfs_read,
    .write   = 0,
    .create  = 0,
    .unlink  = 0,
    .mkdir   = 0,
    .opendir = sysfs_opendir,
    .closedir= sysfs_closedir,
    .readdir = sysfs_readdir,
    .stat    = sysfs_stat,
    .readlink= sysfs_readlink,          /* slice 100 */
    .chmod   = 0,
    .chown   = 0,
    .umount  = 0,
};

/* Mount sysfs at an extra point (mount(2) -t sysfs). Same singleton
 * argument as procfs_mount_at -- and it also closes a gap that predates
 * this slice: /proc/filesystems has always advertised "nodev sysfs",
 * while mount(2) answered ENODEV for it. A filesystem list that names
 * something the kernel then refuses is the same class of lie as a knob
 * that accepts a limit and enforces nothing. */
int sysfs_mount_at(const char *path) {
    return vfs_mount(path, &sysfs_ops, 0);
}

void sysfs_init(void) {
    sysfs_add_dir("/");
    sysfs_add_dir("/cpu");
    sysfs_add_dir("/mem");
    sysfs_add_dir("/kernel");
    sysfs_add_file("/cpu/count", gen_cpu_count);
    sysfs_add_file("/mem/total", gen_mem_total);
    sysfs_add_file("/mem/free", gen_mem_free);
    sysfs_add_file("/kernel/version", gen_kernel_version);
    sysfs_add_file("/kernel/uptime", gen_kernel_uptime);

    /* B24: Linux-layout nodes real software reads at startup. */
    sysfs_add_dir("/devices");
    sysfs_add_dir("/devices/system");
    sysfs_add_dir("/devices/system/cpu");
    sysfs_add_file("/devices/system/cpu/online",   gen_cpu_online);
    sysfs_add_file("/devices/system/cpu/possible", gen_cpu_online);
    sysfs_add_file("/devices/system/cpu/present",  gen_cpu_online);
    sysfs_add_file("/kernel/ostype",    gen_ostype);
    sysfs_add_file("/kernel/osrelease", gen_osrelease);

    /* Slice 100 (tier 3 Phase 1c): the DRM device tree libdrm validates a
     * render-node fd against. Measured need -- Mesa loaded virtio_gpu_dri.so
     * off our node, then fell back to swrast WITHOUT another ioctl, because
     * gallium's pipe_loader calls drmGetDevice2(fd), which never touches the
     * device: it fstats the fd for the major:minor and then reads SYSFS.
     *
     * drmNodeIsDRM()        stat  .../device/drm
     * drmParseSubsystemType readlink .../device/subsystem -> basename "pci"
     * drmParsePciDeviceInfo .../device/{vendor,device,revision,subsystem_*}
     * drmParsePciBusInfo    .../device/uevent -> PCI_SLOT_NAME=
     *
     * 226 is the Linux DRM major; 128 is our render node's minor (see
     * linux_drm.c). The ids are the virtio-gpu ones the driver logs at
     * probe (1af4:1050). This is a static description of one device --
     * when a second DRM device ever exists, generate it from the PCI scan
     * rather than extending this by hand. */
    sysfs_add_dir("/dev");
    sysfs_add_dir("/dev/char");
    sysfs_add_dir("/dev/char/226:128");
    sysfs_add_dir("/dev/char/226:128/device");
    sysfs_add_dir("/dev/char/226:128/device/drm");
    sysfs_add_link("/dev/char/226:128/device/subsystem", "../../../bus/pci");
    sysfs_add_file("/dev/char/226:128/device/vendor",            gen_pci_vendor);
    sysfs_add_file("/dev/char/226:128/device/device",            gen_pci_device);
    sysfs_add_file("/dev/char/226:128/device/revision",          gen_pci_revision);
    sysfs_add_file("/dev/char/226:128/device/subsystem_vendor",  gen_pci_vendor);
    sysfs_add_file("/dev/char/226:128/device/subsystem_device",  gen_pci_subdev);
    sysfs_add_file("/dev/char/226:128/device/uevent",            gen_drm_uevent);
    sysfs_add_file("/dev/char/226:128/device/config",            gen_pci_config);
    /* The bus directory the subsystem link points at, so a resolver that
     * follows it finds something rather than dangling. */
    sysfs_add_dir("/bus");
    sysfs_add_dir("/bus/pci");

    int rc = vfs_mount("/sys", &sysfs_ops, 0);
    if (rc == VFS_OK) {
        kprintf("[sysfs] mounted at /sys (%d nodes)\n", g_sysfs_count);
    } else {
        kprintf("[sysfs] mount failed: %s\n", vfs_strerror(rc));
    }
}
