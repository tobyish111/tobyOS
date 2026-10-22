/* su_chcon.c -- chcon, as GNU coreutils 9.7's (logs/utilgate/staging/
 * chcon.sh):
 *   chcon [OPTION]... CONTEXT FILE...
 *   chcon [OPTION]... [-u USER] [-r ROLE] [-l RANGE] [-t TYPE] FILE...
 *   chcon [OPTION]... --reference=RFILE FILE...
 *
 * A file's security context is its "security.selinux" extended
 * attribute, and that is all GNU's chcon touches when no SELinux policy
 * is loaded -- measured in WSL, which has none: CONTEXT is written as it
 * is (any text; with its NUL, as libselinux writes it), and nothing
 * checks it. -u -r -t -l edit the context the file has: none is "can't
 * apply partial context to unlabeled file 'F'"; one without three
 * ':'-separated parts is "failed to create security context: 'OLD':
 * Invalid argument"; a USER ROLE or TYPE with a ':' is "failed to set
 * user security context component to 'X': Invalid argument" (a RANGE may
 * have them). The parts are user:role:type[:range]. --reference copies
 * RFILE's ("failed to get security context of 'R': strerror").
 *
 * FILEs that cannot be lstat()ed are "cannot access 'F': strerror"; a set
 * that fails is "failed to change context of 'F' to 'C': strerror". -h
 * changes a symbolic link itself, --dereference (the default) what it
 * names. -R walks each directory, changing what is in it before it, in
 * readdir order; -P (the default) changes links themselves, -H follows
 * those named on the command line, -L every one (the last of -H -L -P
 * counts); "-R --dereference requires either -H or -L", "-R -h requires
 * -P". --preserve-root refuses -R on '/' ("(same as '/')" for another
 * name of it). -v says "changing security context of 'F'" before each.
 * The status is 1 when anything failed.
 *
 * ON TOBYOS the kernel keeps only user.* attributes: it refuses to set a
 * security.* one (ENOTSUP) and has none to give (ENODATA), and libtoby
 * has no xattr calls -- so a set there answers ENOTSUP and a get ENODATA,
 * exactly as the kernel would, once the file is known to exist: "failed
 * to change context of 'f' to 'x': Operation not supported".
 *
 * The old chcon refused everything with "SELinux is not enabled". */

#ifdef SU_HOST
#include <sys/xattr.h>
#endif

#define SY_CCATTR "security.selinux"
#ifndef ENOTSUP
#define ENOTSUP 95          /* Linux's number; libtoby's errno.h has no name for it */
#endif

/* the context of PATH into a malloc'd string, or 0 and errno */
static char *sy_ccget(const char *path, int follow) {
#ifdef SU_HOST
    for (;;) {
        ssize_t n = follow ? getxattr(path, SY_CCATTR, 0, 0) : lgetxattr(path, SY_CCATTR, 0, 0);
        if (n < 0) return 0;
        /* an attribute that is there but empty is no context at all:
         * libselinux answers ENOTSUP */
        if (n == 0) { errno = ENOTSUP; return 0; }
        char *b = (char *)malloc((size_t)n + 1);
        if (!b) su_error(1, ENOMEM, "memory exhausted");
        ssize_t m = follow ? getxattr(path, SY_CCATTR, b, (size_t)n)
                           : lgetxattr(path, SY_CCATTR, b, (size_t)n);
        if (m < 0) { int e = errno; free(b); if (e == ERANGE) continue; errno = e; return 0; }
        b[m] = 0;
        return b;
    }
#else
    struct stat st;
    if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) return 0;
    errno = ENODATA;
    return 0;
#endif
}

static int sy_ccset(const char *path, const char *ctx, int follow) {
#ifdef SU_HOST
    size_t n = strlen(ctx) + 1;
    return follow ? setxattr(path, SY_CCATTR, ctx, n, 0) : lsetxattr(path, SY_CCATTR, ctx, n, 0);
#else
    (void)ctx;
    struct stat st;
    if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) return -1;
    errno = ENOTSUP;
    return -1;
#endif
}

static struct {
    const char *ctx;            /* a whole context, or 0 */
    const char *part[4];        /* user role type range; 0 = unchanged */
    int verbose, recursive, deref;  /* deref: -1 unset, 0 -h, 1 --dereference */
    char walk;                  /* 'P' 'H' 'L' */
    int preserve_root;
    dev_t root_dev;
    ino_t root_ino;
    int ok;
} sy_cc;

static const char *const sy_ccpart[4] = { "user", "role", "type", "range" };

/* the context PATH is to get; 0 (reported) when there is none. ACC is
 * the name the reading errors give: inside a walk GNU's reads go through
 * the directory, and its messages name the entry alone */
static char *sy_ccnew(const char *path, const char *acc, int follow) {
    if (sy_cc.ctx) return strdup(sy_cc.ctx);
    char *old = sy_ccget(path, follow);
    if (!old) {
        if (errno == ENODATA)
            su_error(0, 0, "can't apply partial context to unlabeled file %s", su_quote(acc));
        else
            su_error(0, errno, "failed to get security context of %s", su_quote(acc));
        return 0;
    }
    /* user:role:type[:range], the range everything after the third ':' */
    char *f[4] = { old, 0, 0, 0 };
    int n = 1;
    for (char *p = old; *p && n < 4; p++) if (*p == ':') { *p = 0; f[n++] = p + 1; }
    if (n < 3) {
        /* the context as it was, colons and all */
        for (int i = 1; i < n; i++) f[i][-1] = ':';
        su_error(0, EINVAL, "failed to create security context: %s", su_quote(old));
        free(old);
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        if (!sy_cc.part[i]) continue;
        if (i < 3 && strchr(sy_cc.part[i], ':')) {
            su_error(0, EINVAL, "failed to set %s security context component to %s",
                     sy_ccpart[i], su_quote(sy_cc.part[i]));
            free(old);
            return 0;
        }
        f[i] = (char *)sy_cc.part[i];
    }
    size_t len = 4;
    for (int i = 0; i < 4; i++) if (f[i]) len += strlen(f[i]);
    char *s = (char *)malloc(len);
    if (!s) su_error(1, ENOMEM, "memory exhausted");
    snprintf(s, len, "%s:%s:%s%s%s", f[0], f[1], f[2], f[3] ? ":" : "", f[3] ? f[3] : "");
    free(old);
    return s;
}

static void sy_ccchange(const char *path, const char *acc, int follow) {
    if (sy_cc.verbose) printf("changing security context of %s\n", su_quote(path));
    char *ctx = sy_ccnew(path, acc, follow);
    if (!ctx) { sy_cc.ok = 0; return; }
    if (sy_ccset(path, ctx, follow) != 0) {
        su_error(0, errno, "failed to change context of %s to %s", su_quote(path), su_quote(ctx));
        sy_cc.ok = 0;
    }
    free(ctx);
}

/* PATH (ACC its name in its directory), and with -R what is under it;
 * TOP for a command-line FILE */
static void sy_ccwalk(const char *path, const char *acc, int top) {
    struct stat st;
    if (!sy_cc.recursive) {
        if (lstat(path, &st) != 0) {
            su_error(0, errno, "cannot access %s", su_quote(path));
            sy_cc.ok = 0;
            return;
        }
        sy_ccchange(path, acc, sy_cc.deref != 0);
        return;
    }
    int follow = sy_cc.walk == 'L' || (top && sy_cc.walk == 'H');
    if (lstat(path, &st) != 0) {
        su_error(0, errno, "cannot access %s", su_quote(path));
        sy_cc.ok = 0;
        return;
    }
    if (S_ISLNK(st.st_mode) && follow) {
        struct stat t;
        if (stat(path, &t) == 0) st = t;
    }
    if (S_ISDIR(st.st_mode)) {
        if (sy_cc.preserve_root && st.st_dev == sy_cc.root_dev && st.st_ino == sy_cc.root_ino) {
            if (strcmp(path, "/"))
                su_error(0, 0, "it is dangerous to operate recursively on %s (same as %s)",
                         su_quote(path), su_quote("/"));
            else
                su_error(0, 0, "it is dangerous to operate recursively on %s", su_quote(path));
            su_error(0, 0, "use --no-preserve-root to override this failsafe");
            sy_cc.ok = 0;
            return;
        }
        DIR *d = opendir(path);
        if (!d) {
            su_error(0, errno, "cannot read directory %s", su_quote(path));
            sy_cc.ok = 0;
        } else {
            /* the names first, then the walk: a change cannot reorder it */
            char **names = 0;
            size_t nn = 0, cap = 0;
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                if (nn == cap) {
                    cap = cap ? cap * 2 : 32;
                    names = (char **)realloc(names, cap * sizeof *names);
                    if (!names) su_error(1, ENOMEM, "memory exhausted");
                }
                names[nn++] = strdup(de->d_name);
            }
            closedir(d);
            size_t pl = strlen(path);
            while (pl > 1 && path[pl - 1] == '/') pl--;
            for (size_t i = 0; i < nn; i++) {
                size_t l = pl + strlen(names[i]) + 2;
                char *c = (char *)malloc(l);
                if (!c) su_error(1, ENOMEM, "memory exhausted");
                snprintf(c, l, "%.*s%s%s", (int)pl, path, (pl == 1 && path[0] == '/') ? "" : "/", names[i]);
                sy_ccwalk(c, names[i], 0);
                free(c);
                free(names[i]);
            }
            free(names);
        }
    }
    /* a directory named with trailing slashes keeps one of them */
    size_t l = strlen(path);
    if (top && l > 2 && path[l - 1] == '/' && path[l - 2] == '/') {
        char *p = strdup(path);
        size_t k = l;
        while (k > 1 && p[k - 1] == '/' && p[k - 2] == '/') k--;
        p[k] = 0;
        sy_ccchange(p, acc, follow);
        free(p);
        return;
    }
    sy_ccchange(path, acc, follow);
}

static int do_chcon(int argc, char **argv) {
    enum { DEREF_OPT = 0x100, NOPRES_OPT, PRES_OPT, REF_OPT };
    static const struct su_option longs[] = {
        { "recursive", SU_NO_ARG, 0, 'R' },
        { "verbose", SU_NO_ARG, 0, 'v' },
        { "no-dereference", SU_NO_ARG, 0, 'h' },
        { "no-preserve-root", SU_NO_ARG, 0, NOPRES_OPT },
        { "preserve-root", SU_NO_ARG, 0, PRES_OPT },
        { "dereference", SU_NO_ARG, 0, DEREF_OPT },
        { "reference", SU_REQ_ARG, 0, REF_OPT },
        { "user", SU_REQ_ARG, 0, 'u' },
        { "role", SU_REQ_ARG, 0, 'r' },
        { "type", SU_REQ_ARG, 0, 't' },
        { "range", SU_REQ_ARG, 0, 'l' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&sy_cc, 0, sizeof sy_cc);
    sy_cc.deref = -1;
    sy_cc.walk = 'P';
    sy_cc.ok = 1;
    const char *ref = 0;
    int parts = 0, c;
    while ((c = su_getopt_long(argc, argv, "HLPRhvu:r:t:l:", longs, 0)) != -1) {
        switch (c) {
        case 'H': case 'L': case 'P': sy_cc.walk = (char)c; break;
        case 'R': sy_cc.recursive = 1; break;
        case 'h': sy_cc.deref = 0; break;
        case DEREF_OPT: sy_cc.deref = 1; break;
        case 'v': sy_cc.verbose = 1; break;
        case NOPRES_OPT: sy_cc.preserve_root = 0; break;
        case PRES_OPT: sy_cc.preserve_root = 1; break;
        case REF_OPT: ref = su_optarg; break;
        case 'u': sy_cc.part[0] = su_optarg; parts = 1; break;
        case 'r': sy_cc.part[1] = su_optarg; parts = 1; break;
        case 't': sy_cc.part[2] = su_optarg; parts = 1; break;
        case 'l': sy_cc.part[3] = su_optarg; parts = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... CONTEXT FILE...\n"
                   "  or:  %s [OPTION]... [-u USER] [-r ROLE] [-l RANGE] [-t TYPE] FILE...\n"
                   "  or:  %s [OPTION]... --reference=RFILE FILE...\n"
                   "Set the SELinux security context of each FILE to CONTEXT, or to RFILE's\n"
                   "with --reference, or change only the parts given.\n"
                   "\n"
                   "An argument a long option needs, its short form needs too.\n"
                   "      --dereference      change what a symbolic link names (the default)\n"
                   "  -h, --no-dereference   change symbolic links themselves\n"
                   "  -u, --user=USER        the user part of the new context\n"
                   "  -r, --role=ROLE        the role part\n"
                   "  -t, --type=TYPE        the type part\n"
                   "  -l, --range=RANGE      the range part\n"
                   "      --no-preserve-root  treat '/' like any directory (the default)\n"
                   "      --preserve-root    refuse to work recursively on '/'\n"
                   "      --reference=RFILE  take RFILE's context instead of a CONTEXT\n"
                   "  -R, --recursive        work on directories and what they hold\n"
                   "  -v, --verbose          say which file is being changed, for each\n"
                   "\n"
                   "With -R, these choose which symbolic links are followed (the last wins):\n"
                   "  -H                     a link named on the command line, to a directory\n"
                   "  -L                     every link to a directory\n"
                   "  -P                     none (the default)\n"
                   "\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n",
                   su_name, su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (sy_cc.recursive) {
        if (sy_cc.walk == 'P') {
            if (sy_cc.deref == 1) su_error(1, 0, "-R --dereference requires either -H or -L");
            sy_cc.deref = 0;
        } else {
            if (sy_cc.deref == 0) su_error(1, 0, "-R -h requires -P");
            sy_cc.deref = 1;
        }
    } else if (sy_cc.deref < 0) sy_cc.deref = 1;

    int nop = argc - su_optind;
    if (ref || parts) {
        if (nop < 1) {
            su_error(0, 0, "missing operand");
            su_try_help(1);
        }
        if (ref && parts) {
            su_error(0, 0, "conflicting security context specifiers given");
            su_try_help(1);
        }
    } else {
        if (nop < 1) {
            su_error(0, 0, "missing operand");
            su_try_help(1);
        }
        if (nop < 2) {
            su_error(0, 0, "missing operand after %s", su_quote(argv[su_optind]));
            su_try_help(1);
        }
        sy_cc.ctx = argv[su_optind++];
    }
    if (ref) {
        char *rc = sy_ccget(ref, 1);
        if (!rc) su_error(1, errno, "failed to get security context of %s", su_quote(ref));
        sy_cc.ctx = rc;
        memset(sy_cc.part, 0, sizeof sy_cc.part);
    }
    if (sy_cc.preserve_root) {
        struct stat r;
        if (stat("/", &r) == 0) { sy_cc.root_dev = r.st_dev; sy_cc.root_ino = r.st_ino; }
        else sy_cc.preserve_root = 0;
    }
    for (int i = su_optind; i < argc; i++) sy_ccwalk(argv[i], argv[i], 1);
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return sy_cc.ok ? 0 : 1;
}
