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
#include <tobyos/file.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pit.h>
#include <tobyos/pmm.h>
#include <tobyos/smp.h>
#include <tobyos/nsproxy.h>   /* the /proc/PID/ns tree (slice 8) */
#include <tobyos/net.h>       /* /proc/net tables (2026-08-22) */
#include <tobyos/hwinfo.h>    /* real CPUID vendor/brand for cpuinfo */
#include <tobyos/perf.h>      /* boot_id derivation */

/* Describe an open file for /proc/<pid>/fd/<n> readlink: a real path where we
 * have one, else the Linux-style synthetic target (pipe:[..], socket:[..],
 * /dev/console, /dev/pts, anon_inode:...). */
static void describe_fd_target(struct file *f, char *out, size_t cap) {
    const char *s = "anon_inode:[unknown]";
    /* The stamped opening path wins for every kind that has one (2026-08-22).
     * Before open_path existed the VFS arm answered "/", which broke every
     * consumer that round-trips a descriptor through its /proc link -- most
     * importantly glibc's fexecve fallback, execve("/proc/self/fd/N"). */
    if (f->open_path) {
        size_t sl = strlen(f->open_path);
        if (sl >= cap) sl = cap - 1;
        memcpy(out, f->open_path, sl);
        out[sl] = '\0';
        return;
    }
    switch (f->kind) {
    case FILE_KIND_CONSOLE:     s = "/dev/console";          break;
    case FILE_KIND_PTY_MASTER:  s = "/dev/ptmx";             break;
    case FILE_KIND_PTY_SLAVE:   s = "/dev/pts/0";            break;
    case FILE_KIND_PIPE_R:      s = "pipe:[r]";              break;
    case FILE_KIND_PIPE_W:      s = "pipe:[w]";              break;
    case FILE_KIND_SOCKET:      s = "socket:[0]";            break;
    case FILE_KIND_WINDOW:      s = "anon_inode:[window]";   break;
    case FILE_KIND_EPOLL:       s = "anon_inode:[eventpoll]";break;
    case FILE_KIND_DIR:         s = (f->dirpath ? f->dirpath : "/");  break;
    case FILE_KIND_VFS:         s = "/";                     break;
    default:                    s = "anon_inode:[unknown]";  break;
    }
    size_t sl = strlen(s);
    if (sl >= cap) sl = cap - 1;
    memcpy(out, s, sl);
    out[sl] = '\0';
}

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
        /* Slice 10: substitute the pid AS THE CALLER NUMBERS IT. Every numeric
         * component below is now translated from the caller's namespace, so
         * writing the raw kpid here would send it through that translation a
         * second time and resolve to the wrong process (or to nothing). The two
         * have to agree; identity in the initial namespace. */
        int pid = p ? pid_vnr(p) : 0;
        if (p && !pid) pid = p->pid;
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

/* Slice 10: every numeric component of a /proc path is a pid IN THE READER'S
 * PID NAMESPACE. Translate it here, once, and treat "not visible from the
 * reader's namespace" as ENOENT -- which is not an approximation: from inside
 * that namespace the process genuinely does not exist. Identity in the initial
 * namespace, so this is a no-op for a system that never used CLONE_NEWPID.
 *
 * Returns a kpid suitable for proc_lookup(), or 0. */
static int procfs_kpid(const char *s) {
    int v = parse_int(s);
    struct proc *me = current_proc();
    /* A reader in the INITIAL namespace gets the identity mapping verbatim,
     * including /proc/0 -- which this kernel has always exposed even though
     * Linux has no pid 0. Routing it through pid_knr() would have quietly
     * dropped it (pid_vnr treats 0 as "not visible"), and removing /proc/0 is
     * an unrelated behaviour change that a pid-namespace slice has no business
     * making. Inside a namespace, translate and let 0 mean ENOENT. */
    if (!me || !me->pid_ns) return v;
    if (v <= 0) return 0;
    return pid_knr(v);
}

/* ---- content generators ---- */

/* /proc/uptime -- Linux emits two space-separated floats: total uptime and
 * cumulative idle time, both in seconds. We don't track idle separately, so
 * the second field mirrors the first (tools like busybox `uptime` parse the
 * first field only). */
static int gen_uptime(char *buf, size_t cap) {
    uint64_t ticks = pit_ticks();
    uint32_t hz    = pit_hz();
    uint64_t secs  = (hz > 0) ? ticks / hz : 0;
    uint64_t frac  = (hz > 0) ? ((ticks % hz) * 100) / hz : 0;
    int off = 0;
    char tmp[24];
    #define APPEND_STR(s) do { size_t sl = strlen(s); \
        if ((size_t)off + sl >= cap) return off; \
        memcpy(buf + off, s, sl); off += (int)sl; } while (0)
    #define APPEND_UINT(v) do { uint_to_str(tmp, sizeof(tmp), (uint64_t)(v)); APPEND_STR(tmp); } while (0)
    #define APPEND_FRAC2(v) do { char f[3]; f[0] = (char)('0' + (int)(((v)/10)%10)); \
        f[1] = (char)('0' + (int)((v)%10)); f[2] = 0; APPEND_STR(f); } while (0)
    APPEND_UINT(secs); APPEND_STR("."); APPEND_FRAC2(frac);
    APPEND_STR(" ");
    APPEND_UINT(secs); APPEND_STR(".00\n");
    buf[off] = '\0';
    return off;
    #undef APPEND_STR
    #undef APPEND_UINT
    #undef APPEND_FRAC2
}

/* /proc/meminfo -- Linux format: labelled fields in kB with a "kB" suffix.
 * `free`/`top`/sysconf parse MemTotal/MemFree/MemAvailable. kB = pages*4. */
static int gen_meminfo(char *buf, size_t cap) {
    uint64_t total = (uint64_t)pmm_total_pages() * 4;   /* pages*4096/1024 */
    uint64_t fr    = (uint64_t)pmm_free_pages()  * 4;
    char tmp[24];
    int off = 0;

    #define APPEND_STR(s) do { \
        size_t sl = strlen(s); \
        if ((size_t)off + sl >= cap) return off; \
        memcpy(buf + off, s, sl); off += (int)sl; \
    } while (0)
    #define APPEND_KB(v) do { \
        uint_to_str(tmp, sizeof(tmp), (uint64_t)(v)); \
        APPEND_STR(tmp); APPEND_STR(" kB"); \
    } while (0)

    APPEND_STR("MemTotal:     "); APPEND_KB(total); APPEND_STR("\n");
    APPEND_STR("MemFree:      "); APPEND_KB(fr);    APPEND_STR("\n");
    APPEND_STR("MemAvailable: "); APPEND_KB(fr);    APPEND_STR("\n");
    APPEND_STR("Buffers:      "); APPEND_KB(0);     APPEND_STR("\n");
    APPEND_STR("Cached:       "); APPEND_KB(0);     APPEND_STR("\n");
    APPEND_STR("SwapTotal:    "); APPEND_KB(0);     APPEND_STR("\n");
    APPEND_STR("SwapFree:     "); APPEND_KB(0);     APPEND_STR("\n");

    /* Kernel heap, which Linux does not report here and we do.
     *
     * MemFree alone cannot tell a LEAK from FRAGMENTATION, and those need
     * opposite fixes. HeapUsed climbing means unbalanced kmalloc/kfree;
     * HeapUsed flat while HeapTotal climbs means the allocator is holding
     * arenas it cannot reuse. The shell conformance gate loses ~620 KiB per
     * process spawn and reading MemFree for an hour could not distinguish
     * the two. Allocs/Frees make an imbalance countable directly. */
    {
        struct heap_stats hs;
        heap_stats(&hs);
        APPEND_STR("HeapTotal:    "); APPEND_KB(hs.total_bytes / 1024); APPEND_STR("\n");
        APPEND_STR("HeapUsed:     "); APPEND_KB(hs.used_bytes  / 1024); APPEND_STR("\n");
        APPEND_STR("HeapArenas:   "); APPEND_KB(hs.arenas);             APPEND_STR("\n");
        APPEND_STR("HeapAllocs:   "); APPEND_KB(hs.alloc_count);        APPEND_STR("\n");
        APPEND_STR("HeapFrees:    "); APPEND_KB(hs.free_count);         APPEND_STR("\n");
    }
    buf[off] = '\0';
    return off;

    #undef APPEND_STR
    #undef APPEND_KB
}

/* Count live (non-UNUSED) processes. */
static int live_proc_count(void) {
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proc[i].state != PROC_UNUSED &&
            g_proc[i].state != PROC_EMBRYO) n++;
    return n;
}

/* /proc/loadavg -- "0.00 0.00 0.00 <running>/<total> <lastpid>". We don't
 * compute real load averages; the running/total + lastpid fields are real. */
static int gen_loadavg(char *buf, size_t cap) {
    int total = live_proc_count();
    int lastpid = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proc[i].state != PROC_UNUSED &&
            g_proc[i].state != PROC_EMBRYO && g_proc[i].pid > lastpid)
            lastpid = g_proc[i].pid;
    int off = 0;
    char tmp[24];
    #define APPEND_STR(s) do { size_t sl = strlen(s); \
        if ((size_t)off + sl >= cap) return off; \
        memcpy(buf + off, s, sl); off += (int)sl; } while (0)
    #define APPEND_INT(v) do { int_to_str(tmp, sizeof(tmp), (int64_t)(v)); APPEND_STR(tmp); } while (0)
    APPEND_STR("0.00 0.00 0.00 1/");
    APPEND_INT(total); APPEND_STR(" "); APPEND_INT(lastpid); APPEND_STR("\n");
    buf[off] = '\0';
    return off;
    #undef APPEND_STR
    #undef APPEND_INT
}

/* /proc/stat -- the aggregate "cpu" line + per-cpu lines + a few counters
 * tools parse (btime, processes, procs_running). Jiffy counters are zero. */
static int gen_stat(char *buf, size_t cap) {
    int off = 0;
    char tmp[24];
    #define APPEND_STR(s) do { size_t sl = strlen(s); \
        if ((size_t)off + sl >= cap) return off; \
        memcpy(buf + off, s, sl); off += (int)sl; } while (0)
    #define APPEND_INT(v) do { int_to_str(tmp, sizeof(tmp), (int64_t)(v)); APPEND_STR(tmp); } while (0)
    uint32_t ncpu = smp_cpu_count();
    if (ncpu == 0) ncpu = 1;
    APPEND_STR("cpu  0 0 0 0 0 0 0 0 0 0\n");
    for (uint32_t i = 0; i < ncpu; i++) {
        APPEND_STR("cpu"); APPEND_INT(i);
        APPEND_STR(" 0 0 0 0 0 0 0 0 0 0\n");
    }
    APPEND_STR("intr 0\n");
    APPEND_STR("ctxt 0\n");
    APPEND_STR("btime 0\n");
    APPEND_STR("processes "); APPEND_INT(live_proc_count()); APPEND_STR("\n");
    APPEND_STR("procs_running 1\n");
    APPEND_STR("procs_blocked 0\n");
    buf[off] = '\0';
    return off;
    #undef APPEND_STR
    #undef APPEND_INT
}

/* /proc/mounts -- one line per VFS mount: "<dev> <point> <type> <opts> 0 0".
 * Built by walking the mount table. */
struct mounts_ctx { char *buf; int off; size_t cap; };
static bool mounts_cb(const char *point, const struct vfs_ops *ops,
                      void *mnt, void *cookie) {
    (void)ops; (void)mnt;
    struct mounts_ctx *c = (struct mounts_ctx *)cookie;
    #define MAPP(s) do { size_t sl = strlen(s); \
        if ((size_t)c->off + sl >= c->cap) return false; \
        memcpy(c->buf + c->off, s, sl); c->off += (int)sl; } while (0)
    MAPP("none "); MAPP(point); MAPP(" tobyos rw,relatime 0 0\n");
    #undef MAPP
    return true;
}
static int gen_mounts(char *buf, size_t cap) {
    struct mounts_ctx c = { buf, 0, cap };
    vfs_iter_mounts(mounts_cb, &c);
    buf[c.off] = '\0';
    return c.off;
}

static int gen_version(char *buf, size_t cap) {
    const char *v = "tobyOS 1.0 (x86_64)\n";
    size_t vl = strlen(v);
    if (vl >= cap) vl = cap - 1;
    memcpy(buf, v, vl);
    buf[vl] = '\0';
    return (int)vl;
}


/* Linux slice 6: /proc/filesystems.
 *
 * mount(8), busybox's mount applet and most distro init scripts read this to
 * decide what they may mount; an absent file makes them assume NOTHING is
 * supported. The "nodev" column means "needs no block device". */
static int gen_filesystems(char *buf, size_t cap) {
    static const char *txt =
        "nodev\tramfs\n"
        "nodev\tproc\n"
        "nodev\tsysfs\n"
        "nodev\tcgroup2\n"
        "nodev\ttmpfs\n"
        "\ttobyfs\n"
        "\text2\n"
        "\text4\n"
        "\tvfat\n";
    size_t n = strlen(txt);
    if (n >= cap) n = cap - 1;
    memcpy(buf, txt, n); buf[n] = 0;
    return (int)n;
}

/* Linux slice 6: /proc/devices. Reported so tools that enumerate device
 * majors get a plausible, non-empty answer rather than concluding the system
 * has no devices at all. */
static int gen_devices(char *buf, size_t cap) {
    static const char *txt =
        "Character devices:\n"
        "  1 mem\n"
        "  4 tty\n"
        "  5 /dev/tty\n"
        " 13 input\n"
        " 29 fb\n"
        "116 alsa\n"
        "226 drm\n"
        "\nBlock devices:\n"
        "  8 sd\n"
        "259 blkext\n";
    size_t n = strlen(txt);
    if (n >= cap) n = cap - 1;
    memcpy(buf, txt, n); buf[n] = 0;
    return (int)n;
}

/* Linux slice 6 NOTE: /proc/<pid>/environ is deliberately absent.
 *
 * The environment is not kernel state here. `envc`/`envp` live on struct
 * proc_spec (the spawn request) and are deep-copied onto the new process's
 * USER STACK by pack_argv_envp_on_user_stack(); the PCB keeps no pointer to
 * them afterwards. Generating this file would mean recording that stack
 * address at exec and then reading another address space (a CR3 switch) to
 * serve a read.
 *
 * Not built because nothing needs it: `ps e` and a few sandbox probes read it,
 * none of which are on the path to the Alpine milestone. An absent file is an
 * honest ENOENT; a present but empty one would read as "this process has no
 * environment", which is false. */

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
    /* Slice 10: report pids as the READER numbers them, not as the kernel
     * stores them. A process inside a pid namespace reading /proc must see its
     * own numbering, or `ps` shows pids that kill(2) then rejects. Identity in
     * the initial namespace. PPid falls to 0 when the parent is outside the
     * reader's namespace, which is what Linux reports and is also the right
     * answer -- the alternative leaks a host pid across the boundary. */
    /* Tgid (2026-08-22): what ps/pgrep/debuggers group threads by --
     * absent until now. For a thread it is the leader's (translated) pid. */
    {   int tg = p->is_thread ? p->tgid : p->pid;
        struct proc *ld = proc_lookup(tg);
        APPEND_STR("Tgid:   "); APPEND_INT(ld ? pid_vnr(ld) : tg);
        APPEND_STR("\n");
    }
    APPEND_STR("Pid:    "); APPEND_INT(pid_vnr(p));            APPEND_STR("\n");
    APPEND_STR("PPid:   "); APPEND_INT(pid_vnr_of_kpid(p->ppid)); APPEND_STR("\n");
    /* VmSize/VmRSS in kB from the maintained user-page counter (real since
     * the slice-16 fix; before that it was a spawn-time constant). */
    APPEND_STR("VmSize:\t"); APPEND_INT((int64_t)p->user_pages * 4); APPEND_STR(" kB\n");
    APPEND_STR("VmRSS:\t");  APPEND_INT((int64_t)p->user_pages * 4); APPEND_STR(" kB\n");
    /* Threads: live group members incl. the leader. */
    {   extern struct proc g_proc[];
        int tg = p->is_thread ? p->tgid : p->pid, nth = 0;
        for (int ti = 0; ti < PROC_MAX; ti++) {
            struct proc *q = &g_proc[ti];
            if (q->state == PROC_UNUSED || q->state == PROC_EMBRYO ||
                q->state == PROC_TERMINATED) continue;
            if (q->pid == tg || (q->is_thread && q->tgid == tg)) nth++;
        }
        APPEND_STR("Threads:\t"); APPEND_INT(nth); APPEND_STR("\n");
    }
    /* tobyOS extension (2026-08-23): the process's kernel-counted syscall
     * total. Exists so userspace can PROVE a code path stopped syscalling
     * -- the vDSO gate hammers clock_gettime and asserts this barely
     * moves. Linux has no equivalent line; the Toby prefix keeps parsers
     * of standard fields honest. */
    APPEND_STR("TobySyscalls:\t");
    APPEND_INT((int64_t)p->syscall_count);
    APPEND_STR("\n");
    /* Signal masks, 16-digit hex in LINUX numbering (>>1 off the tobyOS
     * bit-per-signal convention). SigIgn/SigCgt are derived by scanning
     * the thread-group action table. */
    {   uint64_t ign = 0, cgt = 0;
        struct sigaction *acts = sig_actions_of(p);
        for (int si = 1; si < SIG_MAX; si++) {
            if (!acts) break;
            if (acts[si].sa_handler == SIG_IGN) ign |= SIGMASK(si);
            else if (acts[si].sa_handler != SIG_DFL) cgt |= SIGMASK(si);
        }
        #define APPEND_SIGHEX(name, v) do { \
            char hx[20]; hex_to_str(hx, sizeof hx, (uint64_t)(v) >> 1); \
            APPEND_STR(name); APPEND_STR(hx); APPEND_STR("\n"); } while (0)
        APPEND_SIGHEX("SigPnd:\t", p->pending_signals);
        APPEND_SIGHEX("SigBlk:\t", p->sigstate.mask);
        APPEND_SIGHEX("SigIgn:\t", ign);
        APPEND_SIGHEX("SigCgt:\t", cgt);
        #undef APPEND_SIGHEX
    }
    /* Linux slice 2: report the full credential set. Linux's format is four
     * TAB-separated columns -- real, effective, saved-set, filesystem -- and
     * `ps`, `id` and every sandbox probe parse exactly that shape. We had a
     * single value, which is the same class of bug as the truncated stat line
     * (see gen_pid_stat): a short line does not read as an error, it reads as
     * a wrong answer. fsuid has no separate storage here, so it mirrors
     * effective, which is true for every process we can create. */
    /* Slice 11: report ids as the READER's user namespace numbers them -- the
     * same rule as the pid fields above. `id`, `ps` and every sandbox probe read
     * these, so a host id here would contradict what getuid(2) told the same
     * process. */
    #define APPEND_UID(v) APPEND_INT(userns_cur_uid((uint32_t)(v)))
    #define APPEND_GID(v) APPEND_INT(userns_cur_gid((uint32_t)(v)))
    APPEND_STR("Uid:\t"); APPEND_UID(p->ruid); APPEND_STR("\t");
    APPEND_UID(p->uid);  APPEND_STR("\t");
    APPEND_UID(p->suid); APPEND_STR("\t");
    APPEND_UID(p->uid);  APPEND_STR("\n");
    APPEND_STR("Gid:\t"); APPEND_GID(p->rgid); APPEND_STR("\t");
    APPEND_GID(p->gid);  APPEND_STR("\t");
    APPEND_GID(p->sgid); APPEND_STR("\t");
    APPEND_GID(p->gid);  APPEND_STR("\n");
    APPEND_STR("Groups:\t");
    for (int gi = 0; gi < p->ngroups; gi++) {
        APPEND_GID(p->groups[gi]); APPEND_STR(" ");
    }
    #undef APPEND_UID
    #undef APPEND_GID
    APPEND_STR("\n");
    buf[off] = '\0';
    return off;

    #undef APPEND_STR
    #undef APPEND_INT
}

static int gen_pid_cmdline(int pid, char *buf, size_t cap) {
    struct proc *p = proc_lookup(pid);
    if (!p) return -1;
    /* 2026-08-22: the REAL argv, NUL-separated, exactly as Linux emits it
     * -- the old name+'\n' fabrication is not the format ps/pgrep/argv
     * readers parse (they split on NULs and got one token plus a stray
     * newline). Kernel procs have no recorded argv; fall back to the name
     * with a terminating NUL, which is at least the right SHAPE. */
    if (p->cmdline_len) {
        size_t nl = p->cmdline_len;
        if (nl > cap) nl = cap;
        memcpy(buf, p->cmdline, nl);
        return (int)nl;
    }
    size_t nl = strlen(p->name);
    if (nl >= cap) nl = cap - 1;
    memcpy(buf, p->name, nl);
    buf[nl] = '\0';
    return (int)(nl + 1);
}

/* /proc/<pid>/fdinfo/<n>.
 *
 * Chromium's sandbox is what made this load-bearing: ChrootToSafeEmptyDir()
 * chroots into /proc/self/fdinfo/ precisely because it is a directory whose
 * contents are exactly the caller's own open descriptors -- so after closing
 * them it is guaranteed EMPTY, and no path in it can be reopened. Without the
 * directory the sandbox dies at credentials.cc:118.
 *
 * FIELDS ARE REPORTED ONLY WHERE THEY ARE REAL. Linux prints pos/flags/mnt_id
 * for every fd and then adds kind-specific lines (epoll, inotify, signalfd...),
 * so the field SET already varies by descriptor and a key-based reader copes
 * with an absent key. `mnt_id` is therefore omitted rather than reported as 0:
 * this kernel tracks no per-descriptor mount id, and a plausible-looking
 * constant is exactly the kind of fabrication this arc keeps having to undo.
 * `pos` is only meaningful for a VFS-backed handle; a pipe or socket has no
 * file position, and Linux reports 0 for those too. */
static int gen_pid_fdinfo(int pid, int fd, char *buf, size_t cap) {
    struct proc *p = proc_lookup(pid);
    if (!p) return -1;
    struct file **tab = proc_fds(p);
    if (!tab || fd < 0 || fd >= PROC_NFDS || !tab[fd]) return -1;
    struct file *f = tab[fd];

    uint64_t pos = (f->kind == FILE_KIND_VFS) ? (uint64_t)file_pos_get(f) : 0;
    /* Linux prints the open flags in OCTAL with a leading 0. We keep only the
     * access mode on the descriptor, so that is what goes out -- the value is
     * right as far as it goes, and no bit is invented to pad it. */
    unsigned flags = (unsigned)f->o_accmode;

    int off = 0;
    char tmp[24];
    #define FD_STR(s) do { size_t sl = strlen(s); \
        if ((size_t)off + sl >= cap) return off; \
        memcpy(buf + off, s, sl); off += (int)sl; } while (0)

    FD_STR("pos:\t");
    int_to_str(tmp, sizeof(tmp), (int64_t)pos);
    FD_STR(tmp);
    FD_STR("\nflags:\t0");
    {   /* octal, by hand: kprintf/int_to_str are decimal only */
        char oct[16]; int oi = 0;
        unsigned v = flags;
        if (v == 0) oct[oi++] = '0';
        while (v) { oct[oi++] = (char)('0' + (v & 7)); v >>= 3; }
        while (oi) { if ((size_t)off + 1 >= cap) return off;
                     buf[off++] = oct[--oi]; }
    }
    FD_STR("\n");
    buf[off] = '\0';
    return off;
    #undef FD_STR
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

    /* Report the REAL CPU (2026-08-23). This used to answer
     * "GenuineTobyOS" / "tobyOS virtual x86-64 CPU" for every core --
     * invented strings, while hwinfo had the genuine CPUID vendor and
     * brand cached since boot. Anything that identifies the machine from
     * /proc/cpuinfo (lscpu, build scripts, JIT feature probes) was being
     * told a fiction it could not act on. */
    const struct abi_hwinfo_summary *hw = hwinfo_current();
    const char *vendor = (hw && hw->cpu_vendor[0]) ? hw->cpu_vendor
                                                   : "Unknown";
    const char *brand  = (hw && hw->cpu_brand[0])  ? hw->cpu_brand
                                                   : "x86-64";

    uint32_t ncpu = smp_cpu_count();
    if (ncpu == 0) ncpu = 1;
    for (uint32_t i = 0; i < ncpu; i++) {
        APPEND_STR("processor\t: "); APPEND_INT(i); APPEND_STR("\n");
        APPEND_STR("vendor_id\t: "); APPEND_STR(vendor); APPEND_STR("\n");
        APPEND_STR("cpu family\t: "); APPEND_INT(hw ? hw->cpu_family : 6);
        APPEND_STR("\n");
        APPEND_STR("model\t\t: "); APPEND_INT(hw ? hw->cpu_model : 0);
        APPEND_STR("\n");
        APPEND_STR("model name\t: "); APPEND_STR(brand); APPEND_STR("\n");
        APPEND_STR("stepping\t: "); APPEND_INT(hw ? hw->cpu_stepping : 0);
        APPEND_STR("\n");
        APPEND_STR("siblings\t: "); APPEND_INT(ncpu); APPEND_STR("\n");
        APPEND_STR("core id\t\t: "); APPEND_INT(i); APPEND_STR("\n");
        APPEND_STR("cpu cores\t: "); APPEND_INT(ncpu); APPEND_STR("\n");
        APPEND_STR("flags\t\t: fpu tsc msr sse sse2 syscall\n");
        APPEND_STR("\n");
    }
    buf[off] = '\0';
    return off;
    #undef APPEND_STR
    #undef APPEND_INT
}

/* Fixed-width uppercase hex, the /proc/net table format. */
static void net_hex32(char *out, uint32_t v) {
    static const char *h = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) { out[i] = h[v & 0xF]; v >>= 4; }
    out[8] = 0;
}
static void net_hex16(char *out, uint16_t v) {
    static const char *h = "0123456789ABCDEF";
    for (int i = 3; i >= 0; i--) { out[i] = h[v & 0xF]; v >>= 4; }
    out[4] = 0;
}

/* /proc/sys -- the sysctls real software actually reads (2026-08-22;
 * there was NO /proc/sys at all). Values are REAL or the file is absent:
 * a sysctl that reads back a number nothing enforces is the
 * accept-and-ignore lie in file form. */
static int gen_sys(const char *rest, char *buf, size_t cap) {
    #define SYS_OUT(s) do { \
        size_t sl = strlen(s); if (sl >= cap) sl = cap - 1; \
        memcpy(buf, s, sl); buf[sl] = 0; return (int)sl; } while (0)
    if (strcmp(rest, "kernel/ostype") == 0)    SYS_OUT("Linux\n");
    if (strcmp(rest, "kernel/osrelease") == 0) SYS_OUT("6.1.0-tobyos\n");
    if (strcmp(rest, "kernel/hostname") == 0) {
        const char *hn = uts_nodename();
        char t[80]; size_t l = strlen(hn); if (l > 76) l = 76;
        memcpy(t, hn, l); t[l] = '\n'; t[l + 1] = 0;
        SYS_OUT(t);
    }
    if (strcmp(rest, "kernel/pid_max") == 0)   SYS_OUT("256\n");
    if (strcmp(rest, "fs/file-max") == 0)      SYS_OUT("1024\n");
    if (strcmp(rest, "vm/overcommit_memory") == 0) SYS_OUT("0\n");
    if (strcmp(rest, "kernel/random/boot_id") == 0) {
        /* Unique per boot (the contract); derived once from the clock. */
        static char uuid[40];
        if (!uuid[0]) {
            uint64_t a = perf_now_ns() * 0x9E3779B97F4A7C15ull;
            uint64_t b = (a ^ (a >> 29)) * 0xBF58476D1CE4E5B9ull;
            static const char *h = "0123456789abcdef";
            int gi = 0;
            for (int i = 0; i < 36; i++) {
                if (i == 8 || i == 13 || i == 18 || i == 23) { uuid[i] = '-'; continue; }
                uint64_t src = (gi < 16) ? a : b;
                uuid[i] = h[(src >> ((gi % 16) * 4)) & 0xF];
                gi++;
            }
            uuid[36] = '\n'; uuid[37] = 0;
        }
        SYS_OUT(uuid);
    }
    return -1;
    #undef SYS_OUT
}

/* /proc/net -- the tables ifconfig/netstat/ss read (2026-08-22; there was
 * NO /proc/net at all, so every one of them was blind). Rows come from
 * the REAL conn/socket pools via read-only snapshots. */
static int gen_net(const char *rest, char *buf, size_t cap) {
    int off = 0;
    char h32[9], h16[5];
    #define NET_STR(s) do { \
        size_t sl = strlen(s); \
        if ((size_t)off + sl >= cap) { buf[off] = 0; return off; } \
        memcpy(buf + off, s, sl); off += (int)sl; } while (0)

    if (strcmp(rest, "dev") == 0) {
        NET_STR("Inter-|   Receive                            "
                "                    |  Transmit\n");
        NET_STR(" face |bytes    packets errs drop fifo frame "
                "compressed multicast|bytes    packets errs "
                "drop fifo colls carrier compressed\n");
        NET_STR("    lo:       0       0    0    0    0     0          0 "
                "        0        0       0    0    0    0     0       0          0\n");
        NET_STR("  eth0:       0       0    0    0    0     0          0 "
                "        0        0       0    0    0    0     0       0          0\n");
        buf[off] = 0; return off;
    }
    if (strcmp(rest, "tcp") == 0) {
        extern int tcp_conn_snapshot(int, uint32_t *, uint16_t *,
                                     uint32_t *, uint16_t *, int *);
        NET_STR("  sl  local_address rem_address   st tx_queue rx_queue "
                "tr tm->when retrnsmt   uid  timeout inode\n");
        for (int i = 0; ; i++) {
            uint32_t lip, rip; uint16_t lp, rp; int st;
            int rc = tcp_conn_snapshot(i, &lip, &lp, &rip, &rp, &st);
            if (rc == -2) break;
            if (rc != 0) continue;
            char row[128]; int ro = 0;
            #define ROW(s) do { size_t l2 = strlen(s); \
                memcpy(row + ro, s, l2); ro += (int)l2; } while (0)
            ROW("   "); { char ix[8]; int_to_str(ix, sizeof ix, i); ROW(ix); }
            ROW(": ");
            net_hex32(h32, lip); ROW(h32); ROW(":");
            net_hex16(h16, ntohs(lp)); ROW(h16); ROW(" ");
            net_hex32(h32, rip); ROW(h32); ROW(":");
            net_hex16(h16, ntohs(rp)); ROW(h16); ROW(" ");
            { char stx[3]; static const char *hx = "0123456789ABCDEF";
              stx[0] = hx[(st >> 4) & 0xF]; stx[1] = hx[st & 0xF]; stx[2] = 0;
              ROW(stx); }
            ROW(" 00000000:00000000 00:00000000 00000000     0        0 0\n");
            row[ro] = 0;
            NET_STR(row);
            #undef ROW
        }
        buf[off] = 0; return off;
    }
    if (strcmp(rest, "udp") == 0 || strcmp(rest, "unix") == 0) {
        extern int sock_snapshot(int, int *, uint16_t *, uint32_t *,
                                 uint16_t *, const char **);
        bool want_udp = (rest[1] == 'd');
        NET_STR(want_udp
            ? "  sl  local_address rem_address   st tx_queue rx_queue "
              "tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n"
            : "Num       RefCount Protocol Flags    Type St Inode Path\n");
        for (int i = 0; ; i++) {
            int kind; uint16_t lp, pp; uint32_t pip; const char *ux;
            int rc = sock_snapshot(i, &kind, &lp, &pip, &pp, &ux);
            if (rc == -2) break;
            if (rc != 0) continue;
            char row[160]; int ro = 0;
            #define ROW(s) do { size_t l2 = strlen(s); \
                memcpy(row + ro, s, l2); ro += (int)l2; } while (0)
            if (want_udp && kind == 1 /* SOCK_KIND_UDP */ && lp) {
                ROW("   "); { char ix[8]; int_to_str(ix, sizeof ix, i); ROW(ix); }
                ROW(": ");
                net_hex32(h32, 0); ROW(h32); ROW(":");
                net_hex16(h16, ntohs(lp)); ROW(h16); ROW(" ");
                net_hex32(h32, pip); ROW(h32); ROW(":");
                net_hex16(h16, ntohs(pp)); ROW(h16);
                ROW(" 07 00000000:00000000 00:00000000 00000000     0        0 0 0 0 0\n");
            } else if (!want_udp && kind == 3 /* SOCK_KIND_UNIX */ && ux) {
                { char ix[20]; hex_to_str(ix, sizeof ix, (uint64_t)i); ROW(ix); }
                ROW(": 00000002 00000000 00000000 0001 01     0 ");
                ROW(ux); ROW("\n");
            } else { continue; }
            row[ro] = 0;
            NET_STR(row);
            #undef ROW
        }
        buf[off] = 0; return off;
    }
    if (strcmp(rest, "route") == 0) {
        NET_STR("Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\t"
                "Mask\t\tMTU\tWindow\tIRTT\n");
        NET_STR("eth0\t00000000\t");
        net_hex32(h32, net_my_gateway()); NET_STR(h32);
        NET_STR("\t0003\t0\t0\t0\t00000000\t0\t0\t0\n");
        buf[off] = 0; return off;
    }
    return -1;
    #undef NET_STR
}

/* /proc/<pid>/maps -- the REAL region table (2026-08-22). The previous
 * version synthesised exactly three lines (one exe page, heap, stack) and
 * listed no mmap regions at all -- anything that walked its own address
 * space (crash handlers, sanitizers, JIT probes) read fiction. Now: every
 * entry in mmap.c's per-process VMA table, plus the exe page, the
 * Linux-personality brk heap and the stack, sorted ascending as the format
 * requires. The 2048-byte procfs generation buffer bounds the output; a
 * table that does not fit ends with an explicit truncation line rather
 * than pretending to be complete. */
#define PMAPS_MAX 96
struct pmaps_ent { uint64_t lo, hi; char perms[5]; const char *tag; };

static int gen_pid_maps(int pid, char *buf, size_t cap) {
    struct proc *p = proc_lookup(pid);
    if (!p) return -1;
    extern int  mmap_vma_snapshot(struct proc *, int, uint64_t *, uint64_t *,
                                  uint32_t *, uint32_t *);
    extern void mmap_brk_range(struct proc *, uint64_t *, uint64_t *);

    struct pmaps_ent e[PMAPS_MAX];
    int n = 0, dropped = 0;

    if (p->user_entry) {
        uint64_t lo = p->user_entry & ~0xfffULL;
        e[n++] = (struct pmaps_ent){ lo, lo + 0x1000ULL, "r-xp",
                                     p->exe_path[0] ? p->exe_path : p->name };
    }
    {   uint64_t bb = 0, bc = 0;
        mmap_brk_range(p, &bb, &bc);
        if (bc <= bb) { bb = p->brk_base; bc = p->brk_cur; } /* native brk */
        if (bc > bb && n < PMAPS_MAX)
            e[n++] = (struct pmaps_ent){ bb, bc, "rw-p", "[heap]" };
    }
    if (p->user_stack_base && p->user_stack_pages && n < PMAPS_MAX) {
        uint64_t slo = p->user_stack_base;
        e[n++] = (struct pmaps_ent){ slo,
            slo + (uint64_t)p->user_stack_pages * 4096ULL, "rw-p", "[stack]" };
    }
    {   int cnt = mmap_vma_snapshot(p, -1, 0, 0, 0, 0);
        for (int i = 0; i < cnt; i++) {
            uint64_t lo, hi; uint32_t prot, fl;
            if (mmap_vma_snapshot(p, i, &lo, &hi, &prot, &fl) != 0) continue;
            if (n >= PMAPS_MAX) { dropped++; continue; }
            struct pmaps_ent *x = &e[n++];
            x->lo = lo; x->hi = hi;
            x->perms[0] = (prot & 0x1) ? 'r' : '-';
            x->perms[1] = (prot & 0x2) ? 'w' : '-';
            x->perms[2] = (prot & 0x4) ? 'x' : '-';
            x->perms[3] = (fl & 0x2 /* VMA_FLAG_SHARED */) ? 's' : 'p';
            x->perms[4] = 0;
            x->tag = "";
        }
    }
    /* Sort ascending (insertion; n <= 96 and reads are rare). */
    for (int i = 1; i < n; i++) {
        struct pmaps_ent k = e[i];
        int j = i - 1;
        while (j >= 0 && e[j].lo > k.lo) { e[j + 1] = e[j]; j--; }
        e[j + 1] = k;
    }

    int off = 0;
    char tmp[24];
    #define APPEND_STR(s) do { \
        size_t sl = strlen(s); \
        if ((size_t)off + sl >= cap) goto full; \
        memcpy(buf + off, s, sl); off += (int)sl; \
    } while (0)
    #define APPEND_HEX(v) do { hex_to_str(tmp, sizeof(tmp), (uint64_t)(v)); APPEND_STR(tmp); } while (0)
    for (int i = 0; i < n; i++) {
        APPEND_HEX(e[i].lo); APPEND_STR("-"); APPEND_HEX(e[i].hi);
        APPEND_STR(" "); APPEND_STR(e[i].perms);
        APPEND_STR(" 00000000 00:00 0 \t"); APPEND_STR(e[i].tag);
        APPEND_STR("\n");
    }
    if (dropped) {
        APPEND_STR("# [truncated: more regions than fit]\n");
    }
    buf[off] = '\0';
    return off;
full:
    /* The buffer filled mid-line: end at the last complete line. */
    while (off > 0 && buf[off - 1] != '\n') off--;
    buf[off] = '\0';
    return off;
    #undef APPEND_STR
    #undef APPEND_HEX
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

    /* Slice 10: fields 1 and 4 are pid and ppid -- namespace-translated for the
     * same reason as /proc/<pid>/status. busybox `ps` reads field 1 and then
     * offers it to kill(1). */
    APPEND_INT(pid_vnr(p)); APPEND_STR(" (");
    APPEND_STR(p->name); APPEND_STR(") ");
    { char ss[2] = { st, 0 }; APPEND_STR(ss); }
    APPEND_STR(" "); APPEND_INT(pid_vnr_of_kpid(p->ppid));

    /* Linux /proc/<pid>/stat has FIFTY-TWO fields. We used to stop at 22,
     * which crashed real programs -- not by returning an error, but by
     * running them off the end of the buffer.
     *
     * busybox `ps` reads vsize and rss, fields 23 and 24. It reaches them with
     * a hand-rolled skip_fields():
     *
     *     do { while (*str++ != ' ') continue;
     *          while (*str == ' ') str++;    } while (--count);
     *
     * which has NO NUL check and no bounds check. That is safe on Linux
     * precisely because the fields are always there; against our truncated
     * line it scanned past the end of ps's stack buffer until the pointer went
     * non-canonical, taking a #GP at rax=0x0000800000000001 (the first address
     * above the user half). Any /proc consumer that indexes a late field --
     * ps, top, procps, and anything Alpine ships -- hits the same wall.
     *
     * So: emit all 52, with honest values where we have them and 0 elsewhere.
     * Field numbers below are 1-based and match proc(5). */
    {
        uint64_t hz100 = p->cpu_ns / 10000000ull;   /* ns -> USER_HZ (100/s) */
        uint64_t rss_pages = p->user_pages;
        uint64_t vsize = rss_pages * 4096ull;

        APPEND_STR(" 0");                    /*  5 pgrp        */
        APPEND_STR(" 0");                    /*  6 session     */
        APPEND_STR(" 0");                    /*  7 tty_nr      */
        APPEND_STR(" -1");                   /*  8 tpgid       */
        APPEND_STR(" 0");                    /*  9 flags       */
        APPEND_STR(" 0");                    /* 10 minflt      */
        APPEND_STR(" 0");                    /* 11 cminflt     */
        APPEND_STR(" 0");                    /* 12 majflt      */
        APPEND_STR(" 0");                    /* 13 cmajflt     */
        APPEND_STR(" "); APPEND_INT(hz100);  /* 14 utime       */
        APPEND_STR(" 0");                    /* 15 stime       */
        APPEND_STR(" 0");                    /* 16 cutime      */
        APPEND_STR(" 0");                    /* 17 cstime      */
        APPEND_STR(" 20");                   /* 18 priority    */
        APPEND_STR(" 0");                    /* 19 nice        */
        APPEND_STR(" 1");                    /* 20 num_threads */
        APPEND_STR(" 0");                    /* 21 itrealvalue */
        APPEND_STR(" 0");                    /* 22 starttime   */
        APPEND_STR(" "); APPEND_INT(vsize);      /* 23 vsize   */
        APPEND_STR(" "); APPEND_INT(rss_pages);  /* 24 rss (in PAGES) */
        /* 25 rsslim: Linux reports RLIM_INFINITY here. */
        APPEND_STR(" 18446744073709551615");
        /* 26..52 -- startcode endcode startstack kstkesp kstkeip signal
         * blocked sigignore sigcatch wchan nswap cnswap exit_signal
         * processor rt_priority policy delayacct_blkio_ticks guest_time
         * cguest_time start_data end_data start_brk arg_start arg_end
         * env_start env_end exit_code = 27 fields. */
        for (int i = 0; i < 27; i++) APPEND_STR(" 0");
    }
    APPEND_STR("\n");
    buf[off] = '\0';
    return off;
    #undef APPEND_STR
    #undef APPEND_INT
}

/* ---- VFS driver ---- */

/* Slice 11: which writable procfs file a handle refers to (PW_NONE for the
 * read-only majority). Recorded at open because procfs_write() only receives
 * the handle, not the path. */
enum procfs_wkind { PW_NONE = 0, PW_UID_MAP, PW_GID_MAP, PW_SETGROUPS };

struct procfs_handle {
    char *data;
    size_t len;
    size_t pos;
    int    wpid;                 /* target pid for a writable file */
    enum procfs_wkind wkind;
};

static int procfs_open(void *mnt, const char *path, struct vfs_file *out) {
    (void)mnt;
    if (!path || path[0] != '/') return VFS_ERR_NOENT;

    char normbuf[ABI_PATH_MAX];
    subst_self(path, normbuf, sizeof(normbuf));
    const char *rel = normbuf + 1; /* skip leading '/' */

    char buf[2048];
    int len = -1;
    int wpid = 0;                          /* slice 11: writable-file target */
    enum procfs_wkind wkind = PW_NONE;

    if (strncmp(rel, "sys/", 4) == 0) { len = gen_sys(rel + 4, buf, sizeof(buf));
                                        if (len < 0) return VFS_ERR_NOENT; }
    else if (strncmp(rel, "net/", 4) == 0) { len = gen_net(rel + 4, buf, sizeof(buf));
                                        if (len < 0) return VFS_ERR_NOENT; }
    else if (strcmp(rel, "uptime") == 0)  { len = gen_uptime(buf, sizeof(buf));  }
    else if (strcmp(rel, "meminfo") == 0) { len = gen_meminfo(buf, sizeof(buf)); }
    else if (strcmp(rel, "version") == 0) { len = gen_version(buf, sizeof(buf)); }
    else if (strcmp(rel, "cpuinfo") == 0) { len = gen_cpuinfo(buf, sizeof(buf)); }
    else if (strcmp(rel, "loadavg") == 0) { len = gen_loadavg(buf, sizeof(buf)); }
    else if (strcmp(rel, "stat") == 0)    { len = gen_stat(buf, sizeof(buf)); }
    else if (strcmp(rel, "mounts") == 0)  { len = gen_mounts(buf, sizeof(buf)); }
    else if (strcmp(rel, "filesystems") == 0) { len = gen_filesystems(buf, sizeof(buf)); }
    else if (strcmp(rel, "devices") == 0) { len = gen_devices(buf, sizeof(buf)); }
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
                int pid = procfs_kpid(pid_str);      /* slice 10 */
                if (!pid) return VFS_ERR_NOENT;
                const char *sub = slash + 1;
                if (strcmp(sub, "status") == 0)
                    len = gen_pid_status(pid, buf, sizeof(buf));
                else if (strcmp(sub, "cmdline") == 0)
                    len = gen_pid_cmdline(pid, buf, sizeof(buf));
                else if (strcmp(sub, "maps") == 0)
                    len = gen_pid_maps(pid, buf, sizeof(buf));
                else if (strncmp(sub, "fdinfo/", 7) == 0) {
                    int fdn = parse_int(sub + 7);
                    len = gen_pid_fdinfo(pid, fdn, buf, sizeof(buf));
                    if (len < 0) return VFS_ERR_NOENT;
                }
                else if (strcmp(sub, "stat") == 0)
                    len = gen_pid_stat(pid, buf, sizeof(buf));
                /* 2026-08-22: /proc/self/mounts is what libmount, findmnt,
                 * systemd and Go's mountinfo actually read -- /proc/mounts
                 * worked while the per-pid dispatch had no arm, so exactly
                 * the common consumers got ENOENT. Same generator (it
                 * already renders the READER's mount namespace). */
                else if (strcmp(sub, "mounts") == 0)
                    len = gen_mounts(buf, sizeof(buf));
                /* Slice 11: the user-namespace id maps. These are the first
                 * WRITABLE files in /proc -- see procfs_write(). The pid is
                 * stashed on the handle because a write has to know which
                 * process's namespace it is configuring. */
                else if (strcmp(sub, "uid_map") == 0 ||
                         strcmp(sub, "gid_map") == 0 ||
                         strcmp(sub, "setgroups") == 0) {
                    struct proc *up = proc_lookup(pid);
                    if (!up) return VFS_ERR_NOENT;
                    if (sub[0] == 's')
                        len = userns_render_setgroups(up, buf, sizeof(buf));
                    else
                        len = userns_render_map(up, sub[0] == 'u', buf,
                                                sizeof(buf));
                    if (len < 0) len = 0;      /* empty map: a real, readable
                                                * state, not an error */
                    wpid = pid;
                    wkind = (sub[0] == 's') ? PW_SETGROUPS
                                            : (sub[0] == 'u' ? PW_UID_MAP
                                                             : PW_GID_MAP);
                }
                else if (strcmp(sub, "exe") == 0) {
                    /* Opening /proc/<pid>/exe FOLLOWS the symlink, as Linux does:
                     * hand back a handle to the real executable. stat/readlink
                     * already reported it as a symlink, but there was no open
                     * path, so open() returned ENOENT. Chromium's child-process
                     * launcher opens /proc/self/exe to re-exec itself, so every
                     * renderer/GPU/network child died with _exit(127) ("GPU
                     * process exited unexpectedly: exit_code=32512"). Many Linux
                     * programs re-exec this way, so this is a general gap. */
                    struct proc *ep = proc_lookup(pid);
                    if (ep && ep->exe_path[0])
                        return vfs_open(ep->exe_path, out);
                    return VFS_ERR_NOENT;
                }
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
    h->wpid  = wpid;
    h->wkind = wkind;

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

/* Slice 11: the FIRST write path in procfs. Everything in /proc was read-only
 * until user namespaces needed /proc/PID/{uid_map,gid_map,setgroups}, which are
 * configured by WRITING them -- there is no syscall for it, the file IS the
 * interface.
 *
 * Only those three files accept writes; every other handle has wkind == PW_NONE
 * and gets VFS_ERR_ROFS, which is what the whole filesystem returned before. */
static long procfs_write(struct vfs_file *f, const void *buf, size_t n) {
    struct procfs_handle *h = f->priv;
    if (!h || h->wkind == PW_NONE) return VFS_ERR_ROFS;
    struct proc *target = proc_lookup(h->wpid);
    if (!target) return VFS_ERR_NOENT;

    long rc;
    if (h->wkind == PW_SETGROUPS)
        rc = userns_write_setgroups(target, (const char *)buf, n);
    else
        rc = userns_write_map(target, h->wkind == PW_UID_MAP,
                              (const char *)buf, n);
    /* userns_* speak ABI errnos; the VFS speaks VFS_ERR_*. Map the two cases
     * that can occur rather than letting an ABI code escape as a VFS code --
     * the bug slice 6 recorded (raw VFS_ERR_* leaking to userspace as Linux
     * errnos) in the other direction. */
    if (rc < 0) {
        /* EPERM must map to VFS_ERR_NOTPERM, not VFS_ERR_PERM: the latter comes
         * out of vfs_err_to_abi() as EACCES, so the caller would see the wrong
         * refusal. Linux reports EPERM for every uid_map refusal and container
         * runtimes test for it specifically. */
        if (rc == -ABI_EPERM)  return VFS_ERR_NOTPERM;
        if (rc == -ABI_ENOENT) return VFS_ERR_NOENT;
        return VFS_ERR_INVAL;
    }
    return rc;
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

    /* The /proc/sys and /proc/net directory skeleton (2026-08-22). The
     * FILES stat through the generic open-and-measure fallthrough below;
     * only the directories need explicit arms so `test -d` works. */
    if (strcmp(rel, "sys") == 0 || strcmp(rel, "sys/kernel") == 0 ||
        strcmp(rel, "sys/kernel/random") == 0 || strcmp(rel, "sys/fs") == 0 ||
        strcmp(rel, "sys/vm") == 0 || strcmp(rel, "net") == 0) {
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
            int pid = procfs_kpid(rel);   /* slice 10 */
            if (proc_lookup(pid)) {
                out->type = VFS_TYPE_DIR;
                out->size = 0;
                out->uid = 0; out->gid = 0; out->mode = 0;
                return VFS_OK;
            }
            return VFS_ERR_NOENT;
        }
        if (*s == '/' && strcmp(s + 1, "exe") == 0) {
            int pid = procfs_kpid(rel);   /* slice 10 */
            if (proc_lookup(pid)) {
                out->type = VFS_TYPE_SYMLINK;
                out->size = 0;
                out->uid = 0; out->gid = 0; out->mode = 0;
                return VFS_OK;
            }
            return VFS_ERR_NOENT;
        }
        /* /proc/<pid>/task (dir) and /proc/<pid>/task/<tid> (dir).
         *
         * chrome's sandbox decides whether the process is mono-threaded by
         * stat'ing this directory and testing st_nlink == 3 -- ".", ".." and
         * one subdirectory per thread (sandbox/linux/services/thread_helpers.cc:41,
         * `PCHECK(0 == fstatat(proc_fd, "self/task/", ...))` then
         * `CHECK_LE(3UL, st_nlink)`). Without it the browser took the PCHECK
         * and died. Note the trailing slash chrome passes -- accept it. */
        if (*s == '/' && (strncmp(s + 1, "task", 4) == 0) &&
            (s[5] == '\0' || s[5] == '/')) {
            int pid = procfs_kpid(rel);   /* slice 10 */
            struct proc *p = proc_lookup(pid);
            if (!p) return VFS_ERR_NOENT;
            int tgid = p->is_thread ? p->tgid : p->pid;

            const char *after = s + 5;             /* "" or "/..." */
            if (*after == '/') after++;            /* skip the slash */
            if (*after == '\0') {                  /* the task directory */
                /* 2 (self + parent) + one entry per live thread in the group. */
                /* Count only LIVE threads. A TERMINATED-but-unreaped thread
                 * must not appear: chrome stops a thread and then polls until
                 * its task/<tid> entry returns ENOENT, giving up with
                 * "Stopped thread does not disappear in /proc" if it lingers
                 * (thread_helpers.cc:104). */
                uint32_t nthreads = 0;
                for (int i = 0; i < PROC_MAX; i++) {
                    if (g_proc[i].state == PROC_UNUSED ||
                        g_proc[i].state == PROC_EMBRYO ||
                        g_proc[i].state == PROC_TERMINATED) continue;
                    int qtg = g_proc[i].is_thread ? g_proc[i].tgid
                                                  : g_proc[i].pid;
                    if (qtg == tgid) nthreads++;
                }
                out->type = VFS_TYPE_DIR;
                out->nlink = 2u + (nthreads ? nthreads : 1u);
                out->size = 0; out->uid = 0; out->gid = 0; out->mode = 0;
                return VFS_OK;
            }
            /* /proc/<pid>/task/<tid> -- a per-thread directory. */
            int tid = procfs_kpid(after); /* slice 10 */
            struct proc *t = proc_lookup(tid);
            if (!t) return VFS_ERR_NOENT;
            /* A stopped/exited thread must READ AS GONE, not merely idle --
             * chrome polls this path waiting for ENOENT after joining a thread
             * (thread_helpers.cc:104). proc_lookup still returns TERMINATED
             * entries that have not been reaped yet, so filter them here. */
            if (t->state == PROC_TERMINATED) return VFS_ERR_NOENT;
            if ((t->is_thread ? t->tgid : t->pid) != tgid) return VFS_ERR_NOENT;
            out->type = VFS_TYPE_DIR;
            out->nlink = 2;
            out->size = 0; out->uid = 0; out->gid = 0; out->mode = 0;
            return VFS_OK;
        }

        /* /proc/<pid>/fdinfo (dir) and /proc/<pid>/fdinfo/<n> (regular file).
         * MUST be tested before the "fd" arm below, whose prefix match would
         * otherwise swallow "fdinfo" and report it as an fd symlink. */
        if (*s == '/' && strncmp(s + 1, "fdinfo", 6) == 0 &&
            (s[7] == '\0' || s[7] == '/')) {
            int pid = procfs_kpid(rel);
            struct proc *p = proc_lookup(pid);
            if (!p) return VFS_ERR_NOENT;
            if (s[7] == '\0') {                    /* the fdinfo directory */
                out->type = VFS_TYPE_DIR;
                out->nlink = 2;
                out->size = 0; out->uid = 0; out->gid = 0; out->mode = 0;
                return VFS_OK;
            }
            int fd = parse_int(s + 8);
            struct file **tab = proc_fds(p);
            if (fd >= 0 && fd < PROC_NFDS && tab && tab[fd]) {
                out->type = VFS_TYPE_FILE;
                out->size = 0; out->uid = 0; out->gid = 0; out->mode = 0;
                return VFS_OK;
            }
            return VFS_ERR_NOENT;
        }

        /* /proc/<pid>/fd (dir) and /proc/<pid>/fd/<n> (symlink) */
        if (*s == '/' && (strcmp(s + 1, "fd") == 0 ||
                          (s[1]=='f' && s[2]=='d' && s[3]=='/'))) {
            int pid = procfs_kpid(rel);   /* slice 10 */
            struct proc *p = proc_lookup(pid);
            if (!p) return VFS_ERR_NOENT;
            if (strcmp(s + 1, "fd") == 0) {        /* the fd directory */
                out->type = VFS_TYPE_DIR;
                out->size = 0; out->uid = 0; out->gid = 0; out->mode = 0;
                return VFS_OK;
            }
            int fd = parse_int(s + 4);             /* fd/<n> */
            if (fd >= 0 && fd < PROC_NFDS && p->fds[fd]) {
                out->type = VFS_TYPE_SYMLINK;
                out->size = 0; out->uid = 0; out->gid = 0; out->mode = 0;
                return VFS_OK;
            }
            return VFS_ERR_NOENT;
        }

        /* Slice 8: /proc/<pid>/ns (dir) and /proc/<pid>/ns/<kind> (symlink).
         * Every kind Linux defines is listed, including the ones this kernel
         * has not implemented yet -- a process really IS in the single global
         * namespace of those kinds, so reporting the link (with the initial
         * namespace's inum) is accurate, and it is what `lsns` expects to
         * find. ns_kind_implemented() is what gates unshare/setns. */
        if (*s == '/' && strncmp(s + 1, "ns", 2) == 0 &&
            (s[3] == '\0' || s[3] == '/')) {
            int pid = procfs_kpid(rel);   /* slice 10 */
            if (!proc_lookup(pid)) return VFS_ERR_NOENT;
            if (s[3] == '\0') {                    /* the ns directory */
                out->type = VFS_TYPE_DIR;
                out->nlink = 2;
                out->size = 0; out->uid = 0; out->gid = 0; out->mode = 0;
                return VFS_OK;
            }
            if (ns_kind_from_name(s + 4) < 0) return VFS_ERR_NOENT;
            out->type = VFS_TYPE_SYMLINK;
            out->size = 0; out->uid = 0; out->gid = 0; out->mode = 0;
            return VFS_OK;
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
    int isfd;   /* 1 if listing /proc/<pid>/fd */
    int isns;   /* 1 if listing /proc/<pid>/ns (slice 8) */
    int isfdi;  /* 1 if listing /proc/<pid>/fdinfo */
};

static int procfs_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    (void)mnt;
    if (!path || path[0] != '/') return VFS_ERR_NOENT;
    struct procfs_dir_handle *dh = kmalloc(sizeof(*dh));
    if (!dh) return VFS_ERR_NOMEM;
    dh->index = 0;
    dh->isfd  = 0;
    dh->isns  = 0;
    dh->isfdi = 0;

    if (strcmp(path, "/") == 0) {
        dh->pid = -1;
    } else {
        char normbuf[ABI_PATH_MAX];
        subst_self(path, normbuf, sizeof(normbuf));
        const char *rel = normbuf + 1;
        dh->pid = procfs_kpid(rel);  /* slice 10: caller-namespace pid */
        if (!proc_lookup(dh->pid)) { kfree(dh); return VFS_ERR_NOENT; }
        /* /proc/<pid>/fd is itself a directory of open-fd symlinks. */
        const char *s = rel;
        while (*s >= '0' && *s <= '9') s++;
        if (*s == '/' && strcmp(s + 1, "fdinfo") == 0) dh->isfdi = 1;
        else if (*s == '/' && strcmp(s + 1, "fd") == 0) dh->isfd = 1;
        else if (*s == '/' && strcmp(s + 1, "ns") == 0) dh->isns = 1;  /* slice 8 */
        else if (*s != '\0') { kfree(dh); return VFS_ERR_NOENT; }
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

    if (dh->isns) {
        /* Slice 8: /proc/<pid>/ns -- one symlink per namespace kind. */
        if (dh->index >= NS_KIND_COUNT) return VFS_ERR_NOENT;
        const char *kn = ns_kind_name(dh->index);
        memset(out->name, 0, VFS_NAME_MAX);
        size_t kl = strlen(kn);
        if (kl >= VFS_NAME_MAX) kl = VFS_NAME_MAX - 1;
        memcpy(out->name, kn, kl);
        out->type = VFS_TYPE_SYMLINK;
        out->size = 0;
        out->uid = 0; out->gid = 0; out->mode = 0;
        dh->index++;
        return VFS_OK;
    }

    if (dh->isfdi) {
        /* /proc/<pid>/fdinfo: one FILE named N per open descriptor -- the same
         * walk as fd below, with the type Linux reports. Chromium chroots here
         * and then requires the directory to be empty once it has closed its
         * descriptors, so the listing has to track the fd table live rather
         * than report a fixed set. */
        struct proc *p = proc_lookup(dh->pid);
        if (!p) return VFS_ERR_NOENT;
        struct file **tab = proc_fds(p);
        if (!tab) return VFS_ERR_NOENT;
        for (int i = dh->index; i < PROC_NFDS; i++) {
            if (tab[i]) {
                memset(out->name, 0, VFS_NAME_MAX);
                char tmp[16];
                int_to_str(tmp, sizeof(tmp), i);
                memcpy(out->name, tmp, strlen(tmp));
                out->type = VFS_TYPE_FILE;
                out->size = 0;
                out->uid = 0; out->gid = 0; out->mode = 0;
                dh->index = i + 1;
                return VFS_OK;
            }
        }
        return VFS_ERR_NOENT;
    }

    if (dh->isfd) {
        /* /proc/<pid>/fd: one symlink-named-N per open descriptor. */
        struct proc *p = proc_lookup(dh->pid);
        if (!p) return VFS_ERR_NOENT;
        for (int i = dh->index; i < PROC_NFDS; i++) {
            if (p->fds[i]) {
                memset(out->name, 0, VFS_NAME_MAX);
                char tmp[16];
                int_to_str(tmp, sizeof(tmp), i);
                size_t tl = strlen(tmp);
                memcpy(out->name, tmp, tl);
                out->type = VFS_TYPE_SYMLINK;
                out->size = 0;
                out->uid = 0; out->gid = 0; out->mode = 0;
                dh->index = i + 1;
                return VFS_OK;
            }
        }
        return VFS_ERR_NOENT;
    }

    if (dh->pid == -1) {
        /* Root /proc listing: global files first, then pid dirs */
        static const char *globals[] = { "uptime", "meminfo", "version",
                                         "cpuinfo", "loadavg", "stat",
                                         "mounts", "self",
                                         /* slice 6 */
                                         "filesystems", "devices" };
        const int nglobals = 10;
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
        struct proc *me = current_proc();
        for (int i = pidx; i < PROC_MAX; i++) {
            if (g_proc[i].state != PROC_UNUSED &&
                g_proc[i].state != PROC_EMBRYO) {
                /* Slice 10: list only what the READER's pid namespace can see,
                 * under the number it sees. It has to be the LISTING that
                 * filters, not just the per-pid lookup, or a container's `ps`
                 * enumerates host processes and then fails to stat them.
                 *
                 * A reader in the initial namespace is left EXACTLY as before,
                 * pid 0 included -- pid_vnr_in reports 0 as "not visible", so
                 * routing the initial namespace through it would have silently
                 * dropped /proc/0 from the listing. Same reasoning as
                 * procfs_kpid(). */
                int vp;
                if (!me || !me->pid_ns) {
                    vp = g_proc[i].pid;
                } else {
                    vp = pid_vnr_in(me->pid_ns, g_proc[i].pid);
                    if (!vp) continue;
                }
                memset(out->name, 0, VFS_NAME_MAX);
                char tmp[16];
                int_to_str(tmp, sizeof(tmp), vp);
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

    /* Per-pid directory listing. "exe" is a symlink, "fd" and "ns" are
     * directories, the rest are files. */
    static const char *entries[] = { "status", "cmdline", "maps", "stat",
                                     "mounts",
                                     "exe", "fd", "ns",
                                     /* slice 11 */
                                     "uid_map", "gid_map", "setgroups",
                                     "fdinfo" };
    const int nentries = 11;
    if (dh->index >= nentries) return VFS_ERR_NOENT;
    memset(out->name, 0, VFS_NAME_MAX);
    size_t nl = strlen(entries[dh->index]);
    memcpy(out->name, entries[dh->index], nl);
    if (dh->index == 4)      out->type = VFS_TYPE_SYMLINK;   /* exe */
    else if (dh->index == 5) out->type = VFS_TYPE_DIR;       /* fd  */
    else if (dh->index == 6) out->type = VFS_TYPE_DIR;       /* ns (slice 8) */
    else if (dh->index == 10) out->type = VFS_TYPE_DIR;      /* fdinfo */
    else                     out->type = VFS_TYPE_FILE;
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

    /* Bare /proc/self -> /proc/<pid> -- the pid in the READER's namespace
     * (2026-08-22: this was the one site still writing the HOST pid,
     * against slice 10's rule; inside a pid namespace the link then named
     * a process the reader cannot even see). */
    if (strcmp(path, "/self") == 0) {
        struct proc *p = current_proc();
        char pidstr[16];
        int pl = int_to_str(pidstr, sizeof(pidstr), p ? pid_vnr(p) : 0);
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
            int pid = procfs_kpid(rel);   /* slice 10 */
            struct proc *p = proc_lookup(pid);
            if (!p) return VFS_ERR_NOENT;
            const char *tgt = p->exe_path[0] ? p->exe_path : p->name;
            size_t tl = strlen(tgt);
            if (tl >= bufsz) tl = bufsz - 1;
            memcpy(buf, tgt, tl);
            buf[tl] = '\0';
#ifdef CHROMIUM_BOOT
            /* Slice 73: chrome derives DIR_ASSETS (icudtl.dat, the .pak
             * files, the v8 snapshot) from readlink("/proc/self/exe") and
             * CHECKs with "Invalid file descriptor to ICU data received" if
             * it lands in the wrong directory -- reproduced exactly on the
             * Linux control by making /proc/self/exe point at the loader.
             * The full-chrome INT3 has the same shape, so log what we hand
             * back, and whether it came from exe_path or the p->name
             * fallback (the fallback is NOT a path and would break ICU). */
            { static int el = 0; if (el < 12) { el++;
                kprintf("[procexe] pid=%d -> '%s' (%s)\n", pid, buf,
                        p->exe_path[0] ? "exe_path" : "NAME FALLBACK");
            } }
#endif
            return VFS_OK;
        }
        /* /proc/<pid>/fd/<n> -> a description of the open file */
        if (*s == '/' && s[1]=='f' && s[2]=='d' && s[3]=='/') {
            int pid = procfs_kpid(rel);   /* slice 10 */
            struct proc *p = proc_lookup(pid);
            if (!p) return VFS_ERR_NOENT;
            int fd = parse_int(s + 4);
            if (fd < 0 || fd >= PROC_NFDS || !p->fds[fd]) return VFS_ERR_NOENT;
            describe_fd_target(p->fds[fd], buf, bufsz);
            return VFS_OK;
        }
        /* Slice 8: /proc/<pid>/ns/<kind> -> "<kind>:[<inum>]".
         *
         * The inum is the ONLY thing that identifies a namespace to userspace:
         * two processes are in the same namespace exactly when these strings
         * match, which is how lsns, `ip netns identify` and every container
         * runtime compare them. So this is not a cosmetic string -- it is the
         * comparison the whole feature is observed through. */
        if (*s == '/' && s[1]=='n' && s[2]=='s' && s[3]=='/') {
            int pid = procfs_kpid(rel);   /* slice 10 */
            struct proc *p = proc_lookup(pid);
            if (!p) return VFS_ERR_NOENT;
            int kind = ns_kind_from_name(s + 4);
            if (kind < 0) return VFS_ERR_NOENT;
            const char *kn = ns_kind_name(kind);
            char num[24];
            int nl = uint_to_str(num, sizeof num, ns_inum_of(p, kind));
            if (nl < 0) return VFS_ERR_INVAL;
            size_t kl = strlen(kn);
            if (kl + 2 + (size_t)nl + 1 + 1 > bufsz) return VFS_ERR_INVAL;
            size_t o = 0;
            memcpy(buf + o, kn, kl); o += kl;
            buf[o++] = ':'; buf[o++] = '[';
            memcpy(buf + o, num, (size_t)nl); o += (size_t)nl;
            buf[o++] = ']';
            buf[o] = '\0';
            return VFS_OK;
        }
    }
    return VFS_ERR_NOENT;
}

static const struct vfs_ops procfs_ops = {
    .open     = procfs_open,
    .close    = procfs_close,
    .read     = procfs_read,
    .write    = procfs_write,   /* slice 11: uid_map/gid_map/setgroups only */
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

/* Mount procfs at an arbitrary point, for mount(2) -t proc.
 *
 * procfs here is a SINGLETON with no per-mount state (ops + data==0), and every
 * file it serves is rendered from the CALLING process at read time -- which is
 * what already makes /proc pid-namespace-aware. So a second mount point is a
 * second view of the same generator, which is exactly what a container runtime
 * asks for and is not a pretence: nothing about the first mount is copied,
 * because there is nothing to copy. */
int procfs_mount_at(const char *path) {
    return vfs_mount(path, &procfs_ops, 0);
}

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
