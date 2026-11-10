/* su_cp.c -- cp, as GNU coreutils 9.7's (logs/utilgate/cp.sh),
 * and the copy engine mv uses when a rename crosses filesystems.
 *
 *   cp [OPTION]... [-T] SOURCE DEST
 *   cp [OPTION]... SOURCE... DIRECTORY
 *   cp [OPTION]... -t DIRECTORY SOURCE...
 *
 * Measured on GNU's cp (gnucp in WSL, with strace for the order of
 * things), case by case in the fixture:
 *
 *   - Symbolic links: followed on the command line and not below it with
 *     -R (unless -L; -H follows the command line only; -P/-d never); a
 *     link is copied as a link when not followed. With -l and no -P/-H,
 *     links are followed everywhere, as GNU does -- a dangling one is
 *     "cannot stat", a link to an ancestor "cannot copy cyclic symbolic
 *     link". A destination that is a link is written through, unless it
 *     dangles ("not writing through dangling symlink").
 *   - A new file's mode is the source's less the umask, its set-id and
 *     sticky bits dropped (a directory keeps sticky); -p/--preserve keep
 *     mode, ownership (silently not, for someone who may not give it) and
 *     times, "links" keeps hard links within one cp, "xattr" copies the
 *     extended attributes (finding 215: they were never copied; -a and
 *     "all" copy them too, saying nothing of one that cannot be set,
 *     where xattr named outright says "setting attributes for 'D'") and
 *     "context" is refused at once ("cannot
 *     preserve security context without an SELinux-enabled kernel")
 *     unless it came only from -a or "all". An existing file keeps its
 *     mode without -p; --no-preserve=mode gives a new one 0666 (0777 for
 *     a directory) less the umask.
 *   - Directories: made with the source's mode (less the umask) once
 *     filled; entries copied in inode order (GNU sorts them so); one that
 *     cannot be read is "cannot access 'D'", its copy left with the
 *     source's mode. -x copies a directory on another device empty.
 *   - -n (with its deprecation warning, printed for every -n), -u and
 *     --update=all|none|none-fail|older; -i ("overwrite 'F'?", or
 *     "unwritable 'F' (mode 0444, r--r--r--); try anyway?", or with -f
 *     "replace 'F', overriding mode ..."); a "no" makes the status 1.
 *   - -b/--backup=CONTROL, -S, VERSION_CONTROL and SIMPLE_BACKUP_SUFFIX:
 *     none/off, simple/never, existing/nil, numbered/t, abbreviations
 *     accepted; numbered backups count from the highest NAME.~N~ there
 *     is (N from 1, any length). -f -b of a file onto itself backs it up;
 *     a directory is never backed up (a file onto one then fails to open,
 *     "Is a directory").
 *   - -l, -s ("can make relative symbolic links only in current
 *     directory"), --parents ("d -> e/d" for each directory it makes),
 *     --remove-destination, -f, --attributes-only, --copy-contents,
 *     --sparse=WHEN, --strip-trailing-slashes, -T, -t.
 *   - --reflink[=always]: there are no reflinks here -- "failed to clone
 *     'D' from 'S': Operation not supported", as GNU says on a filesystem
 *     without them; auto and never copy. --debug's line says what this
 *     engine did: it has no copy offload.
 *   - -Z does nothing, --context=CTX warns, as GNU without SELinux.
 *     --keep-directory-symlink is taken and, as measured on GNU 9.7,
 *     changes nothing.
 *   - "warning: source file 'A' specified more than once", "will not
 *     overwrite just-created 'D' with 'S'", "'A' and 'B' are the same
 *     file" (by GNU's rules: what the operation would destroy), "cannot
 *     copy a directory, 'D', into itself, 'D/D'" -- found as GNU finds it,
 *     when the copy meets the directory it made, after copying what came
 *     before; nothing more of that operand is copied.
 *
 * Needs su_rm.c (rmf_yesno, rmf_argmatch, rmf_join, rmf_canon) included
 * before it.
 *
 * tobyOS: no fchmod, mknod or nanosecond times in libtoby -- modes are
 * set by path, a device node cannot be made ("Operation not permitted",
 * what GNU says to anyone but root), and -p keeps whole seconds. link(2)
 * there is not asked to follow a symbolic link: the link is resolved
 * first (rmf_canon -- libtoby's realpath resolves nothing).
 *
 * The old cp took -rRnfipvaU (-U, a name picked free of clashes, was its
 * own), wrote "cp: cannot open", and stopped at 1024-byte paths. */

#include <sys/time.h>
#include <sys/xattr.h>          /* --preserve=xattr (finding 215) */
#ifdef SU_HOST
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#endif

enum { CPX_DEREF_UNDEF, CPX_DEREF_NEVER, CPX_DEREF_CMDLINE, CPX_DEREF_ALWAYS };
enum { CPX_I_UNSPEC, CPX_I_ASK, CPX_I_NO, CPX_I_FORCE };
enum { CPX_UP_ALL, CPX_UP_NONE, CPX_UP_NONE_FAIL, CPX_UP_OLDER };
enum { CPX_SP_NEVER, CPX_SP_AUTO, CPX_SP_ALWAYS };
enum { CPX_RL_NEVER, CPX_RL_AUTO, CPX_RL_ALWAYS };
enum { CPX_BK_NONE, CPX_BK_SIMPLE, CPX_BK_EXISTING, CPX_BK_NUMBERED };

struct cpx_opts {
    int move;               /* mv's copy: "copied", "created directory" */
    int recursive, deref, hard_link, sym_link;
    int interactive, update, force, remove_dest;
    int backup;
    const char *suffix;
    int p_mode, p_owner, p_time, p_links;
    int p_xattr, p_xattr_req;   /* copy them; one that fails is an error */
    int default_mode;       /* --no-preserve=mode: 0666/0777 less the umask */
    int parents, one_fs, attr_only, copy_contents, sparse, reflink;
    int debug, verbose, stdin_tty;
    int track;              /* the "more than once"/"just-created" tables */
    const char *top_bk;     /* mv's backup, shown with the operand's copy */
    mode_t umask_v;
};

static void *cpx_alloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) su_error(1, ENOMEM, "memory exhausted");
    return p;
}

static char *cpx_dup(const char *s) {
    char *p = strdup(s);
    if (!p) su_error(1, ENOMEM, "memory exhausted");
    return p;
}

/* ---- tables ----------------------------------------------------------------
 *
 * Four small lists, kept for one run: hard links seen (source dev/ino to
 * the name its first copy got); the directories made (one met again as a
 * source is the copy going into itself); and for the command line's
 * operands the destinations made (a later operand will not overwrite one)
 * and which source went to which destination (the same one twice is only
 * warned about). Linear: they hold what one command copies. An inode
 * number of 0 (a filesystem with none) is never found. */
struct cpx_ent { dev_t dev; ino_t ino; char *name; };
struct cpx_tab { struct cpx_ent *v; size_t n, cap; };
static struct cpx_tab cpx_links, cpx_made, cpx_srcs, cpx_dirs;
static const char *cpx_top_src = "", *cpx_top_dst = "";   /* the operand being copied */
static int cpx_abort;       /* the operand went into itself: stop copying it */

static void cpx_tab_add(struct cpx_tab *t, dev_t dev, ino_t ino, const char *name) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 32;
        t->v = (struct cpx_ent *)realloc(t->v, t->cap * sizeof *t->v);
        if (!t->v) su_error(1, ENOMEM, "memory exhausted");
    }
    t->v[t->n].dev = dev;
    t->v[t->n].ino = ino;
    t->v[t->n].name = cpx_dup(name);
    t->n++;
}

/* the entry for DEV/INO (and NAME, when given) */
static const char *cpx_tab_find(const struct cpx_tab *t, dev_t dev, ino_t ino, const char *name) {
    if (!ino) return 0;
    for (size_t i = 0; i < t->n; i++)
        if (t->v[i].dev == dev && t->v[i].ino == ino && (!name || !strcmp(t->v[i].name, name)))
            return t->v[i].name;
    return 0;
}

/* ---- small things ----------------------------------------------------------- */

/* two stats name one file: by device and inode, or -- on a filesystem
 * with no inode numbers -- by where the names lead */
static int cpx_same(const struct stat *a, const struct stat *b, const char *an, const char *bn) {
    if (a->st_ino && b->st_ino) return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
    char *ra = rmf_canon(an), *rb = rmf_canon(bn);
    int same = ra && rb && !strcmp(ra, rb);
    free(ra);
    free(rb);
    return same;
}

static char *cpx_base(const char *src);
static char *cpx_dirname(const char *name);

/* Are A and B (lstat'd as SA, SB) one directory entry -- the same file,
 * under the same name, in the same directory ("a" and "./a" are; two hard
 * links are not)? */
static int cpx_same_entry(const char *a, const char *b, const struct stat *sa,
                          const struct stat *sb) {
    if (!cpx_same(sa, sb, a, b)) return 0;
    char *ba = cpx_base(a), *bb = cpx_base(b);
    int same = !strcmp(ba, bb);
    free(ba);
    free(bb);
    if (!same) return 0;
    char *da = cpx_dirname(a), *db = cpx_dirname(b);
    struct stat x, y;
    same = stat(da, &x) == 0 && stat(db, &y) == 0 && cpx_same(&x, &y, da, db);
    free(da);
    free(db);
    return same;
}

/* "rwxr-xr-x", with s/S and t/T as ls shows them */
static void cpx_mode9(mode_t m, char *o) {
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) o[i] = (m & (0400 >> i)) ? rwx[i] : '-';
    if (m & S_ISUID) o[2] = o[2] == 'x' ? 's' : 'S';
    if (m & S_ISGID) o[5] = o[5] == 'x' ? 's' : 'S';
    if (m & S_ISVTX) o[8] = o[8] == 'x' ? 't' : 'T';
    o[9] = 0;
}

static void cpx_times(const struct stat *st, struct timespec ts[2]) {
    ts[0] = st->st_atim;       /* to the nanosecond, both builds (239) */
    ts[1] = st->st_mtim;
}

/* -u: is D older than S? (equal is not) */
static int cpx_older(const struct stat *d, const struct stat *s) {
    struct timespec dt[2], st[2];
    cpx_times(d, dt);
    cpx_times(s, st);
    if (dt[1].tv_sec != st[1].tv_sec) return dt[1].tv_sec < st[1].tv_sec;
    return dt[1].tv_nsec < st[1].tv_nsec;
}

/* the last component, trailing slashes aside; "." for "." or ".." --
 * what a copy into a directory is named after */
static char *cpx_base(const char *src) {
    size_t n = strlen(src);
    while (n > 1 && src[n - 1] == '/') n--;
    size_t b = n;
    while (b > 0 && src[b - 1] != '/') b--;
    if ((n - b == 1 && src[b] == '.') || (n - b == 2 && src[b] == '.' && src[b + 1] == '.'))
        return cpx_dup(".");
    if (n == 1 && src[0] == '/') return cpx_dup("/");
    char *s = (char *)cpx_alloc(n - b + 1);
    memcpy(s, src + b, n - b);
    s[n - b] = 0;
    return s;
}

/* the directory NAME is in ("." when it has no slash) */
static char *cpx_dirname(const char *name) {
    size_t n = strlen(name);
    while (n > 1 && name[n - 1] == '/') n--;
    while (n > 0 && name[n - 1] != '/') n--;
    while (n > 1 && name[n - 1] == '/') n--;
    if (n == 0) return cpx_dup(".");
    char *s = (char *)cpx_alloc(n + 1);
    memcpy(s, name, n);
    s[n] = 0;
    return s;
}

/* ---- backups ---------------------------------------------------------------- */

static int cpx_backup_type(const char *context, const char *arg) {
    static const char *const names[] = { "none", "off", "simple", "never",
                                         "existing", "nil", "numbered", "t", 0 };
    static const int vals[] = { CPX_BK_NONE, CPX_BK_NONE, CPX_BK_SIMPLE, CPX_BK_SIMPLE,
                                CPX_BK_EXISTING, CPX_BK_EXISTING, CPX_BK_NUMBERED,
                                CPX_BK_NUMBERED };
    return rmf_argmatch(context, arg, names, vals);
}

/* -b/--backup[=CONTROL] as GNU settles it: the option's CONTROL, else
 * VERSION_CONTROL, else existing; an empty one is no choice at all */
static int cpx_backup_choice(int wanted, const char *arg) {
    if (!wanted) return CPX_BK_NONE;
    if (arg && *arg) return cpx_backup_type("backup type", arg);
    const char *env = getenv("VERSION_CONTROL");
    if (env && *env) return cpx_backup_type("$VERSION_CONTROL", env);
    return CPX_BK_EXISTING;
}

static const char *cpx_suffix(const char *opt) {
    const char *s = opt ? opt : getenv("SIMPLE_BACKUP_SUFFIX");
    if (!s || !*s || strchr(s, '/')) s = "~";
    return s;
}

/* the highest N of NAME.~N~ there is (digits, no leading zero), as a
 * string, or 0 for none */
static char *cpx_max_numbered(const char *dst) {
    char *dir = cpx_dirname(dst), *base = cpx_base(dst);
    DIR *d = opendir(dir);
    char *best = 0;
    size_t bl = strlen(base);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *n = e->d_name;
            if (strncmp(n, base, bl) || n[bl] != '.' || n[bl + 1] != '~') continue;
            const char *p = n + bl + 2, *q = p;
            if (*q < '1' || *q > '9') continue;
            while (*q >= '0' && *q <= '9') q++;
            if (q[0] != '~' || q[1]) continue;
            size_t len = (size_t)(q - p);
            if (best && (strlen(best) > len || (strlen(best) == len && strncmp(best, p, len) >= 0)))
                continue;
            free(best);
            best = strndup(p, len);
            if (!best) su_error(1, ENOMEM, "memory exhausted");
        }
        closedir(d);
    }
    free(dir);
    free(base);
    return best;
}

/* the name a backup of DST takes */
static char *cpx_backup_name(const struct cpx_opts *o, const char *dst) {
    int type = o->backup;
    char *max = 0;
    if (type == CPX_BK_NUMBERED || type == CPX_BK_EXISTING) {
        max = cpx_max_numbered(dst);
        if (type == CPX_BK_EXISTING) type = max ? CPX_BK_NUMBERED : CPX_BK_SIMPLE;
    }
    char *out;
    if (type == CPX_BK_SIMPLE) {
        out = (char *)cpx_alloc(strlen(dst) + strlen(o->suffix) + 1);
        sprintf(out, "%s%s", dst, o->suffix);
    } else {
        /* the next number, by decimal string arithmetic: any length */
        const char *m = max ? max : "0";
        size_t l = strlen(m);
        char *num = (char *)cpx_alloc(l + 2);
        memcpy(num + 1, m, l + 1);
        num[0] = '0';
        size_t i = l;
        while (num[i] == '9') num[i--] = '0';
        num[i]++;
        const char *nn = num[0] == '0' ? num + 1 : num;
        out = (char *)cpx_alloc(strlen(dst) + strlen(nn) + 4);
        sprintf(out, "%s.~%s~", dst, nn);
        free(num);
    }
    free(max);
    return out;
}

/* rename DST to its backup; the backup's name, or 0 after saying why not */
static char *cpx_backup(const struct cpx_opts *o, const char *dst) {
    char *bk = cpx_backup_name(o, dst);
    if (rename(dst, bk) != 0) {
        su_error(0, errno, "cannot backup %s", su_quoteaf(dst));
        free(bk);
        return 0;
    }
    return bk;
}

/* ---- saying what was done ---------------------------------------------------- */

static void cpx_say(const struct cpx_opts *o, const char *src, const char *dst,
                    const char *bk, int is_dir) {
    if (!o->verbose) return;
    if (o->move && is_dir) printf("created directory %s", su_quoteaf(dst));
    else printf("%s%s -> %s", o->move ? "copied " : "", su_quoteaf(src), su_quoteaf(dst));
    if (bk) printf(" (backup: %s)", su_quoteaf(bk));
    putchar('\n');
}

/* ---- attributes ----------------------------------------------------------------- */

/* SRC's extended attributes onto DST (finding 215: none were copied), the
 * security context left to --preserve=context. One that cannot be set
 * stops the copying of DST's: an error ("setting attributes for 'DST'",
 * status 1) when xattr was named in --preserve, nothing under -a. Done
 * before the mode, which may leave DST unwritable even to its owner. */
static int cpx_xattrs(const struct cpx_opts *o, const char *src, const char *dst) {
    if (!o->p_xattr) return 0;
    ssize_t n = listxattr(src, 0, 0);
    if (n <= 0) return 0;
    char *names = cpx_alloc((size_t)n);
    n = listxattr(src, names, (size_t)n);
    int rc = 0;
    /* a copy already made read-only (a 0444 source) is its owner's to give
     * attributes all the same, as GNU's is: writable for the moment, then
     * its mode back */
    struct stat dsb;
    int unlocked = 0;
    if (stat(dst, &dsb) == 0 && dsb.st_uid == geteuid() && !(dsb.st_mode & S_IWUSR) &&
        chmod(dst, (dsb.st_mode | S_IWUSR) & 07777) == 0)
        unlocked = 1;
    for (ssize_t i = 0; n > 0 && i < n; i += (ssize_t)strlen(names + i) + 1) {
        const char *nm = names + i;
        if (!strcmp(nm, "security.selinux")) continue;
        ssize_t vl = getxattr(src, nm, 0, 0);
        if (vl < 0) continue;
        char *v = cpx_alloc((size_t)vl);
        vl = getxattr(src, nm, v, (size_t)vl);
        int bad = vl < 0 || setxattr(dst, nm, v, (size_t)vl, 0) != 0;
        int e = errno;
        free(v);
        if (bad) {
            if (o->p_xattr_req) {
                su_error(0, e, "setting attributes for %s", su_quoteaf(dst));
                rc = 1;
            }
            break;
        }
    }
    if (unlocked) (void)chmod(dst, dsb.st_mode & 07777);
    free(names);
    return rc;
}

/* What -p keeps, for DST made from S. LINK: DST is a symbolic link. NEW:
 * DST was made here (so it gets a mode even without -p). */
static int cpx_attrs(const struct cpx_opts *o, const char *dst, const struct stat *s,
                     int link, int is_new) {
    int rc = 0;
    if (o->p_time) {
        struct timespec ts[2];
        cpx_times(s, ts);
        if (utimensat(AT_FDCWD, dst, ts, link ? AT_SYMLINK_NOFOLLOW : 0) != 0 && !link) {
            su_error(0, errno, "preserving times for %s", su_quoteaf(dst));
            rc = 1;
        }
    }
    int owned = 1;
    if (o->p_owner) {
        int r = link ? lchown(dst, s->st_uid, s->st_gid) : chown(dst, s->st_uid, s->st_gid);
        if (r != 0) {
            owned = 0;
            /* the group alone, then nothing: not root may not give a file away */
            if (link ? lchown(dst, (uid_t)-1, s->st_gid) : chown(dst, (uid_t)-1, s->st_gid)) {
                int e = errno;
                if (!((e == EPERM || e == EINVAL) && geteuid() != 0)) {
                    su_error(0, e, "failed to preserve ownership for %s", su_quoteaf(dst));
                    rc = 1;
                }
            }
        }
    }
    if (link) return rc;
    mode_t m;
    if (o->p_mode) {
        m = s->st_mode & 07777;
        if (!owned) m &= ~(mode_t)(S_ISUID | S_ISGID);
    } else if (is_new && o->default_mode) {
        m = (S_ISDIR(s->st_mode) ? 0777 : 0666) & ~o->umask_v;
    } else if (is_new) {
        m = s->st_mode & (S_ISDIR(s->st_mode) ? 01777 : 0777) & ~o->umask_v;
    } else {
        return rc;
    }
    if (chmod(dst, m) != 0) {
        su_error(0, errno, "setting permissions for %s", su_quoteaf(dst));
        rc = 1;
    }
    return rc;
}

/* ---- the copy itself ------------------------------------------------------------- */

struct cpx_anc { dev_t dev; ino_t ino; const struct cpx_anc *up; };

static int cpx_copy(const struct cpx_opts *o, const char *src, const char *dst,
                    int top, dev_t top_dev, const struct cpx_anc *anc);

/* a hard link DST to SRC; FOLLOW: to what SRC names, if a symbolic link */
static int cpx_link(const char *src, const char *dst, int follow) {
#ifdef SU_HOST
    return linkat(AT_FDCWD, src, AT_FDCWD, dst, follow ? AT_SYMLINK_FOLLOW : 0);
#else
    if (follow) {
        struct stat st;
        if (lstat(src, &st) == 0 && S_ISLNK(st.st_mode)) {
            char *r = rmf_canon(src);
            if (!r) return -1;
            int rc = link(r, dst);
            free(r);
            return rc;
        }
    }
    return link(src, dst);
#endif
}

/* a device node or socket -- root only, as Linux's (libtoby's mknod is
 * the kernel's since finding 214; this said EPERM to everyone) */
static int cpx_mknod(const char *dst, const struct stat *s) {
    return mknod(dst, s->st_mode & (S_IFMT | 0777), s->st_rdev);
}

static int cpx_allzero(const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}

/* SRC's bytes into DST, opened; holes for zeros when SPARSE */
static int cpx_data(int in, int out, const char *src, const char *dst, int sparse) {
    static char buf[65536];
    off_t total = 0;
    int hole_at_end = 0;
    for (;;) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0) {
            su_error(0, errno, "error reading %s", su_quoteaf(src));
            return 1;
        }
        if (r == 0) break;
        for (ssize_t off = 0; off < r;) {
            size_t chunk = (size_t)(r - off) < 4096 ? (size_t)(r - off) : 4096;
            if (sparse && cpx_allzero(buf + off, chunk) &&
                lseek(out, (off_t)chunk, SEEK_CUR) >= 0) {
                hole_at_end = 1;
                off += (ssize_t)chunk;
                total += (off_t)chunk;
                continue;
            }
            ssize_t w = write(out, buf + off, chunk);
            if (w <= 0) {
                su_error(0, errno, "error writing %s", su_quoteaf(dst));
                return 1;
            }
            hole_at_end = 0;
            off += w;
            total += w;
        }
    }
    if (hole_at_end && ftruncate(out, total) != 0) {
        su_error(0, errno, "error writing %s", su_quoteaf(dst));
        return 1;
    }
    return 0;
}

/* a regular file (or anything read as one): DEXISTS says DST is there,
 * DL its lstat */
static int cpx_regular(const struct cpx_opts *o, const char *src, const char *dst,
                       const struct stat *s, int dexists, const struct stat *dl,
                       const char *bk) {
    cpx_say(o, src, dst, bk, 0);
    if (dexists && S_ISLNK(dl->st_mode)) {
        struct stat t;
        if (stat(dst, &t) != 0) {
            su_error(0, 0, "not writing through dangling symlink %s", su_quoteaf(dst));
            return 1;
        }
    }
    int in = su_open_read(src, 0);
    if (in < 0) {
        su_error(0, errno, "cannot open %s for reading", su_quoteaf(src));
        return 1;
    }
    int out = -1, is_new = 0;
    if (dexists) {
        out = open(dst, O_WRONLY | (o->attr_only ? 0 : O_TRUNC));
        if (out < 0 && o->force) {
            /* -f: what cannot be opened is removed, and made anew -- never
             * a directory (a native unlink might take an empty one) */
            int ue = S_ISDIR(dl->st_mode) ? EISDIR : unlink(dst) != 0 ? errno : 0;
            if (ue) {
                su_error(0, ue, "cannot remove %s", su_quoteaf(dst));
                close(in);
                return 1;
            }
            if (o->verbose) printf("removed %s\n", su_quoteaf(dst));
            dexists = 0;
        } else if (out < 0) {
            su_error(0, errno, "cannot create regular file %s", su_quoteaf(dst));
            close(in);
            return 1;
        }
    }
    if (!dexists) {
        size_t dn = strlen(dst);
        if (dn > 1 && dst[dn - 1] == '/') {
            /* "x/" names a directory, which a file cannot be: GNU's answer
             * (and not a file "x", which a native open could make) */
            su_error(0, ENOTDIR, "cannot create regular file %s", su_quoteaf(dst));
            close(in);
            return 1;
        }
        out = open(dst, O_WRONLY | O_CREAT | O_EXCL, (int)(s->st_mode & 0777));
        if (out < 0) {
            su_error(0, errno, "cannot create regular file %s", su_quoteaf(dst));
            close(in);
            return 1;
        }
        is_new = 1;
    }
    int rc = 0;
    if (o->reflink == CPX_RL_ALWAYS && !o->attr_only) {
        if (o->debug)
            printf("copy offload: unknown, reflink: unsupported, sparse detection: unknown\n");
        /* EOPNOTSUPP in this libc's words, as every error (utilgate turns
         * glibc's "Operation not supported" into musl's, finding 225) */
        su_error(0, EOPNOTSUPP, "failed to clone %s from %s",
                 su_quoteaf(dst), su_quoteaf(src));
        close(in);
        close(out);
        if (is_new) unlink(dst);
        return 1;
    }
    int sparse = o->sparse == CPX_SP_ALWAYS ||
                 (o->sparse == CPX_SP_AUTO && S_ISREG(s->st_mode) &&
                  s->st_blocks * 512 < s->st_size);
    if (!o->attr_only) {
        rc = cpx_data(in, out, src, dst, sparse);
        if (o->debug && !rc) {
            /* GNU's three words: no copy offload is ever tried here, nor
             * a clone (tobyOS has none to try) */
            int avoided = o->sparse != CPX_SP_AUTO || o->reflink == CPX_RL_NEVER;
            int noclone = o->sparse == CPX_SP_NEVER || o->reflink == CPX_RL_NEVER;
            printf("copy offload: %s, reflink: %s, sparse detection: %s\n",
                   avoided ? "avoided" : "unsupported", noclone ? "no" : "unsupported",
                   sparse ? "zeros" : "no");
        }
    }
    close(in);
    if (close(out) != 0 && !rc) {
        su_error(0, errno, "failed to close %s", su_quoteaf(dst));
        rc = 1;
    }
    if (rc) return 1;
    rc = cpx_xattrs(o, src, dst);
    return cpx_attrs(o, dst, s, 0, is_new) | rc;
}

/* the entries of directory SRC by inode, as GNU's cp takes them (ties in
 * the order the directory gives them) */
struct cpx_dent { ino_t ino; size_t pos; char *name; };

static int cpx_dent_cmp(const void *a, const void *b) {
    const struct cpx_dent *x = (const struct cpx_dent *)a, *y = (const struct cpx_dent *)b;
    if (x->ino != y->ino) return x->ino < y->ino ? -1 : 1;
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}

static struct cpx_dent *cpx_entries(const char *path, size_t *count) {
    DIR *d = opendir(path);
    if (!d) return 0;
    size_t n = 0, cap = 16;
    struct cpx_dent *v = (struct cpx_dent *)cpx_alloc(cap * sizeof *v);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) {
            cap *= 2;
            v = (struct cpx_dent *)realloc(v, cap * sizeof *v);
            if (!v) su_error(1, ENOMEM, "memory exhausted");
        }
        v[n].ino = e->d_ino;
        v[n].pos = n;
        v[n].name = cpx_dup(e->d_name);
        n++;
    }
    closedir(d);
    qsort(v, n, sizeof *v, cpx_dent_cmp);
    *count = n;
    return v;
}

/* directory SRC to DST (DEXISTS: already a directory, to be merged
 * into). DESCEND: copy what is in it (-x leaves a mount point empty). */
static int cpx_dir(const struct cpx_opts *o, const char *src, const char *dst,
                   const struct stat *s, int dexists, int descend, const char *bk,
                   dev_t top_dev, const struct cpx_anc *anc) {
    int rc = 0, is_new = 0;
    if (!dexists) {
        if (mkdir(dst, 0700) != 0) {
            su_error(0, errno, "cannot create directory %s", su_quoteaf(dst));
            return 1;
        }
        is_new = 1;
        /* remembered: a source directory that turns out to be this one is
         * the copy being copied into itself */
        struct stat made;
        if (stat(dst, &made) == 0) cpx_tab_add(&cpx_dirs, made.st_dev, made.st_ino, dst);
        cpx_say(o, src, dst, bk, 1);
    }
    if (descend) {
        size_t n = 0;
        struct cpx_dent *v = cpx_entries(src, &n);
        if (!v) {
            su_error(0, errno, "cannot access %s", su_quoteaf(src));
            rc = 1;
        } else {
            struct cpx_anc me = { s->st_dev, s->st_ino, anc };
            for (size_t i = 0; i < n; i++) {
                /* into itself: GNU copies nothing more of the operand */
                if (!cpx_abort) {
                    char *cs = rmf_join(src, v[i].name), *cd = rmf_join(dst, v[i].name);
                    rc |= cpx_copy(o, cs, cd, 0, top_dev, &me);
                    free(cs);
                    free(cd);
                }
                free(v[i].name);
            }
            free(v);
        }
    }
    rc |= cpx_xattrs(o, src, dst);
    if (is_new || o->p_mode || o->p_owner || o->p_time)
        rc |= cpx_attrs(o, dst, s, 0, is_new);
    return rc;
}

/* A link, FIFO or device node cannot be written into: an existing DST
 * goes first ("removed 'D'" with -v, before the copy is announced). */
static int cpx_replace(const struct cpx_opts *o, const char *dst, int dexists) {
    if (!dexists) return 0;
    /* a failure is said, as GNU's cp says it: it was ignored, and the link
     * made next said "File exists" (a read-only directory) */
    if (unlink(dst) != 0) {
        if (errno == ENOENT) return 0;
        su_error(0, errno, "cannot remove %s", su_quoteaf(dst));
        return 1;
    }
    if (o->verbose) printf("removed %s\n", su_quoteaf(dst));
    return 0;
}

/* Copy SRC to DST. TOP: a command-line operand. 0, or 1 after saying
 * what went wrong. */
static int cpx_copy(const struct cpx_opts *o, const char *src, const char *dst,
                    int top, dev_t top_dev, const struct cpx_anc *anc) {
    struct stat s, sl;
    int follow = o->deref == CPX_DEREF_ALWAYS || (o->deref == CPX_DEREF_CMDLINE && top);
    if ((follow ? stat(src, &s) : lstat(src, &s)) != 0) {
        su_error(0, errno, "cannot stat %s", su_quoteaf(src));
        return 1;
    }
    if (top) top_dev = s.st_dev;
    if (S_ISDIR(s.st_mode) && !o->recursive) {
        su_error(0, 0, "-r not specified; omitting directory %s", su_quoteaf(src));
        return 1;
    }
    if (top && o->track && (S_ISDIR(s.st_mode) || o->backup == CPX_BK_NONE) &&
        cpx_tab_find(&cpx_srcs, s.st_dev, s.st_ino, dst)) {
        fflush(stdout);
        fprintf(stderr, "%s: warning: source %s %s specified more than once\n", su_short,
                S_ISDIR(s.st_mode) ? "directory" : "file", su_quoteaf(src));
        return 0;
    }
    if (top && o->track) cpx_tab_add(&cpx_srcs, s.st_dev, s.st_ino, dst);
    if (top) {
        cpx_top_src = src;
        cpx_top_dst = dst;
        cpx_abort = 0;
    }
    if (S_ISDIR(s.st_mode)) {
        /* A directory this cp made, met again as a source under its own
         * name: the copy is going into itself. GNU finds it so, having
         * copied what came before it, and names the operands. */
        const char *made = cpx_tab_find(&cpx_dirs, s.st_dev, s.st_ino, 0);
        struct stat ml;
        if (made && lstat(src, &sl) == 0 && lstat(made, &ml) == 0 &&
            cpx_same_entry(src, made, &sl, &ml)) {
            su_error(0, 0, "cannot copy a directory, %s, into itself, %s",
                     su_quoteaf(cpx_top_src), su_quoteaf(cpx_top_dst));
            cpx_abort = 1;
            return 1;
        }
        /* one of its own ancestors, reached through a followed link */
        for (const struct cpx_anc *a = anc; a; a = a->up)
            if (a->dev == s.st_dev && a->ino == s.st_ino && s.st_ino) {
                su_error(0, 0, "cannot copy cyclic symbolic link %s", su_quoteaf(src));
                return 1;
            }
    }

    struct stat dl, ds;
    int dexists = 0, dhas = 0;
    if (lstat(dst, &dl) == 0) {
        dexists = 1;
        if (S_ISLNK(dl.st_mode)) dhas = stat(dst, &ds) == 0;
        else { ds = dl; dhas = 1; }
    } else if (errno != ENOENT) {
        su_error(0, errno, "cannot stat %s", su_quoteaf(dst));
        return 1;
    }
    char *bk = 0;
    int rc;
    if (dexists) {
        /* The same file? It is when what is about to be done would
         * destroy what is being copied -- GNU's rules, as measured:
         *  -l, -s: SOURCE against DEST itself (a link is not followed);
         *  a backup moves DEST aside, so only the same directory entry
         *   (or a followed link to it) is the same -- and with -f the very
         *   same name is copied from its backup instead;
         *  --remove-destination unlinks DEST: the same entry, or a link
         *   that leads to it;
         *  else DEST is written through: by what both lead to (two
         *   symbolic links are compared as themselves). */
        struct stat sl2, sf = s;
        int sl_ok = lstat(src, &sl2) == 0;
        int src_is_link = sl_ok && S_ISLNK(sl2.st_mode);
        int sf_ok = !src_is_link || stat(src, &sf) == 0;
        int same, entry = sl_ok && cpx_same_entry(src, dst, &sl2, &dl);
        if (o->hard_link || o->sym_link) {
            same = cpx_same(&s, &dl, src, dst);
            if (same && o->hard_link && !S_ISDIR(s.st_mode)) return 0;
        } else if (o->backup != CPX_BK_NONE) {
            same = entry || (S_ISLNK(sl2.st_mode) && !S_ISLNK(s.st_mode) &&
                             !S_ISLNK(dl.st_mode) && cpx_same(&s, &dl, src, dst));
            if (same && entry && o->force && S_ISREG(s.st_mode) && !strcmp(src, dst)) {
                /* GNU's special case: back the file up, then copy the
                 * backup over the name it had */
                bk = cpx_backup(o, dst);
                if (!bk) return 1;
                if (o->verbose) printf("%s -> %s\n", su_quoteaf(src), su_quoteaf(bk));
                struct stat bs;
                if (stat(bk, &bs) != 0) bs = s;
                struct cpx_opts q = *o;
                q.verbose = 0;
                q.backup = CPX_BK_NONE;
                q.track = 0;
                rc = cpx_regular(&q, bk, dst, &bs, 0, &dl, 0);
                free(bk);
                return rc;
            }
        } else if (o->remove_dest) {
            same = entry || (src_is_link && sf_ok && !S_ISLNK(dl.st_mode) &&
                             cpx_same(&sf, &dl, src, dst));
        } else if (S_ISLNK(s.st_mode) && S_ISLNK(dl.st_mode)) {
            same = cpx_same(&s, &dl, src, dst);
        } else {
            same = sf_ok && dhas && cpx_same(&sf, &ds, src, dst);
        }
        if (same) {
            su_error(0, 0, "%s and %s are the same file", su_quoteaf(src), su_quoteaf(dst));
            return 1;
        }
        if (S_ISDIR(s.st_mode)) {
            if (S_ISDIR(dl.st_mode)) {
                int descend = !o->one_fs || s.st_dev == top_dev || top;
                return cpx_dir(o, src, dst, &s, 1, descend, 0, top_dev, anc);
            }
            if (o->backup == CPX_BK_NONE) {
                su_error(0, 0, "cannot overwrite non-directory %s with directory %s",
                         su_quoteaf(dst), su_quoteaf(src));
                return 1;
            }
        } else {
            /* with a backup asked for, GNU goes on -- a directory is never
             * backed up by cp -- and the open fails ("Is a directory") */
            if (dhas && S_ISDIR(ds.st_mode) && o->backup == CPX_BK_NONE) {
                su_error(0, 0, "cannot overwrite directory %s with non-directory %s",
                         su_quoteaf(dst), su_quoteaf(src));
                return 1;
            }
            if (o->interactive == CPX_I_NO || o->update == CPX_UP_NONE) return 0;
            if (o->update == CPX_UP_NONE_FAIL) {
                su_error(0, 0, "not replacing %s", su_quoteaf(dst));
                return 1;
            }
            if (o->update == CPX_UP_OLDER && dhas && !cpx_older(&ds, &s)) return 0;
            if (o->interactive == CPX_I_ASK) {
                int wp = !S_ISLNK(dl.st_mode) && access(dst, W_OK) != 0 && errno == EACCES;
                fflush(stdout);
                if (wp) {
                    char m9[10];
                    cpx_mode9(dl.st_mode, m9);
                    /* with -f or --remove-destination the file need not be
                     * written, only replaced */
                    if (o->force || o->remove_dest)
                        fprintf(stderr, "%s: replace %s, overriding mode %04o (%s)? ", su_short,
                                su_quoteaf(dst), (unsigned)(dl.st_mode & 07777), m9);
                    else
                        fprintf(stderr, "%s: unwritable %s (mode %04o, %s); try anyway? ",
                                su_short, su_quoteaf(dst), (unsigned)(dl.st_mode & 07777), m9);
                } else {
                    fprintf(stderr, "%s: overwrite %s? ", su_short, su_quoteaf(dst));
                }
                if (!rmf_yesno()) return 1;
            }
        }
        if (top && o->track && o->backup != CPX_BK_NUMBERED && !S_ISDIR(dl.st_mode) &&
            cpx_tab_find(&cpx_made, dl.st_dev, dl.st_ino, dst)) {
            su_error(0, 0, "will not overwrite just-created %s with %s",
                     su_quoteaf(dst), su_quoteaf(src));
            return 1;
        }
        if (o->backup != CPX_BK_NONE && !S_ISDIR(dl.st_mode)) {
            bk = cpx_backup(o, dst);
            if (!bk) return 1;
            dexists = 0;
        } else if (o->backup != CPX_BK_NONE) {
            /* nothing: see above */
        } else if (o->remove_dest) {
            if (unlink(dst) != 0) {
                su_error(0, errno, "cannot remove %s", su_quoteaf(dst));
                return 1;
            }
            if (o->verbose) printf("removed %s\n", su_quoteaf(dst));
            dexists = 0;
        }
    }

    const char *sbk = bk ? bk : top ? o->top_bk : 0;
    if (S_ISDIR(s.st_mode)) {
        int descend = !o->one_fs || s.st_dev == top_dev;
        /* a directory's line names no backup, though one was made */
        rc = cpx_dir(o, src, dst, &s, 0, descend, 0, top_dev, anc);
        goto done;
    }

    /* hard links among the sources stay hard links (-a, --preserve=links) */
    if (o->p_links && (s.st_nlink > 1 || o->move) && !o->hard_link && !o->sym_link) {
        const char *prev = cpx_tab_find(&cpx_links, s.st_dev, s.st_ino, 0);
        if (prev) {
            if (!o->move) cpx_say(o, src, dst, sbk, 0);
            if (dexists && !S_ISDIR(dl.st_mode)) {
                if (unlink(dst) != 0) {
                    su_error(0, errno, "cannot remove %s", su_quoteaf(dst));
                    rc = 1;
                    goto done;
                }
                if (o->verbose && !o->move) printf("removed %s\n", su_quoteaf(dst));
            }
            if (link(prev, dst) != 0) {
                su_error(0, errno, "cannot create hard link %s to %s",
                         su_quoteaf(dst), su_quoteaf(prev));
                rc = 1;
            } else {
                rc = 0;
            }
            goto done;
        }
    }

    /* -l and -s say what they are about to do, then try */
    if (o->hard_link) {
        cpx_say(o, src, dst, sbk, 0);
        if (dexists && o->force && !S_ISDIR(dl.st_mode)) unlink(dst);
        if (cpx_link(src, dst, o->deref != CPX_DEREF_NEVER &&
                     (o->deref == CPX_DEREF_ALWAYS || top)) != 0) {
            su_error(0, errno, "cannot create hard link %s to %s",
                     su_quoteaf(dst), su_quoteaf(src));
            rc = 1;
        } else {
            rc = 0;
        }
        goto done;
    }
    if (o->sym_link) {
        cpx_say(o, src, dst, sbk, 0);
        if (src[0] != '/') {
            char *dd = cpx_dirname(dst);
            struct stat a, b;
            int here = stat(dd, &a) == 0 && stat(".", &b) == 0 && cpx_same(&a, &b, dd, ".");
            free(dd);
            if (!here) {
                su_error(0, 0, "%s: can make relative symbolic links only in current directory",
                         su_quotef(dst));
                rc = 1;
                goto done;
            }
        }
        if (dexists && o->force && !S_ISDIR(dl.st_mode)) unlink(dst);
        if (symlink(src, dst) != 0) {
            su_error(0, errno, "cannot create symbolic link %s to %s",
                     su_quoteaf(dst), su_quoteaf(src));
            rc = 1;
        } else {
            rc = 0;
        }
        goto done;
    }

    if (S_ISLNK(s.st_mode)) {
        char *target = (char *)cpx_alloc((size_t)s.st_size + 4097);
        ssize_t n = readlink(src, target, (size_t)s.st_size + 4096);
        if (n < 0) {
            su_error(0, errno, "cannot read symbolic link %s", su_quoteaf(src));
            free(target);
            rc = 1;
            goto done;
        }
        target[n] = 0;
        if (cpx_replace(o, dst, dexists && !S_ISDIR(dl.st_mode))) {
            free(target);
            rc = 1;
            goto done;
        }
        cpx_say(o, src, dst, sbk, 0);
        if (symlink(target, dst) != 0) {
            su_error(0, errno, "cannot create symbolic link %s", su_quoteaf(dst));
            rc = 1;
        } else {
            rc = cpx_attrs(o, dst, &s, 1, 1);
        }
        free(target);
    } else if (!S_ISREG(s.st_mode) && o->recursive && !o->copy_contents) {
        if (cpx_replace(o, dst, dexists && !S_ISDIR(dl.st_mode))) {
            rc = 1;
            goto done;
        }
        cpx_say(o, src, dst, sbk, 0);
        if (S_ISFIFO(s.st_mode) ? mkfifo(dst, s.st_mode & 0777) != 0 : cpx_mknod(dst, &s) != 0) {
            su_error(0, errno, S_ISFIFO(s.st_mode) ? "cannot create fifo %s"
                                                  : "cannot create special file %s",
                     su_quoteaf(dst));
            rc = 1;
        } else {
            rc = cpx_xattrs(o, src, dst);
            rc |= cpx_attrs(o, dst, &s, 0, 1);
        }
    } else {
        rc = cpx_regular(o, src, dst, &s, dexists, &dl, sbk);
    }

done:
    /* only a file with other names is remembered (mv looks every file up:
     * a source's other name may already have been moved, and counted
     * off, by the time it comes) */
    if (!rc && o->p_links && !S_ISDIR(s.st_mode) && s.st_nlink > 1 &&
        !cpx_tab_find(&cpx_links, s.st_dev, s.st_ino, 0))
        cpx_tab_add(&cpx_links, s.st_dev, s.st_ino, dst);
    if (top && o->track) {
        struct stat now;
        if (lstat(dst, &now) == 0) cpx_tab_add(&cpx_made, now.st_dev, now.st_ino, dst);
    }
    free(bk);
    return rc;
}

/* ---- --parents ------------------------------------------------------------------ */

struct cpx_pdir { char *dst; struct stat st; };

/* make DST's leading directories after SRC's (DST is TARGET/SRC); 0, or
 * 1 after the message. *MADE gets those made, for -p afterwards. */
static int cpx_parents(const struct cpx_opts *o, const char *src, const char *target,
                       struct cpx_pdir **made, size_t *nmade) {
    size_t sl = strlen(src);
    *made = 0;
    *nmade = 0;
    for (size_t i = 1; i < sl; i++) {
        if (src[i] != '/' || src[i - 1] == '/') continue;
        char *ps = strndup(src, i);
        if (!ps) su_error(1, ENOMEM, "memory exhausted");
        char *pd = rmf_join(target, ps);
        if (*ps == '/' && target[strlen(target) - 1] != '/') {
            free(pd);
            pd = (char *)cpx_alloc(strlen(target) + strlen(ps) + 1);
            sprintf(pd, "%s%s", target, ps);
        }
        struct stat s, d;
        if (stat(ps, &s) != 0) {
            su_error(0, errno, "failed to get attributes of %s", su_quoteaf(ps));
            free(ps);
            free(pd);
            return 1;
        }
        if (stat(pd, &d) != 0) {
            if (mkdir(pd, 0700) != 0) {
                su_error(0, errno, "cannot make directory %s", su_quoteaf(pd));
                free(ps);
                free(pd);
                return 1;
            }
            if (o->verbose) printf("%s -> %s\n", ps, pd);
            *made = (struct cpx_pdir *)realloc(*made, (*nmade + 1) * sizeof **made);
            if (!*made) su_error(1, ENOMEM, "memory exhausted");
            (*made)[*nmade].dst = pd;
            (*made)[*nmade].st = s;
            (*nmade)++;
            pd = 0;
        } else if (!S_ISDIR(d.st_mode)) {
            su_error(0, 0, "%s exists but is not a directory", su_quoteaf(pd));
            free(ps);
            free(pd);
            return 1;
        }
        free(ps);
        free(pd);
    }
    return 0;
}

/* ---- cp ---------------------------------------------------------------------- */

static const char *const cpx_update_names[] = { "all", "none", "none-fail", "older", 0 };
static const int cpx_update_vals[] = { CPX_UP_ALL, CPX_UP_NONE, CPX_UP_NONE_FAIL, CPX_UP_OLDER };

/* the operands' destination: TARGET/base(SRC), or TARGET/SRC with --parents */
static char *cpx_into(const char *target, const char *src, int parents) {
    if (parents) {
        if (*src == '/' && *target && target[strlen(target) - 1] != '/') {
            char *s = (char *)cpx_alloc(strlen(target) + strlen(src) + 1);
            sprintf(s, "%s%s", target, src);
            return s;
        }
        return rmf_join(target, src);
    }
    char *b = cpx_base(src), *d = rmf_join(target, b);
    free(b);
    return d;
}

/* -t/the last operand as a directory: 0, or exit with GNU's message */
static void cpx_target_check(const char *t, int from_t) {
    struct stat st;
    if (stat(t, &st) != 0)
        su_error(1, errno, from_t ? "target directory %s" : "target %s", su_quoteaf(t));
    if (!S_ISDIR(st.st_mode))
        su_error(1, ENOTDIR, from_t ? "target directory %s" : "target %s", su_quoteaf(t));
}

static int do_cp(int argc, char **argv) {
    enum { ATTR_ONLY_OPT = 0x100, BACKUP_OPT, COPY_CONTENTS_OPT, DEBUG_OPT, NO_PRESERVE_OPT,
           PARENTS_OPT, PRESERVE_OPT, REMOVE_DEST_OPT, SPARSE_OPT, REFLINK_OPT,
           STRIP_OPT, UPDATE_OPT, KEEP_DIRLINK_OPT, CONTEXT_OPT };
    static const struct su_option longs[] = {
        { "archive", SU_NO_ARG, 0, 'a' },
        { "attributes-only", SU_NO_ARG, 0, ATTR_ONLY_OPT },
        { "backup", SU_OPT_ARG, 0, BACKUP_OPT },
        { "copy-contents", SU_NO_ARG, 0, COPY_CONTENTS_OPT },
        { "debug", SU_NO_ARG, 0, DEBUG_OPT },
        { "dereference", SU_NO_ARG, 0, 'L' },
        { "force", SU_NO_ARG, 0, 'f' },
        { "interactive", SU_NO_ARG, 0, 'i' },
        { "link", SU_NO_ARG, 0, 'l' },
        { "no-clobber", SU_NO_ARG, 0, 'n' },
        { "no-dereference", SU_NO_ARG, 0, 'P' },
        { "no-preserve", SU_REQ_ARG, 0, NO_PRESERVE_OPT },
        { "no-target-directory", SU_NO_ARG, 0, 'T' },
        { "one-file-system", SU_NO_ARG, 0, 'x' },
        { "parents", SU_NO_ARG, 0, PARENTS_OPT },
        { "preserve", SU_OPT_ARG, 0, PRESERVE_OPT },
        { "recursive", SU_NO_ARG, 0, 'R' },
        { "remove-destination", SU_NO_ARG, 0, REMOVE_DEST_OPT },
        { "sparse", SU_REQ_ARG, 0, SPARSE_OPT },
        { "reflink", SU_OPT_ARG, 0, REFLINK_OPT },
        { "strip-trailing-slashes", SU_NO_ARG, 0, STRIP_OPT },
        { "suffix", SU_REQ_ARG, 0, 'S' },
        { "symbolic-link", SU_NO_ARG, 0, 's' },
        { "target-directory", SU_REQ_ARG, 0, 't' },
        { "update", SU_OPT_ARG, 0, UPDATE_OPT },
        { "verbose", SU_NO_ARG, 0, 'v' },
        { "keep-directory-symlink", SU_NO_ARG, 0, KEEP_DIRLINK_OPT },
        { "context", SU_OPT_ARG, 0, CONTEXT_OPT },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    static const char *const pres_names[] = { "mode", "timestamps", "ownership", "links",
                                              "context", "xattr", "all", 0 };
    static const int pres_vals[] = { 1, 2, 4, 8, 16, 32, 63 };
    static const char *const sparse_names[] = { "never", "auto", "always", 0 };
    static const int sparse_vals[] = { CPX_SP_NEVER, CPX_SP_AUTO, CPX_SP_ALWAYS };
    static const char *const reflink_names[] = { "auto", "always", "never", 0 };
    static const int reflink_vals[] = { CPX_RL_AUTO, CPX_RL_ALWAYS, CPX_RL_NEVER };

    struct cpx_opts o;
    memset(&o, 0, sizeof o);
    o.sparse = CPX_SP_AUTO;
    o.reflink = CPX_RL_AUTO;
    o.track = 1;
    o.deref = CPX_DEREF_UNDEF;
    int want_backup = 0, no_target = 0, strip = 0, context_warn = 0, require_context = 0, c;
    const char *backup_arg = 0, *suffix = 0, *target = 0;
    while ((c = su_getopt_long(argc, argv, "abdfHilLnprst:uvxPRS:TZ", longs, 0)) != -1) {
        switch (c) {
        case 'a':
            o.recursive = 1;
            o.deref = CPX_DEREF_NEVER;
            o.p_mode = o.p_owner = o.p_time = o.p_links = 1;
            o.p_xattr = 1;
            o.default_mode = 0;
            break;
        case ATTR_ONLY_OPT: o.attr_only = 1; break;
        case 'b': want_backup = 1; break;
        case BACKUP_OPT: want_backup = 1; backup_arg = su_optarg; break;
        case COPY_CONTENTS_OPT: o.copy_contents = 1; break;
        case DEBUG_OPT: o.debug = o.verbose = 1; break;
        case 'd': o.deref = CPX_DEREF_NEVER; o.p_links = 1; break;
        case 'f': o.force = 1; break;
        case 'H': o.deref = CPX_DEREF_CMDLINE; break;
        case 'i': o.interactive = CPX_I_ASK; break;
        case 'l': o.hard_link = 1; break;
        case 'L': o.deref = CPX_DEREF_ALWAYS; break;
        case 'n':
            o.interactive = CPX_I_NO;
            fflush(stdout);
            fprintf(stderr, "%s: warning: behavior of -n is non-portable and may change in "
                            "future; use --update=none instead\n", su_short);
            break;
        case 'P': o.deref = CPX_DEREF_NEVER; break;
        case 'p': o.p_mode = o.p_owner = o.p_time = 1; o.default_mode = 0; break;
        case NO_PRESERVE_OPT:
        case PRESERVE_OPT: {
            int bits = 0;
            if (!su_optarg) {
                bits = 1 | 2 | 4;
            } else {
                char *list = cpx_dup(su_optarg), *save = list;
                for (char *tok = list;; ) {
                    char *comma = strchr(tok, ',');
                    if (comma) *comma = 0;
                    bits |= rmf_argmatch(c == PRESERVE_OPT ? "--preserve" : "--no-preserve",
                                         tok, pres_names, pres_vals);
                    if (!comma) break;
                    tok = comma + 1;
                }
                free(save);
            }
            int on = c == PRESERVE_OPT;
            if (bits & 1) { o.p_mode = on; o.default_mode = !on; }
            if (bits & 2) o.p_time = on;
            if (bits & 4) o.p_owner = on;
            if (bits & 8) o.p_links = on;
            if (bits & 32) {
                o.p_xattr = on;
                /* named outright, a failure is an error; through "all"
                 * it is not (measured, logs/b33/cpxattr-wsl.out) */
                o.p_xattr_req = on && (bits != 63 || strstr(su_optarg, "xattr"));
            }
            /* "all" asks for a context only if one can be had; naming it
             * asks for it outright */
            if (bits & 16) {
                if (!on) require_context = 0;
                else if (bits != 63 || strstr(su_optarg, "context")) require_context = 1;
            }
            break;
        }
        case PARENTS_OPT: o.parents = 1; break;
        case 'r': case 'R': o.recursive = 1; break;
        case REMOVE_DEST_OPT: o.remove_dest = 1; break;
        case SPARSE_OPT:
            o.sparse = rmf_argmatch("--sparse", su_optarg, sparse_names, sparse_vals);
            break;
        case REFLINK_OPT:
            o.reflink = su_optarg ? rmf_argmatch("--reflink", su_optarg, reflink_names,
                                                 reflink_vals)
                                  : CPX_RL_ALWAYS;
            break;
        case STRIP_OPT: strip = 1; break;
        case 's': o.sym_link = 1; break;
        case 'S': want_backup = 1; suffix = su_optarg; break;
        case 't':
            if (target) su_error(1, 0, "multiple target directories specified");
            target = su_optarg;
            break;
        case 'T': no_target = 1; break;
        case 'u': o.update = CPX_UP_OLDER; break;
        case UPDATE_OPT:
            o.update = su_optarg ? rmf_argmatch("--update", su_optarg, cpx_update_names,
                                                cpx_update_vals)
                                 : CPX_UP_OLDER;
            break;
        case 'v': o.verbose = 1; break;
        case 'x': o.one_fs = 1; break;
        case KEEP_DIRLINK_OPT: break;      /* measured: GNU 9.7 copies the same */
        case 'Z': break;
        case CONTEXT_OPT: if (su_optarg) context_warn++; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [-T] SOURCE DEST\n"
                   "  or:  %s [OPTION]... SOURCE... DIRECTORY\n"
                   "  or:  %s [OPTION]... -t DIRECTORY SOURCE...\n"
                   "Copy SOURCE to DEST, or each SOURCE into DIRECTORY.\n"
                   "\n"
                   "An option's argument, when it must have one, is needed by its short\n"
                   "form too.\n"
                   "  -a, --archive                as -dR --preserve=all\n"
                   "      --attributes-only        give DEST the attributes only, not the data\n"
                   "      --backup[=CONTROL]       keep a backup of each DEST replaced\n"
                   "  -b                           as --backup, CONTROL from VERSION_CONTROL\n"
                   "      --copy-contents          with -R, read special files as files\n"
                   "  -d                           as --no-dereference --preserve=links\n"
                   "      --debug                  say how each file was copied (and -v)\n"
                   "  -f, --force                  remove a DEST that cannot be opened, then\n"
                   "                                 try again (not with -n)\n"
                   "  -i, --interactive            ask before overwriting (undoes an earlier -n)\n"
                   "  -H                           follow symbolic links named on the command\n"
                   "                                 line\n"
                   "  -l, --link                   make hard links, not copies\n"
                   "  -L, --dereference            follow every symbolic link in SOURCE\n"
                   "  -n, --no-clobber             (deprecated) leave existing files alone,\n"
                   "                                 quietly; undoes -u and an earlier -i;\n"
                   "                                 --update=none says the same\n"
                   "  -P, --no-dereference         copy symbolic links in SOURCE as links\n"
                   "  -p                           as --preserve=mode,ownership,timestamps\n"
                   "      --preserve[=ATTR_LIST]   keep these attributes\n"
                   "      --no-preserve=ATTR_LIST  do not keep these attributes\n"
                   "      --parents                name each copy by its whole SOURCE name,\n"
                   "                                 under DIRECTORY\n"
                   "  -R, -r, --recursive          copy directories and what is in them\n"
                   "      --reflink[=WHEN]         clone instead of copying (see below)\n"
                   "      --remove-destination     remove an existing DEST before opening it\n"
                   "                                 (where -f removes only after a failure)\n"
                   "      --sparse=WHEN            how to make sparse files (see below)\n"
                   "      --strip-trailing-slashes  drop trailing slashes from each SOURCE\n"
                   "  -s, --symbolic-link          make symbolic links, not copies\n"
                   "  -S, --suffix=SUFFIX          the suffix of a simple backup\n"
                   "  -t, --target-directory=DIRECTORY  copy every SOURCE into DIRECTORY\n"
                   "  -T, --no-target-directory    DEST is a file name, never a directory to\n"
                   "                                 copy into\n"
                   "      --update[=UPDATE]        which existing files to replace:\n"
                   "                                 UPDATE={all,none,none-fail,older(default)}\n"
                   "  -u                           as --update[=older]\n"
                   "  -v, --verbose                say what is being done\n"
                   "      --keep-directory-symlink  follow existing symlinks to directories\n"
                   "  -x, --one-file-system        do not go into other file systems\n"
                   "  -Z                           give DEST the default SELinux context\n"
                   "                                 (there is no SELinux here: nothing)\n"
                   "      --context[=CTX]          as -Z, or set the context CTX (ignored,\n"
                   "                                 with a warning, without SELinux)\n"
                   "      --help                   show this help and exit\n"
                   "      --version                show the version and exit\n"
                   "\n"
                   "ATTR_LIST is a comma-separated list of: mode (the permissions), ownership\n"
                   "(user and group), timestamps, links (hard links among the sources),\n"
                   "context (the security context), xattr (extended attributes), all.\n"
                   "\n"
                   "--sparse=auto (the default) makes DEST sparse where SOURCE looks sparse;\n"
                   "always makes holes of every long enough run of zero bytes; never makes\n"
                   "none.\n"
                   "\n"
                   "UPDATE: all (the default without --update) replaces every existing DEST;\n"
                   "none replaces none and is not a failure; none-fail replaces none and is\n"
                   "one, saying so; older (the default of --update) replaces a DEST older\n"
                   "than its SOURCE.\n"
                   "\n"
                   "--reflink or --reflink=always clones the data blocks, failing where that\n"
                   "cannot be done; auto falls back to copying; never always copies.\n"
                   "\n"
                   "A simple backup's suffix is '~' unless --suffix or SIMPLE_BACKUP_SUFFIX\n"
                   "gives another. --backup's CONTROL, or VERSION_CONTROL, is one of:\n"
                   "  none, off       no backups at all, even with --backup\n"
                   "  numbered, t     numbered backups\n"
                   "  existing, nil   numbered where numbered backups exist already, else\n"
                   "                    simple\n"
                   "  simple, never   simple backups always\n"
                   "\n"
                   "With --force and a backup, a regular file copied onto itself is backed\n"
                   "up first, then copied from the backup.\n",
                   su_name, su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    for (int i = 0; i < context_warn; i++) {
        fflush(stdout);
        fprintf(stderr, "%s: warning: ignoring --context; it requires an SELinux-enabled kernel\n",
                su_short);
    }
    if (o.hard_link && o.sym_link) {
        su_error(0, 0, "cannot make both hard and symbolic links");
        su_try_help(1);
    }
    o.backup = cpx_backup_choice(want_backup, backup_arg);
    if (o.backup != CPX_BK_NONE && (o.interactive == CPX_I_NO || o.update == CPX_UP_NONE ||
                                    o.update == CPX_UP_NONE_FAIL)) {
        su_error(0, 0, "--backup is mutually exclusive with -n or --update=none-fail");
        su_try_help(1);
    }
    if (o.reflink == CPX_RL_ALWAYS && o.sparse != CPX_SP_AUTO) {
        su_error(0, 0, "--reflink can be used only with --sparse=auto");
        su_try_help(1);
    }
    if (require_context)
        su_error(1, 0, "cannot preserve security context without an SELinux-enabled kernel");
    o.suffix = cpx_suffix(suffix);
    /* -n beats --update; --update=none beats -i */
    if (o.interactive == CPX_I_NO) o.update = CPX_UP_ALL;
    else if (o.update == CPX_UP_NONE || o.update == CPX_UP_NONE_FAIL) o.interactive = CPX_I_UNSPEC;
    if (o.deref == CPX_DEREF_UNDEF)
        o.deref = o.recursive ? (o.hard_link ? CPX_DEREF_ALWAYS : CPX_DEREF_NEVER)
                              : CPX_DEREF_CMDLINE;
    o.umask_v = umask(0);
    umask(o.umask_v);
    o.stdin_tty = isatty(0);

    int n = argc - su_optind;
    char **ops = argv + su_optind;
    if (n <= 0) {
        su_error(0, 0, "missing file operand");
        su_try_help(1);
    }
    if (n == 1 && !target) {
        su_error(0, 0, "missing destination file operand after %s", su_quoteaf(ops[0]));
        su_try_help(1);
    }
    if (target && no_target)
        su_error(1, 0, "cannot combine --target-directory (-t) and --no-target-directory (-T)");
    if (no_target && n > 2) {
        su_error(0, 0, "extra operand %s", su_quoteaf(ops[2]));
        su_try_help(1);
    }
    if (target) {
        cpx_target_check(target, 1);
    } else if (!no_target) {
        struct stat st;
        if (n > 2) {
            cpx_target_check(ops[n - 1], 0);
            target = ops[--n];
        } else if (stat(ops[1], &st) == 0 && S_ISDIR(st.st_mode)) {
            target = ops[--n];
        }
    }
    if (o.parents && !target) {
        su_error(0, 0, "with --parents, the destination must be a directory");
        su_try_help(1);
    }
    if (!target) return cpx_copy(&o, ops[0], ops[1], 1, 0, 0);

    int rc = 0;
    for (int i = 0; i < n; i++) {
        char *src = cpx_dup(ops[i]);
        if (strip) {
            size_t l = strlen(src);
            while (l > 1 && src[l - 1] == '/') src[--l] = 0;
        }
        struct cpx_pdir *made = 0;
        size_t nmade = 0;
        if (o.parents && cpx_parents(&o, src, target, &made, &nmade)) {
            rc = 1;
        } else {
            char *dst = cpx_into(target, src, o.parents);
            rc |= cpx_copy(&o, src, dst, 1, 0, 0);
            free(dst);
        }
        /* the directories --parents made: their modes, -p's attributes */
        for (size_t k = nmade; k-- > 0;) {
            rc |= cpx_attrs(&o, made[k].dst, &made[k].st, 0, 1);
            free(made[k].dst);
        }
        free(made);
        free(src);
    }
    return rc;
}
