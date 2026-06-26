/* procfs.c -- /proc virtual filesystem (M1.5).
 *
 * Mounts at /proc and generates file contents on-the-fly from kernel
 * state. All files are read-only; writes return VFS_ERR_ROFS.
 *
 * Layout:
 *   /proc/uptime         system uptime in seconds
 *   /proc/meminfo        total / free / used memory
 *   /proc/version        kernel version string
 *   /proc/cpuinfo        one block per logical CPU (B20)
 *   /proc/self           symlink -> /proc/<calling-pid> (resolved live, B20)
 *   /proc/<pid>/status   name, state, pid, ppid, uid
 *   /proc/<pid>/cmdline  process name
 *   /proc/<pid>/maps     memory map: exe image + [heap] + [stack] (B20)
 *   /proc/<pid>/stat     single-line numeric stat (B20)
 *   /proc/<pid>/exe      symlink -> the process's executable path (B20)
 *
 * "self" is handled in every op by subst_self(), which rewrites a leading
 * "self" component to the CALLING process's pid -- so /proc/self/* always
 * refers to the live caller, not a boot-time placeholder.
 */

#include <tobyos/procfs.h>
#include <tobyos/vfs.h>
#include <tobyos/proc.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pit.h>
#include <tobyos/pmm.h>
#include <tobyos/smp.h>

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

/* Lowercase hex, no "0x", no leading zeros (Linux /proc/<pid>/maps style). */
static int hex_to_str(char *buf, size_t cap, uint64_t v) {
    char tmp[24];
    int len = 0;
    if (v == 0) { tmp[len++] = '0'; }
    else { while (v > 0) { int d = (int)(v & 0xf); tmp[len++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v >>= 4; } }
    if ((size_t)len >= cap) return -1;
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

/* Rewrite a procfs-relative path, replacing a leading "/self" component
 * with "/<current-pid>" so /proc/self/* works for ANY caller (the static
 * /proc/self symlink couldn't track the live process). Paths without a
 * "self" component are copied through unchanged. */
static void subst_self(const char *rel, char *out, size_t cap) {
    if (rel && rel[0] == '/' &&
        rel[1] == 's' && rel[2] == 'e' && rel[3] == 'l' && rel[4] == 'f' &&
        (rel[5] == '\0' || rel[5] == '/')) {
        struct proc *p = current_proc();
        int pid = p ? p->pid : 0;
        char pidstr[16];
        int pl = int_to_str(pidstr, sizeof(pidstr), pid);
        const char *tail = rel + 5;
        size_t tl = strlen(tail);
        if ((size_t)(1 + pl) + tl + 1 > cap) { /* truncate-safe fallback */
            size_t rl = strlen(rel); if (rl >= cap) rl = cap - 1;
            memcpy(out, rel, rl); out[rl] = '\0'; return;
        }
        out[0] = '/';
        memcpy(out + 1, pidstr, (size_t)pl);
        memcpy(out + 1 + pl, tail, tl + 1);
        return;
    }
    size_t rl = rel ? strlen(rel) : 0;
    if (rl >= cap) rl = cap - 1;
    if (rel) memcpy(out, rel, rl);
    out[rl] = '\0';
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

static int gen_cpuinfo(char *buf, size_t cap) {
    int off = 0;
    char tmp[24];
    #define APPEND_STR(s) do { \
        size_t sl = strlen(s); \
        if ((size_t)off + sl >= cap) return off; \
        memcpy(buf + off, s, sl); off += (int)sl; \
    } while (0)
    #define APPEND_INT(v) do { \
        int_to_str(tmp, sizeof(tmp), (int64_t)(v)); APPEND_STR(tmp); \
    } while (0)

    uint32_t ncpu = smp_cpu_count();
    if (ncpu == 0) ncpu = 1;
    for (uint32_t i = 0; i < ncpu; i++) {
        APPEND_STR("processor\t: "); APPEND_INT(i); APPEND_STR("\n");
        APPEND_STR("vendor_id\t: GenuineTobyOS\n");
        APPEND_STR("cpu family\t: 6\n");
        APPEND_STR("model\t\t: 1\n");
        APPEND_STR("model name\t: tobyOS virtual x86-64 CPU\n");
        APPEND_STR("flags\t\t: fpu tsc msr sse sse2 syscall\n");
        APPEND_STR("\n");
    }
    buf[off] = '\0';
    return off;
    #undef APPEND_STR
    #undef APPEND_INT
}

/* /proc/<pid>/maps -- a best-effort memory map synthesised from the few
 * regions the kernel tracks explicitly: the executable image (around the
 * entry point), the brk heap, and the user stack. Real software reads this
 * to discover its own [heap]/[stack] ranges and exe mapping. */
static int gen_pid_maps(int pid, char *buf, size_t cap) {
    struct proc *p = proc_lookup(pid);
    if (!p) return -1;
    int off = 0;
    char tmp[24];
    #define APPEND_STR(s) do { \
        size_t sl = strlen(s); \
        if ((size_t)off + sl >= cap) return off; \
        memcpy(buf + off, s, sl); off += (int)sl; \
    } while (0)
    #define APPEND_HEX(v) do { hex_to_str(tmp, sizeof(tmp), (uint64_t)(v)); APPEND_STR(tmp); } while (0)

    /* one map line: lo-hi perms 00000000 00:00 0    path */
    #define MAP_LINE(lo, hi, perms, path) do { \
        APPEND_HEX(lo); APPEND_STR("-"); APPEND_HEX(hi); \
        APPEND_STR(" "); APPEND_STR(perms); \
        APPEND_STR(" 00000000 00:00 0 \t"); APPEND_STR(path); APPEND_STR("\n"); \
    } while (0)

    /* executable image: round the entry down to a page; present one r-xp
     * page mapped to the exe path (span is approximate). */
    if (p->user_entry) {
        uint64_t lo = p->user_entry & ~0xfffULL;
        uint64_t hi = lo + 0x1000ULL;
        MAP_LINE(lo, hi, "r-xp", (p->exe_path[0] ? p->exe_path : p->name));
    }
    /* heap */
    if (p->brk_cur > p->brk_base) {
        MAP_LINE(p->brk_base, p->brk_cur, "rw-p", "[heap]");
    }
    /* stack */
    if (p->user_stack_base && p->user_stack_pages) {
        uint64_t slo = p->user_stack_base;
        uint64_t shi = p->user_stack_base + (uint64_t)p->user_stack_pages * 4096ULL;
        MAP_LINE(slo, shi, "rw-p", "[stack]");
    }
    buf[off] = '\0';
    return off;
    #undef APPEND_STR
    #undef APPEND_HEX
    #undef MAP_LINE
}

/* /proc/<pid>/stat -- the single-line numeric form. We emit the leading
 * fields tools actually parse (pid, comm, state, ppid, ...) and pad the
 * rest with zeros. */
static int gen_pid_stat(int pid, char *buf, size_t cap) {
    struct proc *p = proc_lookup(pid);
    if (!p) return -1;
    char st = 'R';
    switch (p->state) {
        case PROC_RUNNING: case PROC_READY: st = 'R'; break;
        case PROC_BLOCKED: st = 'S'; break;
        case PROC_TERMINATED: st = 'Z'; break;
        default: st = 'R'; break;
    }
    int off = 0;
    char tmp[24];
    #define APPEND_STR(s) do { \
        size_t sl = strlen(s); \
        if ((size_t)off + sl >= cap) return off; \
        memcpy(buf + off, s, sl); off += (int)sl; \
    } while (0)
    #define APPEND_INT(v) do { int_to_str(tmp, sizeof(tmp), (int64_t)(v)); APPEND_STR(tmp); } while (0)

    APPEND_INT(p->pid); APPEND_STR(" (");
    APPEND_STR(p->name); APPEND_STR(") ");
    { char ss[2] = { st, 0 }; APPEND_STR(ss); }
    APPEND_STR(" "); APPEND_INT(p->ppid);
    /* pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt
     * utime stime cutime cstime priority nice num_threads itrealvalue
     * starttime -- 18 fields, all zero. */
    for (int i = 0; i < 18; i++) APPEND_STR(" 0");
    APPEND_STR("\n");
    buf[off] = '\0';
    return off;
    #undef APPEND_STR
    #undef APPEND_INT
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

    char normbuf[ABI_PATH_MAX];
    subst_self(path, normbuf, sizeof(normbuf));
    const char *rel = normbuf + 1; /* skip leading '/' */

    char buf[2048];
    int len = -1;

    if (strcmp(rel, "uptime") == 0)  { len = gen_uptime(buf, sizeof(buf));  }
    else if (strcmp(rel, "meminfo") == 0) { len = gen_meminfo(buf, sizeof(buf)); }
    else if (strcmp(rel, "version") == 0) { len = gen_version(buf, sizeof(buf)); }
    else if (strcmp(rel, "cpuinfo") == 0) { len = gen_cpuinfo(buf, sizeof(buf)); }
    else {
        /* Try /proc/<pid>/<file> */
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
                else if (strcmp(sub, "maps") == 0)
                    len = gen_pid_maps(pid, buf, sizeof(buf));
                else if (strcmp(sub, "stat") == 0)
                    len = gen_pid_stat(pid, buf, sizeof(buf));
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

    char normbuf[ABI_PATH_MAX];
    subst_self(path, normbuf, sizeof(normbuf));
    const char *rel = normbuf + 1;

    /* Root /proc directory */
    if (rel[0] == '\0') {
        out->type = VFS_TYPE_DIR;
        out->size = 0;
        out->uid = 0; out->gid = 0; out->mode = 0;
        return VFS_OK;
    }

    /* /proc/<pid>  (dir)  and  /proc/<pid>/exe (symlink) */
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
        if (*s == '/' && strcmp(s + 1, "exe") == 0) {
            int pid = parse_int(rel);
            if (proc_lookup(pid)) {
                out->type = VFS_TYPE_SYMLINK;
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
    int rc = procfs_open(mnt, normbuf, &tmp);
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
        char normbuf[ABI_PATH_MAX];
        subst_self(path, normbuf, sizeof(normbuf));
        const char *rel = normbuf + 1;
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
        static const char *globals[] = { "uptime", "meminfo", "version",
                                         "cpuinfo", "self" };
        const int nglobals = 5;
        if (dh->index < nglobals) {
            memset(out->name, 0, VFS_NAME_MAX);
            size_t nl = strlen(globals[dh->index]);
            memcpy(out->name, globals[dh->index], nl);
            /* the last entry ("self") is a symlink, the rest are files */
            out->type = (dh->index == nglobals - 1) ? VFS_TYPE_SYMLINK
                                                     : VFS_TYPE_FILE;
            out->size = 0;
            out->uid = 0; out->gid = 0; out->mode = 0;
            dh->index++;
            return VFS_OK;
        }
        int pidx = dh->index - nglobals;
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
                dh->index = nglobals + i + 1;
                return VFS_OK;
            }
        }
        return VFS_ERR_NOENT;
    }

    /* Per-pid directory listing */
    static const char *entries[] = { "status", "cmdline", "maps", "stat", "exe" };
    const int nentries = 5;
    if (dh->index >= nentries) return VFS_ERR_NOENT;
    memset(out->name, 0, VFS_NAME_MAX);
    size_t nl = strlen(entries[dh->index]);
    memcpy(out->name, entries[dh->index], nl);
    /* "exe" is a symlink; the rest are files */
    out->type = (dh->index == nentries - 1) ? VFS_TYPE_SYMLINK : VFS_TYPE_FILE;
    out->size = 0;
    out->uid = 0; out->gid = 0; out->mode = 0;
    dh->index++;
    return VFS_OK;
}

/* B20: dynamically-resolved /proc symlinks (the static symlink table can't
 * track the live process). Handles:
 *   /self            -> /proc/<current-pid>
 *   /<pid>/exe       -> the process's executable path
 *   /self/exe        -> current process's executable path (via subst_self) */
static int procfs_readlink(void *mnt, const char *path, char *buf, size_t bufsz) {
    (void)mnt;
    if (!path || path[0] != '/' || !buf || bufsz == 0) return VFS_ERR_INVAL;

    /* Bare /proc/self -> /proc/<pid> */
    if (strcmp(path, "/self") == 0) {
        struct proc *p = current_proc();
        char pidstr[16];
        int pl = int_to_str(pidstr, sizeof(pidstr), p ? p->pid : 0);
        int n = 0;
        const char *pre = "/proc/";
        size_t prelen = strlen(pre);
        if (prelen + (size_t)pl + 1 > bufsz) return VFS_ERR_INVAL;
        memcpy(buf, pre, prelen); n = (int)prelen;
        memcpy(buf + n, pidstr, (size_t)pl); n += pl;
        buf[n] = '\0';
        return VFS_OK;
    }

    /* /proc/<pid>/exe (or /proc/self/exe after substitution) */
    char normbuf[ABI_PATH_MAX];
    subst_self(path, normbuf, sizeof(normbuf));
    const char *rel = normbuf + 1;
    if (rel[0] >= '0' && rel[0] <= '9') {
        const char *s = rel;
        while (*s >= '0' && *s <= '9') s++;
        if (*s == '/' && strcmp(s + 1, "exe") == 0) {
            int pid = parse_int(rel);
            struct proc *p = proc_lookup(pid);
            if (!p) return VFS_ERR_NOENT;
            const char *tgt = p->exe_path[0] ? p->exe_path : p->name;
            size_t tl = strlen(tgt);
            if (tl >= bufsz) tl = bufsz - 1;
            memcpy(buf, tgt, tl);
            buf[tl] = '\0';
            return VFS_OK;
        }
    }
    return VFS_ERR_NOENT;
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
    .readlink = procfs_readlink,
    .umount   = 0,
};

void procfs_init(void) {
    int rc = vfs_mount("/proc", &procfs_ops, 0);
    if (rc != VFS_OK) {
        kprintf("[procfs] mount failed: %s\n", vfs_strerror(rc));
        return;
    }

    /* B20: /proc/self is resolved DYNAMICALLY by procfs_readlink +
     * subst_self() (every op rewrites a leading "self" component to the
     * CALLING process's pid). We deliberately do NOT register a static
     * symlink-table entry -- the old code pinned /proc/self at the boot
     * pid, so /proc/self/* read the wrong process from every later
     * caller. */
    kprintf("[procfs] mounted at /proc (self/exe/maps/stat/cpuinfo)\n");
}
