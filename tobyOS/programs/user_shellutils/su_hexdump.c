/* su_hexdump.c -- hexdump and hd, as util-linux 2.41.3's
 * (logs/utilgate/hexdump.sh). Every name here starts hd_.
 *
 * Measured against util-linux's hexdump in WSL; where the manual and the
 * program disagree, the program won, and its bugs are copied:
 *
 * FORMATS. -e FORMAT and -f FILE (a line each, blank lines and #-lines
 * skipped) give format strings; -b -c -C -d -o -x -X are fixed ones, and
 * hd with none of them is -C. Each string is format units: [N][/B] "TEXT",
 * N iterations of B bytes; TEXT is printf's language with _a _A (address,
 * then d o or x), _c _p _u, and _L[...] colour units after a conversion.
 * The input goes in blocks of the largest string's size; a string that is
 * shorter and ends in a unit with a byte count and no iteration count is
 * repeated to fill the block; a unit iterated more than once drops the
 * last character of its text, if blank, on the last iteration. Past the
 * end of the data a conversion prints as many blanks as its width, and a
 * unit with _A in it prints only once, at the very end -- the rest of its
 * string not at all. A block equal to the one before it is "*" (-v: never).
 *
 * The escapes in TEXT (\a \b \f \n \r \t \v \\, any other \X as X) are
 * done in place the way 2.41.3 does them: after the first escape, the
 * plain characters that follow are not moved down, so "a\tbc" is "a", tab,
 * "tb" -- what was there before stays there. Measured, copied.
 *
 * -n and -s take 0x hex, 0 octal or decimal, then K M G T P E Z Y (any
 * case) with iB (1024) or B (1000) or nothing (1024), and a decimal
 * fraction before the suffix that util-linux adds up digit by digit,
 * each digit d at place p as MULT/(10^p/d) in integers (so 1.25K is 1279
 * and 1.10K is 2048); measured, copied. -n 0 does nothing at all; a length
 * past 2^63 fails every read with "Bad address", as util-linux does.
 *
 * -s skips a named regular file by its size, seeks anything else
 * (failing on a pipe), and seeks stdin absolutely. "-" is a file named
 * "-". A file that will not open is reported, the exit status 1; a read
 * error (a directory) is reported and does not change it.
 *
 * -L/--color[=auto|never|always]: auto colours a terminal only (and not
 * with NO_COLOR set). A colour unit is [!]NAME[:VALUE][@START[-END]];
 * which units an offset range selects when a unit is more than one byte
 * wide is not what the manual suggests, and the rule in hd_in_range was
 * fitted to 508 measured cases (every start 0-9, ends start-2..start+9,
 * units of 1 2 4 8 bytes) and agrees with all of them. One unknown name
 * or malformed unit and the whole list is ignored, as util-linux does.
 *
 * RELIES ON su_num.c: su_ldfmt and struct su_fmtspec print %e %f %g
 * exactly as glibc does (the guest has no SSE, and libtoby's float printf
 * rounds through double arithmetic). */

/* ---- output ---------------------------------------------------------------- */

static void hd_out(const char *s, size_t n) { if (n) fwrite(s, 1, n, stdout); }
static void hd_outs(const char *s) { hd_out(s, strlen(s)); }
static void hd_spaces(long n) { while (n-- > 0) putchar(' '); }

/* ---- format units ---------------------------------------------------------- */

enum {
    HD_TEXT,        /* literal text only */
    HD_ADDR,        /* _a: the address of the next byte */
    HD_ADDR_END,    /* _A: the address after the last byte, once, at the end */
    HD_NONE,        /* _a or _A with nothing after it: prints nothing */
    HD_INT,         /* d i */
    HD_UINT,        /* o u x X */
    HD_CHAR,        /* c */
    HD_STR,         /* s */
    HD_DBL,         /* e E f g G */
    HD_C, HD_P, HD_U
};

struct hd_color {
    int invert;
    const char *code;       /* the SGR parameters, "31" */
    int has_val, is_str;
    uint64_t val;
    char *str;
    size_t slen;
    int has_off, lone;
    long long a, e;         /* the offset range */
};

struct hd_pr {
    int kind;
    char *text;             /* the text before the conversion (all of it for HD_TEXT) */
    int left, plus, space, alt, zero;
    long width, prec;       /* prec -1: none */
    char conv;
    int bcnt;
    struct hd_color *colors;
    int ncolors;
};

struct hd_fu {
    int reps, bcnt, setrep, ignore;
    char *fmt;
    struct hd_pr *prs;
    int npr;
    int nospace;            /* drop the last character of the last PR's text on the last rep */
};

struct hd_fs {
    struct hd_fu *fus;
    int nfu;
    int bcnt;
};

static struct hd_fs *hd_fss;
static int hd_nfs;
static struct hd_fu *hd_endfu;
static int hd_blocksize;
static int hd_color_on;

static int hd_digit(int c, int base) {
    int d = c >= '0' && c <= '9' ? c - '0'
          : c >= 'a' && c <= 'f' ? c - 'a' + 10
          : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
    return d < base ? d : -1;
}

static void *hd_xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) su_error(1, ENOMEM, "cannot allocate %lu bytes", (unsigned long)n);
    return q;
}

static char *hd_strndup(const char *s, size_t n) {
    char *d = (char *)hd_xrealloc(0, n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static void hd_badfmt(const char *s) __attribute__((noreturn));
static void hd_badfmt(const char *s) {
    su_error(1, 0, "bad format {%s}", s);
    exit(1);
}

/* The escapes, in place, as 2.41.3 does them: an escape is written where
 * the write position is, but a plain character is never copied down --
 * the write position just moves past whatever was already there. */
static void hd_escape(char *s) {
    size_t r = 0, w = 0;
    while (s[r]) {
        if (s[r] == '\\') {
            char c = s[r + 1];
            if (!c) { s[w] = 0; return; }
            switch (c) {
            case 'a': c = '\a'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'v': c = '\v'; break;
            default: break;
            }
            s[w++] = c;
            r += 2;
        } else {
            w++;
            r++;
        }
    }
    s[w] = 0;
}

/* one format string: its units, syntax checked now (the conversions are
 * looked at once every string is in) */
static void hd_add(const char *src) {
    struct hd_fs fs = { 0, 0, 0 };
    const char *p = src;
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        struct hd_fu fu;
        memset(&fu, 0, sizeof fu);
        fu.reps = 1;
        if (isdigit((unsigned char)*p)) {
            const char *s = p;
            while (isdigit((unsigned char)*p)) p++;
            if (!isspace((unsigned char)*p) && *p != '/') hd_badfmt(src);
            fu.reps = atoi(s);
            fu.setrep = 1;
            for (p++; isspace((unsigned char)*p); p++) {}
        }
        if (*p == '/') {
            p++;
            while (isspace((unsigned char)*p)) p++;
        }
        if (isdigit((unsigned char)*p)) {
            const char *s = p;
            while (isdigit((unsigned char)*p)) p++;
            if (!isspace((unsigned char)*p)) hd_badfmt(src);
            fu.bcnt = atoi(s);
            for (p++; isspace((unsigned char)*p); p++) {}
        }
        if (*p != '"') hd_badfmt(src);
        const char *s = ++p;
        while (*p != '"') if (!*p++) hd_badfmt(src);
        fu.fmt = hd_strndup(s, (size_t)(p - s));
        hd_escape(fu.fmt);
        p++;
        fs.fus = (struct hd_fu *)hd_xrealloc(fs.fus, (size_t)(fs.nfu + 1) * sizeof *fs.fus);
        fs.fus[fs.nfu++] = fu;
    }
    hd_fss = (struct hd_fs *)hd_xrealloc(hd_fss, (size_t)(hd_nfs + 1) * sizeof *hd_fss);
    hd_fss[hd_nfs++] = fs;
}

/* the first pass: a % with nothing after its flags is a "bad format" --
 * found in every string before any conversion is judged */
static void hd_precheck(void) {
    for (int i = 0; i < hd_nfs; i++)
        for (int j = 0; j < hd_fss[i].nfu; j++) {
            const char *f = hd_fss[i].fus[j].fmt;
            for (const char *p = f; *p; p++) {
                if (*p != '%') continue;
                p++;
                while (*p && strchr("#-+ 0123456789", *p)) p++;
                if (*p == '.') {
                    p++;
                    while (isdigit((unsigned char)*p)) p++;
                }
                if (!*p) hd_badfmt(f);
                if (*p == '_') {
                    if (!p[1]) break;
                    p++;
                }
            }
        }
}

/* ---- colour units -------------------------------------------------------------- */

static const char *hd_color_code(const char *name, size_t n) {
    static const char *const tab[][2] = {
        { "black", "30" }, { "blink", "5" }, { "blue", "34" }, { "bold", "1" },
        { "brown", "33" }, { "cyan", "36" }, { "darkgray", "1;30" }, { "gray", "37" },
        { "green", "32" }, { "halfbright", "2" }, { "lightblue", "1;34" },
        { "lightcyan", "1;36" }, { "lightgray", "37" }, { "lightgreen", "1;32" },
        { "lightmagenta", "1;35" }, { "lightred", "1;31" }, { "magenta", "35" },
        { "red", "31" }, { "reset", "0" }, { "reverse", "7" }, { "white", "1;37" },
        { "yellow", "1;33" },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strlen(tab[i][0]) == n && !strncmp(tab[i][0], name, n)) return tab[i][1];
    return 0;
}

/* a decimal offset as strtoll reads one; 0 when it is not all digits */
static int hd_offnum(const char *s, const char *e, long long *v, const char *unit) {
    const char *p = s;
    int neg = 0;
    if (p < e && (*p == '+' || *p == '-')) { neg = *p == '-'; p++; }
    unsigned long long x = 0;
    if (p == e && s == e) { *v = 0; return 1; }      /* nothing: 0 */
    if (p == e) return 0;
    for (; p < e; p++) {
        if (!isdigit((unsigned char)*p)) return 0;
        unsigned d = (unsigned)(*p - '0');
        if (x > (9223372036854775807ULL - d) / 10) su_error(1, 0, "bad format {%s}", unit);
        x = x * 10 + d;
    }
    *v = neg ? -(long long)x : (long long)x;
    return 1;
}

/* the _L[...] list of PR; a unit that cannot be read drops the whole list */
static void hd_parse_colors(struct hd_pr *pr, const char *list, size_t len) {
    char *buf = hd_strndup(list, len);
    if (len && buf[len - 1] == ',') buf[--len] = 0;     /* one trailing comma is fine */
    struct hd_color *cs = 0;
    int n = 0, ok = 1;
    char *p = buf;
    if (!len) ok = 0;
    while (ok) {
        char *end = strchr(p, ',');
        if (end) *end = 0;
        struct hd_color c;
        memset(&c, 0, sizeof c);
        char *q = p;
        if (*q == '!') { c.invert = 1; q++; }
        size_t nl = strcspn(q, ":@");
        c.code = hd_color_code(q, nl);
        if (!c.code) { ok = 0; break; }
        q += nl;
        if (*q == ':') {
            q++;
            size_t vl = strcspn(q, "@");
            c.has_val = 1;
            if (vl && q[0] == '0') {
                int base = 8;
                const char *d = q + 1;
                if (vl >= 2 && (q[1] == 'x' || q[1] == 'X')) { base = 16; d = q + 2; }
                uint64_t v = 0;
                for (; d < q + vl; d++) {
                    int dv = hd_digit((unsigned char)*d, base);
                    if (dv < 0) { ok = 0; break; }
                    v = v * (uint64_t)base + (uint64_t)dv;
                }
                if (!ok) break;
                c.val = v;
            } else {
                c.is_str = 1;
                c.str = hd_strndup(q, vl);
                c.slen = vl;
            }
            q += vl;
        }
        if (*q == '@') {
            q++;
            char *dash = strchr(q, '-');
            /* a leading sign belongs to the number */
            if (dash == q) dash = strchr(q + 1, '-');
            char *ae = dash ? dash : q + strlen(q);
            if (!hd_offnum(q, ae, &c.a, p)) { ok = 0; break; }
            if (dash) {
                if (!hd_offnum(dash + 1, dash + 1 + strlen(dash + 1), &c.e, p)) { ok = 0; break; }
            } else {
                c.lone = 1;
                c.e = c.a;
            }
            c.has_off = c.a >= 0;
            /* A string is held to the unit's size by its range, not its
             * length, and with one offset only to being there at all.
             * (A range longer than the unit sometimes gets util-linux a
             * double free instead of this message; that is not copied.) */
            if (c.has_off && !c.lone &&
                (c.e < c.a - 1 || (c.is_str && c.e - c.a + 1 != pr->bcnt)))
                su_error(1, 0, "bad byte count for conversion character _L");
            if (c.is_str && c.lone && !c.slen)
                su_error(1, 0, "bad byte count for conversion character _L");
        } else if (*q) {
            ok = 0;
            break;
        } else if (c.is_str && (int)c.slen != pr->bcnt) {
            su_error(1, 0, "bad byte count for conversion character _L");
        }
        cs = (struct hd_color *)hd_xrealloc(cs, (size_t)(n + 1) * sizeof *cs);
        cs[n++] = c;
        if (!end) break;
        p = end + 1;
    }
    free(buf);
    if (ok) { pr->colors = cs; pr->ncolors = n; }
    else free(cs);
}

/* which units an offset range takes, fitted to what util-linux colours:
 * whole units from an aligned start; from a start inside a unit, only
 * the unit the end falls in, and only when the end is as far into that
 * unit as the start was into its own */
static int hd_in_range(const struct hd_color *c, long long a, int b) {
    if (c->lone) return a == c->a;
    long long A = c->a, E = c->e;
    if (b < 1) b = 1;
    if (E == A - 1) return (a <= A && A <= a + b - 1) || (a <= E && E <= a + b - 1);
    long long m = A % b;
    if (m == 0) return A <= a && a <= E;
    return a <= E && E <= a + b - 1 && E % b >= m;
}

/* the escape that colours PR at address A with data BP, or 0 */
static const char *hd_pick_color(const struct hd_pr *pr, long long a, const unsigned char *bp) {
    for (int i = 0; i < pr->ncolors; i++) {
        const struct hd_color *c = &pr->colors[i];
        int b = pr->bcnt;
        if (c->has_off && !hd_in_range(c, a, b)) continue;
        if (!c->has_val) return c->code;
        int match;
        if (c->is_str) {
            match = pr->kind != HD_ADDR && pr->kind != HD_ADDR_END &&
                    (int)c->slen == pr->bcnt && !memcmp(c->str, bp, c->slen);
        } else if (pr->kind == HD_ADDR || pr->kind == HD_ADDR_END) {
            match = c->val == (uint64_t)a;
        } else {
            uint64_t v = 0;
            for (int k = (b > 8 ? 8 : b) - 1; k >= 0; k--) v = v << 8 | bp[k];
            match = v == c->val;
        }
        if (match && c->has_off && !c->lone) {
            long long A = c->a, E = c->e;
            if (E < A || a < A || a + b - 1 > E || (a - A) % (b ? b : 1)) match = 0;
        }
        if (c->invert) match = !match;
        if (match) return c->code;
    }
    return 0;
}

/* ---- conversions: the second pass ------------------------------------------- */

static void hd_badconv(const char *s, size_t n) __attribute__((noreturn));
static void hd_badconv(const char *s, size_t n) {
    char b[8];
    if (n > 3) n = 3;
    memcpy(b, s, n);
    b[n] = 0;
    su_error(1, 0, "bad conversion character %%%s", b);
    exit(1);
}

static void hd_badcnt(const char *what) __attribute__((noreturn));
static void hd_badcnt(const char *what) {
    su_error(1, 0, "bad byte count for conversion character %s", what);
    exit(1);
}

static void hd_add_pr(struct hd_fu *fu, struct hd_pr *pr) {
    fu->prs = (struct hd_pr *)hd_xrealloc(fu->prs, (size_t)(fu->npr + 1) * sizeof *fu->prs);
    fu->prs[fu->npr++] = *pr;
}

static void hd_rewrite_fu(struct hd_fu *fu) {
    const char *f = fu->fmt;
    const char *p = f;
    int nconv = 0, sum = 0;
    while (*p) {
        const char *t = p;
        while (*p && *p != '%') p++;
        struct hd_pr pr;
        memset(&pr, 0, sizeof pr);
        pr.prec = -1;
        pr.text = hd_strndup(t, (size_t)(p - t));
        if (!*p) {
            pr.kind = HD_TEXT;
            if (*pr.text) hd_add_pr(fu, &pr);
            else free(pr.text);
            break;
        }
        p++;
        for (;; p++) {
            if (*p == '-') pr.left = 1;
            else if (*p == '+') pr.plus = 1;
            else if (*p == ' ') pr.space = 1;
            else if (*p == '#') pr.alt = 1;
            else if (*p == '0') pr.zero = 1;
            else break;
        }
        while (isdigit((unsigned char)*p)) { pr.width = pr.width * 10 + (*p - '0'); p++; }
        if (*p == '.') {
            p++;
            pr.prec = 0;
            while (isdigit((unsigned char)*p)) { pr.prec = pr.prec * 10 + (*p - '0'); p++; }
        }
        const char *cs = p;
        int is_addr = 0;
        switch (*p) {
        case 'c':
            pr.kind = HD_CHAR; pr.conv = 'c'; pr.bcnt = 1;
            if (fu->bcnt && fu->bcnt != 1) hd_badcnt("c");
            p++;
            break;
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
            pr.kind = (*p == 'd' || *p == 'i') ? HD_INT : HD_UINT;
            pr.conv = *p;
            pr.bcnt = 4;
            if (fu->bcnt) {
                if (fu->bcnt != 1 && fu->bcnt != 2 && fu->bcnt != 4 && fu->bcnt != 8) {
                    char w[2] = { *p, 0 };
                    hd_badcnt(w);
                }
                pr.bcnt = fu->bcnt;
            }
            p++;
            break;
        case 'e': case 'E': case 'f': case 'g': case 'G':
            pr.kind = HD_DBL;
            pr.conv = *p;
            pr.bcnt = 8;
            if (fu->bcnt) {
                if (fu->bcnt != 4 && fu->bcnt != 8) {
                    char w[2] = { *p, 0 };
                    hd_badcnt(w);
                }
                pr.bcnt = fu->bcnt;
            }
            p++;
            break;
        case 's':
            pr.kind = HD_STR;
            pr.conv = 's';
            if (fu->bcnt) {
                pr.bcnt = fu->bcnt;
                if (pr.prec < 0) pr.prec = fu->bcnt;
            } else if (pr.prec >= 0) {
                pr.bcnt = (int)pr.prec;
            } else {
                su_error(1, 0, "%%s requires a precision or a byte count");
            }
            p++;
            break;
        case '_':
            switch (p[1]) {
            case 'A': case 'a':
                is_addr = 1;
                if (!p[2]) {
                    pr.kind = HD_NONE;
                    if (p[1] == 'A') { fu->ignore = 1; hd_endfu = fu; }
                    p += 2;
                    break;
                }
                if (p[2] != 'd' && p[2] != 'o' && p[2] != 'x') hd_badconv(p, 3);
                pr.kind = p[1] == 'A' ? HD_ADDR_END : HD_ADDR;
                pr.conv = p[2];
                if (p[1] == 'A') { fu->ignore = 1; hd_endfu = fu; }
                p += 3;
                break;
            case 'c': case 'p': case 'u':
                pr.kind = p[1] == 'c' ? HD_C : p[1] == 'p' ? HD_P : HD_U;
                pr.bcnt = 1;
                if (fu->bcnt && fu->bcnt != 1) {
                    char w[3] = { '_', p[1], 0 };
                    hd_badcnt(w);
                }
                p += 2;
                break;
            case 0:
                hd_badconv(p, 1);
            default:
                hd_badconv(p, 2);
            }
            break;
        default:
            hd_badconv(cs, *cs ? 1 : 0);
        }
        if (!is_addr && fu->bcnt && ++nconv > 1)
            su_error(1, 0, "byte count with multiple conversion characters");
        if (p[0] == '_' && p[1] == 'L') {
            const char *lb = strchr(p + 2, '[');
            const char *rb = strrchr(p + 2, ']');
            if (!lb || !rb || rb < lb) hd_badconv("_L", 2);
            hd_parse_colors(&pr, lb + 1, (size_t)(rb - lb - 1));
            p = rb + 1;
        }
        sum += pr.bcnt;
        hd_add_pr(fu, &pr);
    }
    if (!fu->bcnt) fu->bcnt = sum;
}

static void hd_rewrite(void) {
    for (int i = 0; i < hd_nfs; i++) {
        struct hd_fs *fs = &hd_fss[i];
        fs->bcnt = 0;
        for (int j = 0; j < fs->nfu; j++) {
            hd_rewrite_fu(&fs->fus[j]);
            fs->bcnt += fs->fus[j].reps * fs->fus[j].bcnt;
        }
        if (fs->bcnt > hd_blocksize) hd_blocksize = fs->bcnt;
    }
    for (int i = 0; i < hd_nfs; i++) {
        struct hd_fs *fs = &hd_fss[i];
        for (int j = 0; j < fs->nfu; j++) {
            struct hd_fu *fu = &fs->fus[j];
            if (j == fs->nfu - 1 && fs->bcnt < hd_blocksize && !fu->setrep && fu->bcnt)
                fu->reps += (hd_blocksize - fs->bcnt) / fu->bcnt;
            if (fu->reps > 1 && fu->npr) {
                struct hd_pr *last = &fu->prs[fu->npr - 1];
                size_t n = strlen(last->text);
                if (last->kind == HD_TEXT && n && isspace((unsigned char)last->text[n - 1]))
                    fu->nospace = 1;
            }
        }
    }
}

/* ---- printing one PR -------------------------------------------------------- */

/* S (N bytes) in a field as printf's %s would put it */
static void hd_put_str(const struct hd_pr *pr, const char *s, size_t n) {
    if (pr->prec >= 0 && (size_t)pr->prec < n) n = (size_t)pr->prec;
    long pad = pr->width > (long)n ? pr->width - (long)n : 0;
    if (!pr->left) hd_spaces(pad);
    hd_out(s, n);
    if (pr->left) hd_spaces(pad);
}

static void hd_put_char(const struct hd_pr *pr, char c) {
    long pad = pr->width > 1 ? pr->width - 1 : 0;
    if (!pr->left) hd_spaces(pad);
    putchar(c);
    if (pr->left) hd_spaces(pad);
}

/* an integer as printf's %d %o %u %x %X would put it */
static void hd_put_int(const struct hd_pr *pr, char conv, uint64_t mag, int neg) {
    char digits[72];
    int nd = 0;
    int base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X') ? 16 : 10;
    const char *dig = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    uint64_t v = mag;
    if (!(pr->prec == 0 && v == 0))
        do { digits[nd++] = dig[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    long zeros = pr->prec > nd ? pr->prec - nd : 0;
    char prefix[3] = { 0, 0, 0 };
    int np = 0;
    if (conv == 'd' || conv == 'i') {
        if (neg) prefix[np++] = '-';
        else if (pr->plus) prefix[np++] = '+';
        else if (pr->space) prefix[np++] = ' ';
    } else if (pr->alt) {
        if (base == 8 && zeros == 0 && (nd == 0 || digits[nd - 1] != '0')) zeros = 1;
        if (base == 16 && mag) { prefix[np++] = '0'; prefix[np++] = conv; }
    }
    long len = np + zeros + nd;
    long pad = pr->width > len ? pr->width - len : 0;
    if (pr->zero && !pr->left && pr->prec < 0) { zeros += pad; pad = 0; }
    if (!pr->left) hd_spaces(pad);
    hd_out(prefix, (size_t)np);
    for (long i = 0; i < zeros; i++) putchar('0');
    while (nd) putchar(digits[--nd]);
    if (pr->left) hd_spaces(pad);
}

static uint64_t hd_get(const unsigned char *bp, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = v << 8 | bp[i];
    return v;
}

static void hd_put_double(const struct hd_pr *pr, const unsigned char *bp) {
    union sn_ld u;
    memset(&u, 0, sizeof u);
    int neg;
    if (pr->bcnt == 4) {
        uint32_t v = (uint32_t)hd_get(bp, 4);
        neg = (int)(v >> 31);
        unsigned ex = (v >> 23) & 0xff;
        uint64_t fr = v & 0x7fffff;
        if (ex == 0xff) { u.p.se = 0x7fff; u.p.m = fr ? 0xC000000000000000ULL | fr << 40 : 0x8000000000000000ULL; }
        else if (!ex && !fr) { u.p.se = 0; u.p.m = 0; }
        else if (!ex) {
            int sh = 0;
            while (!(fr & 0x800000)) { fr <<= 1; sh++; }
            u.p.m = fr << 40;
            u.p.se = (uint16_t)(-126 - sh + 16383);
        } else { u.p.m = (fr | 0x800000) << 40; u.p.se = (uint16_t)(ex - 127 + 16383); }
    } else {
        uint64_t v = hd_get(bp, 8);
        neg = (int)(v >> 63);
        unsigned ex = (unsigned)(v >> 52) & 0x7ff;
        uint64_t fr = v & 0xfffffffffffffULL;
        if (ex == 0x7ff) { u.p.se = 0x7fff; u.p.m = fr ? 0xC000000000000000ULL | fr << 11 : 0x8000000000000000ULL; }
        else if (!ex && !fr) { u.p.se = 0; u.p.m = 0; }
        else if (!ex) {
            int sh = 0;
            while (!(fr & 0x10000000000000ULL)) { fr <<= 1; sh++; }
            u.p.m = fr << 11;
            u.p.se = (uint16_t)(-1022 - sh + 16383);
        } else { u.p.m = (fr | 0x10000000000000ULL) << 11; u.p.se = (uint16_t)(ex - 1023 + 16383); }
    }
    if (neg) u.p.se |= 0x8000;
    struct su_fmtspec fs;
    fs.left = pr->left; fs.plus = pr->plus; fs.space = pr->space; fs.alt = pr->alt;
    fs.zero = pr->zero;
    fs.width = pr->width > 0 ? (int)pr->width : -1;
    fs.prec = (int)pr->prec;
    fs.conv = pr->conv;
    struct sn_sb sb = { 0, 0, 0 };
    su_ldfmt(&sb, u.v, &fs);
    hd_out(sb.s, sb.n);
    free(sb.s);
}

static const char *const hd_unames[32] = {
    "nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel", "bs", "ht", "lf",
    "vt", "ff", "cr", "so", "si", "dle", "dc1", "dc2", "dc3", "dc4", "nak",
    "syn", "etb", "can", "em", "sub", "esc", "fs", "gs", "rs", "us"
};

static int hd_isprint(int c) { return c >= 0x20 && c < 0x7f; }

/* PR at address A over the data at BP; PAD: the data has ended */
static void hd_print(const struct hd_pr *pr, long long a, const unsigned char *bp, int pad, int nospace) {
    if (pr->kind == HD_TEXT) {
        size_t n = strlen(pr->text);
        if (nospace && n) n--;
        hd_out(pr->text, n);
        return;
    }
    if (pr->kind == HD_NONE) return;
    const char *color = hd_color_on && pr->ncolors ? hd_pick_color(pr, a, bp) : 0;
    if (color) printf("\033[%sm", color);
    hd_outs(pr->text);
    if (pad) {
        hd_spaces(pr->width);
    } else {
        char t[8];
        switch (pr->kind) {
        case HD_ADDR: case HD_ADDR_END:
            if (pr->conv == 'd') hd_put_int(pr, 'd', a < 0 ? (uint64_t)-a : (uint64_t)a, a < 0);
            else hd_put_int(pr, pr->conv, (uint64_t)a, 0);
            break;
        case HD_INT: {
            uint64_t v = hd_get(bp, pr->bcnt);
            int bits = pr->bcnt * 8;
            int neg = (int)((v >> (bits - 1)) & 1);
            uint64_t mag = v;
            if (neg) mag = bits < 64 ? ((uint64_t)1 << bits) - v : (uint64_t)0 - v;
            hd_put_int(pr, 'd', mag, neg);
            break;
        }
        case HD_UINT:
            hd_put_int(pr, pr->conv, hd_get(bp, pr->bcnt), 0);
            break;
        case HD_CHAR:
            hd_put_char(pr, (char)bp[0]);
            break;
        case HD_STR: {
            size_t n = 0;
            while (n < (size_t)pr->bcnt && bp[n]) n++;
            hd_put_str(pr, (const char *)bp, n);
            break;
        }
        case HD_DBL:
            hd_put_double(pr, bp);
            break;
        case HD_C: {
            int c = bp[0];
            const char *e = c == 0 ? "\\0" : c == '\a' ? "\\a" : c == '\b' ? "\\b" : c == '\f' ? "\\f"
                          : c == '\n' ? "\\n" : c == '\r' ? "\\r" : c == '\t' ? "\\t" : c == '\v' ? "\\v" : 0;
            if (e) hd_put_str(pr, e, 2);
            else if (hd_isprint(c)) hd_put_char(pr, (char)c);
            else {
                t[0] = (char)('0' + ((c >> 6) & 3));
                t[1] = (char)('0' + ((c >> 3) & 7));
                t[2] = (char)('0' + (c & 7));
                hd_put_str(pr, t, 3);
            }
            break;
        }
        case HD_P:
            hd_put_char(pr, hd_isprint(bp[0]) ? (char)bp[0] : '.');
            break;
        case HD_U: {
            int c = bp[0];
            if (c < 0x20) hd_put_str(pr, hd_unames[c], strlen(hd_unames[c]));
            else if (c == 0x7f) hd_put_str(pr, "del", 3);
            else if (c < 0x7f) hd_put_char(pr, (char)c);
            else hd_put_int(pr, 'x', (uint64_t)c, 0);   /* as %x: a precision is digits */
            break;
        }
        default: break;
        }
    }
    if (color) hd_outs("\033[0m");
}

/* ---- input ------------------------------------------------------------------ */

static char **hd_files;
static int hd_nfiles, hd_fileidx;
static int hd_fd = -1;
static const char *hd_fname;
static int hd_status;
static int hd_opened_any;
static uint64_t hd_skip;
static long long hd_length = -1;        /* -1: no limit */
static long long hd_address, hd_eaddress;
static unsigned char hd_ibuf[4096];
static size_t hd_ipos, hd_ilen;

static void hd_close(void) {
    if (hd_fd == 0 && hd_ilen > hd_ipos) lseek(0, -(off_t)(hd_ilen - hd_ipos), SEEK_CUR);
    if (hd_fd > 0) close(hd_fd);
    hd_fd = -1;
    hd_ipos = hd_ilen = 0;
}

/* -s against the file just opened; NAMED: stat may skip it whole */
static int hd_doskip(int named) {
    if (named) {
        struct stat st;
        if (fstat(hd_fd, &st) != 0) su_error(1, errno, "%s", hd_fname);
        if (S_ISREG(st.st_mode) && (long long)hd_skip >= (long long)st.st_size) {
            hd_address += st.st_size;
            hd_skip -= (uint64_t)st.st_size;
            return 1;
        }
    }
    if ((long long)hd_skip < 0) su_error(1, EINVAL, "%s", named ? hd_fname : "stdin");
    if (lseek(hd_fd, (off_t)hd_skip, SEEK_SET) < 0) su_error(1, errno, "%s", named ? hd_fname : "stdin");
    hd_address += (long long)hd_skip;
    hd_skip = 0;
    return 0;
}

/* the next input, skipping as it goes; 0 when there is none */
static int hd_next(void) {
    for (;;) {
        hd_close();
        if (hd_nfiles == 0) {
            if (hd_fileidx++) return 0;
            hd_fd = 0;
            hd_fname = "stdin";
            hd_opened_any = 1;
            if (hd_skip) hd_doskip(0);
            return 1;
        }
        if (hd_fileidx >= hd_nfiles) {
            if (!hd_opened_any) {
                if (hd_skip) su_error(1, EBADF, "stdin");
                su_error(1, 0, "all input file arguments failed");
            }
            return 0;
        }
        const char *name = hd_files[hd_fileidx++];
        int fd = su_open_read(name, 0);
        if (fd < 0) {
            su_error(0, errno, "%s", name);
            hd_status = 1;
            continue;
        }
        hd_fd = fd;
        hd_fname = name;
        hd_opened_any = 1;
        if (hd_skip) {
            if (hd_doskip(1)) continue;      /* skipped whole */
        }
        return 1;
    }
}

/* up to WANT bytes into BUF, across files */
static size_t hd_read(unsigned char *buf, size_t want) {
    size_t got = 0;
    while (got < want) {
        if (hd_fd < 0) break;
        if (hd_ipos == hd_ilen) {
            ssize_t n;
            if (hd_length < -1) { errno = EFAULT; n = -1; }
            else n = read(hd_fd, hd_ibuf, sizeof hd_ibuf);
            if (n <= 0) {
                if (n < 0) su_error(0, errno, "%s", hd_fname);
                if (!hd_next()) break;
                continue;
            }
            hd_ipos = 0;
            hd_ilen = (size_t)n;
        }
        size_t k = hd_ilen - hd_ipos;
        if (k > want - got) k = want - got;
        memcpy(buf + got, hd_ibuf + hd_ipos, k);
        hd_ipos += k;
        got += k;
    }
    return got;
}

/* ---- the display ------------------------------------------------------------ */

static void hd_display_block(const unsigned char *block) {
    for (int i = 0; i < hd_nfs; i++) {
        struct hd_fs *fs = &hd_fss[i];
        const unsigned char *bp = block;
        long long a = hd_address;
        for (int j = 0; j < fs->nfu; j++) {
            struct hd_fu *fu = &fs->fus[j];
            if (fu->ignore) break;
            for (int rep = fu->reps; rep > 0; rep--) {
                for (int k = 0; k < fu->npr; k++) {
                    struct hd_pr *pr = &fu->prs[k];
                    int pad = hd_eaddress && a >= hd_eaddress && pr->kind != HD_TEXT;
                    hd_print(pr, a, bp, pad, rep == 1 && fu->nospace && k == fu->npr - 1);
                    a += pr->bcnt;
                    bp += pr->bcnt;
                }
            }
        }
    }
}

static void hd_display(int vflag) {
    size_t bs = (size_t)hd_blocksize;
    unsigned char *cur = (unsigned char *)hd_xrealloc(0, bs + 16), *prev = (unsigned char *)hd_xrealloc(0, bs + 16);
    int have_prev = 0, dup = 0;
    if (!hd_next()) { free(cur); free(prev); return; }
    if (bs) {
        for (;;) {
            size_t want = bs;
            if (hd_length >= 0 && (uint64_t)hd_length < want) want = (size_t)hd_length;
            size_t got = want ? hd_read(cur, want) : 0;
            if (hd_length >= 0) hd_length -= (long long)got;
            if (!got) break;
            if (got < bs) {
                memset(cur + got, 0, bs - got);
                hd_eaddress = hd_address + (long long)got;
                hd_display_block(cur);
                hd_address += (long long)got;
                break;
            }
            if (!vflag && have_prev && !memcmp(cur, prev, bs)) {
                if (!dup) hd_outs("*\n");
                dup = 1;
            } else {
                hd_display_block(cur);
                dup = 0;
            }
            unsigned char *t = prev; prev = cur; cur = t;
            have_prev = 1;
            hd_address += (long long)bs;
        }
    }
    hd_close();
    free(cur);
    free(prev);
}

static void hd_finish(void) {
    if (!hd_endfu) return;
    if (!hd_eaddress) {
        if (!hd_address) return;
        hd_eaddress = hd_address;
    }
    static const unsigned char zero[16];
    for (int k = 0; k < hd_endfu->npr; k++) {
        struct hd_pr *pr = &hd_endfu->prs[k];
        if (pr->kind == HD_TEXT) hd_outs(pr->text);
        else if (pr->kind == HD_ADDR || pr->kind == HD_ADDR_END) hd_print(pr, hd_eaddress, zero, 0, 0);
    }
}

/* ---- options ---------------------------------------------------------------- */

/* -n -s: 0 or an errno */
static int hd_parse_size(const char *s, uint64_t *out) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (strchr(p, '-')) return EINVAL;
    if (*p == '+') p++;
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && hd_digit((unsigned char)p[2], 16) >= 0) { base = 16; p += 2; }
    else if (p[0] == '0') base = 8;
    if (hd_digit((unsigned char)*p, base) < 0) return EINVAL;
    uint64_t v = 0;
    int over = 0;
    for (int d; (d = hd_digit((unsigned char)*p, base)) >= 0; p++) {
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) over = 1;
        else v = v * (uint64_t)base + (uint64_t)d;
    }
    if (over) return ERANGE;
    const char *frac = 0;
    size_t nfrac = 0;
    if (*p == '.') {
        frac = ++p;
        while (isdigit((unsigned char)*p)) p++;
        nfrac = (size_t)(p - frac);
    }
    if (!*p) {
        if (frac) return EINVAL;
        *out = v;
        return 0;
    }
    static const char sufs[] = "KMGTPEZY";
    const char *sp = strchr(sufs, toupper((unsigned char)*p));
    if (!sp || !*p) return EINVAL;
    int power = (int)(sp - sufs) + 1;
    p++;
    uint64_t k = 1024;
    if (p[0] == 'i' && (p[1] == 'B' || p[1] == 'b')) p += 2;
    else if (p[0] == 'B' || p[0] == 'b') { k = 1000; p++; }
    if (*p) return EINVAL;
    uint64_t mult = 1;
    for (int i = 0; i < power; i++) {
        if (mult > UINT64_MAX / k) return ERANGE;
        mult *= k;
    }
    if (v && mult > UINT64_MAX / v) return ERANGE;
    v *= mult;
    if (frac && nfrac) {
        /* util-linux's fraction: each digit d at place p adds MULT/(10^p/d),
         * its places counted from the fraction's value -- 10 is one digit
         * worth ten at the first place, so 1.10K is 2048 */
        const char *q = frac;
        int zeros = 0;
        while (q < frac + nfrac && *q == '0') { zeros++; q++; }
        uint64_t fv = 0;
        int big = 0;
        for (; q < frac + nfrac; q++) {
            if (fv > (UINT64_MAX - 9) / 10) { big = 1; break; }
            fv = fv * 10 + (uint64_t)(*q - '0');
        }
        if (fv && !big) {
            int K = 1;
            uint64_t div = 10;
            while (div < fv && div <= UINT64_MAX / 10) { div *= 10; K++; }
            uint64_t unit = div / 10;           /* the first place's weight */
            uint64_t rest = fv;
            for (int i = 1; i <= K; i++) {
                uint64_t d = i == 1 ? rest / unit : (rest / unit) % 10;
                if (i == 1) rest %= unit;
                else rest %= unit;
                uint64_t place = 1;
                int ok = 1;
                for (int z = 0; z < i + zeros; z++) {
                    if (place > UINT64_MAX / 10) { ok = 0; break; }
                    place *= 10;
                }
                if (d && ok && place / d) v += mult / (place / d);
                unit /= 10;
                if (!unit) break;
            }
        }
    }
    *out = v;
    return 0;
}

static void hd_usage(void) {
    printf("\n"
           "Usage:\n"
           " %s [options] <file>...\n"
           "\n"
           "Show the bytes of the files (or of standard input) as hexadecimal, decimal,\n"
           "octal or text, in a fixed layout or in one given by format strings.\n"
           "\n"
           "Options:\n"
           " -b, --one-byte-octal      every byte as three octal digits\n"
           " -X, --one-byte-hex        every byte as two hex digits\n"
           " -c, --one-byte-char       every byte as a character or its escape\n"
           " -C, --canonical           hex bytes and their text side by side\n"
           " -d, --two-bytes-decimal   two-byte units in unsigned decimal\n"
           " -o, --two-bytes-octal     two-byte units in octal\n"
           " -x, --two-bytes-hex       two-byte units in hex\n"
           " -L, --color[=<mode>]      honour the colour units of the formats;\n"
           "                             <mode> is auto (the default), never or always\n"
           " -e, --format <format>     a format string to show the data with\n"
           " -f, --format-file <file>  format strings, one per line\n"
           " -n, --length <length>     show no more than <length> bytes\n"
           " -s, --skip <offset>       begin <offset> bytes into the input\n"
           " -v, --no-squeezing        show repeated lines as they are, not as *\n"
           "\n"
           " -h, --help                show this help and exit\n"
           " -V, --version             show the version and exit\n"
           "\n"
           "<length> and <offset> may be 0x hex or 0 octal, and may end in a\n"
           "multiplier: KiB, MiB, GiB, TiB, PiB, EiB, ZiB or YiB (powers of 1024; the\n"
           "\"iB\" may be left off), or KB, MB, ... (powers of 1000).\n",
           su_name);
}

static void hd_std(int which) {
    switch (which) {
    case 'b':
        hd_add("\"%07.7_Ax\n\"");
        hd_add("\"%07.7_ax \" 16/1 \"%03o \" \"\\n\"");
        break;
    case 'c':
        hd_add("\"%07.7_Ax\n\"");
        hd_add("\"%07.7_ax \" 16/1 \"%3_c \" \"\\n\"");
        break;
    case 'C':
        hd_add("\"%08.8_Ax\n\"");
        hd_add("\"%08.8_ax  \" 8/1 \"%02x \" \"  \" 8/1 \"%02x \" ");
        hd_add("\"  |\" 16/1 \"%_p\" \"|\\n\"");
        break;
    case 'd':
        hd_add("\"%07.7_Ax\n\"");
        hd_add("\"%07.7_ax \" 8/2 \"  %05u \" \"\\n\"");
        break;
    case 'o':
        hd_add("\"%07.7_Ax\n\"");
        hd_add("\"%07.7_ax \" 8/2 \" %06o \" \"\\n\"");
        break;
    case 'x':
        hd_add("\"%07.7_Ax\n\"");
        hd_add("\"%07.7_ax \" 8/2 \"   %04x \" \"\\n\"");
        break;
    case 'X':
        hd_add("\"%07.7_Ax\n\"");
        hd_add("\"%07.7_ax \" 16/1 \" %02x \" \"\\n\"");
        break;
    default:
        hd_add("\"%07.7_Ax\n\"");
        hd_add("\"%07.7_ax \" 8/2 \"%04x \" \"\\n\"");
        break;
    }
}

/* -f FILE: every line that is not blank or a #-comment */
static void hd_add_file(const char *path) {
    int fd = su_open_read(path, 0);
    if (fd < 0) su_error(1, errno, "can't read %s", path);
    char *buf = 0;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            buf = (char *)hd_xrealloc(buf, cap);
        }
        ssize_t k = read(fd, buf + n, 4096);
        if (k <= 0) break;          /* a directory reads as nothing, as with fgets */
        n += (size_t)k;
    }
    close(fd);
    if (!buf) return;
    buf[n] = 0;
    char *line = buf;
    while (line < buf + n) {
        char *nl = memchr(line, '\n', (size_t)(buf + n - line));
        char *end = nl ? nl + 1 : buf + n;
        char save = *end;
        *end = 0;
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p && *p != '#') hd_add(p);
        *end = save;
        line = end;
    }
    free(buf);
}

static int do_hexdump(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "one-byte-octal", SU_NO_ARG, 0, 'b' },
        { "one-byte-hex", SU_NO_ARG, 0, 'X' },
        { "one-byte-char", SU_NO_ARG, 0, 'c' },
        { "canonical", SU_NO_ARG, 0, 'C' },
        { "two-bytes-decimal", SU_NO_ARG, 0, 'd' },
        { "two-bytes-octal", SU_NO_ARG, 0, 'o' },
        { "two-bytes-hex", SU_NO_ARG, 0, 'x' },
        { "color", SU_OPT_ARG, 0, 'L' },
        { "format", SU_REQ_ARG, 0, 'e' },
        { "format-file", SU_REQ_ARG, 0, 'f' },
        { "length", SU_REQ_ARG, 0, 'n' },
        { "skip", SU_REQ_ARG, 0, 's' },
        { "no-squeezing", SU_NO_ARG, 0, 'v' },
        { "help", SU_NO_ARG, 0, 'h' },
        { "version", SU_NO_ARG, 0, 'V' },
        { 0, 0, 0, 0 }
    };
    int vflag = 0, color = 0;       /* 0 auto, 1 never, 2 always */
    int c;
    while ((c = su_getopt_long(argc, argv, "bcCde:f:hL::n:os:vVxX", longs, 0)) != -1) {
        switch (c) {
        case 'b': case 'c': case 'C': case 'd': case 'o': case 'x': case 'X':
            hd_std(c);
            break;
        case 'e': hd_add(su_optarg); break;
        case 'f': hd_add_file(su_optarg); break;
        case 'L':
            if (!su_optarg || !strcasecmp(su_optarg, "auto")) color = 0;
            else if (!strcasecmp(su_optarg, "never")) color = 1;
            else if (!strcasecmp(su_optarg, "always")) color = 2;
            else su_error(1, 0, "unsupported color mode: '%s'", su_optarg);
            break;
        case 'n': {
            uint64_t v;
            int e = hd_parse_size(su_optarg, &v);
            if (e) su_error(1, e, "failed to parse length: '%s'", su_optarg);
            hd_length = (long long)v;
            break;
        }
        case 's': {
            uint64_t v;
            int e = hd_parse_size(su_optarg, &v);
            if (e) su_error(1, e, "failed to parse offset: '%s'", su_optarg);
            hd_skip = v;
            break;
        }
        case 'v': vflag = 1; break;
        case 'h': hd_usage(); return 0;
        case 'V': return su_version();
        default: su_try_help(1);
        }
    }
    if (!hd_nfs) {
        if (!strcmp(su_short, "hd")) hd_std('C');
        else hd_std(0);
    }
    hd_precheck();
    hd_rewrite();
    if (hd_length == 0) return 0;
    hd_color_on = color == 2 || (color == 0 && isatty(1) && !getenv("NO_COLOR"));
    hd_files = argv + su_optind;
    hd_nfiles = argc - su_optind;
    hd_display(vflag);
    hd_finish();
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return hd_status;
}
