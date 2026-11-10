/* su_rmdir.c -- rmdir, as GNU coreutils': remove each empty DIRECTORY.
 *
 *   -p, --parents   then each of its parents, last component first,
 *                   until one will not go ("failed to remove directory")
 *   -v, --verbose   "rmdir: removing directory, 'DIR'" on stdout, before
 *   --ignore-fail-on-non-empty
 *                   say nothing of a directory that is not empty
 *
 * A name ending in '/' whose last component is a symbolic link to a
 * directory: "Symbolic link not followed", as GNU says. Status 1 if any
 * failed. Measured on coreutils 9.7: logs/utilgate/rmdir.sh.
 *
 * It had no -v and no --ignore-fail-on-non-empty, took any other option
 * as nothing, and printed a usage line where GNU says "missing operand". */

static int rmdir_nonempty(int e) {
    return e == ENOTEMPTY || e == EEXIST;
}

static int rmdir_ignore, rmdir_verbose;

static void rmdir_say(const char *d) {
    if (rmdir_verbose) printf("%s: removing directory, %s\n", su_short, su_quoteaf(d));
}

/* rmdir(D), and the message if it fails: 0 removed, 1 failed, -1 not
 * empty and that is to be ignored (no parents follow, as GNU's) */
static int rmdir_one(const char *d, int parent) {
    rmdir_say(d);
    if (rmdir(d) == 0) return 0;
    int e = errno;
    if (rmdir_ignore && rmdir_nonempty(e)) return -1;
    if (e == ENOTDIR && !parent) {
        size_t n = strlen(d);
        if (n && d[n - 1] == '/') {
            struct stat st;
            char *t = strdup(d);
            if (t) {
                while (n > 1 && t[n - 1] == '/') t[--n] = 0;
                if (lstat(t, &st) == 0 && S_ISLNK(st.st_mode)) {
                    struct stat s2;
                    if (stat(t, &s2) == 0 && S_ISDIR(s2.st_mode)) {
                        free(t);
                        su_error(0, 0, "failed to remove %s: Symbolic link not followed",
                                 su_quoteaf(d));
                        return 1;
                    }
                }
                free(t);
            }
        }
    }
    if (parent) su_error(0, e, "failed to remove directory %s", su_quoteaf(d));
    else su_error(0, e, "failed to remove %s", su_quoteaf(d));
    return 1;
}

/* -p: D's parents, nearest first, until one will not go */
static int rmdir_parents(const char *d) {
    char *p = strdup(d);
    if (!p) su_error(1, ENOMEM, "memory exhausted");
    int rc = 0;
    for (;;) {
        size_t n = strlen(p);
        while (n > 1 && p[n - 1] == '/') p[--n] = 0;
        char *slash = strrchr(p, '/');
        if (!slash) break;
        while (slash > p && slash[-1] == '/') slash--;
        if (slash == p) break;
        *slash = 0;
        int r = rmdir_one(p, 1);
        if (r != 0) {
            if (r > 0) rc = 1;
            break;
        }
    }
    free(p);
    return rc;
}

static int do_rmdir(int argc, char **argv) {
    enum { IGNORE_OPT = 0x100 };
    static const struct su_option longs[] = {
        { "ignore-fail-on-non-empty", SU_NO_ARG, 0, IGNORE_OPT },
        { "parents", SU_NO_ARG, 0, 'p' },
        { "verbose", SU_NO_ARG, 0, 'v' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    int parents = 0, c;
    while ((c = su_getopt_long(argc, argv, "pv", longs, 0)) != -1) {
        switch (c) {
        case 'p': parents = 1; break;
        case 'v': rmdir_verbose = 1; break;
        case IGNORE_OPT: rmdir_ignore = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... DIRECTORY...\n"
                   "Remove each DIRECTORY, which must be empty.\n"
                   "\n"
                   "      --ignore-fail-on-non-empty\n"
                   "                    say nothing when a directory is not empty\n"
                   "  -p, --parents     then remove each parent named in it too,\n"
                   "                    stopping at the first that will not go\n"
                   "  -v, --verbose     say which directory is being removed\n"
                   "      --help        show this help and exit\n"
                   "      --version     show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (su_optind == argc) {
        su_error(0, 0, "missing operand");
        su_try_help(1);
    }
    int rc = 0;
    for (int i = su_optind; i < argc; i++) {
        int r = rmdir_one(argv[i], 0);
        if (r > 0) rc = 1;
        if (r) continue;
        if (parents) rc |= rmdir_parents(argv[i]);
    }
    return rc;
}
