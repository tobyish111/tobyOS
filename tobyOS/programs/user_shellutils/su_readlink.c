/* su_readlink.c -- readlink and realpath, as GNU coreutils 9.7's
 * (logs/utilgate/readlink.sh, realpath.sh).
 *
 * readlink NAME prints what a symbolic link holds; -f -e -m instead print
 * the name made absolute with every link in every component followed,
 * which is also what realpath does (its default is -f's rule, -e and -m
 * its others). The three rules, measured:
 *
 *   -e  every component must exist;
 *   -f  every component but the last must exist -- "the last" being one
 *       followed only by slashes, so `nosuch/` passes and `nosuch/.`
 *       does not -- and only a missing one (ENOENT) is forgiven there, not
 *       one that cannot be searched;
 *   -m  nothing need exist; a component that is not a directory, or a
 *       link that loops, is taken as a plain name and the walk goes on
 *       (`-m loop1/x` is .../loop1/x, `-m f/..` is the directory of f).
 *
 * A component that exists but is not a directory, with anything after it
 * (even one slash), is ENOTDIR under -e and -f. ".." takes off the last
 * component of what has been built so far -- links before it are already
 * followed -- and "//" is "/", as on Linux.
 *
 * LOOPS. GNU follows any number of links -- a chain of 300 resolves --
 * and calls a loop a loop only when the walk comes back to a link it has
 * been at with the same remainder to resolve; and it only starts keeping
 * that record after the 20th link. Which link is "the loop" shows under
 * -m: `-m x1` in the cycle x1 -> x2 -> x3 -> x1 prints .../x3, the link in
 * hand at the 24th step, the first repeat of the 21st. rlk_canon keeps the
 * same record from the same point, so the same link is named. A link
 * that grows the name forever (a -> a/x, b -> b/../b) never repeats: GNU
 * runs until it is killed or memory gives out; rlk_canon calls the walk a
 * loop after RLK_LINK_CAP links (no real chain is that long).
 *
 * realpath -s (--strip, --no-symlinks) resolves no links: "." and ".."
 * are taken by name. What it checks, measured: before a ".." the name so
 * far must be a directory (stat, following links), and the last named
 * component is checked as -e or -f would check it -- but nothing between,
 * so `-s nosuch/x` is fine and `-s nosuch/..` is not. -L (--logical) is
 * -s's walk, then the result walked again with links followed. The last
 * of -L -P -s decides, measured: `-L -s dang` is .../dang, `-s -L dang`
 * and `-s -P dang` are .../nosuch.
 *
 * --relative-to=DIR prints the result relative to DIR (canonicalized by
 * the same rules; an error in it is fatal, -q or not). --relative-base=
 * DIR prints a name relative only when it is DIR or below it, absolute
 * otherwise; with both, when DIR of --relative-to is not below
 * --relative-base, every name is printed absolute.
 *
 * readlink's messages are silent unless -v (-q and -s are its default);
 * "ignoring --no-newline with multiple arguments" is said regardless.
 * Exit status: 1 if any operand failed.
 *
 * The old ones: readlink took -f/-e as "print the cwd joined to it", and
 * realpath followed nothing. */

#define RLK_EXISTING     0
#define RLK_ALL_BUT_LAST 1
#define RLK_MISSING      2
#define RLK_MAXLEN       (1 << 20)   /* a name that grows past this: ENAMETOOLONG */
#define RLK_LOOP_AFTER   20          /* links followed before loops are looked for */
#define RLK_LINK_CAP     10000       /* links followed before any walk is a loop */

struct rlk_buf { char *s; size_t n, cap; };

static void rlk_reserve(struct rlk_buf *b, size_t extra) {
    if (b->n + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->n + extra + 1) nc *= 2;
    char *ns = (char *)realloc(b->s, nc);
    if (!ns) su_error(1, ENOMEM, "memory exhausted");
    b->s = ns;
    b->cap = nc;
}

static void rlk_set(struct rlk_buf *b, const char *s) {
    b->n = 0;
    size_t l = strlen(s);
    rlk_reserve(b, l);
    memcpy(b->s, s, l + 1);
    b->n = l;
}

/* append "/" (unless the name ends in one) and the component */
static void rlk_push(struct rlk_buf *b, const char *c, size_t len) {
    rlk_reserve(b, len + 1);
    if (b->n == 0 || b->s[b->n - 1] != '/') b->s[b->n++] = '/';
    memcpy(b->s + b->n, c, len);
    b->n += len;
    b->s[b->n] = 0;
}

/* take the last component off; "/" stays "/" */
static void rlk_pop(struct rlk_buf *b) {
    while (b->n > 1 && b->s[b->n - 1] != '/') b->n--;
    if (b->n > 1) b->n--;            /* the slash before it */
    b->s[b->n] = 0;
}

/* only slashes (or nothing) from S on */
static int rlk_only_slashes(const char *s) {
    while (*s == '/') s++;
    return *s == 0;
}

/* READLINK(PATH) into a malloc'd string, whatever its length */
static char *rlk_readlink(const char *path) {
    size_t cap = 256;
    for (;;) {
        char *b = (char *)malloc(cap);
        if (!b) su_error(1, ENOMEM, "memory exhausted");
        ssize_t n = readlink(path, b, cap);
        if (n < 0) { int e = errno; free(b); errno = e; return 0; }
        if ((size_t)n < cap) { b[n] = 0; return b; }
        free(b);
        if (cap >= RLK_MAXLEN) { errno = ENAMETOOLONG; return 0; }
        cap *= 2;
    }
}

/* The start of a walk: the working directory, or "/". */
static int rlk_start(struct rlk_buf *r, const char *name) {
    if (*name == '/') { rlk_set(r, "/"); return 0; }
    size_t cap = 256;
    for (;;) {
        char *b = (char *)malloc(cap);
        if (!b) su_error(1, ENOMEM, "memory exhausted");
        if (getcwd(b, cap)) { rlk_set(r, b); free(b); return 0; }
        int e = errno;
        free(b);
        if (e != ERANGE || cap >= RLK_MAXLEN) { errno = e; return -1; }
        cap *= 2;
    }
}

/* the record of links seen, kept after the 20th: the link's identity and
 * what was left to resolve after it -- each as a length and a 64-bit
 * hash, so a walk that grows its name (a -> a/x) costs no more memory
 * per step than one that does not */
struct rlk_mark { uint64_t hid, hrest; size_t lid, lrest; };
struct rlk_seen { struct rlk_mark *m; size_t n, cap; };

static uint64_t rlk_hash(const char *s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;            /* FNV-1a */
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 0x100000001b3ull; }
    return h;
}

/* 1 when (the link, REST) was seen before; else it is recorded */
static int rlk_seen_add(struct rlk_seen *s, const struct stat *st,
                        const char *where, const char *rest) {
    char head[64];
    /* a filesystem that numbers its files names a link by (dev, ino); one
     * that does not (st_ino 0) by where it was found */
    if (st->st_ino) {
        snprintf(head, sizeof head, "%ju:%ju", (uintmax_t)st->st_dev, (uintmax_t)st->st_ino);
        where = head;
    }
    struct rlk_mark k;
    k.lid = strlen(where);
    k.lrest = strlen(rest);
    k.hid = rlk_hash(where, k.lid);
    k.hrest = rlk_hash(rest, k.lrest);
    for (size_t i = 0; i < s->n; i++) {
        const struct rlk_mark *m = &s->m[i];
        if (m->lid == k.lid && m->lrest == k.lrest && m->hid == k.hid && m->hrest == k.hrest)
            return 1;
    }
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->m = (struct rlk_mark *)realloc(s->m, s->cap * sizeof *s->m);
        if (!s->m) su_error(1, ENOMEM, "memory exhausted");
    }
    s->m[s->n++] = k;
    return 0;
}

static void rlk_seen_free(struct rlk_seen *s) {
    free(s->m);
    s->m = 0;
    s->n = s->cap = 0;
}

/* NAME made absolute, links followed, under MODE. A malloc'd string, or
 * 0 with errno set. */
static char *rlk_physical(const char *name, int mode) {
    if (!*name) { errno = ENOENT; return 0; }
    struct rlk_buf r = { 0, 0, 0 };
    if (rlk_start(&r, name) < 0) return 0;
    char *work = strdup(name);
    if (!work) su_error(1, ENOMEM, "memory exhausted");
    const char *p = work;
    struct rlk_seen seen = { 0, 0, 0 };
    unsigned long links = 0;
    int err = 0;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *q = p + strcspn(p, "/");
        size_t len = (size_t)(q - p);
        if (len == 1 && p[0] == '.') { p = q; continue; }
        if (len == 2 && p[0] == '.' && p[1] == '.') { rlk_pop(&r); p = q; continue; }
        rlk_push(&r, p, len);
        if (r.n > RLK_MAXLEN) { err = ENAMETOOLONG; break; }
        struct stat st;
        if (lstat(r.s, &st) != 0) {
            int e = errno;
            if (mode == RLK_MISSING) { p = q; continue; }
            if (mode == RLK_ALL_BUT_LAST && e == ENOENT && rlk_only_slashes(q)) { p = q; continue; }
            err = e;
            break;
        }
        if (S_ISLNK(st.st_mode)) {
            if (++links > RLK_LINK_CAP ||
                (links > RLK_LOOP_AFTER && rlk_seen_add(&seen, &st, r.s, q))) {
                if (mode == RLK_MISSING) { p = q; continue; }   /* a plain name */
                err = ELOOP;
                break;
            }
            char *t = rlk_readlink(r.s);
            if (!t) {
                if (mode == RLK_MISSING) { p = q; continue; }
                err = errno;
                break;
            }
            size_t lt = strlen(t), lq = strlen(q);
            if (lt + lq > RLK_MAXLEN) { free(t); err = ENAMETOOLONG; break; }
            char *nw = (char *)malloc(lt + lq + 1);
            if (!nw) su_error(1, ENOMEM, "memory exhausted");
            memcpy(nw, t, lt);
            memcpy(nw + lt, q, lq + 1);
            if (t[0] == '/') rlk_set(&r, "/");
            else rlk_pop(&r);                /* the link's own name */
            free(t);
            free(work);
            work = nw;
            p = work;
            continue;
        }
        if (!S_ISDIR(st.st_mode) && *q && mode != RLK_MISSING) { err = ENOTDIR; break; }
        p = q;
    }
    free(work);
    rlk_seen_free(&seen);
    if (err) { free(r.s); errno = err; return 0; }
    return r.s;
}

/* is there a component other than "." and ".." in S */
static int rlk_more_names(const char *s) {
    for (;;) {
        while (*s == '/') s++;
        if (!*s) return 0;
        size_t len = strcspn(s, "/");
        if (!(len == 1 && s[0] == '.') && !(len == 2 && s[0] == '.' && s[1] == '.'))
            return 1;
        s += len;
    }
}

/* -s: NAME made absolute with "." and ".." taken by name, no link
 * followed; the checks the header describes. */
static char *rlk_nolinks(const char *name, int mode) {
    if (!*name) { errno = ENOENT; return 0; }
    struct rlk_buf r = { 0, 0, 0 };
    if (rlk_start(&r, name) < 0) return 0;
    const char *p = name;
    int err = 0;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *q = p + strcspn(p, "/");
        size_t len = (size_t)(q - p);
        if (len == 1 && p[0] == '.') { p = q; continue; }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (mode != RLK_MISSING) {
                struct stat st;
                if (stat(r.s, &st) != 0) { err = errno; break; }
                if (!S_ISDIR(st.st_mode)) { err = ENOTDIR; break; }
            }
            rlk_pop(&r);
            p = q;
            continue;
        }
        rlk_push(&r, p, len);
        if (r.n > RLK_MAXLEN) { err = ENAMETOOLONG; break; }
        if (mode != RLK_MISSING && !rlk_more_names(q)) {
            struct stat st;
            if (stat(r.s, &st) != 0) {
                int e = errno;
                if (!(mode == RLK_ALL_BUT_LAST && e == ENOENT && rlk_only_slashes(q))) {
                    err = e;
                    break;
                }
            } else if (!S_ISDIR(st.st_mode) && *q) { err = ENOTDIR; break; }
        }
        p = q;
    }
    if (err) { free(r.s); errno = err; return 0; }
    return r.s;
}

/* NOLINKS: -s; LOGICAL: -L (the -s walk, then the physical one) */
static char *rlk_canon(const char *name, int mode, int nolinks, int logical) {
    if (!nolinks) return rlk_physical(name, mode);
    char *a = rlk_nolinks(name, mode);
    if (!a || !logical) return a;
    char *b = rlk_physical(a, mode);
    int e = errno;
    free(a);
    errno = e;
    return b;
}

/* ------------------------------------------------------------- readlink */

static int do_readlink(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "canonicalize", SU_NO_ARG, 0, 'f' },
        { "canonicalize-existing", SU_NO_ARG, 0, 'e' },
        { "canonicalize-missing", SU_NO_ARG, 0, 'm' },
        { "no-newline", SU_NO_ARG, 0, 'n' },
        { "quiet", SU_NO_ARG, 0, 'q' },
        { "silent", SU_NO_ARG, 0, 's' },
        { "verbose", SU_NO_ARG, 0, 'v' },
        { "zero", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int mode = -1, nonl = 0, verbose = 0, delim = '\n', c;
    while ((c = su_getopt_long(argc, argv, "efmnqsvz", longs, 0)) != -1) {
        switch (c) {
        case 'e': mode = RLK_EXISTING; break;
        case 'f': mode = RLK_ALL_BUT_LAST; break;
        case 'm': mode = RLK_MISSING; break;
        case 'n': nonl = 1; break;
        case 'q': case 's': verbose = 0; break;
        case 'v': verbose = 1; break;
        case 'z': delim = 0; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... FILE...\n"
                   "Print the target a symbolic link holds -- or, with -f, -e or -m,\n"
                   "the file's absolute name with every link along it followed.\n"
                   "\n"
                   "  -f, --canonicalize            follow every link; each component\n"
                   "                                but the last has to exist\n"
                   "  -e, --canonicalize-existing   follow every link; every component\n"
                   "                                has to exist\n"
                   "  -m, --canonicalize-missing    follow every link; no component\n"
                   "                                has to exist\n"
                   "  -n, --no-newline              leave off the final delimiter\n"
                   "  -q, --quiet\n"
                   "  -s, --silent                  say nothing of failures (the default)\n"
                   "  -v, --verbose                 say why a name failed\n"
                   "  -z, --zero                    end each name with NUL, not newline\n"
                   "      --help        show this help and exit\n"
                   "      --version     show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (su_optind >= argc) {
        su_error(0, 0, "missing operand");
        su_try_help(1);
    }
    if (nonl && argc - su_optind > 1) {
        su_error(0, 0, "ignoring --no-newline with multiple arguments");
        nonl = 0;
    }
    int rc = 0;
    for (int i = su_optind; i < argc; i++) {
        const char *name = argv[i];
        char *out;
        if (mode >= 0) out = rlk_canon(name, mode, 0, 0);
        else {
            /* readlink(2)'s answer, through lstat first: a name that is
             * not a link is EINVAL, as Linux says, whatever the
             * filesystem's own readlink would make of it */
            struct stat st;
            if (lstat(name, &st) != 0) out = 0;
            else if (!S_ISLNK(st.st_mode)) { errno = EINVAL; out = 0; }
            else out = rlk_readlink(name);
        }
        if (!out) {
            if (verbose) su_error(0, errno, "%s", su_quotef(name));
            rc = 1;
            continue;
        }
        fputs(out, stdout);
        if (!nonl) putchar(delim);
        free(out);
    }
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return rc;
}

/* ------------------------------------------------------------- realpath */

/* PATH is DIR or below it */
static int rlk_under(const char *path, const char *dir) {
    if (!strcmp(dir, "/")) return 1;
    size_t l = strlen(dir);
    return strncmp(path, dir, l) == 0 && (path[l] == 0 || path[l] == '/');
}

/* PATH relative to DIR (both absolute and canonical) */
static void rlk_put_relative(const char *path, const char *dir) {
    const char *a = path, *b = dir;
    /* the common leading components */
    const char *ka = a, *kb = b;
    for (;;) {
        while (*a == '/') a++;
        while (*b == '/') b++;
        if (!*a || !*b) {
            if (!*a && !*b) { ka = a; kb = b; }
            break;
        }
        size_t la = strcspn(a, "/"), lb = strcspn(b, "/");
        if (la != lb || memcmp(a, b, la) != 0) break;
        a += la;
        b += lb;
        ka = a;
        kb = b;
    }
    int any = 0;
    for (const char *s = kb;;) {
        while (*s == '/') s++;
        if (!*s) break;
        fputs(any ? "/.." : "..", stdout);
        any = 1;
        s += strcspn(s, "/");
    }
    while (*ka == '/') ka++;
    if (*ka) {
        if (any) putchar('/');
        fputs(ka, stdout);
        any = 1;
    }
    if (!any) putchar('.');
}

static int do_realpath(int argc, char **argv) {
    enum { RELTO_OPT = 0x100, RELBASE_OPT };
    static const struct su_option longs[] = {
        { "canonicalize-existing", SU_NO_ARG, 0, 'e' },
        { "canonicalize-missing", SU_NO_ARG, 0, 'm' },
        { "relative-to", SU_REQ_ARG, 0, RELTO_OPT },
        { "relative-base", SU_REQ_ARG, 0, RELBASE_OPT },
        { "quiet", SU_NO_ARG, 0, 'q' },
        { "strip", SU_NO_ARG, 0, 's' },
        { "no-symlinks", SU_NO_ARG, 0, 's' },
        { "zero", SU_NO_ARG, 0, 'z' },
        { "logical", SU_NO_ARG, 0, 'L' },
        { "physical", SU_NO_ARG, 0, 'P' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int mode = RLK_ALL_BUT_LAST, nolinks = 0, logical = 0, quiet = 0, delim = '\n', c;
    const char *relto = 0, *relbase = 0;
    while ((c = su_getopt_long(argc, argv, "emLPqsz", longs, 0)) != -1) {
        switch (c) {
        case 'e': mode = RLK_EXISTING; break;
        case 'm': mode = RLK_MISSING; break;
        case 'L': nolinks = 1; logical = 1; break;
        case 'P': nolinks = 0; logical = 0; break;
        case 's': nolinks = 1; logical = 0; break;
        case 'q': quiet = 1; break;
        case 'z': delim = 0; break;
        case RELTO_OPT: relto = su_optarg; break;
        case RELBASE_OPT: relbase = su_optarg; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... FILE...\n"
                   "Print each FILE's absolute name, with every link along it followed;\n"
                   "each component but the last has to exist.\n"
                   "\n"
                   "  -e, --canonicalize-existing  every component has to exist\n"
                   "  -m, --canonicalize-missing   no component has to exist, or be a\n"
                   "                               directory\n"
                   "  -L, --logical                take '..' by name first, links after\n"
                   "  -P, --physical               follow links as they come (the default)\n"
                   "  -q, --quiet                  say nothing of most failures\n"
                   "      --relative-to=DIR        print each name relative to DIR\n"
                   "      --relative-base=DIR      print names below DIR relative to it,\n"
                   "                               the others absolute\n"
                   "  -s, --strip, --no-symlinks   follow no links at all\n"
                   "  -z, --zero                   end each name with NUL, not newline\n"
                   "      --help        show this help and exit\n"
                   "      --version     show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (su_optind >= argc) {
        su_error(0, 0, "missing operand");
        su_try_help(1);
    }
    char *to = 0, *base = 0;
    if (relto) {
        to = rlk_canon(relto, mode, nolinks, logical);
        if (!to) su_error(1, errno, "%s", su_quotef(relto));
    }
    if (relbase) {
        base = rlk_canon(relbase, mode, nolinks, logical);
        if (!base) su_error(1, errno, "%s", su_quotef(relbase));
    }
    if (base && !to) to = strdup(base);
    /* --relative-to outside --relative-base: everything absolute */
    if (base && to && !rlk_under(to, base)) {
        free(to);
        free(base);
        to = base = 0;
    }
    int rc = 0;
    for (int i = su_optind; i < argc; i++) {
        char *out = rlk_canon(argv[i], mode, nolinks, logical);
        if (!out) {
            if (!quiet) su_error(0, errno, "%s", su_quotef(argv[i]));
            rc = 1;
            continue;
        }
        if (to && (!base || rlk_under(out, base))) rlk_put_relative(out, to);
        else fputs(out, stdout);
        putchar(delim);
        free(out);
    }
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return rc;
}
