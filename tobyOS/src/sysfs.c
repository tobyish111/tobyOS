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

#define SYSFS_MAX_NODES 64
#define SYSFS_BUF_SIZE  256

struct sysfs_node {
    char path[VFS_PATH_MAX];
    enum vfs_type type;
    /* For files: function to generate content dynamically */
    int (*generate)(char *buf, size_t bufsz);
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
    .chmod   = 0,
    .chown   = 0,
    .umount  = 0,
};

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

    int rc = vfs_mount("/sys", &sysfs_ops, 0);
    if (rc == VFS_OK) {
        kprintf("[sysfs] mounted at /sys (%d nodes)\n", g_sysfs_count);
    } else {
        kprintf("[sysfs] mount failed: %s\n", vfs_strerror(rc));
    }
}
