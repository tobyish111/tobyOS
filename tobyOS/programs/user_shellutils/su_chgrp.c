/* su_chgrp.c -- chgrp, as GNU coreutils' (logs/utilgate/chgrp.sh):
 * chgrp [OPTION]... GROUP FILE..., or --reference=RFILE FILE...
 *
 * GROUP is a name in /etc/group, else a decimal number (blanks and a '+'
 * allowed; 0x... is not a number here, and (gid_t)-1 is not a group); ""
 * asks for no change. --reference takes RFILE's group (RFILE followed).
 *
 * Two things decide what is done to a symbolic link, and they are
 * separate, as GNU's are:
 *   - the change itself: chown(2), which follows a link to what it names
 *     (--dereference, the default), or lchown(2), the link itself (-h);
 *   - with -R, the walk: -P (the default) follows no link, -H follows a
 *     link named on the command line, -L follows every link -- into a
 *     directory, too. -R without -H or -L changes links themselves; -R
 *     --dereference without them is refused.
 * A directory is changed after what is in it. --from=[USER][:GROUP]
 * changes only a file owned so (else it is "retained"); --preserve-root
 * refuses -R on '/'.
 *
 * -v says what happened to each file, -c only what changed -- "changed
 * group of 'f' from OLD to GROUP" (GROUP as it was written), "group of
 * 'f' retained as OLD", "failed to change group of 'f' from OLD to GROUP"
 * -- on stdout; -f keeps the errors to itself (the status is still 1).
 * A link that cannot be followed is "cannot dereference", and GNU's
 * report of it says the old group was root (it never read one).
 *
 * The old chgrp followed links nowhere (the kernel's chown changed the
 * link), had no -h -H -L -P --from --preserve-root, and printed group
 * numbers for names. */

static char cg_buf[3][256];

/* the id NAME has in DB (/etc/passwd or /etc/group), or -1 */
static long cg_db_id(const char *db, const char *name) {
    FILE *f = fopen(db, "r");
    if (!f) return -1;
    char line[1024];
    long id = -1;
    size_t nl = strlen(name);
    while (fgets(line, sizeof line, f)) {
        char *c1 = strchr(line, ':');
        if (!c1 || (size_t)(c1 - line) != nl || strncmp(line, name, nl)) continue;
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) continue;
        char *e;
        unsigned long v = strtoul(c2 + 1, &e, 10);
        if (e == c2 + 1 || *e != ':') continue;
        id = (long)v;
        break;
    }
    fclose(f);
    return id;
}

/* the 4th field (a user's login group) of NAME's line in /etc/passwd */
static long cg_login_group(const char *name, long uid) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return -1;
    char line[1024];
    long g = -1;
    while (fgets(line, sizeof line, f)) {
        char *fld[5] = { line, 0, 0, 0, 0 };
        int k = 1;
        for (char *p = line; *p && k < 5; p++) if (*p == ':') { *p = 0; fld[k++] = p + 1; }
        if (k < 5) continue;
        if (name ? strcmp(fld[0], name) : strtol(fld[2], 0, 10) != uid) continue;
        g = strtol(fld[3], 0, 10);
        break;
    }
    fclose(f);
    return g;
}

/* GID's name in /etc/group, or the number, in one of three buffers */
static const char *cg_gname(unsigned long gid, int slot) {
    char *out = cg_buf[slot];
    FILE *f = fopen("/etc/group", "r");
    snprintf(out, sizeof cg_buf[0], "%lu", gid);
    if (!f) return out;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *c1 = strchr(line, ':');
        if (!c1) continue;
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) continue;
        char *e;
        unsigned long v = strtoul(c2 + 1, &e, 10);
        if (e == c2 + 1 || *e != ':' || v != gid) continue;
        *c1 = 0;
        snprintf(out, sizeof cg_buf[0], "%.255s", line);
        break;
    }
    fclose(f);
    return out;
}

/* a decimal id, as xstrtoumax reads one: blanks, '+', digits, nothing
 * after; (id_t)-1 is not an id */
static int cg_number(const char *s, unsigned long *out) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    unsigned long long v = 0;
    for (; isdigit((unsigned char)*p); p++) {
        v = v * 10 + (unsigned)(*p - '0');
        if (v >= 0xFFFFFFFFull) return -1;
    }
    if (*p) return -1;
    *out = (unsigned long)v;
    return 0;
}

/* a group: a name, else a number; -1 if neither */
static int cg_group(const char *s, unsigned long *gid) {
    long id = cg_db_id("/etc/group", s);
    if (id >= 0) { *gid = (unsigned long)id; return 0; }
    return cg_number(s, gid);
}

enum { CG_P, CG_H, CG_L };

struct cg_opts {
    int verbose;        /* 0, 1 -c, 2 -v */
    int force;
    int deref;          /* change the link's referent (chown) or the link */
    int recurse, walk, preserve_root;
    long gid;           /* -1: no change asked */
    const char *spec;   /* the group as the "to" of a report */
    long from_uid, from_gid;
    dev_t root_dev;
    ino_t root_ino;
};

static struct cg_opts cg;

static void cg_report(const char *what, const char *path, const char *old) {
    if (!strcmp(what, "changed"))
        printf("changed group of %s from %s to %s\n", su_quote(path), old, cg.spec);
    else if (!strcmp(what, "retained"))
        printf("group of %s retained as %s\n", su_quote(path), old);
    else if (!strcmp(what, "ownership"))
        printf("ownership of %s retained\n", su_quote(path));
    else if (old)
        printf("failed to change group of %s from %s to %s\n", su_quote(path), old, cg.spec);
    else
        printf("failed to change group of %s to %s\n", su_quote(path), cg.spec);
}

/* change PATH, whose walk found it as ST (lstat, or stat where the walk
 * follows) */
static int cg_change(const char *path, const struct stat *st) {
    struct stat cur = *st;
    if (cg.deref && S_ISLNK(st->st_mode)) {
        if (stat(path, &cur) != 0) {
            int e = errno;
            if (!cg.force) su_error(0, e, "cannot dereference %s", su_quote(path));
            if (cg.verbose == 2) cg_report("failed", path, cg_gname(0, 0));
            return 1;
        }
    }
    const char *old = cg_gname((unsigned long)cur.st_gid, 0);
    if ((cg.from_uid >= 0 && (long)cur.st_uid != cg.from_uid) ||
        (cg.from_gid >= 0 && (long)cur.st_gid != cg.from_gid)) {
        if (cg.verbose == 2) cg_report("retained", path, old);
        return 0;
    }
    if (cg.gid < 0) {
        if (cg.verbose == 2) cg_report("ownership", path, old);
        return 0;
    }
    int r = cg.deref ? chown(path, (uid_t)-1, (gid_t)cg.gid)
                     : lchown(path, (uid_t)-1, (gid_t)cg.gid);
    if (r != 0) {
        int e = errno;
        if (!cg.force) su_error(0, e, "changing group of %s", su_quote(path));
        if (cg.verbose == 2) cg_report("failed", path, old);
        return 1;
    }
    if ((long)cur.st_gid != cg.gid) {
        if (cg.verbose) cg_report("changed", path, old);
    } else if (cg.verbose == 2) {
        cg_report("retained", path, old);
    }
    return 0;
}

static int cg_walk(char *path, size_t cap, int top) {
    struct stat st;
    int follow = cg.recurse && (cg.walk == CG_L || (cg.walk == CG_H && top));
    int r = follow ? stat(path, &st) : lstat(path, &st);
    if (r != 0 && follow && errno == ENOENT) r = lstat(path, &st);   /* dangling */
    if (r != 0) {
        int e = errno;
        if (!cg.force) su_error(0, e, "cannot access %s", su_quote(path));
        if (cg.verbose == 2) cg_report("failed", path, 0);
        return 1;
    }
    int rc = 0;
    if (cg.recurse && S_ISDIR(st.st_mode)) {
        if (top && cg.preserve_root && st.st_dev == cg.root_dev && st.st_ino == cg.root_ino) {
            if (!strcmp(path, "/"))
                su_error(0, 0, "it is dangerous to operate recursively on %s", su_quote(path));
            else
                su_error(0, 0, "it is dangerous to operate recursively on %s (same as %s)",
                         su_quote(path), su_quote("/"));
            su_error(0, 0, "use --no-preserve-root to override this failsafe");
            return 1;
        }
        DIR *d = opendir(path);
        if (!d) {
            int e = errno;
            if (!cg.force) su_error(0, e, "cannot read directory %s", su_quote(path));
            rc = 1;
        } else {
            size_t len = strlen(path);
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                size_t nl = strlen(de->d_name);
                int slash = len > 0 && path[len - 1] != '/';
                if (len + slash + nl + 1 > cap) { rc = 1; continue; }
                if (slash) path[len] = '/';
                memcpy(path + len + slash, de->d_name, nl + 1);
                rc |= cg_walk(path, cap, 0);
                path[len] = 0;
            }
            closedir(d);
        }
    }
    rc |= cg_change(path, &st);
    return rc;
}

/* --from=[USER][:[GROUP]], as chown reads a spec */
static void cg_from(const char *spec) {
    char user[256], grp[256];
    const char *colon = strchr(spec, ':');
    size_t ul = colon ? (size_t)(colon - spec) : strlen(spec);
    if (ul >= sizeof user) ul = sizeof user - 1;
    memcpy(user, spec, ul);
    user[ul] = 0;
    snprintf(grp, sizeof grp, "%s", colon ? colon + 1 : "");
    cg.from_uid = cg.from_gid = -1;
    if (user[0]) {
        long id = cg_db_id("/etc/passwd", user);
        unsigned long n;
        if (id < 0) {
            if (cg_number(user, &n) != 0) su_error(1, 0, "invalid user: %s", su_quote(spec));
            id = (long)n;
        }
        cg.from_uid = id;
        if (colon && !grp[0]) {
            long g = cg_login_group(cg_db_id("/etc/passwd", user) >= 0 ? user : 0, id);
            if (g < 0) su_error(1, 0, "invalid spec: %s", su_quote(spec));
            cg.from_gid = g;
        }
    }
    if (grp[0]) {
        unsigned long g;
        if (cg_group(grp, &g) != 0) su_error(1, 0, "invalid group: %s", su_quote(spec));
        cg.from_gid = (long)g;
    }
}

static int do_chgrp(int argc, char **argv) {
    enum { DEREF_OPT = 0x100, FROM_OPT, NO_PRESERVE_OPT, PRESERVE_OPT, REFERENCE_OPT };
    static const struct su_option longs[] = {
        { "changes", SU_NO_ARG, 0, 'c' },
        { "silent", SU_NO_ARG, 0, 'f' },
        { "quiet", SU_NO_ARG, 0, 'f' },
        { "verbose", SU_NO_ARG, 0, 'v' },
        { "dereference", SU_NO_ARG, 0, DEREF_OPT },
        { "no-dereference", SU_NO_ARG, 0, 'h' },
        { "from", SU_REQ_ARG, 0, FROM_OPT },
        { "no-preserve-root", SU_NO_ARG, 0, NO_PRESERVE_OPT },
        { "preserve-root", SU_NO_ARG, 0, PRESERVE_OPT },
        { "reference", SU_REQ_ARG, 0, REFERENCE_OPT },
        { "recursive", SU_NO_ARG, 0, 'R' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&cg, 0, sizeof cg);
    cg.from_uid = cg.from_gid = -1;
    cg.walk = CG_P;
    int deref = -1, c;
    const char *ref = 0, *from = 0;
    while ((c = su_getopt_long(argc, argv, "HLPRcfhv", longs, 0)) != -1) {
        switch (c) {
        case 'H': cg.walk = CG_H; break;
        case 'L': cg.walk = CG_L; break;
        case 'P': cg.walk = CG_P; break;
        case 'R': cg.recurse = 1; break;
        case 'c': cg.verbose = 1; break;
        case 'v': cg.verbose = 2; break;
        case 'f': cg.force = 1; break;
        case 'h': deref = 0; break;
        case DEREF_OPT: deref = 1; break;
        case FROM_OPT: from = su_optarg; break;
        case NO_PRESERVE_OPT: cg.preserve_root = 0; break;
        case PRESERVE_OPT: cg.preserve_root = 1; break;
        case REFERENCE_OPT: ref = su_optarg; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... GROUP FILE...\n"
                   "  or:  %s [OPTION]... --reference=RFILE FILE...\n"
                   "Set the group of each FILE to GROUP (a name or a number); with\n"
                   "--reference, to RFILE's group.\n"
                   "\n"
                   "  -c, --changes          like -v, but only for a file whose group changed\n"
                   "  -f, --silent, --quiet  leave out most error messages\n"
                   "  -v, --verbose          report on every file\n"
                   "      --dereference      change what a symbolic link names (the default)\n"
                   "  -h, --no-dereference   change a symbolic link itself\n"
                   "      --from=CURRENT_OWNER:CURRENT_GROUP\n"
                   "                         change only a file with this owner and/or group\n"
                   "                         (either may be left out, and then any matches)\n"
                   "      --no-preserve-root  treat '/' as any other directory (the default)\n"
                   "      --preserve-root    refuse to work recursively on '/'\n"
                   "      --reference=RFILE  use RFILE's group (RFILE's link followed)\n"
                   "  -R, --recursive        work on directories and what is in them\n"
                   "\n"
                   "With -R, how symbolic links are walked (the last of these wins):\n"
                   "  -H                     follow a link named on the command line to a\n"
                   "                         directory\n"
                   "  -L                     follow every link to a directory\n"
                   "  -P                     follow no link (the default)\n"
                   "\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n", su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (cg.recurse) {
        if (cg.walk == CG_P) {
            if (deref == 1) su_error(1, 0, "-R --dereference requires either -H or -L");
            deref = 0;
        }
    }
    cg.deref = deref != 0;
    int nop = argc - su_optind;
    if (nop < (ref ? 1 : 2)) {
        if (nop == 0) su_error(0, 0, "missing operand");
        else su_error(0, 0, "missing operand after %s", su_quote(argv[argc - 1]));
        su_try_help(1);
    }
    if (from) cg_from(from);
    if (ref) {
        struct stat rs;
        if (stat(ref, &rs) != 0)
            su_error(1, errno, "failed to get attributes of %s", su_quote(ref));
        cg.gid = (long)rs.st_gid;
        cg.spec = cg_gname((unsigned long)rs.st_gid, 2);
    } else {
        const char *g = argv[su_optind++];
        cg.spec = g;
        if (!*g) cg.gid = -1;
        else {
            unsigned long v;
            if (cg_group(g, &v) != 0) su_error(1, 0, "invalid group: %s", su_quote(g));
            cg.gid = (long)v;
        }
    }
    if (cg.recurse && cg.preserve_root) {
        struct stat r;
        if (lstat("/", &r) != 0) su_error(1, errno, "failed to get attributes of %s", su_quote("/"));
        cg.root_dev = r.st_dev;
        cg.root_ino = r.st_ino;
    }
    int rc = 0;
    for (int i = su_optind; i < argc; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s", argv[i]);
        rc |= cg_walk(path, sizeof path, 1);
    }
    return rc;
}
