/* procfs.c -- /proc virtual filesystem (M1.5).
 *
 * Mounts at /proc and generates file contents on-the-fly from kernel
 * state. All files are read-only; writes return VFS_ERR_ROFS.
 *
 * Layout:
 *   /proc/uptime         system uptime in seconds
 *   /proc/meminfo        total / free / used memory
 *   /proc/version        kernel version string
 *   /proc/self           symlink -> /proc/<current_pid>
 *   /proc/<pid>/status   name, state, pid, ppid, memory
 *   /proc/<pid>/cmdline  process name
 */

#include <tobyos/procfs.h>
#include <tobyos/vfs.h>
#include <tobyos/proc.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pit.h>
#include <tobyos/pmm.h>

extern struct proc g_proc[PROC_MAX];

/* ---- helpers ---- */

static int parse_int(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

static int int_to_str(char *buf, size_t cap, int64_t v) {
    char tmp[24];
    int neg = 0;
    int len = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { tmp[len++] = '0'; }
    else { while (v > 0) { tmp[len++] = '0' + (int)(v % 10); v /= 10; } }
    if (neg) tmp[len++] = '-';
    if ((size_t)len >= cap) return -1;
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

static int uint_to_str(char *buf, size_t cap, uint64_t v) {
    char tmp[24];
    int len = 0;
    if (v == 0) { tmp[len++] = '0'; }
    else { while (v > 0) { tmp[len++] = '0' + (int)(v % 10); v /= 10; } }
    if ((size_t)len >= cap) return -1;
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

/* ---- content generators ---- */

static int gen_uptime(char *buf, size_t cap) {
    uint64_t ticks = pit_ticks();
    uint32_t hz    = pit_hz();
    uint64_t secs  = (hz > 0) ? ticks / hz : 0;
    int n = uint_to_str(buf, cap, secs);
    if (n < 0 || (size_t)(n + 2) >= cap) return 0;
    buf[n] = '\n'; buf[n+1] = '\0';
    return n + 1;
}

static int gen_meminfo(char *buf, size_t cap) {
    size_t total = pmm_total_pages();
    size_t used  = pmm_used_pages();
    size_t fr    = pmm_free_pages();
    char tmp[24];
    int off = 0;

    #define APPEND_STR(s) do { \
        size_t sl = strlen(s); \
        if ((size_t)off + sl >= cap) return off; \
        memcpy(buf + off, s, sl); off += (int)sl; \
    } while (0)
    #define APPEND_UINT(v) do { \
        uint_to_str(tmp, sizeof(tmp), (uint64_t)(v) * 4096); \
        APPEND_STR(tmp); \
    } while (0)

    APPEND_STR("MemTotal: "); APPEND_UINT(total); APPEND_STR("\n");
    APPEND_STR("MemFree:  "); APPEND_UINT(fr);    APPEND_STR("\n");
    APPEND_STR("MemUsed:  "); APPEND_UINT(used);  APPEND_STR("\n");
    buf[off] = '\0';
    return off;

    #undef APPEND_STR
    #undef APPEND_UINT
}

static int gen_version(char *buf, size_t cap) {
    const char *v = "tobyOS 1.0 (x86_64)\n";
    size_t vl = strlen(v);
    if (vl >= cap) vl = cap - 1;
    memcpy(buf, v, vl);
    buf[vl] = '\0';
    return (int)vl;
}

static int gen_pid_status(int pid, char *buf, size_t cap) {
    struct proc *p = proc_lookup(pid);
    if (!p) return -1;

    int off = 0;
    char tmp[24];

    #define APPEND_STR(s) do { \
        size_t sl = strlen(s); \
        if ((size_t)off + sl >= cap) return off; \
        memcpy(buf + off, s, sl); off += (int)sl; \
    } while (0)
    #define APPEND_INT(v) do { \
        int_to_str(tmp, sizeof(tmp), (int64_t)(v)); \
        APPEND_STR(tmp); \
    } while (0)

    APPEND_STR("Name:   "); APPEND_STR(p->name);     APPEND_STR("\n");
    APPEND_STR("State:  "); APPEND_STR(proc_state_name(p->state)); APPEND_STR("\n");
    APPEND_STR("Pid:    "); APPEND_INT(p->pid);       APPEND_STR("\n");
    APPEND_STR("PPid:   "); APPEND_INT(p->ppid);      APPEND_STR("\n");
    APPEND_STR("Uid:    "); APPEND_INT(p->uid);        APPEND_STR("\n");
    buf[off] = '\0';
    return off;

    #undef APPEND_STR
    #undef APPEND_INT
}

static int gen_pid_cmdline(int pid, char *buf, size_t cap) {
    struct proc *p = proc_lookup(pid);
    if (!p) return -1;
    size_t nl = strlen(p->name);
    if (nl >= cap) nl = cap - 1;
    memcpy(buf, p->name, nl);
    buf[nl] = '\n'; nl++;
    buf[nl] = '\0';
    return (int)nl;
}

/* ---- VFS driver ---- */

struct procfs_handle {
    char *data;
    size_t len;
    size_t pos;
};

static int procfs_open(void *mnt, const char *path, struct vfs_file *out) {
    (void)mnt;
    if (!path || path[0] != '/') return VFS_ERR_NOENT;
    const char *rel = path + 1; /* skip leading '/' */

    char buf[1024];
    int len = -1;

    if (strcmp(rel, "uptime") == 0)  { len = gen_uptime(buf, sizeof(buf));  }
    else if (strcmp(rel, "meminfo") == 0) { len = gen_meminfo(buf, sizeof(buf)); }
    else if (strcmp(rel, "version") == 0) { len = gen_version(buf, sizeof(buf)); }
    else {
        /* Try /proc/<pid>/status or /proc/<pid>/cmdline */
        if (rel[0] >= '0' && rel[0] <= '9') {
            const char *slash = rel;
            while (*slash && *slash != '/') slash++;
            if (*slash == '/') {
                char pid_str[16];
                size_t pidlen = (size_t)(slash - rel);
                if (pidlen >= sizeof(pid_str)) return VFS_ERR_NOENT;
                memcpy(pid_str, rel, pidlen);
                pid_str[pidlen] = '\0';
                int pid = parse_int(pid_str);
                const char *sub = slash + 1;
                if (strcmp(sub, "status") == 0)
                    len = gen_pid_status(pid, buf, sizeof(buf));
                else if (strcmp(sub, "cmdline") == 0)
                    len = gen_pid_cmdline(pid, buf, sizeof(buf));
            }
        }
    }

    if (len < 0) return VFS_ERR_NOENT;

    struct procfs_handle *h = kmalloc(sizeof(*h));
    if (!h) return VFS_ERR_NOMEM;
    h->data = kmalloc((size_t)len + 1);
    if (!h->data) { kfree(h); return VFS_ERR_NOMEM; }
    memcpy(h->data, buf, (size_t)len + 1);
    h->len = (size_t)len;
    h->pos = 0;

    out->priv = h;
    out->size = h->len;
    return VFS_OK;
}

static int procfs_close(struct vfs_file *f) {
    struct procfs_handle *h = f->priv;
    if (h) { kfree(h->data); kfree(h); }
    f->priv = 0;
    return VFS_OK;
}

static long procfs_read(struct vfs_file *f, void *buf, size_t n) {
    struct procfs_handle *h = f->priv;
    if (!h) return VFS_ERR_INVAL;
    if (h->pos >= h->len) return 0;
    size_t avail = h->len - h->pos;
    if (n > avail) n = avail;
    memcpy(buf, h->data + h->pos, n);
    h->pos += n;
    f->pos = h->pos;
    return (long)n;
}

static int procfs_stat(void *mnt, const char *path, struct vfs_stat *out) {
    (void)mnt;
    if (!path || path[0] != '/') return VFS_ERR_NOENT;
    const char *rel = path + 1;

    /* Root /proc directory or a pid directory */
    if (rel[0] == '\0') {
        out->type = VFS_TYPE_DIR;
        out->size = 0;
        out->uid = 0; out->gid = 0; out->mode = 0;
        return VFS_OK;
    }

    /* Check if it's a pid directory: /proc/<pid> */
    if (rel[0] >= '0' && rel[0] <= '9') {
        const char *s = rel;
        while (*s >= '0' && *s <= '9') s++;
        if (*s == '\0') {
            int pid = parse_int(rel);
            if (proc_lookup(pid)) {
                out->type = VFS_TYPE_DIR;
                out->size = 0;
                out->uid = 0; out->gid = 0; out->mode = 0;
                return VFS_OK;
            }
            return VFS_ERR_NOENT;
        }
    }

    /* Try opening the file to determine its size. */
    struct vfs_file tmp;
    memset(&tmp, 0, sizeof(tmp));
    int rc = procfs_open(mnt, path, &tmp);
    if (rc != VFS_OK) return rc;
    out->type = VFS_TYPE_FILE;
    out->size = tmp.size;
    out->uid = 0; out->gid = 0; out->mode = 0;
    procfs_close(&tmp);
    return VFS_OK;
}

/* procfs directory listing */

struct procfs_dir_handle {
    int index;
    int pid;    /* -1 for /proc root, else the pid we're listing */
};

static int procfs_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    (void)mnt;
    if (!path || path[0] != '/') return VFS_ERR_NOENT;
    struct procfs_dir_handle *dh = kmalloc(sizeof(*dh));
    if (!dh) return VFS_ERR_NOMEM;
    dh->index = 0;

    if (strcmp(path, "/") == 0) {
        dh->pid = -1;
    } else {
        const char *rel = path + 1;
        dh->pid = parse_int(rel);
        if (!proc_lookup(dh->pid)) { kfree(dh); return VFS_ERR_NOENT; }
    }
    out->priv = dh;
    return VFS_OK;
}

static int procfs_closedir(struct vfs_dir *d) {
    kfree(d->priv);
    d->priv = 0;
    return VFS_OK;
}

static int procfs_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    struct procfs_dir_handle *dh = d->priv;
    if (!dh) return VFS_ERR_INVAL;

    if (dh->pid == -1) {
        /* Root /proc listing: global files first, then pid dirs */
        static const char *globals[] = { "uptime", "meminfo", "version", "self" };
        if (dh->index < 4) {
            memset(out->name, 0, VFS_NAME_MAX);
            size_t nl = strlen(globals[dh->index]);
            memcpy(out->name, globals[dh->index], nl);
            out->type = (dh->index == 3) ? VFS_TYPE_SYMLINK : VFS_TYPE_FILE;
            out->size = 0;
            out->uid = 0; out->gid = 0; out->mode = 0;
            dh->index++;
            return VFS_OK;
        }
        int pidx = dh->index - 4;
        for (int i = pidx; i < PROC_MAX; i++) {
            if (g_proc[i].state != PROC_UNUSED) {
                memset(out->name, 0, VFS_NAME_MAX);
                char tmp[16];
                int_to_str(tmp, sizeof(tmp), g_proc[i].pid);
                size_t tl = strlen(tmp);
                memcpy(out->name, tmp, tl);
                out->type = VFS_TYPE_DIR;
                out->size = 0;
                out->uid = 0; out->gid = 0; out->mode = 0;
                dh->index = 4 + i + 1;
                return VFS_OK;
            }
        }
        return VFS_ERR_NOENT;
    }

    /* Per-pid directory listing */
    static const char *entries[] = { "status", "cmdline" };
    if (dh->index >= 2) return VFS_ERR_NOENT;
    memset(out->name, 0, VFS_NAME_MAX);
    size_t nl = strlen(entries[dh->index]);
    memcpy(out->name, entries[dh->index], nl);
    out->type = VFS_TYPE_FILE;
    out->size = 0;
    out->uid = 0; out->gid = 0; out->mode = 0;
    dh->index++;
    return VFS_OK;
}

static const struct vfs_ops procfs_ops = {
    .open     = procfs_open,
    .close    = procfs_close,
    .read     = procfs_read,
    .write    = 0,
    .create   = 0,
    .unlink   = 0,
    .mkdir    = 0,
    .opendir  = procfs_opendir,
    .closedir = procfs_closedir,
    .readdir  = procfs_readdir,
    .stat     = procfs_stat,
    .chmod    = 0,
    .chown    = 0,
    .umount   = 0,
};

void procfs_init(void) {
    int rc = vfs_mount("/proc", &procfs_ops, 0);
    if (rc != VFS_OK) {
        kprintf("[procfs] mount failed: %s\n", vfs_strerror(rc));
        return;
    }

    /* /proc/self symlink: updated dynamically on readlink, but we
     * register a symlink entry so VFS path resolution catches it. We
     * use pid 0 as a placeholder; the actual readlink of /proc/self
     * will need to resolve to current_proc()->pid. We handle this via
     * a dynamic update in the open path instead. */
    struct proc *p = current_proc();
    char target[32];
    int n = 0;
    memcpy(target, "/proc/", 6); n = 6;
    char pidstr[16];
    int pl = int_to_str(pidstr, sizeof(pidstr), p ? p->pid : 0);
    memcpy(target + n, pidstr, (size_t)pl); n += pl;
    target[n] = '\0';
    vfs_symlink("/proc/self", target);

    kprintf("[procfs] mounted at /proc\n");
}
