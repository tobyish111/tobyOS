/* su_chroot.c -- chroot, as GNU coreutils 9.7's (logs/utilgate/staging/
 * chroot.sh): run COMMAND (or "$SHELL -i", /bin/sh without SHELL) with
 * NEWROOT as its root directory.
 *
 * Measured on GNU's (gnuchroot in WSL, as an unprivileged user, and as
 * WSL's root on NEWROOT / for what only root reaches):
 *   - Every failure of chroot's own is status 125 -- usage errors too;
 *     a COMMAND that cannot be run is 127 when it is not found, else 126
 *     ("failed to run command 'C': ERR").
 *   - "missing operand"; --skip-chdir with a NEWROOT that is not the
 *     current root is "option --skip-chdir only permitted if NEWROOT is
 *     old '/'" -- both with the --help line.
 *   - chroot(2) is asked first: a NEWROOT that does not resolve is its
 *     error, one that is not a directory "Not a directory", one that may
 *     not be searched "Permission denied", and then, for anyone without
 *     CAP_SYS_CHROOT, "Operation not permitted" -- "cannot change root
 *     directory to 'D': ERR". --userspec and --groups are read only after
 *     it, so an unprivileged caller never sees their errors.
 *   - --userspec=USER[:GROUP] (a '.' for the ':' is warned about): a name
 *     from /etc/passwd or a number; "invalid user", "invalid group", "no
 *     group specified for unknown uid: N". A USER without GROUP takes the
 *     user's group and the user's supplementary groups; --groups=G,G,...
 *     sets the supplementary groups ("invalid group 'G'" for each one that
 *     is neither a name nor a number, empty items skipped).
 *   - Then chdir("/") unless --skip-chdir, and the command.
 *
 * tobyOS: libtoby's chroot is the kernel's (ABI_SYS_CHROOT, finding 214),
 * in Linux's order of answers. The supplementary groups go through
 * libtoby's setgroups (ABI_SYS_SETGROUPS). There was no chroot before. */

#include "su_fileops.c"
#ifdef SU_HOST
#include <grp.h>
#include <pwd.h>
#endif

static int cr_chroot(const char *dir) {
    return chroot(dir);
}

static int cr_setgroups(size_t n, const gid_t *list) {
    return setgroups(n, list);
}

/* a decimal id, as GNU reads one: blanks and '+' before it, nothing
 * after, at most MAX (a --userspec id must be below (id_t)-1; a --groups
 * one may be (id_t)-1 itself, which setgroups then refuses) */
static int cr_number(const char *s, unsigned long *out, unsigned long long max) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    unsigned long long v = 0;
    for (; isdigit((unsigned char)*p); p++) {
        v = v * 10 + (unsigned)(*p - '0');
        if (v > max) return -1;
    }
    if (*p) return -1;
    *out = (unsigned long)v;
    return 0;
}

/* NAME's line in DB (/etc/passwd or /etc/group): field 2 (the id) and,
 * for passwd, field 3 (the login group); by id when NAME is 0 */
static int cr_db(const char *db, const char *name, unsigned long id,
                 unsigned long *id_out, unsigned long *gid_out, char *name_out, size_t ncap) {
    FILE *f = fopen(db, "r");
    if (!f) return -1;
    char line[2048];
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        char *fld[5] = { line, 0, 0, 0, 0 };
        int k = 1;
        for (char *p = line; *p && k < 5; p++) if (*p == ':') { *p = 0; fld[k++] = p + 1; }
        if (k < 3) continue;
        unsigned long v = strtoul(fld[2], 0, 10);
        if (name ? strcmp(fld[0], name) != 0 : v != id) continue;
        if (id_out) *id_out = v;
        if (gid_out) *gid_out = k >= 4 ? strtoul(fld[3], 0, 10) : 0;
        if (name_out) snprintf(name_out, ncap, "%s", fld[0]);
        found = 0;
        break;
    }
    fclose(f);
    return found;
}

/* the groups /etc/group lists USER in, after GID: what initgroups sets */
static size_t cr_user_groups(const char *user, gid_t gid, gid_t *list, size_t cap) {
    size_t n = 0;
    list[n++] = gid;
    FILE *f = fopen("/etc/group", "r");
    if (!f) return n;
    char line[4096];
    while (fgets(line, sizeof line, f) && n < cap) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *fld[4] = { line, 0, 0, 0 };
        int k = 1;
        for (char *p = line; *p && k < 4; p++) if (*p == ':') { *p = 0; fld[k++] = p + 1; }
        if (k < 4) continue;
        gid_t g = (gid_t)strtoul(fld[2], 0, 10);
        for (char *m = fld[3]; m && *m;) {
            char *comma = strchr(m, ',');
            if (comma) *comma = 0;
            if (!strcmp(m, user)) {
                size_t j;
                for (j = 0; j < n && list[j] != g; j++) {}
                if (j == n) list[n++] = g;
                break;
            }
            m = comma ? comma + 1 : 0;
        }
    }
    fclose(f);
    return n;
}

static int do_chroot(int argc, char **argv) {
    fo_exit_failure = 125;
    fo_stdout_guard();
    enum { GROUPS_OPT = 0x100, USERSPEC_OPT, SKIP_CHDIR_OPT };
    static const struct su_option longs[] = {
        { "groups", SU_REQ_ARG, 0, GROUPS_OPT },
        { "userspec", SU_REQ_ARG, 0, USERSPEC_OPT },
        { "skip-chdir", SU_NO_ARG, 0, SKIP_CHDIR_OPT },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    const char *userspec = 0, *groups = 0;
    int skip_chdir = 0, c;
    while ((c = su_getopt_long(argc, argv, "+", longs, 0)) != -1) {
        switch (c) {
        case GROUPS_OPT: groups = su_optarg; break;
        case USERSPEC_OPT: userspec = su_optarg; break;
        case SKIP_CHDIR_OPT: skip_chdir = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION] NEWROOT [COMMAND [ARG]...]\n"
                   "  or:  %s OPTION\n"
                   "Run COMMAND with NEWROOT as its root directory.\n"
                   "\n"
                   "      --groups=G_LIST        the supplementary groups, as g1,g2,..,gN\n"
                   "      --userspec=USER:GROUP  the user and group (a name or a number)\n"
                   "                               to run as\n"
                   "      --skip-chdir           stay in the working directory instead of\n"
                   "                               going to '/' (NEWROOT must be '/')\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "With no COMMAND, '\"$SHELL\" -i' is run ('/bin/sh -i' when SHELL is\n"
                   "not set).\n"
                   "\n"
                   "Exit status:\n"
                   "  125  chroot itself failed\n"
                   "  126  COMMAND was found but could not be run\n"
                   "  127  COMMAND was not found\n"
                   "  -    otherwise, COMMAND's own status\n",
                   su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(125);
        }
    }
    if (su_optind == argc) {
        su_error(0, 0, "missing operand");
        su_try_help(125);
    }
    const char *newroot = argv[su_optind];
    struct stat nst, rst;
    int is_oldroot = stat(newroot, &nst) == 0 && stat("/", &rst) == 0 &&
                     nst.st_dev == rst.st_dev && nst.st_ino == rst.st_ino;
    if (skip_chdir && !is_oldroot) {
        su_error(0, 0, "option --skip-chdir only permitted if NEWROOT is old %s", su_quote("/"));
        su_try_help(125);
    }
    if (cr_chroot(newroot) != 0)
        su_error(125, errno, "cannot change root directory to %s", su_quoteaf(newroot));
    if (!skip_chdir && chdir("/") != 0)
        su_error(125, errno, "cannot chdir to root directory");

    /* --userspec: USER[:GROUP], '.' accepted for ':' with a warning */
    int have_uid = 0, have_gid = 0;
    unsigned long uid = 0, gid = 0;
    char uname[256] = "";
    if (userspec && *userspec) {
        char *spec = strdup(userspec);
        if (!spec) su_error(125, ENOMEM, "memory exhausted");
        /* one trailing ':' goes first, as GNU's: "toby::" is "toby:" (the
         * user's own group), "toby:x:" is "toby:x" */
        size_t sl = strlen(spec);
        if (sl && spec[sl - 1] == ':') spec[sl - 1] = 0;
        char *sep = strchr(spec, ':');
        if (!sep && (sep = strchr(spec, '.')) != 0) {
            /* a name with a dot in it is a name; only then is '.' a separator */
            if (cr_db("/etc/passwd", spec, 0, 0, 0, 0, 0) == 0) sep = 0;
            else su_error(0, 0, "warning: '.' should be ':'");
        }
        char *u = spec, *g = 0;
        if (sep) { *sep = 0; g = sep + 1; }
        if (*u) {
            unsigned long login_gid = 0;
            if (cr_db("/etc/passwd", u, 0, &uid, &login_gid, uname, sizeof uname) == 0) {
                have_uid = 1;
                if (!g || !*g) { gid = login_gid; have_gid = 1; }
            } else if (cr_number(u, &uid, 0xfffffffeULL) == 0) {
                /* a number with an empty GROUP after a ':' */
                if (g && !*g) su_error(125, 0, "invalid spec");
                have_uid = 1;
                if (cr_db("/etc/passwd", 0, uid, 0, &login_gid, uname, sizeof uname) == 0) {
                    if (!g || !*g) { gid = login_gid; have_gid = 1; }
                } else {
                    uname[0] = 0;
                    if (!g || !*g)
                        su_error(125, 0, "no group specified for unknown uid: %lu", uid);
                }
            } else {
                su_error(125, 0, "invalid user");
            }
        }
        if (g && *g) {
            if (cr_db("/etc/group", g, 0, &gid, 0, 0, 0) == 0) have_gid = 1;
            else if (cr_number(g, &gid, 0xfffffffeULL) == 0) have_gid = 1;
            else su_error(125, 0, "invalid group");
        }
        free(spec);
    }

    /* --groups, then the groups a user gets */
    gid_t list[256];
    size_t nlist = 0;
    int set_list = 0;
    if (groups) {
        char *gs = strdup(groups);
        if (!gs) su_error(125, ENOMEM, "memory exhausted");
        int bad = 0;
        for (char *t = gs; t;) {
            char *comma = strchr(t, ',');
            if (comma) *comma = 0;
            if (*t) {
                unsigned long g;
                if (cr_db("/etc/group", t, 0, &g, 0, 0, 0) != 0 && cr_number(t, &g, 0xffffffffULL) != 0) {
                    su_error(0, 0, "invalid group %s", su_quote(t));
                    bad = 1;
                } else if (nlist < sizeof list / sizeof list[0]) {
                    list[nlist++] = (gid_t)g;
                }
            }
            t = comma ? comma + 1 : 0;
        }
        free(gs);
        if (bad) exit(125);
        set_list = 1;
    } else if (have_uid) {
        nlist = uname[0] ? cr_user_groups(uname, (gid_t)gid, list, sizeof list / sizeof list[0])
                         : (list[0] = (gid_t)gid, 1);
        set_list = 1;
    }
    if (set_list && cr_setgroups(nlist, list) != 0)
        su_error(125, errno, "failed to set supplemental groups");
    if (have_gid && setgid((gid_t)gid) != 0)
        su_error(125, errno, "failed to set group-ID");
    if (have_uid && setuid((uid_t)uid) != 0)
        su_error(125, errno, "failed to set user-ID");

    char **cmd = argv + su_optind + 1;
    static char dash_i[] = "-i";
    char *shell_argv[3];
    if (su_optind + 1 >= argc) {
        char *shell = getenv("SHELL");
        static char bin_sh[] = "/bin/sh";
        shell_argv[0] = shell ? shell : bin_sh;
        shell_argv[1] = dash_i;
        shell_argv[2] = 0;
        cmd = shell_argv;
    }
    fflush(stdout);
    execvp(cmd[0], cmd);
    int e = errno;
    su_error(0, e, "failed to run command %s", su_quote(cmd[0]));
    return e == ENOENT ? 127 : 126;
}
