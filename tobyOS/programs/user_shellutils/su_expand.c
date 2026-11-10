/* su_expand.c -- expand and unexpand, as GNU coreutils 9.7's
 * (logs/utilgate/expand.sh, unexpand.sh): tabs to spaces, and
 * runs of blanks back to tabs, for each FILE (or standard input) read as
 * ONE stream -- a line that a file leaves unfinished goes on in the next.
 *
 * THE TAB STOPS (-t LIST, both commands, measured): numbers separated by
 * commas or blanks, empty items skipped. One number alone is a tab size;
 * several are stops, ascending and nonzero. The last may be '/N' (stops
 * every N columns after the list, multiples of N counted from column 0)
 * or '+N' (every N columns counted from the last stop); the prefix may be
 * written before an empty item ("2,,/,4" is "2,/4"), the last prefix
 * character of a run counts ("+/4" is "/4"), and /0 or +0 is none. Past
 * the list's end with no prefix, expand writes one space for a tab and
 * unexpand stops converting. Several -t add to one list. GNU's
 * complaints are made in the order measured: a number larger than
 * INTMAX_MAX is "tab stop is too large" and the parse goes on (the run
 * then fails); a stray character, a prefix after a digit, a second '/'
 * or '+', a plain number after a prefixed one in the same argument, or
 * '/' and '+' both, end it at once; zero and descending stops are found
 * when the options are done.
 *
 * THE OLD FORMS DIFFER: expand's -N takes the rest of its argument as a
 * list ("-2,/4"); unexpand's digits accumulate into one number across
 * arguments ("-1 -2" is 12) until a ',' option or the end adds it, after
 * any -t list, and do not turn on -a as -t does.
 *
 * UNEXPAND'S RULE, measured over many lines: where blanks are converted
 * (only before the first non-blank, unless -a or -t; --first-only wins),
 * a run of blanks becomes one tab per stop it reaches and then the spaces
 * past the last stop; a run reaching no stop stays as it was. A run that
 * is ONE space after a non-blank stays a space even when it reaches a
 * stop ("abcdefg h" keeps its space; a single leading space at -t 1 does
 * not). A backspace is a non-blank that moves back a column. GNU's
 * unexpand allocates a buffer as large as its largest stop, so a huge
 * one is "memory exhausted"; this one asks for the same.
 *
 * The old expand and unexpand took only -t N (one size) and -i/-a. */

static struct {
    uintmax_t *stops;
    size_t n, cap;
    uintmax_t extend, incr;     /* '/N' and '+N'; 0 when absent */
    uintmax_t single;           /* the tab size, when the list is one */
    int ok;
} tex;

static void tex_add(uintmax_t v) {
    if (tex.n == tex.cap) {
        tex.cap = tex.cap ? tex.cap * 2 : 16;
        tex.stops = (uintmax_t *)realloc(tex.stops, tex.cap * sizeof *tex.stops);
        if (!tex.stops) su_error(1, ENOMEM, "memory exhausted");
    }
    tex.stops[tex.n++] = v;
}

static void tex_set_extend(uintmax_t v) {
    if (tex.extend) su_error(1, 0, "'/' specifier only allowed with the last value");
    if (tex.incr) su_error(1, 0, "'/' specifier is mutually exclusive with '+'");
    tex.extend = v;
}

static void tex_set_incr(uintmax_t v) {
    if (tex.incr) su_error(1, 0, "'+' specifier only allowed with the last value");
    if (tex.extend) su_error(1, 0, "'/' specifier is mutually exclusive with '+'");
    tex.incr = v;
}

/* one -t argument (or expand's -N); a failure sets tex.ok to 0 */
static void tex_parse(const char *s) {
    int have = 0, ext = 0, inc = 0, over = 0, last_special = 0;
    uintmax_t v = 0;
    const char *num = 0;
    for (const char *p = s;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ',' || c == ' ' || c == '\t' || c == 0) {
            /* the last number of the argument counts only if all went
             * well; one ended by a separator counts regardless */
            if (have && (c != 0 || tex.ok)) {
                if (ext) { tex_set_extend(v); last_special = '/'; }
                else if (inc) { tex_set_incr(v); last_special = '+'; }
                else {
                    if (last_special)
                        su_error(1, 0, "'%c' specifier only allowed with the last value",
                                 last_special);
                    tex_add(v);
                }
                ext = inc = 0;
            }
            have = 0;
            if (!c) break;
            continue;
        }
        if (c == '/' || c == '+') {
            if (have)
                su_error(1, 0, "'%c' specifier not at start of number: %s", c, su_quote(p));
            ext = c == '/';
            inc = c == '+';
            continue;
        }
        if (c >= '0' && c <= '9') {
            if (!have) { have = 1; v = 0; num = p; over = 0; }
            if (!over) {
                unsigned d = c - '0';
                if (v > ((uintmax_t)INTMAX_MAX - d) / 10) {
                    size_t len = strspn(num, "0123456789");
                    char *t = strndup(num, len);
                    su_error(0, 0, "tab stop is too large %s", su_quote(t ? t : num));
                    free(t);
                    tex.ok = 0;
                    over = 1;
                } else v = v * 10 + d;
            }
            continue;
        }
        su_error(1, 0, "tab size contains invalid character(s): %s", su_quote(p));
    }
}

/* the list as a whole, once the options are read */
static void tex_finish(void) {
    for (size_t i = 0; i < tex.n; i++) {
        if (tex.stops[i] == 0) su_error(1, 0, "tab size cannot be 0");
        if (i && tex.stops[i] <= tex.stops[i - 1]) su_error(1, 0, "tab sizes must be ascending");
    }
    tex.single = 0;
    if (tex.n == 0) tex.single = tex.extend ? tex.extend : tex.incr ? tex.incr : 8;
    else if (tex.n == 1 && !tex.extend && !tex.incr) tex.single = tex.stops[0];
}

/* the first stop after column COL; *BEYOND set when there is none */
static uintmax_t tex_next(uintmax_t col, int *beyond) {
    *beyond = 0;
    if (tex.single) {
        uintmax_t r = (col / tex.single + 1) * tex.single;
        if (r <= col) su_error(1, 0, "input line is too long");
        return r;
    }
    size_t lo = 0, hi = tex.n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (tex.stops[mid] > col) hi = mid;
        else lo = mid + 1;
    }
    if (lo < tex.n) return tex.stops[lo];
    if (tex.extend) return (col / tex.extend + 1) * tex.extend;
    if (tex.incr) {
        uintmax_t last = tex.stops[tex.n - 1];
        return last + ((col - last) / tex.incr + 1) * tex.incr;
    }
    *beyond = 1;
    return col + 1;
}

/* the input: each operand in turn, as one stream of bytes */
struct tex_in {
    char **names;
    int n, i;
    int fd;
    const char *cur;
    int status;
    size_t pos, len;
    unsigned char buf[16384];
};

static int tex_getc(struct tex_in *in) {
    for (;;) {
        if (in->pos < in->len) return in->buf[in->pos++];
        if (in->fd < 0) {
            if (in->i >= in->n) return EOF;
            in->cur = in->names[in->i++];
            if (!strcmp(in->cur, "-")) in->fd = 0;
            else {
                in->fd = su_open_read(in->cur, 0);
                if (in->fd < 0) {
                    su_error(0, errno, "%s", su_quotef(in->cur));
                    in->status = 1;
                    continue;
                }
            }
        }
        ssize_t r = read(in->fd, in->buf, sizeof in->buf);
        if (r < 0 && errno == EINTR) continue;
        if (r > 0) { in->pos = 0; in->len = (size_t)r; continue; }
        if (r < 0) {
            su_error(0, errno, "%s", su_quotef(in->cur));
            in->status = 1;
        }
        if (in->fd > 0) close(in->fd);
        in->fd = -1;
    }
}

static void tex_open(struct tex_in *in, int argc, char **argv, int first) {
    static char *dash[] = { (char *)"-", 0 };
    memset(in, 0, offsetof(struct tex_in, buf));
    in->fd = -1;
    if (first < argc) { in->names = argv + first; in->n = argc - first; }
    else { in->names = dash; in->n = 1; }
}

static void tex_spaces(uintmax_t n) {
    while (n--) putchar(' ');
}

static int tex_done(struct tex_in *in) {
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return in->status;
}

static const char tex_list_help[] =
    "  -t, --tabs=N     a tab every N columns, not every 8\n"
    "  -t, --tabs=LIST  tab stops at the columns of a list separated by commas;\n"
    "                     the last item may be written /N for a stop every N\n"
    "                     columns from there on, or +N for one every N columns\n"
    "                     counted from the last listed stop\n"
    "      --help     show this help and exit\n"
    "      --version  show the version and exit\n";

static int do_expand(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "initial", SU_NO_ARG, 0, 'i' },
        { "tabs", SU_REQ_ARG, 0, 't' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&tex, 0, sizeof tex);
    tex.ok = 1;
    int initial_only = 0, c;
    while ((c = su_getopt_long(argc, argv, "it:0::1::2::3::4::5::6::7::8::9::", longs, 0)) != -1) {
        switch (c) {
        case 'i': initial_only = 1; break;
        case 't':
            tex_parse(su_optarg);
            if (!tex.ok) exit(1);
            break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n", su_name);
            printf("Write each FILE to standard output with its tabs turned into spaces.\n"
                   "\n"
                   "With no FILE, or when FILE is -, read standard input.\n"
                   "\n"
                   "A long option's argument is required by its short form too.\n"
                   "  -i, --initial    convert only the tabs before a line's first non-blank\n");
            fputs(tex_list_help, stdout);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default:
            if (c >= '0' && c <= '9') {
                /* the old form: the digit and the rest of its argument */
                size_t len = su_optarg ? strlen(su_optarg) : 0;
                char *l = (char *)malloc(len + 2);
                if (!l) su_error(1, ENOMEM, "memory exhausted");
                l[0] = (char)c;
                if (len) memcpy(l + 1, su_optarg, len);
                l[len + 1] = 0;
                tex_parse(l);
                free(l);
                if (!tex.ok) exit(1);
                break;
            }
            su_try_help(1);
        }
    }
    tex_finish();
    struct tex_in *in = (struct tex_in *)malloc(sizeof *in);
    if (!in) su_error(1, ENOMEM, "memory exhausted");
    tex_open(in, argc, argv, su_optind);
    uintmax_t col = 0;
    int lead = 1, ch;
    while ((ch = tex_getc(in)) != EOF) {
        if (ch == '\t' && (!initial_only || lead)) {
            int beyond;
            uintmax_t next = tex_next(col, &beyond);
            tex_spaces(next - col);
            col = next;
            continue;
        }
        putchar(ch);
        if (ch == '\n') { col = 0; lead = 1; continue; }
        if (ch == '\b') { if (col) col--; lead = 0; continue; }
        if (ch != ' ' && ch != '\t') lead = 0;
        col++;
    }
    return tex_done(in);
}

/* unexpand's pending run of blanks */
static struct {
    char *raw;
    size_t n, cap;
    uintmax_t start, tabs, last_stop;
    int after_nonblank;     /* the run follows a non-blank on its line */
} tux;

static void tux_flush(uintmax_t col) {
    if (tux.n == 0) return;
    if (tux.tabs == 0 || (tux.n == 1 && tux.raw[0] == ' ' && tux.after_nonblank)) {
        fwrite(tux.raw, 1, tux.n, stdout);
    } else {
        for (uintmax_t i = 0; i < tux.tabs; i++) putchar('\t');
        tex_spaces(col - tux.last_stop);
    }
    tux.n = 0;
    tux.tabs = 0;
}

static void tux_push(char c) {
    if (tux.n == tux.cap) {
        tux.cap = tux.cap ? tux.cap * 2 : 256;
        tux.raw = (char *)realloc(tux.raw, tux.cap);
        if (!tux.raw) su_error(1, ENOMEM, "memory exhausted");
    }
    tux.raw[tux.n++] = c;
}

static int do_unexpand(int argc, char **argv) {
    enum { FIRST_OPT = 0x100 };
    static const struct su_option longs[] = {
        { "all", SU_NO_ARG, 0, 'a' },
        { "first-only", SU_NO_ARG, 0, FIRST_OPT },
        { "tabs", SU_REQ_ARG, 0, 't' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&tex, 0, sizeof tex);
    memset(&tux, 0, sizeof tux);
    tex.ok = 1;
    int all = 0, first_only = 0, c, have_old = 0;
    uintmax_t old = 0;
    while ((c = su_getopt_long(argc, argv, ",0123456789at:", longs, 0)) != -1) {
        switch (c) {
        case 'a': all = 1; break;
        case FIRST_OPT: first_only = 1; break;
        case 't':
            all = 1;
            tex_parse(su_optarg);
            if (!tex.ok) exit(1);
            break;
        case ',':
            if (have_old) tex_add(old);
            have_old = 0;
            break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n", su_name);
            printf("Write each FILE to standard output with runs of blanks turned into tabs.\n"
                   "\n"
                   "With no FILE, or when FILE is -, read standard input.\n"
                   "\n"
                   "A long option's argument is required by its short form too.\n"
                   "  -a, --all        convert every run of blanks, not only the leading ones\n"
                   "      --first-only  convert only the leading blanks, even with -a or -t\n");
            fputs(tex_list_help, stdout);
            printf("A -t option turns on -a.\n");
            return 0;
        case SU_VERSION_OPT: return su_version();
        default:
            if (c >= '0' && c <= '9') {
                unsigned d = (unsigned)(c - '0');
                if (!have_old) { old = 0; have_old = 1; }
                if (old > ((uintmax_t)INTMAX_MAX - d) / 10)
                    su_error(1, 0, "tab stop value is too large");
                old = old * 10 + d;
                break;
            }
            su_try_help(1);
        }
    }
    if (have_old) tex_add(old);
    if (first_only) all = 0;
    tex_finish();
    {
        /* GNU sizes a buffer by the largest stop */
        uintmax_t big = tex.single ? tex.single : tex.n ? tex.stops[tex.n - 1] : 0;
        void *probe = big >= SIZE_MAX / 2 ? 0 : malloc((size_t)big + 1);
        if (!probe) su_error(1, 0, "memory exhausted");
        free(probe);
    }
    struct tex_in *in = (struct tex_in *)malloc(sizeof *in);
    if (!in) su_error(1, ENOMEM, "memory exhausted");
    tex_open(in, argc, argv, su_optind);
    uintmax_t col = 0;
    int convert = 1, seen_nonblank = 0, ch;
    while ((ch = tex_getc(in)) != EOF) {
        if ((ch == ' ' || ch == '\t') && convert) {
            int beyond;
            uintmax_t next = tex_next(col, &beyond);
            if (beyond) {
                /* no stop left on this line: the rest is as it is */
                tux_flush(col);
                convert = 0;
                putchar(ch);
                continue;
            }
            if (tux.n == 0) { tux.start = col; tux.after_nonblank = seen_nonblank; }
            tux_push((char)ch);
            if (ch == '\t') col = next;
            else col++;
            if (col == next) { tux.tabs++; tux.last_stop = col; }
            continue;
        }
        tux_flush(col);
        putchar(ch);
        if (ch == '\n') {
            col = 0;
            convert = 1;
            seen_nonblank = 0;
            continue;
        }
        if (ch == ' ' || ch == '\t') continue;      /* not converting */
        if (ch == '\b') { if (col) col--; }
        else col++;
        seen_nonblank = 1;
        if (!all) convert = 0;
    }
    tux_flush(col);
    return tex_done(in);
}
