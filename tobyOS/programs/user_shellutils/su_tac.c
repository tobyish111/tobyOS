/* su_tac.c -- tac, as GNU coreutils': each FILE (or standard input, or
 * "-") written record by record, last record first. A record ends with
 * the separator (a newline unless -s); with -b it begins with it instead;
 * with -r the separator is a regular expression in GNU regex's default
 * (Emacs) syntax, where + and ? are operators and \{ is a brace.
 *
 * THE RECORDS, AS GNU FINDS THEM, measured (logs/utilgate/tac.sh and
 * probes of `tac -r -s 'y*'`): the separator is searched for backwards
 * from the end; each search looks only at the text before the start of
 * the previous match, and takes the match starting furthest right --
 * the longest one there. What is written for each match is the text from
 * its end to the previous match's end (from its start to the previous
 * start, with -b), then whatever precedes the first match. An empty
 * separator (-s '') leaves the input as it is.
 *
 * It read lines only, ignored -b -r -s, cut lines at 8 KiB, and said
 * "tac: FILE: ERROR" for a file it could not open. */

static char *tac_read(int fd, size_t *len) {
    size_t cap = 65536, n = 0;
    char *b = (char *)malloc(cap + 1);
    if (!b) return 0;
    for (;;) {
        if (n == cap) {
            char *nb = (char *)realloc(b, cap * 2 + 1);
            if (!nb) { free(b); errno = ENOMEM; return 0; }
            b = nb;
            cap *= 2;
        }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            free(b);
            errno = e;
            return 0;
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    b[n] = 0;
    *len = n;
    return b;
}

/* GNU regex's Emacs syntax as a POSIX ERE: \( \) \| are the group and
 * alternation, ( ) | { } are characters, \{ is a brace (no intervals),
 * * + ? at the start of a group are characters, ^ and $ anchor only at the
 * ends of a group, and inside [...] a '[' is a character ([:alpha:] is
 * not a class there). 0, or GNU's message for a pattern that is not one:
 * a '[' with nothing after it is "Invalid regular expression", one never
 * closed "Unmatched [, [^, [:, [., or [=", the groups "Unmatched ( or \("
 * and "Unmatched ) or \)", a \N past the groups closed so far "Invalid
 * back reference" (measured: `tac -r -s PATTERN`). */
static const char *tac_emacs_to_ere(const char *p, char *out, size_t cap) {
    size_t n = 0;
    int start = 1, depth = 0, closed = 0;
#define TAC_PUT(c) do { if (n + 2 >= cap) return "Regular expression too big"; out[n++] = (char)(c); } while (0)
    while (*p) {
        char c = *p;
        if (c == '\\') {
            char d = p[1];
            if (!d) return "Trailing backslash";
            p += 2;
            switch (d) {
            case '(': TAC_PUT('('); start = 1; depth++; continue;
            case ')':
                if (depth == 0) return "Unmatched ) or \\)";
                depth--;
                closed++;
                TAC_PUT(')');
                start = 0;
                continue;
            case '|': TAC_PUT('|'); start = 1; continue;
            case '`': TAC_PUT('^'); start = 1; continue;
            case '\'': TAC_PUT('$'); start = 0; continue;
            case 'w': case 'W': case '<': case '>': case 'b': case 'B':
                TAC_PUT('\\'); TAC_PUT(d); start = 0; continue;
            default:
                if (d >= '1' && d <= '9') {
                    if (d - '0' > closed) return "Invalid back reference";
                    TAC_PUT('\\'); TAC_PUT(d); start = 0; continue;
                }
                if (strchr(".[]()*+?{}|^$\\", d)) TAC_PUT('\\');
                TAC_PUT(d);
                start = 0;
                continue;
            }
        }
        if (c == '[') {
            const char *q = p + 1;
            if (*q == '^') q++;
            if (!*q) return "Invalid regular expression";
            if (*q == ']') q++;
            while (*q && *q != ']') q++;
            if (!*q) return "Unmatched [, [^, [:, [., or [=";
            /* the members, a '[' among them moved last, where "[:" cannot
             * start a class */
            const char *m = p + 1;
            TAC_PUT('[');
            if (*m == '^') { TAC_PUT('^'); m++; }
            int lbracket = 0;
            for (const char *r = m; r < q; r++) {
                if (*r == '[' && !(r > m && r[-1] == '-') && r[1] != '-') { lbracket = 1; continue; }
                TAC_PUT(*r);
            }
            if (lbracket) TAC_PUT('[');
            TAC_PUT(']');
            p = q + 1;
            start = 0;
            continue;
        }
        if (c == '*' || c == '+' || c == '?') {
            if (start) TAC_PUT('\\');
            TAC_PUT(c);
            p++;
            continue;
        }
        if (c == '^') {
            if (!start) TAC_PUT('\\');
            TAC_PUT('^');
            p++;
            continue;
        }
        if (c == '$') {
            int end = !p[1] || (p[1] == '\\' && (p[2] == ')' || p[2] == '|'));
            if (!end) TAC_PUT('\\');
            TAC_PUT('$');
            p++;
            start = 0;
            continue;
        }
        if (strchr("(){}|", c)) TAC_PUT('\\');
        TAC_PUT(c);
        p++;
        start = 0;
    }
    if (depth) return "Unmatched ( or \\(";
    out[n] = 0;
#undef TAC_PUT
    return 0;
}

struct tac_opts {
    int before, regex;
    const char *sep;
    size_t seplen;
    regex_t re;
    size_t *starts;         /* -r: every position a match starts at */
    size_t nstarts;
};

/* -r: where in BUF[0, LEN) a match starts, found once going forwards --
 * the backward search then only has to check these. */
static void tac_starts(struct tac_opts *o, const char *buf, size_t len) {
    size_t cap = 256;
    o->nstarts = 0;
    o->starts = (size_t *)malloc(cap * sizeof *o->starts);
    if (!o->starts) su_error(1, ENOMEM, "memory exhausted");
    size_t q = 0;
    while (q <= len) {
        regmatch_t m;
        int ef = (q > 0 && buf[q - 1] != '\n') ? REG_NOTBOL : 0;
        if (regexec(&o->re, buf + q, 1, &m, ef) != 0) break;
        size_t s = q + (size_t)m.rm_so;
        if (o->nstarts == cap) {
            cap *= 2;
            o->starts = (size_t *)realloc(o->starts, cap * sizeof *o->starts);
            if (!o->starts) su_error(1, ENOMEM, "memory exhausted");
        }
        o->starts[o->nstarts++] = s;
        q = s + 1;
    }
}

/* The match the search finds in BUF[0, LIMIT): its start and end, 1; or 0. */
static int tac_find(struct tac_opts *o, char *buf, size_t limit,
                    size_t *ms, size_t *me) {
    if (!o->regex) {
        size_t L = o->seplen;
        if (L > limit) return 0;
        for (size_t p = limit - L + 1; p-- > 0;) {
            if (buf[p] == o->sep[0] && memcmp(buf + p, o->sep, L) == 0) {
                *ms = p;
                *me = p + L;
                return 1;
            }
        }
        return 0;
    }
    /* the highest start below LIMIT whose match still fits before it */
    char saved = buf[limit];
    buf[limit] = 0;
    int found = 0;
    while (o->nstarts > 0) {
        size_t p = o->starts[o->nstarts - 1];
        if (p >= limit) { o->nstarts--; continue; }
        regmatch_t m;
        int ef = (p > 0 && buf[p - 1] != '\n') ? REG_NOTBOL : 0;
        if (regexec(&o->re, buf + p, 1, &m, ef) == 0 && m.rm_so == 0) {
            *ms = p;
            *me = p + (size_t)m.rm_eo;
            found = 1;
            o->nstarts--;
            break;
        }
        o->nstarts--;
    }
    buf[limit] = saved;
    return found;
}

static void tac_out(const char *b, size_t n) {
    if (n && fwrite(b, 1, n, stdout) != n) su_error(1, errno, "write error");
}

static int tac_one(struct tac_opts *o, const char *name) {
    int fd = 0;
    if (strcmp(name, "-") != 0) {
        fd = su_open_read(name, 0);
        if (fd < 0) {
            su_error(0, errno, "failed to open %s for reading", su_quoteaf(name));
            return 1;
        }
    }
    size_t len = 0;
    char *buf = tac_read(fd, &len);
    int e = errno;
    if (fd > 0) close(fd);
    if (!buf) {
        su_error(0, e, "%s: read error", su_quotef(name));
        return 1;
    }
    if (!o->regex && o->seplen == 0) {
        tac_out(buf, len);
        free(buf);
        return 0;
    }
    if (o->regex) tac_starts(o, buf, len);
    /* previous match: its start bounds the next search; its end (or, with
     * -b, its start) is where the text written for the next one stops */
    size_t pstart = len, pend = len;
    size_t ms, me;
    while (tac_find(o, buf, pstart, &ms, &me)) {
        if (o->before) tac_out(buf + ms, pstart - ms);
        else tac_out(buf + me, pend - me);
        pstart = ms;
        pend = me;
    }
    if (o->before) tac_out(buf, pstart);
    else tac_out(buf, pend);
    free(buf);
    if (o->regex) free(o->starts);
    return 0;
}

static int do_tac(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "before", SU_NO_ARG, 0, 'b' },
        { "regex", SU_NO_ARG, 0, 'r' },
        { "separator", SU_REQ_ARG, 0, 's' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    struct tac_opts o;
    memset(&o, 0, sizeof o);
    o.sep = "\n";
    int c;
    while ((c = su_getopt_long(argc, argv, "brs:", longs, 0)) != -1) {
        switch (c) {
        case 'b': o.before = 1; break;
        case 'r': o.regex = 1; break;
        case 's': o.sep = su_optarg; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n"
                   "Write each FILE to standard output, its last record first.\n"
                   "A record ends with the separator, a newline unless -s gives\n"
                   "another. With no FILE, or when FILE is -, read standard input.\n"
                   "\n"
                   "  -b, --before             put the separator before each record\n"
                   "                           instead of after it\n"
                   "  -r, --regex              read the separator as a regular\n"
                   "                           expression\n"
                   "  -s, --separator=STRING   separate records with STRING\n"
                   "      --help               show this help and exit\n"
                   "      --version            show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    o.seplen = strlen(o.sep);
    if (o.regex) {
        static char ere[8192];
        const char *msg = tac_emacs_to_ere(o.sep, ere, sizeof ere);
        if (!msg && regcomp(&o.re, ere, REG_EXTENDED | REG_NEWLINE) != 0)
            msg = "Invalid regular expression";
        if (msg) su_error(1, 0, "%s", msg);
    }
    int rc = 0;
    if (su_optind == argc) rc |= tac_one(&o, "-");
    for (int i = su_optind; i < argc; i++) rc |= tac_one(&o, argv[i]);
    if (fflush(stdout) != 0) su_error(1, errno, "write error");
    return rc;
}
