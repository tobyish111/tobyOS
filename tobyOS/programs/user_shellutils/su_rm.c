/* su_rm.c -- rm, as GNU coreutils 9.7's (logs/utilgate/rm.sh).
 *
 *   rm [OPTION]... [FILE]...
 *
 * Everything here was measured by running GNU's rm (gnurm in WSL), with
 * strace where the order of its system calls decides a message:
 *
 *   - A FILE is lstat'd first: a missing one is "cannot remove 'F': No
 *     such file or directory" -- not an error at all under -f, which also
 *     forgives ENOTDIR ("rm -f file/") but nothing else (ELOOP, EACCES).
 *     More than one trailing slash is cut to one ("rm d//" talks of 'd/'),
 *     except that "//" stays "//" and "///" is "/" -- fts's rule.
 *   - A directory without -r or -d: "Is a directory". With -d alone it
 *     must be empty ("Directory not empty" first, before anything else is
 *     looked at); one that cannot be read is tried anyway and asked about
 *     as "attempt removal of inaccessible directory".
 *   - Recursively: '.' or '..' as the last component is refused
 *     ("refusing to remove '.' or '..' directory: skipping 'X'"), '/' is
 *     refused unless --no-preserve-root (which may not be abbreviated),
 *     and --preserve-root=all refuses an operand on another device than
 *     its parent. --one-file-system skips a directory below an operand on
 *     another device. Entries are taken in the order the directory gives
 *     them, as fts does.
 *   - A directory that cannot be read is removed if it is empty; if not,
 *     the read's error is the one reported ("cannot remove 'p/d':
 *     Permission denied") and every directory above it is left in place
 *     SILENTLY -- a child that stays after an error, or after "no" to
 *     "descend into", keeps its parents without another word; after "no"
 *     to removing a file the parent is still asked about and tried, and
 *     is then "Directory not empty".
 *   - "x/" where x is a link to a directory goes through the link: its
 *     contents go, and then x/ itself is "Not a directory".
 *   - Prompts (on stderr, the answer one line of stdin, yes when it starts
 *     with y or Y): -i asks for everything ("remove regular empty file",
 *     "descend into directory", "remove write-protected directory" ...);
 *     without -i or -f a write-protected file is asked about only when
 *     stdin is a terminal (or ---presume-input-tty, GNU's hidden option).
 *     -I asks once, "remove N arguments?" for more than three or
 *     "remove N argument(s) recursively?" with -r. With -i, a directory
 *     that cannot be read is an error at once (no prompt) unless -d.
 *   - -v: "removed 'F'", "removed directory 'D'".
 *   - Status 1 if anything could not be removed; a "no" is not a failure.
 *
 * The removal engine (rmf_remove) is shared: mv removes what it copied
 * across filesystems with it. rmf_yesno, rmf_argmatch, rmf_join and
 * rmf_canon are shared with su_cp.c and su_mv.c, which must be included
 * after this file (su_rm.c, su_cp.c, su_mv.c, in that order).
 *
 * tobyOS: rmdir and lstat of "link/" are made to answer as Linux does
 * (ENOTDIR from rmdir, the target from lstat), so rm's view of a symbolic
 * link with a trailing slash does not depend on the VFS's.
 *
 * It took -rRfvi, ignored -i, said "cannot open dir" and "'d' is a
 * directory (use -r)", and printed a usage line for no operands. */

enum { RMF_NEVER, RMF_SOMETIMES, RMF_ALWAYS };

struct rmf_opts {
    int force;          /* ENOENT and ENOTDIR are not errors */
    int interactive;    /* RMF_NEVER, RMF_SOMETIMES, RMF_ALWAYS */
    int recursive;
    int dirs;           /* -d: remove empty directories */
    int verbose;
    int one_fs;
    int preserve_root;  /* 0, 1, or 2 for --preserve-root=all */
    int stdin_tty;
    int have_root;
    dev_t root_dev;
    ino_t root_ino;
};

/* ---- shared with cp and mv ---------------------------------------------- */

/* One line of stdin is the answer: yes when it starts with y or Y (the C
 * locale's yesexpr). The whole line is consumed, so the next prompt reads
 * the next line; end of input is no. */
static int rmf_yesno(void) {
    fflush(stderr);
    int c = getchar(), yes = (c == 'y' || c == 'Y');
    while (c != EOF && c != '\n') c = getchar();
    return yes;
}

/* GNU's argmatch: ARG names one of NAMES (an exact name, or a prefix that
 * names only one value -- prefixes of several names with one value are
 * fine); its value, or the "invalid"/"ambiguous" message with the list of
 * valid arguments (names with one value on one line) and exit 1. */
static int rmf_argmatch(const char *context, const char *arg,
                        const char *const *names, const int *vals) {
    size_t len = strlen(arg);
    int found = -1, ambiguous = 0;
    for (int i = 0; names[i]; i++) {
        if (strncmp(names[i], arg, len)) continue;
        if (strlen(names[i]) == len) return vals[i];
        if (found < 0) found = i;
        else if (vals[i] != vals[found]) ambiguous = 1;
    }
    if (found >= 0 && !ambiguous) return vals[found];
    fflush(stdout);
    fprintf(stderr, "%s: %s argument %s for %s\nValid arguments are:", su_short,
            found >= 0 ? "ambiguous" : "invalid", su_quote(arg), su_quote(context));
    for (int i = 0; names[i]; i++) {
        if (i && vals[i] == vals[i - 1]) fprintf(stderr, ", %s", su_quote(names[i]));
        else fprintf(stderr, "\n  - %s", su_quote(names[i]));
    }
    fputc('\n', stderr);
    su_try_help(1);
}

/* PARENT/NAME, without doubling a slash PARENT ends in ("t/" + "a" is
 * "t/a", as fts names it). Freed by the caller. */
static char *rmf_join(const char *parent, const char *name) {
    size_t pl = strlen(parent), nl = strlen(name);
    char *s = (char *)malloc(pl + nl + 2);
    if (!s) su_error(1, ENOMEM, "memory exhausted");
    memcpy(s, parent, pl);
    if (pl && parent[pl - 1] != '/') s[pl++] = '/';
    memcpy(s + pl, name, nl + 1);
    return s;
}

/* PATH made absolute, with ".", ".." and every symbolic link resolved --
 * realpath(3), written here because libtoby's only prefixes the working
 * directory. Malloc'd; 0 with errno when a component does not resolve.
 * Used where an inode number is missing (0: the filesystem has none) and
 * "is this the same file" must still be answered, and to follow a link. */
static char *rmf_canon(const char *path) {
    size_t cap = 8192;
    char *out = (char *)malloc(cap), *pend = (char *)malloc(cap);
    if (!out || !pend) su_error(1, ENOMEM, "memory exhausted");
    out[0] = 0;
    if (path[0] == '/') {
        snprintf(pend, cap, "%s", path);
    } else {
        char cwd[4096];
        if (!getcwd(cwd, sizeof cwd)) { free(out); free(pend); return 0; }
        snprintf(pend, cap, "%s/%s", cwd, path);
    }
    int links = 0;
    size_t at = 0;
    while (pend[at]) {
        while (pend[at] == '/') at++;
        if (!pend[at]) break;
        size_t e = at;
        while (pend[e] && pend[e] != '/') e++;
        size_t cl = e - at, ol = strlen(out);
        if (cl == 1 && pend[at] == '.') { at = e; continue; }
        if (cl == 2 && pend[at] == '.' && pend[at + 1] == '.') {
            while (ol > 0 && out[ol - 1] != '/') ol--;
            if (ol > 0) ol--;
            out[ol] = 0;
            at = e;
            continue;
        }
        if (ol + cl + 2 >= cap) { errno = ENAMETOOLONG; free(out); free(pend); return 0; }
        out[ol] = '/';
        memcpy(out + ol + 1, pend + at, cl);
        out[ol + 1 + cl] = 0;
        at = e;
        struct stat st;
        if (lstat(out, &st) != 0) { free(out); free(pend); return 0; }
        if (!S_ISLNK(st.st_mode)) continue;
        if (++links > 40) { errno = ELOOP; free(out); free(pend); return 0; }
        char target[4096];
        ssize_t n = readlink(out, target, sizeof target - 1);
        if (n < 0) { free(out); free(pend); return 0; }
        target[n] = 0;
        out[ol] = 0;                          /* the link's own name goes */
        if (target[0] == '/') out[0] = 0;
        char *next = (char *)malloc(cap);
        if (!next) su_error(1, ENOMEM, "memory exhausted");
        if ((size_t)snprintf(next, cap, "%s%s", target, pend + at) >= cap) {
            free(next); free(out); free(pend);
            errno = ENAMETOOLONG;
            return 0;
        }
        free(pend);
        pend = next;
        at = 0;
    }
    if (!out[0]) strcpy(out, "/");
    free(pend);
    return out;
}

/* "regular empty file", "directory" ... -- the words GNU's prompts use */
static const char *rmf_type(const struct stat *st) {
    if (S_ISREG(st->st_mode)) return st->st_size ? "regular file" : "regular empty file";
    if (S_ISDIR(st->st_mode)) return "directory";
    if (S_ISLNK(st->st_mode)) return "symbolic link";
    if (S_ISFIFO(st->st_mode)) return "fifo";
    if (S_ISSOCK(st->st_mode)) return "socket";
    if (S_ISCHR(st->st_mode)) return "character special file";
    if (S_ISBLK(st->st_mode)) return "block special file";
    return "weird file";
}

#ifndef SU_HOST
/* the name with a trailing slash or more, stripped; 0 when there is none
 * (tobyOS only: Linux answers trailing slashes itself) */
static char *rmf_noslash(const char *name) {
    size_t n = strlen(name);
    if (n < 2 || name[n - 1] != '/') return 0;
    while (n > 1 && name[n - 1] == '/') n--;
    char *s = strndup(name, n);
    if (!s) su_error(1, ENOMEM, "memory exhausted");
    return s;
}
#endif

/* lstat as Linux answers it: "x/" follows a symbolic link x, and is
 * ENOTDIR when what it names is not a directory */
static int rmf_lstat(const char *name, struct stat *st) {
#ifdef SU_HOST
    return lstat(name, st);
#else
    char *base = rmf_noslash(name);
    if (!base) return lstat(name, st);
    free(base);
    if (stat(name, st) != 0) return -1;
    if (!S_ISDIR(st->st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
#endif
}

/* rmdir as Linux answers it: "link/" is ENOTDIR, not the directory the
 * link names */
static int rmf_rmdir(const char *name) {
#ifndef SU_HOST
    char *base = rmf_noslash(name);
    if (base) {
        struct stat st;
        int islink = lstat(base, &st) == 0 && S_ISLNK(st.st_mode);
        free(base);
        if (islink) { errno = ENOTDIR; return -1; }
    }
#endif
    return rmdir(name);
}

/* write-protected: not writable by us (a symbolic link never is) */
static int rmf_wprot(const char *name) {
    return access(name, W_OK) != 0 && errno == EACCES;
}

/* the last component, trailing slashes aside, is "." or ".." */
static int rmf_dotname(const char *name) {
    size_t n = strlen(name);
    while (n > 1 && name[n - 1] == '/') n--;
    size_t b = n;
    while (b > 0 && name[b - 1] != '/') b--;
    size_t l = n - b;
    return (l == 1 && name[b] == '.') || (l == 2 && name[b] == '.' && name[b + 1] == '.');
}

/* fts's trim: more than one trailing slash becomes one, but a name of
 * exactly two slashes is left alone ("//" may be special) */
static char *rmf_trim(const char *arg) {
    size_t n = strlen(arg);
    if (n > 2 && arg[n - 1] == '/')
        while (n > 1 && arg[n - 2] == '/') n--;
    char *s = strndup(arg, n);
    if (!s) su_error(1, ENOMEM, "memory exhausted");
    return s;
}

/* Every name in directory PATH but . and .., in the order it gives them
 * (as fts does); NULL with errno when it cannot be read. */
static char **rmf_names(const char *path, size_t *count) {
    DIR *d = opendir(path);
    if (!d) return 0;
    size_t n = 0, cap = 16;
    char **v = (char **)malloc(cap * sizeof *v);
    if (!v) su_error(1, ENOMEM, "memory exhausted");
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) {
            cap *= 2;
            v = (char **)realloc(v, cap * sizeof *v);
            if (!v) su_error(1, ENOMEM, "memory exhausted");
        }
        v[n] = strdup(e->d_name);
        if (!v[n]) su_error(1, ENOMEM, "memory exhausted");
        n++;
    }
    closedir(d);
    *count = n;
    return v;
}

static void rmf_free_names(char **v, size_t n) {
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* Is the prompt to be asked? -i always; otherwise (not -f) only for a
 * write-protected file when stdin is a terminal. *WP: write-protected. */
static int rmf_ask(const struct rmf_opts *o, const char *name, const struct stat *st, int *wp) {
    *wp = 0;
    if (o->interactive == RMF_NEVER) return 0;
    if (!S_ISLNK(st->st_mode) && (o->interactive == RMF_ALWAYS || o->stdin_tty))
        *wp = rmf_wprot(name);
    return o->interactive == RMF_ALWAYS || *wp;
}

static int rmf_prompt(const char *what, int wp, const char *type, const char *name) {
    fflush(stdout);
    fprintf(stderr, "%s: %s %s%s %s? ", su_short, what, wp ? "write-protected " : "",
            type, su_quoteaf(name));
    return rmf_yesno();
}

/* Results: 0 removed, 1 failed (said so), 2 kept because a question was
 * answered no, 3 kept because "descend into" was answered no. As GNU's:
 * a failure or a refused descent leaves every directory above it alone,
 * without a word; a refused removal does not -- the parent is still asked
 * about and tried, and is then "Directory not empty". */
static int rmf_fail(const struct rmf_opts *o, int e, const char *name) {
    if (o->force && (e == ENOENT || e == ENOTDIR)) return 0;
    su_error(0, e, "cannot remove %s", su_quoteaf(name));
    return 1;
}

static int rmf_nondir(const struct rmf_opts *o, const char *name, const struct stat *st) {
    int wp;
    if (rmf_ask(o, name, st, &wp) && !rmf_prompt("remove", wp, rmf_type(st), name)) return 2;
    if (unlink(name) != 0) return rmf_fail(o, errno, name);
    if (o->verbose) printf("removed %s\n", su_quoteaf(name));
    return 0;
}

/* rmdir NAME; RD_ERR (when nonzero) is why it could not be read, which is
 * what a "not empty" answer is reported as */
static int rmf_rmdir_say(const struct rmf_opts *o, const char *name, int rd_err) {
    if (rmf_rmdir(name) != 0) {
        int e = errno;
        if (rd_err && (e == ENOTEMPTY || e == EEXIST)) e = rd_err;
        return rmf_fail(o, e, name);
    }
    if (o->verbose) printf("removed directory %s\n", su_quoteaf(name));
    return 0;
}

/* a directory, recursively */
static int rmf_dir(const struct rmf_opts *o, const char *path, const struct stat *st,
                   dev_t top_dev) {
    int wp;
    int ask = rmf_ask(o, path, st, &wp);
    size_t n = 0;
    char **names = rmf_names(path, &n);
    int rd_err = names ? 0 : errno;
    if (ask) {
        if (!names) {
            /* with -i, a directory whose emptiness cannot be known is an
             * error before any question -- unless -d, which asks */
            if (!o->dirs) {
                su_error(0, rd_err, "cannot remove %s", su_quoteaf(path));
                return 1;
            }
            if (!rmf_prompt("remove", wp, "directory", path)) return 2;
            return rmf_rmdir_say(o, path, rd_err);
        }
        if (n == 0) {
            rmf_free_names(names, n);
            if (!rmf_prompt("remove", wp, "directory", path)) return 2;
            return rmf_rmdir_say(o, path, 0);
        }
        if (!rmf_prompt("descend into", wp, "directory", path)) {
            rmf_free_names(names, n);
            return 3;
        }
    }
    if (!names) return rmf_rmdir_say(o, path, rd_err);
    int err = 0, kept = 0;      /* KEPT: an entry stays that silences us */
    for (size_t i = 0; i < n; i++) {
        char *child = rmf_join(path, names[i]);
        struct stat cs;
        int r;
        if (lstat(child, &cs) != 0) {
            r = rmf_fail(o, errno, child);
        } else if (S_ISDIR(cs.st_mode)) {
            if (o->one_fs && cs.st_dev != top_dev) {
                su_error(0, 0, "skipping %s, since it's on a different device",
                         su_quoteaf(child));
                r = 1;
            } else {
                r = rmf_dir(o, child, &cs, top_dev);
            }
        } else {
            r = rmf_nondir(o, child, &cs);
        }
        if (r == 1) err = 1;
        if (r == 1 || r == 3) kept = 1;
        free(child);
    }
    rmf_free_names(names, n);
    if (kept) return err ? 1 : 3;
    struct stat now;
    if (lstat(path, &now) == 0 && rmf_ask(o, path, &now, &wp) &&
        !rmf_prompt("remove", wp, "directory", path))
        return 2;
    return rmf_rmdir_say(o, path, 0);
}

/* the operand is the root directory: by its device and inode, or by a
 * name of nothing but slashes (a second guard that cannot be fooled by a
 * filesystem with no inode numbers) */
static int rmf_is_root(const struct rmf_opts *o, const char *name, const struct stat *st) {
    const char *p = name;
    while (*p == '/') p++;
    if (*name == '/' && !*p) return 1;
    if (!o->have_root) return 0;
    if (st->st_ino == 0 || o->root_ino == 0) {
        char *r = rmf_canon(name);
        int is = r && !strcmp(r, "/");
        free(r);
        return is;
    }
    return st->st_dev == o->root_dev && st->st_ino == o->root_ino;
}

/* One operand, as typed. 0 removed, 1 failed, 2 kept. */
static int rmf_remove(const struct rmf_opts *o, const char *arg) {
    char *name = rmf_trim(arg);
    struct stat st;
    int r;
    if (rmf_lstat(name, &st) != 0) {
        r = rmf_fail(o, errno, name);
    } else if (!S_ISDIR(st.st_mode)) {
        r = rmf_nondir(o, name, &st);
    } else if (!o->recursive && !o->dirs) {
        su_error(0, EISDIR, "cannot remove %s", su_quoteaf(name));
        r = 1;
    } else if (!o->recursive) {
        /* -d: it must be empty; one that cannot be read is tried anyway */
        size_t n = 0;
        char **names = rmf_names(name, &n);
        int rd_err = names ? 0 : errno, wp;
        if (names) rmf_free_names(names, n);
        if (names && n) {
            su_error(0, ENOTEMPTY, "cannot remove %s", su_quoteaf(name));
            r = 1;
        } else if (rmf_dotname(name)) {
            su_error(0, 0, "refusing to remove '.' or '..' directory: skipping %s",
                     su_quoteaf(name));
            r = 1;
        } else if (rmf_ask(o, name, &st, &wp) &&
                   !(names ? rmf_prompt("remove", wp, "directory", name)
                           : (fflush(stdout),
                              fprintf(stderr, "%s: attempt removal of inaccessible directory %s? ",
                                      su_short, su_quoteaf(name)),
                              rmf_yesno()))) {
            r = 2;
        } else {
            r = rmf_rmdir_say(o, name, rd_err);
        }
    } else if (rmf_dotname(name)) {
        su_error(0, 0, "refusing to remove '.' or '..' directory: skipping %s",
                 su_quoteaf(name));
        r = 1;
    } else if (o->preserve_root && rmf_is_root(o, name, &st)) {
        if (strcmp(name, "/"))
            su_error(0, 0, "it is dangerous to operate recursively on %s (same as '/')",
                     su_quoteaf(name));
        else
            su_error(0, 0, "it is dangerous to operate recursively on %s", su_quoteaf(name));
        su_error(0, 0, "use --no-preserve-root to override this failsafe");
        r = 1;
    } else {
        struct stat up;
        char *parent = rmf_join(name, "..");
        int other = o->preserve_root == 2 && lstat(parent, &up) == 0 && up.st_dev != st.st_dev;
        free(parent);
        if (other) {
            su_error(0, 0, "skipping %s, since it's on a different device", su_quoteaf(name));
            su_error(0, 0, "and --preserve-root=all is in effect");
            r = 1;
        } else {
            r = rmf_dir(o, name, &st, st.st_dev);
        }
    }
    free(name);
    return r;
}

/* the root's identity, for --preserve-root */
static void rmf_get_root(struct rmf_opts *o) {
    struct stat st;
    if (lstat("/", &st) != 0) su_error(1, errno, "failed to get attributes of %s", su_quoteaf("/"));
    o->have_root = 1;
    o->root_dev = st.st_dev;
    o->root_ino = st.st_ino;
}

static int do_rm(int argc, char **argv) {
    enum { INTERACTIVE_OPT = 0x100, ONE_FS_OPT, NO_PRESERVE_OPT, PRESERVE_OPT, PRESUME_TTY_OPT };
    static const struct su_option longs[] = {
        { "force", SU_NO_ARG, 0, 'f' },
        { "interactive", SU_OPT_ARG, 0, INTERACTIVE_OPT },
        { "one-file-system", SU_NO_ARG, 0, ONE_FS_OPT },
        { "no-preserve-root", SU_NO_ARG, 0, NO_PRESERVE_OPT },
        { "preserve-root", SU_OPT_ARG, 0, PRESERVE_OPT },
        { "-presume-input-tty", SU_NO_ARG, 0, PRESUME_TTY_OPT },
        { "recursive", SU_NO_ARG, 0, 'r' },
        { "dir", SU_NO_ARG, 0, 'd' },
        { "verbose", SU_NO_ARG, 0, 'v' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    static const char *const when_names[] = { "never", "no", "none", "once", "always", "yes", 0 };
    static const int when_vals[] = { 0, 0, 0, 1, 2, 2 };
    struct rmf_opts o;
    memset(&o, 0, sizeof o);
    o.interactive = RMF_SOMETIMES;
    o.preserve_root = 1;
    int prompt_once = 0, presume_tty = 0, c;
    while ((c = su_getopt_long(argc, argv, "dfirvIR", longs, 0)) != -1) {
        switch (c) {
        case 'd': o.dirs = 1; break;
        case 'f': o.interactive = RMF_NEVER; o.force = 1; prompt_once = 0; break;
        case 'i': o.interactive = RMF_ALWAYS; o.force = 0; prompt_once = 0; break;
        case 'I': o.interactive = RMF_SOMETIMES; o.force = 0; prompt_once = 1; break;
        case 'r': case 'R': o.recursive = 1; break;
        case 'v': o.verbose = 1; break;
        case INTERACTIVE_OPT: {
            int w = su_optarg ? rmf_argmatch("--interactive", su_optarg, when_names, when_vals) : 2;
            if (w == 0) { o.interactive = RMF_NEVER; prompt_once = 0; }
            else if (w == 1) { o.interactive = RMF_SOMETIMES; o.force = 0; prompt_once = 1; }
            else { o.interactive = RMF_ALWAYS; o.force = 0; prompt_once = 0; }
            break;
        }
        case ONE_FS_OPT: o.one_fs = 1; break;
        case NO_PRESERVE_OPT:
            /* GNU will not take this one abbreviated */
            if (strcmp(argv[su_optind - 1], "--no-preserve-root"))
                su_error(1, 0, "you may not abbreviate the --no-preserve-root option");
            o.preserve_root = 0;
            break;
        case PRESERVE_OPT:
            if (su_optarg) {
                if (strcmp(su_optarg, "all"))
                    su_error(1, 0, "unrecognized --preserve-root argument: %s",
                             su_quoteaf(su_optarg));
                o.preserve_root = 2;
            } else {
                o.preserve_root = 1;
            }
            break;
        case PRESUME_TTY_OPT: presume_tty = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n"
                   "Remove each FILE. Directories stay unless -r (or, empty, -d) says\n"
                   "otherwise.\n"
                   "\n"
                   "  -f, --force           never ask, and say nothing of a FILE that does\n"
                   "                          not exist\n"
                   "  -i                    ask before each removal\n"
                   "  -I                    ask once, before removing more than three\n"
                   "                          FILEs or before any recursive removal\n"
                   "      --interactive[=WHEN]  when to ask: never, once (as -I) or\n"
                   "                          always (as -i, and the default WHEN)\n"
                   "      --one-file-system  in a recursive removal, leave alone any\n"
                   "                          directory on a different file system from\n"
                   "                          the FILE it was found under\n"
                   "      --no-preserve-root  let '/' be removed like anything else\n"
                   "      --preserve-root[=all]  refuse to remove '/' (the default); with\n"
                   "                          'all', also refuse a FILE on a different\n"
                   "                          device from its parent\n"
                   "  -r, -R, --recursive   remove directories and everything in them\n"
                   "  -d, --dir             remove directories that are empty\n"
                   "  -v, --verbose         say what each removal was\n"
                   "      --help            show this help and exit\n"
                   "      --version         show the version and exit\n"
                   "\n"
                   "A FILE whose last component is '.' or '..' is never removed.\n"
                   "A name starting with '-' needs '--' before it, or a './' in front:\n"
                   "  %s -- -foo\n"
                   "  %s ./-foo\n"
                   "What rm removes may still be recoverable; shred(1) overwrites first.\n",
                   su_name, su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    int n = argc - su_optind;
    if (n <= 0) {
        if (o.force) return 0;
        su_error(0, 0, "missing operand");
        su_try_help(1);
    }
    o.stdin_tty = presume_tty || isatty(0);
    if (o.recursive && o.preserve_root) rmf_get_root(&o);
    if (prompt_once && (o.recursive || n > 3)) {
        fflush(stdout);
        if (o.recursive)
            fprintf(stderr, "%s: remove %d argument%s recursively? ", su_short, n, n == 1 ? "" : "s");
        else
            fprintf(stderr, "%s: remove %d arguments? ", su_short, n);
        if (!rmf_yesno()) return 0;
    }
    int rc = 0;
    for (int i = su_optind; i < argc; i++)
        if (rmf_remove(&o, argv[i]) == 1) rc = 1;
    return rc;
}
