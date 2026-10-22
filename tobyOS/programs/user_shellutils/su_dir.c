/* su_dir.c -- dir and vdir, as GNU coreutils 9.7's
 * (logs/utilgate/staging/dir.sh, vdir.sh): ls with other defaults, run
 * through su_ls.c's do_ls -- not a second ls.
 *
 * GNU documents dir as `ls -C -b` and vdir as `ls -l -b`; measured, the
 * defaults are softer than options would be:
 *   - the escape quoting yields to a valid QUOTING_STYLE (QUOTING_STYLE=c
 *     dir quotes as c), and an invalid one -- the empty string too -- is
 *     warned of and ignored;
 *   - vdir's long format is only what is used when no option chose a
 *     format: `vdir -1`, `vdir -C`, `vdir -x`, `vdir -m`, `vdir --format=X`
 *     and `vdir --zero` (one per line) are not long, where `ls -l -1` is.
 * So dir runs ls with -C and (unless QUOTING_STYLE says otherwise) -b put
 * before its own arguments, and vdir with -l only when none of its
 * arguments is one of those format options. The name it was run as stays
 * argv[0], so messages and --help say dir or vdir. */

static const char *const sm_dir_styles[] = {
    "literal", "shell", "shell-always", "shell-escape", "shell-escape-always",
    "c", "c-maybe", "escape", "locale", "clocale", 0 };

/* ls's long options, to know which take an argument when looking
 * through vdir's: (name, takes an argument, sets the format) */
static const struct { const char *name; char arg; char fmt; } sm_dir_longs[] = {
    { "all", 0, 0 }, { "almost-all", 0, 0 }, { "author", 0, 0 }, { "escape", 0, 0 },
    { "block-size", 1, 0 }, { "ignore-backups", 0, 0 }, { "color", 0, 0 },
    { "colour", 0, 0 }, { "directory", 0, 0 }, { "dired", 0, 0 }, { "classify", 0, 0 },
    { "file-type", 0, 0 }, { "format", 1, 1 }, { "full-time", 0, 0 },
    { "group-directories-first", 0, 0 }, { "no-group", 0, 0 },
    { "human-readable", 0, 0 }, { "si", 0, 0 }, { "dereference-command-line", 0, 0 },
    { "dereference-command-line-symlink-to-dir", 0, 0 }, { "hide", 1, 0 },
    { "hyperlink", 0, 0 }, { "indicator-style", 1, 0 }, { "inode", 0, 0 },
    { "ignore", 1, 0 }, { "kibibytes", 0, 0 }, { "dereference", 0, 0 },
    { "literal", 0, 0 }, { "numeric-uid-gid", 0, 0 }, { "hide-control-chars", 0, 0 },
    { "show-control-chars", 0, 0 }, { "quote-name", 0, 0 }, { "quoting-style", 1, 0 },
    { "reverse", 0, 0 }, { "recursive", 0, 0 }, { "size", 0, 0 }, { "sort", 1, 0 },
    { "time", 1, 0 }, { "time-style", 1, 0 }, { "tabsize", 1, 0 }, { "width", 1, 0 },
    { "context", 0, 0 }, { "zero", 0, 1 }, { "help", 0, 0 }, { "version", 0, 0 },
};

/* whether ARGV holds an option that picks the output format (-1 -C -x -m
 * --format --zero) -- looked for as getopt would find it: not past "--",
 * nor in an option's argument */
static int sm_dir_format_given(int argc, char **argv) {
    int posix = getenv("POSIXLY_CORRECT") != 0;
    int n = (int)(sizeof sm_dir_longs / sizeof sm_dir_longs[0]);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) break;
        if (a[0] != '-' || !a[1]) {
            if (posix) break;
            continue;
        }
        if (a[1] == '-') {
            const char *name = a + 2;
            size_t len = strcspn(name, "=");
            int hit = -1, exact = 0, many = 0;
            for (int k = 0; k < n; k++) {
                if (strncmp(sm_dir_longs[k].name, name, len)) continue;
                if (strlen(sm_dir_longs[k].name) == len) { hit = k; exact = 1; break; }
                if (hit >= 0) many = 1;
                else hit = k;
            }
            if (hit < 0 || (many && !exact)) continue;   /* ls will refuse it */
            if (sm_dir_longs[hit].fmt) return 1;
            if (sm_dir_longs[hit].arg && !name[len]) i++;
            continue;
        }
        for (const char *p = a + 1; *p; p++) {
            if (strchr("1Cxm", *p)) return 1;
            if (strchr("wIT", *p)) {
                if (!p[1]) i++;     /* the argument is the next word */
                break;              /* or the rest of this one */
            }
        }
    }
    return 0;
}

static int sm_dir_run(int argc, char **argv, int vdir) {
    int escape = 1;
    const char *q = getenv("QUOTING_STYLE");
    if (q) {
        for (int i = 0; sm_dir_styles[i]; i++) if (!strcmp(q, sm_dir_styles[i])) escape = 0;
        /* do_ls passes over an empty one in silence; GNU's warns */
        if (!*q)
            su_error(0, 0, "ignoring invalid value of environment variable QUOTING_STYLE: %s",
                     su_quote(q));
    }
    char **nv = (char **)malloc((size_t)(argc + 3) * sizeof *nv);
    if (!nv) su_error(2, ENOMEM, "memory exhausted");
    int k = 0;
    nv[k++] = argv[0];
    if (!vdir) nv[k++] = (char *)"-C";
    else if (!sm_dir_format_given(argc, argv)) nv[k++] = (char *)"-l";
    if (escape) nv[k++] = (char *)"-b";
    for (int i = 1; i < argc; i++) nv[k++] = argv[i];
    nv[k] = 0;
    return do_ls(k, nv);
}

static int do_dir(int argc, char **argv) { return sm_dir_run(argc, argv, 0); }
static int do_vdir(int argc, char **argv) { return sm_dir_run(argc, argv, 1); }
