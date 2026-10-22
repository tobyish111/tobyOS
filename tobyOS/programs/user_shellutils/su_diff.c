/* su_diff.c -- diff, cmp, sdiff and diff3, as GNU diffutils 3.12's
 * (logs/utilgate/staging/diff.sh, cmp.sh, sdiff.sh, diff3.sh).
 *
 * Everything here was measured against the real tools (WSL's diffutils
 * 3.12), case by case; the help texts are this file's own words.
 *
 * THE HUNKS ARE GNU'S. Which lines a diff calls changed is not unique --
 * "a b c" against "c b a" can keep any one of the three -- so matching
 * GNU's output means matching how it chooses, and that was measured on
 * thousands of random file pairs (small alphabets, many equal lines,
 * blocks moved about, lines repeated many times) until no pair differed:
 *
 *   - what both files begin and end with, byte for byte, is set aside
 *     first, and nothing below looks at it (not even the sliding at the
 *     end) -- except the context lines' worth of it with -c and -u, and
 *     --horizon-lines (diff3 asks for 100);
 *   - a line the other file does not have at all is a change for sure and
 *     is left out of the search; so is a line the other file has "many"
 *     times (more than 5 for a few hundred lines, 10 from 256 lines, 20
 *     from 1024 ...) when it sits among such lines -- the rules for that
 *     are in dfu_discard, each one found by a pattern that only it
 *     explains; --minimal leaves nothing out;
 *   - the rest is Myers' O(ND) search from both corners at once, the
 *     diagonals of each round tried from the highest down, the box split
 *     where the two searches first meet -- and a search that has not met
 *     after 4096 rounds settles for the furthest point either side got
 *     to (without --minimal: files of thousands of differing lines);
 *     --speed-large-files changed nothing on any input tried;
 *   - then each run of changed lines is slid up as far as it goes (taking
 *     in runs it meets), down as far as it goes, and back up to the lowest
 *     place where the other file has changes opposite it, if there is one.
 *
 * Exit status: 0 the same, 1 different, 2 trouble (diff, cmp, sdiff);
 * diff3: 0 merged, 1 conflicts, 2 trouble. */

/* GNU diffutils' error(): "PROG: MESSAGE[: strerror]", PROG as typed */
static void dfu_err(int status, int errnum, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void dfu_err(int status, int errnum, const char *fmt, ...) {
    fflush(stdout);
    fprintf(stderr, "%s: ", su_name);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (errnum) fprintf(stderr, ": %s", strerror(errnum));
    fputc('\n', stderr);
    if (status) exit(status);
}

/* ---- memory ----------------------------------------------------------- */

static void *dfu_alloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) dfu_err(2, 0, "memory exhausted");
    return p;
}

static void *dfu_zalloc(size_t n) {
    void *p = dfu_alloc(n);
    memset(p, 0, n ? n : 1);
    return p;
}

static void *dfu_realloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) dfu_err(2, 0, "memory exhausted");
    return p;
}

static char *dfu_strdup(const char *s) {
    size_t n = strlen(s);
    char *p = dfu_alloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

/* "DIR/NAME" (no doubled slash when DIR already ends in one) */
static char *dfu_join(const char *dir, const char *name) {
    size_t a = strlen(dir), b = strlen(name);
    char *p = dfu_alloc(a + b + 2);
    memcpy(p, dir, a);
    size_t n = a;
    if (a && dir[a - 1] != '/') p[n++] = '/';
    memcpy(p + n, name, b + 1);
    return p;
}

/* ---- quoting ------------------------------------------------------------
 *
 * Three ways, each measured:
 *   dfu_qf  -- messages ("Only in", "Files ... differ", errors): bare when
 *              nothing needs it, else '...', "..." for a name with a '
 *              and no control bytes, and $'\t' pieces for control and
 *              8-bit bytes ('a'$'\t''b'). '=' needs quoting here.
 *   dfu_qa  -- the same, quoted always ("'[': Invalid ...").
 *   dfu_qsw -- the switches echoed in "diff -r ..." lines: the same set
 *              of characters, but control and 8-bit bytes are written as
 *              they are, and only a tab or newline makes quotes.
 *   dfu_qc  -- file names in "diff -r A B" lines and ---/+++ headers:
 *              "..." with C escapes when the name has a space, a quote, a
 *              backslash, a control or an 8-bit byte; else bare. */

static char *dfu_qbuf[8];
static int dfu_qslot;

static char *dfu_qnext(size_t need) {
    int k = dfu_qslot++ & 7;
    dfu_qbuf[k] = dfu_realloc(dfu_qbuf[k], need);
    return dfu_qbuf[k];
}

static int dfu_shell_special(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || strchr("=*?^!$[&;|\"\\'()<>`", c) != 0;
}

static int dfu_isctl(unsigned char c) { return c < 0x20 || c >= 0x7f; }

static const char *dfu_qgen(const char *s, int always, int raw_ctl) {
    size_t len = strlen(s);
    char *b = dfu_qnext(len * 6 + 8);
    int need = !*s, ctl = 0, sq = 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (dfu_shell_special(c)) need = 1;
        if (c == '\'') sq = 1;
        if (dfu_isctl(c) && !(raw_ctl && c != '\t' && c != '\n')) { ctl = 1; need = 1; }
    }
    if (*s == '~' || *s == '#') need = 1;
    if (!need && !always) { memcpy(b, s, len + 1); return b; }
    size_t n = 0;
    if (!ctl && sq) {
        b[n++] = '"';
        for (const char *p = s; *p; p++) {
            if (*p == '"' || *p == '$' || *p == '`' || *p == '\\') b[n++] = '\\';
            b[n++] = *p;
        }
        b[n++] = '"';
        b[n] = 0;
        return b;
    }
    if (!ctl) {
        b[n++] = '\'';
        memcpy(b + n, s, len);
        n += len;
        b[n++] = '\'';
        b[n] = 0;
        return b;
    }
    /* pieces: '...' for the plain runs, $'...' for control bytes */
    int in_dollar = 0;
    b[n++] = '\'';
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (dfu_isctl(c)) {
            if (!in_dollar) { b[n++] = '\''; b[n++] = '$'; b[n++] = '\''; in_dollar = 1; }
            const char *esc = c == '\a' ? "\\a" : c == '\b' ? "\\b" : c == '\t' ? "\\t"
                            : c == '\n' ? "\\n" : c == '\v' ? "\\v" : c == '\f' ? "\\f"
                            : c == '\r' ? "\\r" : 0;
            if (esc) { b[n++] = esc[0]; b[n++] = esc[1]; }
            else n += (size_t)sprintf(b + n, "\\%03o", c);
            continue;
        }
        if (in_dollar) { b[n++] = '\''; b[n++] = '\''; in_dollar = 0; }
        if (c == '\'') { b[n++] = '\''; b[n++] = '\\'; b[n++] = '\''; b[n++] = '\''; continue; }
        b[n++] = (char)c;
    }
    b[n++] = '\'';
    b[n] = 0;
    return b;
}

static const char *dfu_qf(const char *s) { return dfu_qgen(s, 0, 0); }
static const char *dfu_qa(const char *s) { return dfu_qgen(s, 1, 0); }
static const char *dfu_qsw(const char *s) { return dfu_qgen(s, 0, 1); }

static const char *dfu_qc(const char *s) {
    size_t len = strlen(s);
    int need = 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '"' || c == '\\' || dfu_isctl(c)) need = 1;
    }
    char *b = dfu_qnext(len * 4 + 3);
    if (!need) { memcpy(b, s, len + 1); return b; }
    size_t n = 0;
    b[n++] = '"';
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        const char *esc = c == '"' ? "\\\"" : c == '\\' ? "\\\\" : c == '\a' ? "\\a"
                        : c == '\b' ? "\\b" : c == '\t' ? "\\t" : c == '\n' ? "\\n"
                        : c == '\v' ? "\\v" : c == '\f' ? "\\f" : c == '\r' ? "\\r" : 0;
        if (esc) { b[n++] = esc[0]; b[n++] = esc[1]; }
        else if (dfu_isctl(c)) n += (size_t)sprintf(b + n, "\\%03o", c);
        else b[n++] = (char)c;
    }
    b[n++] = '"';
    b[n] = 0;
    return b;
}

/* GNU diffutils' error(): "PROG: MESSAGE[: strerror]"; its "Try" line
 * carries the program name too ("diff: Try 'diff --help' ...") */
static void dfu_try_help(void) __attribute__((noreturn));
static void dfu_try_help(void) {
    fflush(stdout);
    fprintf(stderr, "%s: Try '%s --help' for more information.\n", su_name, su_name);
    exit(2);
}

static void dfu_usage_error(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void dfu_usage_error(const char *fmt, ...) {
    fflush(stdout);
    fprintf(stderr, "%s: ", su_name);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    dfu_try_help();
}

/* regcomp's complaints, in glibc's words (libtoby has its own) */
static const char *dfu_regerr(int code, const char *pat) {
    size_t n = strlen(pat);
    if (n && pat[n - 1] == '[' && (n < 2 || pat[n - 2] != '\\')) return "Invalid regular expression";
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

/* ---- files and lines ------------------------------------------------------ */

struct dfu_file {
    const char *name;           /* as it is named in messages */
    const char *label;          /* --label, or 0 */
    char *buf;                  /* contents */
    size_t len;
    long nl;                    /* lines */
    const char **lp;            /* line I is lp[I] .. lp[I+1] (newline and all) */
    int incomplete;             /* the last line has no newline */
    int *cls;                   /* the class of each line */
    char *chg;                  /* changed flags, one per line */
    char *cr;                   /* --strip-trailing-cr: the line had a CR */
    int missing;                /* -N: absent, taken as empty */
    int isstdin;
    struct stat st;
    int st_ok;
};

/* how lines are compared (diff, sdiff and diff3 share it) */
static struct {
    int icase, itab, itrail, ispace, iall, strip_cr, text, tabsize, minimal;
    int eol_any;                /* ed output: an incomplete last line is a full one */
} dfu_o;

/* the whole of FD into F->buf */
static int dfu_slurp(int fd, struct dfu_file *f) {
    size_t cap = 65536, n = 0;
    char *b = dfu_alloc(cap + 1);
    for (;;) {
        if (n == cap) { cap *= 2; b = dfu_realloc(b, cap + 1); }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            free(b);
            errno = e;
            return -1;
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    b[n] = 0;
    f->buf = b;
    f->len = n;
    return 0;
}

/* a file is binary when its first 4096 bytes hold a NUL (measured: a NUL
 * at offset 4095 makes it binary, at 4096 not, for files and pipes) */
static int dfu_isbinary(const struct dfu_file *f) {
    size_t n = f->len < 4096 ? f->len : 4096;
    return memchr(f->buf, 0, n) != 0;
}

/* split F->buf into lines; with --strip-trailing-cr a CR before a newline
 * is taken out of the text itself (it is gone from the output too; a CR
 * ending an incomplete line stays) */
static void dfu_lines(struct dfu_file *f) {
    free(f->cr);
    f->cr = 0;
    if (dfu_o.strip_cr) {
        /* which lines lose a CR: the files' common ends are found on the
         * bytes as read (measured), CRs and all */
        long nn = 0;
        for (size_t i = 0; i < f->len; i++) if (f->buf[i] == '\n') nn++;
        f->cr = dfu_zalloc((size_t)nn + 2);
        size_t w = 0;
        long ln = 0;
        for (size_t r = 0; r < f->len; r++) {
            if (f->buf[r] == '\r' && r + 1 < f->len && f->buf[r + 1] == '\n') { f->cr[ln] = 1; continue; }
            if (f->buf[r] == '\n') ln++;
            f->buf[w++] = f->buf[r];
        }
        f->len = w;
        f->buf[w] = 0;
    }
    long n = 0;
    for (size_t i = 0; i < f->len; i++) if (f->buf[i] == '\n') n++;
    f->incomplete = f->len && f->buf[f->len - 1] != '\n';
    if (f->incomplete) n++;
    f->nl = n;
    f->lp = dfu_alloc((size_t)(n + 1) * sizeof *f->lp);
    long k = 0;
    const char *p = f->buf, *e = f->buf + f->len;
    while (p < e) {
        f->lp[k++] = p;
        const char *q = memchr(p, '\n', (size_t)(e - p));
        p = q ? q + 1 : e;
    }
    f->lp[k] = e;
}

/* line I's text and length, without its newline */
static size_t dfu_text(const struct dfu_file *f, long i, const char **s) {
    *s = f->lp[i];
    size_t n = (size_t)(f->lp[i + 1] - f->lp[i]);
    if (n && (*s)[n - 1] == '\n') n--;
    return n;
}

static int dfu_isincomplete(const struct dfu_file *f, long i) {
    return f->incomplete && i == f->nl - 1;
}

static void dfu_free_file(struct dfu_file *f) {
    free(f->buf); free(f->lp); free(f->cls); free(f->chg); free(f->cr);
    f->cr = 0;
    f->buf = 0; f->lp = 0; f->cls = 0; f->chg = 0;
}

/* ---- line classes ----------------------------------------------------------
 *
 * Two lines are the same line when what is left of them under -i -E -Z -b
 * -w is the same. The newline is part of a line: an incomplete last line
 * is not the line with a newline -- unless white space at the end is being
 * ignored (-Z, -b, -w), where the missing newline is one more blank. */

static int dfu_isblank_c(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r';
}

/* LINE (LEN bytes, no newline) reduced to what is compared, into OUT */
static size_t dfu_norm(const char *s, size_t len, int nl, char **out, size_t *cap) {
    size_t ts = (size_t)(dfu_o.tabsize > 0 ? dfu_o.tabsize : 8);
    size_t need = len * (dfu_o.itab ? ts : 1) + 2;
    if (*cap < need) { *cap = need * 2; *out = dfu_realloc(*out, *cap); }
    char *o = *out;
    size_t n = 0;
    if (dfu_o.iall) {
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)s[i];
            if (dfu_isblank_c(c)) continue;
            o[n++] = dfu_o.icase ? (char)tolower(c) : (char)c;
        }
        return n;
    }
    if (dfu_o.ispace) {
        size_t i = 0;
        while (i < len) {
            unsigned char c = (unsigned char)s[i];
            if (dfu_isblank_c(c)) {
                size_t j = i;
                while (j < len && dfu_isblank_c((unsigned char)s[j])) j++;
                if (j < len) o[n++] = ' ';
                i = j;
                continue;
            }
            o[n++] = dfu_o.icase ? (char)tolower(c) : (char)c;
            i++;
        }
        return n;
    }
    size_t col = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (dfu_o.itab && c == '\t') {
            size_t to = col + ts - col % ts;
            while (col < to) { o[n++] = ' '; col++; }
            continue;
        }
        o[n++] = dfu_o.icase ? (char)tolower(c) : (char)c;
        if (c == '\b') col = col ? col - 1 : 0;
        else if (c == '\r') col = 0;
        else col++;
    }
    if (dfu_o.itrail) {
        while (n && dfu_isblank_c((unsigned char)o[n - 1])) n--;
        return n;
    }
    if (nl) o[n++] = '\n';
    return n;
}

struct dfu_cent { uint64_t h; size_t off, len; int cls; };
static struct dfu_cent *dfu_ctab;
static size_t dfu_ccap, dfu_cused;
static char *dfu_cstore;
static size_t dfu_cstore_len, dfu_cstore_cap;
static int dfu_ncls;

static uint64_t dfu_hash(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

static void dfu_cls_reset(void) {
    free(dfu_ctab);
    dfu_ccap = 1024;
    dfu_ctab = dfu_alloc(dfu_ccap * sizeof *dfu_ctab);
    for (size_t i = 0; i < dfu_ccap; i++) dfu_ctab[i].cls = -1;
    dfu_cused = 0;
    dfu_cstore_len = 0;
    dfu_ncls = 0;
}

static int dfu_cls_of(const char *s, size_t n) {
    if (dfu_cused * 2 >= dfu_ccap) {
        size_t oc = dfu_ccap;
        struct dfu_cent *ot = dfu_ctab;
        dfu_ccap *= 2;
        dfu_ctab = dfu_alloc(dfu_ccap * sizeof *dfu_ctab);
        for (size_t i = 0; i < dfu_ccap; i++) dfu_ctab[i].cls = -1;
        for (size_t i = 0; i < oc; i++) {
            if (ot[i].cls < 0) continue;
            size_t j = (size_t)ot[i].h & (dfu_ccap - 1);
            while (dfu_ctab[j].cls >= 0) j = (j + 1) & (dfu_ccap - 1);
            dfu_ctab[j] = ot[i];
        }
        free(ot);
    }
    uint64_t h = dfu_hash(s, n);
    size_t j = (size_t)h & (dfu_ccap - 1);
    while (dfu_ctab[j].cls >= 0) {
        if (dfu_ctab[j].h == h && dfu_ctab[j].len == n &&
            !memcmp(dfu_cstore + dfu_ctab[j].off, s, n))
            return dfu_ctab[j].cls;
        j = (j + 1) & (dfu_ccap - 1);
    }
    if (dfu_cstore_len + n + 1 > dfu_cstore_cap) {
        dfu_cstore_cap = (dfu_cstore_len + n + 1) * 2;
        dfu_cstore = dfu_realloc(dfu_cstore, dfu_cstore_cap);
    }
    memcpy(dfu_cstore + dfu_cstore_len, s, n);
    dfu_ctab[j].h = h;
    dfu_ctab[j].off = dfu_cstore_len;
    dfu_ctab[j].len = n;
    dfu_ctab[j].cls = dfu_ncls++;
    dfu_cstore_len += n;
    dfu_cused++;
    return dfu_ctab[j].cls;
}

static void dfu_classify(struct dfu_file *f) {
    char *nb = 0;
    size_t cap = 0;
    free(f->cls);
    f->cls = dfu_alloc((size_t)(f->nl + 1) * sizeof *f->cls);
    for (long i = 0; i < f->nl; i++) {
        const char *s;
        size_t len = dfu_text(f, i, &s);
        int nl = !dfu_isincomplete(f, i) || dfu_o.eol_any;
        size_t n = dfu_norm(s, len, nl, &nb, &cap);
        f->cls[i] = dfu_cls_of(nb, n);
    }
    free(nb);
}

/* ---- the search ------------------------------------------------------------
 *
 * Two sequences of line classes, XV[0..NX) and YV[0..NY), compared with
 * Myers' O(ND) algorithm in its linear-space form: from both corners at
 * once, D-paths grow until a forward and a backward path meet on a
 * diagonal; the point where they meet splits the box in two and each half
 * is compared the same way. What is not on the common subsequence is
 * marked in XC / YC. */

static const int *dfu_xv, *dfu_yv;
static char *dfu_xc, *dfu_yc;
static long *dfu_fv, *dfu_bv;        /* furthest x per diagonal, offset */

#define DFU_NEG (-(1L << 60))
#define DFU_POS (1L << 60)

/* how many rounds a search may take before it settles for the best it
 * has (measured: a split that needs 4096 rounds is still found, past
 * that the result is no longer the minimal one) */
#define DFU_TOO_EXPENSIVE 4096

/* split [xoff,xlim) x [yoff,ylim); *LOMIN / *HIMIN: whether the halves
 * are to be searched to the end (no limit) */
static void dfu_split(long xoff, long xlim, long yoff, long ylim, int minimal,
                      long *xmid, long *ymid, int *lomin, int *himin) {
    long dmin = xoff - ylim, dmax = xlim - yoff;
    long fk0 = xoff - yoff, bk0 = xlim - ylim;
    int odd = (int)((fk0 - bk0) & 1);
    long flo = fk0, fhi = fk0, blo = bk0, bhi = bk0;
    dfu_fv[fk0] = xoff;
    dfu_bv[bk0] = xlim;
    *lomin = *himin = 1;
    for (long d = 1;; d++) {
        /* forward: diagonals fk0-d .. fk0+d, step 2, inside [dmin,dmax] */
        long nlo = fk0 - d, nhi = fk0 + d;
        if (nlo < dmin) nlo += ((dmin - nlo) + 1) / 2 * 2;
        if (nhi > dmax) nhi -= ((nhi - dmax) + 1) / 2 * 2;
        for (long k = nhi; k >= nlo; k -= 2) {
            long r = (k - 1 >= flo && k - 1 <= fhi && dfu_fv[k - 1] != DFU_NEG)
                         ? dfu_fv[k - 1] + 1 : DFU_NEG;
            long dn = (k + 1 >= flo && k + 1 <= fhi) ? dfu_fv[k + 1] : DFU_NEG;
            if (r > xlim) r = DFU_NEG;
            if (dn != DFU_NEG && dn - k > ylim) dn = DFU_NEG;
            long x = r >= dn ? r : dn;
            if (x == DFU_NEG) { dfu_fv[k] = DFU_NEG; continue; }
            long y = x - k;
            while (x < xlim && y < ylim && dfu_xv[x] == dfu_yv[y]) { x++; y++; }
            dfu_fv[k] = x;
            if (odd && k >= blo && k <= bhi && dfu_bv[k] <= x) {
                *xmid = x; *ymid = y;
                return;
            }
        }
        flo = nlo; fhi = nhi;
        /* backward: diagonals bk0-d .. bk0+d */
        nlo = bk0 - d; nhi = bk0 + d;
        if (nlo < dmin) nlo += ((dmin - nlo) + 1) / 2 * 2;
        if (nhi > dmax) nhi -= ((nhi - dmax) + 1) / 2 * 2;
        for (long k = nhi; k >= nlo; k -= 2) {
            long l = (k + 1 >= blo && k + 1 <= bhi && dfu_bv[k + 1] != DFU_POS)
                         ? dfu_bv[k + 1] - 1 : DFU_POS;
            long up = (k - 1 >= blo && k - 1 <= bhi) ? dfu_bv[k - 1] : DFU_POS;
            if (l < xoff) l = DFU_POS;
            if (up != DFU_POS && up - k < yoff) up = DFU_POS;
            long x = l <= up ? l : up;
            if (x == DFU_POS) { dfu_bv[k] = DFU_POS; continue; }
            long y = x - k;
            while (x > xoff && y > yoff && dfu_xv[x - 1] == dfu_yv[y - 1]) { x--; y--; }
            dfu_bv[k] = x;
            if (!odd && k >= flo && k <= fhi && x <= dfu_fv[k]) {
                *xmid = x; *ymid = y;
                return;
            }
        }
        blo = nlo; bhi = nhi;
        if (minimal || d < DFU_TOO_EXPENSIVE) continue;
        /* too long: the point the forward search got furthest to (x+y
         * largest, the first such diagonal from the top), or the
         * backward one (x+y smallest), whichever went further (a tie
         * goes backward); the half behind it is searched to the end, the
         * other with the limit again (measured on random files of 5000
         * to 12000 lines: GNU's choices, edit for edit) */
        long fbest = -1, fbx = xoff;
        for (long k = fhi; k >= flo; k -= 2) {
            if (dfu_fv[k] == DFU_NEG) continue;
            long x = dfu_fv[k], v = 2 * x - k;
            if (v > fbest) { fbest = v; fbx = x; }
        }
        long bbest = DFU_POS, bbx = xlim;
        for (long k = bhi; k >= blo; k -= 2) {
            if (dfu_bv[k] == DFU_POS) continue;
            long x = dfu_bv[k], v = 2 * x - k;
            if (v < bbest) { bbest = v; bbx = x; }
        }
        long fprog = fbest - (xoff + yoff), bprog = (xlim + ylim) - bbest;
        if (fprog > bprog) {
            *xmid = fbx; *ymid = fbest - fbx;
            *lomin = 1; *himin = 0;
        } else {
            *xmid = bbx; *ymid = bbest - bbx;
            *lomin = 0; *himin = 1;
        }
        return;
    }
}

static void dfu_compareseq(long xoff, long xlim, long yoff, long ylim, int minimal) {
    while (xoff < xlim && yoff < ylim && dfu_xv[xoff] == dfu_yv[yoff]) { xoff++; yoff++; }
    while (xlim > xoff && ylim > yoff && dfu_xv[xlim - 1] == dfu_yv[ylim - 1]) { xlim--; ylim--; }
    if (xoff == xlim) {
        while (yoff < ylim) dfu_yc[yoff++] = 1;
    } else if (yoff == ylim) {
        while (xoff < xlim) dfu_xc[xoff++] = 1;
    } else {
        long xm, ym;
        int lomin, himin;
        dfu_split(xoff, xlim, yoff, ylim, minimal, &xm, &ym, &lomin, &himin);
        dfu_compareseq(xoff, xm, yoff, ym, lomin);
        dfu_compareseq(xm, xlim, ym, ylim, himin);
    }
}

/* Where a run of changed lines could as well be somewhere else -- the
 * line before it equals its last line, or the line after it its first --
 * it is slid: up as far as it goes, taking in runs it meets; down as far
 * as it goes, the same; and then back up to the lowest place on the way
 * where the other file has changes opposite it (so a deletion and an
 * insertion meet as one change), if there was one. CLS/CHG are one file's
 * classes and flags, [LO,HI) the part of it that was compared; OCHG and
 * [OLO,OHI) the other file's. */
static void dfu_shift(const int *cls, char *chg, long lo, long hi,
                      const char *ochg, long olo, long ohi) {
    /* the other file's unchanged lines, in order: the U-th unchanged line
     * of this file pairs with OU[U], and the other file's changed lines
     * between OU[U-1] and OU[U] are opposite this file's lines there */
    long nou = 0;
    long *ou = dfu_alloc((size_t)(ohi - olo + 1) * sizeof(long));
    for (long j = olo; j < ohi; j++) if (!ochg[j]) ou[nou++] = j;
#define DFU_OPPOSITE(U) ((U) < nou ? ((U) == 0 ? ou[0] > olo : ou[(U)] > ou[(U) - 1] + 1) \
                                   : ((U) == 0 ? ohi > olo : ohi > ou[(U) - 1] + 1))
    long i = lo, u = 0;      /* u: unchanged lines of this file before i */
    for (;;) {
        while (i < hi && !chg[i]) { i++; u++; }
        if (i >= hi) break;
        long s = i;
        while (i < hi && chg[i]) i++;
        long e = i, len, corr;
        do {
            len = e - s;
            while (s > lo && cls[s - 1] == cls[e - 1]) {
                chg[--s] = 1; chg[--e] = 0;
                u--;
                while (s > lo && chg[s - 1]) s--;
            }
            corr = DFU_OPPOSITE(u) ? e : -1;
            while (e < hi && cls[s] == cls[e]) {
                chg[s++] = 0; chg[e++] = 1;
                u++;
                while (e < hi && chg[e]) e++;
                if (DFU_OPPOSITE(u)) corr = e;
            }
        } while (len != e - s);
        while (corr >= 0 && corr < e) { chg[--s] = 1; chg[--e] = 0; u--; }
        i = e;
    }
#undef DFU_OPPOSITE
    free(ou);
}

/* Which of lines [LO,HI) of one file are left out of the search: FL[i]
 * becomes 1 for a line whose class OCNT says the other file does not
 * have (it cannot be matched), and stays 1 for a line the other file has
 * so many times that matching it would mostly mislead the search -- but
 * only where it sits among lines that are left out anyway. Each rule was
 * found by a pattern of lines no other rule explains (the c's in "1112111"
 * against a file of c's are left out; in "11121111221" they are kept). */
static void dfu_discard(const int *cls, long lo, long hi, const int *ocnt, char *fl) {
    long n = hi - lo;
    /* "many": 5 for a few hundred lines, doubled each time the count of
     * lines grows fourfold past 256 (measured: 10 from 256, 20 from 1024) */
    long many = 5;
    for (long t = n / 256; t > 0; t /= 4) many *= 2;
    for (long i = lo; i < hi; i++) {
        long c = ocnt[cls[i]];
        fl[i] = c == 0 ? 1 : c > many ? 2 : 0;
    }
    for (long i = lo; i < hi;) {
        if (!fl[i]) { i++; continue; }
        long s = i, e = i;
        while (e < hi && fl[e]) e++;
        long next = e;
        /* a run of left-out lines, [s,e). Many-times lines at its ends
         * are kept */
        while (s < e && fl[s] == 2) fl[s++] = 0;
        while (e > s && fl[e - 1] == 2) fl[--e] = 0;
        long len = e - s, prov = 0;
        for (long j = s; j < e; j++) prov += fl[j] == 2;
        /* and all of them are, where they are more than a quarter of it */
        if (prov * 4 > len) {
            for (long j = s; j < e; j++) if (fl[j] == 2) fl[j] = 0;
            i = next;
            continue;
        }
        /* a stretch of many-times lines about as long as the square root
         * of the run is kept whole: 2 under 16 lines, 3 under 64, 5 under
         * 256, 9 ... */
        long minimum = 2;
        for (long t = len / 16; t > 0; t /= 4) minimum = (minimum - 1) * 2 + 1;
        long consec = 0;
        for (long j = s; j < e; j++) {
            if (fl[j] != 2) { consec = 0; continue; }
            if (++consec == minimum) {
                for (long q = j - consec + 1; q <= j; q++) fl[q] = 0;
            } else if (consec > minimum) fl[j] = 0;
        }
        /* near the start of the run, until three left-out lines in a row
         * (or a left-out line eight lines in), many-times lines are kept */
        consec = 0;
        for (long j = s; j < e; j++) {
            if (j - s >= 8 && fl[j] == 1) break;
            if (fl[j] == 2) { consec = 0; fl[j] = 0; }
            else if (fl[j] == 0) consec = 0;
            else if (++consec == 3) break;
        }
        /* and the same from the end */
        consec = 0;
        for (long j = e - 1; j >= s; j--) {
            if (e - 1 - j >= 8 && fl[j] == 1) break;
            if (fl[j] == 2) { consec = 0; fl[j] = 0; }
            else if (fl[j] == 0) consec = 0;
            else if (++consec == 3) break;
        }
        i = next;
    }
}

/* the hunks: runs of changed lines, from line L0 of file 0 and L1 of
 * file 1, DEL and INS lines of each */
struct dfu_chg { long l0, l1, del, ins; int ign; };

static long dfu_horizon;

/* the same bytes, newline and CR and all -- what both files begin and end
 * with is found this way, not by the classes (measured: under -b, "a" and
 * "a " ending the two files are not set aside) */
static int dfu_same_bytes(const struct dfu_file *a, long i, const struct dfu_file *b, long j) {
    const char *sa, *sb;
    size_t na = dfu_text(a, i, &sa), nb = dfu_text(b, j, &sb);
    if (na != nb || memcmp(sa, sb, na)) return 0;
    /* for ed output an incomplete last line has its newline already
     * (measured: -f -w trims such a pair as a common end) */
    if (!dfu_o.eol_any && dfu_isincomplete(a, i) != dfu_isincomplete(b, j)) return 0;
    return (a->cr ? a->cr[i] : 0) == (b->cr ? b->cr[j] : 0);
}

/* F0 against F1 (classified): sets their chg flags; returns the hunks */
static struct dfu_chg *dfu_analyze(struct dfu_file *f0, struct dfu_file *f1, long *nchg) {
    long n0 = f0->nl, n1 = f1->nl;
    free(f0->chg); free(f1->chg);
    f0->chg = dfu_zalloc((size_t)n0 + 1);
    f1->chg = dfu_zalloc((size_t)n1 + 1);
    char *c0 = f0->chg, *c1 = f1->chg;
    /* what both files begin and end with is not looked at again (but
     * --horizon-lines of it are) */
    long pre = 0;
    while (pre < n0 && pre < n1 && dfu_same_bytes(f0, pre, f1, pre)) pre++;
    long suf = 0;
    while (suf < n0 - pre && suf < n1 - pre &&
           dfu_same_bytes(f0, n0 - 1 - suf, f1, n1 - 1 - suf)) suf++;
    pre = pre > dfu_horizon ? pre - dfu_horizon : 0;
    suf = suf > dfu_horizon ? suf - dfu_horizon : 0;
    long a0 = pre, b0 = n0 - suf, a1 = pre, b1 = n1 - suf;
    long m0 = b0 - a0, m1 = b1 - a1;
    char *fl0 = dfu_zalloc((size_t)n0 + 1), *fl1 = dfu_zalloc((size_t)n1 + 1);
    if (!dfu_o.minimal) {
        int *cnt0 = dfu_zalloc((size_t)dfu_ncls * sizeof(int) + 1);
        int *cnt1 = dfu_zalloc((size_t)dfu_ncls * sizeof(int) + 1);
        for (long i = a0; i < b0; i++) cnt0[f0->cls[i]]++;
        for (long i = a1; i < b1; i++) cnt1[f1->cls[i]]++;
        dfu_discard(f0->cls, a0, b0, cnt1, fl0);
        dfu_discard(f1->cls, a1, b1, cnt0, fl1);
        free(cnt0); free(cnt1);
    }
    int *xv = dfu_alloc((size_t)(m0 + 1) * sizeof(int));
    int *yv = dfu_alloc((size_t)(m1 + 1) * sizeof(int));
    long *xi = dfu_alloc((size_t)(m0 + 1) * sizeof(long));
    long *yi = dfu_alloc((size_t)(m1 + 1) * sizeof(long));
    long nx = 0, ny = 0;
    for (long i = a0; i < b0; i++) {
        if (fl0[i]) { c0[i] = 1; continue; }
        xv[nx] = f0->cls[i]; xi[nx++] = i;
    }
    for (long i = a1; i < b1; i++) {
        if (fl1[i]) { c1[i] = 1; continue; }
        yv[ny] = f1->cls[i]; yi[ny++] = i;
    }
    free(fl0); free(fl1);
    char *xc = dfu_zalloc((size_t)nx + 1), *yc = dfu_zalloc((size_t)ny + 1);
    long *fv = dfu_alloc((size_t)(nx + ny + 3) * sizeof(long));
    long *bv = dfu_alloc((size_t)(nx + ny + 3) * sizeof(long));
    dfu_xv = xv; dfu_yv = yv; dfu_xc = xc; dfu_yc = yc;
    dfu_fv = fv + ny + 1; dfu_bv = bv + ny + 1;
    dfu_compareseq(0, nx, 0, ny, dfu_o.minimal);
    for (long i = 0; i < nx; i++) if (xc[i]) c0[xi[i]] = 1;
    for (long i = 0; i < ny; i++) if (yc[i]) c1[yi[i]] = 1;
    free(fv); free(bv); free(xc); free(yc); free(xv); free(yv); free(xi); free(yi);
    dfu_shift(f0->cls, c0, a0, b0, c1, a1, b1);
    dfu_shift(f1->cls, c1, a1, b1, c0, a0, b0);
    /* the hunks */
    long cap = 16, k = 0;
    struct dfu_chg *ch = dfu_alloc((size_t)cap * sizeof *ch);
    long i0 = 0, i1 = 0;
    while (i0 < n0 || i1 < n1) {
        if (i0 < n0 && i1 < n1 && !c0[i0] && !c1[i1]) { i0++; i1++; continue; }
        long s0 = i0, s1 = i1;
        while (i0 < n0 && c0[i0]) i0++;
        while (i1 < n1 && c1[i1]) i1++;
        if (i0 == s0 && i1 == s1) break;     /* cannot happen: the flags pair up */
        if (k == cap) { cap *= 2; ch = dfu_realloc(ch, (size_t)cap * sizeof *ch); }
        ch[k].l0 = s0; ch[k].l1 = s1;
        ch[k].del = i0 - s0; ch[k].ins = i1 - s1;
        ch[k].ign = 0;
        k++;
    }
    *nchg = k;
    return ch;
}

/* ======================================================================
 * diff
 * ====================================================================== */

enum { DFS_NORMAL, DFS_CONTEXT, DFS_UNIFIED, DFS_ED, DFS_FED, DFS_RCS, DFS_IFDEF, DFS_SIDE };

static struct {
    int style;                  /* -1 until an option chooses one */
    long context;               /* -1: none given */
    int brief, report_same, expand_tabs, initial_tab, sbe, paginate;
    long width;
    int left_col, suppress_common, sdiff_assist, inhibit_merge;
    regex_t *fre; int nfre;     /* -F / -p */
    regex_t *ire; int nire;     /* -I */
    int iblank;
    const char *label[2]; int nlabel;
    const char *gfmt[4];        /* old, new, unchanged, changed group formats */
    const char *lfmt[3];        /* old, new, unchanged line formats */
    int color;
    const char *pal_ad, *pal_de, *pal_hd, *pal_ln, *pal_rs;
    int recursive, noderef, newfile, unidir, icasefn;
    char **excl; int nexcl;
    const char *start;
    const char *from, *to;
    char *switches;             /* echoed in "diff -r ..." lines */
    int any_ign;                /* -B or -I given */
} dfx;

static int dfu_status;          /* the worst so far */
static void dfu_st(int s) { if (s > dfu_status) dfu_status = s; }

/* ---- output: straight to stdout, or gathered for --paginate ---------- */

static struct { char *b; size_t n, cap; int on; } dfu_pg;

static void dfu_w(const char *p, size_t n) {
    if (!n) return;
    if (dfu_pg.on) {
        if (dfu_pg.n + n > dfu_pg.cap) {
            dfu_pg.cap = (dfu_pg.n + n) * 2 + 1024;
            dfu_pg.b = dfu_realloc(dfu_pg.b, dfu_pg.cap);
        }
        memcpy(dfu_pg.b + dfu_pg.n, p, n);
        dfu_pg.n += n;
    } else fwrite(p, 1, n, stdout);
}
static void dfu_c(int c) { char ch = (char)c; dfu_w(&ch, 1); }
static void dfu_s(const char *s) { dfu_w(s, strlen(s)); }
static void dfu_f(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dfu_f(const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { dfu_w(tmp, (size_t)n); return; }
    char *big = dfu_alloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    dfu_w(big, (size_t)n);
    free(big);
}

/* --color: "\e[" CODE "m" ... "\e[" RS "m" around a line */
static void dfu_col_on(const char *code) {
    if (!dfx.color || !code) return;
    dfu_s("\033["); dfu_s(code); dfu_c('m');
}
static void dfu_col_off(const char *code) {
    if (!dfx.color || !code) return;
    dfu_s("\033["); dfu_s(dfx.pal_rs); dfu_c('m');
}

/* the width rules of --expand-tabs and side by side: a tab to the next
 * stop, a backspace back one, a CR to the start, other control bytes
 * nothing, everything else (8-bit bytes too) one column */
static size_t dfu_expand(const char *s, size_t len, size_t col0) {
    size_t ts = (size_t)dfu_o.tabsize, col = col0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t') {
            size_t to = col + ts - col % ts;
            while (col < to) { dfu_c(' '); col++; }
            continue;
        }
        dfu_c(c);
        if (c == '\b') { if (col) col--; }
        else if (c == '\r') col = 0;
        else if (c < 0x20 || c == 0x7f) {}
        else col++;
    }
    return col;
}

/* one line of normal, context or unified output: MARK, the separator,
 * the text, the newline -- and the "\ No newline" line after an
 * incomplete one. --suppress-blank-empty leaves only a mark that is not
 * a space before an empty line. */
static void dfu_pline(const char *mark, const char *sep, const struct dfu_file *f, long i,
                      const char *color) {
    const char *s;
    size_t n = dfu_text(f, i, &s);
    dfu_col_on(color);
    if (n == 0 && dfx.sbe) {
        if (*mark && *mark != ' ') dfu_s(mark);
    } else {
        dfu_s(mark);
        dfu_s(sep);
        if (dfx.expand_tabs) dfu_expand(s, n, 0);
        else dfu_w(s, n);
    }
    dfu_col_off(color);
    dfu_c('\n');
    if (dfu_isincomplete(f, i)) dfu_s("\\ No newline at end of file\n");
}

/* ---- the hunks to show ------------------------------------------------ */

static int dfu_line_ignorable(const struct dfu_file *f, long i) {
    const char *s;
    size_t n = dfu_text(f, i, &s);
    if (dfx.iblank) {
        /* blank: empty -- or only white space, when white space is being
         * ignored at the end anyway (-b -w -Z) */
        size_t k = 0;
        if (dfu_o.ispace || dfu_o.iall || dfu_o.itrail)
            while (k < n && isspace((unsigned char)s[k])) k++;
        if (k == n) return 1;
    }
    if (dfx.nire) {
        char *t = dfu_alloc(n + 1);
        memcpy(t, s, n);
        t[n] = 0;
        int hit = 0;
        for (int r = 0; r < dfx.nire && !hit; r++)
            hit = regexec(&dfx.ire[r], t, 0, 0, 0) == 0;
        free(t);
        if (hit) return 1;
    }
    return 0;
}

/* a hunk is ignored when every line it deletes or inserts is (-B -I) */
static void dfu_mark_ignorable(const struct dfu_file *f0, const struct dfu_file *f1,
                               struct dfu_chg *ch, long n) {
    for (long k = 0; k < n; k++) {
        ch[k].ign = 0;
        if (!dfx.any_ign) continue;
        int ign = 1;
        for (long i = 0; i < ch[k].del && ign; i++) ign = dfu_line_ignorable(f0, ch[k].l0 + i);
        for (long i = 0; i < ch[k].ins && ign; i++) ign = dfu_line_ignorable(f1, ch[k].l1 + i);
        ch[k].ign = ign;
    }
}

/* ---- normal ----------------------------------------------------------- */

static void dfu_range_n(char *b, long a, long n, int sep) {   /* lines a+1 .. a+n */
    if (n <= 1) sprintf(b, "%ld", n == 1 ? a + 1 : a);
    else sprintf(b, "%ld%c%ld", a + 1, sep, a + n);
}

static void dfu_out_normal(const struct dfu_file *f0, const struct dfu_file *f1,
                           const struct dfu_chg *ch, long n) {
    const char *sep = dfx.initial_tab ? "\t" : " ";
    for (long k = 0; k < n; k++) {
        const struct dfu_chg *c = &ch[k];
        if (c->ign) continue;
        char r0[64], r1[64];
        dfu_range_n(r0, c->l0, c->del, ',');
        dfu_range_n(r1, c->l1, c->ins, ',');
        dfu_col_on(dfx.pal_ln);
        if (c->del && c->ins) dfu_f("%sc%s", r0, r1);
        else if (c->del) dfu_f("%sd%ld", r0, c->l1);
        else dfu_f("%lda%s", c->l0, r1);
        dfu_col_off(dfx.pal_ln);
        dfu_c('\n');
        for (long i = 0; i < c->del; i++) dfu_pline("<", sep, f0, c->l0 + i, dfx.pal_de);
        if (c->del && c->ins) dfu_s("---\n");
        for (long i = 0; i < c->ins; i++) dfu_pline(">", sep, f1, c->l1 + i, dfx.pal_ad);
    }
}

/* ---- ed, forward ed, RCS ---------------------------------------------- */

/* the new lines of an ed hunk; a line that is just "." is written ".."
 * and put right with s/.// after the text ends, then "a" goes on */
static void dfu_ed_text(const struct dfu_file *f1, long from, long n) {
    int open = 1;
    for (long i = 0; i < n; i++) {
        const char *s;
        size_t len = dfu_text(f1, from + i, &s);
        if (!open) { dfu_s("a\n"); open = 1; }
        if (len == 1 && s[0] == '.') {
            dfu_s("..\n.\ns/.//\n");
            open = 0;
            continue;
        }
        dfu_w(s, len);
        dfu_c('\n');
    }
    if (open) dfu_s(".\n");
}

static void dfu_out_ed(const struct dfu_file *f1, const struct dfu_chg *ch, long n) {
    for (long k = n - 1; k >= 0; k--) {
        const struct dfu_chg *c = &ch[k];
        if (c->ign) continue;
        char r0[64];
        dfu_range_n(r0, c->l0, c->del, ',');
        if (c->del && c->ins) { dfu_f("%sc\n", r0); dfu_ed_text(f1, c->l1, c->ins); }
        else if (c->del) dfu_f("%sd\n", r0);
        else { dfu_f("%lda\n", c->l0); dfu_ed_text(f1, c->l1, c->ins); }
    }
}

static void dfu_out_fed(const struct dfu_file *f1, const struct dfu_chg *ch, long n) {
    for (long k = 0; k < n; k++) {
        const struct dfu_chg *c = &ch[k];
        if (c->ign) continue;
        char r0[64];
        dfu_range_n(r0, c->l0, c->del, ' ');
        if (c->del && c->ins) dfu_f("c%s\n", r0);
        else if (c->del) { dfu_f("d%s\n", r0); continue; }
        else dfu_f("a%ld\n", c->l0);
        for (long i = 0; i < c->ins; i++) {
            const char *s;
            size_t len = dfu_text(f1, c->l1 + i, &s);
            dfu_w(s, len);
            dfu_c('\n');
        }
        dfu_s(".\n");
    }
}

static void dfu_out_rcs(const struct dfu_file *f1, const struct dfu_chg *ch, long n) {
    for (long k = 0; k < n; k++) {
        const struct dfu_chg *c = &ch[k];
        if (c->ign) continue;
        if (c->del) dfu_f("d%ld %ld\n", c->l0 + 1, c->del);
        if (c->ins) {
            dfu_f("a%ld %ld\n", c->l0 + c->del, c->ins);
            for (long i = 0; i < c->ins; i++)
                dfu_w(f1->lp[c->l1 + i], (size_t)(f1->lp[c->l1 + i + 1] - f1->lp[c->l1 + i]));
        }
    }
}

/* ---- context and unified ------------------------------------------------ */

/* the last line of file 0 before line LO that a -F/-p expression matches,
 * as shown after the hunk header: leading blanks dropped, 40 bytes */
static void dfu_funline(const struct dfu_file *f0, long lo) {
    if (!dfx.nfre) return;
    for (long i = lo - 1; i >= 0; i--) {
        const char *s;
        size_t n = dfu_text(f0, i, &s);
        char *t = dfu_alloc(n + 1);
        memcpy(t, s, n);
        t[n] = 0;
        int hit = 0;
        for (int r = dfx.nfre - 1; r >= 0 && !hit; r--)
            hit = regexec(&dfx.fre[r], t, 0, 0, 0) == 0;
        free(t);
        if (!hit) continue;
        size_t k = 0;
        while (k < n && isspace((unsigned char)s[k])) k++;
        size_t m = n - k > 40 ? 40 : n - k;
        /* and blanks at the end of those 40 bytes dropped (measured) */
        while (m && isspace((unsigned char)s[k + m - 1])) m--;
        dfu_c(' ');
        dfu_w(s + k, m);
        return;
    }
}

/* the hunks shown together: from K, each next one whose gap to the one
 * before is at most 2*CONTEXT lines -- or, for one whose lines are all
 * ignored (-B -I), under CONTEXT, so that it shows only where it falls in
 * the context anyway (measured). Returns the last; *SHOW: whether one of
 * them is not ignored (a group of ignored hunks only is not shown). */
static long dfu_group(const struct dfu_chg *ch, long n, long k, long ctx, int *show) {
    long e = k;
    *show = !ch[k].ign;
    while (e + 1 < n && !dfx.inhibit_merge) {
        long gap = ch[e + 1].l0 - (ch[e].l0 + ch[e].del);
        if (ch[e + 1].ign ? gap >= ctx : gap > 2 * ctx) break;
        e++;
        if (!ch[e].ign) *show = 1;
    }
    return e;
}

static void dfu_urange(long lo, long hi) {
    long n = hi - lo;
    if (n == 1) dfu_f("%ld", lo + 1);
    else if (n == 0) dfu_f("%ld,0", lo);
    else dfu_f("%ld,%ld", lo + 1, n);
}

static void dfu_crange(long lo, long hi) {
    long n = hi - lo;
    if (n <= 1) dfu_f("%ld", hi);
    else dfu_f("%ld,%ld", lo + 1, hi);
}

static void dfu_out_context(const struct dfu_file *f0, const struct dfu_file *f1,
                            const struct dfu_chg *ch, long n, int unified) {
    long ctx = dfx.context;
    const char *sep = dfx.initial_tab ? "\t" : " ";
    for (long k = 0; k < n;) {
        int show;
        long e = dfu_group(ch, n, k, ctx, &show);
        if (!show) { k = e + 1; continue; }
        const struct dfu_chg *a = &ch[k], *z = &ch[e];
        long lo0 = a->l0 - ctx < 0 ? 0 : a->l0 - ctx;
        long hi0 = z->l0 + z->del + ctx > f0->nl ? f0->nl : z->l0 + z->del + ctx;
        long lo1 = a->l1 - (a->l0 - lo0);
        long hi1 = z->l1 + z->ins + (hi0 - (z->l0 + z->del));
        if (unified) {
            dfu_col_on(dfx.pal_ln);
            dfu_s("@@ -"); dfu_urange(lo0, hi0);
            dfu_s(" +"); dfu_urange(lo1, hi1);
            dfu_s(" @@");
            dfu_funline(f0, lo0);
            dfu_col_off(dfx.pal_ln);
            dfu_c('\n');
            long i0 = lo0;
            /* -T: a tab after the mark, and none for context lines */
            const char *csep = dfx.initial_tab ? "\t" : "";
            const char *cmark = dfx.initial_tab ? "" : " ";
            for (long h = k; h <= e + 1; h++) {
                long to0 = h <= e ? ch[h].l0 : hi0;
                while (i0 < to0) { dfu_pline(cmark, csep, f0, i0, 0); i0++; }
                if (h > e) break;
                for (long i = 0; i < ch[h].del; i++) dfu_pline("-", csep, f0, ch[h].l0 + i, dfx.pal_de);
                for (long i = 0; i < ch[h].ins; i++) dfu_pline("+", csep, f1, ch[h].l1 + i, dfx.pal_ad);
                i0 = ch[h].l0 + ch[h].del;
            }
        } else {
            dfu_s("***************");
            dfu_funline(f0, lo0);
            dfu_c('\n');
            int dels = 0, inss = 0;
            for (long h = k; h <= e; h++) { if (ch[h].del) dels = 1; if (ch[h].ins) inss = 1; }
            dfu_col_on(dfx.pal_ln);
            dfu_s("*** "); dfu_crange(lo0, hi0); dfu_s(" ****");
            dfu_col_off(dfx.pal_ln);
            dfu_c('\n');
            if (dels) {
                long h = k;
                for (long i = lo0; i < hi0; i++) {
                    while (h <= e && i >= ch[h].l0 + ch[h].del) h++;
                    const char *m = " ";
                    if (h <= e && i >= ch[h].l0) m = ch[h].ins ? "!" : "-";
                    dfu_pline(m, sep, f0, i, dfx.pal_de);
                }
            }
            dfu_col_on(dfx.pal_ln);
            dfu_s("--- "); dfu_crange(lo1, hi1); dfu_s(" ----");
            dfu_col_off(dfx.pal_ln);
            dfu_c('\n');
            if (inss) {
                long h = k;
                for (long i = lo1; i < hi1; i++) {
                    while (h <= e && i >= ch[h].l1 + ch[h].ins) h++;
                    const char *m = " ";
                    if (h <= e && i >= ch[h].l1) m = ch[h].del ? "!" : "+";
                    dfu_pline(m, sep, f1, i, dfx.pal_ad);
                }
            }
        }
        k = e + 1;
    }
}

/* ---- if-then-else: -D and the group and line formats ------------------ */

/* a group: lines [B0,E0) of file 0 and [B1,E1) of file 1 */
struct dfu_grp { const struct dfu_file *f0, *f1; long b0, e0, b1, e1; };

static long dfu_gval(const struct dfu_grp *g, int letter) {
    switch (letter) {
    case 'e': return g->b0;
    case 'f': return g->b0 + 1;
    case 'l': return g->e0;
    case 'm': return g->e0 + 1;
    case 'n': return g->e0 - g->b0;
    case 'E': return g->b1;
    case 'F': return g->b1 + 1;
    case 'L': return g->e1;
    case 'M': return g->e1 + 1;
    case 'N': return g->e1 - g->b1;
    }
    return 0;
}

/* a printf spec at P ("%[-'0][WIDTH][.[PREC]]{doxX}"), followed by one of
 * LETTERS: its length, or 0 when it is not one; *SPEC gets the C format */
static size_t dfu_pspec(const char *p, const char *letters, char *spec, int *letter) {
    const char *q = p + 1;
    size_t n = 0;
    spec[n++] = '%';
    while (*q == '-' || *q == '\'' || *q == '0') {
        if (*q != '\'' && n < 8) spec[n++] = *q;
        q++;
    }
    while (isdigit((unsigned char)*q)) { if (n < 20) spec[n++] = *q; q++; }
    if (*q == '.') {
        spec[n++] = *q++;
        while (isdigit((unsigned char)*q)) { if (n < 30) spec[n++] = *q; q++; }
    }
    if (!*q || !strchr("doxX", *q)) return 0;
    spec[n++] = 'l';
    spec[n++] = *q++;
    spec[n] = 0;
    if (!*q || !strchr(letters, *q)) return 0;
    *letter = *q;
    return (size_t)(q + 1 - p);
}

/* %c'C' or %c'\OOO' at P: its length and *C, or 0 */
static size_t dfu_cspec(const char *p, int *c) {
    if (p[1] != 'c' || p[2] != '\'') return 0;
    const char *q = p + 3;
    if (*q == '\\') {
        q++;
        int v = 0, k = 0;
        while (k < 3 && *q >= '0' && *q <= '7') { v = v * 8 + (*q - '0'); q++; k++; }
        if (!k) return 0;
        *c = v;
    } else {
        if (!*q || *q == '\'') return 0;
        *c = (unsigned char)*q++;
    }
    if (*q != '\'') return 0;
    return (size_t)(q + 1 - p);
}

static void dfu_lines_fmt(const struct dfu_file *f, long b, long e, int which);

/* print group format FMT up to an unquoted END character (or the end of
 * the string); returns where it stopped. DO: print, or only skip. */
static const char *dfu_gfmt_run(const char *fmt, int end, const struct dfu_grp *g, int doit) {
    const char *p = fmt;
    while (*p && *p != end) {
        if (*p != '%') { if (doit) dfu_c(*p); p++; continue; }
        if (p[1] == '%') { if (doit) dfu_c('%'); p += 2; continue; }
        if (p[1] == '<') { if (doit) dfu_lines_fmt(g->f0, g->b0, g->e0, 0); p += 2; continue; }
        if (p[1] == '>') { if (doit) dfu_lines_fmt(g->f1, g->b1, g->e1, 1); p += 2; continue; }
        if (p[1] == '=') { if (doit) dfu_lines_fmt(g->f0, g->b0, g->e0, 2); p += 2; continue; }
        int c;
        size_t k = dfu_cspec(p, &c);
        if (k) { if (doit) dfu_c(c); p += k; continue; }
        if (p[1] == '(') {
            /* %(A=B?T:E) -- A and B a number or a letter */
            const char *q = p + 2;
            long v[2];
            int ok = 1;
            for (int s = 0; s < 2 && ok; s++) {
                if (isdigit((unsigned char)*q)) {
                    long x = 0;
                    while (isdigit((unsigned char)*q)) x = x * 10 + (*q++ - '0');
                    v[s] = x;
                } else if (*q && strchr("eflmnEFLMN", *q)) v[s] = dfu_gval(g, *q++);
                else ok = 0;
                if (ok && *q != (s ? '?' : '=')) ok = 0;
                if (ok) q++;
            }
            if (ok) {
                int yes = v[0] == v[1];
                q = dfu_gfmt_run(q, ':', g, doit && yes);
                if (*q == ':') q = dfu_gfmt_run(q + 1, ')', g, doit && !yes);
                if (*q == ')') q++;
                p = q;
                continue;
            }
        }
        char spec[40];
        int letter;
        k = dfu_pspec(p, "eflmnEFLMN", spec, &letter);
        if (k) { if (doit) dfu_f(spec, dfu_gval(g, letter)); p += k; continue; }
        /* not a directive: written as it is */
        if (doit) dfu_c('%');
        p++;
    }
    return p;
}

/* LINE format for each of lines [B,E) of F; WHICH: 0 old, 1 new, 2 same */
static void dfu_lines_fmt(const struct dfu_file *f, long b, long e, int which) {
    const char *fmt = dfx.lfmt[which];
    for (long i = b; i < e; i++) {
        for (const char *p = fmt; *p;) {
            if (*p != '%') { dfu_c(*p); p++; continue; }
            if (p[1] == '%') { dfu_c('%'); p += 2; continue; }
            if (p[1] == 'l' || p[1] == 'L') {
                const char *s;
                size_t n = dfu_text(f, i, &s);
                dfu_w(s, n);
                if (p[1] == 'L' && !dfu_isincomplete(f, i)) dfu_c('\n');
                p += 2;
                continue;
            }
            int c;
            size_t k = dfu_cspec(p, &c);
            if (k) { dfu_c(c); p += k; continue; }
            char spec[40];
            int letter;
            k = dfu_pspec(p, "n", spec, &letter);
            if (k) { dfu_f(spec, i + 1); p += k; continue; }
            dfu_c('%');
            p++;
        }
    }
}

static void dfu_out_ifdef(const struct dfu_file *f0, const struct dfu_file *f1,
                          const struct dfu_chg *ch, long n) {
    struct dfu_grp g = { f0, f1, 0, 0, 0, 0 };
    long i0 = 0, i1 = 0;
    for (long k = 0; k <= n; k++) {
        if (k < n && ch[k].ign) continue;
        long to0 = k < n ? ch[k].l0 : f0->nl, to1 = k < n ? ch[k].l1 : f1->nl;
        if (to0 > i0 || to1 > i1) {
            g.b0 = i0; g.e0 = to0; g.b1 = i1; g.e1 = to1;
            dfu_gfmt_run(dfx.gfmt[2], 0, &g, 1);
        }
        if (k == n) break;
        g.b0 = ch[k].l0; g.e0 = ch[k].l0 + ch[k].del;
        g.b1 = ch[k].l1; g.e1 = ch[k].l1 + ch[k].ins;
        dfu_gfmt_run(dfx.gfmt[ch[k].del && ch[k].ins ? 3 : ch[k].del ? 0 : 1], 0, &g, 1);
        i0 = g.e0; i1 = g.e1;
    }
}

/* ---- side by side ------------------------------------------------------ */

static long dfu_sb_hw, dfu_sb_off, dfu_sb_mk;
static int dfu_sb_ts;           /* the layout's tab size: 1 with -t */

/* the layout, measured over widths 1..200 and tab sizes 1..13: the right
 * column starts at a tab stop near the middle, the gutter is 3 wide at
 * least, the mark sits in its middle */
static void dfu_sb_layout(void) {
    long t = dfx.expand_tabs ? 1 : dfu_o.tabsize, w = dfx.width;
    dfu_sb_ts = (int)t;
    long off = t * ((w + t + 3) / (2 * t));
    long hw = off - 3 < w - off ? off - 3 : w - off;
    if (hw <= 0) { hw = 0; off = w; }
    dfu_sb_hw = hw;
    dfu_sb_off = off;
    dfu_sb_mk = (hw + off - 1) / 2;
}

/* blanks from column FROM to column TO: tabs while a stop is in reach */
static void dfu_sb_pad(long from, long to) {
    if (!dfx.expand_tabs) {
        for (;;) {
            long next = from + dfu_sb_ts - from % dfu_sb_ts;
            if (next > to) break;
            dfu_c('\t');
            from = next;
        }
    }
    while (from < to) { dfu_c(' '); from++; }
}

/* the text of a line in a column of dfu_sb_hw columns; the column it
 * ends in. RIGHT: a CR goes back to the start of the line, so the right
 * column is padded to again. */
static long dfu_sb_text(const char *s, size_t len, int right) {
    long col = 0, hw = dfu_sb_hw, ts = dfu_o.tabsize;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t') {
            long to = col + ts - col % ts;
            /* a tab that would reach the column's end is not written
             * (measured: "123456789012345\t" shows no tab at width 16) */
            if (to >= hw) {
                if (dfx.expand_tabs) while (col < hw) { dfu_c(' '); col++; }
                break;
            }
            if (dfx.expand_tabs) while (col < to) { dfu_c(' '); col++; }
            else dfu_c('\t');
            col = to;
        } else if (c == '\b') {
            dfu_c(c);
            if (col) col--;
        } else if (c == '\r') {
            dfu_c(c);
            col = 0;
            if (right) dfu_sb_pad(0, dfu_sb_off);
        } else if (c < 0x20 || c == 0x7f) {
            dfu_c(c);
        } else {
            if (col + 1 > hw) break;
            dfu_c(c);
            col++;
        }
    }
    return col;
}

/* one output line: L (or none), the MARK, R (or none) */
static void dfu_sb_line(const struct dfu_file *f0, long i0, int mark,
                        const struct dfu_file *f1, long i1) {
    const char *ls = 0, *rs = 0;
    size_t ln = 0, rn = 0;
    if (f0) ln = dfu_text(f0, i0, &ls);
    if (f1) rn = dfu_text(f1, i1, &rs);
    int linc = f0 && dfu_isincomplete(f0, i0), rinc = f1 && dfu_isincomplete(f1, i1);
    const char *color = mark == '<' ? dfx.pal_de : mark == '>' ? dfx.pal_ad : 0;
    if (mark == '|' && linc != rinc) mark = linc ? '\\' : '/';
    dfu_col_on(color);
    long col = f0 ? dfu_sb_text(ls, ln, 0) : 0;
    if (mark == ' ') {
        if (f1 && rn) {
            dfu_sb_pad(col, dfu_sb_off);
            dfu_sb_text(rs, rn, 1);
        }
    } else {
        dfu_sb_pad(col, dfu_sb_mk);
        dfu_c(mark);
        if (f1 && rn) {
            dfu_sb_pad(dfu_sb_mk + 1, dfu_sb_off);
            dfu_sb_text(rs, rn, 1);
        }
    }
    int nl;
    if (f0 && f1) nl = !(linc && rinc);
    else nl = f0 ? !linc : !rinc;
    if (nl) dfu_c('\n');
    if (color && dfx.color) dfu_col_off(color);
}

/* lines in common between the hunks shown: side by side as they come;
 * left over on one side (hunks ignored by -B -I make the sides uneven):
 * '(' or ')' */
static void dfu_sb_common(const struct dfu_file *f0, long a0, long z0,
                          const struct dfu_file *f1, long a1, long z1) {
    if (dfx.suppress_common) return;
    while (a0 < z0 && a1 < z1) {
        if (dfx.left_col) dfu_sb_line(f0, a0, '(', 0, 0);
        else dfu_sb_line(f0, a0, ' ', f1, a1);
        a0++; a1++;
    }
    for (; a0 < z0; a0++) dfu_sb_line(f0, a0, '(', 0, 0);
    if (!dfx.left_col)
        for (; a1 < z1; a1++) dfu_sb_line(0, 0, ')', f1, a1);
}

static void dfu_out_side(const struct dfu_file *f0, const struct dfu_file *f1,
                         const struct dfu_chg *ch, long n) {
    dfu_sb_layout();
    long i0 = 0, i1 = 0;
    for (long k = 0; k < n; k++) {
        const struct dfu_chg *c = &ch[k];
        if (c->ign) continue;
        /* --sdiff-merge-assist (what sdiff -o reads): each run of common
         * lines and each hunk announced by its line counts */
        if (dfx.sdiff_assist && (c->l0 > i0 || c->l1 > i1)) dfu_f("i%ld,%ld\n", c->l0 - i0, c->l1 - i1);
        dfu_sb_common(f0, i0, c->l0, f1, i1, c->l1);
        if (dfx.sdiff_assist) dfu_f("c%ld,%ld\n", c->del, c->ins);
        long both = c->del < c->ins ? c->del : c->ins;
        for (long j = 0; j < both; j++) dfu_sb_line(f0, c->l0 + j, '|', f1, c->l1 + j);
        for (long j = both; j < c->del; j++) dfu_sb_line(f0, c->l0 + j, '<', 0, 0);
        for (long j = both; j < c->ins; j++) dfu_sb_line(0, 0, '>', f1, c->l1 + j);
        i0 = c->l0 + c->del;
        i1 = c->l1 + c->ins;
    }
    if (dfx.sdiff_assist && (f0->nl > i0 || f1->nl > i1)) dfu_f("i%ld,%ld\n", f0->nl - i0, f1->nl - i1);
    dfu_sb_common(f0, i0, f0->nl, f1, i1, f1->nl);
}

/* ---- --paginate: what pr does with it ----------------------------------
 * Pages of 66 lines: two blank lines, the header (the date, the title
 * centred, "Page N", 72 columns), two blank lines, 56 lines of text, five
 * blank lines; the last page is filled out. Each compared pair is one
 * pr run, its "diff ..." line the title (measured: GNU runs pr on each). */

/* the pages go to file descriptor 1 directly, as pr's do: diff's own
 * lines ("Only in ...") wait in its buffer meanwhile (measured: with -rl
 * they come after the pages) */
static void dfu_wr1(const char *p, size_t n) {
    while (n) {
        ssize_t r = write(1, p, n);
        if (r < 0) { if (errno == EINTR) continue; return; }
        p += r;
        n -= (size_t)r;
    }
}

static void dfu_paginate(const char *title, const char *b, size_t n) {
    int64_t now_s;
    long now_ns;
    tx_now(&now_s, &now_ns);
    struct tx_tm tm;
    tx_tz_local(tx_tz_cur(), now_s, now_ns, &tm);
    char date[64];
    snprintf(date, sizeof date, "%04lld-%02d-%02d %02d:%02d", (long long)tm.year, tm.mon,
             tm.mday, tm.hour, tm.min);
    size_t pos = 0;
    long page = 0;
    size_t hcap = strlen(title) + 256;
    char *hdr = dfu_alloc(hcap);
    static const char blanks[] = "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
                                 "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n";
    while (pos < n) {
        page++;
        char pg[32];
        snprintf(pg, sizeof pg, "Page %ld", page);
        long pad = 72 - (long)strlen(date) - (long)strlen(title) - (long)strlen(pg);
        long lp = pad / 2, rp = pad - lp;
        if (lp < 1) lp = 1;
        if (rp < 1) rp = 1;
        int hl = snprintf(hdr, hcap, "\n\n%s%*s%s%*s%s\n\n\n", date, (int)lp, "", title, (int)rp, "", pg);
        dfu_wr1(hdr, (size_t)hl);
        int lines = 0;
        while (lines < 56 && pos < n) {
            const char *nl = memchr(b + pos, '\n', n - pos);
            size_t e = nl ? (size_t)(nl - b) + 1 : n;
            dfu_wr1(b + pos, e - pos);
            if (!nl) dfu_wr1("\n", 1);
            pos = e;
            lines++;
        }
        dfu_wr1(blanks, (size_t)(56 - lines));
        dfu_wr1(blanks, 5);
    }
    free(hdr);
}

/* ---- timestamps in the context and unified headers --------------------- */

static void dfu_header_line(const char *mark, const struct dfu_file *f, int unified) {
    dfu_col_on(dfx.pal_hd);
    dfu_s(mark);
    dfu_c(' ');
    if (f->label) {
        dfu_s(f->label);
    } else {
        dfu_s(dfu_qc(f->name));
        int64_t s;
        long ns;
        if (f->isstdin) {
            /* standard input: the time of the comparison */
            tx_now(&s, &ns);
        } else if (f->missing || !f->st_ok) {
            s = 0; ns = 0;
        } else {
            s = (int64_t)f->st.st_mtime;
#ifdef SU_HOST
            ns = f->st.st_mtim.tv_nsec;
#else
            ns = 0;
#endif
        }
        struct tx_tm tm;
        tx_tz_local(tx_tz_cur(), s, ns, &tm);
        static const char *const wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        static const char *const mn[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        if (unified) {
            long off = tm.gmtoff;
            char sg = off < 0 ? '-' : '+';
            if (off < 0) off = -off;
            dfu_f("\t%04lld-%02d-%02d %02d:%02d:%02d.%09ld %c%02ld%02ld", (long long)tm.year,
                  tm.mon, tm.mday, tm.hour, tm.min, tm.sec, tm.nsec, sg, off / 3600, off / 60 % 60);
        } else {
            dfu_f("\t%s %s %2d %02d:%02d:%02d %lld", wd[tm.wday], mn[tm.mon - 1], tm.mday,
                  tm.hour, tm.min, tm.sec, (long long)tm.year);
        }
    }
    dfu_col_off(dfx.pal_hd);
    dfu_c('\n');
}

/* ---- one pair of files ------------------------------------------------- */

static const char *dfu_ftype(const struct stat *st) {
    if (S_ISREG(st->st_mode)) return st->st_size ? "regular file" : "regular empty file";
    if (S_ISDIR(st->st_mode)) return "directory";
    if (S_ISLNK(st->st_mode)) return "symbolic link";
    if (S_ISFIFO(st->st_mode)) return "fifo";
    if (S_ISSOCK(st->st_mode)) return "socket";
    if (S_ISCHR(st->st_mode)) return "character special file";
    if (S_ISBLK(st->st_mode)) return "block special file";
    return "weird file";
}

/* the content of F (not a missing one): 0, or -1 with errno */
static int dfu_load(struct dfu_file *f) {
    if (f->missing) { f->buf = dfu_zalloc(1); f->len = 0; return 0; }
    if (f->buf) return 0;
    int fd = f->isstdin ? 0 : su_open_read(f->name, 0);
    if (fd < 0) return -1;
    int r = dfu_slurp(fd, f);
    int e = errno;
    if (!f->isstdin) close(fd);
    errno = e;
    return r;
}

static const char *dfu_shown(const struct dfu_file *f) { return f->label ? f->label : f->name; }

/* "diff SWITCHES A B", for a pair inside a directory and for --paginate */
static char *dfu_title(const struct dfu_file *f0, const struct dfu_file *f1) {
    const char *a = dfu_qc(dfu_shown(f0)), *b = dfu_qc(dfu_shown(f1));
    size_t n = 4 + strlen(dfx.switches) + strlen(a) + strlen(b) + 4;
    char *t = dfu_alloc(n);
    snprintf(t, n, "diff%s %s %s", dfx.switches, a, b);
    return t;
}

static int dfu_nohdr;

static int dfu_eq_bytes(const struct dfu_file *f0, const struct dfu_file *f1) {
    return f0->len == f1->len && !memcmp(f0->buf, f1->buf, f0->len);
}

/* the options that make two different files compare the same */
static int dfu_loose(void) {
    return dfu_o.icase || dfu_o.itab || dfu_o.itrail || dfu_o.ispace || dfu_o.iall ||
           dfu_o.strip_cr || dfx.any_ign;
}

static void dfu_files(struct dfu_file *f0, struct dfu_file *f1, int depth) {
    int lines_out = dfx.style == DFS_SIDE || dfx.style == DFS_IFDEF;
    int same = !f0->missing && !f1->missing && f0->st_ok && f1->st_ok &&
               ((f0->isstdin && f1->isstdin) ||
                (!f0->isstdin && !f1->isstdin && f0->st.st_ino && f0->st.st_ino == f1->st.st_ino &&
                 f0->st.st_dev == f1->st.st_dev));
    const char *q0 = dfu_qf(dfu_shown(f0)), *q1 = dfu_qf(dfu_shown(f1));
    if (same && !lines_out) {
        if (dfx.report_same) printf("Files %s and %s are identical\n", q0, q1);
        return;
    }
    int bad = 0;
    if (dfu_load(f0) < 0) { dfu_err(0, errno, "%s", dfu_qf(f0->name)); bad = 1; }
    if (!same && dfu_load(f1) < 0) { dfu_err(0, errno, "%s", dfu_qf(f1->name)); bad = 1; }
    if (bad) { dfu_st(2); return; }
    if (same) {
        f1->buf = dfu_alloc(f0->len + 1);
        memcpy(f1->buf, f0->buf, f0->len + 1);
        f1->len = f0->len;
    }
    if (!dfu_o.text && (dfu_isbinary(f0) || dfu_isbinary(f1))) {
        if (dfu_eq_bytes(f0, f1)) {
            if (dfx.report_same) printf("Files %s and %s are identical\n", q0, q1);
            return;
        }
        if (dfx.brief) printf("Files %s and %s differ\n", q0, q1);
        else printf("Binary files %s and %s differ\n", q0, q1);
        dfu_st(1);
        return;
    }
    if (dfx.brief && !dfu_loose()) {
        if (dfu_eq_bytes(f0, f1)) {
            if (dfx.report_same) printf("Files %s and %s are identical\n", q0, q1);
            return;
        }
        printf("Files %s and %s differ\n", q0, q1);
        dfu_st(1);
        return;
    }
    dfu_lines(f0);
    dfu_lines(f1);
    dfu_cls_reset();
    dfu_classify(f0);
    dfu_classify(f1);
    long n;
    struct dfu_chg *ch = dfu_analyze(f0, f1, &n);
    dfu_mark_ignorable(f0, f1, ch, n);
    int changes = 0;
    for (long k = 0; k < n; k++) if (!ch[k].ign) changes = 1;
    if (dfx.brief) {
        if (changes) printf("Files %s and %s differ\n", q0, q1);
        else if (dfx.report_same) printf("Files %s and %s are identical\n", q0, q1);
        free(ch);
        dfu_st(changes);
        return;
    }
    if (changes || lines_out) {
        char *title = dfu_title(f0, f1);
        if (dfx.paginate) { dfu_pg.on = 1; dfu_pg.n = 0; }
        else if (depth > 0 && !dfu_nohdr) printf("%s\n", title);
        switch (dfx.style) {
        case DFS_CONTEXT: case DFS_UNIFIED:
            dfu_header_line(dfx.style == DFS_UNIFIED ? "---" : "***", f0, dfx.style == DFS_UNIFIED);
            dfu_header_line(dfx.style == DFS_UNIFIED ? "+++" : "---", f1, dfx.style == DFS_UNIFIED);
            dfu_out_context(f0, f1, ch, n, dfx.style == DFS_UNIFIED);
            break;
        case DFS_ED: dfu_out_ed(f1, ch, n); break;
        case DFS_FED: dfu_out_fed(f1, ch, n); break;
        case DFS_RCS: dfu_out_rcs(f1, ch, n); break;
        case DFS_IFDEF: dfu_out_ifdef(f0, f1, ch, n); break;
        case DFS_SIDE: dfu_out_side(f0, f1, ch, n); break;
        default: dfu_out_normal(f0, f1, ch, n); break;
        }
        if (dfx.paginate) {
            dfu_pg.on = 0;
            dfu_paginate(title, dfu_pg.b, dfu_pg.n);
        }
        free(title);
    }
    /* ed cannot say a file ends without a newline: each such file is
     * complained of, changes or not (with the newline of the "\ No
     * newline" line still on the message), and that is trouble */
    int complained = 0;
    if (dfx.style == DFS_ED || dfx.style == DFS_FED) {
        if (f0->incomplete) { dfu_err(0, 0, "%s: No newline at end of file\n", dfu_shown(f0)); complained = 1; }
        if (f1->incomplete) { dfu_err(0, 0, "%s: No newline at end of file\n", dfu_shown(f1)); complained = 1; }
        if (complained) dfu_st(2);
    }
    if (!changes && !complained && dfx.report_same) printf("Files %s and %s are identical\n", q0, q1);
    free(ch);
    dfu_st(changes);
}

/* ---- directories ---------------------------------------------------- */

/* fnmatch(3) without flags (and FNM_CASEFOLD for --ignore-file-name-case):
 * * ? [...] with ! or ^, and \ quoting; a leading dot is nothing special */
static int dfu_fnmatch(const char *p, const char *s) {
    for (; *p; p++, s++) {
        unsigned char pc = (unsigned char)*p;
        if (pc == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return 1;
            for (; *s; s++) if (dfu_fnmatch(p + 1, s)) return 1;
            return dfu_fnmatch(p + 1, s);
        }
        if (!*s) return 0;
        unsigned char sc = (unsigned char)*s;
        if (dfx.icasefn) sc = (unsigned char)tolower(sc);
        if (pc == '?') continue;
        if (pc == '[') {
            const char *q = p + 1;
            int neg = 0, hit = 0;
            if (*q == '!' || *q == '^') { neg = 1; q++; }
            const char *start = q;
            while (*q && (*q != ']' || q == start)) {
                unsigned char a = (unsigned char)*q, b;
                if (a == '\\' && q[1]) a = (unsigned char)*++q;
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    b = (unsigned char)q[2];
                    if (b == '\\' && q[3]) { b = (unsigned char)q[3]; q++; }
                    q += 3;
                } else { b = a; q++; }
                if (dfx.icasefn) { a = (unsigned char)tolower(a); b = (unsigned char)tolower(b); }
                if (sc >= a && sc <= b) hit = 1;
            }
            if (*q != ']') {             /* no closing bracket: a plain '[' */
                if (sc != '[') return 0;
                continue;
            }
            if (hit == neg) return 0;
            p = q;
            continue;
        }
        if (pc == '\\' && p[1]) pc = (unsigned char)*++p;
        if (dfx.icasefn) pc = (unsigned char)tolower(pc);
        if (pc != sc) return 0;
    }
    return !*s;
}

static int dfu_excluded(const char *name) {
    for (int i = 0; i < dfx.nexcl; i++) if (dfu_fnmatch(dfx.excl[i], name)) return 1;
    return 0;
}

static int dfu_namecmp(const char *a, const char *b) {
    if (dfx.icasefn) {
        for (;; a++, b++) {
            int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
            if (x != y || !x) return x - y;
        }
    }
    return strcmp(a, b);
}

static int dfu_qsort_name(const void *a, const void *b) {
    int r = dfu_namecmp(*(char *const *)a, *(char *const *)b);
    return r ? r : strcmp(*(char *const *)a, *(char *const *)b);
}

/* the names in DIR, sorted, without . .. and the excluded ones; -1 */
static long dfu_listdir(const char *dir, char ***out) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    long n = 0, cap = 32;
    char **v = dfu_alloc((size_t)cap * sizeof *v);
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (dfu_excluded(nm)) continue;
        if (n == cap) { cap *= 2; v = dfu_realloc(v, (size_t)cap * sizeof *v); }
        v[n++] = dfu_strdup(nm);
    }
    closedir(d);
    qsort(v, (size_t)n, sizeof *v, dfu_qsort_name);
    *out = v;
    return n;
}

static void dfu_compare(const char *n0, const char *n1, int depth, int miss);

static void dfu_dirs(const char *d0, const char *d1, int depth, int miss) {
    char **v0 = 0, **v1 = 0;
    long c0 = 0, c1 = 0;
    if (!(miss & 1) && (c0 = dfu_listdir(d0, &v0)) < 0) {
        dfu_err(0, errno, "%s", dfu_qf(d0));
        dfu_st(2);
        return;
    }
    if (!(miss & 2) && (c1 = dfu_listdir(d1, &v1)) < 0) {
        dfu_err(0, errno, "%s", dfu_qf(d1));
        dfu_st(2);
        for (long i = 0; i < c0; i++) free(v0[i]);
        free(v0);
        return;
    }
    long i = 0, j = 0;
    if (depth == 0 && dfx.start) {
        while (i < c0 && dfu_namecmp(v0[i], dfx.start) < 0) i++;
        while (j < c1 && dfu_namecmp(v1[j], dfx.start) < 0) j++;
    }
    while (i < c0 || j < c1) {
        int r = i >= c0 ? 1 : j >= c1 ? -1 : dfu_namecmp(v0[i], v1[j]);
        if (r == 0) {
            char *a = dfu_join(d0, v0[i]), *b = dfu_join(d1, v1[j]);
            dfu_compare(a, b, depth + 1, 0);
            free(a); free(b);
            i++; j++;
        } else if (r < 0) {
            if (dfx.newfile) {
                char *a = dfu_join(d0, v0[i]), *b = dfu_join(d1, v0[i]);
                dfu_compare(a, b, depth + 1, 2);
                free(a); free(b);
            } else {
                printf("Only in %s: %s\n", dfu_qf(d0), dfu_qf(v0[i]));
                dfu_st(1);
            }
            i++;
        } else {
            if (dfx.newfile || dfx.unidir) {
                char *a = dfu_join(d0, v1[j]), *b = dfu_join(d1, v1[j]);
                dfu_compare(a, b, depth + 1, 1);
                free(a); free(b);
            } else {
                printf("Only in %s: %s\n", dfu_qf(d1), dfu_qf(v1[j]));
                dfu_st(1);
            }
            j++;
        }
    }
    for (long k = 0; k < c0; k++) free(v0[k]);
    for (long k = 0; k < c1; k++) free(v1[k]);
    free(v0); free(v1);
}

static int dfu_stat(const char *name, struct stat *st) {
    if (dfx.noderef) return lstat(name, st);
    return stat(name, st);
}

/* N0 against N1; DEPTH 0 for the operands. MISS: 1 or 2 when that side
 * is known to be absent (a -N comparison inside a directory) */
static void dfu_compare(const char *n0, const char *n1, int depth, int miss) {
    struct dfu_file f[2];
    memset(f, 0, sizeof f);
    f[0].name = n0;
    f[1].name = n1;
    f[0].label = dfx.label[0];
    f[1].label = dfx.label[1];
    int err[2] = { 0, 0 };
    for (int k = 0; k < 2; k++) {
        if (miss & (1 << k)) { f[k].missing = 1; continue; }
        if (depth == 0 && !strcmp(f[k].name, "-")) {
            f[k].isstdin = 1;
            if (fstat(0, &f[k].st) == 0) f[k].st_ok = 1;
            continue;
        }
        if (dfu_stat(f[k].name, &f[k].st) == 0) f[k].st_ok = 1;
        else err[k] = errno;
    }
    /* -N: an operand that does not exist is empty, when the other does */
    if (depth == 0) {
        if (err[0] == ENOENT && !err[1] && (dfx.newfile || dfx.unidir)) { err[0] = 0; f[0].missing = 1; }
        if (err[1] == ENOENT && !err[0] && dfx.newfile) { err[1] = 0; f[1].missing = 1; }
    }
    if (err[0] || err[1]) {
        for (int k = 0; k < 2; k++)
            if (err[k]) dfu_err(0, err[k], "%s", dfu_qf(f[k].name));
        dfu_st(2);
        return;
    }
    /* at the top, an absent first file takes the second's date with it
     * (measured: both headers then show the epoch) */
    if (depth == 0 && f[0].missing && !f[1].isstdin) f[1].st_ok = 0;
    int dir0 = f[0].missing ? (f[1].st_ok && S_ISDIR(f[1].st.st_mode)) : S_ISDIR(f[0].st.st_mode);
    int dir1 = f[1].missing ? (f[0].st_ok && S_ISDIR(f[0].st.st_mode)) : S_ISDIR(f[1].st.st_mode);
    if (f[0].isstdin) dir0 = 0;
    if (f[1].isstdin) dir1 = 0;
    /* ... and with it, the second's being a directory: it is read as a
     * file (measured: "diff -N nosuch dir" says "dir: Is a directory") */
    if (depth == 0 && f[0].missing && !f[1].isstdin) dir0 = dir1 = 0;
    if (dir0 && dir1) {
        if (depth > 0 && !dfx.recursive) {
            printf("Common subdirectories: %s and %s\n", dfu_qf(n0), dfu_qf(n1));
            return;
        }
        if (dfx.style == DFS_IFDEF) {
            fflush(stdout);
            fprintf(stderr, "%s: -D option not supported with directories\n", su_name);
            exit(2);
        }
        /* a directory and itself: nothing to say -- except side by side,
         * where GNU's lists every entry as only in the first (measured) */
        if (f[0].st_ok && f[1].st_ok && f[0].st.st_ino && f[0].st.st_ino == f[1].st.st_ino &&
            f[0].st.st_dev == f[1].st.st_dev) {
            if (dfx.style == DFS_SIDE) {
                char **v;
                long c = dfu_listdir(n0, &v);
                for (long i = 0; i < c; i++) {
                    printf("Only in %s: %s\n", dfu_qf(n0), dfu_qf(v[i]));
                    dfu_st(1);
                    free(v[i]);
                }
                if (c >= 0) free(v);
            }
            return;
        }
        dfu_dirs(n0, n1, depth, (f[0].missing ? 1 : 0) | (f[1].missing ? 2 : 0));
        return;
    }
    if (dir0 != dir1) {
        if (depth == 0) {
            int d = dir0 ? 0 : 1;
            struct dfu_file *file = &f[1 - d];
            if (file->isstdin) {
                fflush(stdout);
                fprintf(stderr, "%s: cannot compare '-' to a directory\n", su_name);
                exit(2);
            }
            const char *base = strrchr(file->name, '/');
            base = base ? base + 1 : file->name;
            char *p = dfu_join(f[d].name, base);
            /* a pair named on the command line has no "diff ..." line */
            dfu_nohdr = 1;
            if (d == 0) dfu_compare(p, n1, 1, 0);
            else dfu_compare(n0, p, 1, 0);
            dfu_nohdr = 0;
            free(p);
            return;
        }
        printf("File %s is a %s while file %s is a %s\n", dfu_qf(n0), dfu_ftype(&f[0].st),
               dfu_qf(n1), dfu_ftype(&f[1].st));
        dfu_st(1);
        return;
    }
    if (dfx.noderef && !f[0].missing && !f[1].missing) {
        int l0 = S_ISLNK(f[0].st.st_mode), l1 = S_ISLNK(f[1].st.st_mode);
        if (l0 || l1) {
            if (l0 && l1) {
                char t0[4096], t1[4096];
                ssize_t a = readlink(n0, t0, sizeof t0 - 1), b = readlink(n1, t1, sizeof t1 - 1);
                if (a >= 0) t0[a] = 0;
                if (b >= 0) t1[b] = 0;
                if (a < 0 || b < 0 || strcmp(t0, t1)) {
                    printf("Symbolic links %s -> %s and %s -> %s differ\n", dfu_qa(n0), dfu_qa(t0),
                           dfu_qa(n1), dfu_qa(t1));
                    dfu_st(1);
                } else if (dfx.report_same)
                    printf("Files %s and %s are identical\n", dfu_qf(n0), dfu_qf(n1));
                return;
            }
            printf("File %s is a %s while file %s is a %s\n", dfu_qf(n0), dfu_ftype(&f[0].st),
                   dfu_qf(n1), dfu_ftype(&f[1].st));
            dfu_st(1);
            return;
        }
    }
    if (depth > 0 && !f[0].missing && !f[1].missing &&
        (!S_ISREG(f[0].st.st_mode) || !S_ISREG(f[1].st.st_mode)) &&
        (f[0].st.st_mode & S_IFMT) != (f[1].st.st_mode & S_IFMT)) {
        printf("File %s is a %s while file %s is a %s\n", dfu_qf(n0), dfu_ftype(&f[0].st),
               dfu_qf(n1), dfu_ftype(&f[1].st));
        dfu_st(1);
        return;
    }
    dfu_files(&f[0], &f[1], depth);
    dfu_free_file(&f[0]);
    dfu_free_file(&f[1]);
}

/* ---- diff's options ---------------------------------------------------- */

static void dfu_set_style(int s) {
    if (dfx.style >= 0 && dfx.style != s) dfu_usage_error("conflicting output style options");
    dfx.style = s;
}

/* a decimal (blanks and '+' allowed in front); 0 or -1 */
static int dfu_num(const char *s, uintmax_t *v) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    uintmax_t x = 0;
    int over = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (x > (UINTMAX_MAX - d) / 10) over = 1;
        else x = x * 10 + d;
    }
    if (*p) return -1;
    *v = over ? UINTMAX_MAX : x;
    return over ? 1 : 0;
}

static void dfu_context_arg(const char *a) {
    uintmax_t v;
    if (dfu_num(a, &v) < 0) dfu_usage_error("invalid context length %s", su_quote(a));
    long c = v > LONG_MAX / 4 ? LONG_MAX / 4 : (long)v;
    if (c > dfx.context) dfx.context = c;
}

static void dfu_add_re(regex_t **v, int *n, const char *pat) {
    *v = dfu_realloc(*v, (size_t)(*n + 1) * sizeof **v);
    int flags = dfu_o.icase ? REG_ICASE : 0;
    int rc = regcomp(&(*v)[*n], pat, flags | REG_NOSUB);
    if (rc) {
        fflush(stdout);
        fprintf(stderr, "%s: %s: %s\n", su_name, dfu_qa(pat), dfu_regerr(rc, pat));
        exit(2);
    }
    (*n)++;
}

/* a group or line format given twice must say the same */
static void dfu_set_fmt(const char **slot, const char *v, const char *opt, const char *shown) {
    if (*slot && strcmp(*slot, v))
        dfu_usage_error("conflicting %s option value %s", opt, su_quote(shown));
    *slot = v;
}

static void dfu_add_excl(const char *p) {
    dfx.excl = dfu_realloc(dfx.excl, (size_t)(dfx.nexcl + 1) * sizeof *dfx.excl);
    dfx.excl[dfx.nexcl++] = dfu_strdup(p);
}

/* -X FILE: a pattern a line, white space at the end dropped, empty lines
 * skipped */
static void dfu_excl_file(const char *name) {
    struct dfu_file f;
    memset(&f, 0, sizeof f);
    f.name = name;
    f.isstdin = !strcmp(name, "-");
    if (dfu_load(&f) < 0) {
        dfu_err(2, errno, "%s", dfu_qf(name));
    }
    char *p = f.buf, *e = f.buf + f.len;
    while (p < e) {
        char *nl = memchr(p, '\n', (size_t)(e - p));
        char *end = nl ? nl : e;
        char *t = end;
        while (t > p && isspace((unsigned char)t[-1])) t--;
        if (t > p) {
            char c = *t;
            *t = 0;
            dfu_add_excl(p);
            *t = c;
        }
        p = nl ? nl + 1 : e;
    }
    free(f.buf);
}

/* --palette: rs= hd= ad= de= ln=, colon separated */
static int dfu_palette(const char *s) {
    char *copy = dfu_strdup(s);
    for (char *p = copy; *p;) {
        char *end = strchr(p, ':');
        if (end) *end = 0;
        char *eq = strchr(p, '=');
        if (!eq) { free(copy); return -1; }
        *eq = 0;
        const char **slot = !strcmp(p, "ad") ? &dfx.pal_ad : !strcmp(p, "de") ? &dfx.pal_de
                          : !strcmp(p, "hd") ? &dfx.pal_hd : !strcmp(p, "ln") ? &dfx.pal_ln
                          : !strcmp(p, "rs") ? &dfx.pal_rs : 0;
        if (!slot) {
            fflush(stdout);
            fprintf(stderr, "%s: unrecognized prefix: %s\n", su_name, p);
            free(copy);
            return -1;
        }
        *slot = dfu_strdup(eq + 1);
        if (!end) break;
        p = end + 1;
    }
    free(copy);
    return 0;
}

static void dfu_diff_help(void) {
    printf("Usage: %s [OPTION]... FILES\n"
           "Compare FILES line by line.\n"
           "\n"
           "Mandatory arguments to long options are mandatory for short options too.\n"
           "      --normal                  the classic output (the default)\n"
           "  -q, --brief                   say only whether the files differ\n"
           "  -s, --report-identical-files  say so when two files are the same\n"
           "  -c, -C NUM, --context[=NUM]   context output with NUM (3) lines around\n"
           "  -u, -U NUM, --unified[=NUM]   unified output with NUM (3) lines around\n"
           "  -e, --ed                      an ed script\n"
           "  -n, --rcs                     an RCS diff\n"
           "  -y, --side-by-side            the two files in two columns\n"
           "  -W, --width=NUM               at most NUM (130) columns in all\n"
           "      --left-column             common lines only in the left column\n"
           "      --suppress-common-lines   leave the common lines out\n"
           "\n"
           "  -p, --show-c-function         name the C function a change is in\n"
           "  -F, --show-function-line=RE   name the last line before it matching RE\n"
           "      --label LABEL             LABEL in place of a file's name and date\n"
           "                                  (once for each file)\n"
           "\n"
           "  -t, --expand-tabs             tabs as spaces in the output\n"
           "  -T, --initial-tab             a tab before each line's text\n"
           "      --tabsize=NUM             tab stops NUM (8) columns apart\n"
           "      --suppress-blank-empty    no space or tab before an empty line\n"
           "  -l, --paginate                pages, as pr makes them\n"
           "\n"
           "  -r, --recursive                 go into subdirectories too\n"
           "      --no-dereference            symbolic links as links\n"
           "  -N, --new-file                  a file that is missing is empty\n"
           "      --unidirectional-new-file   a missing first file is empty\n"
           "      --ignore-file-name-case     names match in any case\n"
           "      --no-ignore-file-name-case  names match in their case\n"
           "  -x, --exclude=PAT               skip files whose name matches PAT\n"
           "  -X, --exclude-from=FILE         skip files matching a pattern in FILE\n"
           "  -S, --starting-file=FILE        start at FILE in the directories\n"
           "      --from-file=FILE1           compare FILE1 (a directory too) to each\n"
           "                                    operand\n"
           "      --to-file=FILE2             compare each operand to FILE2\n"
           "\n"
           "  -i, --ignore-case               letters match in any case\n"
           "  -E, --ignore-tab-expansion      a tab matches the spaces it expands to\n"
           "  -Z, --ignore-trailing-space     white space at the end does not count\n"
           "  -b, --ignore-space-change       neither do changes in its amount\n"
           "  -w, --ignore-all-space          white space does not count at all\n"
           "  -B, --ignore-blank-lines        changes of blank lines only do not count\n"
           "  -I, --ignore-matching-lines=RE  nor changes whose lines all match RE\n"
           "\n"
           "  -a, --text                      every file is text\n"
           "      --strip-trailing-cr         a CR before a newline is dropped\n"
           "\n"
           "  -D, --ifdef=NAME                the files merged, with '#ifdef NAME'\n"
           "      --GTYPE-group-format=GFMT   how GTYPE groups of lines are written\n"
           "      --line-format=LFMT          how every line is written\n"
           "      --LTYPE-line-format=LFMT    how LTYPE lines are written\n"
           "    These shape the merged (-D) output:\n"
           "    LTYPE is 'old', 'new', or 'unchanged'; GTYPE is LTYPE or 'changed'.\n"
           "    In GFMT:\n"
           "      %%<  the lines of FILE1\n"
           "      %%>  the lines of FILE2\n"
           "      %%=  the lines both have\n"
           "      %%[-][WIDTH][.[PREC]]{doxX}LETTER  a number, as printf writes it:\n"
           "        F  the first line, L  the last, N  how many (L-F+1),\n"
           "        E  F-1, M  L+1 -- of the new group; lower case: the old group\n"
           "      %%(A=B?T:E)  T when A equals B, else E\n"
           "    In LFMT:\n"
           "      %%L  the line\n"
           "      %%l  the line without its newline\n"
           "      %%[-][WIDTH][.[PREC]]{doxX}n  the line's number\n"
           "    In both:\n"
           "      %%%%  a %%\n"
           "      %%c'C'  the character C\n"
           "      %%c'\\OOO'  the character with octal code OOO\n"
           "      C    any other character, as it is\n"
           "\n"
           "  -d, --minimal            look harder for fewer changes\n"
           "      --horizon-lines=NUM  keep NUM lines of what both begin and end with\n"
           "      --speed-large-files  big files with small changes all over\n"
           "      --color[=WHEN]       colour: WHEN 'never', 'always' or 'auto' (the\n"
           "                             default, for --color alone)\n"
           "      --palette=PALETTE    the colours, terminfo capabilities joined by ':'\n"
           "\n"
           "      --help               show this help and exit\n"
           "  -v, --version            show the version and exit\n"
           "\n"
           "FILES are 'FILE1 FILE2', 'DIR1 DIR2', 'DIR FILE' or 'FILE DIR'; with\n"
           "--from-file or --to-file, any number of them. '-' is standard input.\n"
           "Exit status: 0 the same, 1 different, 2 trouble.\n", su_name);
}

/* diff's options into dfx and dfu_o: -1, or the status to exit with (--help,
 * --version) */
static int dfu_diff_parse(int argc, char **argv) {
    enum {
        O_BINARY = 0x100, O_CGF, O_COLOR, O_CONTEXT, O_FROM, O_HORIZON, O_IFNC, O_INHIBIT,
        O_LABEL, O_LEFT, O_LF, O_NGF, O_NLF, O_NODEREF, O_NOIFNC, O_NORMAL, O_OGF, O_OLF,
        O_PALETTE, O_SDIFF, O_STRIPCR, O_SBE, O_SCL, O_TABSIZE, O_TO, O_UGF, O_ULF, O_UNIDIR,
        O_UNIFIED
    };
    static const struct su_option longs[] = {
        { "binary", SU_NO_ARG, 0, O_BINARY },
        { "brief", SU_NO_ARG, 0, 'q' },
        { "changed-group-format", SU_REQ_ARG, 0, O_CGF },
        { "color", SU_OPT_ARG, 0, O_COLOR },
        { "context", SU_OPT_ARG, 0, O_CONTEXT },
        { "ed", SU_NO_ARG, 0, 'e' },
        { "exclude", SU_REQ_ARG, 0, 'x' },
        { "exclude-from", SU_REQ_ARG, 0, 'X' },
        { "expand-tabs", SU_NO_ARG, 0, 't' },
        { "forward-ed", SU_NO_ARG, 0, 'f' },
        { "from-file", SU_REQ_ARG, 0, O_FROM },
        { "help", SU_NO_ARG, 0, SU_HELP_OPT },
        { "horizon-lines", SU_REQ_ARG, 0, O_HORIZON },
        { "ifdef", SU_REQ_ARG, 0, 'D' },
        { "ignore-all-space", SU_NO_ARG, 0, 'w' },
        { "ignore-blank-lines", SU_NO_ARG, 0, 'B' },
        { "ignore-case", SU_NO_ARG, 0, 'i' },
        { "ignore-file-name-case", SU_NO_ARG, 0, O_IFNC },
        { "ignore-matching-lines", SU_REQ_ARG, 0, 'I' },
        { "ignore-space-change", SU_NO_ARG, 0, 'b' },
        { "ignore-tab-expansion", SU_NO_ARG, 0, 'E' },
        { "ignore-trailing-space", SU_NO_ARG, 0, 'Z' },
        { "inhibit-hunk-merge", SU_NO_ARG, 0, O_INHIBIT },
        { "initial-tab", SU_NO_ARG, 0, 'T' },
        { "label", SU_REQ_ARG, 0, 'L' },
        { "left-column", SU_NO_ARG, 0, O_LEFT },
        { "line-format", SU_REQ_ARG, 0, O_LF },
        { "minimal", SU_NO_ARG, 0, 'd' },
        { "new-file", SU_NO_ARG, 0, 'N' },
        { "new-group-format", SU_REQ_ARG, 0, O_NGF },
        { "new-line-format", SU_REQ_ARG, 0, O_NLF },
        { "no-dereference", SU_NO_ARG, 0, O_NODEREF },
        { "no-ignore-file-name-case", SU_NO_ARG, 0, O_NOIFNC },
        { "normal", SU_NO_ARG, 0, O_NORMAL },
        { "old-group-format", SU_REQ_ARG, 0, O_OGF },
        { "old-line-format", SU_REQ_ARG, 0, O_OLF },
        { "paginate", SU_NO_ARG, 0, 'l' },
        { "palette", SU_REQ_ARG, 0, O_PALETTE },
        { "rcs", SU_NO_ARG, 0, 'n' },
        { "recursive", SU_NO_ARG, 0, 'r' },
        { "report-identical-files", SU_NO_ARG, 0, 's' },
        { "sdiff-merge-assist", SU_NO_ARG, 0, O_SDIFF },
        { "show-c-function", SU_NO_ARG, 0, 'p' },
        { "show-function-line", SU_REQ_ARG, 0, 'F' },
        { "side-by-side", SU_NO_ARG, 0, 'y' },
        { "speed-large-files", SU_NO_ARG, 0, 'H' },
        { "starting-file", SU_REQ_ARG, 0, 'S' },
        { "strip-trailing-cr", SU_NO_ARG, 0, O_STRIPCR },
        { "suppress-blank-empty", SU_NO_ARG, 0, O_SBE },
        { "suppress-common-lines", SU_NO_ARG, 0, O_SCL },
        { "tabsize", SU_REQ_ARG, 0, O_TABSIZE },
        { "text", SU_NO_ARG, 0, 'a' },
        { "to-file", SU_REQ_ARG, 0, O_TO },
        { "unchanged-group-format", SU_REQ_ARG, 0, O_UGF },
        { "unchanged-line-format", SU_REQ_ARG, 0, O_ULF },
        { "unidirectional-new-file", SU_NO_ARG, 0, 'P' },
        { "unified", SU_OPT_ARG, 0, O_UNIFIED },
        { "version", SU_NO_ARG, 0, 'v' },
        { "width", SU_REQ_ARG, 0, 'W' },
        { 0, 0, 0, 0 }
    };
    memset(&dfx, 0, sizeof dfx);
    memset(&dfu_o, 0, sizeof dfu_o);
    dfx.style = -1;
    dfx.context = -1;
    dfu_o.tabsize = 0;
    int show_c = 0, prev_digit = 0;
    long digits = -1, odig = -1;
    int imp3 = 0;               /* -c -u --context --unified: 3 at least */
    const char *color_arg = 0, *palette = 0;
    char *fmtbuf[4] = { 0, 0, 0, 0 };
    int c;
    while ((c = su_getopt_long(argc, argv, "0123456789abBcC:dD:eEfF:hHiI:lL:nNpPqrsS:tTuU:vwW:x:X:yZ",
                               longs, 0)) != -1) {
        if (c >= '0' && c <= '9') {
            /* the old -NUM: digits in a row make one number */
            digits = prev_digit ? digits * 10 + (c - '0') : c - '0';
            if (digits > LONG_MAX / 100) digits = LONG_MAX / 100;
            if (digits > odig) odig = digits;
            prev_digit = 1;
            continue;
        }
        prev_digit = 0;
        switch (c) {
        case 'a': dfu_o.text = 1; break;
        case 'b': dfu_o.ispace = 1; break;
        case 'B': dfx.iblank = 1; break;
        case 'c': imp3 = 1; dfu_set_style(DFS_CONTEXT); break;
        case 'C': dfu_context_arg(su_optarg); dfu_set_style(DFS_CONTEXT); break;
        case O_CONTEXT:
            if (su_optarg) dfu_context_arg(su_optarg);
            else imp3 = 1;
            dfu_set_style(DFS_CONTEXT);
            break;
        case 'u': imp3 = 1; dfu_set_style(DFS_UNIFIED); break;
        case 'U': dfu_context_arg(su_optarg); dfu_set_style(DFS_UNIFIED); break;
        case O_UNIFIED:
            if (su_optarg) dfu_context_arg(su_optarg);
            else imp3 = 1;
            dfu_set_style(DFS_UNIFIED);
            break;
        case 'd': dfu_o.minimal = 1; break;
        case 'D': {
            const char *nm = su_optarg;
            size_t L = strlen(nm) * 2 + 64;
            for (int k = 0; k < 4; k++) fmtbuf[k] = dfu_alloc(L);
            snprintf(fmtbuf[0], L, "#ifndef %s\n%%<#endif /* ! %s */\n", nm, nm);
            snprintf(fmtbuf[1], L, "#ifdef %s\n%%>#endif /* %s */\n", nm, nm);
            snprintf(fmtbuf[2], L, "%%=");
            snprintf(fmtbuf[3], L, "#ifndef %s\n%%<#else /* %s */\n%%>#endif /* %s */\n", nm, nm, nm);
            for (int k = 0; k < 4; k++) dfu_set_fmt(&dfx.gfmt[k], fmtbuf[k], "-D", fmtbuf[k]);
            dfu_set_style(DFS_IFDEF);
            break;
        }
        case 'e': dfu_set_style(DFS_ED); break;
        case 'E': dfu_o.itab = 1; break;
        case 'f': dfu_set_style(DFS_FED); break;
        case 'F': dfu_add_re(&dfx.fre, &dfx.nfre, su_optarg); break;
        case 'h': break;
        case 'H': break;
        case 'i': dfu_o.icase = 1; break;
        case 'I': dfu_add_re(&dfx.ire, &dfx.nire, su_optarg); break;
        case 'l': dfx.paginate = 1; break;
        case 'L':
            if (dfx.nlabel >= 2) { fflush(stdout); fprintf(stderr, "%s: too many file label options\n", su_name); exit(2); }
            dfx.label[dfx.nlabel++] = su_optarg;
            break;
        case 'n': dfu_set_style(DFS_RCS); break;
        case 'N': dfx.newfile = 1; break;
        case 'p': show_c = 1; dfu_add_re(&dfx.fre, &dfx.nfre, "^[[:alpha:]$_]"); break;
        case 'P': dfx.unidir = 1; break;
        case 'q': dfx.brief = 1; break;
        case 'r': dfx.recursive = 1; break;
        case 's': dfx.report_same = 1; break;
        case 'S': dfx.start = su_optarg; break;
        case 't': dfx.expand_tabs = 1; break;
        case 'T': dfx.initial_tab = 1; break;
        case 'v': return su_version();
        case 'w': dfu_o.iall = 1; break;
        case 'W': {
            uintmax_t v;
            if (dfu_num(su_optarg, &v) || v == 0) dfu_usage_error("invalid width %s", su_quote(su_optarg));
            long w = v > (uintmax_t)LONG_MAX / 4 ? LONG_MAX / 4 : (long)v;
            if (dfx.width && dfx.width != w) { fflush(stdout); fprintf(stderr, "%s: conflicting width options\n", su_name); exit(2); }
            dfx.width = w;
            break;
        }
        case 'x': dfu_add_excl(su_optarg); break;
        case 'X': dfu_excl_file(su_optarg); break;
        case 'y': dfu_set_style(DFS_SIDE); break;
        case 'Z': dfu_o.itrail = 1; break;
        case O_BINARY: break;
        case O_CGF: dfu_set_fmt(&dfx.gfmt[3], su_optarg, "--changed-group-format", su_optarg); dfu_set_style(DFS_IFDEF); break;
        case O_OGF: dfu_set_fmt(&dfx.gfmt[0], su_optarg, "--old-group-format", su_optarg); dfu_set_style(DFS_IFDEF); break;
        case O_NGF: dfu_set_fmt(&dfx.gfmt[1], su_optarg, "--new-group-format", su_optarg); dfu_set_style(DFS_IFDEF); break;
        case O_UGF: dfu_set_fmt(&dfx.gfmt[2], su_optarg, "--unchanged-group-format", su_optarg); dfu_set_style(DFS_IFDEF); break;
        case O_OLF: dfu_set_fmt(&dfx.lfmt[0], su_optarg, "--old-line-format", su_optarg); dfu_set_style(DFS_IFDEF); break;
        case O_NLF: dfu_set_fmt(&dfx.lfmt[1], su_optarg, "--new-line-format", su_optarg); dfu_set_style(DFS_IFDEF); break;
        case O_ULF: dfu_set_fmt(&dfx.lfmt[2], su_optarg, "--unchanged-line-format", su_optarg); dfu_set_style(DFS_IFDEF); break;
        case O_LF:
            for (int k = 0; k < 3; k++) dfu_set_fmt(&dfx.lfmt[k], su_optarg, "--line-format", su_optarg);
            dfu_set_style(DFS_IFDEF);
            break;
        case O_COLOR:
            color_arg = su_optarg ? su_optarg : "auto";
            if (strcmp(color_arg, "never") && strcmp(color_arg, "always") && strcmp(color_arg, "auto"))
                dfu_usage_error("invalid color %s", su_quote(color_arg));
            break;
        case O_FROM:
            if (dfx.from && strcmp(dfx.from, su_optarg)) dfu_usage_error("conflicting %s option value %s", "--from-file", su_quote(su_optarg));
            dfx.from = su_optarg;
            break;
        case O_TO:
            if (dfx.to && strcmp(dfx.to, su_optarg)) dfu_usage_error("conflicting %s option value %s", "--to-file", su_quote(su_optarg));
            dfx.to = su_optarg;
            break;
        case O_HORIZON: {
            uintmax_t v;
            if (dfu_num(su_optarg, &v) < 0) dfu_usage_error("invalid horizon length %s", su_quote(su_optarg));
            long h = v > (uintmax_t)LONG_MAX / 4 ? LONG_MAX / 4 : (long)v;
            if (h > dfu_horizon) dfu_horizon = h;
            break;
        }
        case O_IFNC: dfx.icasefn = 1; break;
        case O_NOIFNC: dfx.icasefn = 0; break;
        case O_INHIBIT: dfx.inhibit_merge = 1; break;
        case O_LEFT: dfx.left_col = 1; break;
        case O_NODEREF: dfx.noderef = 1; break;
        case O_NORMAL: dfu_set_style(DFS_NORMAL); break;
        case O_PALETTE: palette = su_optarg; break;
        case O_SDIFF: dfx.sdiff_assist = 1; dfu_set_style(DFS_SIDE); break;
        case O_STRIPCR: dfu_o.strip_cr = 1; break;
        case O_SBE: dfx.sbe = 1; break;
        case O_SCL: dfx.suppress_common = 1; break;
        case O_TABSIZE: {
            uintmax_t v;
            if (dfu_num(su_optarg, &v) || v == 0 || v > ((uintmax_t)1 << 62))
                dfu_usage_error("invalid tabsize %s", su_quote(su_optarg));
            if (dfu_o.tabsize && (uintmax_t)dfu_o.tabsize != v) { fflush(stdout); fprintf(stderr, "%s: conflicting tabsize options\n", su_name); exit(2); }
            dfu_o.tabsize = v > INT_MAX ? INT_MAX : (int)v;
            break;
        }
        case SU_HELP_OPT: dfu_diff_help(); return 0;
        default: dfu_try_help();
        }
    }
    if (dfx.style < 0) dfx.style = show_c ? DFS_CONTEXT : DFS_NORMAL;
    /* how much context, measured: -C/-U values, the largest; the old
     * -NUM digits take part, and then -c/-u's 3 does not (-u -1 is 1,
     * -u -U1 is 3, -2 -U5 is 5) */
    if (odig >= 0) { if (odig > dfx.context) dfx.context = odig; }
    else if (imp3 && dfx.context < 3) dfx.context = 3;
    if (dfx.context < 0) dfx.context = 3;
    if (dfx.paginate) setvbuf(stdout, 0, _IOFBF, 4096);
    /* with context shown, that much of what the files begin and end with
     * stays in play (measured: a deletion in a run slides into it) */
    if ((dfx.style == DFS_CONTEXT || dfx.style == DFS_UNIFIED) && dfx.context > dfu_horizon)
        dfu_horizon = dfx.context;
    if (!dfx.width) dfx.width = 130;
    if (!dfu_o.tabsize) dfu_o.tabsize = 8;
    dfx.any_ign = dfx.iblank || dfx.nire;
    /* ed cannot write an incomplete line: one is compared as if whole */
    dfu_o.eol_any = dfx.style == DFS_ED || dfx.style == DFS_FED;
    if (dfx.style == DFS_IFDEF) {
        for (int k = 0; k < 3; k++) if (!dfx.lfmt[k]) dfx.lfmt[k] = "%l\n";
        if (!dfx.gfmt[0]) dfx.gfmt[0] = dfx.gfmt[3] ? dfx.gfmt[3] : "%<";
        if (!dfx.gfmt[1]) dfx.gfmt[1] = dfx.gfmt[3] ? dfx.gfmt[3] : "%>";
        if (!dfx.gfmt[2]) dfx.gfmt[2] = "%=";
        if (!dfx.gfmt[3]) {
            size_t L = strlen(dfx.gfmt[0]) + strlen(dfx.gfmt[1]) + 1;
            char *cg = dfu_alloc(L);
            snprintf(cg, L, "%s%s", dfx.gfmt[0], dfx.gfmt[1]);
            dfx.gfmt[3] = cg;
        }
    }
    dfx.pal_ad = "32"; dfx.pal_de = "31"; dfx.pal_hd = "1"; dfx.pal_ln = "36"; dfx.pal_rs = "0";
    if (color_arg && !strcmp(color_arg, "always") &&
        (dfx.style == DFS_NORMAL || dfx.style == DFS_CONTEXT || dfx.style == DFS_UNIFIED ||
         dfx.style == DFS_SIDE)) {
        dfx.color = 1;
        if (palette && dfu_palette(palette) < 0) {
            fflush(stdout);
            fprintf(stderr, "%s: unparsable value for --palette\n", su_name);
            dfx.color = 0;
        }
    }
    /* the switches, as given, for "diff -r ..." lines */
    size_t sl = 1;
    for (int i = 1; i < su_optind; i++) sl += strlen(dfu_qsw(argv[i])) + 1;
    dfx.switches = dfu_alloc(sl);
    dfx.switches[0] = 0;
    for (int i = 1; i < su_optind; i++) { strcat(dfx.switches, " "); strcat(dfx.switches, dfu_qsw(argv[i])); }
    return -1;
}

static int do_diff(int argc, char **argv) {
    int r = dfu_diff_parse(argc, argv);
    if (r >= 0) return r;
    int nops = argc - su_optind;
    char **ops = argv + su_optind;
    if (dfx.from && dfx.to) dfu_err(2, 0, "--from-file and --to-file both specified");
    if (dfx.from || dfx.to) {
        for (int i = 0; i < nops; i++) {
            if (dfx.from) dfu_compare(dfx.from, ops[i], 0, 0);
            else dfu_compare(ops[i], dfx.to, 0, 0);
        }
    } else {
        if (nops < 2) dfu_usage_error("missing operand after %s", su_quote(argv[argc - 1]));
        if (nops > 2) dfu_usage_error("extra operand %s", su_quote(ops[2]));
        dfu_compare(ops[0], ops[1], 0, 0);
    }
    if (fflush(stdout) != 0 || ferror(stdout)) dfu_err(2, errno, "write failed");
    return dfu_status;
}

/* ======================================================================
 * cmp
 *
 * Byte by byte. The first difference: "A B differ: char N, line M" (with
 * -b "byte N, line M is OOO C OOO C"); -l every difference, its number
 * in a column as wide as the largest it could be (the smaller file's
 * size after skipping, or -n, as measured; 19 digits when neither size
 * is known); a file that ends first: "EOF on 'F' after byte N, in line
 * M" (or "which is empty") on standard error. SKIP values: decimal, 0
 * octal, 0x hex, with k K M G T P E Z Y and B / iB after them; several
 * -i and operands: the largest skip wins, several -n the smallest.
 * ====================================================================== */

/* SKIP or LIMIT: 0 ok (*V, clamped), -1 invalid */
static int dfu_cmpnum(const char *s, uintmax_t *v) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') return -1;
    if (*p == '+') p++;
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) { base = 16; p += 2; }
    else if (p[0] == '0') base = 8;
    uintmax_t x = 0;
    int over = 0, any = 0;
    for (;; p++) {
        int d;
        if (isdigit((unsigned char)*p)) d = *p - '0';
        else if (base == 16 && isxdigit((unsigned char)*p)) d = tolower((unsigned char)*p) - 'a' + 10;
        else break;
        if (d >= base) break;
        any = 1;
        if (x > (UINTMAX_MAX - (unsigned)d) / (unsigned)base) over = 1;
        else x = x * (unsigned)base + (unsigned)d;
    }
    if (!any) return -1;
    if (*p) {
        const char *sx = "kKMGTPEZY";
        const char *q = strchr(sx, *p);
        if (!q) return -1;
        int power = *p == 'k' || *p == 'K' ? 1 : (int)(q - sx) - 0;
        if (*p == 'K') power = 1;
        unsigned long long mul = 1024;
        p++;
        if (p[0] == 'B' && !p[1]) { mul = 1000; p++; }
        else if (p[0] == 'i' && p[1] == 'B' && !p[2]) p += 2;
        if (*p) return -1;
        for (int i = 0; i < power; i++) {
            if (x > UINTMAX_MAX / mul) { over = 1; break; }
            x *= mul;
        }
    }
    *v = over ? UINTMAX_MAX : x;
    return 0;
}

/* -b's picture of a byte: itself, ^X for a control, M- before 8-bit */
static void dfu_cmp_pic(unsigned char c, char *b) {
    size_t n = 0;
    if (c >= 0x80) { b[n++] = 'M'; b[n++] = '-'; c = (unsigned char)(c - 0x80); }
    if (c < 0x20) { b[n++] = '^'; b[n++] = (char)(c + 0x40); }
    else if (c == 0x7f) { b[n++] = '^'; b[n++] = '?'; }
    else b[n++] = (char)c;
    b[n] = 0;
}

struct dfu_cin { const char *name; int fd; int reg; uintmax_t rem; unsigned char buf[65536]; size_t n, pos; int eof; };

static int dfu_cmp_fill(struct dfu_cin *c) {
    if (c->pos < c->n) return 1;
    if (c->eof) return 0;
    for (;;) {
        ssize_t r = read(c->fd, c->buf, sizeof c->buf);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) { c->eof = 1; return 0; }
        c->n = (size_t)r;
        c->pos = 0;
        return 1;
    }
}

/* skip N bytes of C: 0, or -1 on a read error */
static int dfu_cmp_skip(struct dfu_cin *c, uintmax_t n) {
    if (c->reg && n) {
        off_t r = lseek(c->fd, 0, SEEK_CUR);
        if (r >= 0) {
            uintmax_t to = (uintmax_t)r + n;
            if (to > (uintmax_t)0x7fffffffffffffffLL || n > (uintmax_t)0x7fffffffffffffffLL) {
                /* past any end: the file is used up */
                c->eof = 1;
                return 0;
            }
            if (lseek(c->fd, (off_t)to, SEEK_SET) >= 0) return 0;
        }
    }
    while (n) {
        int f = dfu_cmp_fill(c);
        if (f < 0) return -1;
        if (f == 0) return 0;
        size_t k = c->n - c->pos;
        if (k > n) k = (size_t)n;
        c->pos += k;
        n -= k;
    }
    return 0;
}

static void dfu_cmp_help(void) {
    printf("Usage: %s [OPTION]... FILE1 [FILE2 [SKIP1 [SKIP2]]]\n"
           "Compare two files a byte at a time.\n"
           "\n"
           "SKIP1 and SKIP2 are how many bytes at the start of each file to pass\n"
           "over (none by default).\n"
           "\n"
           "Mandatory arguments to long options are mandatory for short options too.\n"
           "  -b, --print-bytes          show the bytes that differ\n"
           "  -i, --ignore-initial=SKIP         pass over SKIP bytes of both files\n"
           "  -i, --ignore-initial=SKIP1:SKIP2  pass over SKIP1 bytes of FILE1 and\n"
           "                                      SKIP2 bytes of FILE2\n"
           "  -l, --verbose              list every difference: its number, both bytes\n"
           "  -n, --bytes=LIMIT          compare at most LIMIT bytes\n"
           "  -s, --quiet, --silent      write nothing; the exit status says it\n"
           "      --help                 show this help and exit\n"
           "  -v, --version              show the version and exit\n"
           "\n"
           "SKIP and LIMIT may end in kB 1000, K 1024, MB 1,000,000, M 1,048,576,\n"
           "GB 1,000,000,000, G 1,073,741,824, and the same for T, P, E, Z and Y.\n"
           "\n"
           "A FILE that is '-' or not given is standard input.\n"
           "Exit status: 0 the same, 1 different, 2 trouble.\n", su_name);
}

static int do_cmp(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "print-bytes", SU_NO_ARG, 0, 'b' },
        { "print-chars", SU_NO_ARG, 0, 0x100 },
        { "ignore-initial", SU_REQ_ARG, 0, 'i' },
        { "verbose", SU_NO_ARG, 0, 'l' },
        { "bytes", SU_REQ_ARG, 0, 'n' },
        { "silent", SU_NO_ARG, 0, 's' },
        { "quiet", SU_NO_ARG, 0, 's' },
        { "version", SU_NO_ARG, 0, 'v' },
        { "help", SU_NO_ARG, 0, SU_HELP_OPT },
        { 0, 0, 0, 0 }
    };
    int bytes = 0, verbose = 0, silent = 0, haslimit = 0;
    uintmax_t skip[2] = { 0, 0 }, limit = UINTMAX_MAX;
    int c;
    while ((c = su_getopt_long(argc, argv, "bci:ln:sv", longs, 0)) != -1) {
        switch (c) {
        case 'b': case 'c': case 0x100: bytes = 1; break;
        case 'i': {
            uintmax_t a, b;
            char *colon = strchr(su_optarg, ':');
            if (colon) {
                *colon = 0;
                if (dfu_cmpnum(su_optarg, &a) < 0) { *colon = ':'; dfu_usage_error("invalid --ignore-initial value %s", su_quote(su_optarg)); }
                *colon = ':';
                if (dfu_cmpnum(colon + 1, &b) < 0) dfu_usage_error("invalid --ignore-initial value %s", su_quote(colon + 1));
            } else {
                if (dfu_cmpnum(su_optarg, &a) < 0) dfu_usage_error("invalid --ignore-initial value %s", su_quote(su_optarg));
                b = a;
            }
            if (a > skip[0]) skip[0] = a;
            if (b > skip[1]) skip[1] = b;
            break;
        }
        case 'l': verbose = 1; break;
        case 'n': {
            uintmax_t v;
            if (dfu_cmpnum(su_optarg, &v) < 0) dfu_usage_error("invalid --bytes value %s", su_quote(su_optarg));
            if (!haslimit || v < limit) limit = v;
            haslimit = 1;
            break;
        }
        case 's': silent = 1; break;
        case 'v': return su_version();
        case SU_HELP_OPT: dfu_cmp_help(); return 0;
        default: dfu_try_help();
        }
    }
    if (verbose && silent) dfu_usage_error("options -l and -s are incompatible");
    int nops = argc - su_optind;
    char **ops = argv + su_optind;
    if (nops < 1) dfu_usage_error("missing operand after %s", su_quote(argv[argc - 1]));
    for (int k = 2; k < 4 && k < nops; k++) {
        uintmax_t v;
        if (dfu_cmpnum(ops[k], &v) < 0) dfu_usage_error("invalid --ignore-initial value %s", su_quote(ops[k]));
        if (v > skip[k - 2]) skip[k - 2] = v;
    }
    if (nops > 4) dfu_usage_error("extra operand %s", su_quote(ops[4]));
    static struct dfu_cin in[2];
    const char *names[2] = { ops[0], nops > 1 ? ops[1] : "-" };
    struct stat st[2];
    int stok[2] = { 0, 0 };
    for (int k = 0; k < 2; k++) {
        memset(&in[k], 0, sizeof in[k]);
        in[k].name = names[k];
        if (!strcmp(names[k], "-")) in[k].fd = 0;
        else {
            in[k].fd = su_open_read(names[k], 0);
            if (in[k].fd < 0) {
                if (!silent) dfu_err(0, errno, "%s", dfu_qf(names[k]));
                return 2;
            }
        }
        if (fstat(in[k].fd, &st[k]) == 0) stok[k] = 1;
        in[k].reg = stok[k] && S_ISREG(st[k].st_mode);
    }
    /* one file, skipped alike: the same */
    if (stok[0] && stok[1] && skip[0] == skip[1] &&
        ((!strcmp(names[0], "-") && !strcmp(names[1], "-")) ||
         (st[0].st_ino && st[0].st_ino == st[1].st_ino && st[0].st_dev == st[1].st_dev &&
          !S_ISDIR(st[0].st_mode))))
        return 0;
    /* the column for -l: as wide as the largest byte number there can be */
    uintmax_t most = limit;
    for (int k = 0; k < 2; k++) {
        uintmax_t r;
        if (in[k].reg) r = (uintmax_t)st[k].st_size > skip[k] ? (uintmax_t)st[k].st_size - skip[k] : 0;
        else r = (uintmax_t)0x7fffffffffffffffLL - (skip[k] < (uintmax_t)0x7fffffffffffffffLL ? skip[k] : 0);
        if (r < most) most = r;
    }
    int width = 1;
    for (uintmax_t t = most; t >= 10; t /= 10) width++;
    for (int k = 0; k < 2; k++) {
        if (dfu_cmp_skip(&in[k], skip[k]) < 0) {
            dfu_err(0, errno, "%s", dfu_qf(names[k]));
            return 2;
        }
    }
    uintmax_t at = 0, line = 1;
    int differ = 0;
    for (;;) {
        if (at >= limit) break;
        int f0 = dfu_cmp_fill(&in[0]), f1 = dfu_cmp_fill(&in[1]);
        if (f0 < 0 || f1 < 0) {
            int k = f0 < 0 ? 0 : 1;
            dfu_err(0, errno, "%s", dfu_qf(names[k]));
            return 2;
        }
        if (!f0 || !f1) {
            if (f0 || f1) {
                int k = f0 ? 1 : 0;      /* the one that ended */
                if (!silent) {
                    fflush(stdout);
                    if (at == 0) fprintf(stderr, "%s: EOF on %s which is empty\n", su_name, dfu_qa(names[k]));
                    else if (verbose) fprintf(stderr, "%s: EOF on %s after byte %ju\n", su_name, dfu_qa(names[k]), at);
                    else fprintf(stderr, "%s: EOF on %s after byte %ju, in line %ju\n", su_name, dfu_qa(names[k]), at, line);
                }
                differ = 1;
            }
            break;
        }
        size_t n0 = in[0].n - in[0].pos, n1 = in[1].n - in[1].pos;
        size_t n = n0 < n1 ? n0 : n1;
        if ((uintmax_t)n > limit - at) n = (size_t)(limit - at);
        const unsigned char *a = in[0].buf + in[0].pos, *b = in[1].buf + in[1].pos;
        for (size_t i = 0; i < n; i++) {
            if (a[i] != b[i]) {
                differ = 1;
                if (silent) return 1;
                if (!verbose) {
                    if (bytes) {
                        char pa[8], pb[8];
                        dfu_cmp_pic(a[i], pa);
                        dfu_cmp_pic(b[i], pb);
                        printf("%s %s differ: byte %ju, line %ju is %3o %s %3o %s\n", names[0], names[1],
                               at + i + 1, line, a[i], pa, b[i], pb);
                    } else
                        printf("%s %s differ: char %ju, line %ju\n", names[0], names[1], at + i + 1, line);
                    fflush(stdout);
                    return 1;
                }
                if (bytes) {
                    char pa[8], pb[8];
                    dfu_cmp_pic(a[i], pa);
                    dfu_cmp_pic(b[i], pb);
                    printf("%*ju %3o %-4s %3o %s\n", width, at + i + 1, a[i], pa, b[i], pb);
                } else
                    printf("%*ju %3o %3o\n", width, at + i + 1, a[i], b[i]);
            }
            if (a[i] == '\n') line++;
        }
        in[0].pos += n;
        in[1].pos += n;
        at += n;
    }
    if (fflush(stdout) != 0) dfu_err(2, errno, "write error");
    return differ;
}

/* ======================================================================
 * sdiff
 *
 * Without -o it is diff -y: GNU's sdiff runs diff, so that is what is
 * done here -- with its options given to diff the way sdiff gives them
 * (measured with a --diff-program that logs them: in the order given,
 * -w as -W and -W as -w, -l as --left-column, -s as
 * --suppress-common-lines at the end, then -y -- FILE1 FILE2), and
 * diff's complaints in diff's name ("diff: invalid width '0'").
 *
 * With -o FILE it merges: the common lines are shown side by side (until
 * s; v shows them again) and copied to FILE, and after each hunk it asks
 * ("%") which version goes into FILE -- l or 1 the left, r or 2 the
 * right, e/eb/ed/el/e1/er/e2 an edited one (EDITOR, or ed, run on a
 * temporary file holding nothing, both versions, both under "--- FILE1
 * N,M" and "+++ FILE2 N,M" headers, the left or the right), q quits.
 * Status: 0 no differences, 1 merged, 2 trouble (q and the end of the
 * commands are trouble).
 * ====================================================================== */

static int dfu_findprog(const char *prog);

static void dfu_getopt_reset(void) {
    su_optind = 1;
    su_nextind = 1;
    su_cluster = 0;
    su_nnonopt = 0;
    su_noptelem = 0;
    su_optarg = 0;
}

static void dfu_sdiff_help(void) {
    printf("Usage: %s [OPTION]... FILE1 FILE2\n"
           "Show FILE1 and FILE2 side by side, or merge them a hunk at a time.\n"
           "\n"
           "Mandatory arguments to long options are mandatory for short options too.\n"
           "  -o, --output=FILE            merge, asking at each hunk, into FILE\n"
           "\n"
           "  -i, --ignore-case            letters match in any case\n"
           "  -E, --ignore-tab-expansion   a tab matches the spaces it expands to\n"
           "  -Z, --ignore-trailing-space  white space at the end does not count\n"
           "  -b, --ignore-space-change    neither do changes in its amount\n"
           "  -W, --ignore-all-space       white space does not count at all\n"
           "  -B, --ignore-blank-lines     changes of blank lines only do not count\n"
           "  -I, --ignore-matching-lines=RE  nor changes whose lines all match RE\n"
           "      --strip-trailing-cr      a CR before a newline is dropped\n"
           "  -a, --text                   every file is text\n"
           "\n"
           "  -w, --width=NUM              at most NUM (130) columns in all\n"
           "  -l, --left-column            common lines only in the left column\n"
           "  -s, --suppress-common-lines  leave the common lines out\n"
           "\n"
           "  -t, --expand-tabs            tabs as spaces in the output\n"
           "      --tabsize=NUM            tab stops NUM (8) columns apart\n"
           "\n"
           "  -d, --minimal                look harder for fewer changes\n"
           "  -H, --speed-large-files      big files with small changes all over\n"
           "      --diff-program=PROGRAM   compare with PROGRAM, not diff\n"
           "\n"
           "      --help                   show this help and exit\n"
           "  -v, --version                show the version and exit\n"
           "\n"
           "'-' is standard input.\n"
           "Exit status: 0 the same, 1 different, 2 trouble.\n", su_name);
}

/* the merge's own list of commands, for one it does not know */
static void dfu_sdiff_cmds(void) {
    fputs("ed:\tedit both versions, each under a header naming where it is from\n"
          "eb:\tedit both versions\n"
          "el or e1:\tedit the left version\n"
          "er or e2:\tedit the right version\n"
          "e:\tedit a new, empty version (both are dropped)\n"
          "l or 1:\ttake the left version\n"
          "r or 2:\ttake the right version\n"
          "s:\tstop showing the common lines\n"
          "v:\tshow the common lines again\n"
          "q:\tquit\n", stdout);
}

static struct { char buf[4096]; size_t n, pos; int eof; } dfu_cin;

/* one command line from standard input, blanks around it dropped: 1, or
 * 0 at the end (a last line without its newline is not a command: -1) */
static int dfu_sdiff_cmd(char *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        if (dfu_cin.pos == dfu_cin.n) {
            if (dfu_cin.eof) return n ? -1 : 0;
            ssize_t r = read(0, dfu_cin.buf, sizeof dfu_cin.buf);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { dfu_cin.eof = 1; continue; }
            dfu_cin.n = (size_t)r;
            dfu_cin.pos = 0;
        }
        char c = dfu_cin.buf[dfu_cin.pos++];
        if (c == '\n') break;
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = 0;
    char *s = out;
    while (isspace((unsigned char)*s)) s++;
    size_t m = strlen(s);
    while (m && isspace((unsigned char)s[m - 1])) m--;
    memmove(out, s, m);
    out[m] = 0;
    return 1;
}

static void dfu_put_lines(FILE *o, const struct dfu_file *f, long b, long e) {
    if (e > b) fwrite(f->lp[b], 1, (size_t)(f->lp[e] - f->lp[b]), o);
}

static void dfu_range_s(FILE *o, long b, long e) {
    if (e - b == 1) fprintf(o, "%ld", b + 1);
    else fprintf(o, "%ld,%ld", b + 1, e);
}

/* KIND: 'e' nothing, 'b' both, 'd' both with headers, 'l', 'r'.
 * 0, or exits with 2 when the editor fails */
static void dfu_sdiff_edit(int kind, const struct dfu_file *f0, const struct dfu_file *f1,
                           const struct dfu_chg *c, FILE *out, const char *n0, const char *n1) {
    const char *tdir = getenv("TMPDIR");
    if (!tdir || !*tdir) tdir = "/tmp";
    char *tmpl = dfu_join(tdir, "sdiffXXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) dfu_err(2, errno, "%s", dfu_qf(tmpl));
    close(fd);                  /* (libtoby has no fdopen) */
    FILE *t = fopen(tmpl, "w");
    if (!t) dfu_err(2, errno, "%s", dfu_qf(tmpl));
    if (kind == 'd' && c->del) { fprintf(t, "--- %s ", n0); dfu_range_s(t, c->l0, c->l0 + c->del); fputc('\n', t); }
    if (kind == 'b' || kind == 'd' || kind == 'l') dfu_put_lines(t, f0, c->l0, c->l0 + c->del);
    if (kind == 'd' && c->ins) { fprintf(t, "+++ %s ", n1); dfu_range_s(t, c->l1, c->l1 + c->ins); fputc('\n', t); }
    if (kind == 'b' || kind == 'd' || kind == 'r') dfu_put_lines(t, f1, c->l1, c->l1 + c->ins);
    fclose(t);
    const char *ed = getenv("EDITOR");
    if (!ed || !*ed) ed = "ed";
    fflush(stdout);
    fflush(out);
    pid_t pid = fork();
    if (pid < 0) dfu_err(2, errno, "fork");
    if (pid == 0) {
        char *av[3] = { (char *)ed, tmpl, 0 };
        execvp(ed, av);
        _exit(errno == ENOENT ? 127 : 126);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
        unlink(tmpl);
        int x = WEXITSTATUS(st);
        if (x == 127) dfu_err(2, 0, "subsidiary program %s not found", dfu_qa(ed));
        if (x == 126) dfu_err(2, 0, "subsidiary program %s could not be invoked", dfu_qa(ed));
        dfu_err(2, 0, "subsidiary program %s failed (exit status %d)", dfu_qa(ed), x);
    }
    if (WIFSIGNALED(st)) {
        unlink(tmpl);
        dfu_err(2, 0, "subsidiary program %s failed (signal %d)", dfu_qa(ed), WTERMSIG(st));
    }
    struct dfu_file e;
    memset(&e, 0, sizeof e);
    e.name = tmpl;
    if (dfu_load(&e) < 0) dfu_err(2, errno, "%s", dfu_qf(tmpl));
    fwrite(e.buf, 1, e.len, out);
    free(e.buf);
    unlink(tmpl);
    free(tmpl);
}

static int dfu_sdiff_merge(const char *outname, const char *n0, const char *n1, int silent) {
    struct dfu_file f[2];
    memset(f, 0, sizeof f);
    f[0].name = n0;
    f[1].name = n1;
    for (int k = 0; k < 2; k++) {
        f[k].isstdin = !strcmp(f[k].name, "-");
        if (dfu_load(&f[k]) < 0) dfu_err(2, errno, "%s", dfu_qf(f[k].name));
    }
    FILE *out = fopen(outname, "w");
    if (!out) dfu_err(2, errno, "%s", dfu_qf(outname));
    dfu_lines(&f[0]);
    dfu_lines(&f[1]);
    dfu_cls_reset();
    dfu_classify(&f[0]);
    dfu_classify(&f[1]);
    long n;
    struct dfu_chg *ch = dfu_analyze(&f[0], &f[1], &n);
    dfu_mark_ignorable(&f[0], &f[1], ch, n);
    dfu_sb_layout();
    dfx.suppress_common = 0;
    long i0 = 0, i1 = 0;
    int any = 0;
    char cmd[256];
    for (long k = 0; k < n; k++) {
        const struct dfu_chg *c = &ch[k];
        if (c->ign) continue;
        any = 1;
        if (!silent) dfu_sb_common(&f[0], i0, c->l0, &f[1], i1, c->l1);
        dfu_put_lines(out, &f[0], i0, c->l0);
        long both = c->del < c->ins ? c->del : c->ins;
        for (long j = 0; j < both; j++) dfu_sb_line(&f[0], c->l0 + j, '|', &f[1], c->l1 + j);
        for (long j = both; j < c->del; j++) dfu_sb_line(&f[0], c->l0 + j, '<', 0, 0);
        for (long j = both; j < c->ins; j++) dfu_sb_line(0, 0, '>', &f[1], c->l1 + j);
        for (;;) {
            fputc('%', stdout);
            fflush(stdout);
            int r = dfu_sdiff_cmd(cmd, sizeof cmd);
            if (r == 0) { fclose(out); return 2; }
            if (r < 0) { dfu_sdiff_cmds(); continue; }
            if (!strcmp(cmd, "s")) { silent = 1; continue; }
            if (!strcmp(cmd, "v")) { silent = 0; continue; }
            if (!strcmp(cmd, "q")) { fclose(out); return 2; }
            if (!strcmp(cmd, "l") || !strcmp(cmd, "1")) { dfu_put_lines(out, &f[0], c->l0, c->l0 + c->del); break; }
            if (!strcmp(cmd, "r") || !strcmp(cmd, "2")) { dfu_put_lines(out, &f[1], c->l1, c->l1 + c->ins); break; }
            int kind = !strcmp(cmd, "e") ? 'e' : !strcmp(cmd, "eb") ? 'b' : !strcmp(cmd, "ed") ? 'd'
                     : !strcmp(cmd, "el") || !strcmp(cmd, "e1") ? 'l'
                     : !strcmp(cmd, "er") || !strcmp(cmd, "e2") ? 'r' : 0;
            if (kind) { dfu_sdiff_edit(kind, &f[0], &f[1], c, out, n0, n1); break; }
            dfu_sdiff_cmds();
        }
        i0 = c->l0 + c->del;
        i1 = c->l1 + c->ins;
    }
    if (!silent) dfu_sb_common(&f[0], i0, f[0].nl, &f[1], i1, f[1].nl);
    dfu_put_lines(out, &f[0], i0, f[0].nl);
    if (fclose(out) != 0) dfu_err(2, errno, "%s", dfu_qf(outname));
    fflush(stdout);
    free(ch);
    return any;
}

static int do_sdiff(int argc, char **argv) {
    enum { S_DIFFPROG = 0x100, S_STRIPCR, S_TABSIZE };
    static const struct su_option longs[] = {
        { "diff-program", SU_REQ_ARG, 0, S_DIFFPROG },
        { "expand-tabs", SU_NO_ARG, 0, 't' },
        { "help", SU_NO_ARG, 0, SU_HELP_OPT },
        { "ignore-all-space", SU_NO_ARG, 0, 'W' },
        { "ignore-blank-lines", SU_NO_ARG, 0, 'B' },
        { "ignore-case", SU_NO_ARG, 0, 'i' },
        { "ignore-matching-lines", SU_REQ_ARG, 0, 'I' },
        { "ignore-space-change", SU_NO_ARG, 0, 'b' },
        { "ignore-tab-expansion", SU_NO_ARG, 0, 'E' },
        { "ignore-trailing-space", SU_NO_ARG, 0, 'Z' },
        { "left-column", SU_NO_ARG, 0, 'l' },
        { "minimal", SU_NO_ARG, 0, 'd' },
        { "output", SU_REQ_ARG, 0, 'o' },
        { "speed-large-files", SU_NO_ARG, 0, 'H' },
        { "strip-trailing-cr", SU_NO_ARG, 0, S_STRIPCR },
        { "suppress-common-lines", SU_NO_ARG, 0, 's' },
        { "tabsize", SU_REQ_ARG, 0, S_TABSIZE },
        { "text", SU_NO_ARG, 0, 'a' },
        { "version", SU_NO_ARG, 0, 'v' },
        { "width", SU_REQ_ARG, 0, 'w' },
        { 0, 0, 0, 0 }
    };
    /* diff's arguments, built as sdiff gives them */
    char **dv = dfu_alloc((size_t)(argc * 2 + 8) * sizeof *dv);
    int dn = 1, suppress = 0, left = 0;
    const char *output = 0, *prog = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, "abBdEHiI:lo:stvw:WZ", longs, 0)) != -1) {
        switch (c) {
        case 'a': dv[dn++] = "-a"; break;
        case 'b': dv[dn++] = "-b"; break;
        case 'B': dv[dn++] = "-B"; break;
        case 'd': dv[dn++] = "-d"; break;
        case 'E': dv[dn++] = "-E"; break;
        case 'H': dv[dn++] = "-H"; break;
        case 'i': dv[dn++] = "-i"; break;
        case 'I': dv[dn++] = "-I"; dv[dn++] = su_optarg; break;
        case 'l': dv[dn++] = "--left-column"; left = 1; break;
        case 'o': output = su_optarg; break;
        case 's': suppress = 1; break;
        case 't': dv[dn++] = "-t"; break;
        case 'v': return su_version();
        case 'w': dv[dn++] = "-W"; dv[dn++] = su_optarg; break;
        case 'W': dv[dn++] = "-w"; break;
        case 'Z': dv[dn++] = "-Z"; break;
        case S_DIFFPROG: prog = su_optarg; break;
        case S_STRIPCR: dv[dn++] = "--strip-trailing-cr"; break;
        case S_TABSIZE: dv[dn++] = "--tabsize"; dv[dn++] = su_optarg; break;
        case SU_HELP_OPT: dfu_sdiff_help(); return 0;
        default: dfu_try_help();
        }
    }
    int nops = argc - su_optind;
    if (nops < 2) dfu_usage_error("missing operand after %s", su_quote(argv[argc - 1]));
    if (nops > 2) dfu_usage_error("extra operand %s", su_quote(argv[su_optind + 2]));
    const char *n0 = argv[su_optind], *n1 = argv[su_optind + 1];
    if (!output) {
        if (suppress) dv[dn++] = "--suppress-common-lines";
        dv[dn++] = "-y";
    } else {
        dv[dn++] = "--sdiff-merge-assist";
    }
    dv[dn++] = "--";
    dv[dn++] = (char *)n0;
    dv[dn++] = (char *)n1;
    dv[dn] = 0;
    if (prog && !output) {
        dv[0] = (char *)prog;
        fflush(stdout);
        pid_t pid = fork();
        if (pid < 0) dfu_err(2, errno, "fork");
        if (pid == 0) {
            execvp(prog, dv);
            int e = errno;
            dfu_err(0, e, "%s", prog);
            _exit(2);
        }
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        if (WIFEXITED(st)) return WEXITSTATUS(st);
        return 2;
    }
    /* -o: the comparison is made here; a --diff-program is only looked for */
    if (prog && !dfu_findprog(prog)) dfu_err(2, 0, "subsidiary program %s not found", dfu_qa(prog));
    /* diff, in diff's name */
    const char *myname = su_name;
    dv[0] = "diff";
    su_set_name("diff");
    dfu_getopt_reset();
    if (!output) return do_diff(dn, dv);
    if (dfu_diff_parse(dn, dv) >= 0) return 2;
    su_set_name(myname);
    (void)left;
    return dfu_sdiff_merge(output, n0, n1, suppress);
}

/* ======================================================================
 * diff3
 *
 * As GNU's: two two-way diffs against one file -- FILE3 for the plain
 * report, OLDFILE for the ed scripts and -m (measured with a logging
 * --diff-program: "diff --horizon-lines=100 ---no-directory -- OTHER
 * COMMON", the other file first, then MYFILE) -- merged into blocks:
 * hunks that overlap or touch in the common file are one block. A block
 * changed in one file only is "====1", "====2" or "====3"; changed in
 * two the same way, "====" plus the one that differs; else "====".
 * ====================================================================== */

struct dfu_blk {
    int kind;                   /* 0 all differ, else the file that does */
    long lo[3], hi[3];          /* the block in each file: [lo,hi) */
};

/* the hunks of F (0) against the common file C (1) */
static struct dfu_chg *dfu_d3_diff(struct dfu_file *f, struct dfu_file *c, long *n) {
    dfu_cls_reset();
    dfu_classify(f);
    dfu_classify(c);
    return dfu_analyze(f, c, n);
}

static int dfu_d3_nl;          /* the newline counts (not for ed scripts) */

static int dfu_d3_same(const struct dfu_file *a, long alo, long ahi,
                       const struct dfu_file *b, long blo, long bhi) {
    if (ahi - alo != bhi - blo) return 0;
    /* for an ed script, which cannot write one, an incomplete last line
     * is the same as the line with a newline (measured); for -m not */
    for (long i = 0; i < ahi - alo; i++) {
        const char *sa, *sb;
        size_t na = dfu_text(a, alo + i, &sa), nb = dfu_text(b, blo + i, &sb);
        if (na != nb || memcmp(sa, sb, na)) return 0;
        if (dfu_d3_nl && dfu_isincomplete(a, alo + i) != dfu_isincomplete(b, blo + i)) return 0;
    }
    return 1;
}

/* the blocks. F[3] the files; COMMON the index of the common one; the
 * hunks of OTHER (the remaining file that is not file 0) and of file 0
 * against it */
static struct dfu_blk *dfu_d3_blocks(struct dfu_file *f, int common, int other,
                                     const struct dfu_chg *h0, long n0,
                                     const struct dfu_chg *h1, long n1, long *nb) {
    long cap = 16, k = 0;
    struct dfu_blk *b = dfu_alloc((size_t)cap * sizeof *b);
    long p0 = 0, p1 = 0;
    long d0 = 0, d1 = 0;          /* offsets (other line - common line) */
    const struct dfu_chg *H[2] = { h0, h1 };
    long N[2] = { n0, n1 };
    int who[2] = { other, 0 };
    while (p0 < n0 || p1 < n1) {
        long lo, hi;
        if (p1 >= n1 || (p0 < n0 && h0[p0].l1 <= h1[p1].l1)) { lo = h0[p0].l1; hi = lo + h0[p0].ins; }
        else { lo = h1[p1].l1; hi = lo + h1[p1].ins; }
        long s[2] = { p0, p1 }, e[2] = { p0, p1 };
        for (int grew = 1; grew;) {
            grew = 0;
            for (int l = 0; l < 2; l++) {
                while (e[l] < N[l] && H[l][e[l]].l1 <= hi) {
                    long ch = H[l][e[l]].l1 + H[l][e[l]].ins;
                    if (ch > hi) hi = ch;
                    e[l]++;
                    grew = 1;
                }
            }
        }
        if (k == cap) { cap *= 2; b = dfu_realloc(b, (size_t)cap * sizeof *b); }
        struct dfu_blk *x = &b[k++];
        x->lo[common] = lo;
        x->hi[common] = hi;
        long *dd[2] = { &d0, &d1 };
        for (int l = 0; l < 2; l++) {
            int fi = who[l];
            if (e[l] > s[l]) {
                const struct dfu_chg *a = &H[l][s[l]], *z = &H[l][e[l] - 1];
                x->lo[fi] = a->l0 - (a->l1 - lo);
                x->hi[fi] = z->l0 + z->del + (hi - (z->l1 + z->ins));
                *dd[l] = (z->l0 + z->del) - (z->l1 + z->ins);
            } else {
                x->lo[fi] = lo + *dd[l];
                x->hi[fi] = hi + *dd[l];
            }
        }
        int c0 = e[1] > s[1], c1 = e[0] > s[0];      /* file 0 changed, OTHER changed */
        if (c0 && !c1) x->kind = 1;
        else if (!c0 && c1) x->kind = other + 1;
        else x->kind = dfu_d3_same(&f[0], x->lo[0], x->hi[0], &f[other], x->lo[other], x->hi[other])
                           ? common + 1 : 0;
        p0 = e[0];
        p1 = e[1];
    }
    *nb = k;
    return b;
}

static void dfu_d3_lines(FILE *o, const struct dfu_file *f, long lo, long hi, const char *pfx) {
    for (long i = lo; i < hi; i++) {
        fputs(pfx, o);
        fwrite(f->lp[i], 1, (size_t)(f->lp[i + 1] - f->lp[i]), o);
        if (dfu_isincomplete(f, i)) fputs("\n\\ No newline at end of file\n", o);
    }
}

static void dfu_d3_hdr(int fi, long lo, long hi) {
    if (hi > lo) {
        if (hi - lo == 1) printf("%d:%ldc\n", fi + 1, hi);
        else printf("%d:%ld,%ldc\n", fi + 1, lo + 1, hi);
    } else printf("%d:%lda\n", fi + 1, lo);
}

/* lines for an ed script: written whole, a newline added to an
 * incomplete one; does one of them begin with '.'? */
static int dfu_d3_dotted(const struct dfu_file *f, long lo, long hi) {
    for (long i = lo; i < hi; i++) if (f->lp[i] < f->lp[i + 1] && f->lp[i][0] == '.') return 1;
    return 0;
}

static void dfu_d3_edlines(const struct dfu_file *f, long lo, long hi, int dots) {
    for (long i = lo; i < hi; i++) {
        const char *s;
        size_t n = dfu_text(f, i, &s);
        if (dots && n && s[0] == '.') putchar('.');
        fwrite(s, 1, n, stdout);
        putchar('\n');
    }
}

static void dfu_d3_srange(long a, long z) {
    if (a == z) printf("%lds/^\\.//\n", a);
    else printf("%ld,%lds/^\\.//\n", a, z);
}

static void dfu_d3_help(void) {
    printf("Usage: %s [OPTION]... MYFILE OLDFILE YOURFILE\n"
           "Compare three files a line at a time.\n"
           "\n"
           "Mandatory arguments to long options are mandatory for short options too.\n"
           "  -A, --show-all              every change, conflicts between markers\n"
           "\n"
           "  -e, --ed                    an ed script bringing into MYFILE the changes\n"
           "                                from OLDFILE to YOURFILE\n"
           "  -E, --show-overlap          the same, with markers around conflicts\n"
           "  -3, --easy-only             the same, only the changes that do not overlap\n"
           "  -x, --overlap-only          the same, only the changes that overlap\n"
           "  -X                          the same as -x\n"
           "  -i                          end the ed script with 'w' and 'q'\n"
           "\n"
           "  -m, --merge                 the merged file itself (as -A, unless\n"
           "                                another of the above is given)\n"
           "\n"
           "  -a, --text                  every file is text\n"
           "      --strip-trailing-cr     a CR before a newline is dropped\n"
           "  -T, --initial-tab           a tab before each line's text\n"
           "      --diff-program=PROGRAM  compare with PROGRAM, not diff\n"
           "  -L, --label=LABEL           LABEL in place of a file's name (once for each\n"
           "                                file, three at most)\n"
           "\n"
           "      --help                  show this help and exit\n"
           "  -v, --version               show the version and exit\n"
           "\n"
           "Without -e -E -x -X -3 or -m the changes are listed, for reading.\n"
           "'-' is standard input.\n"
           "Exit status: 0 done, 1 conflicts, 2 trouble.\n", su_name);
}

/* can PROG be run: a path, or a name found on $PATH */
static int dfu_findprog(const char *prog) {
    if (strchr(prog, '/')) return access(prog, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin";
    for (const char *p = path;;) {
        const char *e = strchr(p, ':');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char *dir = dfu_alloc(n + 2);
        memcpy(dir, n ? p : ".", n ? n : 1);
        dir[n ? n : 1] = 0;
        char *full = dfu_join(dir, prog);
        int ok = access(full, X_OK) == 0;
        free(full);
        free(dir);
        if (ok) return 1;
        if (!e) return 0;
        p = e + 1;
    }
}

/* "diff: NAME: ..." and the failure diff3 reports after it */
static void dfu_d3_fail(void) __attribute__((noreturn));
static void dfu_d3_fail(void) {
    fflush(stdout);
    fprintf(stderr, "%s: subsidiary program 'diff' failed (exit status 2)\n", su_name);
    exit(2);
}

static int dfu_d3_load(struct dfu_file *f) {
    f->isstdin = !strcmp(f->name, "-");
    struct stat st;
    if (!f->isstdin && stat(f->name, &st) == 0 && S_ISDIR(st.st_mode)) { errno = EISDIR; return -1; }
    if (dfu_load(f) < 0) return -1;
    return 0;
}

static int do_diff3(int argc, char **argv) {
    enum { D_DIFFPROG = 0x100, D_STRIPCR };
    static const struct su_option longs[] = {
        { "diff-program", SU_REQ_ARG, 0, D_DIFFPROG },
        { "easy-only", SU_NO_ARG, 0, '3' },
        { "ed", SU_NO_ARG, 0, 'e' },
        { "help", SU_NO_ARG, 0, SU_HELP_OPT },
        { "initial-tab", SU_NO_ARG, 0, 'T' },
        { "label", SU_REQ_ARG, 0, 'L' },
        { "merge", SU_NO_ARG, 0, 'm' },
        { "overlap-only", SU_NO_ARG, 0, 'x' },
        { "show-all", SU_NO_ARG, 0, 'A' },
        { "show-overlap", SU_NO_ARG, 0, 'E' },
        { "strip-trailing-cr", SU_NO_ARG, 0, D_STRIPCR },
        { "text", SU_NO_ARG, 0, 'a' },
        { "version", SU_NO_ARG, 0, 'v' },
        { 0, 0, 0, 0 }
    };
    memset(&dfu_o, 0, sizeof dfu_o);
    memset(&dfx, 0, sizeof dfx);
    dfu_o.tabsize = 8;
    int mode = 0, nmodes = 0, merge = 0, wq = 0, tab = 0, nlabel = 0;
    const char *prog = 0;
    const char *label[3] = { 0, 0, 0 };
    int c;
    while ((c = su_getopt_long(argc, argv, "3aAeEiL:mTvxX", longs, 0)) != -1) {
        switch (c) {
        case '3': case 'A': case 'e': case 'E': case 'x': case 'X':
            nmodes++;
            mode = c;
            break;
        case 'a': dfu_o.text = 1; break;
        case 'i': wq = 1; break;
        case 'L':
            if (nlabel >= 3) dfu_usage_error("too many file label options");
            label[nlabel++] = su_optarg;
            break;
        case 'm': merge = 1; break;
        case 'T': tab = 1; break;
        case 'v': return su_version();
        case D_DIFFPROG: prog = su_optarg; break;
        case D_STRIPCR: dfu_o.strip_cr = 1; break;
        case SU_HELP_OPT: dfu_d3_help(); return 0;
        default: dfu_try_help();
        }
    }
    if (merge && !nmodes) mode = 'A';
    /* markers (and so labels) are for -A and -E only */
    int flagging = mode == 'A' || mode == 'E';
    if (nmodes > 1 || (nlabel && !flagging) || (merge && wq)) dfu_usage_error("incompatible options");
    int nops = argc - su_optind;
    if (nops < 3) dfu_usage_error("missing operand after %s", su_quote(argv[argc - 1]));
    if (nops > 3) dfu_usage_error("extra operand %s", su_quote(argv[su_optind + 3]));
    char **ops = argv + su_optind;
    /* the comparisons are made here; a --diff-program is only looked for */
    if (prog && !dfu_findprog(prog)) dfu_err(2, 0, "subsidiary program %s not found", dfu_qa(prog));
    int edmode = mode || merge;
    int common = edmode ? 1 : 2, other = edmode ? 2 : 1;
    struct dfu_file f[3];
    memset(f, 0, sizeof f);
    for (int k = 0; k < 3; k++) f[k].name = ops[k];
    /* the files, in the order the two diffs would open them */
    int order[2][2] = { { other, common }, { 0, common } };
    int loaded[3] = { 0, 0, 0 };
    for (int p = 0; p < 2; p++) {
        int bad = 0;
        for (int q = 0; q < 2; q++) {
            int k = order[p][q];
            if (loaded[k]) continue;
            if (dfu_d3_load(&f[k]) < 0) {
                fflush(stdout);
                fprintf(stderr, "diff: %s: %s\n", dfu_qf(f[k].name), strerror(errno));
                bad = 1;
            } else loaded[k] = 1;
        }
        if (bad) dfu_d3_fail();
        int a = order[p][0], b = order[p][1];
        if (!dfu_o.text && (dfu_isbinary(&f[a]) || dfu_isbinary(&f[b])) && !dfu_eq_bytes(&f[a], &f[b])) {
            fflush(stdout);
            fprintf(stderr, "%s: diff failed: Binary files %s and %s differ\n", su_name, f[a].name, f[b].name);
            exit(2);
        }
    }
    /* -m writes MYFILE's own lines as they are, CRs and all; only the
     * comparisons go without them (measured with --strip-trailing-cr) */
    struct dfu_file raw;
    memset(&raw, 0, sizeof raw);
    raw.name = f[0].name;
    raw.buf = dfu_alloc(f[0].len + 1);
    memcpy(raw.buf, f[0].buf, f[0].len + 1);
    raw.len = f[0].len;
    {
        int sc = dfu_o.strip_cr;
        dfu_o.strip_cr = 0;
        dfu_lines(&raw);
        dfu_o.strip_cr = sc;
    }
    for (int k = 0; k < 3; k++) dfu_lines(&f[k]);
    dfu_horizon = 100;
    long n0, n1;
    struct dfu_chg *h0 = dfu_d3_diff(&f[other], &f[common], &n0);
    struct dfu_chg *h1 = dfu_d3_diff(&f[0], &f[common], &n1);
    /* an ed script cannot end a file without a newline: one complaint
     * for each incomplete line a two-way hunk holds (measured) */
    if (mode && !merge) {
        const struct dfu_chg *H[2] = { h0, h1 };
        long N[2] = { n0, n1 };
        int F[2][2] = { { other, common }, { 0, common } };
        for (int p = 0; p < 2; p++)
            for (long k = 0; k < N[p]; k++) {
                const struct dfu_file *a = &f[F[p][0]], *b = &f[F[p][1]];
                if (a->incomplete && H[p][k].del && H[p][k].l0 + H[p][k].del == a->nl) {
                    fflush(stdout); fprintf(stderr, "%s: No newline at end of file\n", su_name);
                }
                if (b->incomplete && H[p][k].ins && H[p][k].l1 + H[p][k].ins == b->nl) {
                    fflush(stdout); fprintf(stderr, "%s: No newline at end of file\n", su_name);
                }
            }
    }
    long nb;
    dfu_d3_nl = !mode || merge;
    struct dfu_blk *bl = dfu_d3_blocks(f, common, other, h0, n0, h1, n1, &nb);
    const char *L[3];
    for (int k = 0; k < 3; k++) L[k] = label[k] ? label[k] : f[k].name;
    int conflicts = 0;
    if (!edmode) {
        const char *pfx = tab ? "\t" : "  ";
        for (long i = 0; i < nb; i++) {
            struct dfu_blk *x = &bl[i];
            if (x->kind) printf("====%d\n", x->kind);
            else printf("====\n");
            /* the files with the same text together, the first of them
             * shown for all */
            int groups[3][3], gn[3] = { 0, 0, 0 }, ng = 0;
            for (int k = 0; k < 3; k++) {
                int g = -1;
                if (x->kind && k + 1 != x->kind)
                    for (int q = 0; q < ng; q++) if (groups[q][0] + 1 != x->kind) g = q;
                if (g < 0) { g = ng++; gn[g] = 0; }
                groups[g][gn[g]++] = k;
            }
            for (int q = 0; q < ng; q++) {
                for (int r = 0; r < gn[q]; r++) dfu_d3_hdr(groups[q][r], x->lo[groups[q][r]], x->hi[groups[q][r]]);
                int k0 = groups[q][0];
                dfu_d3_lines(stdout, &f[k0], x->lo[k0], x->hi[k0], pfx);
            }
        }
    } else if (merge) {
        long pos = 0;
        for (long i = 0; i < nb; i++) {
            struct dfu_blk *x = &bl[i];
            dfu_put_lines(stdout, &raw, pos, x->lo[0]);
            pos = x->hi[0];
            int k = x->kind;
            if (k == 2 && mode == 'A') {
                printf("<<<<<<< %s\n", L[1]);
                dfu_put_lines(stdout, &f[1], x->lo[1], x->hi[1]);
                fputs("=======\n", stdout);
                dfu_put_lines(stdout, &f[0], x->lo[0], x->hi[0]);
                printf(">>>>>>> %s\n", L[2]);
                conflicts = 1;
            } else if (k == 3 && mode != 'x' && mode != 'X') {
                dfu_put_lines(stdout, &f[2], x->lo[2], x->hi[2]);
            } else if (k == 0 && (mode == 'A' || mode == 'E')) {
                printf("<<<<<<< %s\n", L[0]);
                dfu_put_lines(stdout, &f[0], x->lo[0], x->hi[0]);
                if (mode == 'A') {
                    printf("||||||| %s\n", L[1]);
                    dfu_put_lines(stdout, &f[1], x->lo[1], x->hi[1]);
                }
                fputs("=======\n", stdout);
                dfu_put_lines(stdout, &f[2], x->lo[2], x->hi[2]);
                printf(">>>>>>> %s\n", L[2]);
                conflicts = 1;
            } else if (k == 0 && mode != '3') {
                dfu_put_lines(stdout, &f[2], x->lo[2], x->hi[2]);
            } else {
                dfu_put_lines(stdout, &raw, x->lo[0], x->hi[0]);
            }
        }
        dfu_put_lines(stdout, &raw, pos, f[0].nl);
    } else {
        for (long i = nb - 1; i >= 0; i--) {
            struct dfu_blk *x = &bl[i];
            int k = x->kind;
            long mlo = x->lo[0], mhi = x->hi[0];
            if (k == 2 && mode == 'A') {
                printf("%lda\n>>>>>>> %s\n.\n", mhi, L[2]);
                int dots = dfu_d3_dotted(&f[1], x->lo[1], x->hi[1]);
                printf("%lda\n<<<<<<< %s\n", mlo, L[1]);
                dfu_d3_edlines(&f[1], x->lo[1], x->hi[1], dots);
                fputs("=======\n.\n", stdout);
                if (dots) dfu_d3_srange(mlo + 2, mlo + 1 + (x->hi[1] - x->lo[1]));
                conflicts = 1;
            } else if (k == 0 && (mode == 'A' || mode == 'E')) {
                long no = x->hi[1] - x->lo[1], ny = x->hi[2] - x->lo[2];
                int dots = dfu_d3_dotted(&f[2], x->lo[2], x->hi[2]) ||
                           (mode == 'A' && dfu_d3_dotted(&f[1], x->lo[1], x->hi[1]));
                printf("%lda\n", mhi);
                if (mode == 'A') {
                    printf("||||||| %s\n", L[1]);
                    dfu_d3_edlines(&f[1], x->lo[1], x->hi[1], dots);
                }
                fputs("=======\n", stdout);
                dfu_d3_edlines(&f[2], x->lo[2], x->hi[2], dots);
                printf(">>>>>>> %s\n.\n", L[2]);
                if (dots) dfu_d3_srange(mhi + 2, mhi + 2 + no + ny);
                printf("%lda\n<<<<<<< %s\n.\n", mlo, L[0]);
                conflicts = 1;
            } else if ((k == 3 && mode != 'x' && mode != 'X') || (k == 0 && mode != '3')) {
                long ny = x->hi[2] - x->lo[2];
                int dots = dfu_d3_dotted(&f[2], x->lo[2], x->hi[2]);
                if (mhi > mlo) {
                    if (mhi - mlo == 1) printf("%ld", mhi);
                    else printf("%ld,%ld", mlo + 1, mhi);
                    if (!ny) { fputs("d\n", stdout); continue; }
                    fputs("c\n", stdout);
                } else {
                    if (!ny) continue;
                    printf("%lda\n", mlo);
                }
                dfu_d3_edlines(&f[2], x->lo[2], x->hi[2], dots);
                fputs(".\n", stdout);
                if (dots) dfu_d3_srange(mlo + 1, mlo + ny);
            }
        }
        if (wq) fputs("w\nq\n", stdout);
    }
    if (fflush(stdout) != 0) dfu_err(2, errno, "write error");
    return conflicts;
}
