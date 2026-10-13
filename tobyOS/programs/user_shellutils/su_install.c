/* su_install.c -- install, as GNU coreutils 9.7's (logs/utilgate/install.sh).
 *
 *   install [OPTION]... [-T] SOURCE DEST
 *   install [OPTION]... SOURCE... DIRECTORY
 *   install [OPTION]... -t DIRECTORY SOURCE...
 *   install [OPTION]... -d DIRECTORY...
 *
 *   - Each file copied and given -m's MODE (rwxr-xr-x by default) -- a
 *     symbolic MODE taken from nothing, no umask (`-m u+x` is ---x------),
 *     as GNU's; -o and -g its owner and group; -p its source's times; -s
 *     stripped (--strip-program); -C left alone when it already matches
 *     ("removed 'F'" when it does not); -b/--backup[=CONTROL] and -S keep
 *     what it replaces (none off, simple never, existing nil, numbered t;
 *     VERSION_CONTROL and SIMPLE_BACKUP_SUFFIX as GNU reads them).
 *   - -d makes each DIRECTORY and its parents (0755 under umask 022, -m for
 *     the last); -D makes DEST's (or -t's) leading directories.
 *   - -v: "'SRC' -> 'DEST'" (with " (backup: 'B')"), "install: creating
 *     directory 'D'".
 *   - GNU's messages, word for word: "missing file operand", "missing
 *     destination file operand after 'A'", "target 'T': Not a
 *     directory", "cannot stat 'S'", "omitting directory 'D'", "'A' and
 *     'B' are the same file", "cannot create regular file 'F'", "invalid
 *     mode 'M'", "invalid user 'U'", "invalid group 'G'", "cannot change
 *     ownership of 'F'", "failed to access 'D'", "cannot overwrite
 *     directory 'D' with non-directory 'S'", "target directory not
 *     allowed when installing a directory".
 *   - -c is taken (GNU ignores it); -Z and --context=CTX ask for an SELinux
 *     context and there is none here.
 *
 * It took -m as octal only, -d without -m's mode for the last directory,
 * every other option as nothing, and said "install: SRC: ERROR". */

#include <sys/time.h>

enum { BK_NONE, BK_SIMPLE, BK_EXISTING, BK_NUMBERED };

struct in_opts {
    mode_t mode;
    int have_mode, verbose, compare, preserve, strip, makedirs;
    int backup;
    const char *suffix, *strip_prog;
    long uid, gid;            /* -1: leave */
    int fail;
};

static int in_backup_type(const char *s) {
    static const struct { const char *n; int t; } k[] = {
        { "none", BK_NONE }, { "off", BK_NONE }, { "simple", BK_SIMPLE },
        { "never", BK_SIMPLE }, { "existing", BK_EXISTING }, { "nil", BK_EXISTING },
        { "numbered", BK_NUMBERED }, { "t", BK_NUMBERED },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!strcmp(s, k[i].n)) return k[i].t;
    fflush(stdout);
    fprintf(stderr, "%s: invalid argument %s for %s\nValid arguments are:\n"
            "  - 'none', 'off'\n  - 'simple', 'never'\n  - 'existing', 'nil'\n"
            "  - 'numbered', 't'\n", su_short, su_quote(s), su_quote("backup type"));
    su_try_help(1);
}

/* the highest N of PATH.~N~ there is, 0 for none */
static int in_max_numbered(const char *path) {
    const char *slash = strrchr(path, '/');
    char dir[4096];
    const char *base = slash ? slash + 1 : path;
    if (slash) snprintf(dir, sizeof dir, "%.*s", (int)(slash - path), path);
    else snprintf(dir, sizeof dir, ".");
    if (!*dir) snprintf(dir, sizeof dir, "/");
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t bl = strlen(base);
    int max = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        if (strncmp(n, base, bl) || n[bl] != '.' || n[bl + 1] != '~') continue;
        const char *p = n + bl + 2;
        char *end;
        long v = strtol(p, &end, 10);
        if (end != p && end[0] == '~' && !end[1] && v > max) max = (int)v;
    }
    closedir(d);
    return max;
}

/* back DEST up as asked; the backup's name in OUT, or "" for none */
static int in_backup(const struct in_opts *o, const char *dest, char *out, size_t cap) {
    out[0] = 0;
    if (o->backup == BK_NONE) return 0;
    struct stat st;
    if (lstat(dest, &st) != 0) return 0;
    int type = o->backup;
    int maxn = in_max_numbered(dest);
    if (type == BK_EXISTING) type = maxn ? BK_NUMBERED : BK_SIMPLE;
    if (type == BK_NUMBERED) snprintf(out, cap, "%s.~%d~", dest, maxn + 1);
    else snprintf(out, cap, "%s%s", dest, o->suffix);
    if (rename(dest, out) != 0) {
        su_error(0, errno, "cannot backup %s", su_quoteaf(dest));
        out[0] = 0;
        return -1;
    }
    return 0;
}

/* make PATH and every missing parent; MODE for the last, LAST_ONLY */
static int in_mkdirs(const struct in_opts *o, const char *path, mode_t last_mode, int set_last) {
    char *p = strdup(path);
    if (!p) su_error(1, ENOMEM, "memory exhausted");
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    int rc = 0;
    for (char *q = p + 1;; q++) {
        int last = !*q;
        if (*q != '/' && !last) continue;
        char saved = *q;
        *q = 0;
        struct stat st;
        if (stat(p, &st) != 0) {
            if (mkdir(p, 0777) != 0) {
                su_error(0, errno, "cannot create directory %s", su_quoteaf(p));
                rc = 1;
                break;
            }
            if (o->verbose)
                printf("%s: creating directory %s\n", su_short, su_quoteaf(p));
            if (last && set_last && chmod(p, last_mode) != 0) {
                su_error(0, errno, "cannot change permissions of %s", su_quoteaf(p));
                rc = 1;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            su_error(0, ENOTDIR, "cannot create directory %s", su_quoteaf(p));
            rc = 1;
            break;
        } else if (last && set_last && chmod(p, last_mode) != 0) {
            su_error(0, errno, "cannot change permissions of %s", su_quoteaf(p));
            rc = 1;
        }
        *q = saved;
        if (last) break;
    }
    free(p);
    return rc;
}

static int in_same_content(const char *a, const char *b) {
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
    int same = fa >= 0 && fb >= 0;
    static char ba[8192], bb[8192];
    while (same) {
        ssize_t ra = read(fa, ba, sizeof ba), rb = read(fb, bb, sizeof bb);
        if (ra != rb || ra < 0) { same = 0; break; }
        if (ra == 0) break;
        if (memcmp(ba, bb, (size_t)ra)) same = 0;
    }
    if (fa >= 0) close(fa);
    if (fb >= 0) close(fb);
    return same;
}

static int in_copy_file(const struct in_opts *o, const char *src, const char *dest) {
    struct stat ss, ds;
    if (stat(src, &ss) != 0) {
        su_error(0, errno, "cannot stat %s", su_quoteaf(src));
        return 1;
    }
    if (S_ISDIR(ss.st_mode)) {
        su_error(0, 0, "omitting directory %s", su_quoteaf(src));
        return 1;
    }
    int dexists = stat(dest, &ds) == 0;
    if (dexists && ss.st_ino && ss.st_ino == ds.st_ino && ss.st_dev == ds.st_dev) {
        su_error(0, 0, "%s and %s are the same file", su_quoteaf(src), su_quoteaf(dest));
        return 1;
    }
    if (dexists && S_ISDIR(ds.st_mode)) {
        su_error(0, 0, "cannot overwrite directory %s with non-directory %s",
                 su_quoteaf(dest), su_quoteaf(src));
        return 1;
    }
    if (o->compare && dexists && S_ISREG(ds.st_mode) && !o->strip &&
        (ds.st_mode & 07777) == o->mode &&
        (o->uid < 0 ? ds.st_uid == geteuid() : (long)ds.st_uid == o->uid) &&
        (o->gid < 0 ? 1 : (long)ds.st_gid == o->gid) &&
        ds.st_size == ss.st_size && in_same_content(src, dest))
        return 0;
    char bk[4200] = "";
    if (dexists) {
        if (o->backup != BK_NONE) {
            if (in_backup(o, dest, bk, sizeof bk) != 0) return 1;
        } else {
            if (unlink(dest) != 0 && errno != ENOENT) {
                su_error(0, errno, "cannot remove %s", su_quoteaf(dest));
                return 1;
            }
            if (o->compare && o->verbose) printf("removed %s\n", su_quoteaf(dest));
        }
    }
    int in = su_open_read(src, 0);
    if (in < 0) {
        su_error(0, errno, "cannot open %s for reading", su_quoteaf(src));
        return 1;
    }
    int out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0) {
        su_error(0, errno, "cannot create regular file %s", su_quoteaf(dest));
        close(in);
        return 1;
    }
    static char buf[65536];
    ssize_t r;
    int rc = 0;
    while ((r = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w <= 0) { su_error(0, errno, "error writing %s", su_quoteaf(dest)); rc = 1; break; }
            off += w;
        }
        if (rc) break;
    }
    if (r < 0) { su_error(0, errno, "error reading %s", su_quoteaf(src)); rc = 1; }
    close(in);
    close(out);
    if (rc) return 1;
    if (o->strip) {
        pid_t pid = fork();
        if (pid == 0) {
            char *av[] = { (char *)o->strip_prog, (char *)dest, 0 };
            execvp(av[0], av);
            su_error(0, errno, "cannot run %s", su_quoteaf(o->strip_prog));
            _exit(127);
        }
        int st = 0;
        if (pid < 0 || waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) {
            su_error(0, 0, "strip process terminated abnormally");
            unlink(dest);
            return 1;
        }
    }
    if (o->uid >= 0 || o->gid >= 0) {
        if (chown(dest, o->uid >= 0 ? (uid_t)o->uid : (uid_t)-1,
                  o->gid >= 0 ? (gid_t)o->gid : (gid_t)-1) != 0) {
            su_error(0, errno, "cannot change ownership of %s", su_quoteaf(dest));
            rc = 1;
        }
    }
    if (chmod(dest, o->mode) != 0) {
        su_error(0, errno, "cannot change permissions of %s", su_quoteaf(dest));
        rc = 1;
    }
    if (o->preserve) {
        struct timeval tv[2];
        tv[0].tv_sec = ss.st_atime; tv[0].tv_usec = 0;
        tv[1].tv_sec = ss.st_mtime; tv[1].tv_usec = 0;
        if (utimes(dest, tv) != 0) {
            su_error(0, errno, "failed to preserve times for %s", su_quoteaf(dest));
            rc = 1;
        }
    }
    if (o->verbose) {
        printf("%s -> %s", su_quoteaf(src), su_quoteaf(dest));
        if (*bk) printf(" (backup: %s)", su_quoteaf(bk));
        putchar('\n');
    }
    return rc;
}

static long in_id(const char *db, const char *s) {
    char name[1024];
    FILE *f = fopen(db, "r");
    if (f) {
        while (fgets(name, sizeof name, f)) {
            char *c1 = strchr(name, ':');
            if (!c1) continue;
            *c1 = 0;
            if (strcmp(name, s)) continue;
            char *c2 = strchr(c1 + 1, ':');
            if (!c2) continue;
            fclose(f);
            return strtol(c2 + 1, 0, 10);
        }
        fclose(f);
    }
    char *e;
    long v = strtol(s, &e, 10);
    if (*s && !*e && v >= 0) return v;
    return -2;
}

static int do_install(int argc, char **argv) {
    enum { BACKUP_OPT = 0x100, STRIPPROG_OPT, CONTEXT_OPT, PRESCTX_OPT, DEBUG_OPT };
    static const struct su_option longs[] = {
        { "backup", SU_OPT_ARG, 0, BACKUP_OPT },
        { "compare", SU_NO_ARG, 0, 'C' },
        { "directory", SU_NO_ARG, 0, 'd' },
        { "group", SU_REQ_ARG, 0, 'g' },
        { "mode", SU_REQ_ARG, 0, 'm' },
        { "owner", SU_REQ_ARG, 0, 'o' },
        { "preserve-timestamps", SU_NO_ARG, 0, 'p' },
        { "strip", SU_NO_ARG, 0, 's' },
        { "strip-program", SU_REQ_ARG, 0, STRIPPROG_OPT },
        { "suffix", SU_REQ_ARG, 0, 'S' },
        { "target-directory", SU_REQ_ARG, 0, 't' },
        { "no-target-directory", SU_NO_ARG, 0, 'T' },
        { "verbose", SU_NO_ARG, 0, 'v' },
        { "preserve-context", SU_NO_ARG, 0, PRESCTX_OPT },
        { "context", SU_OPT_ARG, 0, CONTEXT_OPT },
        { "debug", SU_NO_ARG, 0, DEBUG_OPT },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    struct in_opts o;
    memset(&o, 0, sizeof o);
    o.mode = 0755;
    o.uid = o.gid = -1;
    o.strip_prog = "strip";
    o.suffix = getenv("SIMPLE_BACKUP_SUFFIX");
    if (!o.suffix || !*o.suffix || strchr(o.suffix, '/')) o.suffix = "~";
    int dirs = 0, no_target = 0, c;
    const char *target = 0, *mstr = 0, *owner = 0, *group = 0, *backup_arg = 0;
    int want_backup = 0;
    while ((c = su_getopt_long(argc, argv, "bcCsDdg:m:o:pt:TvS:Z", longs, 0)) != -1) {
        switch (c) {
        case 'b': want_backup = 1; break;
        case BACKUP_OPT: want_backup = 1; backup_arg = su_optarg; break;
        case 'c': break;
        case 'C': o.compare = 1; break;
        case 's': o.strip = 1; break;
        case STRIPPROG_OPT: o.strip_prog = su_optarg; break;
        case 'D': o.makedirs = 1; break;
        case 'd': dirs = 1; break;
        case 'g': group = su_optarg; break;
        case 'm': mstr = su_optarg; break;
        case 'o': owner = su_optarg; break;
        case 'p': o.preserve = 1; break;
        case 't': target = su_optarg; break;
        case 'T': no_target = 1; break;
        case 'v': o.verbose = 1; break;
        case 'S': o.suffix = su_optarg; want_backup = 1; break;
        case 'Z': case CONTEXT_OPT: case PRESCTX_OPT: case DEBUG_OPT: break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [-T] SOURCE DEST\n"
                   "  or:  %s [OPTION]... SOURCE... DIRECTORY\n"
                   "  or:  %s [OPTION]... -t DIRECTORY SOURCE...\n"
                   "  or:  %s [OPTION]... -d DIRECTORY...\n"
                   "Copy each SOURCE to DEST, or into DIRECTORY, and give the copies\n"
                   "their mode, owner and group; with -d, make each DIRECTORY.\n"
                   "\n"
                   "      --backup[=CONTROL]  keep each file replaced (none, simple,\n"
                   "                          existing, numbered)\n"
                   "  -b                      the same, CONTROL from VERSION_CONTROL\n"
                   "  -c                      (taken, as GNU's: always copies)\n"
                   "  -C, --compare           leave a file that already matches\n"
                   "  -d, --directory         make the operands as directories\n"
                   "  -D                      make DEST's leading directories too\n"
                   "  -g, --group=GROUP       give the copies GROUP\n"
                   "  -m, --mode=MODE         MODE, as chmod reads one (rwxr-xr-x)\n"
                   "  -o, --owner=OWNER       give the copies OWNER\n"
                   "  -p, --preserve-timestamps  give them their sources' times\n"
                   "  -s, --strip             strip the copies\n"
                   "      --strip-program=PROGRAM  strip with PROGRAM\n"
                   "  -S, --suffix=SUFFIX     a backup's suffix, ~ unless given\n"
                   "  -t, --target-directory=DIRECTORY  copy into DIRECTORY\n"
                   "  -T, --no-target-directory  DEST is a file name, always\n"
                   "  -v, --verbose           say what is made and copied\n"
                   "  -Z, --context[=CTX]     (taken: there is no SELinux here)\n"
                   "      --help              show this help and exit\n"
                   "      --version           show the version and exit\n",
                   su_name, su_name, su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (want_backup) {
        const char *vc = backup_arg ? backup_arg : getenv("VERSION_CONTROL");
        o.backup = vc && *vc ? in_backup_type(vc) : BK_EXISTING;
    }
    if (mstr) {
        struct su_mode m;
        if (su_mode_compile(mstr, &m) != 0) su_error(1, 0, "invalid mode %s", su_quote(mstr));
        o.mode = su_mode_adjust(0, dirs, 0, &m);
        o.have_mode = 1;
    }
    if (owner) {
        o.uid = in_id("/etc/passwd", owner);
        if (o.uid == -2) su_error(1, 0, "invalid user %s", su_quote(owner));
    }
    if (group) {
        o.gid = in_id("/etc/group", group);
        if (o.gid == -2) su_error(1, 0, "invalid group %s", su_quote(group));
    }
    int n = argc - su_optind;
    char **ops = argv + su_optind;
    if (dirs && target) su_error(1, 0, "target directory not allowed when installing a directory");
    if (target && no_target) su_error(1, 0, "cannot combine --target-directory (-t) and --no-target-directory (-T)");
    if (n == 0) {
        su_error(0, 0, "missing file operand");
        su_try_help(1);
    }
    int rc = 0;
    if (dirs) {
        for (int i = 0; i < n; i++) {
            if (in_mkdirs(&o, ops[i], o.mode, 1)) { rc = 1; continue; }
            if (o.uid >= 0 || o.gid >= 0)
                if (chown(ops[i], o.uid >= 0 ? (uid_t)o.uid : (uid_t)-1,
                          o.gid >= 0 ? (gid_t)o.gid : (gid_t)-1) != 0) {
                    su_error(0, errno, "cannot change ownership of %s", su_quoteaf(ops[i]));
                    rc = 1;
                }
        }
        return rc;
    }
    if (!target && n == 1) {
        su_error(0, 0, "missing destination file operand after %s", su_quoteaf(ops[0]));
        su_try_help(1);
    }
    struct stat st;
    if (!target && !no_target) {
        const char *last = ops[n - 1];
        if (stat(last, &st) == 0 && S_ISDIR(st.st_mode)) target = last, n--;
        else if (n > 2) {
            if (o.makedirs) { target = last; n--; }
            else su_error(1, ENOTDIR, "target %s", su_quoteaf(last));
        }
    }
    if (no_target && n > 2) {
        su_error(0, 0, "extra operand %s", su_quoteaf(ops[2]));
        su_try_help(1);
    }
    if (target) {
        if (o.makedirs && in_mkdirs(&o, target, 0755, 0)) return 1;
        if (stat(target, &st) != 0) su_error(1, errno, "failed to access %s", su_quoteaf(target));
        if (!S_ISDIR(st.st_mode)) su_error(1, ENOTDIR, "failed to access %s", su_quoteaf(target));
        for (int i = 0; i < n; i++) {
            const char *b = strrchr(ops[i], '/');
            b = b ? b + 1 : ops[i];
            char *dest = (char *)malloc(strlen(target) + strlen(b) + 2);
            if (!dest) su_error(1, ENOMEM, "memory exhausted");
            sprintf(dest, "%s%s%s", target, target[strlen(target) - 1] == '/' ? "" : "/", b);
            rc |= in_copy_file(&o, ops[i], dest);
            free(dest);
        }
        return rc;
    }
    if (o.makedirs) {
        char *d = strdup(ops[1]);
        if (!d) su_error(1, ENOMEM, "memory exhausted");
        char *s = strrchr(d, '/');
        if (s && s != d) { *s = 0; if (in_mkdirs(&o, d, 0755, 0)) { free(d); return 1; } }
        free(d);
    }
    return in_copy_file(&o, ops[0], ops[1]);
}
