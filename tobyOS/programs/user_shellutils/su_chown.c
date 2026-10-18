/* su_chown.c -- chown, as GNU coreutils 9.7's (logs/utilgate/chown.sh):
 * chown [OPTION]... [OWNER][:[GROUP]] FILE..., or --reference=RFILE FILE...
 *
 * Everything chgrp does carries over -- the walk (-R with -H -L -P, -h,
 * --dereference), --from, --reference, --preserve-root, -c -f -v -- and
 * this file leans on su_chgrp.c for the user and group databases
 * (cg_db_id, cg_number, cg_gname, cg_login_group) and on su_whoami.c for
 * a uid's name (su_user_name); the walk is its own, because the change
 * and its reports are chown's. What was measured, in the C locale:
 *
 * THE SPEC. Split at the first ':' (so "u:g:x" asks for the group "g:x").
 * A user is a name in /etc/passwd, else a decimal number (blanks and a '+'
 * before it allowed, nothing after; (uid_t)-1 is not one). "USER:" means
 * USER's login group -- but only for a NAME: "1000:" and "nosuch:" are
 * "invalid spec". ":GROUP" changes the group alone; "" and ":" change
 * nothing (each file is still looked at). A spec with no ':' that does
 * not parse whole is tried again split at its first '.', the old
 * separator; if that parses it is used, with "warning: '.' should be
 * ':': 'SPEC'", and if not, the first error stands ("toby.nosuchgroup" is
 * an invalid USER). Every message quotes the whole spec.
 *
 * THE REPORTS (-v every file, -c only a change; stdout):
 *   changed ownership of 'f' from OLD to NEW
 *   ownership of 'f' retained as OLD
 *   failed to change ownership of 'f' from OLD to NEW   (the change failed)
 *   failed to change ownership of 'f' to NEW            (f was never read)
 * NEW's words are GNU's choice, measured case by case: a user or group
 * given by name is that name; a user missing or given by NUMBER beside a
 * NAMED group is nothing -- "1000:users" and ":users" both report "to
 * :users"; anything still unnamed is its number, reformatted ("+1000" is
 * "1000"), so "1000:100" is "1000:100" and "toby:100" is "toby:100"; a
 * login group is its name. NEW is "U:G" with a group, else "U"; OLD is
 * the file's "user:group" with a group, else "user", by name (or number).
 * With nothing asked for: "ownership of 'f' retained" and "failed to
 * change ownership of 'f'". And with no user and a group by NUMBER the
 * reports are chgrp's -- "changed group of 'f' from OLDGROUP to 100",
 * "group of 'f' retained as OLDGROUP", "failed to change group of ...".
 * --reference shows RFILE's owner and group both, by name or number. A link that
 * cannot be followed is "cannot dereference 'l'", and its report's old
 * owner is root (uid and gid 0 -- it was never read).
 *
 * THE ERRORS (stderr, status 1, the walk goes on): "changing ownership of
 * 'f'" when a user was asked for, else "changing group of 'f'", with the
 * errno's words; "cannot access", "cannot read directory" (and then the
 * directory itself is left alone, as GNU's fts walk leaves it),
 * "cannot dereference". -f keeps them to itself; the status stays 1.
 * --from is read as it is met among the options, so its error comes
 * before a missing operand.
 *
 * A directory is changed after what is in it; a directory met again on
 * its own path (an -L loop) is changed, not entered.
 *
 * The old chown took numbers only, said "chown: f: strerror", and had no
 * options at all. */

struct cho_id {
    long uid, gid;          /* -1: not asked for */
    char uname[256];        /* the new user as reports show it */
    char gname[256];        /* the new group as reports show it */
    int unamed, gnamed;     /* the user, the group, given by a name */
};

enum { CHO_P, CHO_H, CHO_L };

static struct {
    int verbose;            /* 0, 1 -c, 2 -v */
    int force;
    int deref;              /* change what a link names (chown) or the link */
    int recurse, walk, preserve_root;
    struct cho_id to;
    long from_uid, from_gid;
    dev_t root_dev;
    ino_t root_ino;
} cho;

/* UID's name, or the number, into OUT */
static const char *cho_uname(unsigned long uid, char *out, size_t cap) {
    if (su_user_name((uid_t)uid, out, cap) != 0) snprintf(out, cap, "%lu", uid);
    return out;
}

/* SPEC as user SPEC[0..ULEN) and group G (NULL: no separator); NULL, or
 * the kind of error */
static const char *cho_parse_sep(const char *spec, size_t ulen, const char *g,
                                 struct cho_id *o) {
    char user[256];
    unsigned long n;
    o->uid = o->gid = -1;
    o->uname[0] = o->gname[0] = 0;
    o->unamed = o->gnamed = 0;
    if (ulen >= sizeof user) return "invalid user";
    memcpy(user, spec, ulen);
    user[ulen] = 0;
    if (user[0]) {
        long id = cg_db_id("/etc/passwd", user);
        if (id >= 0) {
            o->uid = id;
            o->unamed = 1;
            snprintf(o->uname, sizeof o->uname, "%s", user);
            if (g && !*g) {
                long lg = cg_login_group(user, -1);
                if (lg < 0) return "invalid spec";
                o->gid = lg;
                snprintf(o->gname, sizeof o->gname, "%s", cg_gname((unsigned long)lg, 0));
                o->gnamed = cg_db_id("/etc/group", o->gname) == lg;
            }
        } else {
            /* the login group is a property of a NAMED user only */
            if (g && !*g) return "invalid spec";
            if (cg_number(user, &n) != 0) return "invalid user";
            o->uid = (long)n;
            snprintf(o->uname, sizeof o->uname, "%lu", n);
        }
    }
    if (g && *g) {
        long id = cg_db_id("/etc/group", g);
        if (id >= 0) {
            o->gid = id;
            o->gnamed = 1;
            snprintf(o->gname, sizeof o->gname, "%s", g);
        } else {
            if (cg_number(g, &n) != 0) return "invalid group";
            o->gid = (long)n;
            snprintf(o->gname, sizeof o->gname, "%lu", n);
        }
    }
    return 0;
}

/* SPEC into O, or exit 1 with GNU's message */
static void cho_parse(const char *spec, struct cho_id *o) {
    const char *colon = strchr(spec, ':');
    const char *err = colon ? cho_parse_sep(spec, (size_t)(colon - spec), colon + 1, o)
                            : cho_parse_sep(spec, strlen(spec), 0, o);
    if (err && !colon) {
        const char *dot = strchr(spec, '.');
        struct cho_id t;
        if (dot && !cho_parse_sep(spec, (size_t)(dot - spec), dot + 1, &t)) {
            *o = t;
            su_error(0, 0, "warning: '.' should be ':': %s", su_quote(spec));
            err = 0;
        }
    }
    if (err) su_error(1, 0, "%s: %s", err, su_quote(spec));
}

/* The words a report gives the new owner, as GNU picks them: a user or
 * group given by NAME is that name; then, a user missing (or given by
 * number) beside a NAMED group is "" -- "to :users", even for
 * "1000:users"; then a number stands for whatever is still unnamed. A
 * report whose user is still nothing (only a group, by number) is
 * chgrp's ("changed group of ..."). */
static const char *cho_ustr, *cho_gstr;
static char cho_unum[32], cho_gnum[32];

static void cho_words(void) {
    cho_ustr = cho.to.unamed ? cho.to.uname : 0;
    cho_gstr = cho.to.gnamed ? cho.to.gname : 0;
    if (!cho_ustr && cho_gstr) cho_ustr = "";
    if (!cho_ustr && cho.to.uid >= 0) {
        snprintf(cho_unum, sizeof cho_unum, "%ld", cho.to.uid);
        cho_ustr = cho_unum;
    }
    if (!cho_gstr && cho.to.gid >= 0) {
        snprintf(cho_gnum, sizeof cho_gnum, "%ld", cho.to.gid);
        cho_gstr = cho_gnum;
    }
}

/* OLD as reports show it: "user", or "user:group" when a group is named */
static const char *cho_old(char *out, size_t cap, unsigned long uid, unsigned long gid) {
    char u[256];
    cho_uname(uid, u, sizeof u);
    if (cho_gstr) snprintf(out, cap, "%s:%s", u, cg_gname(gid, 1));
    else snprintf(out, cap, "%s", u);
    return out;
}

static const char *cho_new(char *out, size_t cap) {
    if (cho_gstr) snprintf(out, cap, "%s:%s", cho_ustr, cho_gstr);
    else snprintf(out, cap, "%s", cho_ustr);
    return out;
}

/* WHAT is 'c' changed, 'r' retained, 'f' failed; HAVE says the file's
 * ids UID/GID were read */
static void cho_report(int what, const char *path, int have, unsigned long uid,
                       unsigned long gid) {
    char o[600], n[600];
    const char *q = su_quote(path);
    if (!cho_ustr && !cho_gstr) {
        if (what == 'f') printf("failed to change ownership of %s\n", q);
        else printf("ownership of %s retained\n", q);
        return;
    }
    if (!cho_ustr) {
        const char *og = cg_gname(gid, 1);
        if (what == 'c') printf("changed group of %s from %s to %s\n", q, og, cho_gstr);
        else if (what == 'r') printf("group of %s retained as %s\n", q, og);
        else if (have) printf("failed to change group of %s from %s to %s\n", q, og, cho_gstr);
        else printf("failed to change group of %s to %s\n", q, cho_gstr);
        return;
    }
    if (what == 'c')
        printf("changed ownership of %s from %s to %s\n", q,
               cho_old(o, sizeof o, uid, gid), cho_new(n, sizeof n));
    else if (what == 'r')
        printf("ownership of %s retained as %s\n", q, cho_old(o, sizeof o, uid, gid));
    else if (have)
        printf("failed to change ownership of %s from %s to %s\n", q,
               cho_old(o, sizeof o, uid, gid), cho_new(n, sizeof n));
    else
        printf("failed to change ownership of %s to %s\n", q, cho_new(n, sizeof n));
}

/* change PATH, whose walk found it as ST (lstat, or stat where the walk
 * follows) */
static int cho_change(const char *path, const struct stat *st) {
    struct stat cur = *st;
    if (cho.deref && S_ISLNK(st->st_mode)) {
        if (stat(path, &cur) != 0) {
            int e = errno;
            if (!cho.force) su_error(0, e, "cannot dereference %s", su_quote(path));
            if (cho.verbose == 2) cho_report('f', path, 1, 0, 0);
            return 1;
        }
    }
    unsigned long ou = (unsigned long)cur.st_uid, og = (unsigned long)cur.st_gid;
    if ((cho.from_uid >= 0 && (long)ou != cho.from_uid) ||
        (cho.from_gid >= 0 && (long)og != cho.from_gid) ||
        (cho.to.uid < 0 && cho.to.gid < 0)) {
        if (cho.verbose == 2) cho_report('r', path, 1, ou, og);
        return 0;
    }
    uid_t nu = cho.to.uid >= 0 ? (uid_t)cho.to.uid : (uid_t)-1;
    gid_t ng = cho.to.gid >= 0 ? (gid_t)cho.to.gid : (gid_t)-1;
    int r = cho.deref ? chown(path, nu, ng) : lchown(path, nu, ng);
    if (r != 0) {
        int e = errno;
        if (!cho.force)
            su_error(0, e, cho.to.uid >= 0 ? "changing ownership of %s" : "changing group of %s",
                     su_quote(path));
        if (cho.verbose == 2) cho_report('f', path, 1, ou, og);
        return 1;
    }
    if ((cho.to.uid >= 0 && (long)ou != cho.to.uid) || (cho.to.gid >= 0 && (long)og != cho.to.gid)) {
        if (cho.verbose) cho_report('c', path, 1, ou, og);
    } else if (cho.verbose == 2) {
        cho_report('r', path, 1, ou, og);
    }
    return 0;
}

/* the directories the walk is inside, to stop an -L loop */
struct cho_anc { dev_t dev; ino_t ino; const struct cho_anc *up; };

static int cho_walk(char *path, size_t cap, int top, const struct cho_anc *up) {
    struct stat st;
    int follow = cho.recurse && (cho.walk == CHO_L || (cho.walk == CHO_H && top));
    int r = follow ? stat(path, &st) : lstat(path, &st);
    if (r != 0 && follow && errno == ENOENT) r = lstat(path, &st);   /* dangling */
    if (r != 0) {
        int e = errno;
        if (!cho.force) su_error(0, e, "cannot access %s", su_quote(path));
        if (cho.verbose == 2) cho_report('f', path, 0, 0, 0);
        return 1;
    }
    int rc = 0;
    if (cho.recurse && S_ISDIR(st.st_mode)) {
        if (top && cho.preserve_root && st.st_dev == cho.root_dev && st.st_ino == cho.root_ino) {
            if (!strcmp(path, "/"))
                su_error(0, 0, "it is dangerous to operate recursively on %s", su_quote(path));
            else
                su_error(0, 0, "it is dangerous to operate recursively on %s (same as %s)",
                         su_quote(path), su_quote("/"));
            su_error(0, 0, "use --no-preserve-root to override this failsafe");
            return 1;
        }
        int loop = 0;
        for (const struct cho_anc *a = up; a; a = a->up)
            if (a->dev == st.st_dev && a->ino == st.st_ino) loop = 1;
        if (!loop) {
            DIR *d = opendir(path);
            if (!d) {
                /* GNU reads the directory before changing it, and a
                 * directory it cannot read is left as it is */
                int e = errno;
                if (!cho.force) su_error(0, e, "cannot read directory %s", su_quote(path));
                if (cho.verbose == 2) cho_report('f', path, 0, 0, 0);
                return 1;
            }
            struct cho_anc me = { st.st_dev, st.st_ino, up };
            size_t len = strlen(path);
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                size_t nl = strlen(de->d_name);
                int slash = len > 0 && path[len - 1] != '/';
                if (len + (size_t)slash + nl + 1 > cap) { rc = 1; continue; }
                if (slash) path[len] = '/';
                memcpy(path + len + slash, de->d_name, nl + 1);
                rc |= cho_walk(path, cap, 0, &me);
                path[len] = 0;
            }
            closedir(d);
        }
    }
    rc |= cho_change(path, &st);
    return rc;
}

static int do_chown(int argc, char **argv) {
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
        { "recursive", SU_NO_ARG, 0, 'R' },
        { "reference", SU_REQ_ARG, 0, REFERENCE_OPT },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&cho, 0, sizeof cho);
    cho.from_uid = cho.from_gid = -1;
    cho.walk = CHO_P;
    int deref = -1, c;
    const char *ref = 0;
    while ((c = su_getopt_long(argc, argv, "HLPRcfhv", longs, 0)) != -1) {
        switch (c) {
        case 'H': cho.walk = CHO_H; break;
        case 'L': cho.walk = CHO_L; break;
        case 'P': cho.walk = CHO_P; break;
        case 'R': cho.recurse = 1; break;
        case 'c': cho.verbose = 1; break;
        case 'v': cho.verbose = 2; break;
        case 'f': cho.force = 1; break;
        case 'h': deref = 0; break;
        case DEREF_OPT: deref = 1; break;
        case FROM_OPT: {
            /* read as it is met: its error comes before any other */
            struct cho_id f;
            cho_parse(su_optarg, &f);
            cho.from_uid = f.uid;
            cho.from_gid = f.gid;
            break;
        }
        case NO_PRESERVE_OPT: cho.preserve_root = 0; break;
        case PRESERVE_OPT: cho.preserve_root = 1; break;
        case REFERENCE_OPT: ref = su_optarg; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [OWNER][:[GROUP]] FILE...\n"
                   "  or:  %s [OPTION]... --reference=RFILE FILE...\n"
                   "Give each FILE the owner OWNER and/or the group GROUP; with\n"
                   "--reference, RFILE's owner and group.\n"
                   "\n"
                   "  -c, --changes          like -v, but only for a file whose owner changed\n"
                   "  -f, --silent, --quiet  leave out most error messages\n"
                   "  -v, --verbose          report on every file\n"
                   "      --dereference      change what a symbolic link names (the default)\n"
                   "  -h, --no-dereference   change a symbolic link itself\n"
                   "      --from=CURRENT_OWNER:CURRENT_GROUP\n"
                   "                         change only a file with this owner and/or group\n"
                   "                         (either may be left out, and then any matches)\n"
                   "      --no-preserve-root  treat '/' as any other directory (the default)\n"
                   "      --preserve-root    refuse to work recursively on '/'\n"
                   "      --reference=RFILE  use RFILE's owner and group (RFILE's link followed)\n"
                   "  -R, --recursive        work on directories and what is in them\n"
                   "\n"
                   "With -R, how symbolic links are walked (the last of these wins):\n"
                   "  -H                     follow a link named on the command line to a\n"
                   "                         directory\n"
                   "  -L                     follow every link to a directory\n"
                   "  -P                     follow no link (the default)\n"
                   "\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "A missing OWNER or GROUP is left as it is; OWNER followed by ':' and no\n"
                   "GROUP means OWNER's login group. Either may be a name or a number.\n",
                   su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (cho.recurse) {
        if (cho.walk == CHO_P) {
            if (deref == 1) su_error(1, 0, "-R --dereference requires either -H or -L");
            deref = 0;
        }
    }
    cho.deref = deref != 0;
    int nop = argc - su_optind;
    if (nop < (ref ? 1 : 2)) {
        if (nop == 0) su_error(0, 0, "missing operand");
        else su_error(0, 0, "missing operand after %s", su_quote(argv[argc - 1]));
        su_try_help(1);
    }
    if (ref) {
        struct stat rs;
        if (stat(ref, &rs) != 0)
            su_error(1, errno, "failed to get attributes of %s", su_quote(ref));
        /* RFILE's owner and group by name, or by number where it has no
         * name -- both always shown: an unnamed owner beside a named
         * group is "12345:users" here, not the spec's ":users" */
        cho.to.uid = (long)rs.st_uid;
        cho.to.gid = (long)rs.st_gid;
        cho_uname((unsigned long)rs.st_uid, cho.to.uname, sizeof cho.to.uname);
        snprintf(cho.to.gname, sizeof cho.to.gname, "%s", cg_gname((unsigned long)rs.st_gid, 2));
        cho.to.unamed = cho.to.gnamed = 1;
    } else {
        cho_parse(argv[su_optind++], &cho.to);
    }
    cho_words();
    if (cho.recurse && cho.preserve_root) {
        struct stat r;
        if (lstat("/", &r) != 0) su_error(1, errno, "failed to get attributes of %s", su_quote("/"));
        cho.root_dev = r.st_dev;
        cho.root_ino = r.st_ino;
    }
    int rc = 0;
    for (int i = su_optind; i < argc; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s", argv[i]);
        rc |= cho_walk(path, sizeof path, 1, 0);
    }
    return rc;
}
