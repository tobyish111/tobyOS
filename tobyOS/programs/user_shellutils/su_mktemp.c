/* su_mktemp.c -- mktemp, as GNU coreutils 9.7's (logs/utilgate/mktemp.sh):
 * a file (mode 0600) or with -d a directory (0700) made under a fresh
 * name, and the name printed. With no TEMPLATE it is tmp.XXXXXXXXXX in
 * $TMPDIR or /tmp.
 *
 * THE TEMPLATE, measured: the X's that are replaced are the run just
 * before the suffix, and the suffix is --suffix's, or else whatever
 * follows the template's last X ("aXXXb" makes a????b). The checks come
 * in this order, each with GNU's words:
 *
 *   with --suffix, the template must end in X;
 *   the suffix may hold no slash ("XXX/a" is an invalid suffix '/a');
 *   at least 3 X's ("too few X's in template 'TEMPLATE'", the template
 *       as given, without --suffix's part, but with an implied one);
 *   -t: no slash at all; -p/--tmpdir: not absolute;
 *
 * and -q silences none of them, only the failure to create.
 *
 * WHERE: a template is taken relative to the working directory, unless
 * -p DIR, --tmpdir[=DIR] or -t say otherwise, or there is no template. -p
 * and --tmpdir use DIR when it is given and not empty, else $TMPDIR when
 * set and not empty, else /tmp (a later bare --tmpdir forgets an earlier
 * -p); -t prefers $TMPDIR to -p's DIR. DIR and the template are joined
 * as GNU joins them: DIR's trailing slashes are dropped and one put back,
 * unless DIR is all slashes ("//" + "fooXXX" is "//fooXXX").
 *
 * -u makes nothing: a name that lstat does not find is printed, and an
 * lstat failure other than ENOENT is the failure to create ("failed to
 * create file via template 'sub/file/XXX': Not a directory").
 *
 * The X's become [A-Za-z0-9] from /dev/urandom (all 62 appear; tobyOS's
 * devfs has it), a name taken is tried again, up to 62^3 times. The old
 * mktemp used [a-z0-9] from an LCG seeded with the pid, ignored -u, -q,
 * -p, -t, --suffix and TMPDIR, and wanted 6 X's at the very end. */

static int mkt_rand_fd = -2;
static uint64_t mkt_state;

/* one random value below 62, uniformly */
static unsigned mkt_rand62(void) {
    if (mkt_rand_fd == -2) {
        mkt_rand_fd = open("/dev/urandom", O_RDONLY);
        /* without the device: a generator seeded from what differs
         * between two runs -- the pid, the time, an address */
        mkt_state = (uint64_t)getpid() * 0x9E3779B97F4A7C15ull ^ (uint64_t)time(0) ^
                    (uint64_t)(uintptr_t)&mkt_state;
        if (!mkt_state) mkt_state = 0x2545F4914F6CDD1Dull;
    }
    for (;;) {
        unsigned char b;
        if (mkt_rand_fd >= 0 && read(mkt_rand_fd, &b, 1) == 1) {
            if (b < 248) return b % 62;          /* 248 = 4 * 62 */
            continue;
        }
        mkt_state ^= mkt_state << 13;
        mkt_state ^= mkt_state >> 7;
        mkt_state ^= mkt_state << 17;
        unsigned v = (unsigned)(mkt_state >> 24) & 0xff;
        if (v < 248) return v % 62;
    }
}

/* DIR joined to NAME */
static char *mkt_join(const char *dir, const char *name) {
    size_t dl = strlen(dir);
    size_t keep = dl;
    while (keep > 0 && dir[keep - 1] == '/') keep--;
    int sep = 1;
    if (keep == 0) { keep = dl; sep = dl == 0; }   /* all slashes: as it is */
    char *r = (char *)malloc(keep + strlen(name) + 2);
    if (!r) su_error(1, ENOMEM, "memory exhausted");
    memcpy(r, dir, keep);
    size_t n = keep;
    if (sep && keep) r[n++] = '/';
    strcpy(r + n, name);
    return r;
}

static int do_mktemp(int argc, char **argv) {
    enum { SUFFIX_OPT = 0x100, TMPDIR_OPT };
    static const struct su_option longs[] = {
        { "directory", SU_NO_ARG, 0, 'd' },
        { "quiet", SU_NO_ARG, 0, 'q' },
        { "dry-run", SU_NO_ARG, 0, 'u' },
        { "suffix", SU_REQ_ARG, 0, SUFFIX_OPT },
        { "tmpdir", SU_OPT_ARG, 0, TMPDIR_OPT },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int dir = 0, quiet = 0, dry = 0, use_dir = 0, dash_t = 0, c;
    const char *suffix = 0, *dir_arg = 0;
    while ((c = su_getopt_long(argc, argv, "dp:qtuV", longs, 0)) != -1) {
        switch (c) {
        case 'd': dir = 1; break;
        case 'q': quiet = 1; break;
        case 'u': dry = 1; break;
        case 't': dash_t = 1; use_dir = 1; break;
        case 'p': use_dir = 1; dir_arg = su_optarg; break;
        case TMPDIR_OPT: use_dir = 1; dir_arg = su_optarg; break;
        case SUFFIX_OPT: suffix = su_optarg; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [TEMPLATE]\n"
                   "Make a temporary file or directory under a name no one else has,\n"
                   "and print the name. TEMPLATE's last component needs a run of at\n"
                   "least 3 'X's, which become random letters and digits. With no\n"
                   "TEMPLATE, tmp.XXXXXXXXXX in the temporary directory (as --tmpdir).\n"
                   "A file is made u+rw, a directory u+rwx, less what the umask takes.\n"
                   "\n"
                   "  -d, --directory     make a directory instead of a file\n"
                   "  -u, --dry-run       make nothing, only print a name (unsafe)\n"
                   "  -q, --quiet         say nothing when the file or directory\n"
                   "                        cannot be made\n"
                   "      --suffix=SUFF   put SUFF (no slash in it) after the X's; a\n"
                   "                        TEMPLATE not ending in X implies one\n"
                   "  -p DIR, --tmpdir[=DIR]  make TEMPLATE (not an absolute name) in\n"
                   "                        DIR, or with no DIR in $TMPDIR if set,\n"
                   "                        else /tmp; only its last component is made\n"
                   "  -t                  TEMPLATE is a single component, in $TMPDIR if\n"
                   "                        set, else -p's DIR, else /tmp (obsolete)\n"
                   "      --help        show this help and exit\n"
                   "      --version     show the version and exit\n",
                   su_name);
            return 0;
        case 'V':
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    int nops = argc - su_optind;
    if (nops > 1) {
        su_error(0, 0, "too many templates");
        su_try_help(1);
    }
    const char *tmpl = "tmp.XXXXXXXXXX";
    if (nops == 1) tmpl = argv[su_optind];
    else use_dir = 1;

    /* the suffix: --suffix's, or what follows the last X */
    size_t tl = strlen(tmpl);
    char *full;
    size_t xend;                       /* where the X run ends in FULL */
    if (suffix) {
        if (tl == 0 || tmpl[tl - 1] != 'X')
            su_error(1, 0, "with --suffix, template %s must end in X", su_quote(tmpl));
        full = (char *)malloc(tl + strlen(suffix) + 1);
        if (!full) su_error(1, ENOMEM, "memory exhausted");
        strcpy(full, tmpl);
        strcat(full, suffix);
        xend = tl;
    } else {
        full = strdup(tmpl);
        if (!full) su_error(1, ENOMEM, "memory exhausted");
        const char *lx = strrchr(tmpl, 'X');
        xend = lx ? (size_t)(lx - tmpl) + 1 : tl;
        suffix = tmpl + xend;
    }
    if (strchr(suffix, '/'))
        su_error(1, 0, "invalid suffix %s, contains directory separator", su_quote(suffix));
    size_t xs = 0;
    while (xs < xend && full[xend - 1 - xs] == 'X') xs++;
    if (xs < 3) su_error(1, 0, "too few X's in template %s", su_quote(tmpl));

    if (use_dir) {
        const char *td = getenv("TMPDIR");
        const char *where;
        if (dash_t) {
            if (strchr(tmpl, '/'))
                su_error(1, 0, "invalid template, %s, contains directory separator",
                         su_quote(tmpl));
            where = (td && *td) ? td : (dir_arg && *dir_arg) ? dir_arg : "/tmp";
        } else {
            if (tmpl[0] == '/')
                su_error(1, 0, "invalid template, %s; with --tmpdir, it may not be absolute",
                         su_quote(tmpl));
            where = (dir_arg && *dir_arg) ? dir_arg : (td && *td) ? td : "/tmp";
        }
        char *j = mkt_join(where, full);
        xend += strlen(j) - strlen(full);
        free(full);
        full = j;
    }

    static const char set[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    unsigned long tries = 1;
    for (size_t i = 0; i < xs && tries < 238328ul; i++) tries *= 62;
    if (tries > 238328ul) tries = 238328ul;
    char *name = strdup(full);
    if (!name) su_error(1, ENOMEM, "memory exhausted");
    int err = EEXIST;
    for (unsigned long a = 0; a < tries; a++) {
        for (size_t i = 0; i < xs; i++) name[xend - xs + i] = set[mkt_rand62()];
        if (dry) {
            struct stat st;
            if (lstat(name, &st) == 0) continue;
            err = errno;
            if (err == ENOENT) { err = 0; break; }
            break;
        }
        if (dir) {
            if (mkdir(name, 0700) == 0) { err = 0; break; }
        } else {
            int fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) { close(fd); err = 0; break; }
        }
        err = errno;
        if (err != EEXIST) break;
    }
    if (err) {
        if (!quiet)
            su_error(0, err, "failed to create %s via template %s",
                     dir ? "directory" : "file", su_quote(full));
        return 1;
    }
    puts(name);
    if (fflush(stdout) != 0 || ferror(stdout)) {
        int e = errno;
        if (!dry) { if (dir) rmdir(name); else unlink(name); }
        su_error(1, e, "write error");
    }
    return 0;
}
