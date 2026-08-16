/* linux-ocirun -- a minimal OCI runtime. Slice 16's capstone.
 *
 * ===========================================================================
 * WHAT THIS IS
 *
 * Given an OCI bundle (a directory holding `config.json` and a root
 * filesystem), it starts the configured process inside the namespaces, cgroup
 * limits, mounts, seccomp filter and credentials that config.json asks for,
 * waits for it, and reports its exit status. That is the whole job of an OCI
 * runtime's `run`, and it is the integration proof for the entire Phase 3 arc:
 * every namespace, pivot_root, cgroup v2 and seccomp have to work TOGETHER,
 * driven by a real config file rather than by a test that calls each in turn.
 *
 *     linux-ocirun <bundle-dir> [container-id]
 *
 * ===========================================================================
 * THE RULE THIS PROGRAM IS BUILT AROUND
 *
 * A container runtime's failure mode is not crashing, it is STARTING ANYWAY.
 * Every field in config.json either grants or withholds isolation, so a
 * runtime that shrugs at a directive it does not understand hands back a
 * container weaker than the one that was asked for -- and looks like it
 * worked. So:
 *
 *   - anything that AFFECTS ISOLATION and cannot be honoured is FATAL:
 *     an unknown namespace type, a namespace `path` (setns) we cannot join,
 *     an unknown seccomp action or syscall name, a seccomp arg-filter, a
 *     uid/gid mapping that cannot be written, a failed pivot_root, a failed
 *     privilege drop.
 *   - anything that does NOT affect isolation and is not honoured is REPORTED
 *     BY NAME and counted, never silently dropped: unsupported mount types,
 *     hooks, capabilities, rlimits, annotations.
 *
 * The summary line at the end prints both counts, so a caller can assert on
 * them. "It ran" is not the assertion; WHAT WAS APPLIED is.
 *
 * ===========================================================================
 * WHAT IS AND IS NOT SUPPORTED, STATED RATHER THAN IMPLIED
 *
 * Supported: root.path + root.readonly(reported), process.args/env/cwd/
 * terminal(ignored)/user.{uid,gid}, hostname, linux.namespaces
 * {pid,network,ipc,uts,mount,user}, linux.uidMappings/gidMappings,
 * linux.resources.{pids.limit, memory.limit, cpu.shares}, mounts of type
 * proc/sysfs/cgroup2/bind, linux.seccomp with defaultAction + syscalls[]
 * {names, action, errnoRet}.
 *
 * NOT supported, and fatal: namespace type "cgroup" (this kernel has no cgroup
 * namespace), namespace.path (no setns-into-a-path here), seccomp
 * syscalls[].args (an ignored argument filter silently BROADENS the rule),
 * seccomp actions TRACE/NOTIFY (nothing is listening, so they would fail open).
 *
 * NOT supported, and reported: mounts of any other type (notably tmpfs and
 * devtmpfs -- this kernel has no mountable in-memory filesystem, and accepting
 * `-t tmpfs` to mount something else would be a lie), hooks, capabilities,
 * rlimits, apparmor/selinux labels, devices, rootfsPropagation, oomScoreAdj.
 * ===========================================================================
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include "json.h"
#include "syscalls.h"

/* ---- seccomp bits (no libseccomp here; the kernel takes raw cBPF) -------- */
struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog  { unsigned short len; struct sock_filter *filter; };
#define BPF_LD   0x00
#define BPF_JMP  0x05
#define BPF_RET  0x06
#define BPF_W    0x00
#define BPF_ABS  0x20
#define BPF_JEQ  0x10
#define BPF_K    0x00
#define SECCOMP_SET_MODE_FILTER 1
#define SECCOMP_RET_KILL_PROCESS 0x80000000u
#define SECCOMP_RET_TRAP         0x00030000u
#define SECCOMP_RET_ERRNO        0x00050000u
#define SECCOMP_RET_ALLOW        0x7fff0000u
#define SECCOMP_DATA_NR_OFF      0

#ifndef MS_PRIVATE
#define MS_PRIVATE (1u << 18)
#endif
#ifndef MS_REC
#define MS_REC     0x4000u
#endif
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif

#define OLDROOT ".oldroot"

/* Exit codes. Distinct on purpose: "the container failed" is not a diagnosis,
 * and this runs on a serial console where one number may be all you get. */
#define EX_USAGE      64
#define EX_CONFIG     65   /* config.json missing / unreadable / unparseable   */
#define EX_UNSUPP     66   /* an ISOLATION directive we refuse to fake         */
#define EX_SETUP      67   /* a setup step failed in the parent                */
#define EX_CHILD_FAIL 68   /* the container's setup failed inside the child    */

/* ---- small helpers ------------------------------------------------------ */

static int write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) return -1;
    size_t n = strlen(text);
    ssize_t w = write(fd, text, n);
    int e = errno;
    close(fd);
    if (w != (ssize_t)n) { errno = e; return -1; }
    return 0;
}

static int pivot_root_(const char *new_root, const char *put_old)
{
    return (int)syscall(SYS_pivot_root, new_root, put_old);
}

/* Variadic and format-checked. The first version took a fixed
 * (const char *, long) pair and handed both to printf regardless of what the
 * format wanted, so a message with one conversion consumed the wrong argument
 * and printed a garbage number. These messages are the only explanation a
 * refused container ever gives, and a wrong value in one is indistinguishable
 * from a wrong diagnosis. */
__attribute__((format(printf, 2, 3), noreturn))
static void die(int code, const char *fmt, ...)
{
    va_list ap;
    printf("[ocirun] FATAL: ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(code);
}

/* ---- what the child needs, handed across the clone ---------------------- */

struct childcfg {
    struct json *j;
    int   root;             /* config root node          */
    int   syncfd;           /* read end: parent's go-ahead */
    const char *rootfs;     /* absolute path             */
    const char *hostname;
    int   has_mountns, has_userns, has_utsns;
    int   applied, unsupported;   /* counters, printed by the child */
};

static void note_unsupported(struct childcfg *c, const char *what,
                             const char *detail)
{
    c->unsupported++;
    printf("[ocirun]   NOT APPLIED: %s (%s)\n", what, detail);
}

/* ---- seccomp ------------------------------------------------------------ */

static unsigned seccomp_action(struct json *j, int node, unsigned errno_ret,
                               const char *where)
{
    const char *a = json_str(j, node, NULL);
    if (!a) die(EX_CONFIG, "%s: missing seccomp action", where);
    if (strcmp(a, "SCMP_ACT_ALLOW") == 0)  return SECCOMP_RET_ALLOW;
    if (strcmp(a, "SCMP_ACT_ERRNO") == 0)  return SECCOMP_RET_ERRNO |
                                                  (errno_ret & 0xffff);
    if (strcmp(a, "SCMP_ACT_TRAP")  == 0)  return SECCOMP_RET_TRAP;
    if (strcmp(a, "SCMP_ACT_KILL")  == 0 ||
        strcmp(a, "SCMP_ACT_KILL_PROCESS") == 0 ||
        strcmp(a, "SCMP_ACT_KILL_THREAD")  == 0) return SECCOMP_RET_KILL_PROCESS;
    /* TRACE and NOTIFY need a supervisor that is not here. Honouring them as
     * ALLOW is how a filter fails OPEN, which is the worst outcome available;
     * this arc already found that exact shape once, in the kernel's own
     * seccomp return-value comparison. Refuse instead. */
    die(EX_UNSUPP, "%s: seccomp action %s needs a supervisor this runtime does "
                   "not provide -- refusing rather than treating it as ALLOW",
        where, a);
    return 0;
}

#define MAX_FILTER 512
static struct sock_filter g_filter[MAX_FILTER];

static int build_seccomp(struct json *j, int sc, struct childcfg *c)
{
    int n = 0;
    unsigned dflt = seccomp_action(j, json_member(j, sc, "defaultAction"),
                                   1 /* EPERM */, "linux.seccomp");

    /* A = seccomp_data.nr */
    g_filter[n].code = BPF_LD | BPF_W | BPF_ABS;
    g_filter[n].jt = 0; g_filter[n].jf = 0;
    g_filter[n].k = SECCOMP_DATA_NR_OFF; n++;

    int rules = json_member(j, sc, "syscalls");
    int nrules = json_len(j, rules);
    for (int i = 0; i < nrules; i++) {
        int r = json_at(j, rules, i);
        if (json_member(j, r, "args") >= 0)
            die(EX_UNSUPP, "linux.seccomp.syscalls[%ld].args: argument filters "
                           "are not supported, and IGNORING one would BROADEN "
                           "the rule it belongs to", (long)i);
        unsigned er = (unsigned)json_num(j, json_member(j, r, "errnoRet"), 1);
        unsigned act = seccomp_action(j, json_member(j, r, "action"), er,
                                      "linux.seccomp.syscalls[]");
        int names = json_member(j, r, "names");
        int nn = json_len(j, names);
        for (int k = 0; k < nn; k++) {
            const char *nm = json_str(j, json_at(j, names, k), NULL);
            if (!nm) die(EX_CONFIG, "seccomp syscall name is not a string");
            int nr = syscall_nr_by_name(nm);
            if (nr < 0)
                die(EX_UNSUPP, "seccomp names an unknown syscall '%s' -- "
                               "refusing, because skipping it would silently "
                               "change what the filter permits", nm);
            if (n + 2 >= MAX_FILTER)
                die(EX_UNSUPP, "seccomp profile too large for this runtime "
                               "(%ld instructions)", (long)n);
            /* if (A == nr) -> return act */
            g_filter[n].code = BPF_JMP | BPF_JEQ | BPF_K;
            g_filter[n].jt = 0; g_filter[n].jf = 1;
            g_filter[n].k = (unsigned)nr; n++;
            g_filter[n].code = BPF_RET | BPF_K;
            g_filter[n].jt = 0; g_filter[n].jf = 0;
            g_filter[n].k = act; n++;
        }
    }
    g_filter[n].code = BPF_RET | BPF_K;
    g_filter[n].jt = 0; g_filter[n].jf = 0;
    g_filter[n].k = dflt; n++;

    /* Linux requires no_new_privs (or CAP_SYS_ADMIN) before a filter, and the
     * container wants it regardless: without it a setuid binary inside could
     * regain privilege the profile assumes it cannot. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        printf("[ocirun] child: PR_SET_NO_NEW_PRIVS failed errno=%d\n", errno);
        return -1;
    }
    struct sock_fprog prog = { (unsigned short)n, g_filter };
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) != 0) {
        printf("[ocirun] child: seccomp(SET_MODE_FILTER) failed errno=%d\n",
               errno);
        return -1;
    }
    printf("[ocirun]   seccomp: %d cBPF instructions installed "
           "(%d rule group(s))\n", n, nrules);
    c->applied++;
    return 0;
}

/* ---- the container process --------------------------------------------- */

static int container_main(void *arg)
{
    struct childcfg *c = (struct childcfg *)arg;
    struct json *j = c->j;
    char b[16];

    /* Wait for the parent: uid/gid maps and cgroup membership have to be in
     * place BEFORE we look at our own credentials or allocate anything, and
     * only the parent can write them (a process in a fresh user namespace
     * cannot map itself). A read that returns 0 means the parent closed the
     * pipe without writing = it failed; treat that as fatal rather than
     * racing on. */
    if (read(c->syncfd, b, 1) != 1) {
        printf("[ocirun] child: parent setup failed -- refusing to continue\n");
        return EX_CHILD_FAIL;
    }
    close(c->syncfd);

    if (c->has_utsns && c->hostname) {
        if (sethostname(c->hostname, strlen(c->hostname)) != 0) {
            printf("[ocirun] child: sethostname('%s') errno=%d\n",
                   c->hostname, errno);
            return EX_CHILD_FAIL;
        }
        printf("[ocirun]   hostname: %s\n", c->hostname);
        c->applied++;
    } else if (c->hostname && !c->has_utsns) {
        die(EX_UNSUPP, "config sets a hostname but asks for no uts namespace; "
                       "setting it would rename the HOST");
    }

    /* ---- root filesystem ------------------------------------------------
     * pivot_root, not chroot, when the rootfs is a mount point: chroot only
     * changes name resolution and leaves the old tree reachable through any
     * pre-existing fd, while pivot_root MOVES the root mount and lets us
     * detach the old one. The difference is the whole point.
     *
     * If the rootfs is NOT a mount point this runtime STOPS. It does not fall
     * back to chroot: a silent downgrade from "the old root is gone" to "the
     * old root is merely hard to name" is exactly the kind of quiet weakening
     * this program exists not to do. The caller is told which is missing. */
    if (!c->has_mountns)
        die(EX_UNSUPP, "config asks for a rootfs but no mount namespace; "
                       "pivoting the HOST's root is not something this will "
                       "do");

    /* Detach propagation first, or the pivot and the mounts below can travel
     * back out to the host's mount namespace. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        printf("[ocirun] child: making / private failed errno=%d\n", errno);
        return EX_CHILD_FAIL;
    }

    char oldpath[512];
    snprintf(oldpath, sizeof oldpath, "%s/%s", c->rootfs, OLDROOT);
    if (mkdir(oldpath, 0700) != 0 && errno != EEXIST) {
        printf("[ocirun] child: mkdir %s errno=%d\n", oldpath, errno);
        return EX_CHILD_FAIL;
    }
    if (pivot_root_(c->rootfs, oldpath) != 0) {
        printf("[ocirun] child: pivot_root(%s, %s) errno=%d\n",
               c->rootfs, oldpath, errno);
        if (errno == ENOENT || errno == EINVAL)
            printf("[ocirun]   (this kernel requires the new root to BE a "
                   "mount point, and cannot bind a subtree to make one)\n");
        return EX_CHILD_FAIL;
    }
    if (chdir("/") != 0) {
        printf("[ocirun] child: chdir(/) after pivot errno=%d\n", errno);
        return EX_CHILD_FAIL;
    }
    /* Detaching the old root is not tidying-up: until it happens the container
     * can still walk out through /.oldroot. A failure here is a failure of the
     * container, so it is fatal. */
    if (umount2("/" OLDROOT, MNT_DETACH) != 0 &&
        umount2("/" OLDROOT, 0) != 0) {
        printf("[ocirun] child: could not detach the old root (errno=%d) -- "
               "the container could still escape through /%s\n",
               errno, OLDROOT);
        return EX_CHILD_FAIL;
    }
    rmdir("/" OLDROOT);
    printf("[ocirun]   root: pivot_root into %s, old root detached\n",
           c->rootfs);
    c->applied++;

    /* ---- mounts --------------------------------------------------------- */
    {
        int ms = json_member(j, c->root, "mounts");
        int nm = json_len(j, ms);
        for (int i = 0; i < nm; i++) {
            int m = json_at(j, ms, i);
            const char *dst  = json_str(j, json_member(j, m, "destination"), NULL);
            const char *type = json_str(j, json_member(j, m, "type"), "");
            const char *src  = json_str(j, json_member(j, m, "source"), NULL);
            if (!dst) die(EX_CONFIG, "mounts[%ld] has no destination", (long)i);
            /* mkdir -p one level; a bundle whose rootfs already has the dirs
             * (the normal case) just gets EEXIST. */
            mkdir(dst, 0755);
            unsigned long fl = 0;
            int isbind = (strcmp(type, "bind") == 0);
            if (isbind) fl |= MS_BIND;
            if (mount(isbind ? src : (src ? src : type), dst,
                      isbind ? NULL : type, fl, NULL) != 0) {
                if (errno == ENODEV) {
                    note_unsupported(c, dst, type[0] ? type : "no type");
                } else {
                    printf("[ocirun]   NOT APPLIED: %s (type %s, errno=%d)\n",
                           dst, type, errno);
                    c->unsupported++;
                }
                continue;
            }
            printf("[ocirun]   mount: %s type %s\n", dst, type);
            c->applied++;
        }
    }

    /* ---- seccomp, BEFORE the privilege drop ------------------------------
     * Order matters: installing the filter while still privileged means the
     * drop itself cannot be blocked by the profile, and no_new_privs is set
     * here so the drop cannot be undone by a setuid binary afterwards. */
    {
        int lin = json_member(j, c->root, "linux");
        int sc  = json_member(j, lin, "seccomp");
        if (sc >= 0 && build_seccomp(j, sc, c) != 0) return EX_CHILD_FAIL;
    }

    /* ---- credentials ---------------------------------------------------- */
    {
        int pu = json_path(j, c->root, "process.user");
        long long uid = json_num(j, json_member(j, pu, "uid"), 0);
        long long gid = json_num(j, json_member(j, pu, "gid"), 0);
        if (gid != 0 && setgid((gid_t)gid) != 0) {
            printf("[ocirun] child: setgid(%lld) errno=%d\n", gid, errno);
            return EX_CHILD_FAIL;
        }
        if (uid != 0 && setuid((uid_t)uid) != 0) {
            printf("[ocirun] child: setuid(%lld) errno=%d\n", uid, errno);
            return EX_CHILD_FAIL;
        }
        /* Verify rather than assume: a "drop" that moved the field and left
         * the authority is worse than none, because it looks safe. */
        if ((long long)getuid() != uid || (long long)getgid() != gid) {
            printf("[ocirun] child: credential drop did not take "
                   "(uid=%u gid=%u, wanted %lld/%lld)\n",
                   (unsigned)getuid(), (unsigned)getgid(), uid, gid);
            return EX_CHILD_FAIL;
        }
        if (uid || gid) {
            printf("[ocirun]   user: uid=%lld gid=%lld\n", uid, gid);
            c->applied++;
        }
    }

    /* ---- cwd + exec ------------------------------------------------------ */
    {
        const char *cwd = json_str(j, json_path(j, c->root, "process.cwd"), "/");
        if (chdir(cwd) != 0) {
            printf("[ocirun] child: chdir('%s') errno=%d\n", cwd, errno);
            return EX_CHILD_FAIL;
        }
    }
    printf("[ocirun]   applied=%d unsupported=%d -- exec\n",
           c->applied, c->unsupported);
    fflush(stdout);
    {
        int pa = json_path(j, c->root, "process.args");
        int na = json_len(j, pa);
        int pe = json_path(j, c->root, "process.env");
        int ne = json_len(j, pe);
        if (na < 1) { printf("[ocirun] child: process.args is empty\n");
                      return EX_CHILD_FAIL; }
        static char *av[64], *ev[64];
        if (na > 63 || ne > 63) { printf("[ocirun] child: too many args/env\n");
                                  return EX_CHILD_FAIL; }
        for (int i = 0; i < na; i++)
            av[i] = (char *)json_str(j, json_at(j, pa, i), "");
        av[na] = NULL;
        for (int i = 0; i < ne; i++)
            ev[i] = (char *)json_str(j, json_at(j, pe, i), "");
        ev[ne] = NULL;
        execve(av[0], av, ev);
        printf("[ocirun] child: execve('%s') errno=%d\n", av[0], errno);
    }
    return EX_CHILD_FAIL;
}

/* ---- namespaces --------------------------------------------------------- */

static int ns_flag(const char *type)
{
    if (strcmp(type, "pid")     == 0) return CLONE_NEWPID;
    if (strcmp(type, "network") == 0) return CLONE_NEWNET;
    if (strcmp(type, "ipc")     == 0) return CLONE_NEWIPC;
    if (strcmp(type, "uts")     == 0) return CLONE_NEWUTS;
    if (strcmp(type, "mount")   == 0) return CLONE_NEWNS;
    if (strcmp(type, "user")    == 0) return CLONE_NEWUSER;
    return 0;
}

/* ---- cgroup ------------------------------------------------------------- */

#define CGROOT "/sys/fs/cgroup"

static int cg_write(const char *dir, const char *file, const char *val)
{
    char p[320];
    snprintf(p, sizeof p, "%s/%s", dir, file);
    return write_file(p, val);
}

/* Returns the number of limits actually applied, or -1 on a hard failure. */
static int setup_cgroup(struct json *j, int root, const char *id,
                        char *dirout, size_t dirsz, int *unsupported)
{
    int res = json_path(j, root, "linux.resources");
    int pidlim = json_path(j, root, "linux.resources.pids.limit");
    int memlim = json_path(j, root, "linux.resources.memory.limit");
    int cpushr = json_path(j, root, "linux.resources.cpu.shares");
    if (res < 0) { dirout[0] = '\0'; return 0; }

    snprintf(dirout, dirsz, CGROOT "/%s", id);
    if (mkdir(dirout, 0755) != 0 && errno != EEXIST) {
        printf("[ocirun] cgroup: mkdir %s errno=%d\n", dirout, errno);
        dirout[0] = '\0';
        return -1;
    }
    int n = 0;
    char v[32];
    if (pidlim >= 0) {
        snprintf(v, sizeof v, "%lld", json_num(j, pidlim, 0));
        if (cg_write(dirout, "pids.max", v) != 0) {
            printf("[ocirun] cgroup: pids.max errno=%d\n", errno); return -1;
        }
        printf("[ocirun]   cgroup: pids.max=%s\n", v); n++;
    }
    if (memlim >= 0) {
        snprintf(v, sizeof v, "%lld", json_num(j, memlim, 0));
        if (cg_write(dirout, "memory.max", v) != 0) {
            printf("[ocirun] cgroup: memory.max errno=%d\n", errno); return -1;
        }
        printf("[ocirun]   cgroup: memory.max=%s\n", v); n++;
    }
    if (cpushr >= 0) {
        /* OCI carries cgroup-v1 `shares` (2..262144); cgroup v2 wants
         * `cpu.weight` (1..10000). runc's conversion, used verbatim so the
         * number in the config means the same thing here as it does there. */
        long long sh = json_num(j, cpushr, 1024);
        long long w = (((sh - 2) * 9999) / 262142) + 1;
        if (w < 1) w = 1; if (w > 10000) w = 10000;
        snprintf(v, sizeof v, "%lld", w);
        if (cg_write(dirout, "cpu.weight", v) != 0) {
            printf("[ocirun] cgroup: cpu.weight errno=%d\n", errno); return -1;
        }
        printf("[ocirun]   cgroup: cpu.weight=%s (from cpu.shares=%lld)\n",
               v, sh); n++;
    }
    /* Anything else under resources is not applied; say so by name. */
    for (int ch = j->n[res].first; ch >= 0; ch = j->n[ch].next) {
        const char *k = j->n[ch].key;
        if (!k) continue;
        if (strcmp(k, "pids") == 0 || strcmp(k, "memory") == 0 ||
            strcmp(k, "cpu") == 0) continue;
        printf("[ocirun]   NOT APPLIED: linux.resources.%s\n", k);
        (*unsupported)++;
    }
    return n;
}

/* ---- main --------------------------------------------------------------- */

static char g_cfgbuf[256 * 1024];
static struct json g_json;
static struct childcfg g_cfg;
#define CSTACK_SZ (512u * 1024u)
static char g_cstack[CSTACK_SZ] __attribute__((aligned(64)));

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        printf("usage: linux-ocirun <bundle-dir> [container-id]\n");
        return EX_USAGE;
    }
    const char *bundle = argv[1];
    const char *id = (argc > 2) ? argv[2] : "oci";

    char cfgpath[512];
    snprintf(cfgpath, sizeof cfgpath, "%s/config.json", bundle);
    int fd = open(cfgpath, O_RDONLY);
    if (fd < 0) { printf("[ocirun] cannot open %s errno=%d\n", cfgpath, errno);
                  return EX_CONFIG; }
    ssize_t n = read(fd, g_cfgbuf, sizeof g_cfgbuf - 1);
    close(fd);
    if (n <= 0) { printf("[ocirun] %s is empty/unreadable\n", cfgpath);
                  return EX_CONFIG; }
    if ((size_t)n >= sizeof g_cfgbuf - 1) {
        printf("[ocirun] %s is larger than this runtime reads (%u bytes) -- "
               "refusing a partial config\n", cfgpath,
               (unsigned)sizeof g_cfgbuf);
        return EX_CONFIG;
    }
    g_cfgbuf[n] = '\0';

    int root = json_parse(&g_json, g_cfgbuf);
    if (root < 0) {
        printf("[ocirun] %s: %s\n", cfgpath, g_json.err);
        return EX_CONFIG;
    }
    printf("[ocirun] bundle=%s id=%s ociVersion=%s\n", bundle, id,
           json_str(&g_json, json_member(&g_json, root, "ociVersion"), "?"));

    /* ---- rootfs --------------------------------------------------------- */
    static char rootfs[512];
    {
        const char *rp = json_str(&g_json, json_path(&g_json, root, "root.path"),
                                  "rootfs");
        if (rp[0] == '/') snprintf(rootfs, sizeof rootfs, "%s", rp);
        else              snprintf(rootfs, sizeof rootfs, "%s/%s", bundle, rp);
        int ro = json_member(&g_json, json_member(&g_json, root, "root"),
                             "readonly");
        if (ro >= 0 && g_json.n[ro].type == J_BOOL && g_json.n[ro].bval)
            printf("[ocirun]   NOT APPLIED: root.readonly (this VFS cannot "
                   "remount an existing mount read-only)\n");
    }

    /* ---- namespaces ------------------------------------------------------ */
    int flags = 0;
    {
        int nss = json_path(&g_json, root, "linux.namespaces");
        int cnt = json_len(&g_json, nss);
        for (int i = 0; i < cnt; i++) {
            int e = json_at(&g_json, nss, i);
            const char *t = json_str(&g_json, json_member(&g_json, e, "type"),
                                     NULL);
            if (!t) die(EX_CONFIG, "linux.namespaces[%ld] has no type", (long)i);
            if (json_member(&g_json, e, "path") >= 0)
                die(EX_UNSUPP, "linux.namespaces[].path (join an existing "
                               "namespace) is not supported: type=%s", t);
            int f = ns_flag(t);
            if (!f)
                die(EX_UNSUPP, "namespace type '%s' is not supported by this "
                               "kernel -- refusing rather than starting a "
                               "container with one boundary missing", t);
            flags |= f;
        }
        printf("[ocirun]   namespaces: %s%s%s%s%s%s(clone flags 0x%x)\n",
               (flags & CLONE_NEWUSER) ? "user " : "",
               (flags & CLONE_NEWNS)   ? "mount " : "",
               (flags & CLONE_NEWPID)  ? "pid " : "",
               (flags & CLONE_NEWNET)  ? "network " : "",
               (flags & CLONE_NEWUTS)  ? "uts " : "",
               (flags & CLONE_NEWIPC)  ? "ipc " : "", (unsigned)flags);
    }

    /* ---- things we are not applying, named ------------------------------ */
    int unsupported = 0;
    {
        static const char *ignored_top[] = { "hooks", "annotations", NULL };
        for (int i = 0; ignored_top[i]; i++)
            if (json_member(&g_json, root, ignored_top[i]) >= 0) {
                printf("[ocirun]   NOT APPLIED: %s\n", ignored_top[i]);
                unsupported++;
            }
        static const char *ignored_proc[] = { "capabilities", "rlimits",
                                              "apparmorProfile", "selinuxLabel",
                                              "oomScoreAdj", NULL };
        int pr = json_member(&g_json, root, "process");
        for (int i = 0; ignored_proc[i]; i++)
            if (json_member(&g_json, pr, ignored_proc[i]) >= 0) {
                printf("[ocirun]   NOT APPLIED: process.%s\n", ignored_proc[i]);
                unsupported++;
            }
        static const char *ignored_lin[] = { "devices", "rootfsPropagation",
                                             "maskedPaths", "readonlyPaths",
                                             "sysctl", NULL };
        int li = json_member(&g_json, root, "linux");
        for (int i = 0; ignored_lin[i]; i++)
            if (json_member(&g_json, li, ignored_lin[i]) >= 0) {
                printf("[ocirun]   NOT APPLIED: linux.%s\n", ignored_lin[i]);
                unsupported++;
            }
    }

    /* ---- cgroup (parent-side; the child cannot create its own) ----------- */
    static char cgdir[320];
    int cglimits = setup_cgroup(&g_json, root, id, cgdir, sizeof cgdir,
                                &unsupported);
    if (cglimits < 0) return EX_SETUP;

    /* ---- start ---------------------------------------------------------- */
    int sync_pipe[2];
    if (pipe(sync_pipe) != 0) {
        printf("[ocirun] pipe errno=%d\n", errno); return EX_SETUP;
    }

    g_cfg.j = &g_json;
    g_cfg.root = root;
    g_cfg.syncfd = sync_pipe[0];
    g_cfg.rootfs = rootfs;
    g_cfg.hostname = json_str(&g_json, json_member(&g_json, root, "hostname"),
                              NULL);
    g_cfg.has_mountns = (flags & CLONE_NEWNS)   != 0;
    g_cfg.has_userns  = (flags & CLONE_NEWUSER) != 0;
    g_cfg.has_utsns   = (flags & CLONE_NEWUTS)  != 0;
    g_cfg.applied = cglimits;
    g_cfg.unsupported = unsupported;

    pid_t pid = clone(container_main, g_cstack + CSTACK_SZ,
                      flags | SIGCHLD, &g_cfg);
    if (pid < 0) {
        printf("[ocirun] clone(0x%x) failed errno=%d\n",
               (unsigned)(flags | SIGCHLD), errno);
        return EX_SETUP;
    }
    close(sync_pipe[0]);
    printf("[ocirun] container pid=%d (host)\n", (int)pid);

    /* ---- parent-side setup the child cannot do for itself ---------------- */
    int ok = 1;
    if (flags & CLONE_NEWUSER) {
        char p[96], line[128];
        int wrote = 0;
        int um = json_path(&g_json, root, "linux.uidMappings");
        int gm = json_path(&g_json, root, "linux.gidMappings");
        /* setgroups must be denied before gid_map may be written by an
         * unprivileged writer; harmless when we are root, so always do it. */
        snprintf(p, sizeof p, "/proc/%d/setgroups", (int)pid);
        (void)write_file(p, "deny");
        for (int pass = 0; pass < 2; pass++) {
            int arr = pass ? gm : um;
            int cnt = json_len(&g_json, arr);
            if (cnt <= 0) continue;
            if (cnt > 1)
                printf("[ocirun]   NOTE: only the first %s entry is written "
                       "(this /proc accepts one line)\n",
                       pass ? "gidMappings" : "uidMappings");
            int e = json_at(&g_json, arr, 0);
            long long cid = json_num(&g_json, json_member(&g_json, e,
                                     "containerID"), 0);
            long long hid = json_num(&g_json, json_member(&g_json, e,
                                     "hostID"), 0);
            long long sz  = json_num(&g_json, json_member(&g_json, e,
                                     "size"), 1);
            snprintf(p, sizeof p, "/proc/%d/%s", (int)pid,
                     pass ? "gid_map" : "uid_map");
            snprintf(line, sizeof line, "%lld %lld %lld\n", cid, hid, sz);
            if (write_file(p, line) != 0) {
                printf("[ocirun] writing %s errno=%d -- the container would "
                       "run with an UNMAPPED identity, so this is fatal\n",
                       p, errno);
                ok = 0; break;
            }
            printf("[ocirun]   %s: %lld %lld %lld\n",
                   pass ? "gid_map" : "uid_map", cid, hid, sz);
            wrote++;
        }
        if (ok && !wrote)
            printf("[ocirun]   NOTE: user namespace with no id mappings -- "
                   "the container will see the overflow uid\n");
    }
    if (ok && cgdir[0]) {
        char pb[24];
        snprintf(pb, sizeof pb, "%d", (int)pid);
        if (cg_write(cgdir, "cgroup.procs", pb) != 0) {
            printf("[ocirun] cgroup: could not add pid %d to %s errno=%d -- "
                   "the limits would not apply, so this is fatal\n",
                   (int)pid, cgdir, errno);
            ok = 0;
        } else {
            printf("[ocirun]   cgroup: pid %d joined %s\n", (int)pid, cgdir);
        }
    }

    /* Release the child (or don't: closing without writing tells it we failed,
     * and it exits rather than running with half a container around it). */
    if (ok) { char go = 'g'; (void)!write(sync_pipe[1], &go, 1); }
    close(sync_pipe[1]);

    int st = 0;
    pid_t w = waitpid(pid, &st, 0);
    if (w != pid) {
        printf("[ocirun] waitpid(%d) returned %d errno=%d\n",
               (int)pid, (int)w, errno);
        return EX_SETUP;
    }
    if (cgdir[0]) rmdir(cgdir);

    if (WIFSIGNALED(st)) {
        printf("[ocirun] container KILLED by signal %d\n", WTERMSIG(st));
        return 128 + WTERMSIG(st);
    }
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    printf("[ocirun] container exited %d\n", code);
    return code;
}
