/* su_find.c -- find, as GNU findutils 4.10.0's (logs/utilgate/staging/
 * find.sh): walk each starting point's tree and evaluate an expression of
 * tests, actions and operators on every file in it.
 *
 * Everything here was measured against WSL's /usr/bin/find (GNU findutils
 * 4.10.0) in the C locale; nothing was taken from its sources.
 *
 * THE COMMAND LINE: -H -L -P -D LIST -OLEVEL first (each a whole word; the
 * last of -H -L -P wins), then the starting points -- every word up to one
 * that starts with '-' (but "-" itself is a path), "(" or "!" -- then the
 * expression. No starting point means ".". Every word of the expression is
 * read into a predicate first (so "missing argument" and "unknown
 * predicate" come before any complaint about its shape), then the
 * predicates are built into a tree: ( ) ! -not, implicit or explicit -a
 * -and, -o -or, "," -- with GNU's own words for each malformed shape. With
 * no action but -prune or -quit, the whole is "( EXPR ) -a -print".
 *
 * THE ORDER OF EVALUATION is GNU's -O1 (the default; -O0 is the same):
 * within each run of -a'ed predicates with no side effects, and among the
 * operands of an -o, the tests that need only the name (-name -path -regex
 * and their i-forms, -true -false and the options) are moved first; the
 * rest keep their order. It is visible: `-empty -name x` never opens a
 * directory that is not named x, so it says nothing of one it cannot read.
 *
 * THE WALK is fts's, as GNU uses it: a directory's entries are read whole,
 * in the order readdir gives them (never sorted), "." and ".." skipped; a
 * directory is visited before its entries (after them with -depth or
 * -delete). A file is examined with stat only when something needs it:
 * readdir's d_type answers -type and tells a directory, so `find D` lists
 * a directory it may read but not search, and only a test that needs the
 * inode fails there ("Permission denied", once per file). -L follows every
 * link (a dangling one is reported as the link), -H the starting points;
 * a directory that is its own ancestor under -L is "File system loop
 * detected"; one that cannot be read is reported after it is visited (or
 * before, with -depth).
 *
 * TESTS take N as N, +N (more) or -N (less). Times: -Xmin and -used count
 * whole units back from now, rounding up (-mmin 1 is the last minute);
 * -Xtime counts whole days, rounding down (-mtime 0 is the last 24 hours),
 * and -daystart measures from the end of today instead; fractions and
 * C99 numbers (1.5, 0x1p3, inf) are taken as strtod takes them. -size
 * rounds up to its unit (b c w k M G). -perm MODE, -MODE (all of), /MODE
 * (any of), octal or chmod's symbolic form computed from 0 (X counts for
 * directories only). -newerXY with a b c m t (no birth times here: "This
 * system does not provide a way to find the birth time of a file").
 *
 * ACTIONS: -print -print0 -printf (every directive and flag GNU has; %Z and
 * -context are SELinux's, absent) -fprint* -ls -fls (ls -dils, names
 * escaped), -exec/-execdir/-ok/-okdir with ; or {} +, -delete, -prune,
 * -quit. -ok and -okdir ask "< CMD ... FILE > ? " on stderr and read a line
 * from stdin (yes if it starts with y or Y). On a terminal -print and the
 * name directives of -printf show an unprintable byte as '?'.
 *
 * WARNINGS (-warn, or by default when stdin is a terminal; never under
 * POSIXLY_CORRECT): a global option after a test or operator, -name with a
 * '/', -d. -path ending in '/' is warned of without -warn too (unless a
 * starting point matches it), -perm /000 always. Messages name find as it
 * was invoked ("/usr/bin/find: ..."), as GNU's do. Errors make the exit
 * status 1; -exec ... ; failing does not, -exec ... + failing does.
 *
 * -D: exec, stat, time and search print GNU's lines, each where GNU's
 * walk prints it; help lists the names with descriptions in our own
 * words. tree, opt and rates print the predicate list and trees in GNU's
 * layout, but not GNU's cost model (its estimated success rates and its
 * normalised and optimised trees), so their output is not GNU's.
 *
 * The old find (programs/user_find) knew a subset: no -regex, -newerXY,
 * -fprint*, -fls, -execdir, -ok, -files0-from, -D, -O; no warnings; a
 * -printf without widths or most directives; and it stat'ed every file. */

#include <sys/sysmacros.h>

#define FD_DAY 86400L
#define FD_NS 1000000000L

/* nanoseconds since the epoch, wide enough for any N*86400 */
typedef __int128 fd_i128;
#define FD_BIG ((fd_i128)1 << 100)

static int fd_status;             /* 1 once anything failed */
static int fd_quitting;           /* -quit was run */

static void *fd_alloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) su_error(1, 0, "memory exhausted");
    return p;
}

static void *fd_realloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) su_error(1, 0, "memory exhausted");
    return q;
}

static char *fd_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)fd_alloc(n);
    memcpy(p, s, n);
    return p;
}

/* ---- a growable string -------------------------------------------------- */

struct fd_buf { char *s; size_t n, cap; };

static void fd_bput(struct fd_buf *b, const char *p, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 32;
        b->s = (char *)fd_realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void fd_bputs(struct fd_buf *b, const char *s) { fd_bput(b, s, strlen(s)); }
static void fd_bputc(struct fd_buf *b, int c) { char ch = (char)c; fd_bput(b, &ch, 1); }
static void fd_bprintf(struct fd_buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fd_bprintf(struct fd_buf *b, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { fd_bput(b, tmp, (size_t)n); return; }
    char *big = (char *)fd_alloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    fd_bput(b, big, (size_t)n);
    free(big);
}

/* ---- quoting -------------------------------------------------------------
 *
 * GNU find quotes a name in a message as gnulib's quote() does in the C
 * locale: 'NAME', a quote or a backslash in it escaped, \a \b \f \n \r \t
 * \v for those controls, any other control, DEL or byte past 0x7f as \OOO
 * (measured: find nosuch/E-ACUTE says 'nosuch/\303\251'). The results live in a
 * ring of buffers so a message can hold several. */
static char *fd_qring[8];
static int fd_qslot;

static int fd_cesc(int c) {
    switch (c) {
    case '\a': return 'a';
    case '\b': return 'b';
    case '\f': return 'f';
    case '\n': return 'n';
    case '\r': return 'r';
    case '\t': return 't';
    case '\v': return 'v';
    }
    return 0;
}

static const char *fd_q(const char *s) {
    struct fd_buf b = { 0, 0, 0 };
    fd_bputc(&b, '\'');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\'' || c == '\\') { fd_bputc(&b, '\\'); fd_bputc(&b, c); }
        else if (fd_cesc(c)) { fd_bputc(&b, '\\'); fd_bputc(&b, fd_cesc(c)); }
        else if (c < 0x20 || c >= 0x7f) fd_bprintf(&b, "\\%03o", c);
        else fd_bputc(&b, c);
    }
    fd_bputc(&b, '\'');
    int k = fd_qslot++ & 7;
    free(fd_qring[k]);
    fd_qring[k] = b.s;
    return b.s;
}

/* -ls and -fls: a blank, a backslash and a double quote as C escapes
 * ("\ ", "\\", "\""), the named controls as \n and the like, every other
 * byte outside 041..0176 as \OOO. */
static void fd_ls_name(struct fd_buf *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == ' ' || c == '\\' || c == '"') { fd_bputc(b, '\\'); fd_bputc(b, c); }
        else if (fd_cesc(c)) { fd_bputc(b, '\\'); fd_bputc(b, fd_cesc(c)); }
        else if (c < 041 || c > 0176) fd_bprintf(b, "\\%03o", c);
        else fd_bputc(b, c);
    }
}

/* ---- numbers ---------------------------------------------------------------
 *
 * An unsigned decimal as gnulib's xstrtoumax reads it: blanks, an optional
 * '+', digits, nothing after. 0, or -1 (not one), or -2 (too big). */
static int fd_umax(const char *s, uintmax_t *out) {
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    if (*p == '+') p++;
    if (*p < '0' || *p > '9') return -1;
    uintmax_t v = 0;
    int over = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINTMAX_MAX - d) / 10) over = 1;
        else v = v * 10 + d;
    }
    if (*p) return -1;
    if (over) return -2;
    *out = v;
    return 0;
}

enum { FD_EQ, FD_GT, FD_LT };

/* A number as strtod reads one in the C locale, scaled to nanoseconds of
 * UNIT seconds: blanks, a sign, then decimal digits with a point and an
 * exponent, or 0x hex digits with a point and a binary exponent, or inf,
 * infinity, nan, nan(...) in any case. The tobyOS build has no floating
 * point at all, so the value is kept as a saturating 128-bit count of
 * nanoseconds. Returns 0, -1 (not a number, or out of a double's range, as
 * strtod's ERANGE), or 1 (a NaN). */
static int fd_strtod_ns(const char *s, long unit, fd_i128 *out) {
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
    if (!strncasecmp(p, "nan", 3)) {
        p += 3;
        if (*p == '(') {
            const char *q = p + 1;
            while (isalnum((unsigned char)*q) || *q == '_') q++;
            if (*q == ')') p = q + 1;
        }
        return *p ? -1 : 1;
    }
    if (!strncasecmp(p, "inf", 3)) {
        p += 3;
        if (!strncasecmp(p, "inity", 5)) p += 5;
        if (*p) return -1;
        *out = neg ? -FD_BIG : FD_BIG;
        return 0;
    }
    /* mantissa digits into M (up to 36 significant), the scale in E */
    fd_i128 m = 0;
    long e = 0;                     /* decimal (or binary for hex) exponent */
    int nd = 0, any = 0, hex = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        const char *q = p + 2;
        int hd = 0;
        for (const char *r = q; ; r++) {
            if (isxdigit((unsigned char)*r)) { hd = 1; continue; }
            if (*r == '.') continue;
            break;
        }
        if (hd || (q[0] == '.' && isxdigit((unsigned char)q[1]))) { hex = 1; p = q; }
    }
    int base = hex ? 16 : 10;
    int point = 0;
    for (;; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (hex && isxdigit((unsigned char)*p)) d = (tolower((unsigned char)*p) - 'a') + 10;
        else if (*p == '.' && !point) { point = 1; continue; }
        else break;
        any = 1;
        if (m == 0 && d == 0) { if (point) e -= hex ? 4 : 1; continue; }
        if (nd < 30) {
            m = m * base + d;
            nd++;
            if (point) e -= hex ? 4 : 1;
        } else if (!point) e += hex ? 4 : 1;
    }
    if (!any) return -1;
    if ((hex && (*p == 'p' || *p == 'P')) || (!hex && (*p == 'e' || *p == 'E'))) {
        const char *q = p + 1;
        int eneg = 0;
        if (*q == '+' || *q == '-') { eneg = *q == '-'; q++; }
        if (*q >= '0' && *q <= '9') {
            long x = 0;
            for (; *q >= '0' && *q <= '9'; q++) if (x < 100000) x = x * 10 + (*q - '0');
            e += eneg ? -x : x;
            p = q;
        }
    }
    if (*p) return -1;
    /* a double's range: past 1.8e308 strtod says ERANGE (measured: an
     * underflow to 0 is accepted) */
    if (m != 0) {
        long mag;                   /* the decimal (or binary) magnitude */
        int digits = 0;
        for (fd_i128 t = m; t; t /= base) digits++;
        mag = e + digits;
        if (!hex && mag > 309) return -1;
        if (hex && mag > 1025) return -1;
    }
    /* value * unit * 1e9 */
    fd_i128 v = m * unit;
    long sc = e;
    if (!hex) {
        sc += 9;
        while (sc > 0 && v != 0) {
            if (v > FD_BIG / 10) { v = FD_BIG; sc = 0; break; }
            v *= 10;
            sc--;
        }
        while (sc < 0 && v != 0) { v /= 10; sc++; }
    } else {
        v *= FD_NS;
        while (sc > 0 && v != 0) {
            if (v > FD_BIG / 2) { v = FD_BIG; sc = 0; break; }
            v *= 2;
            sc--;
        }
        while (sc < 0 && v != 0) { v /= 2; sc++; }
    }
    if (v > FD_BIG) v = FD_BIG;
    *out = neg ? -v : v;
    return 0;
}

/* ---- shell patterns ------------------------------------------------------
 *
 * -name -path -lname and their i-forms match as glibc's fnmatch with no
 * flags (FNM_CASEFOLD for the i-forms), in the C locale, measured shape by
 * shape: '*' and '?' match anything, '/' and a leading '.' included; a
 * backslash quotes the next character, and one at the end matches nothing.
 * A bracket [...] takes ! or ^ to negate, ] first as itself, a-z ranges (an
 * empty one matches nothing), [:class:], [=c=] and [.c.] (one character;
 * [.c.] may end or start a range). A bracket that never closes is a plain
 * '[' -- except that a range missing its end, a backslash at the very
 * end, an unknown class or a "[." that never closes make the whole pattern
 * match nothing. Case folding folds plain characters and ranges, not
 * classes or [=c=]. */

enum { FD_FNM_MATCH, FD_FNM_NOMATCH, FD_FNM_LITERAL };

static int fd_fold(int c, int fold) {
    return fold && c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int fd_class(const char *name, size_t len, int c) {
#define FD_CL(n, t) if (len == sizeof(n) - 1 && !memcmp(name, n, len)) return (t) ? 1 : 0;
    FD_CL("alpha", isalpha(c))
    FD_CL("digit", isdigit(c))
    FD_CL("alnum", isalnum(c))
    FD_CL("upper", isupper(c))
    FD_CL("lower", islower(c))
    FD_CL("space", isspace(c))
    FD_CL("blank", c == ' ' || c == '\t')
    FD_CL("punct", ispunct(c))
    FD_CL("xdigit", isxdigit(c))
    FD_CL("print", isprint(c))
    FD_CL("graph", isgraph(c))
    FD_CL("cntrl", iscntrl(c))
#undef FD_CL
    return -1;
}

/* The bracket at *PP (just past '[') against C. Sets *PP past it. */
static int fd_bracket(const char **pp, int c, int fold) {
    const char *p = *pp;
    int neg = 0, hit = 0, first = 1;
    int fc = fd_fold(c, fold);
    if (*p == '!' || *p == '^') { neg = 1; p++; }
    for (;;) {
        int lo = (unsigned char)*p++;
        int sym = 0;                /* LO came from a [.c.] */
        if (lo == 0) return FD_FNM_LITERAL;
        if (lo == ']' && !first) break;
        first = 0;
        if (lo == '\\') {
            lo = (unsigned char)*p++;
            if (!lo) return FD_FNM_NOMATCH;
        } else if (lo == '[' && (*p == ':' || *p == '=' || *p == '.')) {
            char kind = *p;
            const char *name = p + 1, *q = name;
            while (*q && !(q[0] == kind && q[1] == ']')) q++;
            if (!*q) {
                if (kind == '.') return FD_FNM_NOMATCH;
                goto plain;          /* "[:" with no ":]": a plain '[' */
            }
            size_t len = (size_t)(q - name);
            p = q + 2;
            if (kind == ':') {
                int r = fd_class(name, len, c);
                if (r < 0) return FD_FNM_NOMATCH;
                if (r) hit = 1;
                continue;
            }
            if (len != 1) return FD_FNM_NOMATCH;
            if (kind == '=') {
                if ((unsigned char)name[0] == c) hit = 1;
                continue;
            }
            lo = (unsigned char)name[0];
            sym = 1;
            /* measured: "[[.a.]-]" matches '-' and not 'a' */
            if (p[0] == '-' && p[1] == ']') continue;
        }
    plain:
        if (p[0] == '-' && p[1] != ']') {
            int hi = (unsigned char)p[1];
            p += 2;
            if (!hi) return FD_FNM_NOMATCH;
            if (hi == '\\') {
                hi = (unsigned char)*p++;
                if (!hi) return FD_FNM_NOMATCH;
            } else if (hi == '[' && *p == '.') {
                const char *name = p + 1, *q = name;
                while (*q && !(q[0] == '.' && q[1] == ']')) q++;
                if (!*q || q - name != 1) return FD_FNM_NOMATCH;
                hi = (unsigned char)name[0];
                p = q + 2;
            } else if (hi == '[' && (*p == ':' || *p == '=')) {
                return FD_FNM_NOMATCH;
            }
            if (fd_fold(lo, fold) <= fc && fc <= fd_fold(hi, fold)) hit = 1;
            continue;
        }
        /* a [.c.] is not folded */
        if (sym ? lo == c : fd_fold(lo, fold) == fc) hit = 1;
    }
    *pp = p;
    return hit != neg ? FD_FNM_MATCH : FD_FNM_NOMATCH;
}

static int fd_fnm(const char *p, const char *s, int fold) {
    for (;;) {
        int c = (unsigned char)*p++;
        switch (c) {
        case 0:
            return *s ? FD_FNM_NOMATCH : FD_FNM_MATCH;
        case '?':
            if (!*s) return FD_FNM_NOMATCH;
            s++;
            break;
        case '*':
            while (*p == '*') p++;
            if (!*p) return FD_FNM_MATCH;
            for (;; s++) {
                int r = fd_fnm(p, s, fold);
                if (r == FD_FNM_MATCH) return r;
                if (!*s) return FD_FNM_NOMATCH;
            }
        case '[': {
            if (!*s) return FD_FNM_NOMATCH;
            const char *q = p;
            int r = fd_bracket(&q, (unsigned char)*s, fold);
            if (r == FD_FNM_LITERAL) {
                if (*s != '[') return FD_FNM_NOMATCH;
                s++;
                break;
            }
            if (r != FD_FNM_MATCH) return FD_FNM_NOMATCH;
            p = q;
            s++;
            break;
        }
        case '\\':
            c = (unsigned char)*p++;
            if (!c) return FD_FNM_NOMATCH;
            /* fall through */
        default:
            if (fd_fold(c, fold) != fd_fold((unsigned char)*s, fold) || !*s)
                return FD_FNM_NOMATCH;
            s++;
            break;
        }
    }
}

static int fd_fnmatch(const char *pat, const char *s, int fold) {
    return fd_fnm(pat, s, fold) == FD_FNM_MATCH;
}

/* ---- regular expressions ---------------------------------------------------
 *
 * -regex and -iregex match the WHOLE path against a GNU regular expression
 * in one of thirteen dialects (-regextype). This is our own engine (the
 * tobyOS libc has POSIX's BRE and ERE only, and none of these dialects is
 * exactly either): a parser driven by the dialect's syntax switches, each
 * measured against GNU find with a battery of patterns (what is an
 * operator, what is literal, which malformed shapes are errors and which
 * words the error has), and a backtracking matcher that asks whether some
 * match spans the whole string -- for an anchored whole-string match that
 * is the same answer as GNU's longest match.
 *
 *   dialect          groups alt  + ?  {m,n}  [:cl:] \ in []  \1  \w\<..
 *   emacs            \( \)  \|   + ?  none   no     literal  yes yes  . not \n
 *   findutils-def.   like emacs, but . matches a newline
 *   gnu-awk          ( )    |    + ?  {} *   yes    escapes  yes yes
 *   awk              ( )    |    + ?  none   yes    escapes  no  no
 *   posix-awk        ( )    |    + ?  {} *   yes    escapes  yes no   ops at start: error
 *   grep             \( \)  \|  \+ \? \{\}   yes    literal  yes yes  newline = |
 *   posix-basic, sed, ed  as grep without newline-|, "a**" an error
 *   posix-minimal-basic \( \)  none none \{\} yes   literal  yes yes
 *   egrep, posix-egrep ( ) | + ?  {} *   yes    literal  yes yes  newline = |, ops at start: none
 *   posix-extended   ( )    |    + ?  {}     yes    literal  yes yes  ops at start: error
 *     (* a malformed interval is literal text)
 *
 * In the BRE-like dialects ^ and $ anchor only at the ends (of the whole,
 * a group or an alternative) and * there is literal; in the others they
 * always anchor. Errors are glibc's regerror words. */

enum {
    RS_BK_PARENS = 1 << 0,      /* \( \) group; else ( ) */
    RS_BK_VBAR = 1 << 1,        /* \| alternates; else | */
    RS_NO_VBAR = 1 << 2,        /* no alternation at all */
    RS_BK_PLUS_QM = 1 << 3,     /* \+ \? ; else + ? */
    RS_NO_PLUS_QM = 1 << 4,     /* neither */
    RS_INTERVALS = 1 << 5,
    RS_BK_BRACES = 1 << 6,      /* \{ \} ; else { } */
    RS_INV_INTERVAL_ORD = 1 << 7, /* a malformed interval is literal */
    RS_CLASSES = 1 << 8,        /* [:alpha:] */
    RS_BS_IN_LISTS = 1 << 9,    /* \ quotes inside [] */
    RS_NO_BK_REFS = 1 << 10,
    RS_NO_GNU_OPS = 1 << 11,
    RS_DOT_NEWLINE = 1 << 12,
    RS_INDEP_ANCHORS = 1 << 13,
    RS_INDEP_OPS = 1 << 14,     /* * + ? { special at the start too */
    RS_INVALID_OPS = 1 << 15,   /* ... and an error there */
    RS_INVALID_DUP = 1 << 16,   /* a repetition of a repetition is an error */
    RS_NO_EMPTY_RANGES = 1 << 17,
    RS_NEWLINE_ALT = 1 << 18,
};

#define RS_EMACS (RS_BK_PARENS | RS_BK_VBAR)
#define RS_BRE (RS_BK_PARENS | RS_BK_VBAR | RS_BK_PLUS_QM | RS_INTERVALS | RS_BK_BRACES | \
                RS_CLASSES | RS_DOT_NEWLINE | RS_NO_EMPTY_RANGES)
#define RS_ERE (RS_INTERVALS | RS_CLASSES | RS_DOT_NEWLINE | RS_INDEP_ANCHORS | \
                RS_INDEP_OPS | RS_NO_EMPTY_RANGES)

static const struct fd_retype { const char *name; int syntax; } fd_retypes[] = {
    { "findutils-default", RS_EMACS | RS_DOT_NEWLINE },
    { "ed", RS_BRE | RS_INVALID_DUP },
    { "emacs", RS_EMACS },
    { "gnu-awk", RS_INTERVALS | RS_INV_INTERVAL_ORD | RS_CLASSES | RS_BS_IN_LISTS |
                 RS_DOT_NEWLINE | RS_INDEP_ANCHORS | RS_NO_EMPTY_RANGES },
    { "grep", RS_BRE | RS_NEWLINE_ALT },
    { "posix-awk", RS_ERE | RS_INV_INTERVAL_ORD | RS_BS_IN_LISTS | RS_NO_GNU_OPS | RS_INVALID_OPS },
    { "awk", RS_CLASSES | RS_BS_IN_LISTS | RS_NO_BK_REFS | RS_NO_GNU_OPS | RS_DOT_NEWLINE |
             RS_INDEP_ANCHORS | RS_NO_EMPTY_RANGES },
    { "posix-basic", RS_BRE | RS_INVALID_DUP },
    { "posix-egrep", RS_ERE | RS_INV_INTERVAL_ORD | RS_NEWLINE_ALT },
    { "egrep", RS_ERE | RS_INV_INTERVAL_ORD | RS_NEWLINE_ALT },
    { "posix-extended", RS_ERE | RS_INVALID_OPS },
    { "posix-minimal-basic", RS_BK_PARENS | RS_NO_VBAR | RS_NO_PLUS_QM | RS_INTERVALS |
                             RS_BK_BRACES | RS_CLASSES | RS_DOT_NEWLINE | RS_NO_EMPTY_RANGES },
    { "sed", RS_BRE | RS_INVALID_DUP },
    { 0, 0 }
};

enum { RN_CHAR, RN_ANY, RN_SET, RN_GROUP, RN_CAT, RN_ALT, RN_REP, RN_BOL, RN_EOL,
       RN_BREF, RN_WORDB, RN_NWORDB, RN_WBEG, RN_WEND, RN_BUFBEG, RN_BUFEND };

struct fd_rn {
    int t;
    int c;                      /* RN_CHAR; RN_GROUP / RN_BREF: the group number */
    int min, max;               /* RN_REP; max -1: no limit */
    unsigned char set[32];      /* RN_SET */
    struct fd_rn **kid;         /* RN_CAT RN_ALT; RN_REP and RN_GROUP have one */
    int nkid;
};

struct fd_re {
    struct fd_rn *root;
    int ngroups;
    int nodot_nl;               /* . does not match a newline */
};

struct fd_rp {                  /* the parser */
    const char *p, *end;
    int syn, icase;
    int ngroups;                /* opened */
    int closed[64];             /* closed[i]: group i+1 is complete */
    int depth;
    const char *err;
};

#define RE_EBADPAT "Invalid regular expression"
#define RE_ECOLLATE "Invalid collation character"
#define RE_ECTYPE "Invalid character class name"
#define RE_EESCAPE "Trailing backslash"
#define RE_ESUBREG "Invalid back reference"
#define RE_EBRACK "Unmatched [, [^, [:, [., or [="
#define RE_EPAREN "Unmatched ( or \\("
#define RE_EBRACE "Unmatched \\{"
#define RE_EBADBR "Invalid content of \\{\\}"
#define RE_ERANGE "Invalid range end"
#define RE_BADRPT "Invalid preceding regular expression"
#define RE_ESIZE "Regular expression too big"
#define RE_ERPAREN "Unmatched ) or \\)"

static struct fd_rn *fd_rn_new(int t) {
    struct fd_rn *n = (struct fd_rn *)fd_alloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->t = t;
    return n;
}

static void fd_rn_add(struct fd_rn *n, struct fd_rn *k) {
    n->kid = (struct fd_rn **)fd_realloc(n->kid, (size_t)(n->nkid + 1) * sizeof *n->kid);
    n->kid[n->nkid++] = k;
}

static void fd_set_add(struct fd_rn *n, int c, int icase) {
    n->set[c >> 3] |= (unsigned char)(1 << (c & 7));
    if (icase && isalpha(c)) {
        int o = isupper(c) ? tolower(c) : toupper(c);
        n->set[o >> 3] |= (unsigned char)(1 << (o & 7));
    }
}

/* is the parser at the alternation operator? */
static int fd_rp_at_alt(struct fd_rp *r) {
    const char *p = r->p;
    if (p >= r->end) return 0;
    if ((r->syn & RS_NEWLINE_ALT) && *p == '\n') return 1;
    if (r->syn & RS_NO_VBAR) return 0;
    if (r->syn & RS_BK_VBAR) return p + 1 < r->end && p[0] == '\\' && p[1] == '|';
    return *p == '|';
}

static int fd_rp_alt_len(struct fd_rp *r) {
    return (r->syn & RS_NEWLINE_ALT) && *r->p == '\n' ? 1 : (r->syn & RS_BK_VBAR) ? 2 : 1;
}

static int fd_rp_at_close(struct fd_rp *r) {
    const char *p = r->p;
    if (p >= r->end) return 0;
    if (r->syn & RS_BK_PARENS) return p + 1 < r->end && p[0] == '\\' && p[1] == ')';
    return *p == ')';
}

/* a bracket expression; R->p just past '[' */
static struct fd_rn *fd_rp_bracket(struct fd_rp *r) {
    struct fd_rn *n = fd_rn_new(RN_SET);
    int neg = 0;
    if (r->p < r->end && *r->p == '^') { neg = 1; r->p++; }
    if (r->p >= r->end) { r->err = RE_EBADPAT; return 0; }
    int first = 1;
    for (;;) {
        if (r->p >= r->end) { r->err = RE_EBRACK; return 0; }
        int c = (unsigned char)*r->p;
        if (c == ']' && !first) { r->p++; break; }
        first = 0;
        int lo, lo_kind = 0;        /* 0 char, 1 collating symbol, 2 class or equivalence */
        if (c == '\\' && (r->syn & RS_BS_IN_LISTS)) {
            r->p++;
            if (r->p >= r->end) { r->err = RE_EBRACK; return 0; }
            lo = (unsigned char)*r->p++;
        } else if (c == '[' && r->p + 1 < r->end &&
                   (r->p[1] == '.' || r->p[1] == '=' || (r->p[1] == ':' && (r->syn & RS_CLASSES)))) {
            char kind = r->p[1];
            const char *name = r->p + 2, *q = name;
            while (q + 1 < r->end && !(q[0] == kind && q[1] == ']')) q++;
            if (q + 1 >= r->end) { r->err = RE_EBRACK; return 0; }
            size_t len = (size_t)(q - name);
            r->p = q + 2;
            if (kind == ':') {
                char nm[16];
                if (len >= sizeof nm) { r->err = RE_ECTYPE; return 0; }
                memcpy(nm, name, len);
                nm[len] = 0;
                int any = 0;
                for (int ch = 0; ch < 256; ch++) {
                    int k = fd_class(nm, len, ch);
                    if (k < 0) { r->err = RE_ECTYPE; return 0; }
                    if (k) { fd_set_add(n, ch, r->icase); any = 1; }
                }
                (void)any;
                lo_kind = 2;
                lo = -1;
            } else {
                if (len != 1) { r->err = RE_ECOLLATE; return 0; }
                lo = (unsigned char)name[0];
                if (kind == '=') { fd_set_add(n, lo, r->icase); lo_kind = 2; }
                else lo_kind = 1;
            }
        } else {
            r->p++;
            lo = c;
        }
        /* a range? */
        if (r->p + 1 < r->end && r->p[0] == '-' && r->p[1] != ']') {
            if (lo_kind == 2) { r->err = RE_ERANGE; return 0; }
            r->p++;
            int hi;
            int hc = (unsigned char)*r->p;
            if (hc == '\\' && (r->syn & RS_BS_IN_LISTS)) {
                r->p++;
                if (r->p >= r->end) { r->err = RE_EBRACK; return 0; }
                hi = (unsigned char)*r->p++;
            } else if (hc == '[' && r->p + 1 < r->end &&
                       (r->p[1] == '.' || r->p[1] == '=' || (r->p[1] == ':' && (r->syn & RS_CLASSES)))) {
                char kind = r->p[1];
                const char *name = r->p + 2, *q = name;
                while (q + 1 < r->end && !(q[0] == kind && q[1] == ']')) q++;
                if (q + 1 >= r->end) { r->err = RE_EBRACK; return 0; }
                if (kind != '.') { r->err = RE_ERANGE; return 0; }
                if (q - name != 1) { r->err = RE_ECOLLATE; return 0; }
                hi = (unsigned char)name[0];
                r->p = q + 2;
            } else {
                hi = hc;
                r->p++;
            }
            if (lo > hi) {
                if (r->syn & RS_NO_EMPTY_RANGES) { r->err = RE_ERANGE; return 0; }
            } else {
                for (int ch = lo; ch <= hi; ch++) fd_set_add(n, ch, r->icase);
            }
            /* "[a-c-e]": a second '-' after a range */
            if (r->p + 1 < r->end && r->p[0] == '-' && r->p[1] != ']') { r->err = RE_ERANGE; return 0; }
            continue;
        }
        if (lo_kind != 2) fd_set_add(n, lo, r->icase);
    }
    if (neg) for (int i = 0; i < 32; i++) n->set[i] = (unsigned char)~n->set[i];
    return n;
}

/* An interval after an atom: R->p at '{' (or the '\\' of "\{"). Returns 1
 * and sets MIN MAX, 0 when it is literal text, -1 on an error. */
static int fd_rp_interval(struct fd_rp *r, int *min, int *max) {
    const char *save = r->p;
    int bk = (r->syn & RS_BK_BRACES) != 0;
    r->p += bk ? 2 : 1;
    int lo = -1, hi = -1, comma = 0;
    long v = -1;
    for (;;) {
        if (r->p >= r->end) {
            if (r->syn & RS_INV_INTERVAL_ORD) { r->p = save; return 0; }
            r->err = RE_EBRACE;
            return -1;
        }
        int c = (unsigned char)*r->p;
        if (c >= '0' && c <= '9') {
            if (v < 0) v = 0;
            if (v <= 0x7fff) v = v * 10 + (c - '0');
            r->p++;
            continue;
        }
        if (c == ',' && !comma) {
            comma = 1;
            lo = v < 0 ? 0 : (int)(v > 0x8000 ? 0x8000 : v);
            v = -1;
            r->p++;
            continue;
        }
        int close = bk ? (c == '\\' && r->p + 1 < r->end && r->p[1] == '}') : c == '}';
        if (!close) {
            if (r->syn & RS_INV_INTERVAL_ORD) { r->p = save; return 0; }
            r->err = RE_EBADBR;
            return -1;
        }
        r->p += bk ? 2 : 1;
        break;
    }
    if (!comma) {
        if (v < 0) { r->err = RE_EBADBR; return -1; }     /* "{}" */
        lo = hi = (int)(v > 0x8000 ? 0x8000 : v);
    } else {
        hi = v < 0 ? -1 : (int)(v > 0x8000 ? 0x8000 : v);
    }
    if (hi >= 0 && lo > hi) { r->err = RE_EBADBR; return -1; }
    if (lo > 0x7fff || hi > 0x7fff) { r->err = RE_ESIZE; return -1; }
    *min = lo;
    *max = hi;
    return 1;
}

/* the repetition operator at R->p, if any: '*' '+' '?' or 'I' (an
 * interval); 0 if none. Sets *LEN to its length. */
static int fd_rp_dupop(struct fd_rp *r, int *len) {
    const char *p = r->p;
    if (p >= r->end) return 0;
    if (*p == '*') { *len = 1; return '*'; }
    if (!(r->syn & RS_NO_PLUS_QM)) {
        if (r->syn & RS_BK_PLUS_QM) {
            if (p + 1 < r->end && p[0] == '\\' && (p[1] == '+' || p[1] == '?')) { *len = 2; return p[1]; }
        } else if (*p == '+' || *p == '?') { *len = 1; return *p; }
    }
    if (r->syn & RS_INTERVALS) {
        if (r->syn & RS_BK_BRACES) {
            if (p + 1 < r->end && p[0] == '\\' && p[1] == '{') { *len = 2; return 'I'; }
        } else if (*p == '{') { *len = 1; return 'I'; }
    }
    return 0;
}

static struct fd_rn *fd_rp_alt(struct fd_rp *r);

/* one branch: atoms and their repetitions, up to | ) or the end */
static struct fd_rn *fd_rp_branch(struct fd_rp *r) {
    struct fd_rn *cat = fd_rn_new(RN_CAT);
    int start = 1;              /* at the start of the pattern, a group or an alternative */
    for (;;) {
        if (r->p >= r->end || fd_rp_at_alt(r)) break;
        if (fd_rp_at_close(r)) {
            if (r->depth > 0) break;
            if (r->syn & RS_BK_PARENS) { r->err = RE_ERPAREN; return 0; }
            /* an unmatched ')' is itself in the ERE dialects */
            struct fd_rn *n = fd_rn_new(RN_CHAR);
            n->c = ')';
            r->p++;
            fd_rn_add(cat, n);
            start = 0;
            goto postfix;
        }
        {
            int dl, op = fd_rp_dupop(r, &dl);
            if (op && start) {
                /* a repetition with nothing before it */
                if (r->syn & RS_INDEP_OPS) {
                    if (r->syn & RS_INVALID_OPS) { r->err = RE_BADRPT; return 0; }
                    r->p += dl;             /* applies to nothing: dropped */
                    continue;
                }
                if (op == 'I' && (r->syn & RS_INVALID_DUP)) { r->err = RE_BADRPT; return 0; }
                /* else the operator is literal text */
                struct fd_rn *n = fd_rn_new(RN_CHAR);
                n->c = (unsigned char)r->p[dl - 1];
                r->p += dl;
                fd_rn_add(cat, n);
                start = 0;
                goto postfix;
            }
        }
        {
            int c = (unsigned char)*r->p;
            struct fd_rn *n = 0;
            if (c == '^' && ((r->syn & RS_INDEP_ANCHORS) || start)) {
                n = fd_rn_new(RN_BOL);
                r->p++;
                fd_rn_add(cat, n);
                start = 1;              /* a repetition after it has nothing: "^*" */
                continue;
            }
            if (c == '$') {
                const char *after = r->p + 1;
                int anchor = (r->syn & RS_INDEP_ANCHORS) != 0;
                if (!anchor) {
                    struct fd_rp t = *r;
                    t.p = after;
                    anchor = after >= r->end || fd_rp_at_alt(&t) || (r->depth > 0 && fd_rp_at_close(&t));
                }
                if (anchor) {
                    n = fd_rn_new(RN_EOL);
                    r->p++;
                    fd_rn_add(cat, n);
                    start = 0;
                    continue;
                }
            }
            if (!(r->syn & RS_BK_PARENS) && c == '(') {
                r->p++;
                goto group;
            }
            if ((r->syn & RS_BK_PARENS) && c == '\\' && r->p + 1 < r->end && r->p[1] == '(') {
                r->p += 2;
            group:;
                int g = ++r->ngroups;
                r->depth++;
                struct fd_rn *inner = fd_rp_alt(r);
                if (!inner) return 0;
                r->depth--;
                if (!fd_rp_at_close(r)) { r->err = RE_EPAREN; return 0; }
                r->p += (r->syn & RS_BK_PARENS) ? 2 : 1;
                if (g <= 64) r->closed[g - 1] = 1;
                n = fd_rn_new(RN_GROUP);
                n->c = g;
                fd_rn_add(n, inner);
            } else if (c == '[') {
                r->p++;
                n = fd_rp_bracket(r);
                if (!n) return 0;
            } else if (c == '.') {
                r->p++;
                n = fd_rn_new(RN_ANY);
            } else if (c == '\\') {
                if (r->p + 1 >= r->end) { r->err = RE_EESCAPE; return 0; }
                int e = (unsigned char)r->p[1];
                r->p += 2;
                int gnu = !(r->syn & RS_NO_GNU_OPS);
                if (e >= '1' && e <= '9' && !(r->syn & RS_NO_BK_REFS)) {
                    int g = e - '0';
                    if (g > r->ngroups || !r->closed[g - 1]) { r->err = RE_ESUBREG; return 0; }
                    n = fd_rn_new(RN_BREF);
                    n->c = g;
                } else if (gnu && (e == 'w' || e == 'W' || e == 's' || e == 'S')) {
                    n = fd_rn_new(RN_SET);
                    for (int ch = 0; ch < 256; ch++) {
                        int in = (e == 'w' || e == 'W') ? (isalnum(ch) || ch == '_') : isspace(ch);
                        if (e == 'W' || e == 'S') in = !in;
                        if (in) n->set[ch >> 3] |= (unsigned char)(1 << (ch & 7));
                    }
                } else if (gnu && (e == '<' || e == '>' || e == 'b' || e == 'B' || e == '`' || e == '\'')) {
                    n = fd_rn_new(e == '<' ? RN_WBEG : e == '>' ? RN_WEND : e == 'b' ? RN_WORDB :
                                  e == 'B' ? RN_NWORDB : e == '`' ? RN_BUFBEG : RN_BUFEND);
                    fd_rn_add(cat, n);
                    start = 0;
                    continue;
                } else {
                    n = fd_rn_new(RN_CHAR);
                    n->c = e;
                }
            } else {
                r->p++;
                n = fd_rn_new(RN_CHAR);
                n->c = c;
            }
            fd_rn_add(cat, n);
            start = 0;
        }
    postfix:
        for (int nrep = 0;; nrep++) {
            int dl, op = fd_rp_dupop(r, &dl);
            if (!op) break;
            int min = 0, max = -1;
            if (op == 'I') {
                int k = fd_rp_interval(r, &min, &max);
                if (k < 0) return 0;
                if (k == 0) break;          /* literal text, read as atoms */
            } else {
                r->p += dl;
                if (op == '+') min = 1;
                if (op == '?') max = 1;
            }
            if (nrep > 0 && (r->syn & RS_INVALID_DUP) && (op == '*' || op == 'I')) { r->err = RE_BADRPT; return 0; }
            struct fd_rn *last = cat->kid[cat->nkid - 1];
            struct fd_rn *rep = fd_rn_new(RN_REP);
            rep->min = min;
            rep->max = max;
            fd_rn_add(rep, last);
            cat->kid[cat->nkid - 1] = rep;
        }
    }
    return cat;
}

static struct fd_rn *fd_rp_alt(struct fd_rp *r) {
    struct fd_rn *first = fd_rp_branch(r);
    if (!first) return 0;
    if (!fd_rp_at_alt(r)) return first;
    struct fd_rn *alt = fd_rn_new(RN_ALT);
    fd_rn_add(alt, first);
    while (fd_rp_at_alt(r)) {
        r->p += fd_rp_alt_len(r);
        struct fd_rn *b = fd_rp_branch(r);
        if (!b) return 0;
        fd_rn_add(alt, b);
    }
    return alt;
}

/* PAT in dialect SYN: 0, or the error's words */
static const char *fd_re_compile(const char *pat, int syn, int icase, struct fd_re *re) {
    struct fd_rp r;
    memset(&r, 0, sizeof r);
    r.p = pat;
    r.end = pat + strlen(pat);
    r.syn = syn;
    r.icase = icase;
    struct fd_rn *root = fd_rp_alt(&r);
    if (!root) return r.err ? r.err : RE_EBADPAT;
    if (r.p < r.end) {
        /* only an unbalanced close of a group stops the top level */
        return (syn & RS_BK_PARENS) ? RE_ERPAREN : RE_EBADPAT;
    }
    re->root = root;
    re->ngroups = r.ngroups;
    re->nodot_nl = !(syn & RS_DOT_NEWLINE);
    return 0;
}

/* ---- matching ---- */

struct fd_rm {                  /* the matcher's state */
    const unsigned char *s;
    int len;
    int icase;
    const struct fd_re *re;
    int gs[65], ge[65];         /* group starts and ends, -1 unset */
    long steps;
};

struct fd_rk {                  /* a continuation: what to match next */
    const struct fd_rn *n;      /* a node, or 0 for "the rest of K" */
    int idx;                    /* RN_CAT: the next child; RN_REP: iterations done */
    int from;                   /* RN_REP: where this iteration began; RN_GROUP: the start */
    const struct fd_rk *next;
};

static int fd_rx(struct fd_rm *m, const struct fd_rn *n, int i, const struct fd_rk *k);

static int fd_rk_run(struct fd_rm *m, int i, const struct fd_rk *k);

static int fd_word(struct fd_rm *m, int i) {
    if (i < 0 || i >= m->len) return 0;
    int c = m->s[i];
    return isalnum(c) || c == '_';
}

static int fd_rx_one(struct fd_rm *m, const struct fd_rn *n, int c) {
    switch (n->t) {
    case RN_CHAR:
        if (m->icase) return tolower(c) == tolower(n->c);
        return c == n->c;
    case RN_ANY:
        return !(c == '\n' && m->re->nodot_nl);
    case RN_SET:
        return (n->set[c >> 3] >> (c & 7)) & 1;
    }
    return 0;
}

static int fd_rep_iter(struct fd_rm *m, const struct fd_rn *n, int count, int i, const struct fd_rk *k) {
    if (++m->steps > 20000000L) return 0;
    const struct fd_rn *kid = n->kid[0];
    if (n->max < 0 || count < n->max) {
        struct fd_rk again = { n, count + 1, i, k };
        if (fd_rx(m, kid, i, &again)) return 1;
    }
    if (count >= n->min) return fd_rk_run(m, i, k);
    return 0;
}

static int fd_rk_run(struct fd_rm *m, int i, const struct fd_rk *k) {
    if (!k) return i == m->len;
    const struct fd_rn *n = k->n;
    if (n->t == RN_CAT) {
        if (k->idx >= n->nkid) return fd_rk_run(m, i, k->next);
        struct fd_rk nk = { n, k->idx + 1, 0, k->next };
        return fd_rx(m, n->kid[k->idx], i, &nk);
    }
    if (n->t == RN_REP) {
        /* one more iteration ended at I; an empty one ends the loop */
        if (i == k->from && k->idx > n->min) return 0;
        if (i == k->from) {
            /* an empty iteration that was needed for MIN: count it and go on */
            if (k->idx >= n->min) return fd_rk_run(m, i, k->next);
        }
        return fd_rep_iter(m, n, k->idx, i, k->next);
    }
    if (n->t == RN_GROUP) {
        int g = n->c;
        int os = m->gs[g], oe = m->ge[g];
        if (g <= 64) { m->gs[g] = k->from; m->ge[g] = i; }
        if (fd_rk_run(m, i, k->next)) return 1;
        if (g <= 64) { m->gs[g] = os; m->ge[g] = oe; }
        return 0;
    }
    return 0;
}

static int fd_rx(struct fd_rm *m, const struct fd_rn *n, int i, const struct fd_rk *k) {
    if (++m->steps > 20000000L) return 0;
    switch (n->t) {
    case RN_CHAR: case RN_ANY: case RN_SET:
        if (i >= m->len || !fd_rx_one(m, n, m->s[i])) return 0;
        return fd_rk_run(m, i + 1, k);
    case RN_CAT:
        if (!n->nkid) return fd_rk_run(m, i, k);
        {
            struct fd_rk nk = { n, 1, 0, k };
            return fd_rx(m, n->kid[0], i, &nk);
        }
    case RN_ALT:
        for (int j = 0; j < n->nkid; j++)
            if (fd_rx(m, n->kid[j], i, k)) return 1;
        return 0;
    case RN_GROUP: {
        struct fd_rk nk = { n, 0, i, k };
        return fd_rx(m, n->kid[0], i, &nk);
    }
    case RN_REP: {
        const struct fd_rn *kid = n->kid[0];
        if (kid->t == RN_CHAR || kid->t == RN_ANY || kid->t == RN_SET) {
            /* one character at a time: greedy, without recursion */
            int most = 0;
            while (i + most < m->len && (n->max < 0 || most < n->max) &&
                   fd_rx_one(m, kid, m->s[i + most])) most++;
            for (int c = most; c >= n->min; c--)
                if (fd_rk_run(m, i + c, k)) return 1;
            return 0;
        }
        return fd_rep_iter(m, n, 0, i, k);
    }
    case RN_BOL: return i == 0 && fd_rk_run(m, i, k);
    case RN_EOL: return i == m->len && fd_rk_run(m, i, k);
    case RN_BUFBEG: return i == 0 && fd_rk_run(m, i, k);
    case RN_BUFEND: return i == m->len && fd_rk_run(m, i, k);
    case RN_WORDB: return fd_word(m, i - 1) != fd_word(m, i) && fd_rk_run(m, i, k);
    case RN_NWORDB: return fd_word(m, i - 1) == fd_word(m, i) && fd_rk_run(m, i, k);
    case RN_WBEG: return !fd_word(m, i - 1) && fd_word(m, i) && fd_rk_run(m, i, k);
    case RN_WEND: return fd_word(m, i - 1) && !fd_word(m, i) && fd_rk_run(m, i, k);
    case RN_BREF: {
        int g = n->c;
        if (g > 64 || m->gs[g] < 0) return 0;
        int l = m->ge[g] - m->gs[g];
        if (i + l > m->len || memcmp(m->s + m->gs[g], m->s + i, (size_t)l)) return 0;
        return fd_rk_run(m, i + l, k);
    }
    }
    return 0;
}

static int fd_re_full(const struct fd_re *re, const char *s, int icase) {
    struct fd_rm m;
    m.s = (const unsigned char *)s;
    m.len = (int)strlen(s);
    m.icase = icase;
    m.re = re;
    m.steps = 0;
    for (int g = 0; g <= 64; g++) m.gs[g] = m.ge[g] = -1;
    return fd_rx(&m, re->root, 0, 0);
}

/* ---- -perm's modes ---------------------------------------------------------
 *
 * GNU's mode syntax, measured with -perm and chmod: octal (at most 07777),
 * or comma-separated clauses, each [ugoa]* then one or more of an operator
 * [-+=] followed by [rwxXst]*, or by one of u g o (copy that class), or --
 * only when the clause names no [ugoa] -- by octal digits ("=644",
 * "+u=644"; "u=644" is invalid). find computes the mode from 0 with a
 * umask of 0, twice: X counts for a directory, and for anything else only
 * once an x bit is set. */
static int fd_mode_compile(const char *s, mode_t *dirmode, mode_t *filemode) {
    if (*s >= '0' && *s <= '7') {
        unsigned long v = 0;
        for (; *s >= '0' && *s <= '7'; s++) {
            v = v * 8 + (unsigned long)(*s - '0');
            if (v > 07777) return -1;
        }
        if (*s) return -1;
        *dirmode = *filemode = (mode_t)v;
        return 0;
    }
    mode_t m[2] = { 0, 0 };         /* [0] a directory's, [1] another file's */
    const char *p = s;
    for (;;) {
        mode_t who = 0;
        int named = 0;
        for (;; p++) {
            if (*p == 'u') who |= 04700;
            else if (*p == 'g') who |= 02070;
            else if (*p == 'o') who |= 01007;
            else if (*p == 'a') who |= 07777;
            else break;
            named = 1;
        }
        if (!named) who = 07777;
        if (*p != '=' && *p != '+' && *p != '-') return -1;
        while (*p == '=' || *p == '+' || *p == '-') {
            char op = *p++;
            for (int k = 0; k < 2; k++) {
                mode_t cur = m[k], val = 0;
                const char *q = p;
                if (*q >= '0' && *q <= '7') {
                    if (named) return -1;
                    unsigned long v = 0;
                    for (; *q >= '0' && *q <= '7'; q++) {
                        v = v * 8 + (unsigned long)(*q - '0');
                        if (v > 07777) return -1;
                    }
                    val = (mode_t)v;
                } else if (*q == 'u' || *q == 'g' || *q == 'o') {
                    mode_t src = *q == 'u' ? (cur >> 6) & 7 : *q == 'g' ? (cur >> 3) & 7 : cur & 7;
                    val = (mode_t)((src << 6) | (src << 3) | src) & who;
                    q++;
                } else {
                    for (;; q++) {
                        if (*q == 'r') val |= 0444;
                        else if (*q == 'w') val |= 0222;
                        else if (*q == 'x') val |= 0111;
                        else if (*q == 'X') { if (k == 0 || (cur & 0111)) val |= 0111; }
                        else if (*q == 's') val |= 06000;
                        else if (*q == 't') val |= 01000;
                        else break;
                    }
                    val &= who;
                }
                if (op == '=') m[k] = (cur & ~who) | val;
                else if (op == '+') m[k] = cur | val;
                else m[k] = cur & ~val;
                if (k == 1) p = q;
            }
        }
        if (*p == ',') { p++; continue; }
        if (*p) return -1;
        break;
    }
    *dirmode = m[0] & 07777;
    *filemode = m[1] & 07777;
    return 0;
}

/* ---- users and groups ------------------------------------------------------
 *
 * From /etc/passwd and /etc/group, as the C library's files service reads
 * them (the tobyOS libc has no getpwuid). */
struct fd_idname { unsigned long id; char *name; struct fd_idname *next; };
static struct fd_idname *fd_unames, *fd_gnames;

/* ID's name in DB, or 0 */
static const char *fd_id_name(const char *db, unsigned long id, struct fd_idname **cache) {
    for (struct fd_idname *c = *cache; c; c = c->next) if (c->id == id) return c->name;
    struct fd_idname *c = (struct fd_idname *)fd_alloc(sizeof *c);
    c->id = id;
    c->name = 0;
    FILE *f = fopen(db, "r");
    if (f) {
        char *line = 0;
        size_t cap = 0;
        while (getline(&line, &cap, f) > 0) {
            char *c1 = strchr(line, ':');
            if (!c1) continue;
            char *c2 = strchr(c1 + 1, ':');
            if (!c2) continue;
            char *e;
            unsigned long v = strtoul(c2 + 1, &e, 10);
            if (e == c2 + 1 || (*e != ':' && *e != '\n' && *e)) continue;
            if (v != id) continue;
            *c1 = 0;
            c->name = fd_strdup(line);
            break;
        }
        free(line);
        fclose(f);
    }
    c->next = *cache;
    *cache = c;
    return c->name;
}

/* NAME's id in DB, or -1 */
static long fd_name_id(const char *db, const char *name) {
    FILE *f = fopen(db, "r");
    if (!f) return -1;
    char *line = 0;
    size_t cap = 0;
    long id = -1;
    size_t nl = strlen(name);
    while (getline(&line, &cap, f) > 0) {
        char *c1 = strchr(line, ':');
        if (!c1 || (size_t)(c1 - line) != nl || strncmp(line, name, nl)) continue;
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) continue;
        char *e;
        unsigned long v = strtoul(c2 + 1, &e, 10);
        if (e == c2 + 1) continue;
        id = (long)v;
        break;
    }
    free(line);
    fclose(f);
    return id;
}

/* ---- file systems ------------------------------------------------------------
 *
 * -fstype and %F: the type of the mount whose device is the file's, from
 * /proc/self/mountinfo ("ID PARENT MAJ:MIN ROOT POINT OPTS ... - TYPE ..."),
 * the last such mount when several share it; "unknown" when none does. */
struct fd_mnt { unsigned maj, min; char *type; };
static struct fd_mnt *fd_mnts;
static int fd_nmnt = -1;

static void fd_mnt_load(void) {
    if (fd_nmnt >= 0) return;
    fd_nmnt = 0;
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return;
    char *line = 0;
    size_t cap = 0;
    while (getline(&line, &cap, f) > 0) {
        unsigned maj, min;
        char *p = line;
        /* the third field */
        for (int k = 0; k < 2; k++) { p = strchr(p, ' '); if (!p) break; p++; }
        if (!p || sscanf(p, "%u:%u", &maj, &min) != 2) continue;
        char *dash = strstr(p, " - ");
        if (!dash) continue;
        char *t = dash + 3, *te = t;
        while (*te && *te != ' ' && *te != '\n') te++;
        fd_mnts = (struct fd_mnt *)fd_realloc(fd_mnts, (size_t)(fd_nmnt + 1) * sizeof *fd_mnts);
        fd_mnts[fd_nmnt].maj = maj;
        fd_mnts[fd_nmnt].min = min;
        fd_mnts[fd_nmnt].type = (char *)fd_alloc((size_t)(te - t) + 1);
        memcpy(fd_mnts[fd_nmnt].type, t, (size_t)(te - t));
        fd_mnts[fd_nmnt].type[te - t] = 0;
        fd_nmnt++;
    }
    free(line);
    fclose(f);
}

static const char *fd_fstype_of(dev_t dev) {
    fd_mnt_load();
    const char *t = 0;
    for (int i = 0; i < fd_nmnt; i++)
        if (fd_mnts[i].maj == major(dev) && fd_mnts[i].min == minor(dev)) t = fd_mnts[i].type;
    return t ? t : "unknown";
}

/* ---- time ------------------------------------------------------------------- */

static fd_i128 fd_now_ns;            /* when find started */
static fd_i128 fd_day_start;         /* GNU's cur_day_start: now - a day, or today's midnight */

static fd_i128 fd_ns(int64_t s, long ns) { return (fd_i128)s * FD_NS + ns; }

#ifdef SU_HOST
#define FD_ATIM(st) fd_ns((st)->st_atim.tv_sec, (st)->st_atim.tv_nsec)
#define FD_MTIM(st) fd_ns((st)->st_mtim.tv_sec, (st)->st_mtim.tv_nsec)
#define FD_CTIM(st) fd_ns((st)->st_ctim.tv_sec, (st)->st_ctim.tv_nsec)
#else
/* the native stat has whole seconds */
#define FD_ATIM(st) fd_ns((int64_t)(st)->st_atime, 0)
#define FD_MTIM(st) fd_ns((int64_t)(st)->st_mtime, 0)
#define FD_CTIM(st) fd_ns((int64_t)(st)->st_ctime, 0)
#endif

static int64_t fd_sec_of(fd_i128 t) {
    int64_t s = (int64_t)(t / FD_NS);
    if (t % FD_NS < 0) s--;
    return s;
}
static long fd_nsec_of(fd_i128 t) {
    long n = (long)(t % FD_NS);
    if (n < 0) n += FD_NS;
    return n;
}

/* T as ctime(3) says it, in the local zone, with no newline */
static void fd_ctime(struct fd_buf *b, int64_t t) {
    char *s = tx_strftime("%a %b %e %H:%M:%S %Y", t, 0, 0);
    fd_bputs(b, s);
    free(s);
}

/* ---- the predicates --------------------------------------------------------- */

enum {
    PK_OPEN, PK_CLOSE, PK_NOT, PK_AND, PK_OR, PK_COMMA,
    PK_TRUE, PK_FALSE,              /* -true -false, and every option */
    PK_NAME, PK_PATH, PK_REGEX, PK_LNAME,
    PK_TYPE, PK_XTYPE, PK_PERM, PK_SIZE, PK_EMPTY,
    PK_TIME, PK_USED, PK_NEWER,
    PK_UID, PK_GID, PK_NOUSER, PK_NOGROUP, PK_LINKS, PK_INUM, PK_SAMEFILE,
    PK_ACCESS, PK_FSTYPE,
    PK_PRINT, PK_PRINTF, PK_LS, PK_EXEC, PK_DELETE, PK_PRUNE, PK_QUIT
};

enum { PT_PRIMARY, PT_UNI, PT_BI, PT_OPEN, PT_CLOSE };
enum { PR_NO, PR_COMMA, PR_OR, PR_AND, PR_NEGATE };

/* what a test needs to know of a file */
enum { NEED_NOTHING, NEED_TYPE, NEED_STAT };

struct fd_out {                 /* an output stream: stdout, or a -fprint file */
    FILE *f;
    const char *name;           /* for messages */
    dev_t dev;
    ino_t ino;
    int failed;
    struct fd_out *next;
};

/* a piece of a -printf format */
struct fd_seg {
    int kind;                   /* 0 text; else the directive's letter */
    int k;                      /* %A %B %C %T: the time conversion */
    char *text;                 /* the text, or the directive as written */
    size_t tlen;
    char flags[8];              /* "-+ #0" seen */
    int width, prec;            /* -1: none */
    int stop;                   /* \c: stop here */
};

struct fd_exec {
    char **argv;
    int argc;
    int dir;                    /* -execdir -okdir */
    int ok;                     /* -ok -okdir */
    int plus;                   /* {} + */
    /* the + batch */
    char **batch;
    int nb, cap;
    size_t bytes;
    char *bdir;                 /* -execdir +: the batch's directory */
};

struct fd_pred {
    int kind, ptype, prec;
    const char *name;           /* "-name", for messages and -D */
    char *dname;                /* what -D tree shows: "-name f" */
    int side_effects;
    int action;                 /* suppresses the default -print */
    int need;                   /* NEED_* */
    int cheap;                  /* reads only the name: -O1 runs it first */
    int lost_side;              /* -O1 rebuilt this chain's end: its side effects no longer hold others back */
    struct fd_pred *l, *r;      /* the tree */
    struct fd_pred *next;       /* the token list */
    /* arguments */
    const char *s;
    int fold;
    int cmp;
    uintmax_t u;
    fd_i128 t;                  /* a time, or N in nanoseconds of the unit */
    int tx, ty;                 /* time fields: 'a' 'c' 'm' (-newerXY), unit */
    int days;                   /* -Xtime (1) or -Xmin (0) */
    fd_i128 ref;                /* the time window's reference */
    mode_t mode_d, mode_f;
    int perm_how;               /* 0 exact, '-' all, '/' any */
    uintmax_t unit;             /* -size */
    unsigned types;             /* -type -xtype: bits */
    dev_t dev;
    ino_t ino;
    int amode;                  /* -readable ... */
    int follow;                 /* -follow was in effect when it was read */
    struct fd_re re;
    struct fd_out *out;
    int print0;
    struct fd_seg *segs;
    int nseg;
    struct fd_exec *ex;
    unsigned long evals, succ;  /* -D rates */
};

/* the whole command's state */
static struct {
    int follow;                 /* 0 -P, 1 -H, 2 -L (or -follow) */
    int debug;
    int olevel;
    long maxdepth, mindepth;
    int depth, depth_explicit;
    int xdev, ignore_race;
    int warn, daystart, regex_syn;
    int posixly;
    int has_prune, has_delete, has_ok;
    const char *files0;         /* -files0-from FILE */
    char **paths;               /* the starting points on the command line */
    int npaths;
    int tty_out;                /* stdout is a terminal: quote names */
    const char *first_nonopt;   /* the first argument that was not an option */
    struct fd_pred *head, *tail;
    struct fd_pred *tree;
    struct fd_out *outs;
    struct fd_out *stdout_o, *stderr_o;
    struct fd_out **used;       /* the streams actions write to, in first-use order */
    int nused;
    struct fd_pred **plus;      /* the -exec ... + predicates, for the end */
    int nplus;
} fdg;

enum { DB_EXEC = 1, DB_OPT = 2, DB_RATES = 4, DB_SEARCH = 8, DB_STAT = 16, DB_TIME = 32, DB_TREE = 64 };

static char **fd_av;
static const char *fd_base = "find";      /* argv[0]'s last component */
static int fd_ac, fd_ai;

static struct fd_pred *fd_add(int kind, int ptype, const char *name) {
    struct fd_pred *p = (struct fd_pred *)fd_alloc(sizeof *p);
    memset(p, 0, sizeof *p);
    p->kind = kind;
    p->ptype = ptype;
    p->name = name;
    p->dname = (char *)name;
    p->prec = ptype == PT_PRIMARY ? PR_NO : kind == PK_NOT ? PR_NEGATE :
              kind == PK_AND ? PR_AND : kind == PK_OR ? PR_OR : kind == PK_COMMA ? PR_COMMA : PR_NO;
    p->cmp = FD_EQ;
    /* an implicit -a before a primary, ( or ! that follows a primary or ) */
    if ((ptype == PT_PRIMARY || ptype == PT_OPEN || ptype == PT_UNI) && fdg.tail &&
        (fdg.tail->ptype == PT_PRIMARY || fdg.tail->ptype == PT_CLOSE)) {
        struct fd_pred *a = (struct fd_pred *)fd_alloc(sizeof *a);
        memset(a, 0, sizeof *a);
        a->kind = PK_AND;
        a->ptype = PT_BI;
        a->prec = PR_AND;
        a->name = a->dname = (char *)"-a";
        fdg.tail->next = a;
        fdg.tail = a;
    }
    if (fdg.tail) fdg.tail->next = p;
    else fdg.head = p;
    fdg.tail = p;
    return p;
}

static void fd_dname(struct fd_pred *p, const char *arg) {
    size_t n = strlen(p->name) + strlen(arg) + 2;
    p->dname = (char *)fd_alloc(n);
    snprintf(p->dname, n, "%s %s", p->name, arg);
}

static const char *fd_arg(const char *pred) {
    if (fd_ai >= fd_ac) su_error(1, 0, "missing argument to `%s'", pred);
    return fd_av[fd_ai++];
}

/* the global options' warning, when warnings are on */
static void fd_global_opt(const char *name) {
    if (fdg.warn && fdg.first_nonopt)
        su_error(0, 0, "warning: you have specified the global option %s after the argument %s, "
                 "but global options are not positional, i.e., %s affects tests specified before "
                 "it as well as those specified after it.  Please specify global options before "
                 "other arguments.", name, fdg.first_nonopt, name);
}

/* [+-]N for -links -inum -uid -gid */
static void fd_num_arg(struct fd_pred *p, const char *a) {
    const char *s = a;
    if (*s == '+') { p->cmp = FD_GT; s++; }
    else if (*s == '-') { p->cmp = FD_LT; s++; }
    if (fd_umax(s, &p->u) != 0)
        su_error(1, 0, "non-numeric argument to %s: %s", p->name, fd_q(a));
}

/* the reference file of -newer and the rest: stat'ed or lstat'ed as the
 * link options say (with -H or -L, or after -follow, a link is followed --
 * a dangling one is taken as itself) */
static void fd_ref_stat(const char *f, struct stat *st) {
    int r;
    if (fdg.debug & DB_STAT) fprintf(stderr, "debug_stat (%s)\n", f);
    if (fdg.follow) {
        r = stat(f, st);
        if (r != 0 && errno == ENOENT) {
            if (fdg.debug & DB_STAT)
                fprintf(stderr, "fallback_stat(): stat(%s) failed; falling back on lstat()\n", f);
            r = lstat(f, st);
        }
    } else
        r = lstat(f, st);
    if (r != 0) su_error(1, errno, "%s", fd_q(f));
}

static fd_i128 fd_stat_time(const struct stat *st, int which) {
    return which == 'a' ? FD_ATIM(st) : which == 'c' ? FD_CTIM(st) : FD_MTIM(st);
}

/* the time windows, as measured (see fd_eval): N whole minutes back from
 * now (or from the end of today, with -daystart), N whole days back from a
 * day ago (or from today's midnight) */
static void fd_time_window(struct fd_pred *p) {
    if (p->days) p->ref = fd_day_start - p->t;
    else p->ref = fd_day_start + (fd_i128)FD_DAY * FD_NS - p->t;
}

static void fd_time_arg(struct fd_pred *p, const char *a, int days) {
    const char *s = a;
    if (*s == '+') { p->cmp = FD_GT; s++; }
    else if (*s == '-') { p->cmp = FD_LT; s++; }
    fd_i128 v = 0;
    int r = *s ? fd_strtod_ns(s, days ? FD_DAY : 60, &v) : -1;
    if (r == 1) su_error(1, 0, "invalid not-a-number argument: `%s'", s);
    if (r != 0) {
        if (p->kind == PK_USED) su_error(1, 0, "Invalid argument %s to -used", a);
        su_error(1, 0, "invalid argument `%s' to `%s'", a, p->name);
    }
    p->t = v;
}

static unsigned fd_type_bit(int c) {
    switch (c) {
    case 'b': return 1;
    case 'c': return 2;
    case 'd': return 4;
    case 'p': return 8;
    case 'f': return 16;
    case 'l': return 32;
    case 's': return 64;
    case 'D': return 128;
    }
    return 0;
}

static void fd_type_arg(struct fd_pred *p, const char *a) {
    if (!*a) su_error(1, 0, "Arguments to %s should contain at least one letter", p->name);
    for (const char *q = a; *q; q++) {
        unsigned bit = fd_type_bit((unsigned char)*q);
        if (!bit) su_error(1, 0, "Unknown argument to %s: %c", p->name, *q);
        if (*q == 'D')
            su_error(1, 0, "%s D is not supported because Solaris doors are not supported on the "
                     "platform find was compiled on.", p->name);
        if (p->types & bit)
            su_error(1, 0, "Duplicate file type '%c' in the argument list to %s.", *q, p->name);
        p->types |= bit;
        if (q[1] == ',') {
            q++;
            if (!q[1])
                su_error(1, 0, "Last file type in list argument to %s is missing, i.e., list is "
                         "ending on: ','", p->name);
        } else if (q[1]) {
            su_error(1, 0, "Must separate multiple arguments to %s using: ','", p->name);
        }
    }
}

/* a -fprint* / -fls / -fprintf file, opened now (truncated, even if nothing
 * is ever printed to it); the same file twice is one stream */
static struct fd_out *fd_open_out(const char *name) {
    if (!strcmp(name, "/dev/stdout")) return fdg.stdout_o;
    if (!strcmp(name, "/dev/stderr")) return fdg.stderr_o;
    struct stat st;
    if (stat(name, &st) == 0) {
        for (struct fd_out *o = fdg.outs; o; o = o->next)
            if (o->f != stdout && o->f != stderr && o->dev == st.st_dev && o->ino == st.st_ino &&
                (st.st_ino != 0 || !strcmp(o->name, name)))
                return o;
    }
    FILE *f = fopen(name, "w");
    if (!f) su_error(1, errno, "%s", fd_q(name));
    struct fd_out *o = (struct fd_out *)fd_alloc(sizeof *o);
    memset(o, 0, sizeof *o);
    o->f = f;
    o->name = name;
    if (fstat(fileno(f), &st) == 0) { o->dev = st.st_dev; o->ino = st.st_ino; }
    o->next = fdg.outs;
    fdg.outs = o;
    return o;
}

static void fd_printf_compile(struct fd_pred *p, const char *fmt);

/* note that an action writes to O: at the end each such stream is checked */
static struct fd_out *fd_use(struct fd_out *o) {
    for (int i = 0; i < fdg.nused; i++) if (fdg.used[i] == o) return o;
    fdg.used = (struct fd_out **)fd_realloc(fdg.used, (size_t)(fdg.nused + 1) * sizeof *fdg.used);
    fdg.used[fdg.nused++] = o;
    return o;
}

/* -exec -execdir -ok -okdir: the command up to ";" or "{} +" */
static void fd_exec_arg(struct fd_pred *p, int dir, int ok) {
    int start = fd_ai, end = -1, plus = 0;
    for (int i = start; i < fd_ac; i++) {
        if (!strcmp(fd_av[i], ";")) { end = i; break; }
        if (!strcmp(fd_av[i], "+") && i > start && !ok && strstr(fd_av[i - 1], "{}")) {
            if (strcmp(fd_av[i - 1], "{}"))
                su_error(1, 0, "In '%s ... {} +' the '{}' must appear by itself, but you specified %s",
                         p->name, fd_q(fd_av[i - 1]));
            end = i;
            plus = 1;
            break;
        }
    }
    if (end < 0) su_error(1, 0, "missing argument to `%s'", p->name);
    if (end == start && !plus) su_error(1, 0, "invalid argument `%s' to `%s'", fd_av[end], p->name);
    struct fd_exec *x = (struct fd_exec *)fd_alloc(sizeof *x);
    memset(x, 0, sizeof *x);
    x->argv = fd_av + start;
    x->argc = end - start - plus;
    x->dir = dir;
    x->ok = ok;
    x->plus = plus;
    if (plus) {
        /* "{}" only once, at the end */
        for (int i = 0; i < x->argc; i++)
            if (!strcmp(x->argv[i], "{}"))
                su_error(1, 0, "Only one instance of {} is supported with %s ... +", p->name);
        fdg.plus = (struct fd_pred **)fd_realloc(fdg.plus, (size_t)(fdg.nplus + 1) * sizeof *fdg.plus);
        fdg.plus[fdg.nplus++] = p;
    }
    fd_ai = end + 1;
    p->ex = x;
    fd_dname(p, x->argc ? x->argv[0] : "{}");
    if (dir) {
        /* GNU refuses a PATH in which a relative entry could be found */
        const char *path = getenv("PATH");
        if (path) {
            for (const char *q = path;;) {
                const char *e = strchr(q, ':');
                size_t len = e ? (size_t)(e - q) : strlen(q);
                if (len == 0 || (len == 1 && q[0] == '.'))
                    su_error(1, 0, "The current directory is included in the PATH environment "
                             "variable, which is insecure in combination with the %s action of "
                             "find.  Please remove the current directory from your $PATH (that "
                             "is, remove \".\", doubled colons, or leading or trailing colons)",
                             p->name);
                if (q[0] != '/') {
                    char *ent = (char *)fd_alloc(len + 1);
                    memcpy(ent, q, len);
                    ent[len] = 0;
                    su_error(1, 0, "The relative path %s is included in the PATH environment "
                             "variable, which is insecure in combination with the %s action of "
                             "find.  Please remove that entry from $PATH", fd_q(ent), p->name);
                }
                if (!e) break;
                q = e + 1;
            }
        }
    }
}

static void fd_usage(void);

/* one predicate (or operator) from the command line; ARG is fd_av[fd_ai-1] */
static void fd_parse_one(const char *a) {
    struct fd_pred *p;
#define IS(n) (!strcmp(a, n))
    if (IS("(")) { p = fd_add(PK_OPEN, PT_OPEN, "("); goto nonopt; }
    if (IS(")")) { p = fd_add(PK_CLOSE, PT_CLOSE, ")"); goto nonopt; }
    if (IS("!") || IS("-not")) { p = fd_add(PK_NOT, PT_UNI, IS("!") ? "!" : "-not"); goto nonopt; }
    if (IS("-a") || IS("-and")) { p = fd_add(PK_AND, PT_BI, IS("-a") ? "-a" : "-and"); goto nonopt; }
    if (IS("-o") || IS("-or")) { p = fd_add(PK_OR, PT_BI, IS("-o") ? "-o" : "-or"); goto nonopt; }
    if (IS(",")) { p = fd_add(PK_COMMA, PT_BI, ","); goto nonopt; }

    if (a[0] != '-' || !a[1]) {
        struct stat st;
        su_error(0, 0, "paths must precede expression: `%s'", a);
        if (fdg.tail && stat(a, &st) == 0)
            su_error(0, 0, "possible unquoted pattern after predicate `%s'?", fdg.tail->name);
        exit(1);
    }

    /* help and version, wherever they are */
    if (IS("-help") || IS("--help")) { fd_usage(); exit(0); }
    if (IS("-version") || IS("--version")) { su_short = fd_base; su_version(); exit(0); }

    /* positional options: true, and they affect what follows */
    if (IS("-daystart")) {
        if (!fdg.daystart) {
            /* the start of today in the local zone */
            struct tx_tm tm;
            int64_t now = fd_sec_of(fd_now_ns);
            tx_tz_local(tx_tz_cur(), now, 0, &tm);
            int64_t local_mid = now + tm.gmtoff - (tm.hour * 3600 + tm.min * 60 + tm.sec);
            fd_day_start = (fd_i128)tx_tz_mktime(tx_tz_cur(), local_mid, -1) * FD_NS;
            fdg.daystart = 1;
        }
        /* measured: -daystart adds no predicate; it renames the one before
         * it, if any -- "! -daystart" is "expected an expression after
         * '-daystart'", "-daystart , -print" a ',' with nothing before it */
        if (fdg.tail) fdg.tail->name = fdg.tail->dname = (char *)"-daystart";
        return;
    }
    if (IS("-follow")) { fdg.follow = 2; p = fd_add(PK_TRUE, PT_PRIMARY, "-follow"); p->cheap = 1; return; }
    if (IS("-warn") || IS("-nowarn")) {
        fdg.warn = IS("-warn") && !fdg.posixly;     /* measured: POSIXLY_CORRECT silences -warn */
        p = fd_add(PK_TRUE, PT_PRIMARY, IS("-warn") ? "-warn" : "-nowarn");
        p->cheap = 1;
        return;
    }
    if (IS("-regextype")) {
        const char *t = fd_arg("-regextype");
        int i;
        for (i = 0; fd_retypes[i].name; i++) if (!strcmp(fd_retypes[i].name, t)) break;
        if (!fd_retypes[i].name) {
            struct fd_buf b = { 0, 0, 0 };
            for (int j = 0; fd_retypes[j].name; j++)
                fd_bprintf(&b, "%s'%s'", j ? ", " : "", fd_retypes[j].name);
            su_error(1, 0, "Unknown regular expression type %s; valid types are %s.", fd_q(t), b.s);
        }
        fdg.regex_syn = fd_retypes[i].syntax;
        p = fd_add(PK_TRUE, PT_PRIMARY, "-regextype");
        p->cheap = 1;
        fd_dname(p, t);
        return;
    }

    /* global options: true, wherever they are */
    if (IS("-maxdepth") || IS("-mindepth")) {
        fd_global_opt(a);
        const char *n = fd_arg(a);
        const char *q = n;
        while (*q >= '0' && *q <= '9') q++;
        if (!*n || *q)
            su_error(1, 0, "Expected a positive decimal integer argument to %s, but got %s", a, fd_q(n));
        uintmax_t v = 0;
        int over = 0;
        for (q = n; *q; q++) {
            if (v > ((uintmax_t)LONG_MAX - (uintmax_t)(*q - '0')) / 10) { over = 1; break; }
            v = v * 10 + (uintmax_t)(*q - '0');
        }
        if (over) su_error(1, ERANGE, "%s", n);
        if (v == (uintmax_t)LONG_MAX) su_error(1, 0, "%s", n);
        if (v > INT_MAX) su_error(1, ERANGE, "%s", n);
        if (a[2] == 'a') fdg.maxdepth = (long)v; else fdg.mindepth = (long)v;
        p = fd_add(PK_TRUE, PT_PRIMARY, a[2] == 'a' ? "-maxdepth" : "-mindepth");
        p->cheap = 1;
        fd_dname(p, n);
        return;
    }
    if (IS("-depth") || IS("-d")) {
        fd_global_opt(a);
        if (IS("-d") && fdg.warn)
            su_error(0, 0, "warning: the -d option is deprecated; please use -depth instead, "
                     "because the latter is a POSIX-compliant feature.");
        fdg.depth = fdg.depth_explicit = 1;
        fd_add(PK_TRUE, PT_PRIMARY, IS("-d") ? "-d" : "-depth")->cheap = 1;
        return;
    }
    if (IS("-xdev") || IS("-mount")) {
        fd_global_opt(a);
        fdg.xdev = 1;
        fd_add(PK_TRUE, PT_PRIMARY, IS("-xdev") ? "-xdev" : "-mount")->cheap = 1;
        return;
    }
    if (IS("-noleaf")) { fd_global_opt(a); fd_add(PK_TRUE, PT_PRIMARY, "-noleaf")->cheap = 1; return; }
    if (IS("-ignore_readdir_race") || IS("-noignore_readdir_race")) {
        fd_global_opt(a);
        fdg.ignore_race = a[1] == 'i';
        fd_add(PK_TRUE, PT_PRIMARY, a[1] == 'i' ? "-ignore_readdir_race" : "-noignore_readdir_race")->cheap = 1;
        return;
    }
    if (IS("-files0-from")) {
        fd_global_opt(a);
        fdg.files0 = fd_arg(a);
        p = fd_add(PK_TRUE, PT_PRIMARY, "-files0-from");
        p->cheap = 1;
        fd_dname(p, fdg.files0);
        return;
    }

    /* everything else is a test or an action: not an option */
    if (!fdg.first_nonopt) fdg.first_nonopt = a;

    if (IS("-true") || IS("-false")) {
        p = fd_add(IS("-true") ? PK_TRUE : PK_FALSE, PT_PRIMARY, IS("-true") ? "-true" : "-false");
        p->cheap = 1;
        return;
    }
    if (IS("-name") || IS("-iname")) {
        p = fd_add(PK_NAME, PT_PRIMARY, IS("-name") ? "-name" : "-iname");
        p->s = fd_arg(a);
        p->fold = a[1] == 'i';
        p->cheap = 1;
        fd_dname(p, p->s);
        if (fdg.warn && strchr(p->s, '/') && strcmp(p->s, "/"))
            su_error(0, 0, "warning: '%s' matches against basenames only, but the given pattern "
                     "contains a directory separator ('/'), thus the expression will evaluate to "
                     "false all the time.  Did you mean '%s'?", a,
                     p->fold ? "-iwholename" : "-wholename");
        return;
    }
    if (IS("-path") || IS("-ipath") || IS("-wholename") || IS("-iwholename")) {
        p = fd_add(PK_PATH, PT_PRIMARY, a[1] == 'i' ? (a[2] == 'p' ? "-ipath" : "-iwholename")
                                                     : (a[1] == 'p' ? "-path" : "-wholename"));
        p->s = fd_arg(a);
        p->fold = a[1] == 'i';
        p->cheap = 1;
        fd_dname(p, p->s);
        size_t n = strlen(p->s);
        /* said with or without -warn -- unless a starting point matches it */
        int matches = 0;
        for (int k = 0; k < fdg.npaths; k++) if (fd_fnmatch(p->s, fdg.paths[k], p->fold)) matches = 1;
        if (!fdg.npaths && !fdg.files0 && fd_fnmatch(p->s, ".", p->fold)) matches = 1;
        if (n && p->s[n - 1] == '/' && !fdg.posixly && !matches)
            su_error(0, 0, "warning: %s %s will not match anything because it ends with /.", a, p->s);
        return;
    }
    if (IS("-regex") || IS("-iregex")) {
        p = fd_add(PK_REGEX, PT_PRIMARY, IS("-regex") ? "-regex" : "-iregex");
        p->s = fd_arg(a);
        p->fold = a[1] == 'i';
        p->cheap = 1;
        fd_dname(p, p->s);
        const char *err = fd_re_compile(p->s, fdg.regex_syn, p->fold, &p->re);
        if (err) su_error(1, 0, "failed to compile regular expression '%s': %s", p->s, err);
        return;
    }
    if (IS("-lname") || IS("-ilname")) {
        p = fd_add(PK_LNAME, PT_PRIMARY, IS("-lname") ? "-lname" : "-ilname");
        p->s = fd_arg(a);
        p->fold = a[1] == 'i';
        p->need = NEED_STAT;
        p->follow = fdg.follow == 2;
        fd_dname(p, p->s);
        return;
    }
    if (IS("-type") || IS("-xtype")) {
        p = fd_add(IS("-type") ? PK_TYPE : PK_XTYPE, PT_PRIMARY, IS("-type") ? "-type" : "-xtype");
        const char *t = fd_arg(a);
        fd_type_arg(p, t);
        p->need = p->kind == PK_TYPE ? NEED_TYPE : NEED_STAT;
        p->follow = fdg.follow == 2;
        fd_dname(p, t);
        return;
    }
    if (IS("-perm")) {
        p = fd_add(PK_PERM, PT_PRIMARY, "-perm");
        const char *m = fd_arg(a), *s = m;
        p->need = NEED_STAT;
        fd_dname(p, m);
        if (*s == '-' || *s == '/') { p->perm_how = *s; s++; }
        else if (*s == '+' && s[1] >= '0' && s[1] <= '9')
            su_error(1, 0, "invalid mode %s", fd_q(m));
        if (fd_mode_compile(s, &p->mode_d, &p->mode_f) != 0)
            su_error(1, 0, "invalid mode %s", fd_q(m));
        if (p->perm_how == '/' && p->mode_d == 0 && p->mode_f == 0)
            su_error(0, 0, "warning: you have specified a mode pattern %s (which is equivalent to "
                     "/000). The meaning of -perm /000 has now been changed to be consistent with "
                     "-perm -000; that is, while it used to match no files, it now matches all "
                     "files.", m);
        return;
    }
    if (IS("-size")) {
        p = fd_add(PK_SIZE, PT_PRIMARY, "-size");
        const char *v = fd_arg(a);
        p->need = NEED_STAT;
        fd_dname(p, v);
        size_t n = strlen(v);
        if (!n) su_error(1, 0, "invalid null argument to -size");
        int last = (unsigned char)v[n - 1];
        p->unit = 512;
        size_t nlen = n;
        if (!(last >= '0' && last <= '9')) {
            switch (last) {
            case 'b': p->unit = 512; break;
            case 'c': p->unit = 1; break;
            case 'w': p->unit = 2; break;
            case 'k': p->unit = 1024; break;
            case 'M': p->unit = 1024 * 1024; break;
            case 'G': p->unit = 1024UL * 1024 * 1024; break;
            default: su_error(1, 0, "invalid -size type `%c'", last);
            }
            nlen = n - 1;
        }
        char *num = (char *)fd_alloc(nlen + 1);
        memcpy(num, v, nlen);
        num[nlen] = 0;
        const char *s = num;
        if (*s == '+') { p->cmp = FD_GT; s++; }
        else if (*s == '-') { p->cmp = FD_LT; s++; }
        if (fd_umax(s, &p->u) != 0) su_error(1, 0, "Invalid argument `%s' to -size", v);
        free(num);
        return;
    }
    if (IS("-empty")) { p = fd_add(PK_EMPTY, PT_PRIMARY, "-empty"); p->need = NEED_STAT; return; }
    if (IS("-amin") || IS("-cmin") || IS("-mmin") || IS("-atime") || IS("-ctime") || IS("-mtime")) {
        static const char *const names[] = { "-amin", "-cmin", "-mmin", "-atime", "-ctime", "-mtime" };
        int days = a[2] == 't';
        const char *nm = names[(days ? 3 : 0) + (a[1] == 'a' ? 0 : a[1] == 'c' ? 1 : 2)];
        p = fd_add(PK_TIME, PT_PRIMARY, nm);
        p->tx = a[1];
        p->days = days;
        p->need = NEED_STAT;
        const char *v = fd_arg(a);
        fd_dname(p, v);
        fd_time_arg(p, v, days);
        fd_time_window(p);
        return;
    }
    if (IS("-used")) {
        p = fd_add(PK_USED, PT_PRIMARY, "-used");
        p->need = NEED_STAT;
        const char *v = fd_arg(a);
        fd_dname(p, v);
        fd_time_arg(p, v, 1);
        return;
    }
    if (IS("-newer") || IS("-anewer") || IS("-cnewer") || !strncmp(a, "-newer", 6)) {
        int x, y = 'm';
        if (IS("-newer")) x = 'm';
        else if (IS("-anewer")) x = 'a';
        else if (IS("-cnewer")) x = 'c';
        else {
            if (strlen(a) != 8) su_error(1, 0, "unknown predicate `%s'", a);
            x = (unsigned char)a[6];
            y = (unsigned char)a[7];
            if (x == 'B' || y == 'B') {
                su_error(0, 0, "This system does not provide a way to find the birth time of a file.");
                su_error(1, 0, "invalid predicate `%s'", a);
            }
            if (!strchr("acm", x) || !strchr("acmt", y)) su_error(1, 0, "invalid predicate `%s'", a);
        }
        p = fd_add(PK_NEWER, PT_PRIMARY, a);
        p->tx = x;
        p->ty = y;
        p->need = NEED_STAT;
        if (y == 't') {
            if (fd_ai >= fd_ac) su_error(1, 0, "The '%s' test needs an argument", a);
            const char *d = fd_av[fd_ai++];
            fd_dname(p, d);
            int64_t sec;
            long ns;
            if (!tx_parse_date(d, fd_sec_of(fd_now_ns), fd_nsec_of(fd_now_ns), &sec, &ns))
                su_error(1, 0, "I cannot figure out how to interpret %s as a date or time", fd_q(d));
            p->ref = fd_ns(sec, ns);
        } else {
            const char *f = fd_arg(a);
            fd_dname(p, f);
            struct stat st;
            fd_ref_stat(f, &st);
            p->ref = fd_stat_time(&st, y);
        }
        return;
    }
    if (IS("-user") || IS("-group")) {
        int user = a[1] == 'u';
        p = fd_add(user ? PK_UID : PK_GID, PT_PRIMARY, user ? "-user" : "-group");
        p->need = NEED_STAT;
        const char *v = fd_arg(a);
        fd_dname(p, v);
        long id = *v ? fd_name_id(user ? "/etc/passwd" : "/etc/group", v) : -1;
        if (id >= 0) p->u = (uintmax_t)id;
        else {
            uintmax_t n = 0;
            if (!*v || fd_umax(v, &n) != 0 || n > 0xFFFFFFFFu)
                su_error(1, 0, "invalid %s name or %cID argument to %s: %s", user ? "user" : "group",
                         user ? 'U' : 'G', a, fd_q(v));
            p->u = n;
        }
        return;
    }
    if (IS("-uid") || IS("-gid") || IS("-links") || IS("-inum")) {
        int k = IS("-uid") ? PK_UID : IS("-gid") ? PK_GID : IS("-links") ? PK_LINKS : PK_INUM;
        p = fd_add(k, PT_PRIMARY, IS("-uid") ? "-uid" : IS("-gid") ? "-gid" : IS("-links") ? "-links" : "-inum");
        p->need = k == PK_INUM ? NEED_TYPE : NEED_STAT;
        const char *v = fd_arg(a);
        fd_dname(p, v);
        fd_num_arg(p, v);
        return;
    }
    if (IS("-nouser") || IS("-nogroup")) {
        p = fd_add(IS("-nouser") ? PK_NOUSER : PK_NOGROUP, PT_PRIMARY, IS("-nouser") ? "-nouser" : "-nogroup");
        p->need = NEED_STAT;
        return;
    }
    if (IS("-samefile")) {
        p = fd_add(PK_SAMEFILE, PT_PRIMARY, "-samefile");
        p->need = NEED_STAT;
        const char *f = fd_arg(a);
        fd_dname(p, f);
        struct stat st;
        fd_ref_stat(f, &st);
        p->dev = st.st_dev;
        p->ino = st.st_ino;
        /* measured: -D stat shows -samefile's file twice -- once, when it
         * is a link that is not followed */
        if (fdg.debug & DB_STAT) {
            struct stat ls;
            if (!(!fdg.follow && lstat(f, &ls) == 0 && S_ISLNK(ls.st_mode)))
                fprintf(stderr, "debug_stat (%s)\n", f);
        }
        return;
    }
    if (IS("-readable") || IS("-writable") || IS("-executable")) {
        p = fd_add(PK_ACCESS, PT_PRIMARY, IS("-readable") ? "-readable" : IS("-writable") ? "-writable" : "-executable");
        p->amode = a[1] == 'r' ? R_OK : a[1] == 'w' ? W_OK : X_OK;
        return;
    }
    if (IS("-fstype")) {
        p = fd_add(PK_FSTYPE, PT_PRIMARY, "-fstype");
        p->need = NEED_STAT;
        p->s = fd_arg(a);
        fd_dname(p, p->s);
        return;
    }
    if (IS("-context")) {
        fd_arg(a);
        su_error(1, 0, "invalid predicate -context: SELinux is not enabled.");
    }

    /* actions */
    if (IS("-print") || IS("-print0") || IS("-fprint") || IS("-fprint0")) {
        int f = a[1] == 'f';
        p = fd_add(PK_PRINT, PT_PRIMARY, f ? (a[7] ? "-fprint0" : "-fprint") : (a[6] ? "-print0" : "-print"));
        p->side_effects = p->action = 1;
        p->print0 = a[f ? 7 : 6] == '0';
        p->out = fdg.stdout_o;
        if (f) {
            const char *n = fd_arg(a);
            fd_dname(p, n);
            p->out = fd_open_out(n);
        }
        fd_use(p->out);
        return;
    }
    if (IS("-ls") || IS("-fls")) {
        p = fd_add(PK_LS, PT_PRIMARY, IS("-ls") ? "-ls" : "-fls");
        p->side_effects = p->action = 1;
        p->need = NEED_STAT;
        p->out = fdg.stdout_o;
        if (IS("-fls")) {
            const char *n = fd_arg(a);
            fd_dname(p, n);
            p->out = fd_open_out(n);
        }
        fd_use(p->out);
        return;
    }
    if (IS("-printf") || IS("-fprintf")) {
        p = fd_add(PK_PRINTF, PT_PRIMARY, IS("-printf") ? "-printf" : "-fprintf");
        p->side_effects = p->action = 1;
        p->out = fdg.stdout_o;
        if (IS("-fprintf")) {
            const char *n = fd_arg(a);
            if (fd_ai >= fd_ac) su_error(1, 0, "invalid argument `%s' to `%s'", n, a);
            p->out = fd_open_out(n);
        }
        fd_use(p->out);
        const char *f = fd_arg(a);
        fd_dname(p, f);
        fd_printf_compile(p, f);
        return;
    }
    if (IS("-exec") || IS("-execdir") || IS("-ok") || IS("-okdir")) {
        int dir = !strcmp(a + strlen(a) - 3, "dir"), ok = a[1] == 'o';
        p = fd_add(PK_EXEC, PT_PRIMARY, IS("-exec") ? "-exec" : IS("-execdir") ? "-execdir" :
                                        IS("-ok") ? "-ok" : "-okdir");
        p->side_effects = p->action = 1;
        fd_exec_arg(p, dir, ok);
        if (ok) fdg.has_ok = 1;
        return;
    }
    if (IS("-delete")) {
        p = fd_add(PK_DELETE, PT_PRIMARY, "-delete");
        p->side_effects = p->action = 1;
        fdg.depth = 1;
        fdg.has_delete = 1;
        return;
    }
    if (IS("-prune")) {
        p = fd_add(PK_PRUNE, PT_PRIMARY, "-prune");
        p->side_effects = 1;
        p->need = NEED_STAT;
        p->follow = fdg.depth;       /* here: -depth was already in effect */
        fdg.has_prune = 1;
        return;
    }
    if (IS("-quit")) {
        p = fd_add(PK_QUIT, PT_PRIMARY, "-quit");
        p->side_effects = 1;
        return;
    }
#undef IS
    su_error(1, 0, "unknown predicate `%s'", a);
    return;

nonopt:
    if (!fdg.first_nonopt) fdg.first_nonopt = p->name;
}

static void fd_flush_all(void);

/* ---- -printf's format ------------------------------------------------------
 *
 * Escapes: \a \b \c (stop here) \f \n \r \t \v \0 \\ and \NNN (one to three
 * octal digits, the low byte); any other is "warning: unrecognized escape
 * `\X'" and both characters print; a backslash at the end is "warning:
 * escape `\' followed by nothing at all". Directives: %[-+ #]*[0-9]*
 * (.[0-9]*)? then a letter; %A %B %C %T take one more (a strftime letter,
 * '@' or '+'). An unknown letter is "warning: unrecognized format directive
 * `%X'" and the directive prints as written; %{ %[ %( (and a directive cut
 * off by the end) are "error: the format directive `%X' is reserved for
 * future use"; a lone % at the end is "error: % at end of format string".
 * These are said when the format is read. */
static void fd_seg_add(struct fd_pred *p, struct fd_seg *s) {
    p->segs = (struct fd_seg *)fd_realloc(p->segs, (size_t)(p->nseg + 1) * sizeof *p->segs);
    p->segs[p->nseg++] = *s;
}

static void fd_seg_text(struct fd_pred *p, struct fd_buf *t) {
    if (!t->n) return;
    struct fd_seg s;
    memset(&s, 0, sizeof s);
    s.text = t->s;
    s.tlen = t->n;
    fd_seg_add(p, &s);
    t->s = 0;
    t->n = t->cap = 0;
}

static void fd_printf_compile(struct fd_pred *p, const char *fmt) {
    struct fd_buf t = { 0, 0, 0 };
    const char *f = fmt;
    while (*f) {
        if (*f == '\\') {
            f++;
            if (!*f) {
                su_error(0, 0, "warning: escape `\\' followed by nothing at all");
                fd_bputc(&t, '\\');
                break;
            }
            int c = (unsigned char)*f;
            if (c >= '0' && c <= '7') {
                int v = 0, n = 0;
                while (n < 3 && *f >= '0' && *f <= '7') { v = v * 8 + (*f - '0'); f++; n++; }
                fd_bputc(&t, v & 0xff);
                continue;
            }
            f++;
            switch (c) {
            case 'a': fd_bputc(&t, '\a'); break;
            case 'b': fd_bputc(&t, '\b'); break;
            case 'f': fd_bputc(&t, '\f'); break;
            case 'n': fd_bputc(&t, '\n'); break;
            case 'r': fd_bputc(&t, '\r'); break;
            case 't': fd_bputc(&t, '\t'); break;
            case 'v': fd_bputc(&t, '\v'); break;
            case '\\': fd_bputc(&t, '\\'); break;
            case 'c': {
                fd_seg_text(p, &t);
                struct fd_seg s;
                memset(&s, 0, sizeof s);
                s.stop = 1;
                fd_seg_add(p, &s);
                break;
            }
            default:
                su_error(0, 0, "warning: unrecognized escape `\\%c'", c);
                fd_bputc(&t, '\\');
                fd_bputc(&t, c);
            }
            continue;
        }
        if (*f != '%') { fd_bputc(&t, (unsigned char)*f++); continue; }
        const char *start = f++;
        if (!*f) su_error(1, 0, "error: %% at end of format string");
        if (*f == '%') { fd_bputc(&t, '%'); f++; continue; }
        struct fd_seg s;
        memset(&s, 0, sizeof s);
        s.width = s.prec = -1;
        int nf = 0;
        while (*f == '-' || *f == '+' || *f == ' ' || *f == '#') {
            if (nf < 7 && !strchr(s.flags, *f)) s.flags[nf++] = *f;
            f++;
        }
        if (*f >= '0' && *f <= '9') {
            if (*f == '0' && nf < 7 && !strchr(s.flags, '0')) s.flags[nf++] = '0';
            long w = 0;
            while (*f >= '0' && *f <= '9') { if (w < 100000000000L) w = w * 10 + (*f - '0'); f++; }
            s.width = w > INT_MAX ? INT_MAX : (int)w;
            if (w > INT_MAX) s.width = -2;        /* printf's EOVERFLOW */
        }
        if (*f == '.') {
            f++;
            long w = 0;
            while (*f >= '0' && *f <= '9') { if (w < 100000000000L) w = w * 10 + (*f - '0'); f++; }
            s.prec = w > INT_MAX ? INT_MAX : (int)w;
        }
        int c = (unsigned char)*f;
        if (!c || c == '{' || c == '[' || c == '(') {
            /* measured: the directive cut off by the end is named with its NUL */
            fflush(stdout);
            fprintf(stderr, "%s: error: the format directive `%%", su_short);
            fputc(c, stderr);
            fputs("' is reserved for future use\n", stderr);
            exit(1);
        }
        f++;
        if (c == '%') {
            /* "%10%" prints the directive's text without its last '%' */
            fd_bput(&t, start, (size_t)(f - 1 - start));
            continue;
        }
        if (strchr("AaBbCcDdFfGgHhiklMmnPpSsTtUuYyZ", c) == 0) {
            su_error(0, 0, "warning: unrecognized format directive `%%%c'", c);
            fd_bput(&t, start, (size_t)(f - start));
            continue;
        }
        if (c == 'A' || c == 'B' || c == 'C' || c == 'T') {
            if (!*f) {
                su_error(0, 0, "warning: format directive `%%%c' should be followed by another character", c);
                fd_bput(&t, start, (size_t)(f - start));
                continue;
            }
            s.k = (unsigned char)*f++;
        }
        fd_seg_text(p, &t);
        s.kind = c;
        s.text = (char *)fd_alloc((size_t)(f - start) + 1);
        memcpy(s.text, start, (size_t)(f - start));
        s.text[f - start] = 0;
        fd_seg_add(p, &s);
        /* what the directives need: the name ones nothing (and %i readdir's
         * inode number), %y the type */
        if (strchr("fhHpPdi", c)) continue;
        if (c == 'y') { if (p->need < NEED_TYPE) p->need = NEED_TYPE; continue; }
        p->need = NEED_STAT;
    }
    fd_seg_text(p, &t);
    free(t.s);
}

/* ---- a file being looked at ------------------------------------------------ */

struct fd_file {
    const char *path;           /* as printed */
    const char *root;           /* the starting point it is under */
    int depth;
    int dtype;                  /* the type from readdir: DT_*, or -1 */
    int have;                   /* st is real */
    int stat_err;               /* the lazy stat failed with this */
    int stat_tried;
    int followed;               /* st is the target's (a link was followed) */
    int islink;                 /* it is a symbolic link (readdir or lstat said) */
    int err_said;               /* its stat's failure has been reported */
    ino_t dino;                 /* the inode number readdir gave, or 0 */
    struct stat st;
    int prune;
    const char *dir;            /* -execdir's directory */
    const char *base;           /* -execdir's ./NAME */
};

/* the follow rule for this file: -L everything, -H the starting points */
static int fd_follows(const struct fd_file *f) {
    return fdg.follow == 2 || (fdg.follow == 1 && f->depth == 0);
}

/* the name -D stat shows: the path, or under -P and -H below a starting
 * point the last component (GNU's fts works from within the directory) */
static const char *fd_accpath(const struct fd_file *f) {
    if (f->depth > 0 && fdg.follow != 2) {
        const char *b = strrchr(f->path, '/');
        if (b) return b + 1;
    }
    return f->path;
}

/* stat F as the link options say, -D stat's lines and all; 0 or an errno */
static int fd_do_stat(struct fd_file *f) {
    if (fdg.debug & DB_STAT) fprintf(stderr, "debug_stat (%s)\n", fd_accpath(f));
    int r;
    if (fd_follows(f)) {
        r = stat(f->path, &f->st);
        if (r == 0) f->followed = f->islink;
        else if (errno == ENOENT || errno == ENOTDIR) {
            if (fdg.debug & DB_STAT)
                fprintf(stderr, "fallback_stat(): stat(%s) failed; falling back on lstat()\n", f->path);
            r = lstat(f->path, &f->st);
        }
    } else
        r = lstat(f->path, &f->st);
    if (r != 0) return errno ? errno : EIO;
    f->have = 1;
    if (S_ISLNK(f->st.st_mode)) f->islink = 1;
    return 0;
}

/* the stat a test needs, made now if the walk did not: measured, it is
 * tried again for each test that needs it, and a failure is said once */
static int fd_need_stat(struct fd_file *f) {
    if (f->have) return 1;
    int e = fd_do_stat(f);
    if (!e) return 1;
    f->stat_err = e;
    if (!f->err_said && !(fdg.ignore_race && e == ENOENT)) {
        su_error(0, e, "%s", fd_q(f->path));
        fd_status = 1;
    }
    f->err_said = 1;
    return 0;
}

static int fd_mode_type(mode_t m) {
    if (S_ISREG(m)) return 'f';
    if (S_ISDIR(m)) return 'd';
    if (S_ISLNK(m)) return 'l';
    if (S_ISCHR(m)) return 'c';
    if (S_ISBLK(m)) return 'b';
    if (S_ISFIFO(m)) return 'p';
    if (S_ISSOCK(m)) return 's';
    return 'U';
}

static int fd_dt_type(int dt) {
    switch (dt) {
    case DT_REG: return 'f';
    case DT_DIR: return 'd';
    case DT_LNK: return 'l';
    case DT_CHR: return 'c';
    case DT_BLK: return 'b';
    case DT_FIFO: return 'p';
    case DT_SOCK: return 's';
    }
    return 0;
}

/* the file's type letter, from stat or readdir; 0 if neither can say */
static int fd_type_of(struct fd_file *f) {
    if (f->have) return fd_mode_type(f->st.st_mode);
    int t = f->dtype >= 0 ? fd_dt_type(f->dtype) : 0;
    /* a link that is followed needs stat to say what it is */
    if (t && !(t == 'l' && fd_follows(f))) return t;
    if (!fd_need_stat(f)) return 0;
    return fd_mode_type(f->st.st_mode);
}

/* the name -name sees: the last component, trailing slashes dropped ("/"
 * for a path of slashes) */
static char *fd_basename(const char *path) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    if (n == 1 && path[0] == '/') return fd_strdup("/");
    size_t s = n;
    while (s > 0 && path[s - 1] != '/') s--;
    char *b = (char *)fd_alloc(n - s + 1);
    memcpy(b, path + s, n - s);
    b[n - s] = 0;
    return b;
}

/* %f: the last component, with one '/' if the path ended in some */
static char *fd_pct_f(const char *path) {
    size_t n = strlen(path);
    size_t e = n;
    while (e > 0 && path[e - 1] == '/') e--;
    if (e == 0) return fd_strdup(n ? "/" : "");
    size_t s = e;
    while (s > 0 && path[s - 1] != '/') s--;
    int slash = e < n;
    char *b = (char *)fd_alloc(e - s + 2);
    memcpy(b, path + s, e - s);
    if (slash) b[e - s] = '/';
    b[e - s + slash] = 0;
    return b;
}

/* %h, as measured: the path up to the slash before its last component; a
 * path of one component is "." -- except that one of one character with
 * slashes after it ("x/", ".//") loses only its last slash */
static char *fd_pct_h(const char *path) {
    size_t n = strlen(path);
    size_t e = n;
    while (e > 0 && path[e - 1] == '/') e--;
    if (e == 0) {                     /* all slashes */
        char *b = (char *)fd_alloc(n);
        memcpy(b, path, n - 1);
        b[n - 1] = 0;
        return b;
    }
    size_t s = e;
    while (s > 0 && path[s - 1] != '/') s--;
    if (s == 0) {
        if (e == n || e > 1) return fd_strdup(".");
        char *b = (char *)fd_alloc(n);
        memcpy(b, path, n - 1);
        b[n - 1] = 0;
        return b;
    }
    char *b = (char *)fd_alloc(s);
    memcpy(b, path, s - 1);
    b[s - 1] = 0;
    return b;
}

/* ---- output ------------------------------------------------------------------ */

static void fd_write(struct fd_out *o, const char *s, size_t n) {
    if (!n) return;
    if (fwrite(s, 1, n, o->f) != n && !o->failed) {
        o->failed = errno ? errno : EIO;
    }
}

/* a name, as -print and -printf's name directives show it: on a terminal
 * an unprintable byte is '?' */
static void fd_write_name(struct fd_out *o, const char *s) {
    if (!(isatty(fileno(o->f)))) { fd_write(o, s, strlen(s)); return; }
    struct fd_buf b = { 0, 0, 0 };
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        fd_bputc(&b, (c < 0x20 || c >= 0x7f) ? '?' : c);
    }
    if (b.n) fd_write(o, b.s, b.n);
    free(b.s);
}

static void fd_mode_str(mode_t m, char *out) {
    int t = fd_mode_type(m);
    out[0] = t == 'f' ? '-' : t == 'U' ? '?' : (char)t;
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) out[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
    if (m & 04000) out[3] = (m & 0100) ? 's' : 'S';
    if (m & 02000) out[6] = (m & 0010) ? 's' : 'S';
    if (m & 01000) out[9] = (m & 0001) ? 't' : 'T';
    out[10] = 0;
}

static int fd_read_link(const char *path, struct fd_buf *b) {
    size_t cap = 256;
    for (;;) {
        char *buf = (char *)fd_alloc(cap);
        ssize_t n = readlink(path, buf, cap);
        if (n < 0) { free(buf); return -1; }
        if ((size_t)n < cap) { fd_bput(b, buf, (size_t)n); free(buf); return 0; }
        free(buf);
        cap *= 2;
    }
}

/* the -ls line */
static void fd_ls(struct fd_out *o, struct fd_file *f) {
    struct fd_buf b = { 0, 0, 0 };
    const struct stat *st = &f->st;
    char mode[11];
    fd_mode_str(st->st_mode, mode);
    uintmax_t blocks = (uintmax_t)st->st_blocks;
    if (!fdg.posixly) blocks = (blocks + 1) / 2;     /* 1 KiB blocks */
    fd_bprintf(&b, "%9ju %6ju %s %3ju ", (uintmax_t)st->st_ino, blocks, mode, (uintmax_t)st->st_nlink);
    const char *u = fd_id_name("/etc/passwd", (unsigned long)st->st_uid, &fd_unames);
    const char *g = fd_id_name("/etc/group", (unsigned long)st->st_gid, &fd_gnames);
    if (u) fd_bprintf(&b, "%-8s ", u); else fd_bprintf(&b, "%-8lu ", (unsigned long)st->st_uid);
    if (g) fd_bprintf(&b, "%-8s ", g); else fd_bprintf(&b, "%-8lu ", (unsigned long)st->st_gid);
    if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode))
        fd_bprintf(&b, "%3u, %3u ", (unsigned)major(st->st_rdev), (unsigned)minor(st->st_rdev));
    else
        fd_bprintf(&b, "%8ju ", (uintmax_t)st->st_size);
    /* the date: recent (in the last 180 days, and not more than an hour
     * ahead) with the time, else with the year */
    int64_t t = (int64_t)st->st_mtime;
    int64_t now = fd_sec_of(fd_now_ns);
    int recent = t > now - 180L * FD_DAY && t <= now + 3600;
    char *ds = tx_strftime(recent ? "%b %e %H:%M" : "%b %e  %Y", t, 0, 0);
    fd_bputs(&b, ds);
    free(ds);
    fd_bputc(&b, ' ');
    fd_ls_name(&b, f->path);
    if (S_ISLNK(st->st_mode)) {
        struct fd_buf l = { 0, 0, 0 };
        if (fd_read_link(f->path, &l) == 0) {
            fd_bputs(&b, " -> ");
            fd_ls_name(&b, l.s ? l.s : "");
        }
        free(l.s);
    }
    fd_bputc(&b, '\n');
    fd_write(o, b.s, b.n);
    free(b.s);
}

/* a rational A/B as printf's %g would print it (six significant digits,
 * trailing zeros dropped), with no floating point */
static void fd_fmt_g(struct fd_buf *b, uintmax_t a, uintmax_t d) {
    if (d == 0) { fd_bputs(b, "inf"); return; }
    if (a == 0) { fd_bputc(b, '0'); return; }
    /* find the exponent: 10^e <= a/d < 10^(e+1) */
    int e = 0;
    fd_i128 na = a, nd = d;
    while (na >= nd * 10) { nd *= 10; e++; }
    while (na < nd) { na *= 10; e--; }
    /* six digits, rounded half to even like glibc's correctly rounded output */
    fd_i128 digits = 0, rem = na;
    for (int i = 0; i < 6; i++) {
        fd_i128 q = rem / nd;
        digits = digits * 10 + q;
        rem = (rem - q * nd) * 10;
    }
    /* rem/10 is the remainder scaled; compare 2*rem with 10*nd */
    if (rem * 2 > nd * 10 || (rem * 2 == nd * 10 && (digits & 1))) {
        digits++;
        if (digits == 1000000) { digits = 100000; e++; }
    }
    char ds[8];
    for (int i = 5; i >= 0; i--) { ds[i] = (char)('0' + (int)(digits % 10)); digits /= 10; }
    ds[6] = 0;
    int last = 5;
    if (e < -4 || e >= 6) {
        while (last > 0 && ds[last] == '0') last--;
        fd_bputc(b, ds[0]);
        if (last > 0) { fd_bputc(b, '.'); fd_bput(b, ds + 1, (size_t)last); }
        fd_bprintf(b, "e%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
        return;
    }
    /* fixed: e+1 digits before the point */
    char out[32];
    int n = 0;
    if (e < 0) {
        out[n++] = '0';
        out[n++] = '.';
        for (int i = -1; i > e; i--) out[n++] = '0';
        for (int i = 0; i < 6; i++) out[n++] = ds[i];
    } else {
        for (int i = 0; i < 6; i++) {
            out[n++] = ds[i];
            if (i == e && i < 5) out[n++] = '.';
        }
    }
    out[n] = 0;
    if (strchr(out, '.')) {
        while (n > 0 && out[n - 1] == '0') n--;
        if (n > 0 && out[n - 1] == '.') n--;
    }
    fd_bput(b, out, (size_t)n);
}

/* a time directive's text: %Ak and the rest */
static void fd_time_dir(struct fd_buf *b, int k, fd_i128 t) {
    int64_t sec = fd_sec_of(t);
    long ns = fd_nsec_of(t);
    char frac[16];
    snprintf(frac, sizeof frac, ".%09ld0", ns);
    if (k == '@') { fd_bprintf(b, "%lld%s", (long long)sec, frac); return; }
    struct tx_tm tm;
    tx_tz_local(tx_tz_cur(), sec, ns, &tm);
    if (k == '+') {
        char *s = tx_strftime("%Y-%m-%d+%H:%M:%S", sec, ns, 0);
        fd_bputs(b, s);
        free(s);
        fd_bputs(b, frac);
        return;
    }
    if (k == 'S' || k == 'T' || k == 'X') {
        char *s = tx_strftime(k == 'S' ? "%S" : "%H:%M:%S", sec, ns, 0);
        fd_bputs(b, s);
        free(s);
        fd_bputs(b, frac);
        return;
    }
    /* what glibc's strftime makes of "%K": its conversions, else the text
     * itself, padded to a digit's width when K is a digit */
    if (k >= '0' && k <= '9') {
        for (int i = 2; i < k - '0'; i++) fd_bputc(b, ' ');
        fd_bputc(b, '%');
        fd_bputc(b, k);
        return;
    }
    if (!strchr("aAbBcCdDeFgGhHIjklmMnpPrRstTuUVwWxXyYzZ%", k)) {
        fd_bputc(b, '%');
        fd_bputc(b, k);
        return;
    }
    char fmt[3] = { '%', (char)k, 0 };
    char *s = tx_strftime(fmt, sec, ns, 0);
    fd_bputs(b, s);
    free(s);
}

/* print TEXT for directive S: its width, precision and '-' as %s takes them */
static void fd_put_field(struct fd_buf *out, const struct fd_seg *s, const char *text) {
    size_t len = strlen(text);
    if (s->prec >= 0 && (size_t)s->prec < len) len = (size_t)s->prec;
    int w = s->width < 0 ? 0 : s->width;
    int left = strchr(s->flags, '-') != 0;
    if (!left) for (int i = (int)len; i < w; i++) fd_bputc(out, ' ');
    fd_bput(out, text, len);
    if (left) for (int i = (int)len; i < w; i++) fd_bputc(out, ' ');
}

/* %d and %m take printf's numeric flags: -, +, space, # and 0, with a
 * width and a precision -- done here as glibc's printf does them, so the
 * answer does not depend on the C library's printf */
static void fd_put_num(struct fd_buf *out, const struct fd_seg *s, uintmax_t v, int octal) {
    char digits[32];
    int nd = 0;
    uintmax_t x = v;
    do { digits[nd++] = (char)('0' + (int)(x % (octal ? 8 : 10))); x /= octal ? 8 : 10; } while (x);
    if (s->prec == 0 && v == 0) nd = 0;
    int prec = s->prec < 0 ? 1 : s->prec;
    if (s->prec == 0) prec = 0;
    int zeros = prec > nd ? prec - nd : 0;
    /* '#' with octal: the first digit is a 0 */
    if (octal && strchr(s->flags, '#') && zeros == 0 && (nd == 0 || digits[nd - 1] != '0')) zeros = 1;
    const char *sign = "";
    if (!octal) {
        if (strchr(s->flags, '+')) sign = "+";
        else if (strchr(s->flags, ' ')) sign = " ";
    }
    int len = (int)strlen(sign) + zeros + nd;
    int w = s->width < 0 ? 0 : s->width;
    int left = strchr(s->flags, '-') != 0;
    int zpad = !left && strchr(s->flags, '0') && s->prec < 0;
    if (!left && !zpad) for (int i = len; i < w; i++) fd_bputc(out, ' ');
    fd_bputs(out, sign);
    if (zpad) for (int i = len; i < w; i++) fd_bputc(out, '0');
    for (int i = 0; i < zeros; i++) fd_bputc(out, '0');
    for (int i = nd - 1; i >= 0; i--) fd_bputc(out, digits[i]);
    if (left) for (int i = len; i < w; i++) fd_bputc(out, ' ');
}

static int fd_printf_run(struct fd_pred *p, struct fd_file *f) {
    struct fd_buf b = { 0, 0, 0 };
    int tty = isatty(fileno(p->out->f));
    for (int i = 0; i < p->nseg; i++) {
        const struct fd_seg *s = &p->segs[i];
        if (s->stop) break;
        if (!s->kind) { fd_bput(&b, s->text, s->tlen); continue; }
        if (s->width == -2) {
            /* a width past INT_MAX: printf says EOVERFLOW */
            if (b.n) fd_write(p->out, b.s, b.n);
            b.n = 0;
            fd_flush_all();
            su_error(0, EOVERFLOW, "%s", fd_q(p->out->name));
            fd_status = 1;
            continue;
        }
        struct fd_buf v = { 0, 0, 0 };
        int quote = 0;              /* a name: '?' for unprintable bytes on a terminal */
        const struct stat *st = &f->st;
        switch (s->kind) {
        case 'p': fd_bputs(&v, f->path); quote = 1; break;
        case 'f': { char *x = fd_pct_f(f->path); fd_bputs(&v, x); free(x); quote = 1; break; }
        case 'h': { char *x = fd_pct_h(f->path); fd_bputs(&v, x); free(x); quote = 1; break; }
        case 'H': fd_bputs(&v, f->root); break;
        case 'P': {
            const char *q = f->path + strlen(f->root);
            if (f->depth == 0) q = f->path + strlen(f->path);
            else if (*q == '/') q++;
            fd_bputs(&v, q);
            quote = 1;
            break;
        }
        case 'd': fd_put_num(&b, s, (uintmax_t)f->depth, 0); continue;
        case 'm': fd_put_num(&b, s, (uintmax_t)(st->st_mode & 07777), 1); continue;
        case 'M': { char m[11]; fd_mode_str(st->st_mode, m); fd_bputs(&v, m); break; }
        case 's': fd_bprintf(&v, "%ju", (uintmax_t)st->st_size); break;
        case 'b': fd_bprintf(&v, "%ju", (uintmax_t)st->st_blocks); break;
        case 'k': fd_bprintf(&v, "%ju", ((uintmax_t)st->st_blocks + 1) / 2); break;
        case 'S':
            if (st->st_size == 0) fd_bputc(&v, '1');
            else fd_fmt_g(&v, (uintmax_t)st->st_blocks * 512, (uintmax_t)st->st_size);
            break;
        case 'n': fd_bprintf(&v, "%ju", (uintmax_t)st->st_nlink); break;
        case 'i': fd_bprintf(&v, "%ju", (uintmax_t)(f->have || !f->dino ? st->st_ino : f->dino)); break;
        case 'D': fd_bprintf(&v, "%ju", (uintmax_t)st->st_dev); break;
        case 'U': fd_bprintf(&v, "%lu", (unsigned long)st->st_uid); break;
        case 'G': fd_bprintf(&v, "%lu", (unsigned long)st->st_gid); break;
        case 'u': {
            const char *u = fd_id_name("/etc/passwd", (unsigned long)st->st_uid, &fd_unames);
            if (u) fd_bputs(&v, u); else fd_bprintf(&v, "%lu", (unsigned long)st->st_uid);
            break;
        }
        case 'g': {
            const char *g = fd_id_name("/etc/group", (unsigned long)st->st_gid, &fd_gnames);
            if (g) fd_bputs(&v, g); else fd_bprintf(&v, "%lu", (unsigned long)st->st_gid);
            break;
        }
        case 'F': fd_bputs(&v, fd_fstype_of(st->st_dev)); break;
        case 'l':
            if (f->have ? S_ISLNK(st->st_mode) : f->dtype == DT_LNK) {
                if (!f->followed) fd_read_link(f->path, &v);
            }
            quote = 1;
            break;
        case 'y': { int t = fd_type_of(f); fd_bputc(&v, t ? t : 'U'); break; }
        case 'Y': {
            int t = fd_mode_type(st->st_mode);
            if (t == 'l') {
                struct stat ts;
                if (stat(f->path, &ts) == 0) t = fd_mode_type(ts.st_mode);
                else t = errno == ELOOP ? 'L' : errno == ENOENT ? 'N' : '?';
            }
            fd_bputc(&v, t);
            break;
        }
        case 'a': fd_ctime(&v, (int64_t)st->st_atime); goto ctime_frac;
        case 'c': fd_ctime(&v, (int64_t)st->st_ctime); goto ctime_frac;
        case 't': fd_ctime(&v, (int64_t)st->st_mtime);
        ctime_frac: {
            /* ctime's text with the fraction of the second before the year */
            fd_i128 tt = s->kind == 'a' ? FD_ATIM(st) : s->kind == 'c' ? FD_CTIM(st) : FD_MTIM(st);
            char frac[16];
            snprintf(frac, sizeof frac, ".%09ld0", fd_nsec_of(tt));
            char *sp = v.s ? strrchr(v.s, ' ') : 0;
            if (sp) {
                struct fd_buf w = { 0, 0, 0 };
                fd_bput(&w, v.s, (size_t)(sp - v.s));
                fd_bputs(&w, frac);
                fd_bputs(&w, sp);
                free(v.s);
                v = w;
            }
            break;
        }
        case 'A': fd_time_dir(&v, s->k, FD_ATIM(st)); break;
        case 'C': fd_time_dir(&v, s->k, FD_CTIM(st)); break;
        case 'T': fd_time_dir(&v, s->k, FD_MTIM(st)); break;
        case 'B': break;                    /* no birth times here: empty */
        case 'Z':
            /* no SELinux: the field is empty, and each file is an error */
            fd_put_field(&b, s, "");
            if (b.n) fd_write(p->out, b.s, b.n);
            b.n = 0;
            fd_flush_all();
            su_error(0, ENODATA, "getfilecon failed: %s", fd_q(f->path));
            fd_status = 1;
            continue;
        }
        const char *txt = v.s ? v.s : "";
        if (quote && tty) {
            for (char *q = v.s; q && *q; q++)
                if ((unsigned char)*q < 0x20 || (unsigned char)*q >= 0x7f) *q = '?';
        }
        fd_put_field(&b, s, txt);
        free(v.s);
    }
    if (b.n) fd_write(p->out, b.s, b.n);
    free(b.s);
    return 1;
}

/* ---- running commands ------------------------------------------------------- */

static void fd_flush_all(void) {
    for (struct fd_out *o = fdg.outs; o; o = o->next) fflush(o->f);
    fflush(stdout);
}

/* ARGV, in DIR when not 0; true if it exited 0. PATH is the file (for -ok's
 * question). */
static int fd_run(struct fd_exec *x, char **argv, int argc, const char *dir, const char *path) {
    fd_flush_all();
    if (x->ok) {
        fprintf(stderr, "< %s ... %s > ? ", argv[0], path);
        fflush(stderr);
        char *line = 0;
        size_t cap = 0;
        ssize_t n = getline(&line, &cap, stdin);
        int yes = n > 0 && (line[0] == 'y' || line[0] == 'Y');
        free(line);
        if (!yes) return 0;
    }
    if (fdg.debug & DB_EXEC) {
        fprintf(stderr, "DebugExec: launching process (argc=%d):", argc);
        for (int i = 0; i < argc; i++) fprintf(stderr, " %s", fd_q(argv[i]));
        fputc('\n', stderr);
    }
    pid_t pid = fork();
    if (pid < 0) {
        su_error(0, errno, "cannot fork");
        fd_status = 1;
        return 0;
    }
    if (pid == 0) {
        if (dir && chdir(dir) != 0) {
            su_error(0, errno, "%s", fd_q(dir));
            _exit(1);
        }
        if (x->ok) close(0);
        execvp(argv[0], argv);
        su_error(0, errno, "%s", fd_q(argv[0]));
        _exit(1);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) { fd_status = 1; return 0; }
    }
    /* the native wait reports a signal's death as an exit too: ask first */
    if (WIFSIGNALED(st)) {
        su_error(0, 0, "%s terminated by signal %d", fd_q(argv[0]), WTERMSIG(st));
        return 0;
    }
    int code = WEXITSTATUS(st);
    if (fdg.debug & DB_EXEC)
        fprintf(stderr, "DebugExec: process (PID=%ld) terminated with exit status: %d\n", (long)pid, code);
    return code == 0;
}

/* WORD with every "{}" in it replaced by WITH */
static char *fd_subst(const char *w, const char *with) {
    struct fd_buf b = { 0, 0, 0 };
    size_t wl = strlen(with);
    for (const char *q = w; *q;) {
        if (q[0] == '{' && q[1] == '}') { fd_bput(&b, with, wl); q += 2; }
        else fd_bputc(&b, (unsigned char)*q++);
    }
    if (!b.s) return fd_strdup("");
    return b.s;
}

#define FD_ARG_MAX (128 * 1024)

static void fd_plus_flush(struct fd_pred *p) {
    struct fd_exec *x = p->ex;
    if (!x->nb) return;
    int argc = x->argc + x->nb;
    char **av = (char **)fd_alloc((size_t)(argc + 1) * sizeof *av);
    for (int i = 0; i < x->argc; i++) av[i] = x->argv[i];
    for (int i = 0; i < x->nb; i++) av[x->argc + i] = x->batch[i];
    av[argc] = 0;
    if (!fd_run(x, av, argc, x->dir ? x->bdir : 0, av[0])) fd_status = 1;
    for (int i = 0; i < x->nb; i++) free(x->batch[i]);
    free(av);
    x->nb = 0;
    x->bytes = 0;
    free(x->bdir);
    x->bdir = 0;
}

/* the directory that holds PATH, for -execdir at a starting point */
static char *fd_dirpart(const char *path) {
    size_t n = strlen(path);
    size_t e = n;
    while (e > 0 && path[e - 1] == '/') e--;
    if (e == 0) return fd_strdup("/");
    while (e > 0 && path[e - 1] != '/') e--;
    while (e > 1 && path[e - 1] == '/') e--;
    if (e == 0) return fd_strdup(".");
    char *d = (char *)fd_alloc(e + 1);
    memcpy(d, path, e);
    d[e] = 0;
    return d;
}

static int fd_exec_one(struct fd_pred *p, struct fd_file *f) {
    struct fd_exec *x = p->ex;
    char *arg = 0;              /* what {} stands for */
    char *dir = 0;
    if (x->dir) {
        char *b = fd_pct_f(f->path);
        if (!strcmp(b, "/")) arg = b;
        else {
            arg = (char *)fd_alloc(strlen(b) + 3);
            sprintf(arg, "./%s", b);
            free(b);
        }
        dir = f->dir ? fd_strdup(f->dir) : fd_dirpart(f->path);
    } else
        arg = fd_strdup(f->path);
    if (x->plus) {
        if (x->dir && x->bdir && strcmp(x->bdir, dir)) fd_plus_flush(p);
        size_t fixed = 0;
        for (int i = 0; i < x->argc; i++) fixed += strlen(x->argv[i]) + 1 + sizeof(char *);
        size_t need = strlen(arg) + 1 + sizeof(char *);
        if (x->nb && fixed + x->bytes + need > FD_ARG_MAX) fd_plus_flush(p);
        if (x->nb == x->cap) {
            x->cap = x->cap ? x->cap * 2 : 64;
            x->batch = (char **)fd_realloc(x->batch, (size_t)x->cap * sizeof *x->batch);
        }
        x->batch[x->nb++] = arg;
        x->bytes += need;
        if (x->dir && !x->bdir) x->bdir = dir;
        else free(dir);
        return 1;
    }
    char **av = (char **)fd_alloc((size_t)(x->argc + 1) * sizeof *av);
    for (int i = 0; i < x->argc; i++) av[i] = fd_subst(x->argv[i], arg);
    av[x->argc] = 0;
    int ok = fd_run(x, av, x->argc, dir, f->path);
    for (int i = 0; i < x->argc; i++) free(av[i]);
    free(av);
    free(arg);
    free(dir);
    return ok;
}

/* ---- evaluating the expression ---------------------------------------------- */

static int fd_cmp(int how, uintmax_t v, uintmax_t n) {
    return how == FD_GT ? v > n : how == FD_LT ? v < n : v == n;
}

static int fd_eval(struct fd_pred *p, struct fd_file *f);

static int fd_eval1(struct fd_pred *p, struct fd_file *f) {
    switch (p->kind) {
    case PK_AND: {
        int v = fd_eval(p->l, f);
        if (!v || fd_quitting) return v;
        return fd_eval(p->r, f);
    }
    case PK_OR: {
        int v = fd_eval(p->l, f);
        if (v || fd_quitting) return v;
        return fd_eval(p->r, f);
    }
    case PK_NOT: return !fd_eval(p->l, f);
    case PK_COMMA:
        fd_eval(p->l, f);
        if (fd_quitting) return 0;
        return fd_eval(p->r, f);
    case PK_TRUE: return 1;
    case PK_FALSE: return 0;
    case PK_NAME: {
        char *b = fd_basename(f->path);
        int r = fd_fnmatch(p->s, b, p->fold);
        free(b);
        return r;
    }
    case PK_PATH: return fd_fnmatch(p->s, f->path, p->fold);
    case PK_REGEX: return fd_re_full(&p->re, f->path, p->fold);
    case PK_LNAME: {
        if (!fd_need_stat(f)) return 0;
        if (!S_ISLNK(f->st.st_mode) || f->followed) return 0;
        struct fd_buf l = { 0, 0, 0 };
        if (fd_read_link(f->path, &l) != 0) return 0;
        int r = fd_fnmatch(p->s, l.s ? l.s : "", p->fold);
        free(l.s);
        return r;
    }
    case PK_TYPE: {
        int t = fd_type_of(f);
        return t && (p->types & fd_type_bit(t)) != 0;
    }
    case PK_XTYPE: {
        if (!fd_need_stat(f)) return 0;
        int t = fd_mode_type(f->st.st_mode);
        if (fd_follows(f)) {
            /* a link that was followed is 'l' here */
            if (f->islink) t = 'l';
        } else if (t == 'l') {
            struct stat ts;
            if (stat(f->path, &ts) == 0) t = fd_mode_type(ts.st_mode);
            else if ((errno == ENOENT || errno == ENOTDIR) && (fdg.debug & DB_STAT))
                fprintf(stderr, "fallback_stat(): stat(%s) failed; falling back on lstat()\n",
                        fd_accpath(f));
        }
        return (p->types & fd_type_bit(t)) != 0;
    }
    case PK_PERM: {
        if (!fd_need_stat(f)) return 0;
        mode_t m = f->st.st_mode & 07777;
        mode_t want = S_ISDIR(f->st.st_mode) ? p->mode_d : p->mode_f;
        if (p->perm_how == '-') return (m & want) == want;
        if (p->perm_how == '/') return want == 0 || (m & want) != 0;
        return m == want;
    }
    case PK_SIZE: {
        if (!fd_need_stat(f)) return 0;
        uintmax_t sz = (uintmax_t)f->st.st_size;
        uintmax_t units = sz / p->unit + (sz % p->unit != 0);
        return fd_cmp(p->cmp, units, p->u);
    }
    case PK_EMPTY: {
        if (!fd_need_stat(f)) return 0;
        if (S_ISREG(f->st.st_mode)) return f->st.st_size == 0;
        if (!S_ISDIR(f->st.st_mode)) return 0;
        DIR *d = opendir(f->path);
        if (!d) {
            su_error(0, errno, "%s", fd_q(f->path));
            fd_status = 1;
            return 0;
        }
        struct dirent *de;
        int empty = 1;
        while ((de = readdir(d))) {
            if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) { empty = 0; break; }
        }
        closedir(d);
        return empty;
    }
    case PK_TIME: {
        if (!fd_need_stat(f)) return 0;
        fd_i128 t = fd_stat_time(&f->st, p->tx);
        fd_i128 win = (fd_i128)(p->days ? FD_DAY : 60) * FD_NS;
        if (p->cmp == FD_GT) return t < p->ref;
        if (p->cmp == FD_LT) return p->days ? t > p->ref + win - FD_NS : t > p->ref;
        return t >= p->ref && t < p->ref + win;
    }
    case PK_USED: {
        if (!fd_need_stat(f)) return 0;
        /* measured: a file read before its status changed never matches;
         * N is [N-1, N) days after the change */
        fd_i128 delta = FD_ATIM(&f->st) - FD_CTIM(&f->st);
        if (delta < 0) return 0;
        if (p->cmp == FD_GT) return delta > p->t;
        if (p->cmp == FD_LT) return delta < p->t;
        return delta >= p->t - (fd_i128)FD_DAY * FD_NS && delta < p->t;
    }
    case PK_NEWER:
        if (!fd_need_stat(f)) return 0;
        return fd_stat_time(&f->st, p->tx) > p->ref;
    case PK_UID:
        if (!fd_need_stat(f)) return 0;
        return fd_cmp(p->cmp, (uintmax_t)f->st.st_uid, p->u);
    case PK_GID:
        if (!fd_need_stat(f)) return 0;
        return fd_cmp(p->cmp, (uintmax_t)f->st.st_gid, p->u);
    case PK_NOUSER:
        if (!fd_need_stat(f)) return 0;
        return fd_id_name("/etc/passwd", (unsigned long)f->st.st_uid, &fd_unames) == 0;
    case PK_NOGROUP:
        if (!fd_need_stat(f)) return 0;
        return fd_id_name("/etc/group", (unsigned long)f->st.st_gid, &fd_gnames) == 0;
    case PK_LINKS:
        if (!fd_need_stat(f)) return 0;
        return fd_cmp(p->cmp, (uintmax_t)f->st.st_nlink, p->u);
    case PK_INUM:
        /* measured: readdir's inode number is enough, no stat */
        if (!f->have && f->dino && !(f->islink && fd_follows(f)))
            return fd_cmp(p->cmp, (uintmax_t)f->dino, p->u);
        if (!fd_need_stat(f)) return 0;
        return fd_cmp(p->cmp, (uintmax_t)f->st.st_ino, p->u);
    case PK_SAMEFILE:
        if (!fd_need_stat(f)) return 0;
        return f->st.st_dev == p->dev && f->st.st_ino == p->ino;
    case PK_ACCESS:
        return access(f->path, p->amode) == 0;
    case PK_FSTYPE:
        if (!fd_need_stat(f)) return 0;
        return !strcmp(fd_fstype_of(f->st.st_dev), p->s);
    case PK_PRINT:
        if (p->print0) { fd_write(p->out, f->path, strlen(f->path)); fd_write(p->out, "", 1); }
        else { fd_write_name(p->out, f->path); fd_write(p->out, "\n", 1); }
        return 1;
    case PK_PRINTF:
        if (p->need == NEED_STAT && !fd_need_stat(f)) return 0;
        if (p->need == NEED_TYPE && !fd_type_of(f)) return 0;
        return fd_printf_run(p, f);
    case PK_LS:
        if (!fd_need_stat(f)) return 0;
        fd_ls(p->out, f);
        return 1;
    case PK_EXEC:
        return fd_exec_one(p, f);
    case PK_DELETE: {
        /* "." itself is never removed (and that is not an error) */
        if (!strcmp(f->path, ".")) return 1;
        int t = fd_type_of(f);
        int r = t == 'd' ? rmdir(f->path) : unlink(f->path);
        if (r != 0) {
            if (fdg.ignore_race && errno == ENOENT) return 1;
            su_error(0, errno, "cannot delete %s", fd_q(f->path));
            fd_status = 1;
            return 0;
        }
        return 1;
    }
    case PK_PRUNE:
        /* measured: -prune examines the file (a failure is said, and
         * -prune is then false) -- unless -depth was already given when it
         * was read: then it does nothing at all */
        if (p->follow) return 1;
        if (!fd_need_stat(f)) return 0;
        if (!fdg.depth) f->prune = 1;
        return 1;
    case PK_QUIT:
        fd_quitting = 1;
        return 1;
    }
    return 0;
}

static int fd_eval(struct fd_pred *p, struct fd_file *f) {
    int r = fd_eval1(p, f);
    p->evals++;
    if (r) p->succ++;
    return r;
}

/* ---- the tree --------------------------------------------------------------- */

static struct fd_pred *fd_cur;
static int fd_has_action;

static void fd_b_need(struct fd_pred *ctx) {
    if (!fd_cur) {
        if (ctx && ctx->ptype == PT_OPEN)
            su_error(1, 0, "invalid expression; expected to find a ')' but didn't see one. "
                     "Perhaps you need an extra predicate after '%s'", ctx->name);
        if (fd_has_action) su_error(1, 0, "invalid expression");
        su_error(1, 0, "expected an expression after '%s'", ctx ? ctx->name : "");
    }
    if (fd_cur->ptype == PT_CLOSE) {
        if (ctx && ctx->ptype == PT_OPEN)
            su_error(1, 0, "invalid expression; empty parentheses are not allowed.");
        if (ctx) su_error(1, 0, "expected an expression between '%s' and ')'", ctx->name);
        su_error(1, 0, "invalid expression: expected expression before closing parentheses ')'.");
    }
    if (fd_cur->ptype == PT_BI)
        su_error(1, 0, "invalid expression; you have used a binary operator '%s' with nothing "
                 "before it.", fd_cur->name);
}

static struct fd_pred *fd_b_expr(struct fd_pred *ctx);

static struct fd_pred *fd_b_unary(struct fd_pred *ctx) {
    fd_b_need(ctx);
    struct fd_pred *t = fd_cur;
    fd_cur = fd_cur->next;
    if (t->ptype == PT_UNI) {
        t->l = fd_b_unary(t);
        return t;
    }
    if (t->ptype == PT_OPEN) {
        struct fd_pred *e = fd_b_expr(t);
        if (!fd_cur || fd_cur->ptype != PT_CLOSE)
            su_error(1, 0, "invalid expression; I was expecting to find a ')' somewhere but did not see one.");
        fd_cur = fd_cur->next;
        return e;
    }
    return t;
}

static struct fd_pred *fd_b_and(struct fd_pred *ctx) {
    struct fd_pred *l = fd_b_unary(ctx);
    while (fd_cur && fd_cur->kind == PK_AND) {
        struct fd_pred *op = fd_cur;
        fd_cur = fd_cur->next;
        op->l = l;
        op->r = fd_b_unary(op);
        l = op;
    }
    return l;
}

static struct fd_pred *fd_b_or(struct fd_pred *ctx) {
    struct fd_pred *l = fd_b_and(ctx);
    while (fd_cur && fd_cur->kind == PK_OR) {
        struct fd_pred *op = fd_cur;
        fd_cur = fd_cur->next;
        op->l = l;
        op->r = fd_b_and(op);
        l = op;
    }
    return l;
}

static struct fd_pred *fd_b_expr(struct fd_pred *ctx) {
    struct fd_pred *l = fd_b_or(ctx);
    while (fd_cur && fd_cur->kind == PK_COMMA) {
        struct fd_pred *op = fd_cur;
        fd_cur = fd_cur->next;
        op->l = l;
        op->r = fd_b_or(op);
        l = op;
    }
    return l;
}

static int fd_side(const struct fd_pred *p) {
    if (!p) return 0;
    return p->side_effects || fd_side(p->l) || fd_side(p->r);
}

/* the side effects GNU's -O1 sees in an operand when it decides what may
 * move past it: measured, a subtree whose own chain had its last run of
 * side-effect-free operands reordered no longer counts ("( -print -a
 * -nouser -a -name x ) -o -depth" runs -depth first) */
static int fd_vis_side(const struct fd_pred *p) {
    if (!p) return 0;
    if (p->ptype == PT_PRIMARY) return p->side_effects;
    if (p->ptype == PT_UNI) return fd_side(p->l);     /* a ! still holds its own back */
    if (p->lost_side) return 0;
    return fd_vis_side(p->l) || fd_vis_side(p->r);
}

/* the estimate GNU's -O1 orders the name-only tests by */
static int fd_cheap_rate(const struct fd_pred *p) {
    return p->kind == PK_FALSE ? 0 : p->kind == PK_TRUE ? 2 : 1;
}

/* does -O1 change the order of the run Q[0..N) (no side effects)? the
 * name-only tests go first; two of them are sorted by their estimates
 * (ascending under -a, descending under -o, ties swapped), three or more
 * always move */
static int fd_run_changes(struct fd_pred **q, int n, int kind) {
    int ncheap = 0, seen_other = 0, moved = 0;
    struct fd_pred *c[2] = { 0, 0 };
    for (int i = 0; i < n; i++) {
        int cheap = q[i]->ptype == PT_PRIMARY && q[i]->cheap;
        if (cheap) {
            if (seen_other) moved = 1;
            if (ncheap < 2) c[ncheap] = q[i];
            ncheap++;
        } else
            seen_other = 1;
    }
    if (moved || ncheap >= 3) return 1;
    if (ncheap == 2) {
        int r0 = fd_cheap_rate(c[0]), r1 = fd_cheap_rate(c[1]);
        return kind == PK_AND ? !(r0 < r1) : !(r0 > r1);
    }
    return 0;
}

/* -O: reorder each -a or -o chain (followed down its left side, as GNU's
 * normalisation makes one) so that, between operands with side effects,
 * the tests that need only the name come first (at -O2 and up, then the
 * ones that need only the type); the rest keep their order. */
static struct fd_pred *fd_opt(struct fd_pred *t) {
    if (!t) return t;
    if (t->kind == PK_NOT) { t->l = fd_opt(t->l); return t; }
    if (t->kind == PK_COMMA) { t->l = fd_opt(t->l); t->r = fd_opt(t->r); return t; }
    if (t->kind != PK_AND && t->kind != PK_OR) return t;
    int kind = t->kind;
    /* the chain: ops[0] is the top, operands left to right */
    int n = 0, cap = 8;
    struct fd_pred **ops = (struct fd_pred **)fd_alloc((size_t)cap * sizeof *ops);
    struct fd_pred **opd = (struct fd_pred **)fd_alloc((size_t)(cap + 1) * sizeof *opd);
    struct fd_pred *c = t;
    while (c->kind == kind && c->ptype == PT_BI) {
        if (n == cap) {
            cap *= 2;
            ops = (struct fd_pred **)fd_realloc(ops, (size_t)cap * sizeof *ops);
            opd = (struct fd_pred **)fd_realloc(opd, (size_t)(cap + 1) * sizeof *opd);
        }
        ops[n++] = c;
        c = c->l;
    }
    /* operands: c, then the right sides from the bottom op up */
    int m = 0;
    opd[m++] = c;
    for (int i = n - 1; i >= 0; i--) opd[m++] = ops[i]->r;
    for (int i = 0; i < m; i++) opd[i] = fd_opt(opd[i]);
    struct fd_pred **out = (struct fd_pred **)fd_alloc((size_t)m * sizeof *out);
    int o = 0;
    int last_changed = 0;
    for (int s = 0; s < m;) {
        int e = s;
        while (e < m && !fd_vis_side(opd[e])) e++;
        last_changed = e == m ? fd_run_changes(opd + s, e - s, kind) : 0;
        /* opd[s..e) has no side effects */
        for (int pass = 0; pass < 3; pass++)
            for (int i = s; i < e; i++) {
                struct fd_pred *q = opd[i];
                int cls = (q->ptype == PT_PRIMARY && q->cheap) ? 0 :
                          (fdg.olevel >= 2 && q->ptype == PT_PRIMARY && q->need == NEED_TYPE) ? 1 : 2;
                if (cls == pass) out[o++] = q;
            }
        if (e < m) out[o++] = opd[e++];
        s = e;
    }
    /* rebuild, left-leaning, with the same operator nodes */
    struct fd_pred *acc = out[0];
    for (int i = 1; i < m; i++) {
        struct fd_pred *op = ops[n - i];
        op->l = acc;
        op->r = out[i];
        acc = op;
    }
    if (acc->ptype == PT_BI) acc->lost_side = last_changed;
    free(ops);
    free(opd);
    free(out);
    return acc;
}

/* ---- -D's dumps ----------------------------------------------------------------
 *
 * tree, opt and rates print the predicate list and trees in GNU's shape;
 * GNU's cost model (its estimated success rates and their arithmetic) is
 * its own, and here every estimate is printed as 1. */
static const char *fd_ptype_name(int t) {
    return t == PT_PRIMARY ? "primary" : t == PT_UNI ? "uni_op" : t == PT_BI ? "bi_op" :
           t == PT_OPEN ? "open_paren" : "close_paren";
}
static const char *fd_prec_name(int p) {
    return p == PR_COMMA ? "comma" : p == PR_OR ? "or" : p == PR_AND ? "and" : p == PR_NEGATE ? "negate" : "no";
}

static void fd_dump(const struct fd_pred *p, int ind, const char *cost) {
    for (int i = 0; i < ind; i++) fputc(' ', stderr);
    fprintf(stderr, "pred=[%s] type=%s prec=%s cost=%s est_success_rate=1.000 %s\n", p->dname,
            fd_ptype_name(p->ptype), fd_prec_name(p->prec), cost, fd_side(p) ? "side effects" : "no side effects");
    if (p->ptype == PT_PRIMARY) {
        for (int i = 0; i < ind; i++) fputc(' ', stderr);
        fputs("no children.\n", stderr);
        return;
    }
    if (p->l && p->ptype == PT_BI) {
        for (int i = 0; i < ind; i++) fputc(' ', stderr);
        fputs("left:\n", stderr);
        fd_dump(p->l, ind + 4, cost);
    } else {
        for (int i = 0; i < ind; i++) fputc(' ', stderr);
        fputs("no left.\n", stderr);
    }
    const struct fd_pred *r = p->ptype == PT_UNI ? p->l : p->r;
    if (r) {
        for (int i = 0; i < ind; i++) fputc(' ', stderr);
        fputs("right:\n", stderr);
        fd_dump(r, ind + 4, cost);
    }
}

static void fd_cmdline(const struct fd_pred *p, int rates) {
    if (!p) return;
    int bin = p->ptype == PT_BI, uni = p->ptype == PT_UNI;
    if (bin || uni) fputs(" ( ", stderr);
    if (bin) fd_cmdline(p->l, rates);
    if (p->ptype == PT_PRIMARY) {
        if (p->need == NEED_STAT) fputs("[call stat] ", stderr);
        if (p->need >= NEED_TYPE && p->kind != PK_TYPE) fputs("[need type] ", stderr);
        else if (p->kind == PK_TYPE) fputs("[need type] ", stderr);
    }
    fprintf(stderr, "%s [est success rate 1]", p->dname);
    if (rates) {
        if (p->evals) {
            fprintf(stderr, " [real success rate %lu/%lu=", p->succ, p->evals);
            struct fd_buf b = { 0, 0, 0 };
            fd_fmt_g(&b, p->succ, p->evals);
            fprintf(stderr, "%s]", b.s);
            free(b.s);
        } else fputs(" [real success rate 0/0=_]", stderr);
    }
    fputc(' ', stderr);
    if (bin) fd_cmdline(p->r, rates);
    if (uni) fd_cmdline(p->l, rates);
    if (bin || uni) fputs(" ) ", stderr);
}

/* ---- the walk ----------------------------------------------------------------- */

struct fd_anc { dev_t dev; ino_t ino; const char *path; const struct fd_anc *up; };

static int fd_prev_level = INT_MIN;     /* -D search's prev_depth */

/* -D search: what fts said of the entry, before and after find looked */
static void fd_search_early(const struct fd_file *f, const char *info) {
    if (!(fdg.debug & DB_SEARCH)) return;
    fprintf(stderr, "consider_visiting (early): %s: fts_info=%-6s, fts_level=%2d, prev_depth=%d "
            "fts_path=%s, fts_accpath=%s\n", fd_q(f->path), info, f->depth, fd_prev_level,
            fd_q(f->path), fd_q(fd_accpath(f)));
    fd_prev_level = f->depth;
}

static void fd_search_late(const struct fd_file *f, const char *info, int isdir, int ignore) {
    if (!(fdg.debug & DB_SEARCH)) return;
    fprintf(stderr, "consider_visiting (late): %s: fts_info=%-6s, isdir=%d ignore=%d have_stat=%d "
            "have_type=%d \n", fd_q(f->path), info, isdir, ignore, f->have,
            f->have || (f->dtype >= 0 && f->dtype != DT_UNKNOWN));
}

static const char *fd_info_of(const struct fd_file *f) {
    if (!f->have) return f->stat_err ? "FTS_NS" : "FTS_NSOK";
    mode_t m = f->st.st_mode;
    if (S_ISREG(m)) return "FTS_F";
    if (S_ISLNK(m)) return fd_follows(f) ? "FTS_SLNONE" : "FTS_SL";
    return "FTS_DEFAULT";
}

static void fd_visit(struct fd_file *f) {
    if (f->depth < fdg.mindepth) return;
    fd_eval(fdg.tree, f);
}

static void fd_walk(struct fd_file *f, const struct fd_anc *anc, dev_t rootdev);

/* a directory's entries, whole, in readdir's order */
struct fd_dent { char *name; int type; ino_t ino; };

static int fd_read_dir(const char *path, struct fd_dent **out, int *n) {
    DIR *d = opendir(path);
    if (!d) return errno ? errno : EACCES;
    struct dirent *de;
    int cap = 0;
    *n = 0;
    *out = 0;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (*n == cap) {
            cap = cap ? cap * 2 : 32;
            *out = (struct fd_dent *)fd_realloc(*out, (size_t)cap * sizeof **out);
        }
        (*out)[*n].name = fd_strdup(de->d_name);
        (*out)[*n].type = de->d_type;
        (*out)[*n].ino = de->d_ino;
        (*n)++;
    }
    closedir(d);
    return 0;
}

/* a directory's visit: before its entries, or after with -depth; one that
 * cannot be read is reported after the first visit */
static void fd_walk_dir(struct fd_file *f, const struct fd_anc *anc, dev_t rootdev) {
    int pre_ignore = fdg.depth || f->depth < fdg.mindepth;
    int post_ignore = !fdg.depth || f->depth < fdg.mindepth;
    fd_search_early(f, "FTS_D");
    fd_search_late(f, "FTS_D", 1, pre_ignore);
    if (!fdg.depth) {
        fd_visit(f);
        if (fd_quitting) return;
    }
    int descend = !f->prune && (fdg.maxdepth < 0 || f->depth < fdg.maxdepth) &&
                  !(fdg.xdev && f->st.st_dev != rootdev);
    if (descend) {
        struct fd_dent *ents;
        int n;
        int err = fd_read_dir(f->path, &ents, &n);
        if (err) {
            fd_search_early(f, "FTS_DNR");
            su_error(0, err, "%s", fd_q(f->path));
            fd_status = 1;
            if (post_ignore) return;
            fd_search_late(f, "FTS_DNR", 1, 0);
            fd_visit(f);
            return;
        }
        struct fd_anc me = { f->st.st_dev, f->st.st_ino, f->path, anc };
        size_t pl = strlen(f->path);
        int slash = pl && f->path[pl - 1] == '/';
        for (int i = 0; i < n; i++) {
            if (fd_quitting) { free(ents[i].name); continue; }
            size_t nl = strlen(ents[i].name);
            char *cp = (char *)fd_alloc(pl + nl + 2);
            memcpy(cp, f->path, pl);
            size_t k = pl;
            if (!slash) cp[k++] = '/';
            memcpy(cp + k, ents[i].name, nl + 1);
            struct fd_file c;
            memset(&c, 0, sizeof c);
            c.path = cp;
            c.root = f->root;
            c.depth = f->depth + 1;
            c.dtype = ents[i].type;
            c.dino = ents[i].ino;
            c.dir = f->path;
            fd_walk(&c, &me, rootdev);
            free(cp);
            free(ents[i].name);
        }
        free(ents);
    }
    if (fd_quitting) return;
    fd_search_early(f, "FTS_DP");
    fd_search_late(f, "FTS_DP", 1, post_ignore);
    if (fdg.depth) fd_visit(f);
}

/* an entry the walk found (DEPTH > 0) or a starting point (already stat'ed) */
static void fd_walk(struct fd_file *f, const struct fd_anc *anc, dev_t rootdev) {
    int err = 0;
    if (f->depth > 0) {
        int t = f->dtype;
        if (t == DT_LNK) f->islink = 1;
        /* fts examines a directory, an entry readdir could not type, and
         * (with -L) a link; the rest only when a test asks */
        if (t == DT_DIR || t == DT_UNKNOWN || (t == DT_LNK && fdg.follow == 2)) {
            if (fdg.follow == 2 && t == DT_UNKNOWN) {
                struct stat ls;
                if (lstat(f->path, &ls) == 0 && S_ISLNK(ls.st_mode)) f->islink = 1;
            }
            int r;
            if (fdg.follow == 2) {
                r = stat(f->path, &f->st);
                if (r == 0) f->followed = f->islink;
                else if (errno == ENOENT || errno == ENOTDIR) {
                    r = lstat(f->path, &f->st);
                    if (r == 0 && (fdg.debug & DB_STAT)) {
                        /* find looks again at a dangling link itself */
                        fprintf(stderr, "debug_stat (%s)\n", f->path);
                        fprintf(stderr, "fallback_stat(): stat(%s) failed; falling back on lstat()\n", f->path);
                    }
                }
            } else
                r = lstat(f->path, &f->st);
            if (r == 0) {
                f->have = 1;
                if (S_ISLNK(f->st.st_mode)) f->islink = 1;
            } else {
                err = errno ? errno : EIO;
                if (fdg.ignore_race && err == ENOENT) return;
                f->stat_err = err;
                f->dtype = DT_UNKNOWN;     /* nothing is known of it now */
            }
        }
    }
    if (err) {
        fd_search_early(f, "FTS_NS");
        if (fdg.debug & DB_STAT) fprintf(stderr, "debug_stat (%s)\n", fd_accpath(f));
        su_error(0, err, "%s", fd_q(f->path));
        fd_status = 1;
        f->err_said = 1;
        if (err == ELOOP) return;
        fd_search_late(f, "FTS_NS", 0, f->depth < fdg.mindepth);
        fd_visit(f);
        return;
    }
    if (f->have && S_ISDIR(f->st.st_mode)) {
        /* a directory that is its own ancestor (only links can make one) */
        if (f->st.st_ino) {
            for (const struct fd_anc *a = anc; a; a = a->up) {
                if (a->dev == f->st.st_dev && a->ino == f->st.st_ino) {
                    fd_search_early(f, "FTS_DC");
                    su_error(0, 0, "File system loop detected; %s is part of the same file system "
                             "loop as %s.", fd_q(f->path), fd_q(a->path));
                    fd_status = 1;
                    return;
                }
            }
        }
        fd_walk_dir(f, anc, rootdev);
        return;
    }
    const char *info = fd_info_of(f);
    fd_search_early(f, info);
    fd_search_late(f, info, 0, f->depth < fdg.mindepth);
    fd_visit(f);
}

/* a starting point */
static void fd_root(const char *path) {
    struct fd_file f;
    memset(&f, 0, sizeof f);
    f.path = path;
    f.root = path;
    f.dtype = -1;
    if (!*path) {
        su_error(0, ENOENT, "%s", fd_q(path));
        fd_status = 1;
        return;
    }
    int r = lstat(path, &f.st);
    if (r == 0 && S_ISLNK(f.st.st_mode)) {
        f.islink = 1;
        if (fdg.follow) {
            struct stat ts;
            if (stat(path, &ts) == 0) { f.st = ts; f.followed = 1; }
            else if (errno != ENOENT && errno != ENOTDIR) r = -1;
            else if (fdg.debug & DB_STAT) {
                fprintf(stderr, "debug_stat (%s)\n", path);
                fprintf(stderr, "fallback_stat(): stat(%s) failed; falling back on lstat()\n", path);
            }
        }
    }
    if (r != 0) {
        int e = errno;
        if (!(fdg.ignore_race && e == ENOENT)) {
            fd_search_early(&f, "FTS_NS");
            su_error(0, e, "%s", fd_q(path));
            fd_status = 1;
        }
        return;
    }
    f.have = 1;
    fd_walk(&f, 0, f.st.st_dev);
}

/* ---- usage ---------------------------------------------------------------------- */

static void fd_usage(void) {
    printf("Usage: %s [-H] [-L] [-P] [-Olevel] [-D debugopts] [path...] [expression]\n", su_name);
    fputs("\n"
          "Search the directory trees at each PATH (by default the current directory) for files\n"
          "the EXPRESSION is true of; with no action in it, print their names.\n"
          "\n"
          "Operators, binding tightest first (two expressions side by side are joined by -a):\n"
          "      ( EXPR )   ! EXPR   -not EXPR   EXPR1 -a EXPR2   EXPR1 -and EXPR2\n"
          "      EXPR1 -o EXPR2   EXPR1 -or EXPR2   EXPR1 , EXPR2\n"
          "\n"
          "Positional options (true; they change only what comes after them):\n"
          "      -daystart -follow -nowarn -regextype -warn\n"
          "\n"
          "Global options (true; they change the whole search, so give them first):\n"
          "      -depth -files0-from FILE -maxdepth LEVELS -mindepth LEVELS\n"
          "      -mount -noleaf -xdev -ignore_readdir_race -noignore_readdir_race\n"
          "\n"
          "Tests (a number N may be written +N for more than N, -N for less):\n"
          "      -amin N -anewer FILE -atime N -cmin N -cnewer FILE -context CONTEXT\n"
          "      -ctime N -empty -false -fstype TYPE -gid N -group NAME -ilname PATTERN\n"
          "      -iname PATTERN -inum N -iwholename PATTERN -iregex PATTERN\n"
          "      -links N -lname PATTERN -mmin N -mtime N -name PATTERN -newer FILE\n"
          "      -nouser -nogroup -path PATTERN -perm [-/]MODE -regex PATTERN\n"
          "      -readable -writable -executable\n"
          "      -wholename PATTERN -size N[bcwkMG] -true -type [bcdpflsD] -uid N\n"
          "      -used N -user NAME -xtype [bcdpfls]\n"
          "\n"
          "Actions:\n"
          "      -delete -print0 -printf FORMAT -fprintf FILE FORMAT -print\n"
          "      -fprint0 FILE -fprint FILE -ls -fls FILE -prune -quit\n"
          "      -exec COMMAND ; -exec COMMAND {} + -ok COMMAND ;\n"
          "      -execdir COMMAND ; -execdir COMMAND {} + -okdir COMMAND ;\n"
          "\n"
          "Other options:\n"
          "      --help                   show this text and exit\n"
          "      --version                show the version and exit\n"
          "\n"
          "-D takes a comma-separated list of: exec, opt, rates, search, stat, time, tree,\n"
          "all, help ('-D help' says what each shows).\n", stdout);
}

static void fd_debug_help(void) {
    static const char *const h[][2] = {
        { "exec", "Report each command that -exec, -execdir, -ok or -okdir runs, and its end" },
        { "opt", "Show the expression tree as it is rearranged for speed" },
        { "rates", "Count, at the end, how often each predicate was true" },
        { "search", "Report each step of the walk through the directory trees" },
        { "stat", "Report each file that has to be examined with stat(2) or lstat(2)" },
        { "time", "Report the moment that time tests count back from" },
        { "tree", "Show the predicate list and the expression tree built from it" },
        { "all", "Turn on every one of these (except help)" },
        { "help", "Show this list" },
    };
    puts("Valid arguments for -D:");
    for (size_t i = 0; i < sizeof h / sizeof h[0]; i++) printf("%-10s %s\n", h[i][0], h[i][1]);
}

/* -D LIST */
static void fd_debug_arg(const char *arg) {
    size_t first = strcspn(arg, ",");
    char *firstname = (char *)fd_alloc(first + 1);
    memcpy(firstname, arg, first);
    firstname[first] = 0;
    int any = 0;
    for (const char *q = arg; *q;) {
        size_t n = strcspn(q, ",");
        if (n) {
            any = 1;
#define DBN(s) (n == sizeof(s) - 1 && !strncmp(q, s, n))
            if (DBN("exec")) fdg.debug |= DB_EXEC;
            else if (DBN("opt")) fdg.debug |= DB_OPT | DB_TREE;
            else if (DBN("rates")) fdg.debug |= DB_RATES;
            else if (DBN("search")) fdg.debug |= DB_SEARCH;
            else if (DBN("stat")) fdg.debug |= DB_STAT;
            else if (DBN("time")) fdg.debug |= DB_TIME;
            else if (DBN("tree")) fdg.debug |= DB_TREE;
            else if (DBN("all")) fdg.debug |= DB_EXEC | DB_OPT | DB_RATES | DB_SEARCH | DB_STAT | DB_TIME | DB_TREE;
            else if (DBN("help")) { fd_debug_help(); exit(0); }
            /* measured: the message names the list's first word, whichever is wrong */
            else su_error(0, 0, "Ignoring unrecognised debug flag %s", fd_q(firstname));
#undef DBN
        }
        q += n;
        if (*q == ',') q++;
    }
    free(firstname);
    if (!any) {
        su_error(0, 0, "Empty argument to the -D option.");
        su_try_help(1);
    }
}

/* -files0-from FILE: the starting points, NUL-separated */
static char **fd_files0(const char *name, int *count) {
    FILE *f = strcmp(name, "-") ? fopen(name, "r") : stdin;
    if (!f) su_error(1, errno, "cannot open %s for reading", fd_q(name));
    struct fd_buf b = { 0, 0, 0 };
    char buf[4096];
    for (;;) {
        size_t n = fread(buf, 1, sizeof buf, f);
        if (n) fd_bput(&b, buf, n);
        if (n < sizeof buf) {
            if (ferror(f)) su_error(1, errno, "%s: read error", fd_q(name));
            break;
        }
    }
    if (f != stdin) fclose(f);
    char **v = 0;
    int nv = 0;
    size_t i = 0;
    while (i < b.n) {
        size_t e = i;
        while (e < b.n && b.s[e]) e++;
        v = (char **)fd_realloc(v, (size_t)(nv + 1) * sizeof *v);
        char *s = (char *)fd_alloc(e - i + 1);
        memcpy(s, b.s + i, e - i);
        s[e - i] = 0;
        v[nv++] = s;
        i = e + 1;
    }
    free(b.s);
    *count = nv;
    return v;
}

/* ---- find ------------------------------------------------------------------------ */

static int do_find(int argc, char **argv) {
    /* GNU's messages name find as it was invoked: "/usr/bin/find: ..." */
    const char *base = su_short;
    su_short = su_name;
    fd_base = base;
    memset(&fdg, 0, sizeof fdg);
    fdg.maxdepth = -1;
    fdg.olevel = 1;
    for (int i = 0; fd_retypes[i].name; i++)
        if (!strcmp(fd_retypes[i].name, "emacs")) fdg.regex_syn = fd_retypes[i].syntax;
    fdg.posixly = getenv("POSIXLY_CORRECT") != 0;
    fdg.warn = !fdg.posixly && isatty(0);
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        fd_now_ns = fd_ns((int64_t)ts.tv_sec, ts.tv_nsec);
        fd_day_start = fd_now_ns - (fd_i128)FD_DAY * FD_NS;
    }
    static struct fd_out so, se;
    so.f = stdout; so.name = "standard output";
    se.f = stderr; se.name = "standard error";
    fdg.stdout_o = &so;
    fdg.stderr_o = &se;

    /* -H -L -P -D -O, each its own word */
    int i = 1;
    while (i < argc) {
        const char *a = argv[i];
        if (!strcmp(a, "-H")) { fdg.follow = 1; i++; continue; }
        if (!strcmp(a, "-L")) { fdg.follow = 2; i++; continue; }
        if (!strcmp(a, "-P")) { fdg.follow = 0; i++; continue; }
        if (!strcmp(a, "-D")) {
            if (i + 1 >= argc) {
                su_error(0, 0, "Missing argument after the -D option.");
                su_try_help(1);
            }
            fd_debug_arg(argv[i + 1]);
            i += 2;
            continue;
        }
        if (!strncmp(a, "-O", 2)) {
            const char *s = a + 2;
            if (!*s) su_error(1, 0, "The -O option must be immediately followed by a decimal integer");
            if (*s < '0' || *s > '9') su_error(1, 0, "Please specify a decimal number immediately after -O");
            uintmax_t v = 0;
            int over = 0;
            const char *q = s;
            for (; *q >= '0' && *q <= '9'; q++) {
                if (v > (UINTMAX_MAX - (uintmax_t)(*q - '0')) / 10) over = 1;
                else v = v * 10 + (uintmax_t)(*q - '0');
            }
            if (*q) su_error(1, 0, "Invalid optimisation level %s", s);
            if (over || v > LONG_MAX) su_error(1, ERANGE, "Invalid optimisation level %s", s);
            fdg.olevel = v == 0 ? 1 : (int)(v > 3 ? 3 : v);
            i++;
            continue;
        }
        if (!strcmp(a, "--")) { i++; break; }
        break;
    }
    if (fdg.debug & DB_TIME) {
        struct fd_buf b = { 0, 0, 0 };
        fd_ctime(&b, fd_sec_of(fd_day_start));
        fprintf(stderr, "cur_day_start = %s\n", b.s);
        free(b.s);
    }
    int first_path = i;
    while (i < argc && !(argv[i][0] == '-' && argv[i][1]) && strcmp(argv[i], "(") && strcmp(argv[i], "!"))
        i++;
    int npaths = i - first_path;
    fdg.paths = argv + first_path;
    fdg.npaths = npaths;
    fdg.tty_out = isatty(1);

    fd_av = argv;
    fd_ac = argc;
    fd_ai = i;
    while (fd_ai < fd_ac) {
        const char *a = fd_av[fd_ai++];
        fd_parse_one(a);
    }
    for (struct fd_pred *p = fdg.head; p; p = p->next) if (p->action) fd_has_action = 1;
    struct fd_pred *tree = 0;
    if (fdg.head) {
        fd_cur = fdg.head;
        tree = fd_b_expr(0);
        if (fd_cur) su_error(1, 0, "you have too many ')'");
    }
    if (fdg.debug & DB_TREE) {
        fputs("Predicate List:\n", stderr);
        if (!fd_has_action && fdg.head) fputs("[(] ", stderr);
        for (struct fd_pred *p = fdg.head; p; p = p->next) fprintf(stderr, "[%s] ", p->name);
        if (!fd_has_action) fputs(fdg.head ? "[)] [-a] [-print] " : "[-print] ", stderr);
        fputc('\n', stderr);
    }
    if (!fd_has_action) {
        struct fd_pred *pr = (struct fd_pred *)fd_alloc(sizeof *pr);
        memset(pr, 0, sizeof *pr);
        pr->kind = PK_PRINT;
        pr->ptype = PT_PRIMARY;
        pr->name = pr->dname = (char *)"-print";
        pr->side_effects = pr->action = 1;
        pr->out = fd_use(fdg.stdout_o);
        if (tree) {
            struct fd_pred *a = (struct fd_pred *)fd_alloc(sizeof *a);
            memset(a, 0, sizeof *a);
            a->kind = PK_AND;
            a->ptype = PT_BI;
            a->prec = PR_AND;
            a->name = a->dname = (char *)"-a";
            a->l = tree;
            a->r = pr;
            tree = a;
        } else
            tree = pr;
    }
    if (fdg.has_delete && fdg.has_prune && !fdg.depth_explicit)
        su_error(1, 0, "The -delete action automatically turns on -depth, but -prune does nothing "
                 "when -depth is in effect.  If you want to carry on anyway, just explicitly use "
                 "the -depth option.");
    if (fdg.files0 && !strcmp(fdg.files0, "-") && fdg.has_ok)
        su_error(1, 0, "option -files0-from reading from standard input cannot be combined with -ok, -okdir");
    if (fdg.debug & DB_TREE) {
        fputs("Eval Tree:\n", stderr);
        fd_dump(tree, 0, "Unknown");
    }
    tree = fd_opt(tree);
    if (fdg.debug & DB_TREE) {
        fputs("Optimized Eval Tree:\n", stderr);
        fd_dump(tree, 0, "Nothing");
        fputs("Optimized command line:\n", stderr);
        fd_cmdline(tree, fdg.debug & DB_RATES);
        fputc('\n', stderr);
    }
    fdg.tree = tree;

    if (fdg.files0) {
        if (npaths) {
            su_error(0, 0, "extra operand %s", fd_q(argv[first_path]));
            su_error(1, 0, "file operands cannot be combined with -files0-from");
        }
        int n;
        char **v = fd_files0(fdg.files0, &n);
        for (int k = 0; k < n && !fd_quitting; k++) {
            if (!*v[k]) {
                su_error(0, 0, "%s:%d: invalid zero-length file name", fd_q(fdg.files0), k + 1);
                fd_status = 1;
                continue;
            }
            fd_root(v[k]);
        }
    } else if (npaths == 0) {
        fd_root(".");
    } else {
        for (int k = first_path; k < first_path + npaths && !fd_quitting; k++) fd_root(argv[k]);
    }
    for (int k = 0; k < fdg.nplus; k++) fd_plus_flush(fdg.plus[k]);
    if (fdg.debug & DB_RATES) {
        fputs("Predicate success rates after completion:\n", stderr);
        fd_cmdline(tree, 1);
        fputc('\n', stderr);
    }
    /* write errors, as measured: the first stream an action used that
     * failed is named; then closing standard output says "write error"
     * (with the reason, when the close itself fails) */
    for (int k = 0; k < fdg.nused; k++) {
        struct fd_out *o = fdg.used[k];
        if (fflush(o->f) != 0 && !o->failed) o->failed = errno ? errno : EIO;
        if (o->failed || ferror(o->f)) {
            su_error(0, o->failed, "%s", fd_q(o->name));
            fd_status = 1;
            break;
        }
    }
    for (struct fd_out *o = fdg.outs; o; o = o->next) fflush(o->f);
    {
        int bad = so.failed || ferror(stdout);
        if (fflush(stdout) != 0) bad = 1;
        int ce = close(1) != 0 ? errno : 0;
        if (ce == EBADF && !bad) ce = 0;       /* nothing was written: no error */
        if (bad || ce) {
            if (ce) su_error(0, ce, "write error");
            else su_error(0, 0, "write error");
            fd_status = 1;
        }
    }
    return fd_status;
}
