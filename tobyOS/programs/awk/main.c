/* awk -- the POSIX pattern-scanning and processing language.
 *
 * WHAT THIS REPLACES, AND WHY IT HAD TO BE REPLACED.
 *
 * The previous awk here was 313 lines that matched a pattern string
 * against a line and ran a hand-interpreted action. It had no expression
 * grammar at all, which meant:
 *
 *   - no $NF (the field count could not appear in a field reference)
 *   - no ?: , no sub()/gsub(), no split(), no user functions, no arrays
 *   - a bare `$1 == "x"` PATTERN did not select the line -- it was read
 *     as an unrecognised pattern and silently matched nothing
 *
 * Every one of those produced WRONG OUTPUT rather than an error, which is
 * the worst failure mode a text tool has: gate fixtures written the
 * natural awk way had to be rewritten with sed/cut/grep to get correct
 * answers, and the note in the native-parity handoff -- "a user CAN tell
 * these from the Linux ones" -- was about exactly this.
 *
 * So this is a real awk: a lexer, a recursive-descent parser to an AST,
 * and a tree-walking interpreter. It implements the POSIX language --
 * BEGIN/END and pattern-range rules, user functions with array
 * parameters passed by reference, associative arrays, every operator
 * including concatenation and ?:, all four getline forms, print/printf
 * with the three redirections, and the full built-in function set.
 *
 * NOTES ON THINGS THAT ARE EASY TO GET WRONG, all of which are covered by
 * etc/awk_test.sh (which is diffed against real GNU awk):
 *
 *   - Comparison is POSIX's, not C's: a field that LOOKS numeric compares
 *     numerically against a number, but a string constant never does.
 *     `$1 == 10` is true for the input "10.0"; `$1 == "10"` is not.
 *   - Concatenation binds LOOSER than + and -, so `"x" - 1` is -1.
 *   - `-2^2` is -4: ^ binds tighter than unary minus, and is right
 *     associative.
 *   - Passing an untyped variable to a function that subscripts it makes
 *     it an array IN THE CALLER; passing one that the callee assigns as a
 *     scalar must not. Both are handled by aliasing the caller's cell and
 *     rolling back a scalar write on return.
 *   - libtoby's stdio is UNBUFFERED (one write(2) per fputs), so output
 *     goes through this file's own 8 KiB buffers; anything that can let
 *     another process observe the output -- system(), close(), fflush(),
 *     a pipe redirection, exit -- flushes first.
 *
 * Regexes come from libtoby's engine, compiled with REG_ESCBRACKET
 * because awk (unlike sed and grep) processes backslash escapes inside a
 * bracket expression -- see the note on that flag in regex.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <regex.h>
#include <unistd.h>
#include <errno.h>

/* ================================================================== */
/* diagnostics                                                        */
/* ================================================================== */

static const char *g_progname = "awk";
static int   g_srcline = 1;
static void  flush_all(void);

static void fatal(const char *fmt, ...) __attribute__((noreturn));

#include <stdarg.h>
static void fatal(const char *fmt, ...) {
    va_list ap;
    flush_all();
    fprintf(stderr, "%s: ", g_progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static void synerr(const char *msg) {
    fatal("syntax error at source line %d: %s", g_srcline, msg);
}

static void *emalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) fatal("out of memory");
    return p;
}
static void *erealloc(void *q, size_t n) {
    void *p = realloc(q, n ? n : 1);
    if (!p) fatal("out of memory");
    return p;
}
static char *estrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)emalloc(n);
    memcpy(p, s, n);
    return p;
}
static char *estrndup(const char *s, size_t n) {
    char *p = (char *)emalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ================================================================== */
/* values                                                             */
/* ================================================================== */

#define C_NUM    0x01   /* num is valid */
#define C_STR    0x02   /* str is valid */
#define C_STRNUM 0x04   /* came from input and looks numeric */
/* str holds a CACHED CONVERSION of num, not a string anybody assigned.
 *
 * Without this distinction, merely printing or concatenating a number
 * turned it into a string for every later comparison: `x=2; s=x ""` then
 * `x < 10` compared "2" against "10" and answered false. The cache has to
 * be invisible to the type rules, so it gets its own bit and C_STR keeps
 * its original meaning of "this value IS a string". */
#define C_STRCACHE 0x20
#define C_ARR    0x08
#define C_UNINIT 0x10

typedef struct Map Map;

typedef struct Cell {
    unsigned  flags;
    double    num;
    char     *str;
    Map      *arr;
} Cell;

typedef struct MapEnt {
    char          *key;
    Cell          *val;
    struct MapEnt *next;
} MapEnt;

struct Map {
    MapEnt **b;
    int      nb;
    int      n;
};

static unsigned hashstr(const char *s) {
    unsigned h = 5381;
    while (*s) h = h * 33u + (unsigned char)*s++;
    return h;
}

static Map *map_new(void) {
    Map *m = (Map *)emalloc(sizeof *m);
    m->nb = 31;
    m->n = 0;
    m->b = (MapEnt **)emalloc(sizeof(MapEnt *) * (size_t)m->nb);
    memset(m->b, 0, sizeof(MapEnt *) * (size_t)m->nb);
    return m;
}

static Cell *cell_new(void) {
    Cell *c = (Cell *)emalloc(sizeof *c);
    c->flags = C_UNINIT;
    c->num = 0;
    c->str = 0;
    c->arr = 0;
    return c;
}

static void map_grow(Map *m) {
    int nb = m->nb * 2 + 1;
    MapEnt **b = (MapEnt **)emalloc(sizeof(MapEnt *) * (size_t)nb);
    memset(b, 0, sizeof(MapEnt *) * (size_t)nb);
    for (int i = 0; i < m->nb; i++) {
        MapEnt *e = m->b[i];
        while (e) {
            MapEnt *nx = e->next;
            unsigned h = hashstr(e->key) % (unsigned)nb;
            e->next = b[h];
            b[h] = e;
            e = nx;
        }
    }
    free(m->b);
    m->b = b;
    m->nb = nb;
}

static Cell *map_find(Map *m, const char *k) {
    unsigned h = hashstr(k) % (unsigned)m->nb;
    for (MapEnt *e = m->b[h]; e; e = e->next)
        if (strcmp(e->key, k) == 0) return e->val;
    return 0;
}

static Cell *map_get(Map *m, const char *k) {
    Cell *c = map_find(m, k);
    if (c) return c;
    if (m->n > m->nb * 2) map_grow(m);
    unsigned h = hashstr(k) % (unsigned)m->nb;
    MapEnt *e = (MapEnt *)emalloc(sizeof *e);
    e->key = estrdup(k);
    e->val = cell_new();
    e->next = m->b[h];
    m->b[h] = e;
    m->n++;
    return e->val;
}

static void cell_release(Cell *c);

static void map_clear(Map *m) {
    for (int i = 0; i < m->nb; i++) {
        MapEnt *e = m->b[i];
        while (e) {
            MapEnt *nx = e->next;
            cell_release(e->val);
            free(e->val);
            free(e->key);
            free(e);
            e = nx;
        }
        m->b[i] = 0;
    }
    m->n = 0;
}

static void map_del(Map *m, const char *k) {
    unsigned h = hashstr(k) % (unsigned)m->nb;
    MapEnt **pp = &m->b[h];
    while (*pp) {
        if (strcmp((*pp)->key, k) == 0) {
            MapEnt *e = *pp;
            *pp = e->next;
            cell_release(e->val);
            free(e->val);
            free(e->key);
            free(e);
            m->n--;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void cell_release(Cell *c) {
    if (!c) return;
    free(c->str);
    c->str = 0;
    if (c->arr) { map_clear(c->arr); free(c->arr->b); free(c->arr); c->arr = 0; }
    c->flags = C_UNINIT;
}

/* ================================================================== */
/* globals, special variables                                         */
/* ================================================================== */

static Map  *globals;
static Cell *V_NR, *V_NF, *V_FNR, *V_FS, *V_OFS, *V_ORS, *V_RS,
            *V_FILENAME, *V_SUBSEP, *V_RSTART, *V_RLENGTH,
            *V_CONVFMT, *V_OFMT, *V_ENVIRON, *V_ARGC, *V_ARGV;

static void setsval(Cell *c, const char *s);
static void setfval(Cell *c, double d);
static const char *getsval(Cell *c);
static double getfval(Cell *c);

/* Does this string look like a number, in full? Leading and trailing
 * blanks are allowed, nothing else -- "10 " is a strnum, "10x" is not. */
static int looks_numeric(const char *s, double *out) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (!*p) return 0;
    char *end;
    errno = 0;
    double v = strtod(p, &end);
    if (end == p) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\n') end++;
    if (*end) return 0;
    if (out) *out = v;
    return 1;
}

static double getfval(Cell *c) {
    if (!c) return 0;
    if (c->flags & C_NUM) return c->num;
    if (c->flags & (C_STR | C_STRCACHE)) {
        double v = 0;
        if (c->str) { char *e; v = strtod(c->str, &e); if (e == c->str) v = 0; }
        c->num = v;
        c->flags |= C_NUM;
        return v;
    }
    return 0;
}

/* awk prints an integral value as an integer whatever CONVFMT says --
 * `x = 1; print x ""` is "1", never "1.000000". */
static char *num_to_str(double d, const char *fmt) {
    static char buf[256];
    if (d == (double)(long long)d && d > -1e18 && d < 1e18)
        snprintf(buf, sizeof buf, "%lld", (long long)d);
    else
        snprintf(buf, sizeof buf, fmt, d);
    return buf;
}

static const char *getsval_fmt(Cell *c, const char *fmt) {
    if (!c) return "";
    if (c->flags & (C_STR | C_STRCACHE)) return c->str ? c->str : "";
    if (c->flags & C_NUM) {
        free(c->str);
        c->str = estrdup(num_to_str(c->num, fmt));
        c->flags |= C_STRCACHE;      /* a cache, NOT "this is a string" */
        return c->str;
    }
    return "";
}

static const char *getsval(Cell *c) {
    const char *cf = "%.6g";
    if (V_CONVFMT && (V_CONVFMT->flags & (C_STR | C_STRCACHE)) && V_CONVFMT->str)
        cf = V_CONVFMT->str;
    return getsval_fmt(c, cf);
}

/* `print` formats a number with OFMT, and CONVFMT is used everywhere
 * else. A number that has already been converted once carries a cached
 * string built with CONVFMT, so this must format FRESH rather than take
 * that cache -- otherwise `OFMT="%.2f"; x=3.14159; s = x ""; print x`
 * printed the CONVFMT answer. A field keeps its own text: printing $1
 * never reformats it. */
static const char *getpsval(Cell *c) {
    const char *of = "%.6g";
    if (V_OFMT && (V_OFMT->flags & (C_STR | C_STRCACHE)) && V_OFMT->str)
        of = V_OFMT->str;
    if (c && !(c->flags & (C_UNINIT | C_STR | C_STRNUM)) && (c->flags & C_NUM))
        return num_to_str(c->num, of);
    return getsval(c);
}

static void set_nf_from_assign(double d);
static void rebuild_record(void);
static void ensure_split(void);

static void setfval(Cell *c, double d) {
    if (c == V_NF) { set_nf_from_assign(d); return; }
    free(c->str);
    c->str = 0;
    c->num = d;
    c->flags = C_NUM;
}

static void setsval(Cell *c, const char *s) {
    if (c == V_NF) { double d = 0; looks_numeric(s, &d); set_nf_from_assign(d); return; }
    char *ns = estrdup(s ? s : "");
    free(c->str);
    c->str = ns;
    c->flags = C_STR;
}

/* A value that came from the INPUT (a field, a getline var, an ARGV
 * assignment, ENVIRON): it is a string, but compares numerically when it
 * looks like a number. This distinction is the whole of awk's comparison
 * rule and nothing else in the language creates it. */
static void set_input_val(Cell *c, const char *s) {
    double d;
    setsval(c, s);
    if (looks_numeric(s, &d)) {
        c->num = d;
        c->flags |= C_NUM | C_STRNUM;
    }
}

/* Is this value a STRING for the purposes of comparison and truth?
 * A cached conversion is not; a field that looks numeric is not; an
 * uninitialised value is neither string nor number and compares equal to
 * both "" and 0. */
static int cell_is_str(const Cell *c) {
    if (!c) return 0;
    if (c->flags & C_UNINIT) return 0;
    if (c->flags & C_STRNUM) return 0;
    return (c->flags & C_STR) != 0;
}

/* Copy a value, cache and all, without letting the destination inherit
 * the array-ness of the source. */
static void cell_copy(Cell *dst, const Cell *src) {
    char *ns = 0;
    if (src->str && (src->flags & (C_STR | C_STRCACHE)))
        ns = estrdup(src->str);
    free(dst->str);
    dst->str = ns;
    dst->num = src->num;
    dst->flags = src->flags & ~C_ARR;
    if (!ns) dst->flags &= ~(C_STR | C_STRCACHE);
}

static void set_uninit(Cell *c) {
    free(c->str);
    c->str = 0;
    c->num = 0;
    c->flags = C_UNINIT;
}

/* ================================================================== */
/* $0 and the fields                                                  */
/* ================================================================== */

static char  *g_record;          /* $0 */
static size_t g_reccap;
static char **g_fld;             /* $1..$NF, owned strings */
static int    g_fldcap;
static int    g_nf;
static int    g_split_done;      /* fields reflect g_record */
static int    g_rec_valid;       /* g_record reflects the fields */

static Cell *g_fldcell;          /* scratch cell handed out for $n reads */

static void set_record(const char *s) {
    size_t n = strlen(s);
    if (n + 1 > g_reccap) {
        g_reccap = n + 64;
        g_record = (char *)erealloc(g_record, g_reccap);
    }
    memcpy(g_record, s, n + 1);
    g_split_done = 0;
    g_rec_valid = 1;
}

static void fld_ensure(int n) {
    if (n < g_fldcap) return;
    int nc = n + 32;
    g_fld = (char **)erealloc(g_fld, sizeof(char *) * (size_t)nc);
    for (int i = g_fldcap; i < nc; i++) g_fld[i] = 0;
    g_fldcap = nc;
}

static void fld_set_raw(int i, const char *s, size_t len) {
    fld_ensure(i + 1);
    free(g_fld[i]);
    g_fld[i] = estrndup(s, len);
}

static regex_t *re_compile_cached(const char *pat);

/* Split `s` by `fs` into a callback-free vector. Used for both the record
 * split and split(). Returns the count; the pieces are handed to `emit`. */
static int split_string(const char *s, const char *fs,
                        void (*emit)(int idx, const char *p, size_t n, void *ud),
                        void *ud) {
    int n = 0;
    if (!*s) return 0;

    if (fs[0] == ' ' && fs[1] == 0) {
        const char *p = s;
        for (;;) {
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            if (!*p) break;
            const char *q = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            emit(n++, q, (size_t)(p - q), ud);
        }
        return n;
    }
    if (fs[0] && !fs[1]) {
        /* A single character is used LITERALLY, even a regex metachar:
         * `awk -F. '{print $1}'` splits on a dot. */
        const char *p = s;
        for (;;) {
            const char *q = strchr(p, fs[0]);
            if (!q) { emit(n++, p, strlen(p), ud); break; }
            emit(n++, p, (size_t)(q - p), ud);
            p = q + 1;
        }
        return n;
    }
    {
        regex_t *re = re_compile_cached(fs);
        const char *p = s;
        regmatch_t m;
        for (;;) {
            if (!*p) { emit(n++, p, 0, ud); break; }
            if (regexec(re, p, 1, &m, p == s ? 0 : REG_NOTBOL) != 0) {
                emit(n++, p, strlen(p), ud);
                break;
            }
            if (m.rm_eo == m.rm_so) {   /* empty separator: no progress */
                emit(n++, p, strlen(p), ud);
                break;
            }
            emit(n++, p, (size_t)m.rm_so, ud);
            p += m.rm_eo;
        }
        return n;
    }
}

static void emit_field(int idx, const char *p, size_t n, void *ud) {
    (void)ud;
    fld_set_raw(idx, p, n);
}

static void ensure_split(void) {
    if (g_split_done) return;
    for (int i = 0; i < g_fldcap; i++) { free(g_fld[i]); g_fld[i] = 0; }
    const char *fs = getsval(V_FS);
    /* In paragraph mode a newline is ALWAYS a field separator as well as
     * whatever FS says -- POSIX requires it and scripts that read stanza
     * files depend on it. */
    g_nf = split_string(g_record ? g_record : "", fs, emit_field, 0);
    g_split_done = 1;
    /* Store NF DIRECTLY. Going through setfval() would take its NF hook,
     * which calls set_nf_from_assign() -> rebuild_record() -- so merely
     * READING a field rebuilt $0 with the current OFS, and
     * `BEGIN{OFS="-"} {print $1; print $0}` printed a rewritten $0 that
     * GNU leaves alone. $0 is rebuilt when a field is ASSIGNED, never
     * when one is read. */
    V_NF->flags = C_NUM;
    V_NF->num = (double)g_nf;
    free(V_NF->str); V_NF->str = 0;
}

static void rebuild_record(void) {
    const char *ofs = getsval(V_OFS);
    size_t olen = strlen(ofs), tot = 1;
    for (int i = 0; i < g_nf; i++)
        tot += (g_fld[i] ? strlen(g_fld[i]) : 0) + olen;
    if (tot > g_reccap) { g_reccap = tot + 64; g_record = (char *)erealloc(g_record, g_reccap); }
    char *w = g_record;
    for (int i = 0; i < g_nf; i++) {
        if (i) { memcpy(w, ofs, olen); w += olen; }
        const char *f = g_fld[i] ? g_fld[i] : "";
        size_t n = strlen(f);
        memcpy(w, f, n);
        w += n;
    }
    *w = 0;
    g_rec_valid = 1;
}

static void set_nf_from_assign(double d) {
    ensure_split();
    int n = (int)d;
    if (n < 0) n = 0;
    fld_ensure(n + 1);
    for (int i = n; i < g_nf; i++) { free(g_fld[i]); g_fld[i] = 0; }
    for (int i = g_nf; i < n; i++) { free(g_fld[i]); g_fld[i] = estrdup(""); }
    g_nf = n;
    V_NF->flags = C_NUM;
    V_NF->num = (double)n;
    free(V_NF->str); V_NF->str = 0;
    rebuild_record();
}

static const char *field_str(int i) {
    if (i == 0) {
        if (!g_rec_valid) rebuild_record();
        return g_record ? g_record : "";
    }
    ensure_split();
    if (i < 1 || i > g_nf) return "";
    return g_fld[i - 1] ? g_fld[i - 1] : "";
}

static void field_assign(int i, const char *s) {
    if (i == 0) {
        set_record(s);
        ensure_split();
        return;
    }
    ensure_split();
    if (i > g_nf) {
        fld_ensure(i);
        for (int k = g_nf; k < i; k++) { free(g_fld[k]); g_fld[k] = estrdup(""); }
        g_nf = i;
        V_NF->flags = C_NUM;
        V_NF->num = (double)g_nf;
        free(V_NF->str); V_NF->str = 0;
    }
    fld_set_raw(i - 1, s, strlen(s));
    rebuild_record();
}

/* ================================================================== */
/* regex cache                                                        */
/* ================================================================== */

typedef struct RxEnt { char *pat; regex_t re; struct RxEnt *next; } RxEnt;
static RxEnt *g_rxcache;

static regex_t *re_compile_cached(const char *pat) {
    for (RxEnt *e = g_rxcache; e; e = e->next)
        if (strcmp(e->pat, pat) == 0) return &e->re;
    RxEnt *e = (RxEnt *)emalloc(sizeof *e);
    e->pat = estrdup(pat);
    int rc = regcomp(&e->re, pat, REG_EXTENDED | REG_ESCBRACKET);
    if (rc) {
        char eb[128];
        regerror(rc, &e->re, eb, sizeof eb);
        fatal("bad regular expression /%s/: %s", pat, eb);
    }
    e->next = g_rxcache;
    g_rxcache = e;
    return &e->re;
}

/* ================================================================== */
/* lexer                                                              */
/* ================================================================== */

enum {
    T_EOF = 256, T_NUMBER, T_STRING, T_ERE, T_NAME, T_FUNC_NAME,
    T_BUILTIN, T_GETLINE, T_BEGIN, T_END, T_FUNCTION,
    T_LE, T_GE, T_EQ, T_NE, T_MATCH, T_NOMATCH, T_AND, T_OR,
    T_INCR, T_DECR, T_APPEND,
    T_ADD_A, T_SUB_A, T_MUL_A, T_DIV_A, T_MOD_A, T_POW_A,
    T_IF, T_ELSE, T_WHILE, T_FOR, T_DO, T_BREAK, T_CONTINUE,
    T_NEXT, T_NEXTFILE, T_EXIT, T_RETURN, T_DELETE, T_IN,
    T_PRINT, T_PRINTF, T_NEWLINE
};

enum {
    B_LENGTH = 1, B_SUBSTR, B_INDEX, B_SPLIT, B_SUB, B_GSUB, B_MATCHF,
    B_SPRINTF, B_SIN, B_COS, B_ATAN2, B_EXP, B_LOG, B_SQRT, B_INT,
    B_RAND, B_SRAND, B_TOLOWER, B_TOUPPER, B_SYSTEM, B_CLOSE, B_FFLUSH
};

static const struct { const char *nm; int tok; int aux; } g_words[] = {
    { "BEGIN", T_BEGIN, 0 }, { "END", T_END, 0 }, { "function", T_FUNCTION, 0 },
    { "func", T_FUNCTION, 0 },
    { "if", T_IF, 0 }, { "else", T_ELSE, 0 }, { "while", T_WHILE, 0 },
    { "for", T_FOR, 0 }, { "do", T_DO, 0 }, { "break", T_BREAK, 0 },
    { "continue", T_CONTINUE, 0 }, { "next", T_NEXT, 0 },
    { "nextfile", T_NEXTFILE, 0 }, { "exit", T_EXIT, 0 },
    { "return", T_RETURN, 0 }, { "delete", T_DELETE, 0 }, { "in", T_IN, 0 },
    { "print", T_PRINT, 0 }, { "printf", T_PRINTF, 0 },
    { "getline", T_GETLINE, 0 },
    { "length", T_BUILTIN, B_LENGTH },   { "substr", T_BUILTIN, B_SUBSTR },
    { "index", T_BUILTIN, B_INDEX },     { "split", T_BUILTIN, B_SPLIT },
    { "sub", T_BUILTIN, B_SUB },         { "gsub", T_BUILTIN, B_GSUB },
    { "match", T_BUILTIN, B_MATCHF },    { "sprintf", T_BUILTIN, B_SPRINTF },
    { "sin", T_BUILTIN, B_SIN },         { "cos", T_BUILTIN, B_COS },
    { "atan2", T_BUILTIN, B_ATAN2 },     { "exp", T_BUILTIN, B_EXP },
    { "log", T_BUILTIN, B_LOG },         { "sqrt", T_BUILTIN, B_SQRT },
    { "int", T_BUILTIN, B_INT },         { "rand", T_BUILTIN, B_RAND },
    { "srand", T_BUILTIN, B_SRAND },     { "tolower", T_BUILTIN, B_TOLOWER },
    { "toupper", T_BUILTIN, B_TOUPPER }, { "system", T_BUILTIN, B_SYSTEM },
    { "close", T_BUILTIN, B_CLOSE },     { "fflush", T_BUILTIN, B_FFLUSH },
    { 0, 0, 0 }
};

static const char *g_src;
static const char *g_sp;
static int    g_tok;
static int    g_aux;          /* builtin id */
static double g_tnum;
static char  *g_tstr;
static int    g_prev_can_end; /* the previous token could END an expression,
                               * so a '/' here is division, not a regex */

typedef struct { const char *sp; int tok, aux, line, prev; double num; char *str; } LexSave;

static void lex_save(LexSave *s) {
    s->sp = g_sp; s->tok = g_tok; s->aux = g_aux; s->line = g_srcline;
    s->prev = g_prev_can_end; s->num = g_tnum;
    s->str = g_tstr ? estrdup(g_tstr) : 0;
}
static void lex_restore(LexSave *s) {
    g_sp = s->sp; g_tok = s->tok; g_aux = s->aux; g_srcline = s->line;
    g_prev_can_end = s->prev; g_tnum = s->num;
    free(g_tstr); g_tstr = s->str;
}

/* Backslash escapes in a STRING literal. awk's set, plus \/ which is
 * meaningless in a string but harmless, and octal. */
static size_t unescape(const char *s, size_t n, char *out) {
    char *w = out;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\' || i + 1 >= n) { *w++ = s[i]; continue; }
        i++;
        switch (s[i]) {
        case 'n': *w++ = '\n'; break;
        case 't': *w++ = '\t'; break;
        case 'r': *w++ = '\r'; break;
        case '\\': *w++ = '\\'; break;
        case '"': *w++ = '"'; break;
        case '/': *w++ = '/'; break;
        case 'a': *w++ = '\a'; break;
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'v': *w++ = '\v'; break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            int v = 0, k = 0;
            while (k < 3 && i < n && s[i] >= '0' && s[i] <= '7') {
                v = v * 8 + (s[i] - '0'); i++; k++;
            }
            i--;
            *w++ = (char)v;
            break;
        }
        default:
            /* An unknown escape keeps the backslash, which is what awk
             * does and what makes `gsub(/\./, "")` work when the regex
             * arrives as a string. */
            *w++ = '\\';
            *w++ = s[i];
            break;
        }
    }
    return (size_t)(w - out);
}

static void lex(void) {
    free(g_tstr);
    g_tstr = 0;
    g_aux = 0;

    for (;;) {
        while (*g_sp == ' ' || *g_sp == '\t' || *g_sp == '\r') g_sp++;
        if (*g_sp == '\\' && g_sp[1] == '\n') { g_sp += 2; g_srcline++; continue; }
        if (*g_sp == '\\' && g_sp[1] == '\r' && g_sp[2] == '\n') { g_sp += 3; g_srcline++; continue; }
        if (*g_sp == '#') { while (*g_sp && *g_sp != '\n') g_sp++; continue; }
        break;
    }

    if (!*g_sp) { g_tok = T_EOF; g_prev_can_end = 0; return; }

    if (*g_sp == '\n') { g_sp++; g_srcline++; g_tok = T_NEWLINE; g_prev_can_end = 0; return; }

    unsigned char c = (unsigned char)*g_sp;

    if (isdigit(c) || (c == '.' && isdigit((unsigned char)g_sp[1]))) {
        char *end;
        g_tnum = strtod(g_sp, &end);
        g_sp = end;
        g_tok = T_NUMBER;
        g_prev_can_end = 1;
        return;
    }

    if (isalpha(c) || c == '_') {
        const char *st = g_sp;
        while (isalnum((unsigned char)*g_sp) || *g_sp == '_') g_sp++;
        size_t n = (size_t)(g_sp - st);
        char *w = estrndup(st, n);
        for (int i = 0; g_words[i].nm; i++) {
            if (strcmp(w, g_words[i].nm) == 0) {
                free(w);
                g_tok = g_words[i].tok;
                g_aux = g_words[i].aux;
                /* `length` may appear with no parentheses at all. */
                g_prev_can_end = (g_tok == T_BUILTIN && g_aux == B_LENGTH);
                return;
            }
        }
        g_tstr = w;
        /* A NAME immediately followed by '(' -- no space -- is a function
         * call. With a space it is a concatenation of a variable and a
         * parenthesised expression, and awk really does distinguish. */
        g_tok = (*g_sp == '(') ? T_FUNC_NAME : T_NAME;
        g_prev_can_end = 1;
        return;
    }

    if (c == '"') {
        g_sp++;
        const char *st = g_sp;
        while (*g_sp && *g_sp != '"') {
            if (*g_sp == '\\' && g_sp[1]) g_sp++;
            if (*g_sp == '\n') g_srcline++;
            g_sp++;
        }
        if (*g_sp != '"') synerr("unterminated string");
        size_t n = (size_t)(g_sp - st);
        char *buf = (char *)emalloc(n + 1);
        size_t m = unescape(st, n, buf);
        buf[m] = 0;
        g_sp++;
        g_tstr = buf;
        g_tok = T_STRING;
        g_prev_can_end = 1;
        return;
    }

    if (c == '/' && !g_prev_can_end) {
        g_sp++;
        const char *st = g_sp;
        int inbr = 0;
        while (*g_sp && (*g_sp != '/' || inbr)) {
            if (*g_sp == '\\' && g_sp[1]) g_sp++;
            else if (*g_sp == '[') inbr = 1;
            else if (*g_sp == ']') inbr = 0;
            else if (*g_sp == '\n') synerr("newline in regular expression");
            g_sp++;
        }
        if (*g_sp != '/') synerr("unterminated regular expression");
        size_t n = (size_t)(g_sp - st);
        /* Only \/ is unescaped here: every other backslash belongs to the
         * regex, and eating it would turn /\./ into /./ -- "any
         * character" where the author wrote "a literal dot". */
        char *buf = (char *)emalloc(n + 1);
        char *w = buf;
        for (size_t i = 0; i < n; i++) {
            if (st[i] == '\\' && i + 1 < n && st[i + 1] == '/') { *w++ = '/'; i++; }
            else *w++ = st[i];
        }
        *w = 0;
        g_sp++;
        g_tstr = buf;
        g_tok = T_ERE;
        g_prev_can_end = 1;
        return;
    }

    g_sp++;
    g_prev_can_end = 0;
    switch (c) {
    case '+': if (*g_sp == '+') { g_sp++; g_tok = T_INCR; g_prev_can_end = 1; return; }
              if (*g_sp == '=') { g_sp++; g_tok = T_ADD_A; return; }
              g_tok = '+'; return;
    case '-': if (*g_sp == '-') { g_sp++; g_tok = T_DECR; g_prev_can_end = 1; return; }
              if (*g_sp == '=') { g_sp++; g_tok = T_SUB_A; return; }
              g_tok = '-'; return;
    case '*': if (*g_sp == '*') {          /* ** and **= are ^ and ^= */
                  g_sp++;
                  if (*g_sp == '=') { g_sp++; g_tok = T_POW_A; return; }
                  g_tok = '^'; return;
              }
              if (*g_sp == '=') { g_sp++; g_tok = T_MUL_A; return; }
              g_tok = '*'; return;
    case '/': if (*g_sp == '=') { g_sp++; g_tok = T_DIV_A; return; }
              g_tok = '/'; return;
    case '%': if (*g_sp == '=') { g_sp++; g_tok = T_MOD_A; return; }
              g_tok = '%'; return;
    case '^': if (*g_sp == '=') { g_sp++; g_tok = T_POW_A; return; }
              g_tok = '^'; return;
    case '=': if (*g_sp == '=') { g_sp++; g_tok = T_EQ; return; }
              g_tok = '='; return;
    case '!': if (*g_sp == '=') { g_sp++; g_tok = T_NE; return; }
              if (*g_sp == '~') { g_sp++; g_tok = T_NOMATCH; return; }
              g_tok = '!'; return;
    case '<': if (*g_sp == '=') { g_sp++; g_tok = T_LE; return; }
              g_tok = '<'; return;
    case '>': if (*g_sp == '=') { g_sp++; g_tok = T_GE; return; }
              if (*g_sp == '>') { g_sp++; g_tok = T_APPEND; return; }
              g_tok = '>'; return;
    case '&': if (*g_sp == '&') { g_sp++; g_tok = T_AND; return; }
              g_tok = '&'; return;
    case '|': if (*g_sp == '|') { g_sp++; g_tok = T_OR; return; }
              g_tok = '|'; return;
    case '~': g_tok = T_MATCH; return;
    case ')': case ']': g_prev_can_end = 1; g_tok = c; return;
    default:  g_tok = c; return;
    }
}

/* ================================================================== */
/* AST                                                                */
/* ================================================================== */

enum {
    N_NUM = 1, N_STR, N_REGEX, N_VAR, N_FIELD, N_INDEX, N_GROUPLIST,
    N_ASSIGN, N_COND, N_OR, N_AND, N_IN, N_MATCH, N_CMP,
    N_CONCAT, N_BINOP, N_UNARY, N_NOT, N_INCDEC,
    N_CALL, N_BUILTIN, N_GETLINE,
    N_PRINT, N_PRINTF, N_IF, N_WHILE, N_DO, N_FOR, N_FORIN,
    N_BLOCK, N_NEXT, N_NEXTFILE, N_EXIT, N_RETURN, N_BREAK, N_CONT,
    N_DELETE, N_DELETEALL, N_EXPRSTMT, N_NOP
};

/* getline flavours */
enum { GL_PLAIN = 1, GL_FILE, GL_CMD };
/* print/printf redirections */
enum { R_NONE = 0, R_FILE, R_APPEND, R_PIPE };

typedef struct Node {
    int    type;
    int    op;
    double num;
    char  *str;
    regex_t *re;
    struct Node *a, *b, *c, *d;
    struct Node **list;
    int    nlist;
    int    slot;      /* >= 0: a function-local, indexed into the frame */
} Node;

static Node *nnew(int type) {
    Node *n = (Node *)emalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->type = type;
    n->slot = -1;
    return n;
}

static void nlist_add(Node *n, Node *x) {
    n->list = (Node **)erealloc(n->list, sizeof(Node *) * (size_t)(n->nlist + 1));
    n->list[n->nlist++] = x;
}

/* ---- rules and functions ---- */

typedef struct Rule {
    Node *pat, *pat2, *act;
    int   is_begin, is_end, range_active;
    struct Rule *next;
} Rule;

typedef struct Func {
    char  *name;
    char **params;
    int    nparams;
    Node  *body;
    struct Func *next;
} Func;

static Rule *g_rules, *g_rules_tail;
static Func *g_funcs;

static Func *func_find(const char *nm) {
    for (Func *f = g_funcs; f; f = f->next)
        if (strcmp(f->name, nm) == 0) return f;
    return 0;
}

/* ================================================================== */
/* parser                                                             */
/* ================================================================== */

static char **g_locals;        /* parameter names of the function being parsed */
static int    g_nlocals;

static int local_slot(const char *nm) {
    for (int i = 0; i < g_nlocals; i++)
        if (strcmp(g_locals[i], nm) == 0) return i;
    return -1;
}

static Node *p_expr(int allow_in, int no_gt);
static Node *p_stmt(void);
static Node *p_stmt_list(int endtok);

static void expect(int t, const char *what) {
    if (g_tok != t) synerr(what);
    lex();
}


/* after these, a newline is not a terminator */
static void opt_nl(void) { while (g_tok == T_NEWLINE) lex(); }

static int is_lvalue(Node *n) {
    return n && (n->type == N_VAR || n->type == N_FIELD || n->type == N_INDEX);
}

static Node *mk_var(const char *nm) {
    Node *n = nnew(N_VAR);
    n->str = estrdup(nm);
    n->slot = local_slot(nm);
    return n;
}

/* index list -> a single SUBSEP-joined subscript expression is built at
 * run time; the parser just keeps the list. */
static Node *p_index(Node *var) {
    Node *n = nnew(N_INDEX);
    n->a = var;
    lex();                                  /* past '[' */
    for (;;) {
        nlist_add(n, p_expr(1, 0));
        if (g_tok == ',') { lex(); opt_nl(); continue; }
        break;
    }
    expect(']', "expected ] after subscript");
    return n;
}

static Node *p_primary(int allow_in, int no_gt);

static Node *p_getline_tail(Node *cmd, int flavour) {
    Node *n = nnew(N_GETLINE);
    n->op = flavour;
    n->b = cmd;
    /* an optional lvalue: getline x, getline a[i], getline $2 */
    if (g_tok == T_NAME) {
        Node *v = mk_var(g_tstr);
        lex();
        if (g_tok == '[') v = p_index(v);
        n->a = v;
    } else if (g_tok == '$') {
        lex();
        Node *f = nnew(N_FIELD);
        f->a = p_primary(1, 1);
        n->a = f;
    }
    return n;
}

static Node *p_primary(int allow_in, int no_gt) {
    Node *n;
    switch (g_tok) {
    case T_INCR: case T_DECR: {
        int isinc = (g_tok == T_INCR);
        lex();
        Node *t = p_primary(allow_in, no_gt);
        if (!is_lvalue(t)) synerr("++/-- needs a variable");
        n = nnew(N_INCDEC);
        n->op = isinc ? 1 : 2;     /* pre-inc / pre-dec */
        n->a = t;
        return n;
    }
    case '$': {
        lex();
        n = nnew(N_FIELD);
        n->a = p_primary(allow_in, no_gt);
        break;
    }
    case '(': {
        lex();
        opt_nl();
        Node *first = p_expr(1, 0);
        if (g_tok == ',') {
            Node *g = nnew(N_GROUPLIST);
            nlist_add(g, first);
            while (g_tok == ',') { lex(); opt_nl(); nlist_add(g, p_expr(1, 0)); }
            expect(')', "expected ) after expression list");
            if (g_tok == T_IN) {
                lex();
                if (g_tok != T_NAME) synerr("expected array name after `in'");
                Node *in = nnew(N_IN);
                in->a = g;
                in->b = mk_var(g_tstr);
                lex();
                n = in;
                break;
            }
            n = g;                 /* only legal as a print/printf list */
            break;
        }
        expect(')', "expected )");
        n = first;
        break;
    }
    case T_NUMBER:
        n = nnew(N_NUM);
        n->num = g_tnum;
        lex();
        break;
    case T_STRING:
        n = nnew(N_STR);
        n->str = estrdup(g_tstr);
        lex();
        break;
    case T_ERE:
        n = nnew(N_REGEX);
        n->str = estrdup(g_tstr);
        n->re = re_compile_cached(n->str);
        lex();
        break;
    case T_NAME: {
        char *nm = estrdup(g_tstr);
        lex();
        if (g_tok == '[') {
            n = p_index(mk_var(nm));
        } else {
            n = mk_var(nm);
        }
        free(nm);
        break;
    }
    case T_FUNC_NAME: {
        char *nm = estrdup(g_tstr);
        lex();
        expect('(', "expected ( after function name");
        n = nnew(N_CALL);
        n->str = nm;
        opt_nl();
        if (g_tok != ')') {
            for (;;) {
                nlist_add(n, p_expr(1, 0));
                if (g_tok == ',') { lex(); opt_nl(); continue; }
                break;
            }
        }
        expect(')', "expected ) after arguments");
        break;
    }
    case T_BUILTIN: {
        int id = g_aux;
        lex();
        n = nnew(N_BUILTIN);
        n->op = id;
        if (g_tok == '(') {
            lex();
            opt_nl();
            if (g_tok != ')') {
                for (;;) {
                    nlist_add(n, p_expr(1, 0));
                    if (g_tok == ',') { lex(); opt_nl(); continue; }
                    break;
                }
            }
            expect(')', "expected ) after arguments");
        } else if (id != B_LENGTH) {
            synerr("this built-in needs an argument list");
        }
        break;
    }
    case T_GETLINE:
        lex();
        n = p_getline_tail(0, GL_PLAIN);
        if (g_tok == '<') {
            lex();
            n->op = GL_FILE;
            n->b = p_primary(allow_in, no_gt);
            /* the file expression is a CONCATENATION, so `getline < "a"
             * "b"` reads from "ab"; keep pulling operands the way the
             * concat level would. */
            while (g_tok == T_STRING || g_tok == T_NUMBER || g_tok == T_NAME ||
                   g_tok == '$' || g_tok == '(') {
                Node *cc = nnew(N_CONCAT);
                cc->a = n->b;
                cc->b = p_primary(allow_in, no_gt);
                n->b = cc;
            }
        }
        break;
    case '!': case '-': case '+':
        /* reached when a unary operator follows an operator */
        return 0;
    default:
        synerr("unexpected token");
        return 0;
    }

    /* postfix ++/-- */
    while ((g_tok == T_INCR || g_tok == T_DECR) && is_lvalue(n)) {
        Node *t = nnew(N_INCDEC);
        t->op = (g_tok == T_INCR) ? 3 : 4;
        t->a = n;
        lex();
        n = t;
    }
    return n;
}

static Node *p_unary(int allow_in, int no_gt);

static Node *p_pow(int allow_in, int no_gt) {
    Node *a = p_primary(allow_in, no_gt);
    if (g_tok == '^') {
        lex();
        Node *n = nnew(N_BINOP);
        n->op = '^';
        n->a = a;
        n->b = p_unary(allow_in, no_gt);   /* right associative, and -x binds under it */
        return n;
    }
    return a;
}

static Node *p_unary(int allow_in, int no_gt) {
    if (g_tok == '!') {
        lex();
        Node *n = nnew(N_NOT);
        n->a = p_unary(allow_in, no_gt);
        return n;
    }
    if (g_tok == '-' || g_tok == '+') {
        int neg = (g_tok == '-');
        lex();
        Node *n = nnew(N_UNARY);
        n->op = neg ? '-' : '+';
        n->a = p_unary(allow_in, no_gt);
        return n;
    }
    return p_pow(allow_in, no_gt);
}

static Node *p_mul(int allow_in, int no_gt) {
    Node *a = p_unary(allow_in, no_gt);
    while (g_tok == '*' || g_tok == '/' || g_tok == '%') {
        int op = g_tok;
        lex();
        Node *n = nnew(N_BINOP);
        n->op = op;
        n->a = a;
        n->b = p_unary(allow_in, no_gt);
        a = n;
    }
    return a;
}

static Node *p_add(int allow_in, int no_gt) {
    Node *a = p_mul(allow_in, no_gt);
    while (g_tok == '+' || g_tok == '-') {
        int op = g_tok;
        lex();
        Node *n = nnew(N_BINOP);
        n->op = op;
        n->a = a;
        n->b = p_mul(allow_in, no_gt);
        a = n;
    }
    return a;
}

/* Can this token begin another operand of a concatenation? Deliberately
 * NOT + or - (p_add has already taken those, which is what makes
 * `"x" - 1` arithmetic and not concatenation), and not ++/-- (which
 * belong to the operand in front). */
static int starts_operand(int t) {
    switch (t) {
    case T_NUMBER: case T_STRING: case T_ERE: case T_NAME:
    case T_FUNC_NAME: case T_BUILTIN: case '$': case '(': case '!':
        return 1;
    default:
        return 0;
    }
}

static Node *p_concat(int allow_in, int no_gt) {
    Node *a = p_add(allow_in, no_gt);
    while (starts_operand(g_tok)) {
        /* `in' is a keyword, but the array name after it must not be
         * swallowed as a concatenation operand. */
        Node *n = nnew(N_CONCAT);
        n->a = a;
        n->b = p_add(allow_in, no_gt);
        a = n;
    }
    return a;
}

static Node *p_pipe_getline(int allow_in, int no_gt) {
    Node *a = p_concat(allow_in, no_gt);
    while (g_tok == '|') {
        LexSave sv;
        lex_save(&sv);
        lex();
        if (g_tok != T_GETLINE) { lex_restore(&sv); break; }
        lex();
        a = p_getline_tail(a, GL_CMD);
    }
    return a;
}

static Node *p_rel(int allow_in, int no_gt) {
    Node *a = p_pipe_getline(allow_in, no_gt);
    int op = 0;
    switch (g_tok) {
    case '<':   op = '<'; break;
    case T_LE:  op = T_LE; break;
    case T_EQ:  op = T_EQ; break;
    case T_NE:  op = T_NE; break;
    case T_GE:  op = T_GE; break;
    case '>':   if (no_gt) return a; op = '>'; break;
    default:    return a;
    }
    lex();
    Node *n = nnew(N_CMP);
    n->op = op;
    n->a = a;
    n->b = p_pipe_getline(allow_in, no_gt);
    return n;                                /* non-associative */
}

static Node *p_match(int allow_in, int no_gt) {
    Node *a = p_rel(allow_in, no_gt);
    while (g_tok == T_MATCH || g_tok == T_NOMATCH) {
        int neg = (g_tok == T_NOMATCH);
        lex();
        Node *n = nnew(N_MATCH);
        n->op = neg;
        n->a = a;
        n->b = p_rel(allow_in, no_gt);
        a = n;
    }
    return a;
}

static Node *p_in(int allow_in, int no_gt) {
    Node *a = p_match(allow_in, no_gt);
    while (allow_in && g_tok == T_IN) {
        lex();
        if (g_tok != T_NAME) synerr("expected array name after `in'");
        Node *n = nnew(N_IN);
        Node *g = nnew(N_GROUPLIST);
        nlist_add(g, a);
        n->a = g;
        n->b = mk_var(g_tstr);
        lex();
        a = n;
    }
    return a;
}

static Node *p_and(int allow_in, int no_gt) {
    Node *a = p_in(allow_in, no_gt);
    while (g_tok == T_AND) {
        lex();
        opt_nl();
        Node *n = nnew(N_AND);
        n->a = a;
        n->b = p_in(allow_in, no_gt);
        a = n;
    }
    return a;
}

static Node *p_or(int allow_in, int no_gt) {
    Node *a = p_and(allow_in, no_gt);
    while (g_tok == T_OR) {
        lex();
        opt_nl();
        Node *n = nnew(N_OR);
        n->a = a;
        n->b = p_and(allow_in, no_gt);
        a = n;
    }
    return a;
}

static Node *p_ternary(int allow_in, int no_gt) {
    Node *a = p_or(allow_in, no_gt);
    if (g_tok == '?') {
        lex();
        opt_nl();
        Node *n = nnew(N_COND);
        n->a = a;
        n->b = p_ternary(allow_in, no_gt);
        opt_nl();
        expect(':', "expected : in ?: ");
        opt_nl();
        n->c = p_ternary(allow_in, no_gt);
        return n;
    }
    return a;
}

static Node *p_expr(int allow_in, int no_gt) {
    Node *a = p_ternary(allow_in, no_gt);
    int op = 0;
    switch (g_tok) {
    case '=':     op = '=';   break;
    case T_ADD_A: op = '+';   break;
    case T_SUB_A: op = '-';   break;
    case T_MUL_A: op = '*';   break;
    case T_DIV_A: op = '/';   break;
    case T_MOD_A: op = '%';   break;
    case T_POW_A: op = '^';   break;
    default: return a;
    }
    if (!is_lvalue(a)) synerr("assignment to something that is not a variable");
    lex();
    opt_nl();
    Node *n = nnew(N_ASSIGN);
    n->op = op;
    n->a = a;
    n->b = p_expr(allow_in, no_gt);
    return n;
}

/* ---- statements ---- */

static int stmt_end(void) {
    return g_tok == ';' || g_tok == T_NEWLINE || g_tok == '}' || g_tok == T_EOF;
}

static void eat_terminators(void) {
    while (g_tok == ';' || g_tok == T_NEWLINE) lex();
}

static Node *p_simple_or_block(void) {
    opt_nl();
    return p_stmt();
}

static Node *p_print(int is_printf) {
    Node *n = nnew(is_printf ? N_PRINTF : N_PRINT);
    lex();
    if (!stmt_end() && g_tok != '>' && g_tok != T_APPEND && g_tok != '|') {
        for (;;) {
            nlist_add(n, p_expr(1, 1));      /* no_gt: '>' is a redirection */
            if (g_tok == ',') { lex(); opt_nl(); continue; }
            break;
        }
    }
    /* `print (a, b)` and `printf("%s\n", x)`: a single parenthesised list
     * IS the argument list. */
    if (n->nlist == 1 && n->list[0]->type == N_GROUPLIST) {
        Node *g = n->list[0];
        free(n->list);
        n->list = g->list;
        n->nlist = g->nlist;
    }
    if (g_tok == '>')            { n->op = R_FILE;   lex(); n->b = p_concat(1, 1); }
    else if (g_tok == T_APPEND)  { n->op = R_APPEND; lex(); n->b = p_concat(1, 1); }
    else if (g_tok == '|')       { n->op = R_PIPE;   lex(); n->b = p_concat(1, 1); }
    if (is_printf && n->nlist == 0) synerr("printf needs a format");
    return n;
}

static Node *p_stmt(void) {
    Node *n;
    switch (g_tok) {
    case '{':
        lex();
        n = p_stmt_list('}');
        expect('}', "expected }");
        return n;
    case ';':
        lex();
        return nnew(N_NOP);
    case T_IF: {
        lex();
        expect('(', "expected ( after if");
        n = nnew(N_IF);
        n->a = p_expr(1, 0);
        expect(')', "expected ) after if condition");
        n->b = p_simple_or_block();
        /* `else` may be preceded by newlines and by the ; that ended the
         * then-part. */
        {
            LexSave sv;
            lex_save(&sv);
            eat_terminators();
            if (g_tok == T_ELSE) {
                free(sv.str);
                lex();
                n->c = p_simple_or_block();
            } else {
                lex_restore(&sv);
            }
        }
        return n;
    }
    case T_WHILE:
        lex();
        expect('(', "expected ( after while");
        n = nnew(N_WHILE);
        n->a = p_expr(1, 0);
        expect(')', "expected ) after while condition");
        n->b = p_simple_or_block();
        return n;
    case T_DO:
        lex();
        n = nnew(N_DO);
        n->b = p_simple_or_block();
        eat_terminators();
        if (g_tok != T_WHILE) synerr("expected while after do body");
        lex();
        expect('(', "expected ( after while");
        n->a = p_expr(1, 0);
        expect(')', "expected ) after while condition");
        return n;
    case T_FOR: {
        lex();
        expect('(', "expected ( after for");
        /* for (k in arr) -- distinguished by lookahead, because the init
         * clause of a C-style for can also begin with a NAME. */
        if (g_tok == '(' ) {
            /* for ((i,j) in arr) */
            LexSave sv;
            lex_save(&sv);
            Node *e = p_expr(1, 0);
            if (e->type == N_IN && g_tok == ')') {
                free(sv.str);
                lex();
                n = nnew(N_FORIN);
                n->a = e->a;
                n->b = e->b;
                n->c = p_simple_or_block();
                return n;
            }
            lex_restore(&sv);
        }
        if (g_tok == T_NAME) {
            LexSave sv;
            lex_save(&sv);
            char *nm = estrdup(g_tstr);
            lex();
            if (g_tok == T_IN) {
                lex();
                if (g_tok != T_NAME) synerr("expected array name after `in'");
                n = nnew(N_FORIN);
                Node *g = nnew(N_GROUPLIST);
                nlist_add(g, mk_var(nm));
                n->a = g;
                n->b = mk_var(g_tstr);
                lex();
                expect(')', "expected ) after for-in");
                n->c = p_simple_or_block();
                free(nm);
                free(sv.str);
                return n;
            }
            free(nm);
            lex_restore(&sv);
        }
        n = nnew(N_FOR);
        if (g_tok != ';') n->a = p_expr(1, 0);
        expect(';', "expected ; in for");
        opt_nl();
        if (g_tok != ';') n->b = p_expr(1, 0);
        expect(';', "expected ; in for");
        opt_nl();
        if (g_tok != ')') n->c = p_expr(1, 0);
        expect(')', "expected ) after for clauses");
        n->d = p_simple_or_block();
        return n;
    }
    case T_BREAK:    lex(); return nnew(N_BREAK);
    case T_CONTINUE: lex(); return nnew(N_CONT);
    case T_NEXT:     lex(); return nnew(N_NEXT);
    case T_NEXTFILE: lex(); return nnew(N_NEXTFILE);
    case T_EXIT:
        lex();
        n = nnew(N_EXIT);
        if (!stmt_end()) n->a = p_expr(1, 0);
        return n;
    case T_RETURN:
        lex();
        n = nnew(N_RETURN);
        if (!stmt_end()) n->a = p_expr(1, 0);
        return n;
    case T_DELETE: {
        lex();
        if (g_tok != T_NAME) synerr("expected array name after delete");
        Node *v = mk_var(g_tstr);
        lex();
        if (g_tok == '[') {
            n = p_index(v);
            Node *d = nnew(N_DELETE);
            d->a = n;
            return d;
        }
        n = nnew(N_DELETEALL);
        n->a = v;
        return n;
    }
    case T_PRINT:  return p_print(0);
    case T_PRINTF: return p_print(1);
    default:
        n = nnew(N_EXPRSTMT);
        n->a = p_expr(1, 0);
        return n;
    }
}

static Node *p_stmt_list(int endtok) {
    Node *n = nnew(N_BLOCK);
    eat_terminators();
    while (g_tok != endtok && g_tok != T_EOF) {
        nlist_add(n, p_stmt());
        eat_terminators();
    }
    return n;
}

static void add_rule(Rule *r) {
    r->next = 0;
    if (g_rules_tail) g_rules_tail->next = r;
    else g_rules = r;
    g_rules_tail = r;
}

static void p_program(void) {
    lex();
    for (;;) {
        eat_terminators();
        if (g_tok == T_EOF) break;

        if (g_tok == T_FUNCTION) {
            lex();
            if (g_tok != T_NAME && g_tok != T_FUNC_NAME)
                synerr("expected function name");
            char *nm = estrdup(g_tstr);
            lex();
            expect('(', "expected ( after function name");
            Func *f = (Func *)emalloc(sizeof *f);
            memset(f, 0, sizeof *f);
            f->name = nm;
            while (g_tok != ')') {
                if (g_tok != T_NAME) synerr("expected parameter name");
                f->params = (char **)erealloc(f->params,
                                sizeof(char *) * (size_t)(f->nparams + 1));
                f->params[f->nparams++] = estrdup(g_tstr);
                lex();
                if (g_tok == ',') { lex(); opt_nl(); }
            }
            lex();
            opt_nl();
            if (g_tok != '{') synerr("expected { for function body");
            /* Register BEFORE parsing the body so recursion resolves. */
            f->next = g_funcs;
            g_funcs = f;
            g_locals = f->params;
            g_nlocals = f->nparams;
            lex();
            f->body = p_stmt_list('}');
            expect('}', "expected } to end function");
            g_locals = 0;
            g_nlocals = 0;
            continue;
        }

        Rule *r = (Rule *)emalloc(sizeof *r);
        memset(r, 0, sizeof *r);

        if (g_tok == T_BEGIN)      { r->is_begin = 1; lex(); opt_nl(); }
        else if (g_tok == T_END)   { r->is_end = 1;   lex(); opt_nl(); }
        else if (g_tok != '{') {
            r->pat = p_expr(1, 0);
            if (g_tok == ',') {
                lex();
                opt_nl();
                r->pat2 = p_expr(1, 0);
            }
        }
        if (g_tok == '{') {
            lex();
            r->act = p_stmt_list('}');
            expect('}', "expected } to end action");
        } else {
            if (r->is_begin || r->is_end) synerr("BEGIN/END need an action");
            r->act = 0;                       /* default: print $0 */
        }
        add_rule(r);
    }
}

/* ================================================================== */
/* output streams (with our own buffering -- libtoby's stdio has none)  */
/* ================================================================== */

typedef struct OFile {
    char  *name;
    FILE  *f;
    int    is_pipe;
    char   buf[8192];
    size_t blen;
    struct OFile *next;
} OFile;

static OFile *g_ofiles;
static OFile  g_stdout_of;

static void of_flush(OFile *o) {
    if (o->blen) {
        fwrite(o->buf, 1, o->blen, o->f);
        o->blen = 0;
    }
}

static void of_write(OFile *o, const char *s, size_t n) {
    if (n >= sizeof o->buf) {
        of_flush(o);
        fwrite(s, 1, n, o->f);
        return;
    }
    if (o->blen + n > sizeof o->buf) of_flush(o);
    memcpy(o->buf + o->blen, s, n);
    o->blen += n;
}

static void flush_all(void) {
    of_flush(&g_stdout_of);
    for (OFile *o = g_ofiles; o; o = o->next) of_flush(o);
}

static OFile *of_get(const char *name, int mode) {
    for (OFile *o = g_ofiles; o; o = o->next)
        if (strcmp(o->name, name) == 0) return o;
    OFile *o = (OFile *)emalloc(sizeof *o);
    memset(o, 0, sizeof *o);
    o->name = estrdup(name);
    if (mode == R_PIPE) {
        /* Anything already buffered for stdout must reach the terminal
         * before the child writes to it, or the output interleaves in the
         * wrong order. */
        flush_all();
        o->f = popen(name, "w");
        o->is_pipe = 1;
    } else if (strcmp(name, "/dev/stdout") == 0) {
        o->f = stdout;
    } else if (strcmp(name, "/dev/stderr") == 0) {
        o->f = stderr;
    } else {
        o->f = fopen(name, mode == R_APPEND ? "a" : "w");
    }
    if (!o->f) fatal("can't redirect to %s", name);
    o->next = g_ofiles;
    g_ofiles = o;
    return o;
}

/* ================================================================== */
/* input streams                                                      */
/* ================================================================== */

typedef struct Reader {
    int     fd;
    FILE   *fp;          /* non-NULL when we opened it (file or pipe) */
    int     is_pipe;
    char   *buf;
    size_t  len, pos, cap;
    int     eof;
} Reader;

typedef struct IFile {
    char   *name;
    Reader *r;
    struct IFile *next;
} IFile;

static IFile *g_ifiles;

static Reader *reader_new(int fd, FILE *fp, int is_pipe) {
    Reader *r = (Reader *)emalloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->fd = fd;
    r->fp = fp;
    r->is_pipe = is_pipe;
    r->cap = 8192;
    r->buf = (char *)emalloc(r->cap);
    return r;
}

static void reader_free(Reader *r) {
    if (!r) return;
    if (r->fp) { if (r->is_pipe) pclose(r->fp); else fclose(r->fp); }
    free(r->buf);
    free(r);
}

/* Refill.
 *
 * Uses read(2) directly rather than fread(), which loops until it has the
 * whole request and would therefore block on a pipe that is still
 * producing -- `"cmd" | getline` has to return the first line as soon as
 * the command emits it.
 *
 * THE INVARIANT, and the reason it is worth stating: a refill may COMPACT
 * the buffer, which moves every byte. What it never changes is a byte's
 * offset RELATIVE TO r->pos. So a caller mid-record keeps its position as
 * a relative count and stays correct across any number of refills; a
 * caller holding an absolute index does not, and the earlier version of
 * the paragraph-mode scanner did exactly that -- correct for a record that
 * fitted in one read, silently wrong for one that did not. */
static int reader_fill(Reader *r) {
    if (r->eof) return 0;
    if (r->pos > 0) {
        memmove(r->buf, r->buf + r->pos, r->len - r->pos);
        r->len -= r->pos;
        r->pos = 0;
    }
    if (r->len == r->cap) {
        r->cap *= 2;
        r->buf = (char *)erealloc(r->buf, r->cap);
    }
    ssize_t got = read(r->fd, r->buf + r->len, r->cap - r->len);
    if (got <= 0) { r->eof = 1; return 0; }
    r->len += (size_t)got;
    return 1;
}

/* Read one record according to RS. Returns 0 at EOF, else 1 and points
 * *out at a NUL-terminated record inside a private buffer. */
static char  *g_recbuf;
static size_t g_recbufcap;

static void recbuf_set(const char *p, size_t n) {
    if (n + 1 > g_recbufcap) {
        g_recbufcap = n + 128;
        g_recbuf = (char *)erealloc(g_recbuf, g_recbufcap);
    }
    memcpy(g_recbuf, p, n);
    g_recbuf[n] = 0;
}

static int reader_getrec(Reader *r, char **out) {
    const char *rs = getsval(V_RS);
    size_t avail, scan;

    if (rs[0] == 0) {
        /* Paragraph mode: skip leading newlines, then read up to a blank
         * line or EOF; the record excludes the separating newlines, and a
         * newline is additionally a field separator (POSIX). */
        for (;;) {
            while (r->pos < r->len && r->buf[r->pos] == '\n') r->pos++;
            if (r->pos < r->len) break;
            if (!reader_fill(r)) return 0;
        }
        scan = 0;
        for (;;) {
            avail = r->len - r->pos;
            for (size_t k = scan; k + 1 < avail; k++) {
                if (r->buf[r->pos + k] == '\n' && r->buf[r->pos + k + 1] == '\n') {
                    recbuf_set(r->buf + r->pos, k);
                    r->pos += k + 1;      /* the blank line is skipped next time */
                    *out = g_recbuf;
                    return 1;
                }
            }
            scan = (avail > 1) ? avail - 1 : 0;
            if (!reader_fill(r)) {
                size_t n = r->len - r->pos;
                while (n && r->buf[r->pos + n - 1] == '\n') n--;
                if (n == 0) return 0;
                recbuf_set(r->buf + r->pos, n);
                r->pos = r->len;
                *out = g_recbuf;
                return 1;
            }
        }
    }

    if (rs[1] == 0) {
        int sep = (unsigned char)rs[0];
        scan = 0;
        for (;;) {
            avail = r->len - r->pos;
            for (size_t k = scan; k < avail; k++) {
                if ((unsigned char)r->buf[r->pos + k] == sep) {
                    recbuf_set(r->buf + r->pos, k);
                    r->pos += k + 1;
                    *out = g_recbuf;
                    return 1;
                }
            }
            scan = avail;
            if (!reader_fill(r)) {
                if (r->pos >= r->len) return 0;
                recbuf_set(r->buf + r->pos, r->len - r->pos);
                r->pos = r->len;
                *out = g_recbuf;
                return 1;
            }
        }
    }

    /* RS is a regular expression (a superset of POSIX, which looks only
     * at the first character; gawk does the same). */
    for (;;) {
        avail = r->len - r->pos;
        if (avail) {
            recbuf_set(r->buf + r->pos, avail);
            regex_t *re = re_compile_cached(rs);
            regmatch_t m;
            if (regexec(re, g_recbuf, 1, &m, 0) == 0 && m.rm_eo > m.rm_so) {
                /* Only trust a match that does not run to the very end of
                 * what we have, or a separator could be cut in half. */
                if ((size_t)m.rm_eo < avail || r->eof) {
                    r->pos += (size_t)m.rm_eo;
                    g_recbuf[(size_t)m.rm_so] = 0;
                    *out = g_recbuf;
                    return 1;
                }
            }
        }
        if (!reader_fill(r)) {
            if (r->pos >= r->len) return 0;
            recbuf_set(r->buf + r->pos, r->len - r->pos);
            r->pos = r->len;
            *out = g_recbuf;
            return 1;
        }
    }
}

static Reader *if_get(const char *name, int is_cmd) {
    for (IFile *f = g_ifiles; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f->r;
    Reader *r = 0;
    if (is_cmd) {
        flush_all();
        FILE *fp = popen(name, "r");
        if (!fp) return 0;
        r = reader_new(fileno(fp), fp, 1);
    } else if (strcmp(name, "-") == 0 || strcmp(name, "/dev/stdin") == 0) {
        r = reader_new(0, 0, 0);
    } else {
        FILE *fp = fopen(name, "r");
        if (!fp) return 0;
        r = reader_new(fileno(fp), fp, 0);
    }
    IFile *f = (IFile *)emalloc(sizeof *f);
    f->name = estrdup(name);
    f->r = r;
    f->next = g_ifiles;
    g_ifiles = f;
    return r;
}

/* close() covers both directions and reports -1 for a name that was never
 * open, which is what scripts test. */
static int do_close(const char *name) {
    int found = -1;
    for (IFile **pp = &g_ifiles; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->name, name) == 0) {
            IFile *f = *pp;
            *pp = f->next;
            reader_free(f->r);
            free(f->name);
            free(f);
            found = 0;
            break;
        }
    }
    for (OFile **pp = &g_ofiles; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->name, name) == 0) {
            OFile *o = *pp;
            *pp = o->next;
            of_flush(o);
            if (o->is_pipe) found = pclose(o->f);
            else { if (o->f != stdout && o->f != stderr) fclose(o->f); found = 0; }
            free(o->name);
            free(o);
            break;
        }
    }
    return found;
}

/* ================================================================== */
/* interpreter                                                        */
/* ================================================================== */

enum { X_NORMAL = 0, X_BREAK, X_CONT, X_NEXT, X_NEXTFILE, X_EXIT, X_RETURN };

typedef struct Frame {
    Cell **slot;         /* the cell each parameter resolves to */
    int   *owned;        /* 1: we allocated it and must free it */
    Cell **alias;        /* non-NULL: the caller cell we aliased */
    int    n;
    Cell  *ret;
} Frame;

static Frame *g_frame;
static int    g_exit_code;
static int    g_in_end;

/* eval returns a borrowed Cell*; the caller must not free it. Temporaries
 * come from a rotating pool, which is enough because no expression holds
 * more than a handful live at once and each is consumed immediately. */
#define TMPN 64
static Cell g_tmps[TMPN];
static int  g_tmpi;

static Cell *tmp(void) {
    Cell *c = &g_tmps[g_tmpi];
    g_tmpi = (g_tmpi + 1) % TMPN;
    free(c->str);
    c->str = 0;
    c->arr = 0;
    c->num = 0;
    c->flags = C_UNINIT;
    return c;
}

static Cell *tmp_num(double d) { Cell *c = tmp(); c->num = d; c->flags = C_NUM; return c; }
static Cell *tmp_str(const char *s) {
    Cell *c = tmp();
    c->str = estrdup(s ? s : "");
    c->flags = C_STR;
    return c;
}

static Cell *eval(Node *n);
static int   exec(Node *n);

static int truth(Cell *c) {
    if (!c) return 0;
    if (c->flags & C_UNINIT) return 0;
    /* A genuine string is true when it is non-empty -- INCLUDING "0",
     * which is why this cannot ask getfval first. Everything else (a
     * number, or a field that looks like one) is true when non-zero.
     *
     * This used to test C_NUM before deciding, so a string whose numeric
     * value had ever been taken -- `x="0abc"; y=x+0` -- answered on the
     * cached 0 and became false. */
    if (cell_is_str(c)) return c->str && c->str[0];
    return getfval(c) != 0;
}

static Cell *var_cell(Node *n) {
    if (n->slot >= 0 && g_frame) return g_frame->slot[n->slot];
    return map_get(globals, n->str);
}

static Map *array_of(Node *n) {
    Cell *c;
    if (n->type != N_VAR) fatal("expected an array name");
    c = var_cell(n);
    if (!c->arr) {
        c->arr = map_new();
        c->flags = (c->flags & ~C_UNINIT) | C_ARR;
    }
    return c->arr;
}

/* Join a subscript list with SUBSEP. */
static const char *subscript(Node **list, int nlist) {
    static char *buf;
    static size_t cap;
    const char *ss = getsval(V_SUBSEP);
    size_t sl = strlen(ss), tot = 1;
    const char *parts[16];
    char *heap[16];
    int nh = 0;
    if (nlist > 16) fatal("too many subscripts");
    for (int i = 0; i < nlist; i++) {
        const char *s = getsval(eval(list[i]));
        heap[nh] = estrdup(s);
        parts[i] = heap[nh];
        nh++;
        tot += strlen(parts[i]) + sl;
    }
    if (tot > cap) { cap = tot + 64; buf = (char *)erealloc(buf, cap); }
    char *w = buf;
    for (int i = 0; i < nlist; i++) {
        if (i) { memcpy(w, ss, sl); w += sl; }
        size_t n = strlen(parts[i]);
        memcpy(w, parts[i], n);
        w += n;
    }
    *w = 0;
    for (int i = 0; i < nh; i++) free(heap[i]);
    return buf;
}

/* Resolve an lvalue to somewhere we can store. Fields are special
 * because writing one rebuilds $0. */
typedef struct LV { Cell *cell; int field; } LV;

static LV lvalue(Node *n) {
    LV lv;
    lv.cell = 0;
    lv.field = -1;
    switch (n->type) {
    case N_VAR:
        lv.cell = var_cell(n);
        break;
    case N_INDEX: {
        Map *m = array_of(n->a);
        lv.cell = map_get(m, subscript(n->list, n->nlist));
        break;
    }
    case N_FIELD:
        lv.field = (int)getfval(eval(n->a));
        if (lv.field < 0) fatal("attempt to access field %d", lv.field);
        break;
    default:
        fatal("not an assignable expression");
    }
    return lv;
}

static void lv_set_str(LV lv, const char *s) {
    if (lv.field >= 0) field_assign(lv.field, s);
    else setsval(lv.cell, s);
}
static void lv_set_num(LV lv, double d) {
    /* A number stored into a FIELD becomes text, and POSIX says CONVFMT
     * decides how -- not a hardcoded %.6g. */
    if (lv.field >= 0) {
        Cell t;
        memset(&t, 0, sizeof t);
        t.flags = C_NUM;
        t.num = d;
        field_assign(lv.field, getsval(&t));
        free(t.str);
    } else setfval(lv.cell, d);
}
static Cell *lv_get(LV lv) {
    if (lv.field >= 0) {
        Cell *c = tmp();
        set_input_val(c, field_str(lv.field));
        return c;
    }
    if (lv.cell == V_NF) ensure_split();
    return lv.cell;
}

/* POSIX comparison: numeric when neither side is a genuine string. */
static int cmp_cells(Cell *a, Cell *b) {
    int a_str = cell_is_str(a);
    int b_str = cell_is_str(b);
    if (!a_str && !b_str) {
        double x = getfval(a), y = getfval(b);
        return (x < y) ? -1 : (x > y) ? 1 : 0;
    }
    return strcmp(getsval(a), getsval(b));
}

static regex_t *regex_of(Node *n) {
    if (n->type == N_REGEX) return n->re;
    return re_compile_cached(getsval(eval(n)));
}

static void awk_printf(OFile *out, Node **args, int nargs, char **retstr);

static double awk_rand_next(void);
static void   awk_srand(double seed);

static const char *bi_substr(const char *s, double m, double nch, int have_n) {
    size_t len = strlen(s);
    /* Arithmetic in DOUBLES, because substr($0, 0, 3) and substr($0, -1)
     * both have defined answers that integer truncation gets wrong.
     *
     * A start before position 1 is CLAMPED to 1 and the length is left
     * alone, so substr("hello",-2,3) is "hel". Strict POSIX would count
     * the missing positions against the length and give "" -- GNU awk
     * does it this way, and matching the tool people actually have is the
     * whole point of this program. */
    double start = m < 1 ? 1 : m;
    double end = have_n ? start + nch : (double)len + 1;
    if (end > (double)len + 1) end = (double)len + 1;
    if (end <= start) return "";
    size_t so = (size_t)(start - 1), eo = (size_t)(end - 1);
    if (so > len) so = len;
    if (eo > len) eo = len;
    static char *buf;
    static size_t cap;
    size_t n = eo - so;
    if (n + 1 > cap) { cap = n + 64; buf = (char *)erealloc(buf, cap); }
    memcpy(buf, s + so, n);
    buf[n] = 0;
    return buf;
}

/* The replacement text of sub()/gsub(): & is the matched text, \& a
 * literal ampersand, \\ a literal backslash. */
static void append_repl(char **out, size_t *len, size_t *cap,
                        const char *repl, const char *matched, size_t mlen) {
    for (const char *p = repl; *p; p++) {
        const char *add = 0;
        size_t addn = 0;
        char one[2];
        if (*p == '\\' && (p[1] == '&' || p[1] == '\\')) {
            p++;
            one[0] = *p;
            add = one;
            addn = 1;
        } else if (*p == '&') {
            add = matched;
            addn = mlen;
        } else {
            one[0] = *p;
            add = one;
            addn = 1;
        }
        if (*len + addn + 1 > *cap) {
            *cap = (*len + addn) * 2 + 64;
            *out = (char *)erealloc(*out, *cap);
        }
        memcpy(*out + *len, add, addn);
        *len += addn;
    }
}

static int do_subst(regex_t *re, const char *repl, const char *src,
                    int global, char **result) {
    char  *out = 0;
    size_t len = 0, cap = 0;
    int    count = 0;
    const char *p = src;
    regmatch_t m;

    while (regexec(re, p, 1, &m, p == src ? 0 : REG_NOTBOL) == 0) {
        size_t so = (size_t)m.rm_so, eo = (size_t)m.rm_eo;
        if (len + so + 1 > cap) { cap = (len + so) * 2 + 64; out = (char *)erealloc(out, cap); }
        memcpy(out + len, p, so);
        len += so;
        append_repl(&out, &len, &cap, repl, p + so, eo - so);
        count++;
        if (!global) { p += eo; break; }
        if (eo == so) {
            /* An empty match must still advance, or gsub(/x*\/, "-")
             * would never terminate. Copy one character and move on. */
            if (!p[eo]) { p += eo; break; }
            if (len + 2 > cap) { cap = len * 2 + 64; out = (char *)erealloc(out, cap); }
            out[len++] = p[eo];
            p += eo + 1;
        } else {
            p += eo;
        }
    }
    size_t rest = strlen(p);
    if (len + rest + 1 > cap) { cap = len + rest + 64; out = (char *)erealloc(out, cap); }
    memcpy(out + len, p, rest);
    len += rest;
    if (!out) { out = (char *)emalloc(1); }
    out[len] = 0;
    *result = out;
    return count;
}

static void emit_field_into(int idx, const char *p, size_t n, void *ud) {
    Map *m = (Map *)ud;
    char key[32];
    snprintf(key, sizeof key, "%d", idx + 1);
    Cell *c = map_get(m, key);
    char *s = estrndup(p, n);
    set_input_val(c, s);
    free(s);
}

static int getline_into(Reader *r, Node *target, int set_nr, int set_fnr) {
    char *rec;
    if (!r) return -1;
    if (!reader_getrec(r, &rec)) return 0;
    if (target) {
        LV lv = lvalue(target);
        if (lv.field >= 0) {
            field_assign(lv.field, rec);
        } else {
            set_input_val(lv.cell, rec);
        }
    } else {
        set_record(rec);
        ensure_split();
    }
    if (set_nr)  setfval(V_NR, getfval(V_NR) + 1);
    if (set_fnr) setfval(V_FNR, getfval(V_FNR) + 1);
    return 1;
}

static Reader *g_main_reader;      /* the reader the main loop is using */
static int     main_next_record(void);

static Cell *eval_builtin(Node *n) {
    int nargs = n->nlist;
    switch (n->op) {
    case B_LENGTH: {
        if (nargs == 0) return tmp_num((double)strlen(field_str(0)));
        /* length(arr) is the element count, which scripts use constantly. */
        if (n->list[0]->type == N_VAR) {
            Cell *c = var_cell(n->list[0]);
            if (c->arr) return tmp_num((double)c->arr->n);
        }
        return tmp_num((double)strlen(getsval(eval(n->list[0]))));
    }
    case B_SUBSTR: {
        if (nargs < 2) fatal("substr needs at least 2 arguments");
        char *s = estrdup(getsval(eval(n->list[0])));
        double m = getfval(eval(n->list[1]));
        double k = 0;
        int have = 0;
        if (nargs >= 3) { k = getfval(eval(n->list[2])); have = 1; }
        Cell *r = tmp_str(bi_substr(s, m, k, have));
        free(s);
        return r;
    }
    case B_INDEX: {
        if (nargs < 2) fatal("index needs 2 arguments");
        char *s = estrdup(getsval(eval(n->list[0])));
        const char *t = getsval(eval(n->list[1]));
        const char *p = strstr(s, t);
        Cell *r = tmp_num(p ? (double)(p - s + 1) : 0);
        free(s);
        return r;
    }
    case B_SPLIT: {
        if (nargs < 2) fatal("split needs at least 2 arguments");
        char *s = estrdup(getsval(eval(n->list[0])));
        Map *m = array_of(n->list[1]);
        map_clear(m);
        char *fs;
        if (nargs >= 3) {
            if (n->list[2]->type == N_REGEX) fs = estrdup(n->list[2]->str);
            else fs = estrdup(getsval(eval(n->list[2])));
        } else {
            fs = estrdup(getsval(V_FS));
        }
        int cnt = split_string(s, fs, emit_field_into, m);
        free(fs);
        free(s);
        return tmp_num((double)cnt);
    }
    case B_SUB:
    case B_GSUB: {
        if (nargs < 2) fatal("sub/gsub need at least 2 arguments");
        regex_t *re = regex_of(n->list[0]);
        char *repl = estrdup(getsval(eval(n->list[1])));
        Node *tgt = (nargs >= 3) ? n->list[2] : 0;
        const char *src;
        LV lv;
        if (tgt) { lv = lvalue(tgt); src = (lv.field >= 0) ? field_str(lv.field) : getsval(lv.cell); }
        else     { lv.cell = 0; lv.field = 0; src = field_str(0); }
        char *srccopy = estrdup(src);
        char *res = 0;
        int cnt = do_subst(re, repl, srccopy, n->op == B_GSUB, &res);
        if (cnt) lv_set_str(lv, res);
        free(res);
        free(srccopy);
        free(repl);
        return tmp_num((double)cnt);
    }
    case B_MATCHF: {
        if (nargs < 2) fatal("match needs 2 arguments");
        char *s = estrdup(getsval(eval(n->list[0])));
        regex_t *re = regex_of(n->list[1]);
        regmatch_t m;
        double rs, rl;
        if (regexec(re, s, 1, &m, 0) == 0) { rs = m.rm_so + 1; rl = m.rm_eo - m.rm_so; }
        else { rs = 0; rl = -1; }
        setfval(V_RSTART, rs);
        setfval(V_RLENGTH, rl);
        free(s);
        return tmp_num(rs);
    }
    case B_SPRINTF: {
        char *s = 0;
        awk_printf(0, n->list, n->nlist, &s);
        Cell *c = tmp_str(s);
        free(s);
        return c;
    }
    case B_SIN:  return tmp_num(sin(getfval(eval(n->list[0]))));
    case B_COS:  return tmp_num(cos(getfval(eval(n->list[0]))));
    case B_ATAN2: {
        double y = getfval(eval(n->list[0]));
        double x = getfval(eval(n->list[1]));
        return tmp_num(atan2(y, x));
    }
    case B_EXP:  return tmp_num(exp(getfval(eval(n->list[0]))));
    case B_LOG:  return tmp_num(log(getfval(eval(n->list[0]))));
    case B_SQRT: return tmp_num(sqrt(getfval(eval(n->list[0]))));
    case B_INT: {
        double d = getfval(eval(n->list[0]));
        return tmp_num(d < 0 ? ceil(d) : floor(d));
    }
    case B_RAND: return tmp_num(awk_rand_next());
    case B_SRAND: {
        static double prev = 0;
        double old = prev;
        double s = nargs ? getfval(eval(n->list[0])) : 1;
        prev = s;
        awk_srand(s);
        return tmp_num(old);
    }
    case B_TOLOWER:
    case B_TOUPPER: {
        char *s = estrdup(getsval(eval(n->list[0])));
        for (char *p = s; *p; p++)
            *p = (char)(n->op == B_TOLOWER ? tolower((unsigned char)*p)
                                           : toupper((unsigned char)*p));
        Cell *c = tmp_str(s);
        free(s);
        return c;
    }
    case B_SYSTEM: {
        flush_all();
        int rc = system(getsval(eval(n->list[0])));
        return tmp_num((double)rc);
    }
    case B_CLOSE:
        if (!nargs) fatal("close needs an argument");
        return tmp_num((double)do_close(getsval(eval(n->list[0]))));
    case B_FFLUSH:
        flush_all();
        return tmp_num(0);
    }
    fatal("unknown built-in");
    return 0;
}

static Cell *call_func(Node *n) {
    Func *f = func_find(n->str);
    if (!f) fatal("calling undefined function %s", n->str);
    if (n->nlist > f->nparams)
        fatal("function %s called with %d args, declared with %d",
              f->name, n->nlist, f->nparams);

    Frame *fr = (Frame *)emalloc(sizeof *fr);
    fr->n = f->nparams;
    fr->slot  = (Cell **)emalloc(sizeof(Cell *) * (size_t)(f->nparams ? f->nparams : 1));
    fr->owned = (int *)emalloc(sizeof(int) * (size_t)(f->nparams ? f->nparams : 1));
    fr->alias = (Cell **)emalloc(sizeof(Cell *) * (size_t)(f->nparams ? f->nparams : 1));
    fr->ret = cell_new();

    /* Arguments are evaluated in the CALLER's frame. */
    for (int i = 0; i < f->nparams; i++) {
        fr->alias[i] = 0;
        if (i < n->nlist) {
            Node *a = n->list[i];
            if (a->type == N_VAR) {
                Cell *c = var_cell(a);
                if ((c->flags & C_ARR) || (c->flags & C_UNINIT)) {
                    /* Share the caller's cell. An array then propagates,
                     * as awk requires; a SCALAR written by the callee is
                     * rolled back below, because a scalar parameter is
                     * by value. */
                    fr->slot[i] = c;
                    fr->owned[i] = 0;
                    fr->alias[i] = c;
                    continue;
                }
            }
            Cell *v = eval(a);
            if (v->flags & C_ARR) fatal("can't use array %s in a scalar context", a->str ? a->str : "");
            Cell *c = cell_new();
            cell_copy(c, v);
            fr->slot[i] = c;
            fr->owned[i] = 1;
        } else {
            fr->slot[i] = cell_new();
            fr->owned[i] = 1;
        }
    }

    Frame *save = g_frame;
    g_frame = fr;
    int r = exec(f->body);
    g_frame = save;

    Cell *out = tmp();
    if (r == X_RETURN && fr->ret) cell_copy(out, fr->ret);
    if (r == X_EXIT) { g_frame = save; }

    for (int i = 0; i < fr->n; i++) {
        if (fr->owned[i]) { cell_release(fr->slot[i]); free(fr->slot[i]); }
        else if (fr->alias[i] && !(fr->alias[i]->flags & C_ARR)) {
            /* It was untyped when we aliased it and the callee did not
             * make it an array, so any scalar it wrote was a local. */
            set_uninit(fr->alias[i]);
        }
    }
    cell_release(fr->ret);
    free(fr->ret);
    free(fr->slot);
    free(fr->owned);
    free(fr->alias);
    free(fr);

    if (r == X_EXIT) {
        /* Propagate through the expression that called us. */
        flush_all();
        exit(g_exit_code);
    }
    return out;
}

static Cell *eval_getline(Node *n) {
    int rc;
    switch (n->op) {
    case GL_FILE: {
        const char *nm = getsval(eval(n->b));
        Reader *r = if_get(nm, 0);
        if (!r) return tmp_num(-1);
        rc = getline_into(r, n->a, 0, 0);
        break;
    }
    case GL_CMD: {
        const char *nm = getsval(eval(n->b));
        Reader *r = if_get(nm, 1);
        if (!r) return tmp_num(-1);
        rc = getline_into(r, n->a, 1, 0);
        break;
    }
    default:
        /* Plain getline advances the MAIN input, moving to the next file
         * when the current one runs out. */
        if (n->a) {
            for (;;) {
                if (g_main_reader && getline_into(g_main_reader, n->a, 1, 1)) { rc = 1; break; }
                if (!main_next_record()) { rc = 0; break; }
            }
        } else {
            rc = main_next_record() ? 1 : 0;
        }
        break;
    }
    return tmp_num((double)rc);
}

static Cell *eval(Node *n) {
    if (!n) return tmp();
    switch (n->type) {
    case N_NUM:   return tmp_num(n->num);
    case N_STR:   return tmp_str(n->str);
    case N_REGEX: {
        /* A bare /re/ in a value context is ($0 ~ /re/). */
        return tmp_num(regexec(n->re, field_str(0), 0, 0, 0) == 0 ? 1 : 0);
    }
    case N_VAR: {
        Cell *c = var_cell(n);
        if (c == V_NF) ensure_split();
        return c;
    }
    case N_FIELD: {
        int i = (int)getfval(eval(n->a));
        if (i < 0) fatal("attempt to access field %d", i);
        Cell *c = tmp();
        set_input_val(c, field_str(i));
        return c;
    }
    case N_INDEX: {
        Map *m = array_of(n->a);
        const char *k = subscript(n->list, n->nlist);
        return map_get(m, k);
    }
    case N_GROUPLIST:
        if (n->nlist == 1) return eval(n->list[0]);
        fatal("a parenthesised list is only valid in print or with `in'");
        return 0;
    case N_ASSIGN: {
        LV lv = lvalue(n->a);
        if (n->op == '=') {
            Cell *v = eval(n->b);
            if (v->flags & C_ARR) fatal("can't assign an array to a scalar");
            if (lv.field >= 0) {
                field_assign(lv.field, getsval(v));
                Cell *r = tmp();
                set_input_val(r, field_str(lv.field));
                return r;
            }
            /* cell_copy preserves strnum-ness, so `x = $1; if (x == 3)`
             * compares numerically exactly as `$1 == 3` does. */
            if (lv.cell == V_NF) { set_nf_from_assign(getfval(v)); return lv_get(lv); }
            cell_copy(lv.cell, v);
            return lv.cell;
        }
        {
            double a = getfval(lv_get(lv));
            double b = getfval(eval(n->b));
            double r;
            switch (n->op) {
            case '+': r = a + b; break;
            case '-': r = a - b; break;
            case '*': r = a * b; break;
            case '/': if (b == 0) fatal("division by zero"); r = a / b; break;
            case '%': if (b == 0) fatal("division by zero in %%"); r = fmod(a, b); break;
            default:  r = pow(a, b); break;
            }
            lv_set_num(lv, r);
            return tmp_num(r);
        }
    }
    case N_COND:
        return truth(eval(n->a)) ? eval(n->b) : eval(n->c);
    case N_OR:
        if (truth(eval(n->a))) return tmp_num(1);
        return tmp_num(truth(eval(n->b)) ? 1 : 0);
    case N_AND:
        if (!truth(eval(n->a))) return tmp_num(0);
        return tmp_num(truth(eval(n->b)) ? 1 : 0);
    case N_IN: {
        Map *m = array_of(n->b);
        const char *k = subscript(n->a->list, n->a->nlist);
        return tmp_num(map_find(m, k) ? 1 : 0);
    }
    case N_MATCH: {
        char *s = estrdup(getsval(eval(n->a)));
        regex_t *re = regex_of(n->b);
        int hit = (regexec(re, s, 0, 0, 0) == 0);
        free(s);
        return tmp_num((n->op ? !hit : hit) ? 1 : 0);
    }
    case N_CMP: {
        Cell *a = eval(n->a);
        /* The left value must survive evaluating the right one, which may
         * reuse the same temporary slot. */
        Cell save;
        memset(&save, 0, sizeof save);
        cell_copy(&save, a);
        Cell *b = eval(n->b);
        int c = cmp_cells(&save, b);
        free(save.str);
        int r;
        switch (n->op) {
        case '<':   r = c < 0;  break;
        case T_LE:  r = c <= 0; break;
        case T_EQ:  r = c == 0; break;
        case T_NE:  r = c != 0; break;
        case T_GE:  r = c >= 0; break;
        default:    r = c > 0;  break;
        }
        return tmp_num(r ? 1 : 0);
    }
    case N_CONCAT: {
        char *a = estrdup(getsval(eval(n->a)));
        const char *b = getsval(eval(n->b));
        size_t la = strlen(a), lb = strlen(b);
        Cell *c = tmp();
        c->str = (char *)emalloc(la + lb + 1);
        memcpy(c->str, a, la);
        memcpy(c->str + la, b, lb + 1);
        c->flags = C_STR;
        free(a);
        return c;
    }
    case N_BINOP: {
        double a = getfval(eval(n->a));
        double b = getfval(eval(n->b));
        switch (n->op) {
        case '+': return tmp_num(a + b);
        case '-': return tmp_num(a - b);
        case '*': return tmp_num(a * b);
        case '/': if (b == 0) fatal("division by zero"); return tmp_num(a / b);
        case '%': if (b == 0) fatal("division by zero in %%"); return tmp_num(fmod(a, b));
        default:  return tmp_num(pow(a, b));
        }
    }
    case N_UNARY: {
        double a = getfval(eval(n->a));
        return tmp_num(n->op == '-' ? -a : +a);
    }
    case N_NOT:
        return tmp_num(truth(eval(n->a)) ? 0 : 1);
    case N_INCDEC: {
        LV lv = lvalue(n->a);
        double old = getfval(lv_get(lv));
        double nv = (n->op == 1 || n->op == 3) ? old + 1 : old - 1;
        lv_set_num(lv, nv);
        return tmp_num((n->op <= 2) ? nv : old);
    }
    case N_CALL:    return call_func(n);
    case N_BUILTIN: return eval_builtin(n);
    case N_GETLINE: return eval_getline(n);
    }
    fatal("cannot evaluate this expression");
    return 0;
}

/* ---- printf ---- */

static void awk_printf(OFile *out, Node **args, int nargs, char **retstr) {
    char  *acc = 0;
    size_t alen = 0, acap = 0;
    char *fmt = estrdup(getsval(eval(args[0])));
    int argi = 1;

    char spec[64];
    char piece[512];

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            if (alen + 2 > acap) { acap = alen * 2 + 128; acc = (char *)erealloc(acc, acap); }
            acc[alen++] = *p;
            continue;
        }
        if (p[1] == '%') {
            if (alen + 2 > acap) { acap = alen * 2 + 128; acc = (char *)erealloc(acc, acap); }
            acc[alen++] = '%';
            p++;
            continue;
        }
        /* Copy the conversion out, expanding * from the argument list --
         * libtoby's snprintf supports * itself, but resolving it here
         * keeps the argument order right when there are two of them. */
        size_t si = 0;
        spec[si++] = '%';
        p++;
        long width = -1, prec = -1;
        int have_w = 0, have_p = 0;
        while (*p && strchr("-+ #0", *p)) { if (si < sizeof spec - 8) spec[si++] = *p; p++; }
        if (*p == '*') { width = (long)getfval(eval(args[argi < nargs ? argi++ : nargs - 1])); have_w = 1; p++; }
        else while (isdigit((unsigned char)*p)) { if (si < sizeof spec - 8) spec[si++] = *p; p++; }
        if (*p == '.') {
            p++;
            if (*p == '*') { prec = (long)getfval(eval(args[argi < nargs ? argi++ : nargs - 1])); have_p = 1; p++; }
            else {
                if (si < sizeof spec - 8) spec[si++] = '.';
                int any = 0;
                while (isdigit((unsigned char)*p)) { if (si < sizeof spec - 8) spec[si++] = *p; p++; any = 1; }
                if (!any && si < sizeof spec - 8) spec[si++] = '0';
            }
        }
        while (*p && strchr("hlLqjzt", *p)) p++;    /* length modifiers: ours */
        char conv = *p;
        if (!conv) fatal("bad printf format: trailing %%");

        /* Splice a * width/precision back in as digits. */
        char wbuf[32] = "", pbuf[32] = "";
        if (have_w) {
            if (width < 0) { /* a negative * width means left-justify */
                snprintf(wbuf, sizeof wbuf, "-%ld", -width);
            } else snprintf(wbuf, sizeof wbuf, "%ld", width);
        }
        if (have_p) snprintf(pbuf, sizeof pbuf, ".%ld", prec < 0 ? 0L : prec);

        char full[96];
        {
            spec[si] = 0;
            /* spec currently holds "%" + flags + literal width + literal
             * precision; insert the * values in the right place. */
            if (have_w || have_p) {
                char flags[16];
                size_t fi = 0;
                size_t k = 1;
                while (k < si && strchr("-+ #0", spec[k]) && fi < sizeof flags - 1)
                    flags[fi++] = spec[k++];
                flags[fi] = 0;
                snprintf(full, sizeof full, "%%%s%s%s", flags,
                         have_w ? wbuf : "", have_p ? pbuf : "");
            } else {
                snprintf(full, sizeof full, "%s", spec);
            }
        }

        /* Running out of arguments is FATAL, as it is in GNU awk. The
         * alternative -- silently supplying "" and 0 -- turns a script
         * bug into plausible-looking output, which is the failure mode
         * this whole program exists to remove. */
        if (argi >= nargs)
            fatal("not enough arguments to satisfy format string \"%s\"", fmt);
        Cell *av = eval(args[argi++]);
        size_t need;
        char *dyn = 0;
        char cbuf[8];
        switch (conv) {
        case 'd': case 'i': {
            strcat(full, "lld");
            double d = getfval(av);
            snprintf(piece, sizeof piece, full, (long long)d);
            break;
        }
        case 'o': case 'x': case 'X': case 'u': {
            char c2[4] = { 'l', 'l', conv, 0 };
            strcat(full, c2);
            double d = getfval(av);
            long long v = (long long)d;
            snprintf(piece, sizeof piece, full, (unsigned long long)v);
            break;
        }
        case 'c': {
            const char *s;
            if (cell_is_str(av)) s = getsval(av);
            else { cbuf[0] = (char)(int)getfval(av); cbuf[1] = 0; s = cbuf; }
            char one[2];
            one[0] = s[0];
            one[1] = 0;
            strcat(full, "s");
            snprintf(piece, sizeof piece, full, one);
            break;
        }
        case 's': {
            const char *s = getsval(av);
            strcat(full, "s");
            need = strlen(s) + 128;
            if (need > sizeof piece) {
                dyn = (char *)emalloc(need);
                snprintf(dyn, need, full, s);
            } else {
                snprintf(piece, sizeof piece, full, s);
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            char c2[2] = { conv, 0 };
            strcat(full, c2);
            snprintf(piece, sizeof piece, full, getfval(av));
            break;
        }
        default:
            fatal("unknown printf conversion %%%c", conv);
        }
        const char *src = dyn ? dyn : piece;
        size_t sl = strlen(src);
        if (alen + sl + 1 > acap) { acap = (alen + sl) * 2 + 128; acc = (char *)erealloc(acc, acap); }
        memcpy(acc + alen, src, sl);
        alen += sl;
        free(dyn);
    }
    if (!acc) acc = (char *)emalloc(1);
    acc[alen] = 0;
    free(fmt);
    if (retstr) *retstr = acc;
    else { of_write(out, acc, alen); free(acc); }
}

/* ---- statements ---- */

static OFile *redirect_of(Node *n) {
    if (!n->op) return &g_stdout_of;
    const char *nm = getsval(eval(n->b));
    return of_get(nm, n->op);
}

static int exec(Node *n) {
    if (!n) return X_NORMAL;
    switch (n->type) {
    case N_NOP: return X_NORMAL;
    case N_BLOCK:
        for (int i = 0; i < n->nlist; i++) {
            int r = exec(n->list[i]);
            if (r != X_NORMAL) return r;
        }
        return X_NORMAL;
    case N_EXPRSTMT:
        eval(n->a);
        return X_NORMAL;
    case N_PRINT: {
        OFile *o = redirect_of(n);
        const char *ofs = getsval(V_OFS);
        const char *ors = getsval(V_ORS);
        if (n->nlist == 0) {
            const char *s = field_str(0);
            of_write(o, s, strlen(s));
        } else {
            for (int i = 0; i < n->nlist; i++) {
                if (i) of_write(o, ofs, strlen(ofs));
                const char *s = getpsval(eval(n->list[i]));
                of_write(o, s, strlen(s));
            }
        }
        of_write(o, ors, strlen(ors));
        if (o->is_pipe || o->f == stderr) of_flush(o);
        return X_NORMAL;
    }
    case N_PRINTF: {
        OFile *o = redirect_of(n);
        awk_printf(o, n->list, n->nlist, 0);
        if (o->is_pipe || o->f == stderr) of_flush(o);
        return X_NORMAL;
    }
    case N_IF:
        if (truth(eval(n->a))) return exec(n->b);
        return exec(n->c);
    case N_WHILE:
        while (truth(eval(n->a))) {
            int r = exec(n->b);
            if (r == X_BREAK) break;
            if (r != X_NORMAL && r != X_CONT) return r;
        }
        return X_NORMAL;
    case N_DO:
        for (;;) {
            int r = exec(n->b);
            if (r == X_BREAK) break;
            if (r != X_NORMAL && r != X_CONT) return r;
            if (!truth(eval(n->a))) break;
        }
        return X_NORMAL;
    case N_FOR:
        if (n->a) eval(n->a);
        for (;;) {
            if (n->b && !truth(eval(n->b))) break;
            int r = exec(n->d);
            if (r == X_BREAK) break;
            if (r != X_NORMAL && r != X_CONT) return r;
            if (n->c) eval(n->c);
        }
        return X_NORMAL;
    case N_FORIN: {
        Map *m = array_of(n->b);
        LV lv = lvalue(n->a->list[0]);
        /* Snapshot the keys: the body may add or delete entries, and
         * walking the live chains while it does is how a for-in loop
         * turns into a crash. */
        int cnt = m->n, k = 0;
        char **keys = (char **)emalloc(sizeof(char *) * (size_t)(cnt ? cnt : 1));
        for (int i = 0; i < m->nb; i++)
            for (MapEnt *e = m->b[i]; e; e = e->next)
                if (k < cnt) keys[k++] = estrdup(e->key);
        int res = X_NORMAL;
        for (int i = 0; i < k; i++) {
            if (res == X_NORMAL) {
                if (lv.field >= 0) field_assign(lv.field, keys[i]);
                else set_input_val(lv.cell, keys[i]);
                int r = exec(n->c);
                if (r == X_BREAK) res = X_BREAK;
                else if (r != X_NORMAL && r != X_CONT) res = r;
            }
            free(keys[i]);
        }
        free(keys);
        return (res == X_BREAK) ? X_NORMAL : res;
    }
    case N_BREAK:    return X_BREAK;
    case N_CONT:     return X_CONT;
    case N_NEXT:     return X_NEXT;
    case N_NEXTFILE: return X_NEXTFILE;
    case N_EXIT:
        if (n->a) g_exit_code = (int)getfval(eval(n->a));
        return X_EXIT;
    case N_RETURN:
        if (g_frame && n->a) {
            Cell *v = eval(n->a);
            cell_release(g_frame->ret);
            cell_copy(g_frame->ret, v);
        }
        return X_RETURN;
    case N_DELETE: {
        Map *m = array_of(n->a->a);
        map_del(m, subscript(n->a->list, n->a->nlist));
        return X_NORMAL;
    }
    case N_DELETEALL: {
        Map *m = array_of(n->a);
        map_clear(m);
        return X_NORMAL;
    }
    default:
        eval(n);
        return X_NORMAL;
    }
}

/* ================================================================== */
/* rand                                                               */
/* ================================================================== */

static unsigned long long g_rngstate = 1;
static void awk_srand(double seed) { g_rngstate = (unsigned long long)seed + 1; }
static double awk_rand_next(void) {
    g_rngstate = g_rngstate * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((g_rngstate >> 11) & 0x1FFFFFFFFFFFFFULL) / 9007199254740992.0;
}

/* ================================================================== */
/* the main input loop                                                */
/* ================================================================== */

static int   g_argidx = 1;
static int   g_used_stdin;
static int   g_have_main_rules;

static int is_assignment(const char *s, const char **eq) {
    const char *p = s;
    if (!(isalpha((unsigned char)*p) || *p == '_')) return 0;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    if (*p != '=') return 0;
    *eq = p;
    return 1;
}

static void do_cmdline_assign(const char *s, const char *eq) {
    char *nm = estrndup(s, (size_t)(eq - s));
    size_t vn = strlen(eq + 1);
    char *val = (char *)emalloc(vn + 1);
    size_t m = unescape(eq + 1, vn, val);
    val[m] = 0;
    Cell *c = map_get(globals, nm);
    set_input_val(c, val);
    free(nm);
    free(val);
}

/* Advance to the next input record, opening files from ARGV as needed.
 * Returns 1 when a record was read into $0. */
static int main_next_record(void) {
    for (;;) {
        if (g_main_reader) {
            char *rec;
            if (reader_getrec(g_main_reader, &rec)) {
                set_record(rec);
                setfval(V_NR, getfval(V_NR) + 1);
                setfval(V_FNR, getfval(V_FNR) + 1);
                return 1;
            }
            if (g_main_reader->fp) {
                if (g_main_reader->is_pipe) pclose(g_main_reader->fp);
                else fclose(g_main_reader->fp);
            }
            free(g_main_reader->buf);
            free(g_main_reader);
            g_main_reader = 0;
        }

        int argc_now = (int)getfval(V_ARGC);
        while (g_argidx < argc_now) {
            char key[32];
            snprintf(key, sizeof key, "%d", g_argidx);
            Cell *a = map_find(V_ARGV->arr, key);
            const char *s = a ? getsval(a) : "";
            g_argidx++;
            if (!s[0]) continue;
            const char *eq;
            if (is_assignment(s, &eq)) { do_cmdline_assign(s, eq); continue; }
            setsval(V_FILENAME, s);
            setfval(V_FNR, 0);
            if (strcmp(s, "-") == 0) {
                g_main_reader = reader_new(0, 0, 0);
            } else {
                FILE *fp = fopen(s, "r");
                if (!fp) {
                    flush_all();
                    fprintf(stderr, "%s: can't open file %s\n", g_progname, s);
                    g_exit_code = 2;
                    continue;
                }
                g_main_reader = reader_new(fileno(fp), fp, 0);
            }
            break;
        }
        if (g_main_reader) continue;

        if (!g_used_stdin && g_argidx >= argc_now) {
            /* No file operands at all: read standard input. Only once. */
            int any_file = 0;
            for (int i = 1; i < argc_now; i++) {
                char key[32];
                snprintf(key, sizeof key, "%d", i);
                Cell *a = map_find(V_ARGV->arr, key);
                const char *s = a ? getsval(a) : "";
                const char *eq;
                if (s[0] && !is_assignment(s, &eq)) any_file = 1;
            }
            g_used_stdin = 1;
            if (!any_file) { g_main_reader = reader_new(0, 0, 0); continue; }
        }
        return 0;
    }
}

static int run_rules(void) {
    for (Rule *r = g_rules; r; r = r->next) {
        if (r->is_begin || r->is_end) continue;
        int sel;
        if (!r->pat) {
            sel = 1;
        } else if (r->pat2) {
            if (!r->range_active) {
                if (truth(eval(r->pat))) {
                    r->range_active = 1;
                    /* A range whose two patterns match the SAME record is
                     * one record long, not open-ended. */
                    if (truth(eval(r->pat2))) r->range_active = 0;
                    sel = 1;
                } else sel = 0;
            } else {
                sel = 1;
                if (truth(eval(r->pat2))) r->range_active = 0;
            }
        } else {
            sel = truth(eval(r->pat));
        }
        if (!sel) continue;
        if (!r->act) {
            const char *s = field_str(0);
            of_write(&g_stdout_of, s, strlen(s));
            const char *ors = getsval(V_ORS);
            of_write(&g_stdout_of, ors, strlen(ors));
            continue;
        }
        int rc = exec(r->act);
        if (rc == X_NEXT) return X_NORMAL;
        if (rc == X_NEXTFILE) return X_NEXTFILE;
        if (rc == X_EXIT) return X_EXIT;
    }
    return X_NORMAL;
}

/* ================================================================== */
/* main                                                               */
/* ================================================================== */

static void init_globals(char **envp) {
    globals = map_new();
    V_NR       = map_get(globals, "NR");
    V_NF       = map_get(globals, "NF");
    V_FNR      = map_get(globals, "FNR");
    V_FS       = map_get(globals, "FS");
    V_OFS      = map_get(globals, "OFS");
    V_ORS      = map_get(globals, "ORS");
    V_RS       = map_get(globals, "RS");
    V_FILENAME = map_get(globals, "FILENAME");
    V_SUBSEP   = map_get(globals, "SUBSEP");
    V_RSTART   = map_get(globals, "RSTART");
    V_RLENGTH  = map_get(globals, "RLENGTH");
    V_CONVFMT  = map_get(globals, "CONVFMT");
    V_OFMT     = map_get(globals, "OFMT");
    V_ENVIRON  = map_get(globals, "ENVIRON");
    V_ARGC     = map_get(globals, "ARGC");
    V_ARGV     = map_get(globals, "ARGV");

    setfval(V_NR, 0);
    V_NF->flags = C_NUM; V_NF->num = 0;
    setfval(V_FNR, 0);
    setsval(V_FS, " ");
    setsval(V_OFS, " ");
    setsval(V_ORS, "\n");
    setsval(V_RS, "\n");
    setsval(V_FILENAME, "");
    setsval(V_SUBSEP, "\034");
    setfval(V_RSTART, 0);
    setfval(V_RLENGTH, -1);
    setsval(V_CONVFMT, "%.6g");
    setsval(V_OFMT, "%.6g");

    V_ENVIRON->arr = map_new();
    V_ENVIRON->flags = C_ARR;
    for (char **e = envp; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        char *k = estrndup(*e, (size_t)(eq - *e));
        set_input_val(map_get(V_ENVIRON->arr, k), eq + 1);
        free(k);
    }
    V_ARGV->arr = map_new();
    V_ARGV->flags = C_ARR;

    g_stdout_of.f = stdout;
    g_stdout_of.name = (char *)"/dev/stdout";
}

static char *read_progfile(const char *path) {
    FILE *f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "r");
    if (!f) fatal("can't open program file %s", path);
    size_t cap = 4096, len = 0;
    char *b = (char *)emalloc(cap);
    for (;;) {
        if (len + 1024 > cap) { cap *= 2; b = (char *)erealloc(b, cap); }
        ssize_t got = read(fileno(f), b + len, cap - len - 1);
        if (got <= 0) break;
        len += (size_t)got;
    }
    b[len] = 0;
    if (f != stdin) fclose(f);
    return b;
}

static void usage(void) {
    fprintf(stderr,
        "usage: awk [-F fs] [-v var=value] 'program' [file ...]\n"
        "       awk [-F fs] [-v var=value] -f progfile [-f progfile] ... [file ...]\n");
    exit(2);
}

int main(int argc, char **argv, char **envp) {
    if (argc > 0 && argv[0]) {
        const char *b = argv[0];
        for (const char *p = b; *p; p++) if (*p == '/') b = p + 1;
        g_progname = b;
    }
    init_globals(envp);

    char  *prog = 0;
    size_t proglen = 0;
    const char *fsarg = 0;
    char **assigns = 0;
    int    nassign = 0;
    int    i = 1;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == 0) break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "--version") == 0) {
            printf("awk (tobyOS) 1.0 -- POSIX awk\n");
            return 0;
        }
        if (strcmp(a, "--help") == 0) usage();
        /* Options may be bundled with their argument (-F: -vx=1 -f p). */
        for (int k = 1; a[k]; k++) {
            const char *val = 0;
            char opt = a[k];
            if (opt == 'F' || opt == 'v' || opt == 'f') {
                if (a[k + 1]) val = a + k + 1;
                else if (i + 1 < argc) val = argv[++i];
                else usage();
            }
            switch (opt) {
            case 'F': fsarg = val; break;
            case 'v':
                assigns = (char **)erealloc(assigns, sizeof(char *) * (size_t)(nassign + 1));
                assigns[nassign++] = estrdup(val);
                break;
            case 'f': {
                char *src = read_progfile(val);
                size_t sl = strlen(src);
                prog = (char *)erealloc(prog, proglen + sl + 2);
                memcpy(prog + proglen, src, sl);
                proglen += sl;
                prog[proglen++] = '\n';
                prog[proglen] = 0;
                free(src);
                break;
            }
            default:
                fprintf(stderr, "%s: unknown option -%c\n", g_progname, opt);
                usage();
            }
            if (val) break;      /* consumed the rest of this argv element */
        }
    }

    if (!prog) {
        if (i >= argc) usage();
        prog = estrdup(argv[i++]);
    }

    /* -F is processed as a STRING with escapes, so -F'\t' is a tab, and
     * -F' ' really is the default splitting rule rather than one space. */
    if (fsarg) {
        size_t n = strlen(fsarg);
        char *fs = (char *)emalloc(n + 1);
        size_t m = unescape(fsarg, n, fs);
        fs[m] = 0;
        /* awk -Ft is the documented shorthand for a tab. */
        if (m == 1 && fs[0] == 't') { fs[0] = '\t'; }
        setsval(V_FS, fs);
        free(fs);
    }

    /* ARGV[0] is the program name; the operands follow. */
    set_input_val(map_get(V_ARGV->arr, "0"), "awk");
    int an = 1;
    for (int k = i; k < argc; k++, an++) {
        char key[32];
        snprintf(key, sizeof key, "%d", an);
        set_input_val(map_get(V_ARGV->arr, key), argv[k]);
    }
    setfval(V_ARGC, (double)an);

    for (int k = 0; k < nassign; k++) {
        const char *eq;
        if (!is_assignment(assigns[k], &eq))
            fatal("invalid -v argument: %s", assigns[k]);
        do_cmdline_assign(assigns[k], eq);
        free(assigns[k]);
    }
    free(assigns);

    g_src = prog;
    g_sp = prog;
    set_record("");
    g_fldcell = cell_new();
    awk_srand(0);

    p_program();

    for (Rule *r = g_rules; r; r = r->next)
        if (!r->is_begin) g_have_main_rules = 1;

    int status = X_NORMAL;
    for (Rule *r = g_rules; r; r = r->next) {
        if (!r->is_begin) continue;
        int rc = exec(r->act);
        if (rc == X_EXIT) { status = X_EXIT; break; }
    }

    if (status != X_EXIT && g_have_main_rules) {
        while (main_next_record()) {
            int rc = run_rules();
            if (rc == X_EXIT) { status = X_EXIT; break; }
            if (rc == X_NEXTFILE && g_main_reader) {
                if (g_main_reader->fp) {
                    if (g_main_reader->is_pipe) pclose(g_main_reader->fp);
                    else fclose(g_main_reader->fp);
                }
                free(g_main_reader->buf);
                free(g_main_reader);
                g_main_reader = 0;
            }
        }
    }

    g_in_end = 1;
    for (Rule *r = g_rules; r; r = r->next) {
        if (!r->is_end) continue;
        if (exec(r->act) == X_EXIT) break;
    }

    flush_all();
    for (OFile *o = g_ofiles; o; o = o->next) {
        of_flush(o);
        if (o->is_pipe) pclose(o->f);
        else if (o->f != stdout && o->f != stderr) fclose(o->f);
    }
    for (IFile *f = g_ifiles; f; f = f->next) reader_free(f->r);
    return g_exit_code;
}
