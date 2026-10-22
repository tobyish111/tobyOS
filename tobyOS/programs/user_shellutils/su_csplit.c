/* su_csplit.c -- csplit, as GNU coreutils 9.7's (logs/utilgate/staging/
 * csplit.sh): FILE (or standard input) cut into xx00, xx01 ... at the
 * lines PATTERNs name, each piece's size in bytes written as it closes.
 * Measured against gnucsplit in WSL, in the C locale.
 *
 * PATTERNS. N copies up to (not including) line N, counted from the start
 * of the input; a line already passed makes an empty piece, and N past the
 * last line is "line number out of range" after the rest has been written
 * to the piece. /RE/[OFFSET] copies up to the matching line plus OFFSET,
 * %RE%[OFFSET] skips to it; a split may land one past the last line, not
 * further, nor before the piece's first line. {N} repeats the previous
 * pattern N more times -- a line number repeats as N, 2N, 3N -- and {*}
 * until a search finds nothing, which then ends the whole run quietly:
 * the piece being copied keeps the rest, nothing is written after it,
 * later patterns are ignored. Without {*} a failed search is "match not
 * found", the rest of the input written to the piece first.
 *
 * WHERE A SEARCH STARTS: after the last line already looked at -- the
 * last match, the line a positive offset landed on, the last line of a
 * piece copied -- so /15/-10 /9/ finds 19, /3/+5 /8/ finds 18, and 5 /5/
 * finds line 5 itself. --suppress-matched drops the line each split lands
 * on (for %RE% too), including after an empty piece.
 *
 * A LINE NUMBER is out of range when, after its piece is copied, there is
 * no line to start the next one at or after the line past the last one
 * looked at (so /20/ 5 fails on a 20-line file and /19/ 5 does not);
 * with --suppress-matched the line past the last one looked at must exist
 * before the copy instead, and the line split at need not.
 *
 * Every pattern is checked before any output, in order, after the input
 * is opened: line numbers 1..INTMAX_MAX, a line number below the one
 * before it is an error and an equal one a warning, offsets and counts
 * are decimals that fit intmax_t. A regular expression is a GNU basic
 * one, checked here first so that the messages are glibc's whatever the
 * engine (tobyOS's libtoby regcomp or glibc's).
 *
 * FILES. A piece's file is made when its pattern starts (so a directory
 * as input still leaves "0"), -z removes an empty one and does not count
 * it, and on any error every file made is removed unless -k. The name is
 * PREFIX then the number as %0Nd (-n N, 2 by default) or as -b FORMAT,
 * one conversion of d i u o x X with the flags ' - 0 # as printf would. */

#define FM_CS_LINE 0
#define FM_CS_COPY 1
#define FM_CS_SKIP 2

struct fm_cs_pat {
    int kind;
    const char *arg;          /* as given, for messages */
    intmax_t lineno;          /* FM_CS_LINE */
    regex_t re;
    intmax_t offset;
    uintmax_t reps;
    int forever;
};

struct fm_cs_line { char *s; size_t n; int nl; };

static struct {
    /* input */
    int fd;
    unsigned char *rb;
    size_t rpos, rlen;
    int eof;
    struct fm_cs_line *L;
    size_t nlines, cap;
    char *part;               /* an unfinished line being read */
    size_t partn, partcap;
    /* output */
    const char *prefix;
    const char *suffix;       /* -b FORMAT, or 0 */
    int digits;
    int keep, quiet, elide, suppress;
    char **made;
    size_t nmade, madecap;
    int ofd;                  /* -1 when no piece is open */
    char *oname;
    uintmax_t obytes;
    unsigned filenum;
    char ob[8192];
    size_t on;
} fm_cs;

static void fm_cs_nomem(void) { su_error(1, 0, "memory exhausted"); }

/* ---- output files ----------------------------------------------------------- */

/* the number N as a validated -b conversion (flags, width, precision,
 * conversion) would print it */
struct fm_cs_conv {
    int minus, zero, alt;
    long width, prec;         /* -1: none */
    char conv;
};

static size_t fm_cs_fmtnum(char *out, unsigned v, const struct fm_cs_conv *c) {
    char dig[64];
    size_t nd = 0;
    unsigned base = c->conv == 'o' ? 8 : (c->conv == 'x' || c->conv == 'X') ? 16 : 10;
    const char *hx = c->conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned x = v;
    do { dig[nd++] = hx[x % base]; x /= base; } while (x);
    if (c->prec == 0 && v == 0) nd = 0;
    size_t zeros = 0;
    if (c->prec > 0 && (size_t)c->prec > nd) zeros = (size_t)c->prec - nd;
    const char *pfx = "";
    if (c->alt && c->conv == 'o' && zeros == 0 && (nd == 0 || dig[nd - 1] != '0')) zeros = 1;
    if (c->alt && v != 0 && c->conv == 'x') pfx = "0x";
    if (c->alt && v != 0 && c->conv == 'X') pfx = "0X";
    size_t body = strlen(pfx) + zeros + nd;
    size_t pad = (c->width > 0 && (size_t)c->width > body) ? (size_t)c->width - body : 0;
    size_t n = 0;
    int zpad = c->zero && !c->minus && c->prec < 0;
    if (!c->minus && !zpad) { memset(out + n, ' ', pad); n += pad; }
    memcpy(out + n, pfx, strlen(pfx));
    n += strlen(pfx);
    if (zpad) { memset(out + n, '0', pad); n += pad; }
    memset(out + n, '0', zeros);
    n += zeros;
    while (nd) out[n++] = dig[--nd];
    if (c->minus) { memset(out + n, ' ', pad); n += pad; }
    return n;
}

/* -b FORMAT checked as GNU checks it; the conversion is stored in C and
 * the text around it in *PRE / *POST */
static struct fm_cs_conv fm_cs_conv;
static char *fm_cs_pre, *fm_cs_post;

static long fm_cs_digits(const char **pp) {
    const char *p = *pp;
    long v = 0;
    int big = 0;
    while (isdigit((unsigned char)*p)) {
        if (v > (INT_MAX - 9) / 10) big = 1;
        else v = v * 10 + (*p - '0');
        p++;
    }
    *pp = p;
    return big ? (long)INT_MAX + 1 : v;
}

static void fm_cs_suffix_check(const char *f) {
    struct fm_cs_conv c;
    memset(&c, 0, sizeof c);
    c.width = c.prec = -1;
    int count = 0, quote = 0;
    size_t pre_end = 0, post_start = 0;
    const char *p = f;
    while (*p) {
        if (*p != '%') { p++; continue; }
        if (p[1] == '%') { p += 2; continue; }
        if (count) su_error(1, 0, "too many %% conversion specifications in suffix");
        count = 1;
        pre_end = (size_t)(p - f);
        const char *q = p + 1;
        const char *flags = q;
        while (*q == '\'' || *q == '-' || *q == '0' || *q == '#') {
            if (*q == '\'') quote = 1;
            else if (*q == '-') c.minus = 1;
            else if (*q == '0') c.zero = 1;
            else c.alt = 1;
            q++;
        }
        size_t nflags = (size_t)(q - flags);
        if (isdigit((unsigned char)*q)) c.width = fm_cs_digits(&q);
        if (*q == '.') {
            q++;
            c.prec = fm_cs_digits(&q);
        }
        if (!*q) su_error(1, 0, "missing conversion specifier in suffix");
        c.conv = *q;
        if (!strchr("diuoxX", c.conv))
            su_error(1, 0, "invalid conversion specifier in suffix: %c", c.conv);
        if (strchr("diu", c.conv) && c.alt)
            su_error(1, 0, "invalid flags in conversion specification: %%#%c", c.conv);
        if (strchr("oxX", c.conv) && quote)
            su_error(1, 0, "invalid flags in conversion specification: %%'%c", c.conv);
        (void)nflags;
        post_start = (size_t)(q + 1 - f);
        p = q + 1;
    }
    if (!count) su_error(1, 0, "missing %% conversion specification in suffix");
    if (c.width > INT_MAX || c.prec > INT_MAX) fm_cs_nomem();
    if (c.conv == 'i' || c.conv == 'u') c.conv = 'd';
    fm_cs_conv = c;
    /* the literal text, %% halved */
    fm_cs_pre = (char *)malloc(pre_end + 1);
    fm_cs_post = (char *)malloc(strlen(f) - post_start + 1);
    if (!fm_cs_pre || !fm_cs_post) fm_cs_nomem();
    size_t n = 0;
    for (size_t i = 0; i < pre_end; i++) {
        fm_cs_pre[n++] = f[i];
        if (f[i] == '%') i++;
    }
    fm_cs_pre[n] = 0;
    n = 0;
    for (size_t i = post_start; f[i]; i++) {
        fm_cs_post[n++] = f[i];
        if (f[i] == '%') i++;
    }
    fm_cs_post[n] = 0;
}

static char *fm_cs_name(unsigned num) {
    size_t pl = strlen(fm_cs.prefix);
    char *name;
    if (fm_cs.suffix) {
        long w = fm_cs_conv.width > fm_cs_conv.prec ? fm_cs_conv.width : fm_cs_conv.prec;
        size_t room = (size_t)(w > 0 ? w : 0) + 32;
        name = (char *)malloc(pl + strlen(fm_cs_pre) + room + strlen(fm_cs_post) + 1);
        if (!name) fm_cs_nomem();
        size_t n = 0;
        memcpy(name, fm_cs.prefix, pl);
        n = pl;
        memcpy(name + n, fm_cs_pre, strlen(fm_cs_pre));
        n += strlen(fm_cs_pre);
        n += fm_cs_fmtnum(name + n, num, &fm_cs_conv);
        memcpy(name + n, fm_cs_post, strlen(fm_cs_post));
        n += strlen(fm_cs_post);
        name[n] = 0;
    } else {
        struct fm_cs_conv c;
        memset(&c, 0, sizeof c);
        c.zero = 1;
        c.width = fm_cs.digits;
        c.prec = -1;
        c.conv = 'd';
        name = (char *)malloc(pl + (size_t)fm_cs.digits + 32);
        if (!name) fm_cs_nomem();
        memcpy(name, fm_cs.prefix, pl);
        size_t n = pl + fm_cs_fmtnum(name + pl, num, &c);
        name[n] = 0;
    }
    return name;
}

static void fm_cs_oflush(void) {
    size_t off = 0;
    while (off < fm_cs.on) {
        ssize_t w = write(fm_cs.ofd, fm_cs.ob + off, fm_cs.on - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) su_error(1, errno, "write error for %s", su_quoteaf(fm_cs.oname));
        off += (size_t)w;
    }
    fm_cs.on = 0;
}

static void fm_cs_owrite(const char *p, size_t n) {
    fm_cs.obytes += n;
    while (n) {
        if (fm_cs.on == sizeof fm_cs.ob) fm_cs_oflush();
        size_t k = sizeof fm_cs.ob - fm_cs.on;
        if (k > n) k = n;
        memcpy(fm_cs.ob + fm_cs.on, p, k);
        fm_cs.on += k;
        p += k;
        n -= k;
    }
}

static void fm_cs_cleanup(void);

static void fm_cs_open(void) {
    char *name = fm_cs_name(fm_cs.filenum);
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        int e = errno;
        su_error(0, e, "%s", su_quotef(name));
        fm_cs_cleanup();
        exit(1);
    }
    if (fm_cs.nmade == fm_cs.madecap) {
        fm_cs.madecap = fm_cs.madecap ? fm_cs.madecap * 2 : 16;
        fm_cs.made = (char **)realloc(fm_cs.made, fm_cs.madecap * sizeof *fm_cs.made);
        if (!fm_cs.made) fm_cs_nomem();
    }
    fm_cs.made[fm_cs.nmade++] = name;
    fm_cs.ofd = fd;
    fm_cs.oname = name;
    fm_cs.obytes = 0;
    fm_cs.on = 0;
}

static void fm_cs_close(void) {
    if (fm_cs.ofd < 0) return;
    fm_cs_oflush();
    close(fm_cs.ofd);
    fm_cs.ofd = -1;
    if (fm_cs.elide && fm_cs.obytes == 0) {
        /* -z: the empty piece goes, and its number is used again */
        unlink(fm_cs.oname);
        free(fm_cs.oname);
        fm_cs.nmade--;
    } else {
        if (!fm_cs.quiet) printf("%ju\n", fm_cs.obytes);
        fm_cs.filenum++;
    }
    fm_cs.oname = 0;
}

/* an error: the open piece closed, every piece removed unless -k */
static void fm_cs_cleanup(void) {
    fm_cs_close();
    if (!fm_cs.keep)
        for (size_t i = 0; i < fm_cs.nmade; i++) unlink(fm_cs.made[i]);
}

static void fm_cs_die(void) __attribute__((noreturn));
static void fm_cs_die(void) {
    fm_cs_cleanup();
    fflush(stdout);
    exit(1);
}

/* ---- input ------------------------------------------------------------------ */

static void fm_cs_addline(const char *s, size_t n, int nl) {
    if (fm_cs.nlines == fm_cs.cap) {
        fm_cs.cap = fm_cs.cap ? fm_cs.cap * 2 : 1024;
        fm_cs.L = (struct fm_cs_line *)realloc(fm_cs.L, fm_cs.cap * sizeof *fm_cs.L);
        if (!fm_cs.L) fm_cs_nomem();
    }
    char *b = (char *)malloc(n + 1);
    if (!b) fm_cs_nomem();
    memcpy(b, s, n);
    b[n] = 0;
    fm_cs.L[fm_cs.nlines].s = b;
    fm_cs.L[fm_cs.nlines].n = n;
    fm_cs.L[fm_cs.nlines].nl = nl;
    fm_cs.nlines++;
}

static void fm_cs_partadd(const unsigned char *p, size_t n) {
    if (fm_cs.partn + n > fm_cs.partcap) {
        size_t c = fm_cs.partcap ? fm_cs.partcap : 256;
        while (c < fm_cs.partn + n) c *= 2;
        fm_cs.part = (char *)realloc(fm_cs.part, c);
        if (!fm_cs.part) fm_cs_nomem();
        fm_cs.partcap = c;
    }
    memcpy(fm_cs.part + fm_cs.partn, p, n);
    fm_cs.partn += n;
}

/* read until there are N lines, or the end: whether line N exists */
static int fm_cs_have(uintmax_t n) {
    if (!fm_cs.rb) {
        fm_cs.rb = (unsigned char *)malloc(65536);
        if (!fm_cs.rb) fm_cs_nomem();
    }
    while (fm_cs.nlines < n && !fm_cs.eof) {
        if (fm_cs.rpos >= fm_cs.rlen) {
            ssize_t r = read(fm_cs.fd, fm_cs.rb, 65536);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) {
                su_error(0, errno, "read error");
                fm_cs_die();
            }
            if (r == 0) {
                fm_cs.eof = 1;
                if (fm_cs.partn) { fm_cs_addline(fm_cs.part, fm_cs.partn, 0); fm_cs.partn = 0; }
                break;
            }
            fm_cs.rpos = 0;
            fm_cs.rlen = (size_t)r;
        }
        unsigned char *s = fm_cs.rb + fm_cs.rpos;
        size_t avail = fm_cs.rlen - fm_cs.rpos;
        unsigned char *nl = (unsigned char *)memchr(s, '\n', avail);
        if (!nl) { fm_cs_partadd(s, avail); fm_cs.rpos = fm_cs.rlen; continue; }
        size_t k = (size_t)(nl - s);
        if (fm_cs.partn) {
            fm_cs_partadd(s, k);
            fm_cs_addline(fm_cs.part, fm_cs.partn, 1);
            fm_cs.partn = 0;
        } else {
            fm_cs_addline((const char *)s, k, 1);
        }
        fm_cs.rpos += k + 1;
    }
    return n <= fm_cs.nlines;
}

/* lines FROM..TO-1 (1-based) to the open piece; TO 0 is "to the end" */
static void fm_cs_copy(uintmax_t from, uintmax_t to) {
    for (uintmax_t i = from; to == 0 || i < to; i++) {
        if (!fm_cs_have(i)) break;
        struct fm_cs_line *l = &fm_cs.L[i - 1];
        fm_cs_owrite(l->s, l->n);
        if (l->nl) fm_cs_owrite("\n", 1);
        /* a line written is never looked at again */
        free(l->s);
        l->s = 0;
    }
}

static void fm_cs_drop(uintmax_t from, uintmax_t to) {
    for (uintmax_t i = from; to == 0 || i < to; i++) {
        if (!fm_cs_have(i)) break;
        free(fm_cs.L[i - 1].s);
        fm_cs.L[i - 1].s = 0;
    }
}

/* ---- patterns --------------------------------------------------------------- */

/* glibc's messages for its regcomp codes */
static const char *fm_cs_rxmsg(int code) {
    switch (code) {
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
    default: return "Invalid regular expression";
    }
}

static const char *const fm_cs_classes[] = {
    "alpha", "upper", "lower", "digit", "xdigit", "space", "print",
    "punct", "graph", "cntrl", "blank", "alnum", 0
};

/* RE (LEN bytes) read as a GNU basic regular expression: 0, or glibc's
 * message for the first thing wrong with it */
static const char *fm_cs_recheck(const char *re, size_t len) {
    const char *p = re, *e = re + len;
    int depth = 0, opened = 0;
    int closed[10] = { 0 };
    int stack[256];
    int start = 1;                /* where * and \{ are ordinary */
    while (p < e) {
        if (*p == '[') {
            p++;
            if (p < e && *p == '^') p++;
            if (p == e) return "Invalid regular expression";
            if (*p == ']') p++;
            for (;;) {
                if (p >= e) return "Unmatched [, [^, [:, [., or [=";
                if (*p == ']') { p++; break; }
                if (*p == '[' && p + 1 < e && (p[1] == ':' || p[1] == '.' || p[1] == '=')) {
                    char kind = p[1];
                    const char *q = p + 2, *end = 0;
                    for (const char *r = q; r + 1 < e; r++)
                        if (r[0] == kind && r[1] == ']') { end = r; break; }
                    if (!end) return "Unmatched [, [^, [:, [., or [=";
                    size_t n = (size_t)(end - q);
                    if (kind == ':') {
                        int ok = 0;
                        for (int k = 0; fm_cs_classes[k]; k++)
                            if (strlen(fm_cs_classes[k]) == n && !strncmp(fm_cs_classes[k], q, n)) ok = 1;
                        if (!ok) return "Invalid character class name";
                    } else if (n != 1) {
                        return "Invalid collation character";
                    }
                    p = end + 2;
                    continue;
                }
                p++;
            }
            start = 0;
            continue;
        }
        if (*p != '\\') {
            if (*p == '*' && start) { p++; start = 0; continue; }
            if (*p == '^' && start) { p++; continue; }
            p++;
            start = 0;
            continue;
        }
        if (p + 1 >= e) return "Trailing backslash";
        char c = p[1];
        p += 2;
        if (c == '(') {
            if (depth >= 256) return "Regular expression too big";
            stack[depth++] = ++opened;
            start = 1;
            continue;
        }
        if (c == ')') {
            if (!depth) return "Unmatched ) or \\)";
            int g = stack[--depth];
            if (g < 10) closed[g] = 1;
            start = 0;
            continue;
        }
        if (c == '|') { start = 1; continue; }
        if (c >= '1' && c <= '9') {
            if (!closed[c - '0']) return "Invalid back reference";
            start = 0;
            continue;
        }
        if (c == '{' && !start) {
            /* an interval: [MIN][,[MAX]]\} */
            long mn = -1, mx = -1;
            int comma = 0, big = 0;
            if (p < e && isdigit((unsigned char)*p)) {
                mn = 0;
                while (p < e && isdigit((unsigned char)*p)) {
                    if (mn > 100000) big = 1;
                    else mn = mn * 10 + (*p - '0');
                    p++;
                }
            }
            if (p < e && *p == ',') {
                comma = 1;
                p++;
                if (p < e && isdigit((unsigned char)*p)) {
                    mx = 0;
                    while (p < e && isdigit((unsigned char)*p)) {
                        if (mx > 100000) big = 1;
                        else mx = mx * 10 + (*p - '0');
                        p++;
                    }
                }
            }
            if (p >= e || (*p == '\\' && p + 1 >= e)) return "Unmatched \\{";
            if (!(*p == '\\' && p[1] == '}')) return "Invalid content of \\{\\}";
            p += 2;
            if (mn < 0 && !comma) return "Invalid content of \\{\\}";
            if (!comma) mx = mn;
            if (mn >= 0 && mx >= 0 && mn > mx && !big) return "Invalid content of \\{\\}";
            if (big || mn > 0x7fff || mx > 0x7fff) return "Regular expression too big";
            start = 0;
            continue;
        }
        start = 0;
    }
    if (depth) return "Unmatched ( or \\(";
    return 0;
}

/* a decimal that fits intmax_t, as strtoimax reads it (leading blanks, a
 * sign): 0, or -1 */
static int fm_cs_imax(const char *s, intmax_t *out, int allow_minus) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+') p++;
    else if (*p == '-' && allow_minus) { neg = 1; p++; }
    if (!isdigit((unsigned char)*p)) return -1;
    uintmax_t v = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > ((uintmax_t)INTMAX_MAX + 1 - d) / 10) return -1;
        v = v * 10 + d;
    }
    if (*p) return -1;
    if (!neg && v > (uintmax_t)INTMAX_MAX) return -1;
    *out = neg ? (intmax_t)(0 - v) : (intmax_t)v;
    if (neg && v == (uintmax_t)INTMAX_MAX + 1) *out = INTMAX_MIN;
    return 0;
}

static struct fm_cs_pat *fm_cs_pats;
static size_t fm_cs_npats;

static void fm_cs_parse(int n, char **args) {
    fm_cs_pats = (struct fm_cs_pat *)calloc((size_t)n + 1, sizeof *fm_cs_pats);
    if (!fm_cs_pats) fm_cs_nomem();
    int have_last = 0;
    intmax_t last = 0;
    for (int i = 0; i < n; i++) {
        const char *a = args[i];
        size_t len = strlen(a);
        if (a[0] == '{') {
            if (!fm_cs_npats || i == 0 || args[i - 1][0] == '{')
                su_error(1, 0, "%s: invalid pattern", su_quote(a));
            struct fm_cs_pat *p = &fm_cs_pats[fm_cs_npats - 1];
            if (a[len - 1] != '}' || len < 2)
                su_error(1, 0, "%s: '}' is required in repeat count", su_quote(a));
            char *in = (char *)malloc(len);
            if (!in) fm_cs_nomem();
            memcpy(in, a + 1, len - 2);
            in[len - 2] = 0;
            intmax_t v;
            if (!strcmp(in, "*")) p->forever = 1;
            else if (fm_cs_imax(in, &v, 0) == 0) p->reps = (uintmax_t)v;
            else {
                /* the count as typed, its closing brace outside the quotes */
                char *t = (char *)malloc(len);
                if (!t) fm_cs_nomem();
                memcpy(t, a, len - 1);
                t[len - 1] = 0;
                su_error(1, 0, "%s}: integer required between '{' and '}'", su_quote(t));
            }
            free(in);
            continue;
        }
        struct fm_cs_pat *p = &fm_cs_pats[fm_cs_npats++];
        p->arg = a;
        if (a[0] == '/' || a[0] == '%') {
            const char *close = strrchr(a + 1, a[0]);
            if (!close) su_error(1, 0, "%s: closing delimiter '%c' missing", a, a[0]);
            p->kind = a[0] == '/' ? FM_CS_COPY : FM_CS_SKIP;
            size_t rl = (size_t)(close - (a + 1));
            if (close[1]) {
                if (fm_cs_imax(close + 1, &p->offset, 1) != 0)
                    su_error(1, 0, "%s: integer expected after delimiter", su_quote(a));
            }
            const char *msg = fm_cs_recheck(a + 1, rl);
            char *re = (char *)malloc(rl + 1);
            if (!re) fm_cs_nomem();
            memcpy(re, a + 1, rl);
            re[rl] = 0;
            if (!msg) {
                int rc = regcomp(&p->re, re, 0);
                if (rc) msg = fm_cs_rxmsg(rc);
            }
            if (msg) su_error(1, 0, "%s: invalid regular expression: %s", su_quote(a), msg);
            free(re);
            continue;
        }
        p->kind = FM_CS_LINE;
        intmax_t v;
        if (fm_cs_imax(a, &v, 0) != 0) su_error(1, 0, "%s: invalid pattern", su_quote(a));
        if (v == 0) su_error(1, 0, "%s: line number must be greater than zero", a);
        if (have_last && v < last)
            su_error(1, 0, "line number %s is smaller than preceding line number, %jd",
                     su_quote(a), last);
        if (have_last && v == last)
            su_error(0, 0, "warning: line number %s is the same as preceding line number",
                     su_quote(a));
        have_last = 1;
        last = v;
        p->lineno = v;
    }
}

static void fm_cs_fail(const struct fm_cs_pat *p, uintmax_t rep, const char *what) {
    fflush(stdout);
    if (rep) fprintf(stderr, "%s: %s: %s on repetition %ju\n", su_short, su_quote(p->arg), what, rep);
    else fprintf(stderr, "%s: %s: %s\n", su_short, su_quote(p->arg), what);
    fm_cs_die();
}

/* the first line from FROM on that P's expression matches, or 0 -- the
 * whole line, past a NUL, as GNU's search does (REG_STARTEND) */
static uintmax_t fm_cs_search(const struct fm_cs_pat *p, uintmax_t from) {
    for (uintmax_t i = from;; i++) {
        if (!fm_cs_have(i)) return 0;
        regmatch_t m[1];
        m[0].rm_so = 0;
        m[0].rm_eo = (regoff_t)fm_cs.L[i - 1].n;
        if (regexec(&p->re, fm_cs.L[i - 1].s, 1, m, REG_STARTEND) == 0) return i;
    }
}

static int do_csplit(int argc, char **argv) {
    enum { SUPPRESS_OPT = 0x100 };
    static const struct su_option longs[] = {
        { "suffix-format", SU_REQ_ARG, 0, 'b' },
        { "prefix", SU_REQ_ARG, 0, 'f' },
        { "keep-files", SU_NO_ARG, 0, 'k' },
        { "suppress-matched", SU_NO_ARG, 0, SUPPRESS_OPT },
        { "digits", SU_REQ_ARG, 0, 'n' },
        { "quiet", SU_NO_ARG, 0, 's' },
        { "silent", SU_NO_ARG, 0, 's' },
        { "elide-empty-files", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&fm_cs, 0, sizeof fm_cs);
    fm_cs.prefix = "xx";
    fm_cs.digits = 2;
    fm_cs.ofd = -1;
    int c;
    while ((c = su_getopt_long(argc, argv, "b:f:kn:sqz", longs, 0)) != -1) {
        switch (c) {
        case 'b': fm_cs.suffix = su_optarg; break;
        case 'f': fm_cs.prefix = su_optarg; break;
        case 'k': fm_cs.keep = 1; break;
        case SUPPRESS_OPT: fm_cs.suppress = 1; break;
        case 'n': {
            const char *s = su_optarg, *q = s;
            while (isspace((unsigned char)*q)) q++;
            int neg = 0;
            if (*q == '+') q++;
            else if (*q == '-') { neg = 1; q++; }
            if (!isdigit((unsigned char)*q)) su_error(1, 0, "invalid number: %s", su_quote(s));
            uintmax_t v = 0;
            int over = 0;
            for (; isdigit((unsigned char)*q); q++) {
                if (v > (UINTMAX_MAX - 9) / 10) over = 1;
                else v = v * 10 + (uintmax_t)(*q - '0');
            }
            if (*q) su_error(1, 0, "invalid number: %s", su_quote(s));
            if (over || (neg && v) || v > INT_MAX)
                su_error(1, EOVERFLOW, "invalid number: %s", su_quote(s));
            fm_cs.digits = (int)v;
            break;
        }
        case 's': case 'q': fm_cs.quiet = 1; break;
        case 'z': fm_cs.elide = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... FILE PATTERN...\n"
                   "Cut FILE into pieces at the lines PATTERNs name, writing them to the files\n"
                   "'xx00', 'xx01', ..., and write the size in bytes of each piece.\n"
                   "\n"
                   "FILE - is standard input.\n"
                   "\n"
                   "An option's argument is required in its short form too.\n"
                   "  -b, --suffix-format=FORMAT  number the files with printf FORMAT, not %%02d\n"
                   "  -f, --prefix=PREFIX        begin the file names with PREFIX, not 'xx'\n"
                   "  -k, --keep-files           keep the files made when an error stops the run\n"
                   "      --suppress-matched     leave out the line each split lands on\n"
                   "  -n, --digits=DIGITS        number the files with DIGITS digits, not 2\n"
                   "  -s, --quiet, --silent      do not write the sizes of the pieces\n"
                   "  -z, --elide-empty-files    make no file for an empty piece\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "A PATTERN is one of:\n"
                   "  INTEGER            a piece up to, not including, that line\n"
                   "  /REGEXP/[OFFSET]   a piece up to, not including, the next line matching\n"
                   "  %%REGEXP%%[OFFSET]   skip, without a file, to the next line matching\n"
                   "  {INTEGER}          the pattern before it again, INTEGER more times\n"
                   "  {*}                the pattern before it again, until nothing matches\n"
                   "\n"
                   "OFFSET is a whole number of lines, with an optional + or -.\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    int nops = argc - su_optind;
    if (nops < 1) {
        su_error(0, 0, "missing operand");
        su_try_help(1);
    }
    if (nops < 2) {
        su_error(0, 0, "missing operand after %s", su_quote(argv[su_optind]));
        su_try_help(1);
    }
    if (fm_cs.suffix) fm_cs_suffix_check(fm_cs.suffix);
    const char *in = argv[su_optind];
    if (!strcmp(in, "-")) fm_cs.fd = 0;
    else {
        fm_cs.fd = su_open_read(in, 0);
        if (fm_cs.fd < 0) su_error(1, errno, "cannot open %s for reading", su_quoteaf(in));
    }
    fm_cs_parse(nops - 1, argv + su_optind + 1);

    /* CUR is the next line to write; SEEN the last line looked at -- by a
     * search, by a split's offset, by a piece copied or a line dropped.
     * Searches start after SEEN, and a line number's check looks past it. */
    uintmax_t cur = 1, seen = 0;
    for (size_t i = 0; i < fm_cs_npats; i++) {
        struct fm_cs_pat *p = &fm_cs_pats[i];
        for (uintmax_t rep = 0; p->forever || rep <= p->reps; rep++) {
            if (p->kind == FM_CS_LINE) {
                uintmax_t t = (uintmax_t)p->lineno;
                if (rep && t > UINTMAX_MAX / (rep + 1)) t = UINTMAX_MAX;
                else t *= rep + 1;
                fm_cs_open();
                /* with --suppress-matched there must be a line after the
                 * last one looked at; without, after the copy, the line
                 * the next piece starts with (or the one after SEEN) */
                if (fm_cs.suppress && !fm_cs_have(seen + 1)) {
                    fm_cs_close();
                    fm_cs_fail(p, rep, "line number out of range");
                }
                if (t > cur) {
                    if (!fm_cs_have(t - 1)) {
                        fm_cs_copy(cur, 0);
                        fm_cs_close();
                        fm_cs_fail(p, rep, "line number out of range");
                    }
                    fm_cs_copy(cur, t);
                    cur = t;
                    if (t - 1 > seen) seen = t - 1;
                }
                if (!fm_cs.suppress && !fm_cs_have(seen + 1 > t ? seen + 1 : t)) {
                    fm_cs_close();
                    fm_cs_fail(p, rep, "line number out of range");
                }
                fm_cs_close();
            } else {
                int copy = p->kind == FM_CS_COPY;
                if (copy) fm_cs_open();
                uintmax_t m = fm_cs_search(p, seen + 1);
                if (!m) {
                    if (copy) { fm_cs_copy(cur, 0); fm_cs_close(); }
                    else fm_cs_drop(cur, 0);
                    if (p->forever) goto done;
                    fm_cs_fail(p, rep, "match not found");
                }
                seen = m;
                /* where the split lands: M + OFFSET, from CUR to one past
                 * the end; the message names no repetition */
                intmax_t off = p->offset;
                int low = 0, high = 0;
                uintmax_t t = m;
                if (off < 0) {
                    uintmax_t back = (uintmax_t)(-(off + 1)) + 1;
                    if (back >= m || m - back < cur) low = 1;
                    else t = m - back;
                } else if (off > 0) {
                    /* a target past INTMAX_MAX is refused before anything
                     * is written; one past the end after the rest is */
                    if ((uintmax_t)off > (uintmax_t)INTMAX_MAX - m) low = 1;
                    else {
                        t = m + (uintmax_t)off;
                        if (!fm_cs_have(t - 1)) high = 1;
                    }
                }
                if (low) {
                    if (copy) fm_cs_close();
                    fm_cs_fail(p, 0, "line number out of range");
                }
                if (high) {
                    if (copy) { fm_cs_copy(cur, 0); fm_cs_close(); }
                    fm_cs_fail(p, 0, "line number out of range");
                }
                if (copy) { fm_cs_copy(cur, t); fm_cs_close(); }
                else fm_cs_drop(cur, t);
                cur = t;
                /* the line the split lands on has been looked at */
                if (t > seen) seen = fm_cs_have(t) ? t : t - 1;
            }
            if (fm_cs.suppress && fm_cs_have(cur)) {
                fm_cs_drop(cur, cur + 1);
                if (cur > seen) seen = cur;
                cur++;
            }
            if (!p->forever && rep == p->reps) break;
        }
    }
    /* the rest is the last piece */
    fm_cs_open();
    fm_cs_copy(cur, 0);
    fm_cs_close();
done:
    if (fm_cs.fd > 0) close(fm_cs.fd);
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return 0;
}

#undef FM_CS_LINE
#undef FM_CS_COPY
#undef FM_CS_SKIP
