/* su_ln.c -- ln, as GNU coreutils 9.7's (logs/utilgate/staging/ln.sh):
 *
 *   ln [OPTION]... [-T] TARGET LINK_NAME
 *   ln [OPTION]... TARGET                  (a link to it in ".")
 *   ln [OPTION]... TARGET... DIRECTORY
 *   ln [OPTION]... -t DIRECTORY TARGET...
 *
 * Measured on GNU's (gnuln in WSL), case by case in the fixture:
 *   - A hard link is to TARGET itself when it is a symbolic link (-P, the
 *     default) or to what it names (-L); the last of -L and -P counts and
 *     -s ignores both. A TARGET that does not stat is "failed to access",
 *     a directory "T: hard link not allowed for directory" unless -d/-F.
 *   - The last operand is a directory to link into when it is one --
 *     followed, unless -n and it is a symbolic link -- and must be one
 *     with three or more ("target 'X': ERR"). -t DIR: "failed to access
 *     'DIR'", "target 'DIR' is not a directory". A name made in a
 *     directory is DIR/BASE, BASE TARGET's last part.
 *   - An existing LINK_NAME is "failed to create ... 'L': File exists"
 *     unless -f, -i (asks "ln: replace 'L'? ", status 1 for a no) or -b
 *     (renames it to its backup first). Then: a directory there is "L:
 *     cannot overwrite directory"; with -f or -b (not -s -b, nor -i) a
 *     LINK_NAME that is the very directory entry TARGET is -- TARGET's
 *     symbolic links followed where the link would follow them -- is
 *     "'T' and 'L' are the same file"; one made earlier by this ln is "will
 *     not overwrite just-created 'L' with 'T'". -f replaces atomically: a
 *     link made beside it under another name is renamed over it.
 *   - Messages: "failed to create hard link 'L' => 'T': ERR" (just 'L' for
 *     EEXIST), "failed to create symbolic link 'L': ERR" ('L' -> 'T' when
 *     TARGET is empty or ERR is ENAMETOOLONG); -v: "'L' => 'T'", "'L' ->
 *     'T'", after "'BACKUP' ~ " when one was made.
 *   - -r makes a symbolic link's text TARGET relative to LINK_NAME's
 *     directory, both with their symbolic links resolved ("cannot do
 *     --relative without --symbolic" without -s).
 *   - --backup[=CONTROL], -b, -S, VERSION_CONTROL, SIMPLE_BACKUP_SUFFIX as
 *     GNU's: none/off, simple/never, existing/nil, numbered/t; a suffix
 *     with a '/' in it, or empty, is "~".
 *   - "multiple target directories specified", "cannot combine
 *     --target-directory and --no-target-directory".
 *
 * tobyOS: libtoby has no linkat, so -L links what fo_canon resolves the
 * TARGET to, resolved when the link is made (after a backup, as GNU's
 * linkat resolves it). The old ln (programs/user_ln) had -s -f -n -v. */

#include "su_fileops.c"

enum { LN_BK_NONE, LN_BK_SIMPLE, LN_BK_EXISTING, LN_BK_NUMBERED };

static struct {
    int symbolic, relative, force, interactive, hard_dir, logical, no_deref;
    int backup, verbose;
    const char *suffix;
    char **made;            /* LINK_NAMEs made by this run */
    size_t nmade, capmade;
} lnk;

static char *ln_dup(const char *s) {
    char *p = strdup(s);
    if (!p) su_error(1, ENOMEM, "memory exhausted");
    return p;
}

/* ---- names ------------------------------------------------------------------ */

/* the last part of NAME, trailing slashes dropped ("x/" is x, "/" is /) */
static char *ln_base(const char *name) {
    size_t n = strlen(name);
    while (n > 1 && name[n - 1] == '/') n--;
    if (n == 1 && name[0] == '/') return ln_dup("/");
    size_t s = n;
    while (s > 0 && name[s - 1] != '/') s--;
    char *b = (char *)malloc(n - s + 1);
    if (!b) su_error(1, ENOMEM, "memory exhausted");
    memcpy(b, name + s, n - s);
    b[n - s] = 0;
    return b;
}

/* NAME's directory: "." when it has none */
static char *ln_dirname(const char *name) {
    size_t n = strlen(name);
    while (n > 1 && name[n - 1] == '/') n--;
    while (n > 0 && name[n - 1] != '/') n--;
    while (n > 1 && name[n - 1] == '/') n--;
    if (n == 0) return ln_dup(".");
    char *d = (char *)malloc(n + 1);
    if (!d) su_error(1, ENOMEM, "memory exhausted");
    memcpy(d, name, n);
    d[n] = 0;
    return d;
}

/* the name a TARGET gets in a directory: its last part, trailing slashes
 * dropped -- and nothing at all for "/" (GNU then makes the link at the
 * empty name in the directory, which is ENOENT) */
static char *ln_last(const char *name) {
    const char *p = name;
    while (*p == '/') p++;
    if (!*p) return ln_dup("");
    return ln_base(name);
}

/* DIR/BASE, as GNU joins them: DIR's trailing slashes dropped, one slash
 * between (none after a DIR of "/") */
static char *ln_join(const char *dir, const char *base) {
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/') n--;
    int sep = base[0] != '/' && !(n == 1 && dir[0] == '/');
    char *p = (char *)malloc(n + strlen(base) + 2);
    if (!p) su_error(1, ENOMEM, "memory exhausted");
    memcpy(p, dir, n);
    p[n] = 0;
    if (sep) strcat(p, "/");
    strcat(p, base);
    return p;
}

/* PATH absolute, every symbolic link in it resolved, "." and ".." gone;
 * with MISSING, parts that do not exist are kept as written. 0 and errno
 * when it does not resolve. */
static char *ln_canon(const char *path, int missing) {
    char cwd[4096];
    size_t cap = 4096;
    char *res = (char *)malloc(cap);
    char *rest = (char *)malloc(strlen(path) + 2);
    if (!res || !rest) su_error(1, ENOMEM, "memory exhausted");
    strcpy(rest, path);
    if (path[0] == '/') {
        strcpy(res, "/");
    } else {
        if (!getcwd(cwd, sizeof cwd)) { free(res); free(rest); return 0; }
        snprintf(res, cap, "%s", cwd);
    }
    int links = 0, gone = 0;
    char *p = rest;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;
        char *e = p;
        while (*e && *e != '/') e++;
        size_t cl = (size_t)(e - p);
        char comp[4096];
        if (cl >= sizeof comp) { free(res); free(rest); errno = ENAMETOOLONG; return 0; }
        memcpy(comp, p, cl);
        comp[cl] = 0;
        p = e;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) {
            char *s = strrchr(res, '/');
            if (s == res) res[1] = 0;
            else if (s) *s = 0;
            continue;
        }
        size_t rl = strlen(res);
        if (rl + cl + 2 > cap) { free(res); free(rest); errno = ENAMETOOLONG; return 0; }
        char *cand = (char *)malloc(rl + cl + 2);
        if (!cand) su_error(1, ENOMEM, "memory exhausted");
        memcpy(cand, res, rl);
        size_t at = rl;
        if (rl > 1) cand[at++] = '/';
        memcpy(cand + at, comp, cl + 1);
        struct stat st;
        if (gone || lstat(cand, &st) != 0) {
            if (!gone && (errno != ENOENT || !missing)) {
                free(cand); free(res); free(rest);
                return 0;
            }
            gone = 1;
            strcpy(res, cand);
            free(cand);
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            if (++links > 40) { free(cand); free(res); free(rest); errno = ELOOP; return 0; }
            char target[4096];
            ssize_t tl = readlink(cand, target, sizeof target - 1);
            free(cand);
            if (tl < 0) { free(res); free(rest); return 0; }
            target[tl] = 0;
            char *nr = (char *)malloc((size_t)tl + strlen(p) + 2);
            if (!nr) su_error(1, ENOMEM, "memory exhausted");
            sprintf(nr, "%s%s", target, p);
            free(rest);
            rest = nr;
            p = rest;
            if (target[0] == '/') strcpy(res, "/");
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            const char *q = p;
            while (*q == '/') q++;
            if (*q || *p == '/') {
                free(cand); free(res); free(rest);
                errno = ENOTDIR;
                return 0;
            }
        }
        strcpy(res, cand);
        free(cand);
    }
    free(rest);
    return res;
}

/* TARGET as seen from DIR, both absolute and resolved */
static char *ln_relpath(const char *target, const char *dir) {
    const char *t = target, *d = dir;
    /* the common leading parts, whole parts only */
    const char *tc = t, *dc = d;
    for (;;) {
        while (*t == '/') t++;
        while (*d == '/') d++;
        const char *te = t, *de = d;
        while (*te && *te != '/') te++;
        while (*de && *de != '/') de++;
        if (te == t || de == d || te - t != de - d || strncmp(t, d, (size_t)(te - t))) break;
        t = te;
        d = de;
        tc = t;
        dc = d;
    }
    size_t ups = 0;
    for (const char *q = dc; *q;) {
        while (*q == '/') q++;
        if (!*q) break;
        ups++;
        while (*q && *q != '/') q++;
    }
    while (*tc == '/') tc++;
    char *out = (char *)malloc(ups * 3 + strlen(tc) + 2);
    if (!out) su_error(1, ENOMEM, "memory exhausted");
    out[0] = 0;
    for (size_t i = 0; i < ups; i++) strcat(out, i ? "/.." : "..");
    if (*tc) {
        if (ups) strcat(out, "/");
        strcat(out, tc);
    }
    if (!out[0]) strcpy(out, ".");
    return out;
}

/* the directory entry NAME is: its directory resolved, and its last part
 * -- or, with FOLLOW, NAME wholly resolved. 0 when it does not resolve. */
static char *ln_entry(const char *name, int follow) {
    char *base = ln_base(name);
    if (follow || !strcmp(base, ".") || !strcmp(base, "..") || !strcmp(base, "/") || !*base) {
        free(base);
        return ln_canon(name, 0);
    }
    char *dir = ln_dirname(name);
    char *cd = ln_canon(dir, 0);
    free(dir);
    if (!cd) { free(base); return 0; }
    char *r = ln_join(cd, base);
    free(cd);
    free(base);
    return r;
}

/* ---- backups ---------------------------------------------------------------- */

static int ln_backup_type(const char *context, const char *arg) {
    static const char *const names[] = { "none", "off", "simple", "never",
                                         "existing", "nil", "numbered", "t", 0 };
    static const int vals[] = { LN_BK_NONE, LN_BK_NONE, LN_BK_SIMPLE, LN_BK_SIMPLE,
                                LN_BK_EXISTING, LN_BK_EXISTING, LN_BK_NUMBERED,
                                LN_BK_NUMBERED };
    return fo_argmatch(context, arg, names, vals);
}

/* the highest N of DEST.~N~ there is, as a string (any length), or 0 */
static char *ln_max_numbered(const char *dst) {
    char *dir = ln_dirname(dst), *base = ln_base(dst);
    DIR *d = opendir(dir);
    char *best = 0;
    size_t bl = strlen(base);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *n = e->d_name;
            if (strncmp(n, base, bl) || n[bl] != '.' || n[bl + 1] != '~') continue;
            const char *q = n + bl + 2, *s = q;
            if (*s < '1' || *s > '9') continue;
            while (*s >= '0' && *s <= '9') s++;
            if (s[0] != '~' || s[1]) continue;
            size_t len = (size_t)(s - q);
            if (best && (strlen(best) > len || (strlen(best) == len && strncmp(best, q, len) >= 0)))
                continue;
            free(best);
            best = strndup(q, len);
            if (!best) su_error(1, ENOMEM, "memory exhausted");
        }
        closedir(d);
    }
    free(dir);
    free(base);
    return best;
}

static char *ln_backup_name(const char *dst) {
    int type = lnk.backup;
    char *max = 0;
    if (type == LN_BK_NUMBERED || type == LN_BK_EXISTING) {
        max = ln_max_numbered(dst);
        if (type == LN_BK_EXISTING) type = max ? LN_BK_NUMBERED : LN_BK_SIMPLE;
    }
    char *out;
    if (type == LN_BK_SIMPLE) {
        out = (char *)malloc(strlen(dst) + strlen(lnk.suffix) + 1);
        if (!out) su_error(1, ENOMEM, "memory exhausted");
        sprintf(out, "%s%s", dst, lnk.suffix);
    } else {
        const char *m = max ? max : "0";
        size_t l = strlen(m);
        char *num = (char *)malloc(l + 2);
        if (!num) su_error(1, ENOMEM, "memory exhausted");
        num[0] = '0';
        memcpy(num + 1, m, l + 1);
        size_t i = l;
        while (num[i] == '9') num[i--] = '0';
        num[i]++;
        const char *nn = num[0] == '0' ? num + 1 : num;
        out = (char *)malloc(strlen(dst) + strlen(nn) + 4);
        if (!out) su_error(1, ENOMEM, "memory exhausted");
        sprintf(out, "%s.~%s~", dst, nn);
        free(num);
    }
    free(max);
    return out;
}

/* ---- making one link -------------------------------------------------------- */

/* GNU's yesno: a line from standard input, yes when it starts y or Y */
static int ln_yes(void) {
    int c = getchar(), yes = c == 'y' || c == 'Y';
    while (c != EOF && c != '\n') c = getchar();
    return yes;
}

/* link(2), or with -L linkat(AT_SYMLINK_FOLLOW): 0 or -1 with errno */
static int ln_hard(const char *src, const char *dst) {
    if (!lnk.logical) return link(src, dst);
#ifdef SU_HOST
    return linkat(AT_FDCWD, src, AT_FDCWD, dst, AT_SYMLINK_FOLLOW);
#else
    char *r = ln_canon(src, 0);
    if (!r) return -1;
    int rc = link(r, dst);
    int e = errno;
    free(r);
    errno = e;
    return rc;
#endif
}

static int ln_make(const char *src, const char *dst) {
    return lnk.symbolic ? symlink(src, dst) : ln_hard(src, dst);
}

/* make the link at a name beside DST, then rename it over DST */
static int ln_replace(const char *src, const char *dst) {
    char *dir = ln_dirname(dst);
    static unsigned long serial;
    char *tmp = (char *)malloc(strlen(dir) + 40);
    if (!tmp) su_error(1, ENOMEM, "memory exhausted");
    for (int tries = 0;; tries++) {
        sprintf(tmp, "%s/.ln%lu.%lu", dir, (unsigned long)getpid(), serial++);
        if (ln_make(src, tmp) == 0) break;
        if (errno != EEXIST || tries > 100) {
            int e = errno;
            free(tmp);
            free(dir);
            errno = e;
            return -1;
        }
    }
    free(dir);
    if (rename(tmp, dst) != 0) {
        int e = errno;
        unlink(tmp);
        free(tmp);
        errno = e;
        return -1;
    }
    /* two names of one file: rename(2) does nothing and leaves both */
    struct stat st;
    if (lstat(tmp, &st) == 0) unlink(tmp);
    free(tmp);
    return 0;
}

static void ln_failed(const char *src, const char *dst, int e) {
    if (lnk.symbolic) {
        if (e == ENAMETOOLONG || !*src)
            su_error(0, e, "failed to create symbolic link %s -> %s", su_quoteaf(dst), su_quoteaf(src));
        else
            su_error(0, e, "failed to create symbolic link %s", su_quoteaf(dst));
    } else {
        if (e == EEXIST || e == ENOSPC || e == EROFS
#ifdef EDQUOT
            || e == EDQUOT
#endif
           )
            su_error(0, e, "failed to create hard link %s", su_quoteaf(dst));
        else
            su_error(0, e, "failed to create hard link %s => %s", su_quoteaf(dst), su_quoteaf(src));
    }
}

/* SRC linked as DST; NOBASE: DST is a directory's empty name (SRC "/") */
static int ln_one(const char *src, const char *dst, int nobase) {
    struct stat sst, dst_st;
    char *rel = 0;
    if (!lnk.symbolic) {
        if ((lnk.logical ? stat(src, &sst) : lstat(src, &sst)) != 0) {
            su_error(0, errno, "failed to access %s", su_quoteaf(src));
            return 1;
        }
        if (S_ISDIR(sst.st_mode) && !lnk.hard_dir) {
            su_error(0, 0, "%s: hard link not allowed for directory", su_quotef(src));
            return 1;
        }
    }
    if (nobase) {
        ln_failed(src, dst, ENOENT);
        return 1;
    }
    if (lnk.symbolic && lnk.relative) {
        char *t = ln_canon(src, 1);
        char *dd = ln_dirname(dst);
        char *d = ln_canon(dd, 1);
        free(dd);
        if (t && d) rel = ln_relpath(t, d);
        free(t);
        free(d);
    }
    const char *text = rel ? rel : src;

    int exists = lstat(dst, &dst_st) == 0;
    char *bk = 0;
    if (exists && (lnk.force || lnk.interactive || lnk.backup != LN_BK_NONE)) {
        if (S_ISDIR(dst_st.st_mode)) {
            su_error(0, 0, "%s: cannot overwrite directory", su_quotef(dst));
            free(rel);
            return 1;
        }
        int check = lnk.symbolic ? (lnk.force && lnk.backup == LN_BK_NONE)
                                 : (lnk.force || lnk.backup != LN_BK_NONE);
        if (check) {
            char *se = ln_entry(src, lnk.symbolic || lnk.logical);
            char *de = ln_entry(dst, 0);
            int same = se && de && !strcmp(se, de);
            free(se);
            free(de);
            if (same) {
                su_error(0, 0, "%s and %s are the same file", su_quoteaf(src), su_quoteaf(dst));
                free(rel);
                return 1;
            }
        }
        /* a hard link with -f only: -b alone backs the new one up, and a
         * symbolic link is replaced (both measured) */
        if (!lnk.symbolic && lnk.force) {
            for (size_t i = 0; i < lnk.nmade; i++) {
                if (strcmp(lnk.made[i], dst)) continue;
                su_error(0, 0, "will not overwrite just-created %s with %s",
                         su_quoteaf(dst), su_quoteaf(src));
                free(rel);
                return 1;
            }
        }
        if (lnk.interactive) {
            fprintf(stderr, "%s: replace %s? ", su_short, su_quoteaf(dst));
            fflush(stderr);
            if (!ln_yes()) { free(rel); return 1; }
        }
        if (lnk.backup != LN_BK_NONE) {
            bk = ln_backup_name(dst);
            if (rename(dst, bk) != 0) {
                su_error(0, errno, "cannot backup %s", su_quoteaf(dst));
                free(bk);
                free(rel);
                return 1;
            }
            exists = 0;
        }
    }
    int rc = (exists && (lnk.force || lnk.interactive)) ? ln_replace(text, dst)
                                                        : ln_make(text, dst);
    if (rc != 0) {
        int e = errno;
        ln_failed(text, dst, e);
        free(bk);
        free(rel);
        return 1;
    }
    if (lnk.nmade == lnk.capmade) {
        lnk.capmade = lnk.capmade ? lnk.capmade * 2 : 16;
        lnk.made = (char **)realloc(lnk.made, lnk.capmade * sizeof *lnk.made);
        if (!lnk.made) su_error(1, ENOMEM, "memory exhausted");
    }
    lnk.made[lnk.nmade++] = ln_dup(dst);
    if (lnk.verbose) {
        if (bk) printf("%s ~ ", su_quoteaf(bk));
        printf("%s %s %s\n", su_quoteaf(dst), lnk.symbolic ? "->" : "=>", su_quoteaf(text));
    }
    free(bk);
    free(rel);
    return 0;
}

/* is NAME a directory to link into (followed unless -n and it is a link)? */
static int ln_is_dir(const char *name) {
    struct stat st;
    if (lnk.no_deref && lstat(name, &st) == 0 && S_ISLNK(st.st_mode)) return 0;
    return stat(name, &st) == 0 && S_ISDIR(st.st_mode);
}

static int do_ln(int argc, char **argv) {
    fo_stdout_guard();
    enum { BACKUP_OPT = 0x100 };
    /* GNU's order: the ambiguity messages list them so */
    static const struct su_option longs[] = {
        { "backup", SU_OPT_ARG, 0, BACKUP_OPT },
        { "directory", SU_NO_ARG, 0, 'd' },
        { "force", SU_NO_ARG, 0, 'f' },
        { "no-dereference", SU_NO_ARG, 0, 'n' },
        { "interactive", SU_NO_ARG, 0, 'i' },
        { "logical", SU_NO_ARG, 0, 'L' },
        { "physical", SU_NO_ARG, 0, 'P' },
        { "relative", SU_NO_ARG, 0, 'r' },
        { "suffix", SU_REQ_ARG, 0, 'S' },
        { "symbolic", SU_NO_ARG, 0, 's' },
        { "target-directory", SU_REQ_ARG, 0, 't' },
        { "no-target-directory", SU_NO_ARG, 0, 'T' },
        { "verbose", SU_NO_ARG, 0, 'v' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    int want_backup = 0, no_target = 0, c;
    const char *backup_arg = 0, *suffix = 0, *target_dir = 0;
    while ((c = su_getopt_long(argc, argv, "bdfinrst:vFLPS:T", longs, 0)) != -1) {
        switch (c) {
        case BACKUP_OPT: want_backup = 1; backup_arg = su_optarg; break;
        case 'b': want_backup = 1; break;
        case 'd': case 'F': lnk.hard_dir = 1; break;
        case 'f': lnk.force = 1; lnk.interactive = 0; break;
        case 'i': lnk.interactive = 1; lnk.force = 0; break;
        case 'n': lnk.no_deref = 1; break;
        case 'r': lnk.relative = 1; break;
        case 's': lnk.symbolic = 1; break;
        case 'L': lnk.logical = 1; break;
        case 'P': lnk.logical = 0; break;
        case 'S': want_backup = 1; suffix = su_optarg; break;
        case 't':
            if (target_dir) su_error(1, 0, "multiple target directories specified");
            target_dir = su_optarg;
            break;
        case 'T': no_target = 1; break;
        case 'v': lnk.verbose = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [-T] TARGET LINK_NAME\n"
                   "  or:  %s [OPTION]... TARGET\n"
                   "  or:  %s [OPTION]... TARGET... DIRECTORY\n"
                   "  or:  %s [OPTION]... -t DIRECTORY TARGET...\n"
                   "Make LINK_NAME a link to TARGET (the 1st form); a link to TARGET in\n"
                   "the working directory (the 2nd); a link to each TARGET in DIRECTORY\n"
                   "(the 3rd and 4th).  Links are hard links unless --symbolic.  A\n"
                   "LINK_NAME that exists is an error unless one of -f, -i, -b says what\n"
                   "to do with it.  A hard link needs a TARGET that exists; a symbolic\n"
                   "link holds any text, and a relative one is looked up from the link's\n"
                   "own directory.\n"
                   "\n"
                   "An option's argument is required in its short form too.\n"
                   "      --backup[=CONTROL]      back up each LINK_NAME that exists\n"
                   "  -b                          --backup, taking no CONTROL\n"
                   "  -d, -F, --directory         let the superuser try a hard link to a\n"
                   "                                directory (the system will most likely\n"
                   "                                refuse it anyway)\n"
                   "  -f, --force                 replace a LINK_NAME that exists\n"
                   "  -i, --interactive           ask before replacing one\n"
                   "  -L, --logical               link to what a symbolic TARGET names\n"
                   "  -n, --no-dereference        take a LINK_NAME that is a symbolic link to\n"
                   "                                a directory as a plain file\n"
                   "  -P, --physical              hard-link a symbolic TARGET itself\n"
                   "  -r, --relative              with -s, make the link's text relative to\n"
                   "                                the link's directory\n"
                   "  -s, --symbolic              make symbolic links\n"
                   "  -S, --suffix=SUFFIX         the backup suffix, instead of ~\n"
                   "  -t, --target-directory=DIRECTORY  make the links in DIRECTORY\n"
                   "  -T, --no-target-directory   always take LINK_NAME as a plain file\n"
                   "  -v, --verbose               name each link made\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "The backup suffix is '~' unless --suffix or SIMPLE_BACKUP_SUFFIX sets\n"
                   "one.  CONTROL (or VERSION_CONTROL) chooses the kind of backup:\n"
                   "\n"
                   "  none, off       no backups at all, even with --backup\n"
                   "  numbered, t     numbered backups\n"
                   "  existing, nil   numbered where there are numbered ones, else simple\n"
                   "  simple, never   simple backups always\n"
                   "\n"
                   "-s ignores -L and -P.  Otherwise the last of them decides what a hard\n"
                   "link to a symbolic TARGET is; -P when there is neither.\n",
                   su_name, su_name, su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (want_backup) {
        if (backup_arg && *backup_arg) {
            lnk.backup = ln_backup_type("backup type", backup_arg);
        } else {
            const char *env = getenv("VERSION_CONTROL");
            lnk.backup = (env && *env) ? ln_backup_type("$VERSION_CONTROL", env) : LN_BK_EXISTING;
        }
    }
    lnk.suffix = suffix ? suffix : getenv("SIMPLE_BACKUP_SUFFIX");
    if (!lnk.suffix || !*lnk.suffix || strchr(lnk.suffix, '/')) lnk.suffix = "~";
    if (lnk.relative && !lnk.symbolic) su_error(1, 0, "cannot do --relative without --symbolic");
    if (target_dir && no_target)
        su_error(1, 0, "cannot combine --target-directory and --no-target-directory");

    int n = argc - su_optind;
    char **op = argv + su_optind;
    if (n == 0) {
        su_error(0, 0, "missing file operand");
        su_try_help(1);
    }
    if (target_dir) {
        struct stat st;
        if (lnk.no_deref && lstat(target_dir, &st) == 0 && S_ISLNK(st.st_mode))
            su_error(1, ENOTDIR, "target %s", su_quoteaf(target_dir));
        if (stat(target_dir, &st) != 0)
            su_error(1, errno, "failed to access %s", su_quoteaf(target_dir));
        if (!S_ISDIR(st.st_mode))
            su_error(1, 0, "target %s is not a directory", su_quoteaf(target_dir));
    } else if (no_target) {
        if (n < 2) {
            su_error(0, 0, "missing destination file operand after %s", su_quoteaf(op[0]));
            su_try_help(1);
        }
        if (n > 2) {
            su_error(0, 0, "extra operand %s", su_quoteaf(op[2]));
            su_try_help(1);
        }
        return ln_one(op[0], op[1], 0);
    } else if (n == 1) {
        target_dir = ".";
    } else if (n == 2 && !ln_is_dir(op[1])) {
        return ln_one(op[0], op[1], 0);
    } else if (n == 2 && lnk.symbolic && (!*op[0] || strlen(op[0]) >= 4096)) {
        /* GNU makes the link at LINK_NAME itself first, and goes into the
         * directory only when that is EEXIST: a symbolic link to "" (or to
         * a text too long) fails there, so it is named LINK_NAME */
        ln_failed(op[0], op[1], *op[0] ? ENAMETOOLONG : ENOENT);
        return 1;
    } else {
        target_dir = op[--n];
        if (!ln_is_dir(target_dir)) {
            struct stat st;
            int e = (lnk.no_deref && lstat(target_dir, &st) == 0 && S_ISLNK(st.st_mode)) ? ENOTDIR
                  : stat(target_dir, &st) != 0 ? errno : ENOTDIR;
            su_error(1, e, "target %s", su_quoteaf(target_dir));
        }
    }
    int rc = 0;
    for (int i = 0; i < n; i++) {
        char *base = ln_last(op[i]);
        char *dst = ln_join(target_dir, base);
        rc |= ln_one(op[i], dst, !*base);
        free(dst);
        free(base);
    }
    return rc;
}
