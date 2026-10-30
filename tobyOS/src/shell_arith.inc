/* ---- arithmetic, as bash evaluates it (B10, 2026-10-01) ----------------
 *
 * bash's $(( )), (( )), let, for (( )) and array subscripts all go through
 * one evaluator (expr.c): a tokenizer that reads a variable's value AS IT
 * READS THE NAME -- unless an `=` follows, when the name is an lvalue --
 * and a recursive descent over C's precedence levels, with a "noeval"
 * count that the short-circuit operators raise so `0 && (x = 44)` assigns
 * nothing. tsh had a recursive descent of its own that differed from it in
 * ways the Oils corpus kept finding:
 *
 *     $(( 36#z ))              bash 35       tsh "bad arithmetic expansion"
 *     (( 0 && (x = 44) ))      bash x kept   tsh x=44 -- both sides ran
 *     (( ++a[1] ))             bash works    tsh ran "++a[1]" as a COMMAND
 *     set -u; $(( y + 5 ))     bash "y: unbound variable", script ends
 *     (( 1[2] = 3 ))           bash "attempted assignment to non-variable"
 *
 * This is bash's evaluator, written from expr.c as the specification: the
 * same tokens (readtok), the same levels and lvalue rules, strlong's bases
 * (0x, 0, BASE# up to 64 with @ and _), noeval, the recursion through a
 * variable's value, and evalerror's words -- "EXPR: MESSAGE (error token
 * is "REST")", the shell adding "NAME: line N: " and "((: " or "let: ".
 *
 * The shell is reached only through the callbacks in sarith_env (variable
 * values, assignment, array elements, nounset), so the host harness
 * (tools/agent/sarith) compiles this file and compares it with bash. */

#ifndef SARITH_ALLOC
#define SARITH_ALLOC(n) kmalloc(n)
#define SARITH_FREE(p)  kfree(p)
#endif

/* bash computes in intmax_t: 64 bits. tsh's long is (LP64); the host
 * harness on Windows (LLP64) says long long. */
#ifndef SARITH_INT
#define SARITH_INT long
#endif
typedef SARITH_INT sa_int;
typedef unsigned SARITH_INT sa_uint;
#define SA_INT_MIN ((sa_int)((sa_uint)1 << (sizeof(sa_int) * 8 - 1)))

/* What a callback reports: done; failed (it has said why, and the
 * expression fails); or fatal (set -u met an unbound variable inside a
 * subscript -- it has said so, and the shell input ends). */
#define SARITH_CB_OK    0
#define SARITH_CB_FAIL  1
#define SARITH_CB_FATAL 2

struct sarith_env {
    /* The value of NAME, or of the element NAME[SUB] (sub != 0). *val is a
     * new string (the caller frees it with SARITH_FREE) or 0 for unset;
     * *set says whether the VARIABLE (the array, for an element) exists --
     * set -u asks that. The subscript is evaluated even when the array is
     * unset (side effects: ${w[i++]}), EXCEPT under set -u, where bash
     * reports the unset array first and never reaches the subscript. A
     * negative index past the start says "NAME: bad array subscript" and
     * reads nothing -- it does not fail. *ind is the index an indexed
     * element resolved to, -1 otherwise. */
    int (*get)(void *ctx, const char *name, const char *sub, char **val,
               bool *set, long *ind);
    /* NAME = VAL, or NAME[SUB] = VAL (ind >= 0: the index it resolved to
     * when it was read). A negative index past the start says "NAME[SUB]:
     * bad array subscript" and assigns nothing -- it does not fail. */
    int (*set)(void *ctx, const char *name, const char *sub, long ind,
               const char *val);
    /* false: no stack left for another level of recursion */
    bool (*stack_ok)(void *ctx);
    void *ctx;
    bool nounset;
};

#define SARITH_OK      0
#define SARITH_ERROR   1    /* evalerror: *msg is "EXPR: MSG (error token is "T")" */
#define SARITH_UNBOUND 2    /* set -u: *msg is the variable's name (0: a
                               subscript's evaluation has already said it) */
#define SARITH_FAILED  3    /* a callback failed and has reported it */

/* tokens: bash's */
#define SA_EQEQ    1
#define SA_NEQ     2
#define SA_LEQ     3
#define SA_GEQ     4
#define SA_STR     5
#define SA_NUM     6
#define SA_LAND    7
#define SA_LOR     8
#define SA_LSH     9
#define SA_RSH     10
#define SA_OPASSIGN 11
#define SA_COND    12
#define SA_POWER   13
#define SA_PREINC  14
#define SA_PREDEC  15
#define SA_POSTINC 16
#define SA_POSTDEC 17

#define SA_MAX_DEPTH 1024

struct sarith_lval {
    char *tokstr;           /* aliases the tokstr it was read from */
    sa_int tokval;
    long ind;
};

struct sarith {
    const struct sarith_env *env;
    char *expression;       /* this level's own copy */
    char *tp, *lasttp;
    int curtok, lasttok, assigntok;
    char *tokstr;           /* the current STR token (owned) */
    sa_int tokval;
    int noeval;
    struct sarith_lval curlval, lastlval;
    /* shared by every level of one evaluation */
    int *depth;
    int *err;
    char **msg;
};

static int g_sarith_depth;

/* ---- helpers ----------------------------------------------------------- */

static bool sa_ws(int c) { return c == ' ' || c == '\t' || c == '\n'; }
static bool sa_digit(int c) { return c >= '0' && c <= '9'; }
static bool sa_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool sa_alnum(int c) { return sa_alpha(c) || sa_digit(c); }
static bool sa_var_start(int c) { return sa_alpha(c) || c == '_'; }
static bool sa_var_char(int c) { return sa_alnum(c) || c == '_'; }

static size_t sa_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static char *sa_strdup(const char *s, size_t n) {
    char *r = (char *)SARITH_ALLOC(n + 1);
    if (!r) return 0;
    for (size_t i = 0; i < n; i++) r[i] = s[i];
    r[n] = '\0';
    return r;
}

/* itos */
static void sa_itos(sa_int v, char *buf) {
    char tmp[24];
    int n = 0;
    sa_uint u = v < 0 ? (sa_uint)0 - (sa_uint)v : (sa_uint)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    int i = 0;
    if (v < 0) buf[i++] = '-';
    while (n) buf[i++] = tmp[--n];
    buf[i] = '\0';
}

/* evalerror: the message is built at the level the error happened in --
 * bash prints the INNER expression for an error inside a variable's value */
static void sa_error(struct sarith *s, const char *msg) {
    if (*s->err) return;
    *s->err = SARITH_ERROR;
    const char *t = s->expression ? s->expression : "";
    while (*t == ' ' || *t == '\t') t++;
    const char *tok = (s->lasttp && *s->lasttp) ? s->lasttp : "";
    size_t n = sa_strlen(t) + sa_strlen(msg) + sa_strlen(tok) + 32;
    char *m = (char *)SARITH_ALLOC(n);
    if (!m) return;
    size_t k = 0;
    for (const char *q = t; *q; q++) m[k++] = *q;
    m[k++] = ':'; m[k++] = ' ';
    for (const char *q = msg; *q; q++) m[k++] = *q;
    const char *mid = " (error token is \"";
    for (const char *q = mid; *q; q++) m[k++] = *q;
    for (const char *q = tok; *q; q++) m[k++] = *q;
    m[k++] = '"'; m[k++] = ')';
    m[k] = '\0';
    *s->msg = m;
}

#define SA_BAIL(s) do { if (*(s)->err) return 0; } while (0)

/* skip_matched_pair over a subscript: the index (from p) of the `]` that
 * closes the `[` at p[0], or of the end of the string */
static size_t sa_skip_subscript(const char *p) {
    size_t i = 1;
    int count = 1;
    bool pass = false, backq = false;
    while (p[i]) {
        char c = p[i];
        if (pass) { pass = false; i++; continue; }
        if (c == '\\') { pass = true; i++; continue; }
        if (backq) { if (c == '`') backq = false; i++; continue; }
        if (c == '`') { backq = true; i++; continue; }
        if (c == '[') { count++; i++; continue; }
        if (c == ']') { if (--count == 0) break; i++; continue; }
        /* `$'...'` honours `\'`: `m[$'x\'y']=v` found no `]`, the subscript
         * went through the word's ordinary quote removal and the key came
         * out `xy` */
        if (c == '$' && p[i + 1] == '\'') {
            i += 2;
            while (p[i] && p[i] != '\'') { if (p[i] == '\\' && p[i + 1]) i++; i++; }
            if (p[i]) i++;
            continue;
        }
        if (c == '\'') {
            i++;
            while (p[i] && p[i] != '\'') i++;
            if (p[i]) i++;
            continue;
        }
        if (c == '"') {
            i++;
            while (p[i] && p[i] != '"') { if (p[i] == '\\' && p[i + 1]) i++; i++; }
            if (p[i]) i++;
            continue;
        }
        if (c == '$' && (p[i + 1] == '(' || p[i + 1] == '{')) {
            char open = p[i + 1], close = open == '(' ? ')' : '}';
            int d = 1;
            i += 2;
            while (p[i] && d) {
                if (p[i] == '\\' && p[i + 1]) { i += 2; continue; }
                if (p[i] == '\'') { i++; while (p[i] && p[i] != '\'') i++; if (p[i]) i++; continue; }
                if (p[i] == '"') { i++; while (p[i] && p[i] != '"') { if (p[i] == '\\' && p[i + 1]) i++; i++; } if (p[i]) i++; continue; }
                if (p[i] == open) d++;
                else if (p[i] == close) d--;
                i++;
            }
            continue;
        }
        i++;
    }
    return i;
}

/* strlong: 0nnn is octal, 0x hex, BASE#digits any base 2..64 */
static sa_int sa_strlong(struct sarith *s, const char *num) {
    const char *p = num;
    int base = 10;
    bool foundbase = false;
    sa_int val = 0;
    if (*p == '0') {
        p++;
        if (*p == '\0') return 0;
        if (*p == 'x' || *p == 'X') { base = 16; p++; }
        else base = 8;
        foundbase = true;
    }
    for (unsigned char c = (unsigned char)*p++; c; c = (unsigned char)*p++) {
        if (c == '#') {
            if (foundbase) { sa_error(s, "invalid number"); return 0; }
            if (val < 2 || val > 64) { sa_error(s, "invalid arithmetic base"); return 0; }
            base = (int)val;
            val = 0;
            foundbase = true;
            unsigned char nx = (unsigned char)*p;
            if (!(sa_alnum(nx) || nx == '_' || nx == '@')) {
                sa_error(s, "invalid integer constant");
                return 0;
            }
        } else if (sa_alnum(c) || c == '_' || c == '@') {
            int d;
            if (sa_digit(c)) d = c - '0';
            else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'Z') d = c - 'A' + (base <= 36 ? 10 : 36);
            else if (c == '@') d = 62;
            else d = 63;
            if (d >= base) { sa_error(s, "value too great for base"); return 0; }
            val = (sa_int)((sa_uint)val * (sa_uint)base + (sa_uint)d);
        } else {
            break;
        }
    }
    return val;
}

/* ---- the levels --------------------------------------------------------- */

static void sa_readtok(struct sarith *s);
static sa_int sa_comma(struct sarith *s);
static sa_int sa_subexpr(struct sarith *parent, const char *text);

static void sa_lval_init(struct sarith_lval *lv) {
    lv->tokstr = 0;
    lv->tokval = -1;
    lv->ind = -1;
}

/* name[sub] -> the name and the subscript text */
static void sa_split_elem(const char *tok, char **name, char **sub) {
    *name = 0;
    *sub = 0;
    size_t i = 0;
    while (tok[i] && tok[i] != '[') i++;
    *name = sa_strdup(tok, i);
    if (tok[i] == '[') {
        size_t n = sa_strlen(tok);
        size_t e = n;
        if (e > i + 1 && tok[e - 1] == ']') e--;
        *sub = sa_strdup(tok + i + 1, e - i - 1);
    }
}

/* expr_streval: a variable's value, as an expression */
static sa_int sa_streval(struct sarith *s, const char *tok, int e, struct sarith_lval *lv) {
    if (s->noeval) return 0;
    char *name = 0, *sub = 0, *val = 0;
    bool isset = false;
    long ind = -1;
    if (e == ']') sa_split_elem(tok, &name, &sub);
    else name = sa_strdup(tok, sa_strlen(tok));
    if (!name || (e == ']' && !sub)) {
        if (name) SARITH_FREE(name);
        if (sub) SARITH_FREE(sub);
        *s->err = SARITH_FAILED;
        return 0;
    }
    int rc = s->env->get(s->env->ctx, name, sub, &val, &isset, &ind);
    if (rc != SARITH_CB_OK) {
        SARITH_FREE(name);
        if (sub) SARITH_FREE(sub);
        if (val) SARITH_FREE(val);
        if (!*s->err) *s->err = rc == SARITH_CB_FATAL ? SARITH_UNBOUND : SARITH_FAILED;
        return 0;
    }
    if (!isset && s->env->nounset) {
        if (!*s->err) {
            *s->err = SARITH_UNBOUND;
            *s->msg = name;
            name = 0;
        }
        if (name) SARITH_FREE(name);
        if (sub) SARITH_FREE(sub);
        if (val) SARITH_FREE(val);
        return 0;
    }
    SARITH_FREE(name);
    if (sub) SARITH_FREE(sub);
    sa_int tval = 0;
    if (val && *val) tval = sa_subexpr(s, val);
    if (val) SARITH_FREE(val);
    if (lv) {
        lv->tokstr = (char *)tok;
        lv->tokval = tval;
        lv->ind = ind;
    }
    return tval;
}

/* bind: NAME=VAL or NAME[SUB]=VAL */
static void sa_bind(struct sarith *s, const char *lhs, long ind, sa_int v) {
    char buf[24];
    sa_itos(v, buf);
    char *name = 0, *sub = 0;
    bool elem = false;
    for (const char *q = lhs; *q; q++) if (*q == '[') { elem = true; break; }
    if (elem) sa_split_elem(lhs, &name, &sub);
    else name = sa_strdup(lhs, sa_strlen(lhs));
    if (!name || (elem && !sub)) {
        if (name) SARITH_FREE(name);
        if (sub) SARITH_FREE(sub);
        *s->err = SARITH_FAILED;
        return;
    }
    int rc = s->env->set(s->env->ctx, name, sub, ind, buf);
    SARITH_FREE(name);
    if (sub) SARITH_FREE(sub);
    if (rc != SARITH_CB_OK && !*s->err)
        *s->err = rc == SARITH_CB_FATAL ? SARITH_UNBOUND : SARITH_FAILED;
}

static bool sa_is_arithop(int c) {
    switch (c) {
    case '=': case '>': case '<': case '+': case '-': case '*': case '/':
    case '%': case '!': case '(': case ')': case '&': case '|': case '^':
    case '~': case '?': case ':': case ',':
        return true;
    }
    return false;
}

static bool sa_is_multiop(int c) {
    switch (c) {
    case SA_EQEQ: case SA_NEQ: case SA_LEQ: case SA_GEQ: case SA_LAND:
    case SA_LOR: case SA_LSH: case SA_RSH: case SA_OPASSIGN: case SA_COND:
    case SA_POWER: case SA_PREINC: case SA_PREDEC: case SA_POSTINC:
    case SA_POSTDEC:
        return true;
    }
    return false;
}

struct sa_saved {
    int curtok, lasttok;
    char *tp, *lasttp;
    sa_int tokval;
    char *tokstr;
    int noeval;
    struct sarith_lval lval;
};

static void sa_save(struct sarith *s, struct sa_saved *x) {
    x->curtok = s->curtok; x->lasttok = s->lasttok;
    x->tp = s->tp; x->lasttp = s->lasttp;
    x->tokval = s->tokval; x->tokstr = s->tokstr;
    x->noeval = s->noeval; x->lval = s->curlval;
}

static void sa_restore(struct sarith *s, const struct sa_saved *x) {
    s->curtok = x->curtok; s->lasttok = x->lasttok;
    s->tp = x->tp; s->lasttp = x->lasttp;
    s->tokval = x->tokval; s->tokstr = x->tokstr;
    s->noeval = x->noeval; s->curlval = x->lval;
}

static void sa_set_tokstr(struct sarith *s, char *t) {
    if (s->curlval.tokstr && s->curlval.tokstr == s->tokstr) sa_lval_init(&s->curlval);
    if (s->tokstr) SARITH_FREE(s->tokstr);
    s->tokstr = t;
}

/* readtok */
static void sa_readtok(struct sarith *s) {
    if (*s->err) return;
    char *cp = s->tp;
    unsigned char c = 0;
    int e = 0;
    while (cp && (c = (unsigned char)*cp) && sa_ws(c)) cp++;
    if (c) cp++;
    if (c == '\0') {
        s->lasttok = s->curtok;
        s->curtok = 0;
        s->tp = cp;
        return;
    }
    s->lasttp = s->tp = cp - 1;

    if (sa_var_start(c)) {
        while (sa_var_char(c)) c = (unsigned char)*cp++;
        c = (unsigned char)*--cp;
        if (c == '[') {
            size_t k = sa_skip_subscript(cp);
            /* under assoc_expand_once an associative array's subscript was
             * expanded once already and is taken as it is: no quote in it
             * is one -- `let "++a[80's]"` is the key 80's, where tsh found
             * no `]` (bash's assoc9.sub; logs/b42 t7) */
            if (shell_assoc_expand_once()) {
                char *nm = sa_strdup(s->tp, (size_t)(cp - s->tp));
                bool as = nm && sh_array_is_assoc(nm);
                if (nm) SARITH_FREE(nm);
                if (as) {
                    int d = 0;
                    for (k = 0; cp[k]; k++) {
                        if (cp[k] == '[') d++;
                        else if (cp[k] == ']' && --d == 0) break;
                    }
                }
            }
            if (cp[k] == ']') {
                cp += k + 1;
                c = (unsigned char)*cp;
                e = ']';
            } else {
                sa_error(s, "bad array subscript");
                return;
            }
        }
        char *t = sa_strdup(s->tp, (size_t)(cp - s->tp));
        if (!t) { *s->err = SARITH_FAILED; return; }
        sa_set_tokstr(s, t);

        /* peek at the next token: an `=` makes this name an lvalue */
        struct sa_saved ec;
        sa_save(s, &ec);
        s->tokstr = 0;
        s->tp = cp;
        s->noeval = 1;
        s->curtok = SA_STR;
        sa_readtok(s);
        int peektok = s->curtok;
        if (peektok == SA_STR && s->tokstr) SARITH_FREE(s->tokstr);
        sa_restore(s, &ec);
        if (*s->err) return;
        s->tp = cp;

        if (s->lasttok == SA_PREINC || s->lasttok == SA_PREDEC || peektok != '=') {
            s->lastlval = s->curlval;
            s->tokval = sa_streval(s, s->tokstr, e, &s->curlval);
        } else {
            s->tokval = 0;
        }
        s->lasttok = s->curtok;
        s->curtok = SA_STR;
    } else if (sa_digit(c)) {
        while (sa_alnum(c) || c == '#' || c == '@' || c == '_') c = (unsigned char)*cp++;
        c = (unsigned char)*--cp;
        /* bash cuts the expression at the number's end while it reads it,
         * so a bad constant's message shows the expression only that far */
        char keep = *cp;
        *cp = '\0';
        s->tokval = sa_strlong(s, s->tp);
        *cp = keep;
        if (*s->err) return;
        s->lasttok = s->curtok;
        s->curtok = SA_NUM;
    } else {
        unsigned char c1 = (unsigned char)*cp++;
        int tok = c;
        if (c == '=' && c1 == '=') tok = SA_EQEQ;
        else if (c == '!' && c1 == '=') tok = SA_NEQ;
        else if (c == '>' && c1 == '=') tok = SA_GEQ;
        else if (c == '<' && c1 == '=') tok = SA_LEQ;
        else if (c == '<' && c1 == '<') {
            if (*cp == '=') { s->assigntok = SA_LSH; tok = SA_OPASSIGN; cp++; }
            else tok = SA_LSH;
        } else if (c == '>' && c1 == '>') {
            if (*cp == '=') { s->assigntok = SA_RSH; tok = SA_OPASSIGN; cp++; }
            else tok = SA_RSH;
        } else if (c == '&' && c1 == '&') tok = SA_LAND;
        else if (c == '|' && c1 == '|') tok = SA_LOR;
        else if (c == '*' && c1 == '*') tok = SA_POWER;
        else if ((c == '-' || c == '+') && c1 == c && s->curtok == SA_STR)
            tok = (c == '-') ? SA_POSTDEC : SA_POSTINC;
        else if ((c == '-' || c == '+') && c1 == c && s->curtok == SA_NUM &&
                 (s->lasttok == SA_PREINC || s->lasttok == SA_PREDEC)) {
            sa_error(s, c == '-' ? "--: assignment requires lvalue"
                                 : "++: assignment requires lvalue");
            return;
        } else if ((c == '-' || c == '+') && c1 == c) {
            const char *xp = cp;
            while (*xp && sa_ws((unsigned char)*xp)) xp++;
            if (sa_var_start((unsigned char)*xp)) tok = (c == '-') ? SA_PREDEC : SA_PREINC;
            else cp--;
        } else if (c1 == '=' && (c == '*' || c == '/' || c == '%' || c == '+' ||
                                 c == '-' || c == '&' || c == '^' || c == '|')) {
            s->assigntok = c;
            tok = SA_OPASSIGN;
        } else if (!sa_is_arithop(c)) {
            cp--;
            s->tp = cp;
            if (s->curtok == 0 || sa_is_arithop(s->curtok) || sa_is_multiop(s->curtok))
                sa_error(s, "syntax error: operand expected");
            else
                sa_error(s, "syntax error: invalid arithmetic operator");
            return;
        } else {
            cp--;
        }
        s->lasttok = s->curtok;
        s->curtok = tok;
    }
    s->tp = cp;
}

static sa_int sa_assign(struct sarith *s);
static sa_int sa_cond(struct sarith *s);

static sa_int sa_comma(struct sarith *s) {
    sa_int v = sa_assign(s);
    while (!*s->err && s->curtok == ',') {
        sa_readtok(s);
        v = sa_assign(s);
    }
    return v;
}

static sa_int sa_assign(struct sarith *s) {
    sa_int value = sa_cond(s);
    SA_BAIL(s);
    if (s->curtok == '=' || s->curtok == SA_OPASSIGN) {
        bool special = s->curtok == SA_OPASSIGN;
        int op = 0;
        sa_int lvalue = 0;
        if (s->lasttok != SA_STR) { sa_error(s, "attempted assignment to non-variable"); return 0; }
        if (special) { op = s->assigntok; lvalue = value; }
        if (!s->tokstr) { sa_error(s, "syntax error in variable assignment"); return 0; }
        char *lhs = sa_strdup(s->tokstr, sa_strlen(s->tokstr));
        if (!lhs) { *s->err = SARITH_FAILED; return 0; }
        long lind = s->curlval.ind;
        sa_readtok(s);
        value = sa_assign(s);
        if (*s->err) { SARITH_FREE(lhs); return 0; }
        if (special) {
            if ((op == '/' || op == '%') && value == 0) {
                if (s->noeval == 0) { SARITH_FREE(lhs); sa_error(s, "division by 0"); return 0; }
                value = 1;
            }
            switch (op) {
            case '*': lvalue = (sa_int)((sa_uint)lvalue * (sa_uint)value); break;
            case '/': case '%':
                if (lvalue == SA_INT_MIN && value == -1)
                    lvalue = (op == '/') ? lvalue : 0;
                else
                    lvalue = (op == '/') ? lvalue / value : lvalue % value;
                break;
            case '+': lvalue = (sa_int)((sa_uint)lvalue + (sa_uint)value); break;
            case '-': lvalue = (sa_int)((sa_uint)lvalue - (sa_uint)value); break;
            case SA_LSH: lvalue = (sa_int)((sa_uint)lvalue << (value & 63)); break;
            case SA_RSH: lvalue >>= (value & 63); break;
            case '&': lvalue &= value; break;
            case '|': lvalue |= value; break;
            case '^': lvalue ^= value; break;
            default:
                SARITH_FREE(lhs);
                sa_error(s, "bug: bad expassign token");
                return 0;
            }
            value = lvalue;
        }
        if (s->noeval == 0) sa_bind(s, lhs, lind, value);
        if (s->curlval.tokstr && s->curlval.tokstr == s->tokstr) sa_lval_init(&s->curlval);
        SARITH_FREE(lhs);
        if (s->tokstr) SARITH_FREE(s->tokstr);
        s->tokstr = 0;
    }
    return value;
}

static sa_int sa_lor(struct sarith *s);

static sa_int sa_cond(struct sarith *s) {
    sa_int cval = sa_lor(s), val1 = 0, val2 = 0, rval = cval;
    SA_BAIL(s);
    if (s->curtok == '?') {
        bool set_noeval = false;
        if (cval == 0) { set_noeval = true; s->noeval++; }
        sa_readtok(s);
        SA_BAIL(s);
        if (s->curtok == 0 || s->curtok == ':') { sa_error(s, "expression expected"); return 0; }
        val1 = sa_comma(s);
        SA_BAIL(s);
        if (set_noeval) s->noeval--;
        if (s->curtok != ':') { sa_error(s, "`:' expected for conditional expression"); return 0; }
        set_noeval = false;
        if (cval) { set_noeval = true; s->noeval++; }
        sa_readtok(s);
        SA_BAIL(s);
        if (s->curtok == 0) { sa_error(s, "expression expected"); return 0; }
        val2 = sa_cond(s);
        SA_BAIL(s);
        if (set_noeval) s->noeval--;
        rval = cval ? val1 : val2;
        s->lasttok = SA_COND;
    }
    return rval;
}

static sa_int sa_land(struct sarith *s);

static sa_int sa_lor(struct sarith *s) {
    sa_int v1 = sa_land(s);
    while (!*s->err && s->curtok == SA_LOR) {
        bool set_noeval = false;
        if (v1 != 0) { s->noeval++; set_noeval = true; }
        sa_readtok(s);
        sa_int v2 = sa_land(s);
        if (set_noeval) s->noeval--;
        v1 = v1 || v2;
        s->lasttok = SA_LOR;
    }
    return v1;
}

static sa_int sa_bor(struct sarith *s);

static sa_int sa_land(struct sarith *s) {
    sa_int v1 = sa_bor(s);
    while (!*s->err && s->curtok == SA_LAND) {
        bool set_noeval = false;
        if (v1 == 0) { set_noeval = true; s->noeval++; }
        sa_readtok(s);
        sa_int v2 = sa_bor(s);
        if (set_noeval) s->noeval--;
        v1 = v1 && v2;
        s->lasttok = SA_LAND;
    }
    return v1;
}

static sa_int sa_bxor(struct sarith *s);
static sa_int sa_band(struct sarith *s);
static sa_int sa_eq(struct sarith *s);

static sa_int sa_bor(struct sarith *s) {
    sa_int v1 = sa_bxor(s);
    while (!*s->err && s->curtok == '|') {
        sa_readtok(s);
        sa_int v2 = sa_bxor(s);
        v1 = v1 | v2;
        s->lasttok = SA_NUM;
    }
    return v1;
}

static sa_int sa_bxor(struct sarith *s) {
    sa_int v1 = sa_band(s);
    while (!*s->err && s->curtok == '^') {
        sa_readtok(s);
        sa_int v2 = sa_band(s);
        v1 = v1 ^ v2;
        s->lasttok = SA_NUM;
    }
    return v1;
}

static sa_int sa_band(struct sarith *s) {
    sa_int v1 = sa_eq(s);
    while (!*s->err && s->curtok == '&') {
        sa_readtok(s);
        sa_int v2 = sa_eq(s);
        v1 = v1 & v2;
        s->lasttok = SA_NUM;
    }
    return v1;
}

static sa_int sa_rel(struct sarith *s);

static sa_int sa_eq(struct sarith *s) {
    sa_int v1 = sa_rel(s);
    while (!*s->err && (s->curtok == SA_EQEQ || s->curtok == SA_NEQ)) {
        int op = s->curtok;
        sa_readtok(s);
        sa_int v2 = sa_rel(s);
        v1 = (op == SA_EQEQ) ? (v1 == v2) : (v1 != v2);
        s->lasttok = SA_NUM;
    }
    return v1;
}

static sa_int sa_shift(struct sarith *s);

static sa_int sa_rel(struct sarith *s) {
    sa_int v1 = sa_shift(s);
    while (!*s->err && (s->curtok == SA_LEQ || s->curtok == SA_GEQ ||
                        s->curtok == '<' || s->curtok == '>')) {
        int op = s->curtok;
        sa_readtok(s);
        sa_int v2 = sa_shift(s);
        if (op == SA_LEQ) v1 = v1 <= v2;
        else if (op == SA_GEQ) v1 = v1 >= v2;
        else if (op == '<') v1 = v1 < v2;
        else v1 = v1 > v2;
        s->lasttok = SA_NUM;
    }
    return v1;
}

static sa_int sa_add(struct sarith *s);

static sa_int sa_shift(struct sarith *s) {
    sa_int v1 = sa_add(s);
    while (!*s->err && (s->curtok == SA_LSH || s->curtok == SA_RSH)) {
        int op = s->curtok;
        sa_readtok(s);
        sa_int v2 = sa_add(s);
        if (op == SA_LSH) v1 = (sa_int)((sa_uint)v1 << (v2 & 63));
        else v1 = v1 >> (v2 & 63);
        s->lasttok = SA_NUM;
    }
    return v1;
}

static sa_int sa_muldiv(struct sarith *s);

static sa_int sa_add(struct sarith *s) {
    sa_int v1 = sa_muldiv(s);
    while (!*s->err && (s->curtok == '+' || s->curtok == '-')) {
        int op = s->curtok;
        sa_readtok(s);
        sa_int v2 = sa_muldiv(s);
        if (op == '+') v1 = (sa_int)((sa_uint)v1 + (sa_uint)v2);
        else v1 = (sa_int)((sa_uint)v1 - (sa_uint)v2);
        s->lasttok = SA_NUM;
    }
    return v1;
}

static sa_int sa_power(struct sarith *s);

static sa_int sa_muldiv(struct sarith *s) {
    sa_int v1 = sa_power(s);
    while (!*s->err && (s->curtok == '*' || s->curtok == '/' || s->curtok == '%')) {
        int op = s->curtok;
        char *stp = s->tp;
        sa_readtok(s);
        sa_int v2 = sa_power(s);
        SA_BAIL(s);
        if ((op == '/' || op == '%') && v2 == 0) {
            if (s->noeval == 0) {
                s->lasttp = stp;
                while (s->lasttp && *s->lasttp && (*s->lasttp == ' ' || *s->lasttp == '\t'))
                    s->lasttp++;
                sa_error(s, "division by 0");
                return 0;
            }
            v2 = 1;
        } else if (op == '%' && v1 == SA_INT_MIN && v2 == -1) {
            v1 = 0;
            continue;
        } else if (op == '/' && v1 == SA_INT_MIN && v2 == -1) {
            v2 = 1;
        }
        if (op == '*') v1 = (sa_int)((sa_uint)v1 * (sa_uint)v2);
        else if (op == '/') v1 = v1 / v2;
        else v1 = v1 % v2;
        s->lasttok = SA_NUM;
    }
    return v1;
}

static sa_int sa_ipow(sa_int base, sa_int exp) {
    sa_uint result = 1, b = (sa_uint)base;
    while (exp) {
        if (exp & 1) result *= b;
        exp >>= 1;
        b *= b;
    }
    return (sa_int)result;
}

static sa_int sa_unary(struct sarith *s);

static sa_int sa_power(struct sarith *s) {
    sa_int v1 = sa_unary(s);
    while (!*s->err && s->curtok == SA_POWER) {
        sa_readtok(s);
        sa_int v2 = sa_power(s);
        SA_BAIL(s);
        s->lasttok = SA_NUM;
        if (v2 == 0) return 1;
        if (v2 < 0) { sa_error(s, "exponent less than 0"); return 0; }
        v1 = sa_ipow(v1, v2);
    }
    return v1;
}

static sa_int sa_primary(struct sarith *s);

static sa_int sa_unary(struct sarith *s) {
    sa_int v;
    SA_BAIL(s);
    if (s->curtok == '!') { sa_readtok(s); v = !sa_unary(s); s->lasttok = SA_NUM; }
    else if (s->curtok == '~') { sa_readtok(s); v = ~sa_unary(s); s->lasttok = SA_NUM; }
    else if (s->curtok == '-') { sa_readtok(s); v = (sa_int)((sa_uint)0 - (sa_uint)sa_unary(s)); s->lasttok = SA_NUM; }
    else if (s->curtok == '+') { sa_readtok(s); v = sa_unary(s); s->lasttok = SA_NUM; }
    else v = sa_primary(s);
    return v;
}

static sa_int sa_primary(struct sarith *s) {
    sa_int val = 0;
    SA_BAIL(s);
    if (s->curtok == SA_PREINC || s->curtok == SA_PREDEC) {
        int stok = s->lasttok = s->curtok;
        sa_readtok(s);
        SA_BAIL(s);
        if (s->curtok != SA_STR) {
            sa_error(s, "identifier expected after pre-increment or pre-decrement");
            return 0;
        }
        sa_int v2 = s->tokval + (stok == SA_PREINC ? 1 : -1);
        if (s->noeval == 0) {
            if (s->curlval.ind != -1 && s->curlval.tokstr)
                sa_bind(s, s->curlval.tokstr, s->curlval.ind, v2);
            else if (s->tokstr)
                sa_bind(s, s->tokstr, -1, v2);
        }
        SA_BAIL(s);
        val = v2;
        s->curtok = SA_NUM;           /* --x=7 is an error */
        sa_readtok(s);
    } else if (s->curtok == '(') {
        sa_readtok(s);
        val = sa_comma(s);
        SA_BAIL(s);
        if (s->curtok != ')') { sa_error(s, "missing `)'"); return 0; }
        sa_readtok(s);
    } else if (s->curtok == SA_NUM || s->curtok == SA_STR) {
        val = s->tokval;
        if (s->curtok == SA_STR) {
            struct sa_saved ec;
            sa_save(s, &ec);
            s->tokstr = 0;
            s->noeval = 1;
            sa_readtok(s);
            int stok = s->curtok;
            if (stok == SA_POSTINC || stok == SA_POSTDEC) {
                s->tokstr = ec.tokstr;
                s->noeval = ec.noeval;
                s->curlval = ec.lval;
                s->lasttok = SA_STR;
                sa_int v2 = val + (stok == SA_POSTINC ? 1 : -1);
                if (s->noeval == 0) {
                    if (s->curlval.ind != -1 && s->curlval.tokstr)
                        sa_bind(s, s->curlval.tokstr, s->curlval.ind, v2);
                    else if (s->tokstr)
                        sa_bind(s, s->tokstr, -1, v2);
                }
                s->curtok = SA_NUM;   /* x++=7 is an error */
            } else {
                if (stok == SA_STR && s->tokstr) SARITH_FREE(s->tokstr);
                sa_restore(s, &ec);
            }
            SA_BAIL(s);
        }
        sa_readtok(s);
    } else {
        sa_error(s, "syntax error: operand expected");
        return 0;
    }
    return val;
}

/* subexpr: one expression at one level -- the top, or a variable's value.
 * The caller has checked the depth (bash's pushexp). */
static sa_int sa_level(const struct sarith_env *env, const char *text, int *err,
                     char **msg) {
    struct sarith s;
    s.env = env;
    s.expression = sa_strdup(text, sa_strlen(text));
    if (!s.expression) { *err = SARITH_FAILED; return 0; }
    s.tp = s.expression;
    s.lasttp = 0;
    s.curtok = s.lasttok = s.assigntok = 0;
    s.tokstr = 0;
    s.tokval = 0;
    s.noeval = 0;
    sa_lval_init(&s.curlval);
    s.lastlval = s.curlval;
    s.err = err;
    s.msg = msg;
    g_sarith_depth++;
    sa_readtok(&s);
    sa_int val = *err ? 0 : sa_comma(&s);
    if (!*err && s.curtok != 0) sa_error(&s, "syntax error in expression");
    g_sarith_depth--;
    if (s.tokstr) SARITH_FREE(s.tokstr);
    SARITH_FREE(s.expression);
    return *err ? 0 : val;
}

static bool sa_too_deep(const struct sarith_env *env) {
    return g_sarith_depth >= SA_MAX_DEPTH ||
           (env->stack_ok && !env->stack_ok(env->ctx));
}

/* A variable's value: blank is 0; too deep is the PARENT's error, as
 * bash's pushexp reports it before the new expression is set up. */
static sa_int sa_subexpr(struct sarith *parent, const char *text) {
    const char *p = text;
    while (*p && sa_ws((unsigned char)*p)) p++;
    if (!*p) return 0;
    if (sa_too_deep(parent->env)) {
        sa_error(parent, "expression recursion level exceeded");
        return 0;
    }
    return sa_level(parent->env, text, parent->err, parent->msg);
}

/* evalexp: the value of EXPR. SARITH_OK, or what went wrong (*msg: a new
 * string the caller frees -- the message for SARITH_ERROR, the variable
 * for SARITH_UNBOUND). */
static int sarith_eval(const char *expr, const struct sarith_env *env, sa_int *out,
                       char **msg) {
    int err = SARITH_OK;
    *msg = 0;
    *out = 0;
    const char *p = expr;
    while (*p && sa_ws((unsigned char)*p)) p++;
    if (!*p) return SARITH_OK;
    if (sa_too_deep(env)) {
        struct sarith s;
        s.expression = (char *)expr;
        s.lasttp = 0;
        s.err = &err;
        s.msg = msg;
        sa_error(&s, "expression recursion level exceeded");
        return err ? err : SARITH_FAILED;
    }
    int saved_depth = g_sarith_depth;
    sa_int v = sa_level(env, expr, &err, msg);
    g_sarith_depth = saved_depth;
    if (err) return err;
    *out = v;
    return SARITH_OK;
}
