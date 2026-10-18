/* su_nl.c -- nl, as GNU coreutils 9.7's (logs/utilgate/nl.sh):
 * the lines of each FILE (or standard input), all read as one stream,
 * written with line numbers.
 *
 * LOGICAL PAGES: a line that is exactly the delimiter three times starts
 * a header section, twice a body, once a footer (the delimiter is "\:"
 * by default; -d C means C followed by ':', a longer -d is used as it
 * is, and -d '' turns sections off). Such a line is written as an empty
 * line, and -- measured -- EVERY section line, not only a header, puts
 * the number back to -v's value, unless -p. Each section numbers by its
 * own style: a (all lines), t (non-empty ones; the body's default), n
 * (none; the header's and footer's default), or pBRE (lines the basic
 * regular expression matches anywhere) -- GNU reads only a style's first
 * character, so "ax" is a. Under a, -l N numbers only every
 * Nth of a run of empty lines; the count runs across sections and files
 * and only a's lines touch it.
 *
 * A numbered line is the number, -w wide (6), as -n says: rn right
 * aligned, ln left aligned, rz zero padded after its sign; then -s's
 * separator (a TAB), then the line. An unnumbered line has as many spaces
 * as the number and the separator would take. The number then grows by
 * -i (1); going past INTMAX_MAX or INTMAX_MIN is only an error when the
 * next number is written ("line number overflow").
 *
 * OPTIONS, measured: a bad STYLE or FORMAT is reported and the options
 * go on, the run then failing with the try-help line; a bad number or a
 * bad expression ends it at once. -v and -i take any intmax_t (else
 * EOVERFLOW's message), -w 1 to INT_MAX (0 or less is ERANGE's), -l any
 * count that is not negative. The expression's messages are glibc's,
 * written here from the error code, since libtoby's regerror words them
 * differently. GNU's nl searches a line containing NUL bytes whole; on
 * the host build REG_STARTEND does the same, and without it each piece
 * between NULs is searched.
 *
 * The old nl had -b a/t/n, -s, -v, -w and ignored sections. */

struct tnl_style { char kind; regex_t re; int has_re; };

static struct {
    struct tnl_style st[3];     /* header, body, footer */
    intmax_t start, incr, line;
    int overflowed;
    uintmax_t blank_join, blanks;
    int width;
    int fmt;                    /* 'l' ln, 'r' rn, 'z' rz */
    const char *sep;
    size_t seplen;
    char *delim;                /* NULL: no sections */
    size_t dlen;
    int renumber;
    int section;                /* 0 header, 1 body, 2 footer */
} tnl;

static const char *tnl_regmsg(int code) {
    switch (code) {
    case REG_NOMATCH: return "No match";
    case REG_BADPAT: return "Invalid regular expression";
    case REG_ECOLLATE: return "Invalid collation character";
    case REG_ECTYPE: return "Invalid character class name";
    case REG_EESCAPE: return "Trailing backslash";
    case REG_ESUBREG: return "Invalid back reference";
    case REG_EBRACK: return "Unmatched [, [^, [:, [., or [=";
    case REG_EPAREN: return "Unmatched ( or \\(";
    case REG_EBRACE: return "Unmatched \\{";
    case REG_BADBR: return "Invalid content of \\{\\}";
    case REG_ERANGE: return "Invalid range end";
    case REG_ESPACE: return "Memory exhausted";
    case REG_BADRPT: return "Invalid preceding regular expression";
#ifdef REG_EEND
    case REG_EEND: return "Premature end of regular expression";
#endif
#ifdef REG_ESIZE
    case REG_ESIZE: return "Regular expression too big";
#endif
#ifdef REG_ERPAREN
    case REG_ERPAREN: return "Unmatched ) or \\)";
#endif
    }
    return "Invalid regular expression";
}

/* What GNU's regex says of a few malformed BREs that glibc's regcomp or
 * libtoby's words differently or accepts (measured with nl -bp): a '['
 * (or "[^") ending the pattern is "Invalid regular expression", a
 * bracket never closed "Unmatched [, [^, [:, [., or [=", a \) with no \(
 * open "Unmatched ) or \)", an interval \{ after an atom never closed by
 * \} "Unmatched \{", one holding anything but M, M, or M,N (M <= N)
 * "Invalid content of \{\}", a count past 32767 "Regular expression too
 * big". A \{ with no atom before it is a character. NULL when none of
 * these is in P; regcomp has the last word. */
static const char *tnl_prescan(const char *p) {
    int depth = 0, atom = 0;
    while (*p) {
        if (*p == '[') {
            /* a bracket expression: ']' first is a member */
            const char *q = p + 1;
            if (*q == '^') q++;
            if (!*q) return "Invalid regular expression";
            if (*q == ']') q++;
            while (*q && *q != ']') {
                if (*q == '[' && (q[1] == ':' || q[1] == '.' || q[1] == '=')) {
                    char k = q[1];
                    q += 2;
                    while (*q && !(q[0] == k && q[1] == ']')) q++;
                    if (!*q) break;
                    q += 2;
                    continue;
                }
                q++;
            }
            if (!*q) return "Unmatched [, [^, [:, [., or [=";
            p = q + 1;
            atom = 1;
            continue;
        }
        if (*p == '\\' && p[1]) {
            char e = p[1];
            p += 2;
            if (e == '(') { depth++; atom = 0; continue; }
            if (e == '|') { atom = 0; continue; }
            if (e == ')') {
                if (--depth < 0) return "Unmatched ) or \\)";
                atom = 1;
                continue;
            }
            if (e == '{' && atom) {
                const char *q = p;
                uintmax_t lo = 0, hi = 0;
                int dlo = 0, dhi = 0, comma = 0;
                for (; isdigit((unsigned char)*q); q++, dlo = 1)
                    if (lo < 100000) lo = lo * 10 + (uintmax_t)(*q - '0');
                if (*q == ',') {
                    comma = 1;
                    for (q++; isdigit((unsigned char)*q); q++, dhi = 1)
                        if (hi < 100000) hi = hi * 10 + (uintmax_t)(*q - '0');
                }
                if (q[0] == '\\' && q[1] == '}') {
                    if ((!dlo && !comma) || (comma && dhi && lo > hi))
                        return "Invalid content of \\{\\}";
                    if (lo > 32767 || hi > 32767) return "Regular expression too big";
                    p = q + 2;
                    continue;
                }
                return strstr(q, "\\}") ? "Invalid content of \\{\\}" : "Unmatched \\{";
            }
            atom = 1;
            continue;
        }
        /* a leading ^ anchors; anything else is an atom */
        if (!(*p == '^' && !atom)) atom = 1;
        p++;
    }
    return 0;
}

/* STYLE for section S; 0, or -1 when it is not one */
static int tnl_style(int s, const char *arg) {
    struct tnl_style *st = &tnl.st[s];
    /* only the first character is looked at: "ax" is a */
    if (arg[0] == 'a' || arg[0] == 't' || arg[0] == 'n') {
        st->kind = arg[0];
        return 0;
    }
    if (arg[0] != 'p') return -1;
    if (st->has_re) { regfree(&st->re); st->has_re = 0; }
    const char *bad = tnl_prescan(arg + 1);
    if (bad) su_error(1, 0, "%s", bad);
    int rc = regcomp(&st->re, arg + 1, REG_NOSUB);
    if (rc) su_error(1, 0, "%s", tnl_regmsg(rc));
    st->has_re = 1;
    st->kind = 'p';
    return 0;
}

static int tnl_match(const regex_t *re, const char *s, size_t n) {
#ifdef REG_STARTEND
    regmatch_t m[1];
    m[0].rm_so = 0;
    m[0].rm_eo = (regoff_t)n;
    return regexec(re, s, 1, m, REG_STARTEND) == 0;
#else
    /* the pieces between NUL bytes, each searched */
    size_t off = 0;
    for (;;) {
        size_t len = strlen(s + off);
        int fl = (off ? REG_NOTBOL : 0) | (off + len < n ? REG_NOTEOL : 0);
        if (regexec(re, s + off, 0, 0, fl) == 0) return 1;
        off += len + 1;
        if (off > n) return 0;
    }
#endif
}

/* ARG as a decimal intmax_t: leading blanks, a sign; 0 when it is one,
 * else ERANGE/EOVERFLOW-style codes: -1 invalid, 1 too big or small */
static int tnl_num(const char *arg, intmax_t *out) {
    const char *p = arg;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    int neg = 0;
    if (*p == '-' || *p == '+') { neg = *p == '-'; p++; }
    if (!isdigit((unsigned char)*p)) return -1;
    uintmax_t v = 0;
    int over = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINTMAX_MAX - d) / 10) over = 1;
        else v = v * 10 + d;
    }
    if (*p) return -1;
    if (over) return 1;
    if (neg) {
        if (v > (uintmax_t)INTMAX_MAX + 1) return 1;
        *out = v == (uintmax_t)INTMAX_MAX + 1 ? INTMAX_MIN : -(intmax_t)v;
    } else {
        if (v > (uintmax_t)INTMAX_MAX) return 1;
        *out = (intmax_t)v;
    }
    return 0;
}

static intmax_t tnl_intopt(const char *arg, const char *what) {
    intmax_t v = 0;
    int rc = tnl_num(arg, &v);
    if (rc < 0) su_error(1, 0, "%s: %s", what, su_quote(arg));
    if (rc > 0) su_error(1, EOVERFLOW, "%s: %s", what, su_quote(arg));
    return v;
}

static void tnl_pad(size_t n) {
    while (n--) putchar(' ');
}

static void tnl_number(void) {
    if (tnl.overflowed) su_error(1, 0, "line number overflow");
    char digits[32];
    uintmax_t mag = tnl.line < 0 ? (uintmax_t)0 - (uintmax_t)tnl.line : (uintmax_t)tnl.line;
    int nd = 0;
    do { digits[nd++] = (char)('0' + mag % 10); mag /= 10; } while (mag);
    size_t len = (size_t)nd + (tnl.line < 0);
    size_t w = (size_t)tnl.width;
    if (tnl.fmt == 'r') tnl_pad(len < w ? w - len : 0);
    if (tnl.line < 0) putchar('-');
    if (tnl.fmt == 'z') for (size_t i = len; i < w; i++) putchar('0');
    while (nd) putchar(digits[--nd]);
    if (tnl.fmt == 'l') tnl_pad(len < w ? w - len : 0);
    fwrite(tnl.sep, 1, tnl.seplen, stdout);
    /* the next number; going past the ends is reported when it is used */
    intmax_t next;
    if (__builtin_add_overflow(tnl.line, tnl.incr, &next)) tnl.overflowed = 1;
    else tnl.line = next;
}

static void tnl_line(const char *s, size_t n) {
    if (tnl.delim && n && n % tnl.dlen == 0 && n / tnl.dlen <= 3) {
        size_t k = n / tnl.dlen, i;
        for (i = 0; i < k; i++)
            if (memcmp(s + i * tnl.dlen, tnl.delim, tnl.dlen)) break;
        if (i == k) {
            tnl.section = k == 3 ? 0 : k == 2 ? 1 : 2;
            if (tnl.renumber) { tnl.line = tnl.start; tnl.overflowed = 0; }
            putchar('\n');
            return;
        }
    }
    const struct tnl_style *st = &tnl.st[tnl.section];
    int num = 0;
    switch (st->kind) {
    case 'a':
        if (n == 0) {
            if (++tnl.blanks >= tnl.blank_join) { num = 1; tnl.blanks = 0; }
        } else {
            num = 1;
            tnl.blanks = 0;
        }
        break;
    case 't': num = n > 0; break;
    case 'p': num = tnl_match(&st->re, s, n); break;
    }
    if (num) tnl_number();
    else tnl_pad((size_t)tnl.width + tnl.seplen);
    fwrite(s, 1, n, stdout);
    putchar('\n');
}

/* one file: 0, or 1 when it could not be read */
static int tnl_file(const char *name, char **buf, size_t *cap) {
    int fd = 0;
    if (strcmp(name, "-")) {
        fd = su_open_read(name, 0);
        if (fd < 0) { su_error(0, errno, "%s", su_quotef(name)); return 1; }
    }
    static unsigned char ib[16384];
    size_t n = 0;
    int rc = 0, partial = 0;
    for (;;) {
        ssize_t r = read(fd, ib, sizeof ib);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { su_error(0, errno, "%s", su_quotef(name)); rc = 1; break; }
        if (r == 0) break;
        size_t k = 0;
        while (k < (size_t)r) {
            unsigned char *q = (unsigned char *)memchr(ib + k, '\n', (size_t)r - k);
            size_t take = q ? (size_t)(q - (ib + k)) : (size_t)r - k;
            if (n + take + 1 > *cap) {
                size_t nc = *cap ? *cap : 256;
                while (nc < n + take + 1) nc *= 2;
                char *nb = (char *)realloc(*buf, nc);
                if (!nb) su_error(1, ENOMEM, "memory exhausted");
                *buf = nb;
                *cap = nc;
            }
            memcpy(*buf + n, ib + k, take);
            n += take;
            k += take;
            partial = 1;
            if (q) {
                (*buf)[n] = 0;
                tnl_line(*buf, n);
                n = 0;
                partial = 0;
                k++;
            }
        }
    }
    if (partial) {
        (*buf)[n] = 0;
        tnl_line(*buf, n);
    }
    if (fd > 0) close(fd);
    return rc;
}

static int do_nl(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "header-numbering", SU_REQ_ARG, 0, 'h' },
        { "body-numbering", SU_REQ_ARG, 0, 'b' },
        { "footer-numbering", SU_REQ_ARG, 0, 'f' },
        { "starting-line-number", SU_REQ_ARG, 0, 'v' },
        { "line-increment", SU_REQ_ARG, 0, 'i' },
        { "no-renumber", SU_NO_ARG, 0, 'p' },
        { "join-blank-lines", SU_REQ_ARG, 0, 'l' },
        { "number-separator", SU_REQ_ARG, 0, 's' },
        { "number-width", SU_REQ_ARG, 0, 'w' },
        { "number-format", SU_REQ_ARG, 0, 'n' },
        { "section-delimiter", SU_REQ_ARG, 0, 'd' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&tnl, 0, sizeof tnl);
    tnl.st[0].kind = 'n';
    tnl.st[1].kind = 't';
    tnl.st[2].kind = 'n';
    tnl.start = 1;
    tnl.incr = 1;
    tnl.blank_join = 1;
    tnl.width = 6;
    tnl.fmt = 'r';
    tnl.sep = "\t";
    tnl.seplen = 1;
    tnl.renumber = 1;
    static char dflt[] = "\\:";
    tnl.delim = dflt;
    tnl.dlen = 2;
    int bad = 0, c;
    while ((c = su_getopt_long(argc, argv, "h:b:f:v:i:pl:s:w:n:d:", longs, 0)) != -1) {
        switch (c) {
        case 'h':
            if (tnl_style(0, su_optarg)) { su_error(0, 0, "invalid header numbering style: %s", su_quote(su_optarg)); bad = 1; }
            break;
        case 'b':
            if (tnl_style(1, su_optarg)) { su_error(0, 0, "invalid body numbering style: %s", su_quote(su_optarg)); bad = 1; }
            break;
        case 'f':
            if (tnl_style(2, su_optarg)) { su_error(0, 0, "invalid footer numbering style: %s", su_quote(su_optarg)); bad = 1; }
            break;
        case 'v': tnl.start = tnl_intopt(su_optarg, "invalid starting line number"); break;
        case 'i': tnl.incr = tnl_intopt(su_optarg, "invalid line number increment"); break;
        case 'p': tnl.renumber = 0; break;
        case 'l': {
            const char *p = su_optarg;
            while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
            int neg = *p == '-';
            if (*p == '-' || *p == '+') p++;
            if (!isdigit((unsigned char)*p) || p[strspn(p, "0123456789")])
                su_error(1, 0, "invalid line number of blank lines: %s", su_quote(su_optarg));
            uintmax_t v = 0;
            for (; isdigit((unsigned char)*p); p++) {
                unsigned d = (unsigned)(*p - '0');
                v = v > (UINTMAX_MAX - d) / 10 ? UINTMAX_MAX : v * 10 + d;
            }
            if (neg && v)
                su_error(1, ERANGE, "invalid line number of blank lines: %s", su_quote(su_optarg));
            tnl.blank_join = v;
            break;
        }
        case 's':
            tnl.sep = su_optarg;
            tnl.seplen = strlen(su_optarg);
            break;
        case 'w': {
            intmax_t v = 0;
            int rc = tnl_num(su_optarg, &v);
            if (rc < 0) su_error(1, 0, "invalid line number field width: %s", su_quote(su_optarg));
            if (rc > 0 && su_optarg[strspn(su_optarg, " \t\n\v\f\r")] == '-') v = -1;
            else if (rc > 0 || v > INT_MAX)
                su_error(1, EOVERFLOW, "invalid line number field width: %s", su_quote(su_optarg));
            if (v < 1) su_error(1, ERANGE, "invalid line number field width: %s", su_quote(su_optarg));
            tnl.width = (int)v;
            break;
        }
        case 'n':
            if (!strcmp(su_optarg, "ln")) tnl.fmt = 'l';
            else if (!strcmp(su_optarg, "rn")) tnl.fmt = 'r';
            else if (!strcmp(su_optarg, "rz")) tnl.fmt = 'z';
            else { su_error(0, 0, "invalid line numbering format: %s", su_quote(su_optarg)); bad = 1; }
            break;
        case 'd': {
            size_t n = strlen(su_optarg);
            if (n == 0) { tnl.delim = 0; tnl.dlen = 0; }
            else if (n == 1) {
                static char two[3];
                two[0] = su_optarg[0];
                two[1] = ':';
                two[2] = 0;
                tnl.delim = two;
                tnl.dlen = 2;
            } else { tnl.delim = su_optarg; tnl.dlen = n; }
            break;
        }
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n", su_name);
            printf("Write each FILE to standard output with numbers put before its lines.\n"
                   "\n"
                   "With no FILE, or when FILE is -, read standard input.\n"
                   "\n"
                   "A long option's argument is required by its short form too.\n"
                   "  -b, --body-numbering=STYLE      how body lines are numbered\n"
                   "  -d, --section-delimiter=CC      the characters that mark sections\n"
                   "  -f, --footer-numbering=STYLE    how footer lines are numbered\n"
                   "  -h, --header-numbering=STYLE    how header lines are numbered\n"
                   "  -i, --line-increment=NUMBER     add NUMBER to the number after each line\n"
                   "  -l, --join-blank-lines=NUMBER   under style a, number only every\n"
                   "                                    NUMBERth of a run of empty lines\n"
                   "  -n, --number-format=FORMAT      how numbers are laid out\n"
                   "  -p, --no-renumber               keep counting across sections\n"
                   "  -s, --number-separator=STRING   write STRING after each number\n"
                   "  -v, --starting-line-number=NUMBER  the first number of each section\n"
                   "  -w, --number-width=NUMBER       numbers take NUMBER columns\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "The defaults are -bt -d'\\:' -fn -hn -i1 -l1 -n'rn' -s<TAB> -v1 -w6.\n"
                   "\n"
                   "A line of the two delimiter characters three times starts a header,\n"
                   "twice a body, once a footer. One character given means it and ':';\n"
                   "more than two are used as they are; -d '' turns sections off.\n"
                   "\n"
                   "STYLE: a numbers every line, t the non-empty ones, n none, and pBRE the\n"
                   "lines that the basic regular expression BRE matches.\n"
                   "\n"
                   "FORMAT: ln left aligned, rn right aligned, rz right aligned with zeros.\n");
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (bad) su_try_help(1);
    tnl.line = tnl.start;
    tnl.section = 1;
    char *buf = 0;
    size_t cap = 0;
    int rc = 0;
    if (su_optind >= argc) rc = tnl_file("-", &buf, &cap);
    for (int i = su_optind; i < argc; i++) rc |= tnl_file(argv[i], &buf, &cap);
    free(buf);
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return rc;
}
