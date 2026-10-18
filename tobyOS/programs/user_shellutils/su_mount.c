/* su_mount.c -- mount and umount, as util-linux 2.41.3's
 * (logs/utilgate/mount.sh, umount.sh).
 *
 *   mount [-lhV]                       the table, -t TYPES narrowing it
 *   mount -a [options]                 everything /etc/fstab (-T FILE) lists
 *   mount [options] SOURCE | DIRECTORY one fstab entry
 *   mount [options] SOURCE DIRECTORY   SOURCE on DIRECTORY
 *   mount --bind|--rbind|--move OLD NEW, --make-[r]{shared,slave,private,
 *                                       unbindable} DIRECTORY
 *   umount [options] DIRECTORY|SOURCE..., umount -a [options]
 *
 * THE TABLE is /proc/self/mounts (or /proc/mounts): "SOURCE on TARGET type
 * TYPE (OPTIONS)", octal escapes undone ("C:\134" is "C:\"), in the
 * kernel's order -- what util-linux prints from mountinfo comes to the
 * same thing. -l adds labels, and nothing here has one. -t LIST is a
 * comma list of types matched in any case; a "no" before the FIRST one
 * turns the whole list around ("noproc,sysfs" is neither; "-t no" is
 * everything; a later "noproc" is just a name). Listing is refused --
 * "bad usage" -- when -o, -r, -w, -m or an operation is given with
 * nothing to mount.
 *
 * NOT ROOT (what a user is told, measured): one operand is looked up in
 * fstab, by mount point or source -- "X: can't find in /etc/fstab." (status
 * 1; X resolved when it exists), the same wording with -T; --source X "can't find mount source X
 * in /etc/fstab.", --target X "can't find mount point in /etc/fstab.".
 * Two: a missing mount point is "X: mount point does not exist."; else
 * "DIR: must be superuser to use mount." -- both status 32, both followed
 * by an indented "dmesg(1) may have more information after failed mount
 * system call.", DIR made absolute with its links resolved. LABEL=, UUID=,
 * -L and -U name nothing here: "DIR: can't find LABEL=x." (status 1;
 * -L x is LABEL="x"). A propagation change (--make-*) is "DIR: mount
 * failed: Unknown error 5005." -- libmount's own code, printed as it is.
 * -f (--fake) mounts nothing and so needs no privilege; -v says "mount:
 * SOURCE mounted on DIR." ("bound on", "moved to", "propagation flags
 * changed.") on stdout. More than two operands, or an operation with -t,
 * is "bad usage".
 *
 * umount, not root: "DIR: must be superuser to unmount." (32) for a mount
 * point -- or for a source the table names (its last mount point); "DIR:
 * not mounted." (1; not under -q) for anything else that exists; and a
 * path that cannot be resolved ends it at once: "X: No such file or
 * directory", status 1. The statuses of several operands ADD UP (two
 * refusals are 64). Once a mount point has been refused, util-linux's
 * context keeps it: a later existing path is refused as if mounted (named
 * as written), and a missing one is "X: no mount point specified." (32) --
 * kept, as measured.
 * -R takes a mount's submounts first, deepest first ("X: not found", "X:
 * not mounted" with no period, status 1, as the argument was written).
 * -a works from the last mount back, leaving out proc, sysfs and devpts
 * unless -t names them (-t narrows -a only: a mount named outright goes,
 * whatever its type); a mount point it cannot reach is "block devices
 * are not permitted on filesystem.". --fake unmounts nothing; with -v
 * "umount: X unmounted" on stderr, and -a -v lists every mount as "%-25s:
 * ignored" or "successfully unmounted" on stdout. -a with operands is
 * "unexpected number of arguments"; -a with -R "mutually exclusive
 * arguments: --recursive --all".
 *
 * ROOT does it: mount(2) and umount2(2) on Linux (the SU_HOST build, so
 * WSL's root can check this file), and on tobyOS the native syscalls
 * ABI_SYS_MOUNT / ABI_SYS_UMOUNT2 when the ABI defines them -- the kernel's
 * mount and umount2 are the Linux personality's (src/syscall.c LX_mount,
 * LX_umount2), and no native number reaches them yet, so without those
 * this answers ENOSYS ("mount(2) system call failed: Function not
 * implemented."). The kernel's errors become util-linux's words: ENODEV
 * "unknown filesystem type 'T'.", ENOENT "special device S does not
 * exist.", ENOTBLK "S is not a block device.", EBUSY "S already mounted or
 * mount point busy.", EINVAL "wrong fs type, bad option, bad superblock on
 * S, missing codepage or helper program, or other error." (for a remount
 * "mount point not mounted or bad option."); umount's EINVAL "X: not
 * mounted.", EBUSY "DIR: target is busy.", and -v "umount: DIR (SOURCE)
 * unmounted"; root's umount also takes a source the table names when it
 * is no path ("umount tmpfs"). A real propagation change says nothing
 * under -v (only -f's does).
 *
 * The old mount printed /proc/mounts raw and refused everything else; the
 * old umount refused everything. */

#ifdef SU_HOST
#include <sys/mount.h>
#include <sys/syscall.h>
#endif
/* libtoby's errno.h has neither; these are Linux's numbers, which the
 * native ABI's are (ABI_ENODEV is 19) */
#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef ENOTBLK
#define ENOTBLK 15
#endif

#define MT_RDONLY      0x1UL
#define MT_NOSUID      0x2UL
#define MT_NODEV       0x4UL
#define MT_NOEXEC      0x8UL
#define MT_SYNC        0x10UL
#define MT_REMOUNT     0x20UL
#define MT_MANDLOCK    0x40UL
#define MT_DIRSYNC     0x80UL
#define MT_NOSYMFOLLOW 0x100UL
#define MT_NOATIME     0x400UL
#define MT_NODIRATIME  0x800UL
#define MT_BIND        0x1000UL
#define MT_MOVE        0x2000UL
#define MT_REC         0x4000UL
#define MT_SILENT      0x8000UL
#define MT_UNBINDABLE  (1UL << 17)
#define MT_PRIVATE     (1UL << 18)
#define MT_SLAVE       (1UL << 19)
#define MT_SHARED      (1UL << 20)
#define MT_RELATIME    (1UL << 21)
#define MT_STRICTATIME (1UL << 24)
#define MT_LAZYTIME    (1UL << 25)
#define MT_PROPAGATION (MT_UNBINDABLE | MT_PRIVATE | MT_SLAVE | MT_SHARED)

static const char *const mt_dmesg =
    "       dmesg(1) may have more information after failed mount system call.";

/* ---- the system calls ---------------------------------------------------- */

#ifndef SU_HOST
static long mt_sc5(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    __asm__ volatile("syscall" : "=a"(r)
                     : "0"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}
#endif

static int mt_sys_mount(const char *src, const char *tgt, const char *type,
                        unsigned long flags, const char *data) {
#ifdef SU_HOST
    return mount(src, tgt, type, flags, data);
#elif defined(ABI_SYS_MOUNT)
    long r = mt_sc5(ABI_SYS_MOUNT, (long)src, (long)tgt, (long)type, (long)flags, (long)data);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
#else
    (void)src; (void)tgt; (void)type; (void)flags; (void)data;
    (void)mt_sc5;
    errno = ENOSYS;
    return -1;
#endif
}

static int mt_sys_umount(const char *tgt, int flags) {
#ifdef SU_HOST
    return umount2(tgt, flags);
#elif defined(ABI_SYS_UMOUNT2)
    long r = mt_sc5(ABI_SYS_UMOUNT2, (long)tgt, (long)flags, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
#else
    (void)tgt; (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

/* ---- the mount table ------------------------------------------------------ */

struct mt_ent { char *src, *tgt, *type, *opts; };
static struct mt_ent *mt_tab;
static int mt_n;

/* undo the kernel's \NNN escapes, in place */
static void mt_unescape(char *s) {
    char *d = s;
    while (*s) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' &&
            s[3] >= '0' && s[3] <= '7') {
            *d++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
            s += 4;
        } else *d++ = *s++;
    }
    *d = 0;
}

static char *mt_field(char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
    if (!**p || **p == '\n') return 0;
    char *f = *p;
    while (**p && **p != ' ' && **p != '\t' && **p != '\n') (*p)++;
    if (**p) *(*p)++ = 0;
    return f;
}

static void mt_load(void) {
    if (mt_tab) return;
    FILE *f = fopen("/proc/self/mounts", "r");
    if (!f) f = fopen("/proc/mounts", "r");
    int cap = 0;
    mt_tab = (struct mt_ent *)malloc(sizeof *mt_tab);
    if (!f || !mt_tab) { if (f) fclose(f); return; }
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        char *p = line, *a = mt_field(&p), *b = mt_field(&p), *c = mt_field(&p), *d = mt_field(&p);
        if (!a || !b || !c) continue;
        if (mt_n == cap) {
            cap = cap ? cap * 2 : 64;
            struct mt_ent *n = (struct mt_ent *)realloc(mt_tab, (size_t)cap * sizeof *n);
            if (!n) break;
            mt_tab = n;
        }
        mt_unescape(a); mt_unescape(b); mt_unescape(c);
        if (d) mt_unescape(d);
        mt_tab[mt_n].src = strdup(a);
        mt_tab[mt_n].tgt = strdup(b);
        mt_tab[mt_n].type = strdup(c);
        mt_tab[mt_n].opts = strdup(d ? d : "");
        mt_n++;
    }
    fclose(f);
}

/* -t LIST: does TYPE pass? */
static int mt_type_ok(const char *list, const char *type) {
    if (!list) return 1;
    int neg = strncmp(list, "no", 2) == 0, hit = 0;
    char *copy = strdup(list);
    if (!copy) return 1;
    for (char *tok = copy, *next; tok; tok = next) {
        next = strchr(tok, ',');
        if (next) *next++ = 0;
        const char *t = tok;
        if (neg && !strncmp(t, "no", 2)) t += 2;
        if (!strcasecmp(t, type)) hit = 1;
    }
    free(copy);
    return neg ? !hit : hit;
}

/* ---- paths ---------------------------------------------------------------- */

/* PATH made absolute, every "." ".." and symbolic link resolved, into OUT;
 * 0, or -1 with errno (ENOENT for a missing part). libtoby's realpath only
 * joins the working directory on. */
static int mt_canon(const char *path, char *out, size_t cap) {
    char res[4096], work[4096], tmp[8192];
    int links = 0;
    if (!*path) { errno = ENOENT; return -1; }
    if (path[0] != '/') {
        if (!getcwd(res, sizeof res)) return -1;
        if (!strcmp(res, "/")) res[0] = 0;
    } else res[0] = 0;
    snprintf(work, sizeof work, "%s", path);
    char *p = work;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *e = p;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - p);
        if (n == 1 && p[0] == '.') { p = e; continue; }
        if (n == 2 && p[0] == '.' && p[1] == '.') {
            char *s = strrchr(res, '/');
            if (s) *s = 0;
            p = e;
            continue;
        }
        size_t rl = strlen(res);
        if (rl + 1 + n >= sizeof res) { errno = ENAMETOOLONG; return -1; }
        res[rl] = '/';
        memcpy(res + rl + 1, p, n);
        res[rl + 1 + n] = 0;
        p = e;
        struct stat st;
        if (lstat(res, &st) != 0) return -1;
        if (S_ISLNK(st.st_mode)) {
            char lk[4096];
            if (++links > 40) { errno = ELOOP; return -1; }
            ssize_t ln = readlink(res, lk, sizeof lk - 1);
            if (ln < 0) return -1;
            lk[ln] = 0;
            snprintf(tmp, sizeof tmp, "%s%s", lk, p);
            if (strlen(tmp) >= sizeof work) { errno = ENAMETOOLONG; return -1; }
            memcpy(work, tmp, strlen(tmp) + 1);
            p = work;
            if (lk[0] == '/') res[0] = 0;
            else res[rl] = 0;
            continue;
        }
        if (*p && !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    }
    if (!res[0]) snprintf(res, sizeof res, "/");
    if (strlen(res) >= cap) { errno = ENAMETOOLONG; return -1; }
    snprintf(out, cap, "%s", res);
    return 0;
}

/* the last mount on TGT, or -1 */
static int mt_find_target(const char *tgt) {
    mt_load();
    for (int i = mt_n - 1; i >= 0; i--)
        if (!strcmp(mt_tab[i].tgt, tgt)) return i;
    return -1;
}

static int mt_find_source(const char *src) {
    mt_load();
    for (int i = mt_n - 1; i >= 0; i--)
        if (!strcmp(mt_tab[i].src, src)) return i;
    return -1;
}

static int mt_is_root(void) { return geteuid() == 0; }

/* -N NS: a pid's mount namespace (/proc/PID/ns/mnt) or a path to one;
 * 0, or util-linux's message and status 2 */
static int mt_setns(const char *ns) {
    char path[4096];
    const char *q = ns;
    while (isdigit((unsigned char)*q)) q++;
    if (*ns && !*q) snprintf(path, sizeof path, "/proc/%s/ns/mnt", ns);
    else snprintf(path, sizeof path, "%s", ns);
    int fd = open(path, O_RDONLY);
    int e = 0;
    if (fd < 0) e = errno;
    else {
#ifdef SU_HOST
        if (syscall(SYS_setns, fd, 0x00020000) != 0) e = errno;   /* CLONE_NEWNS */
#else
        e = mt_is_root() ? ENOSYS : EPERM;      /* no setns on tobyOS */
#endif
        close(fd);
    }
    if (!e) return 0;
    su_error(0, 0, "failed to set target namespace to %s: %s", path, strerror(e));
    return 2;
}

/* ---- options -------------------------------------------------------------- */

struct mt_req {
    unsigned long flags;        /* MS_* */
    int ro;                     /* -1 untouched, 0 rw, 1 ro */
    char data[1024];            /* the options the filesystem gets */
    int remount;
};

/* -o LIST into R, in order */
static void mt_opts(struct mt_req *r, const char *list) {
    static const struct { const char *name; unsigned long set, clear; } k[] = {
        { "nosuid", MT_NOSUID, 0 }, { "suid", 0, MT_NOSUID },
        { "nodev", MT_NODEV, 0 }, { "dev", 0, MT_NODEV },
        { "noexec", MT_NOEXEC, 0 }, { "exec", 0, MT_NOEXEC },
        { "sync", MT_SYNC, 0 }, { "async", 0, MT_SYNC },
        { "dirsync", MT_DIRSYNC, 0 }, { "mand", MT_MANDLOCK, 0 }, { "nomand", 0, MT_MANDLOCK },
        { "noatime", MT_NOATIME, 0 }, { "atime", 0, MT_NOATIME },
        { "nodiratime", MT_NODIRATIME, 0 }, { "diratime", 0, MT_NODIRATIME },
        { "relatime", MT_RELATIME, 0 }, { "norelatime", 0, MT_RELATIME },
        { "strictatime", MT_STRICTATIME, 0 }, { "lazytime", MT_LAZYTIME, 0 },
        { "nolazytime", 0, MT_LAZYTIME }, { "silent", MT_SILENT, 0 }, { "loud", 0, MT_SILENT },
        { "nosymfollow", MT_NOSYMFOLLOW, 0 },
        { "bind", MT_BIND, 0 }, { "rbind", MT_BIND | MT_REC, 0 }, { "move", MT_MOVE, 0 },
    };
    char *copy = strdup(list);
    if (!copy) return;
    for (char *tok = copy, *next; tok; tok = next) {
        next = strchr(tok, ',');
        if (next) *next++ = 0;
        if (!*tok || !strcmp(tok, "defaults")) continue;
        if (!strcmp(tok, "ro")) { r->ro = 1; continue; }
        if (!strcmp(tok, "rw")) { r->ro = 0; continue; }
        if (!strcmp(tok, "remount")) { r->remount = 1; r->flags |= MT_REMOUNT; continue; }
        /* fstab-only words the kernel never sees */
        if (!strcmp(tok, "auto") || !strcmp(tok, "noauto") || !strcmp(tok, "user") ||
            !strcmp(tok, "nouser") || !strcmp(tok, "users") || !strcmp(tok, "owner") ||
            !strcmp(tok, "group") || !strcmp(tok, "nofail") || !strcmp(tok, "_netdev") ||
            !strncmp(tok, "x-", 2) || !strncmp(tok, "X-", 2))
            continue;
        int known = 0;
        for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
            if (strcmp(tok, k[i].name)) continue;
            r->flags = (r->flags | k[i].set) & ~k[i].clear;
            known = 1;
            break;
        }
        if (known) continue;
        size_t dl = strlen(r->data);
        snprintf(r->data + dl, sizeof r->data - dl, "%s%s", dl ? "," : "", tok);
    }
    free(copy);
}

/* ---- fstab ---------------------------------------------------------------- */

struct mt_fs { char *src, *tgt, *type, *opts; };

/* FILE's entries; -1 (said) when an explicit -T FILE cannot be read */
static int mt_fstab(const char *file, int explicit_file, struct mt_fs **out) {
    *out = 0;
    FILE *f = fopen(file, "r");
    if (!f) {
        if (!explicit_file) return 0;
        su_error(0, 0, "%s: failed to parse", file);
        return -1;
    }
    int n = 0, cap = 0, lineno = 0;
    char line[4096];
    struct mt_fs *v = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '#') continue;
        char *a = mt_field(&p), *b = mt_field(&p), *c = mt_field(&p), *d = mt_field(&p);
        if (!a || !b) {
            su_error(0, 0, "%s: parse error at line %d -- ignored", file, lineno);
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            struct mt_fs *nv = (struct mt_fs *)realloc(v, (size_t)cap * sizeof *nv);
            if (!nv) break;
            v = nv;
        }
        mt_unescape(a); mt_unescape(b);
        v[n].src = strdup(a);
        v[n].tgt = strdup(b);
        v[n].type = strdup(c ? c : "auto");
        v[n].opts = strdup(d ? d : "defaults");
        n++;
    }
    fclose(f);
    *out = v;
    return n;
}

static int mt_has_opt(const char *opts, const char *want) {
    size_t wl = strlen(want);
    for (const char *p = opts; p && *p; ) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == wl && !strncmp(p, want, wl)) return 1;
        p = e ? e + 1 : 0;
    }
    return 0;
}

/* ---- mount ---------------------------------------------------------------- */

enum { MT_OP_NONE, MT_OP_BIND, MT_OP_RBIND, MT_OP_MOVE, MT_OP_PROP };

struct mt_ctx {
    int fake, verbose, op;
    unsigned long prop;         /* the propagation flags of --make-* */
    const char *types;
    struct mt_req req;
};

/* is SRC a name a device has (LABEL=, UUID=, ...)? */
static int mt_tagged(const char *src) {
    return !strncmp(src, "LABEL=", 6) || !strncmp(src, "UUID=", 5) ||
           !strncmp(src, "PARTLABEL=", 10) || !strncmp(src, "PARTUUID=", 9) ||
           !strncmp(src, "ID=", 3);
}

/* the first filesystem /proc/filesystems lists that takes a device, after
 * PREV (NULL: the first) -- what util-linux tries with no -t */
static int mt_next_type(int idx, char *out, size_t cap) {
    FILE *f = fopen("/proc/filesystems", "r");
    if (!f) return -1;
    char line[256];
    int k = 0, rc = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "nodev", 5)) continue;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        p[strcspn(p, "\n")] = 0;
        if (!*p) continue;
        if (k++ == idx) { snprintf(out, cap, "%s", p); rc = 0; break; }
    }
    fclose(f);
    return rc;
}

/* mount SRC (NULL: none) on TGT as written; util-linux's words for what
 * goes wrong; the status */
static int mt_mount_one(struct mt_ctx *x, const char *src, const char *tgt,
                        const char *type, const char *tgt_shown) {
    char ctgt[4096], csrc[4096];
    int bind = x->op == MT_OP_BIND || x->op == MT_OP_RBIND || (x->req.flags & MT_BIND);
    if (src && mt_tagged(src)) {
        su_error(0, 0, "%s: can't find %s.", tgt_shown, src);
        return 1;
    }
    int have_tgt = mt_canon(tgt, ctgt, sizeof ctgt) == 0;
    const char *shown_tgt = have_tgt ? ctgt : tgt;
    /* a remount of something the table does not hold names no source,
     * and util-linux prints the null pointer as glibc does */
    const char *shown_src = src ? src : x->req.remount ? "(null)" : "none";
    if ((bind || x->op == MT_OP_MOVE) && src && mt_canon(src, csrc, sizeof csrc) == 0)
        shown_src = csrc;
    if (x->fake) {
        if (x->verbose) {
            if (x->op == MT_OP_PROP) printf("mount: %s propagation flags changed.\n", shown_tgt);
            else if (x->op == MT_OP_MOVE) printf("mount: %s moved to %s.\n", shown_src, shown_tgt);
            else if (bind) printf("mount: %s bound on %s.\n", shown_src, shown_tgt);
            else printf("mount: %s mounted on %s.\n", shown_src, shown_tgt);
        }
        return 0;
    }
    if (x->op == MT_OP_PROP) {
        if (!mt_is_root() || mt_sys_mount("none", shown_tgt, 0, x->prop, 0) != 0) {
            su_error(0, 0, "%s: mount failed: Unknown error 5005.", shown_tgt);
            return 32;
        }
        return 0;       /* -v says nothing of a real change, only of a -f one */
    }
    if (!have_tgt) {
        su_error(0, 0, "%s: mount point does not exist.", tgt);
        fprintf(stderr, "%s\n", mt_dmesg);
        return 32;
    }
    if (!mt_is_root()) {
        su_error(0, 0, "%s: must be superuser to use mount.", ctgt);
        fprintf(stderr, "%s\n", mt_dmesg);
        return 32;
    }
    unsigned long fl = x->req.flags;
    if (x->req.ro == 1) fl |= MT_RDONLY;
    if (x->op == MT_OP_BIND) fl |= MT_BIND;
    if (x->op == MT_OP_RBIND) fl |= MT_BIND | MT_REC;
    if (x->op == MT_OP_MOVE) fl |= MT_MOVE;
    const char *data = x->req.data[0] ? x->req.data : 0;
    const char *ksrc = (bind || x->op == MT_OP_MOVE) ? shown_src : (src ? src : "none");
    int r, e = 0;
    char probe[256];
    if (!type && !bind && x->op != MT_OP_MOVE && !x->req.remount) {
#ifdef SU_HOST
        /* with no type, a source that is not there is said so first */
        struct stat ss;
        if (stat(ksrc, &ss) != 0) {
            su_error(0, 0, "%s: special device %s does not exist.", ctgt, ksrc);
            fprintf(stderr, "%s\n", mt_dmesg);
            return 32;
        }
#endif
        r = -1;
        e = ENODEV;
        for (int i = 0; mt_next_type(i, probe, sizeof probe) == 0; i++) {
            r = mt_sys_mount(ksrc, ctgt, probe, fl, data);
            if (r == 0) break;
            e = errno;
            if (e != EINVAL && e != ENODEV) break;
        }
        if (r != 0) errno = e;
    } else {
        r = mt_sys_mount(ksrc, ctgt, type, fl, data);
    }
    if (r == 0) {
        if (x->verbose) {
            if (x->op == MT_OP_MOVE) printf("mount: %s moved to %s.\n", shown_src, ctgt);
            else if (bind) printf("mount: %s bound on %s.\n", shown_src, ctgt);
            else printf("mount: %s mounted on %s.\n", shown_src, ctgt);
        }
        return 0;
    }
    e = errno;
    struct stat ss;
    switch (e) {
    case ENODEV:
        su_error(0, 0, "%s: unknown filesystem type '%s'.", ctgt, type ? type : "");
        break;
    case ENOENT:
        su_error(0, 0, "%s: special device %s does not exist.", ctgt, ksrc);
        break;
    case ENOTBLK:
        if (stat(ksrc, &ss) == 0 && S_ISREG(ss.st_mode) && mt_canon(ksrc, csrc, sizeof csrc) == 0)
            su_error(0, 0, "%s: %s is not a block device; try \"-o loop\".", ctgt, csrc);
        else
            su_error(0, 0, "%s: %s is not a block device.", ctgt, ksrc);
        break;
    case EBUSY:
        su_error(0, 0, "%s: %s already mounted or mount point busy.", ctgt, ksrc);
        break;
    case EINVAL:
        if (x->req.remount)
            su_error(0, 0, "%s: mount point not mounted or bad option.", ctgt);
        else
            su_error(0, 0, "%s: wrong fs type, bad option, bad superblock on %s, "
                     "missing codepage or helper program, or other error.", ctgt, ksrc);
        break;
    case EPERM:
    case EACCES:
        su_error(0, 0, "%s: must be superuser to use mount.", ctgt);
        break;
    case EROFS:
        su_error(0, 0, "%s: cannot mount %s read-only.", ctgt, ksrc);
        break;
    default:
        su_error(0, 0, "%s: mount(2) system call failed: %s.", ctgt, strerror(e));
        break;
    }
    fprintf(stderr, "%s\n", mt_dmesg);
    return 32;
}

static void mt_bad_usage(void) __attribute__((noreturn));
static void mt_bad_usage(void) {
    su_error(0, 0, "bad usage");
    su_try_help(1);
}

/* the table, as `mount` lists it */
static int mt_list(const char *types) {
    mt_load();
    for (int i = 0; i < mt_n; i++) {
        if (!mt_type_ok(types, mt_tab[i].type)) continue;
        printf("%s on %s type %s (%s)\n", mt_tab[i].src, mt_tab[i].tgt, mt_tab[i].type,
               mt_tab[i].opts);
    }
    return 0;
}

/* an fstab entry, mounted as the command line says on top of it */
static int mt_mount_entry(struct mt_ctx *x, const struct mt_fs *e) {
    struct mt_ctx y = *x;
    struct mt_req base;
    memset(&base, 0, sizeof base);
    base.ro = -1;
    mt_opts(&base, e->opts);
    /* the command line's options after the entry's */
    base.flags |= x->req.flags;
    if (x->req.ro >= 0) base.ro = x->req.ro;
    if (x->req.remount) base.remount = 1;
    if (x->req.data[0]) {
        size_t dl = strlen(base.data);
        snprintf(base.data + dl, sizeof base.data - dl, "%s%s", dl ? "," : "", x->req.data);
    }
    y.req = base;
    const char *type = strcmp(e->type, "auto") ? e->type : 0;
    return mt_mount_one(&y, e->src, e->tgt, x->types ? x->types : type, e->tgt);
}

static int mt_mount_all(struct mt_ctx *x, const char *fstab, int explicit_file) {
    struct mt_fs *v;
    int n = mt_fstab(fstab, explicit_file, &v);
    if (n < 0) return 1;
    int ok = 0, bad = 0;
    for (int i = 0; i < n; i++) {
        if (mt_has_opt(v[i].opts, "noauto") || !mt_type_ok(x->types, v[i].type) ||
            !strcmp(v[i].tgt, "none") || !strcmp(v[i].tgt, "swap")) {
            if (x->verbose) printf("%-25s: ignored\n", v[i].tgt);
            continue;
        }
        char c[4096];
        if (mt_canon(v[i].tgt, c, sizeof c) == 0 && mt_find_target(c) >= 0 && !x->req.remount &&
            mt_find_source(v[i].src) >= 0 && !strcmp(mt_tab[mt_find_source(v[i].src)].tgt, c)) {
            if (x->verbose) printf("%-25s: already mounted\n", v[i].tgt);
            continue;
        }
        struct mt_ctx y = *x;
        y.verbose = 0;
        if (mt_mount_entry(&y, &v[i]) == 0) {
            ok++;
            if (x->verbose) printf("%-25s: successfully mounted\n", v[i].tgt);
        } else bad++;
    }
    return bad ? (ok ? 64 : 32) : 0;
}

static int do_mount(int argc, char **argv) {
    enum { SOURCE_OPT = 0x100, TARGET_OPT, TPFX_OPT, SHARED_OPT, SLAVE_OPT, PRIVATE_OPT,
           UNBIND_OPT, RSHARED_OPT, RSLAVE_OPT, RPRIVATE_OPT, RUNBIND_OPT, MAPG_OPT,
           MAPU_OPT, ONCE_OPT, OMODE_OPT, OSRC_OPT, OSRCF_OPT };
    static const struct su_option longs[] = {
        { "all", SU_NO_ARG, 0, 'a' },
        { "no-mtab", SU_NO_ARG, 0, 'n' },
        { "fake", SU_NO_ARG, 0, 'f' },
        { "fstab", SU_REQ_ARG, 0, 'T' },
        { "fork", SU_NO_ARG, 0, 'F' },
        { "help", SU_NO_ARG, 0, 'h' },
        { "no-canonicalize", SU_NO_ARG, 0, 'c' },
        { "internal-only", SU_NO_ARG, 0, 'i' },
        { "show-labels", SU_NO_ARG, 0, 'l' },
        { "read-only", SU_NO_ARG, 0, 'r' },
        { "read-write", SU_NO_ARG, 0, 'w' },
        { "rw", SU_NO_ARG, 0, 'w' },
        { "options", SU_REQ_ARG, 0, 'o' },
        { "test-opts", SU_REQ_ARG, 0, 'O' },
        { "types", SU_REQ_ARG, 0, 't' },
        { "source", SU_REQ_ARG, 0, SOURCE_OPT },
        { "target", SU_REQ_ARG, 0, TARGET_OPT },
        { "target-prefix", SU_REQ_ARG, 0, TPFX_OPT },
        { "verbose", SU_NO_ARG, 0, 'v' },
        { "version", SU_NO_ARG, 0, 'V' },
        { "label", SU_REQ_ARG, 0, 'L' },
        { "uuid", SU_REQ_ARG, 0, 'U' },
        { "bind", SU_NO_ARG, 0, 'B' },
        { "move", SU_NO_ARG, 0, 'M' },
        { "rbind", SU_NO_ARG, 0, 'R' },
        { "make-shared", SU_NO_ARG, 0, SHARED_OPT },
        { "make-slave", SU_NO_ARG, 0, SLAVE_OPT },
        { "make-private", SU_NO_ARG, 0, PRIVATE_OPT },
        { "make-unbindable", SU_NO_ARG, 0, UNBIND_OPT },
        { "make-rshared", SU_NO_ARG, 0, RSHARED_OPT },
        { "make-rslave", SU_NO_ARG, 0, RSLAVE_OPT },
        { "make-rprivate", SU_NO_ARG, 0, RPRIVATE_OPT },
        { "make-runbindable", SU_NO_ARG, 0, RUNBIND_OPT },
        { "map-groups", SU_REQ_ARG, 0, MAPG_OPT },
        { "map-users", SU_REQ_ARG, 0, MAPU_OPT },
        { "mkdir", SU_OPT_ARG, 0, 'm' },
        { "onlyonce", SU_NO_ARG, 0, ONCE_OPT },
        { "options-mode", SU_REQ_ARG, 0, OMODE_OPT },
        { "options-source", SU_REQ_ARG, 0, OSRC_OPT },
        { "options-source-force", SU_NO_ARG, 0, OSRCF_OPT },
        { "namespace", SU_REQ_ARG, 0, 'N' },
        { 0, 0, 0, 0 }
    };
    struct mt_ctx x;
    memset(&x, 0, sizeof x);
    x.req.ro = -1;
    int all = 0, opts_given = 0, rw_given = 0, mkdir_given = 0, c;
    const char *fstab = "/etc/fstab", *src_opt = 0, *tgt_opt = 0, *ns = 0;
    int explicit_fstab = 0;
    char tag[1024] = "";
    while ((c = su_getopt_long(argc, argv, "aBcfFhilL:Mm::nN:o:O:rRsT:t:U:vVw", longs, 0)) != -1) {
        switch (c) {
        case 'a': all = 1; break;
        case 'f': x.fake = 1; break;
        case 'v': x.verbose = 1; break;
        case 'n': case 'c': case 'i': case 'l': case 's': case 'F': case 'O':
        case TPFX_OPT: case MAPG_OPT: case MAPU_OPT: case ONCE_OPT: case OMODE_OPT:
        case OSRC_OPT: case OSRCF_OPT:
            break;
        case 'N': ns = su_optarg; break;
        case 'T': fstab = su_optarg; explicit_fstab = 1; break;
        case 't': x.types = su_optarg; break;
        case 'o': opts_given = 1; mt_opts(&x.req, su_optarg); break;
        case 'r': rw_given = 1; x.req.ro = 1; break;
        case 'w': rw_given = 1; x.req.ro = 0; break;
        case 'm': mkdir_given = 1; break;
        case 'B': x.op = MT_OP_BIND; break;
        case 'R': x.op = MT_OP_RBIND; break;
        case 'M': x.op = MT_OP_MOVE; break;
        case SHARED_OPT: x.op = MT_OP_PROP; x.prop = MT_SHARED; break;
        case SLAVE_OPT: x.op = MT_OP_PROP; x.prop = MT_SLAVE; break;
        case PRIVATE_OPT: x.op = MT_OP_PROP; x.prop = MT_PRIVATE; break;
        case UNBIND_OPT: x.op = MT_OP_PROP; x.prop = MT_UNBINDABLE; break;
        case RSHARED_OPT: x.op = MT_OP_PROP; x.prop = MT_SHARED | MT_REC; break;
        case RSLAVE_OPT: x.op = MT_OP_PROP; x.prop = MT_SLAVE | MT_REC; break;
        case RPRIVATE_OPT: x.op = MT_OP_PROP; x.prop = MT_PRIVATE | MT_REC; break;
        case RUNBIND_OPT: x.op = MT_OP_PROP; x.prop = MT_UNBINDABLE | MT_REC; break;
        case 'L': snprintf(tag, sizeof tag, "LABEL=\"%s\"", su_optarg); break;
        case 'U': snprintf(tag, sizeof tag, "UUID=\"%s\"", su_optarg); break;
        case SOURCE_OPT: src_opt = su_optarg; break;
        case TARGET_OPT: tgt_opt = su_optarg; break;
        case 'h':
            printf("\n"
                   "Usage:\n"
                   " %s [-lhV]\n"
                   " %s -a [options]\n"
                   " %s [options] [--source] <source> | [--target] <directory>\n"
                   " %s [options] <source> <directory>\n"
                   " %s <operation> <mountpoint> [<target>]\n"
                   "\n"
                   "Attach a filesystem to the directory tree, or list what is attached.\n"
                   "\n"
                   "Options:\n"
                   " -a, --all               mount every fstab entry not marked noauto\n"
                   " -c, --no-canonicalize   take paths as they are written\n"
                   " -f, --fake              do everything but the mount(2) call\n"
                   " -F, --fork              one process per device (with -a)\n"
                   " -T, --fstab <path>      read this file instead of /etc/fstab\n"
                   " -i, --internal-only     never run a mount.<type> helper\n"
                   " -l, --show-labels       add each filesystem's label to the list\n"
                   " -m, --mkdir[=<mode>]    make the mount point first ('-o X-mount.mkdir')\n"
                   " -n, --no-mtab           leave /etc/mtab alone\n"
                   " -o, --options <list>    mount options, separated by commas\n"
                   " -O, --test-opts <list>  only the fstab entries with these options (-a)\n"
                   " -r, --read-only         mount it read-only (-o ro)\n"
                   " -t, --types <list>      only these filesystem types\n"
                   "     --source <src>      say outright that this is the source\n"
                   "     --target <target>   say outright that this is the mount point\n"
                   " -v, --verbose           say what is done\n"
                   " -w, --rw, --read-write  mount it read-write (the default)\n"
                   "\n"
                   " -h, --help              show this help\n"
                   " -V, --version           show the version\n"
                   "\n"
                   "Source:\n"
                   " -L, --label <label>     the same as LABEL=<label>\n"
                   " -U, --uuid <uuid>       the same as UUID=<uuid>\n"
                   " <device>                a device, by its path\n"
                   " <directory>             a directory, for --bind and --rbind\n"
                   "\n"
                   "Operations:\n"
                   " -B, --bind              show a subtree in a second place too (-o bind)\n"
                   " -M, --move              move a subtree to another place\n"
                   " -R, --rbind             --bind, with everything mounted under it\n"
                   " --make-shared           make a subtree shared\n"
                   " --make-slave            make a subtree a slave\n"
                   " --make-private          make a subtree private\n"
                   " --make-unbindable       make a subtree unbindable\n"
                   " --make-rshared          the same, for the subtree and all below it\n"
                   " --make-rslave           the same, for the subtree and all below it\n"
                   " --make-rprivate         the same, for the subtree and all below it\n"
                   " --make-runbindable      the same, for the subtree and all below it\n",
                   su_short, su_short, su_short, su_short, su_short);
            return 0;
        case 'V': return su_version();
        default: su_try_help(1);
        }
    }
    if (ns && mt_setns(ns)) return 2;
    int nargs = argc - su_optind;
    char **args = argv + su_optind;
    if (x.op != MT_OP_NONE && x.types) mt_bad_usage();
    const char *src = src_opt ? src_opt : (tag[0] ? tag : 0);
    if (all) {
        if (nargs || src || tgt_opt) mt_bad_usage();
        return mt_mount_all(&x, fstab, explicit_fstab);
    }
    int named = nargs + (src ? 1 : 0) + (tgt_opt ? 1 : 0);
    if (named == 0) {
        if (opts_given || rw_given || x.op != MT_OP_NONE || mkdir_given) mt_bad_usage();
        return mt_list(x.types);
    }
    if (named > 2) mt_bad_usage();
    if (named == 2) {
        const char *s = src ? src : args[0];
        const char *t = tgt_opt ? tgt_opt : args[src ? 0 : 1];
        if (tgt_opt && !src) s = args[0];
        return mt_mount_one(&x, s, t, x.types, t);
    }
    /* one: a mount point for an operation, else an fstab entry */
    const char *one = src ? src : tgt_opt ? tgt_opt : args[0];
    if (!src && (x.op == MT_OP_PROP || x.op == MT_OP_BIND || x.op == MT_OP_RBIND ||
                 x.op == MT_OP_MOVE || x.req.remount)) {
        const char *rsrc = 0;
        char c1[4096];
        if (x.req.remount && mt_canon(one, c1, sizeof c1) == 0) {
            int k = mt_find_target(c1);
            if (k >= 0) rsrc = mt_tab[k].src;
        }
        return mt_mount_one(&x, rsrc, one, x.types, one);
    }
    struct mt_fs *v;
    int n = mt_fstab(fstab, explicit_fstab, &v);
    if (n < 0) return 1;
    char c1[4096];
    int have_c = mt_canon(one, c1, sizeof c1) == 0;
    for (int i = 0; i < n; i++) {
        int by_tgt = !src && (!strcmp(v[i].tgt, one) || (have_c && !strcmp(v[i].tgt, c1)));
        int by_src = !tgt_opt && !strcmp(v[i].src, one);
        if (by_tgt || by_src) return mt_mount_entry(&x, &v[i]);
    }
    /* named as written -- or resolved, when it is there to resolve */
    const char *shown = have_c ? c1 : one;
    if (src) su_error(0, 0, "%s: can't find mount source %s in /etc/fstab.", shown, shown);
    else if (tgt_opt) su_error(0, 0, "%s: can't find mount point in /etc/fstab.", shown);
    else su_error(0, 0, "%s: can't find in /etc/fstab.", shown);
    return 1;
}

/* ---- umount --------------------------------------------------------------- */

struct mt_uctx {
    int fake, verbose, quiet, flags, recursive;
    const char *types;
    int sticky;                 /* a mount point was refused: see the header */
};

/* unmount the mount TABLE[K] (its point CANON) as root; the status */
static int mt_umount_real(struct mt_uctx *u, const char *arg, const char *canon, int k, int quiet_src) {
    if (mt_sys_umount(canon, u->flags) == 0) {
        if (u->verbose) {
            fflush(stdout);
            if (quiet_src || k < 0) fprintf(stderr, "umount: %s unmounted\n", canon);
            else fprintf(stderr, "umount: %s (%s) unmounted\n", canon, mt_tab[k].src);
        }
        return 0;
    }
    int e = errno;
    switch (e) {
    case EINVAL: if (!u->quiet) su_error(0, 0, "%s: not mounted.", arg); break;
    case EBUSY: su_error(0, 0, "%s: target is busy.", canon); break;
    case EPERM: case EACCES: su_error(0, 0, "%s: must be superuser to unmount.", canon); break;
    case ENOENT: su_error(0, 0, "%s: no mount point specified.", arg); break;
    default: su_error(0, 0, "%s: umount failed: %s.", canon, strerror(e)); break;
    }
    return 32;
}

/* -R: TABLE[K]'s submounts, deepest (last mounted) first, then it */
static int mt_umount_tree(struct mt_uctx *u, const char *canon) {
    size_t cl = strlen(canon);
    for (int i = mt_n - 1; i >= 0; i--) {
        const char *t = mt_tab[i].tgt;
        int under = !strcmp(canon, "/") ? strcmp(t, "/") != 0
                                        : (!strncmp(t, canon, cl) && t[cl] == '/');
        if (!under) continue;
        if (u->fake) {
            if (u->verbose) { fflush(stdout); fprintf(stderr, "umount: %s unmounted\n", t); }
            continue;
        }
        if (!mt_is_root()) {
            su_error(0, 0, "%s: must be superuser to unmount.", t);
            return 32;
        }
        int r = mt_umount_real(u, t, t, i, 1);
        if (r) return r;
    }
    if (u->fake) {
        if (u->verbose) { fflush(stdout); fprintf(stderr, "umount: %s unmounted\n", canon); }
        return 0;
    }
    if (!mt_is_root()) {
        su_error(0, 0, "%s: must be superuser to unmount.", canon);
        return 32;
    }
    return mt_umount_real(u, canon, canon, -1, 1);
}

/* one operand; the status it adds */
static int mt_umount_one(struct mt_uctx *u, const char *arg) {
    char canon[4096];
    mt_load();
    if (u->recursive) {
        if (mt_canon(arg, canon, sizeof canon) != 0) {
            su_error(0, 0, "%s: not found", arg);
            return 1;
        }
        if (mt_find_target(canon) < 0) {
            if (!u->quiet) su_error(0, 0, "%s: not mounted", arg);
            return 1;
        }
        return mt_umount_tree(u, canon);
    }
    if (u->fake) {
        if (u->verbose) {
            fflush(stdout);
            fprintf(stderr, "umount: %s unmounted\n", arg);
        }
        return 0;
    }
    int have = mt_canon(arg, canon, sizeof canon) == 0;
    int e = errno;
    if (mt_is_root()) {
        int k = have ? mt_find_target(canon) : -1;
        if (!have) {
            /* not a path: perhaps a source the table names */
            k = mt_find_source(arg);
            if (k < 0) {
                su_error(0, 0, "%s: no mount point specified.", arg);
                return 32;
            }
            snprintf(canon, sizeof canon, "%s", mt_tab[k].tgt);
        }
        if (k < 0) {
            int s = mt_find_source(arg);
            if (s < 0) {
                if (!u->quiet) su_error(0, 0, "%s: not mounted.", arg);
                return 32;
            }
            k = s;
            snprintf(canon, sizeof canon, "%s", mt_tab[s].tgt);
        }
        /* -t narrows -a only: a mount named outright goes whatever it is */
        int r = mt_umount_real(u, arg, canon, k, 0);
        if (r == 0) {
            /* the table changed: read it again for the next operand */
            mt_tab = 0;
            mt_n = 0;
        }
        return r;
    }
    if (u->sticky) {
        /* the refused mount is still in util-linux's context: see above */
        if (!have) su_error(0, 0, "%s: no mount point specified.", arg);
        else su_error(0, 0, "%s: must be superuser to unmount.", arg);
        return 32;
    }
    if (!have) {
        su_error(0, 0, "%s: %s", arg, strerror(e));
        exit(1);
    }
    int k = mt_find_target(canon);
    if (k < 0) {
        k = mt_find_source(arg);
        if (k >= 0) snprintf(canon, sizeof canon, "%s", mt_tab[k].tgt);
    }
    if (k < 0) {
        if (!u->quiet) su_error(0, 0, "%s: not mounted.", canon);
        return 1;
    }
    su_error(0, 0, "%s: must be superuser to unmount.", canon);
    u->sticky = 1;
    return 32;
}

static int mt_umount_all(struct mt_uctx *u) {
    mt_load();
    int n = mt_n, rc = 0;
    struct mt_ent *tab = mt_tab;
    for (int i = n - 1; i >= 0; i--) {
        const char *t = tab[i].tgt;
        int pick = u->types ? mt_type_ok(u->types, tab[i].type)
                            : !(mt_type_ok("proc,sysfs,devpts,devfs,rpc_pipefs,nfsd", tab[i].type));
        if (u->fake) {
            if (u->verbose) printf("%-25s: %s\n", t, pick ? "successfully unmounted" : "ignored");
            continue;
        }
        if (!pick) continue;
        if (!mt_is_root()) {
            struct stat st;
            if (stat(t, &st) != 0)
                su_error(0, 0, "%s: block devices are not permitted on filesystem.", t);
            else
                su_error(0, 0, "%s: must be superuser to unmount.", t);
            rc = 32;
            continue;
        }
        int r = mt_umount_real(u, t, t, i, 0);
        if (r) rc = 32;
    }
    return rc;
}

static int do_umount(int argc, char **argv) {
    enum { FAKE_OPT = 0x100 };
    static const struct su_option longs[] = {
        { "all", SU_NO_ARG, 0, 'a' },
        { "all-targets", SU_NO_ARG, 0, 'A' },
        { "no-canonicalize", SU_NO_ARG, 0, 'c' },
        { "detach-loop", SU_NO_ARG, 0, 'd' },
        { "fake", SU_NO_ARG, 0, FAKE_OPT },
        { "force", SU_NO_ARG, 0, 'f' },
        { "internal-only", SU_NO_ARG, 0, 'i' },
        { "lazy", SU_NO_ARG, 0, 'l' },
        { "no-mtab", SU_NO_ARG, 0, 'n' },
        { "quiet", SU_NO_ARG, 0, 'q' },
        { "read-only", SU_NO_ARG, 0, 'r' },
        { "recursive", SU_NO_ARG, 0, 'R' },
        { "test-opts", SU_REQ_ARG, 0, 'O' },
        { "types", SU_REQ_ARG, 0, 't' },
        { "verbose", SU_NO_ARG, 0, 'v' },
        { "version", SU_NO_ARG, 0, 'V' },
        { "help", SU_NO_ARG, 0, 'h' },
        { "namespace", SU_REQ_ARG, 0, 'N' },
        { 0, 0, 0, 0 }
    };
    struct mt_uctx u;
    memset(&u, 0, sizeof u);
    int all = 0, c;
    const char *ns = 0;
    while ((c = su_getopt_long(argc, argv, "aAcdfhilnN:O:qrRt:vV", longs, 0)) != -1) {
        switch (c) {
        case 'a': all = 1; break;
        case 'f': u.flags |= 1; break;           /* MNT_FORCE */
        case 'l': u.flags |= 2; break;           /* MNT_DETACH */
        case 'q': u.quiet = 1; break;
        case 'R': u.recursive = 1; break;
        case 't': u.types = su_optarg; break;
        case 'v': u.verbose = 1; break;
        case FAKE_OPT: u.fake = 1; break;
        case 'A': case 'c': case 'd': case 'i': case 'n': case 'O': case 'r': break;
        case 'N': ns = su_optarg; break;
        case 'h':
            printf("\n"
                   "Usage:\n"
                   " %s [-hV]\n"
                   " %s -a [options]\n"
                   " %s [options] <source> | <directory>\n"
                   "\n"
                   "Detach filesystems from the directory tree.\n"
                   "\n"
                   "Options:\n"
                   " -a, --all               unmount everything mounted\n"
                   " -A, --all-targets       every mount point of the device given, in this\n"
                   "                           namespace\n"
                   " -c, --no-canonicalize   take paths as they are written\n"
                   " -d, --detach-loop       free the loop device too, if it was one\n"
                   "     --fake              do everything but the umount(2) call\n"
                   " -f, --force             force it (an NFS server that is gone)\n"
                   " -i, --internal-only     never run an umount.<type> helper\n"
                   " -n, --no-mtab           leave /etc/mtab alone\n"
                   " -l, --lazy              detach it now, and tidy up when it is idle\n"
                   " -O, --test-opts <list>  only mounts with these options (with -a)\n"
                   " -R, --recursive         the mount and everything mounted under it\n"
                   " -r, --read-only         when it cannot be unmounted, make it read-only\n"
                   " -t, --types <list>      only these filesystem types\n"
                   " -v, --verbose           say what is done\n"
                   " -q, --quiet             say nothing of 'not mounted'\n"
                   "\n"
                   " -h, --help              show this help\n"
                   " -V, --version           show the version\n",
                   su_short, su_short, su_short);
            return 0;
        case 'V': return su_version();
        default: su_try_help(1);
        }
    }
    if (ns && mt_setns(ns)) return 2;
    int nargs = argc - su_optind;
    if (all) {
        if (u.recursive) {
            su_error(0, 0, "mutually exclusive arguments: --recursive --all");
            return 1;
        }
        if (nargs) {
            su_error(0, 0, "unexpected number of arguments");
            su_try_help(1);
        }
        return mt_umount_all(&u);
    }
    if (!nargs) {
        su_error(0, 0, "bad usage");
        su_try_help(1);
    }
    int rc = 0;
    for (int i = su_optind; i < argc; i++) rc += mt_umount_one(&u, argv[i]);
    return rc;
}
