/* su_uniq.c -- uniq, as GNU coreutils' (logs/utilgate/uniq.sh): adjacent
 * matching lines of INPUT (or stdin, or -) merged, to OUTPUT (or stdout).
 *
 * What is printed is three choices, as GNU makes them: a line that has no
 * match (on unless -d or -D), the first line of a run of matches (on
 * unless -u), and the run's later lines (on with -D). So -du prints
 * nothing and -Du prints each run but its first line. -c counts ("%7d ");
 * -D with -c is refused, and --group (a blank line between runs, or
 * before, after, or both) takes none of -c -d -D -u. --all-repeated=
 * prepend|separate puts the blank line before each run, or between.
 *
 * Lines match on what is left after -f N fields (a field: blanks, then
 * not blanks), then -s N bytes, at most -w N bytes of it, with -i in any
 * case; -N is -f N and +N is -s N, the old forms. -z: lines end in NUL.
 * A line is printed with its terminator even when the input's last had
 * none. The old uniq (sbase's) had no -i, -D, --group or -z. */

struct uq_line { char *s; size_t n, cap; };

static int uq_read(FILE *f, struct uq_line *l, int delim) {
    l->n = 0;
    int c;
    while ((c = getc(f)) != EOF) {
        if (l->n + 1 >= l->cap) {
            size_t nc = l->cap ? l->cap * 2 : 128;
            char *ns = (char *)realloc(l->s, nc);
            if (!ns) su_error(1, ENOMEM, "memory exhausted");
            l->s = ns;
            l->cap = nc;
        }
        if (c == delim) { l->s[l->n] = 0; return 1; }
        l->s[l->n++] = (char)c;
    }
    if (l->s) l->s[l->n] = 0;
    return l->n > 0;
}

static struct {
    size_t skip_fields, skip_chars, check_chars;
    int ignore_case, count, delim;
    int out_unique, out_first, out_later;
    int all_rep;            /* 0 none, 1 prepend, 2 separate */
    int group;              /* 0 off, 1 separate, 2 prepend, 3 append, 4 both */
} uq;

/* where comparison starts in L, and how many bytes are compared */
static const char *uq_key(const struct uq_line *l, size_t *len) {
    const char *p = l->s, *e = l->s + l->n;
    for (size_t f = 0; f < uq.skip_fields && p < e; f++) {
        while (p < e && (*p == ' ' || *p == '\t')) p++;
        while (p < e && *p != ' ' && *p != '\t') p++;
    }
    size_t left = (size_t)(e - p);
    p += uq.skip_chars < left ? uq.skip_chars : left;
    left = (size_t)(e - p);
    *len = left < uq.check_chars ? left : uq.check_chars;
    return p;
}

static int uq_same(const struct uq_line *a, const struct uq_line *b) {
    size_t la, lb;
    const char *ka = uq_key(a, &la), *kb = uq_key(b, &lb);
    if (la != lb) return 0;
    if (!uq.ignore_case) return memcmp(ka, kb, la) == 0;
    for (size_t i = 0; i < la; i++)
        if (toupper((unsigned char)ka[i]) != toupper((unsigned char)kb[i])) return 0;
    return 1;
}

static void uq_put(const struct uq_line *l, uintmax_t count) {
    if (uq.count) printf("%7ju ", count);
    fwrite(l->s, 1, l->n, stdout);
    putchar(uq.delim);
}

/* -f -s -w's number: a decimal, as large as it likes (it is clamped) */
static size_t uq_num(const char *s, const char *what) {
    const char *p = s;
    if (!isdigit((unsigned char)*p)) su_error(1, 0, "%s: %s", s, what);
    uintmax_t v = 0;
    for (; isdigit((unsigned char)*p); p++) {
        if (v > (UINTMAX_MAX - 9) / 10) v = UINTMAX_MAX;
        else v = v * 10 + (uintmax_t)(*p - '0');
    }
    if (*p) su_error(1, 0, "%s: %s", s, what);
    return v > SIZE_MAX ? SIZE_MAX : (size_t)v;
}

static void uq_badarg(const char *opt, const char *arg, const char *const *valid) {
    fflush(stdout);
    fprintf(stderr, "%s: invalid argument %s for %s\nValid arguments are:\n",
            su_short, su_quote(arg), su_quote(opt));
    for (int i = 0; valid[i]; i++) fprintf(stderr, "  - %s\n", su_quote(valid[i]));
    su_try_help(1);
}

static int do_uniq(int argc, char **argv) {
    enum { ALLREP_OPT = 0x100, GROUP_OPT };
    static const struct su_option longs[] = {
        { "count", SU_NO_ARG, 0, 'c' },
        { "repeated", SU_NO_ARG, 0, 'd' },
        { "all-repeated", SU_OPT_ARG, 0, ALLREP_OPT },
        { "group", SU_OPT_ARG, 0, GROUP_OPT },
        { "ignore-case", SU_NO_ARG, 0, 'i' },
        { "unique", SU_NO_ARG, 0, 'u' },
        { "skip-fields", SU_REQ_ARG, 0, 'f' },
        { "skip-chars", SU_REQ_ARG, 0, 's' },
        { "check-chars", SU_REQ_ARG, 0, 'w' },
        { "zero-terminated", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    static const char *const allrep_v[] = { "none", "prepend", "separate", 0 };
    static const char *const group_v[] = { "prepend", "append", "separate", "both", 0 };
    memset(&uq, 0, sizeof uq);
    uq.check_chars = SIZE_MAX;
    uq.delim = '\n';
    uq.out_unique = uq.out_first = 1;
    /* the old forms first: -N is -f N, +N is -s N */
    char **av = (char **)malloc(((size_t)argc * 2 + 1) * sizeof *av);
    if (!av) su_error(1, ENOMEM, "memory exhausted");
    int ac = 0, dashdash = 0;
    av[ac++] = argv[0];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int digits = a[0] && a[1] && strspn(a + 1, "0123456789") == strlen(a + 1);
        if (!dashdash && !strcmp(a, "--")) dashdash = 1;
        if (!dashdash && digits && a[0] == '-') { av[ac++] = (char *)"-f"; av[ac++] = (char *)a + 1; continue; }
        if (!dashdash && digits && a[0] == '+') { av[ac++] = (char *)"-s"; av[ac++] = (char *)a + 1; continue; }
        av[ac++] = argv[i];
        /* an option whose value is the next argument: that is a value */
        int takes = 0;
        if (!dashdash && a[0] == '-' && a[1] == '-' && a[2] && !strchr(a, '=')) {
            static const char *const withval[] = { "skip-fields", "skip-chars", "check-chars" };
            for (int k = 0; k < 3; k++) if (!strncmp(withval[k], a + 2, strlen(a + 2))) takes = 1;
        } else if (!dashdash && a[0] == '-' && a[1] && a[1] != '-') {
            for (const char *p = a + 1; *p; p++)
                if (strchr("fsw", *p)) { takes = !p[1]; break; }
        }
        if (takes && i + 1 < argc) av[ac++] = argv[++i];
    }
    av[ac] = 0;
    int c, all_dup = 0;
    while ((c = su_getopt_long(ac, av, "cdDf:is:uw:z", longs, 0)) != -1) {
        switch (c) {
        case 'c': uq.count = 1; break;
        case 'd': uq.out_unique = 0; break;
        case 'D': uq.out_unique = 0; uq.out_later = 1; all_dup = 1; break;
        case ALLREP_OPT:
            uq.out_unique = 0;
            uq.out_later = 1;
            all_dup = 1;
            if (!su_optarg || !strcmp(su_optarg, "none")) uq.all_rep = 0;
            else if (!strcmp(su_optarg, "prepend")) uq.all_rep = 1;
            else if (!strcmp(su_optarg, "separate")) uq.all_rep = 2;
            else uq_badarg("--all-repeated", su_optarg, allrep_v);
            break;
        case GROUP_OPT:
            if (!su_optarg || !strcmp(su_optarg, "separate")) uq.group = 1;
            else if (!strcmp(su_optarg, "prepend")) uq.group = 2;
            else if (!strcmp(su_optarg, "append")) uq.group = 3;
            else if (!strcmp(su_optarg, "both")) uq.group = 4;
            else uq_badarg("--group", su_optarg, group_v);
            break;
        case 'f': uq.skip_fields = uq_num(su_optarg, "invalid number of fields to skip"); break;
        case 's': uq.skip_chars = uq_num(su_optarg, "invalid number of bytes to skip"); break;
        case 'w': uq.check_chars = uq_num(su_optarg, "invalid number of bytes to compare"); break;
        case 'i': uq.ignore_case = 1; break;
        case 'u': uq.out_first = 0; break;
        case 'z': uq.delim = 0; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [INPUT [OUTPUT]]\n"
                   "Write INPUT (or standard input) to OUTPUT (or standard output) with each run\n"
                   "of adjacent matching lines written once, as its first line.\n"
                   "\n"
                   "  -c, --count           put each line's number of occurrences before it\n"
                   "  -d, --repeated        only the lines that repeat, once for each run\n"
                   "  -D                    every line of each run that repeats\n"
                   "      --all-repeated[=METHOD]  like -D, with METHOD none (the default),\n"
                   "                        prepend (a blank line before each run) or\n"
                   "                        separate (a blank line between runs)\n"
                   "  -f, --skip-fields=N   do not compare the first N fields\n"
                   "      --group[=METHOD]  every line, a blank line between runs; METHOD is\n"
                   "                        separate (the default), prepend, append or both\n"
                   "  -i, --ignore-case     compare letters without regard to case\n"
                   "  -s, --skip-chars=N    do not compare the first N characters\n"
                   "  -u, --unique          only the lines that do not repeat\n"
                   "  -z, --zero-terminated  lines end in NUL, not newline\n"
                   "  -w, --check-chars=N   compare at most N characters of each line\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "A field is blanks (spaces or tabs) and then what is not blank; fields\n"
                   "are skipped before characters. Only ADJACENT lines are compared: sort\n"
                   "first to find every repeat (or use sort -u).\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (uq.group && (uq.count || !uq.out_unique || !uq.out_first || uq.out_later)) {
        su_error(0, 0, "--group is mutually exclusive with -c/-d/-D/-u");
        su_try_help(1);
    }
    if (all_dup && uq.count) {
        su_error(0, 0, "printing all duplicated lines and repeat counts is meaningless");
        su_try_help(1);
    }
    int nop = ac - su_optind;
    if (nop > 2) {
        su_error(0, 0, "extra operand %s", su_quote(av[su_optind + 2]));
        su_try_help(1);
    }
    const char *in = nop >= 1 ? av[su_optind] : "-";
    const char *out = nop >= 2 ? av[su_optind + 1] : "-";
    FILE *f = stdin;
    if (strcmp(in, "-")) {
        f = fopen(in, "r");
        /* a directory: Linux opens it and the read fails, which is what
         * GNU reports; the native open refuses it outright */
        if (!f && errno == EISDIR) su_error(1, EISDIR, "error reading %s", su_quote(in));
        if (!f) su_error(1, errno, "%s", su_quotef(in));
    }
    if (strcmp(out, "-")) {
        /* OUTPUT is stdout from here on (libtoby has no freopen) */
        int ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (ofd < 0) su_error(1, errno, "%s", su_quotef(out));
        fflush(stdout);
        if (ofd != 1) { dup2(ofd, 1); close(ofd); }
    }

    /* run by run: FIRST is the run's first line, RUN its length */
    struct uq_line first = { 0, 0, 0 }, cur = { 0, 0, 0 };
    uintmax_t run = 0;
    int shown_run = 0;
    errno = 0;
    if (uq_read(f, &first, uq.delim)) {
        run = 1;
        if (uq.group == 2 || uq.group == 4) putchar(uq.delim);
        if (uq.group) uq_put(&first, 0);
        for (;;) {
            int got = uq_read(f, &cur, uq.delim);
            if (got && uq_same(&first, &cur)) {
                run++;
                if (uq.group) uq_put(&cur, 0);
                else if (uq.out_later) {
                    if (run == 2) {
                        /* the run is a repeat: its separator, its first line */
                        if (uq.all_rep == 1 || (uq.all_rep == 2 && shown_run)) putchar(uq.delim);
                        shown_run = 1;
                        if (uq.out_first) uq_put(&first, 0);
                    }
                    uq_put(&cur, 0);
                }
                continue;
            }
            if (!uq.group) {
                if (run == 1) { if (uq.out_unique) uq_put(&first, 1); }
                else if (!uq.out_later && uq.out_first) uq_put(&first, run);
            }
            if (!got) break;
            struct uq_line t = first; first = cur; cur = t;
            run = 1;
            if (uq.group) {
                putchar(uq.delim);          /* one line between runs, for every method */
                uq_put(&first, 0);
            }
        }
        if (uq.group == 3 || uq.group == 4) putchar(uq.delim);
    }
    if (ferror(f)) {
        int e = errno;
        su_error(1, e, "error reading %s", su_quote(in));
    }
    if (f != stdin) fclose(f);
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return 0;
}
