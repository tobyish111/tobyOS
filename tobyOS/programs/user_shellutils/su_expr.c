/* su_expr.c -- expr, as GNU coreutils 9.7's (logs/utilgate/staging/
 * expr.sh): the value of an expression made of the arguments.
 *
 * WHAT WAS MEASURED, in the C locale:
 *   - The grammar, loosest first: | ; & ; < <= = == != >= > ; + - ;
 *     * / % ; : ; then match STR RE, substr STR POS LEN, index STR CHARS,
 *     length STR, + TOKEN (TOKEN as it is), ( EXPR ), or any other
 *     argument as a string (an operator alone, `expr <`, is the string
 *     "<"). All left to right.
 *   - Integers are -?[0-9]+ (no blanks, no '+') and of any size: GNU's is
 *     built with GMP, so 9223372036854775807 + 1 is 9223372036854775808
 *     and nothing overflows. / truncates toward zero, % takes the sign of
 *     the dividend. A result is printed normalised; an argument passed
 *     through is printed as written (`expr 00` is "00").
 *   - Comparisons are numeric when both sides are integers, else by
 *     bytes; "1" or "0". A | B is A unless A is null or 0, else B unless
 *     that is too, else 0; A & B is A unless either is null or 0, else 0;
 *     the side that cannot matter is parsed but not evaluated (`1 | 1 /
 *     0` is 1). Null or 0 is "", or -?0+.
 *   - STR : RE matches a GNU basic regular expression anchored at the
 *     start (su_expr.c's own matcher, below): with \( \) the text of the
 *     first group (or ""), else the length matched (or 0).
 *   - substr is "" unless POS and LEN are integers with POS in 1..length
 *     and LEN >= 1; index is the first place of any of CHARS, or 0.
 *   - Exit status 0, or 1 for a null or 0 result; 2 for "syntax error:
 *     ..." (missing argument after 'X', unexpected argument 'X', expecting
 *     ')' after/instead of 'X', unexpected ')'), "non-integer argument",
 *     "division by zero" and a bad RE (glibc's messages); 3 for a failed
 *     write ("write error").
 *   - Options: only --help or --version (or a prefix of one) as the only
 *     argument; a first "--" is dropped; anything else is an operand
 *     (`expr -x` is "-x", `expr --help foo` a syntax error).
 *
 * The old expr (sbase's) used 64-bit numbers ("integer overflow"),
 * libtoby's regcomp and its own messages. */

#include "su_txio.c"

/* ======================================================================
 * The regular expressions: GNU's basic syntax, as expr has it.
 *
 * Measured (expr STR : RE): \( \) groups (\1-\9 refer back, only to a
 * group closed before in the same branch: \(a\)\|\1 is "Invalid back
 * reference"); \| alternation; * \+ \? and \{M,N\} (\{,N\} \{M,\}; at most
 * 32767, "Regular expression too big"); . [...] with [:class:] [=c=]
 * [.c.] and ranges (an empty range like [z-a] is allowed; a class as a
 * range end is "Invalid range end"); \w \W \s \S \b \B \< \> \` \'. ^ is
 * an anchor first in a branch (also after \( and \|), $ last in one;
 * elsewhere they are characters, as are * \+ \? \{ first in a branch and
 * \} { } + ? | ( ) anywhere. Any other \c is c; a trailing \ is "Trailing
 * backslash". The match is the longest one at the start (a\|ab on "ab"
 * is 2); among the ways to make it, the first in the order greedy
 * repeats and left alternatives give is the one the groups report (a
 * repeated group reports its last iteration), which is glibc's answer in
 * every case measured. A ']' first in brackets is itself; '[' (or '[^')
 * with nothing after it is "Invalid regular expression".
 * ====================================================================== */

enum { XN_CHAR, XN_SET, XN_BOL, XN_EOL, XN_GROUP, XN_BREF, XN_CAT, XN_ALT, XN_REP,
       XN_WB, XN_NWB, XN_WBEG, XN_WEND, XN_BUFB, XN_BUFE };

struct txx_node {
    int type;
    int c;                  /* XN_CHAR */
    unsigned char set[32];  /* XN_SET */
    int group;              /* XN_GROUP, XN_BREF */
    int child;              /* XN_GROUP, XN_REP */
    int min, max;           /* XN_REP; max -1: no limit */
    int *kids, nkids;       /* XN_CAT, XN_ALT */
};

static struct {
    const char *p;
    size_t len, pos;
    struct txx_node *n;
    int nn, cap;
    int ngroups;
    unsigned completed;     /* the groups closed so far, in this branch */
    int depth;
} txx;

static void txx_fail(const char *msg) __attribute__((noreturn));
static void txx_fail(const char *msg) {
    tx_error(2, 0, "%s", msg);
    exit(2);
}

static int txx_new(int type) {
    if (txx.nn == txx.cap) {
        txx.cap = txx.cap ? txx.cap * 2 : 64;
        txx.n = (struct txx_node *)realloc(txx.n, (size_t)txx.cap * sizeof *txx.n);
        if (!txx.n) tx_error(3, ENOMEM, "memory exhausted");
    }
    memset(&txx.n[txx.nn], 0, sizeof txx.n[0]);
    txx.n[txx.nn].type = type;
    return txx.nn++;
}

static void txx_kid(int parent, int kid) {
    struct txx_node *p = &txx.n[parent];
    p->kids = (int *)realloc(p->kids, (size_t)(p->nkids + 1) * sizeof *p->kids);
    if (!p->kids) tx_error(3, ENOMEM, "memory exhausted");
    p->kids[p->nkids++] = kid;
}

static void txx_setbit(unsigned char *set, int c) { set[c >> 3] |= (unsigned char)(1 << (c & 7)); }
static int txx_bit(const unsigned char *set, int c) { return set[c >> 3] >> (c & 7) & 1; }

static const char *const txx_cls[] = {
    "alnum", "alpha", "blank", "cntrl", "digit", "graph",
    "lower", "print", "punct", "space", "upper", "xdigit", 0
};

static int txx_in_class(int cls, int c) {
    int lower = c >= 'a' && c <= 'z', upper = c >= 'A' && c <= 'Z';
    int digit = c >= '0' && c <= '9', graph = c > 0x20 && c < 0x7f;
    switch (cls) {
    case 0: return lower || upper || digit;
    case 1: return lower || upper;
    case 2: return c == ' ' || c == '\t';
    case 3: return c < 0x20 || c == 0x7f;
    case 4: return digit;
    case 5: return graph;
    case 6: return lower;
    case 7: return graph || c == ' ';
    case 8: return graph && !(lower || upper || digit);
    case 9: return c == ' ' || (c >= '\t' && c <= '\r');
    case 10: return upper;
    case 11: return digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    return 0;
}

static int txx_wordc(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int txx_at(size_t i, char c) { return i < txx.len && txx.p[i] == c; }
static int txx_esc_at(size_t i, char c) { return txx_at(i, '\\') && txx_at(i + 1, c); }

/* [...] at txx.pos (on the '[') */
static int txx_bracket(void) {
    int node = txx_new(XN_SET);
    unsigned char set[32];
    memset(set, 0, sizeof set);
    size_t q = txx.pos + 1;
    int neg = 0;
    if (q < txx.len && txx.p[q] == '^') { neg = 1; q++; }
    if (q >= txx.len) txx_fail("Invalid regular expression");
    int first = 1;
    for (;;) {
        if (q >= txx.len) txx_fail("Unmatched [, [^, [:, [., or [=");
        unsigned char c = (unsigned char)txx.p[q];
        if (c == ']' && !first) { q++; break; }
        first = 0;
        int start, is_class = -1;
        if (c == '[' && q + 1 < txx.len &&
            (txx.p[q + 1] == ':' || txx.p[q + 1] == '=' || txx.p[q + 1] == '.')) {
            char kind = txx.p[q + 1];
            size_t k = q + 2;
            while (k + 1 < txx.len && !(txx.p[k] == kind && txx.p[k + 1] == ']')) k++;
            if (k + 1 >= txx.len) txx_fail("Unmatched [, [^, [:, [., or [=");
            size_t clen = k - (q + 2);
            if (kind == ':') {
                int m = 0;
                for (; txx_cls[m]; m++)
                    if (strlen(txx_cls[m]) == clen && !strncmp(txx_cls[m], txx.p + q + 2, clen)) break;
                if (!txx_cls[m]) txx_fail("Invalid character class name");
                is_class = m;
                start = -1;
            } else {
                if (clen != 1) txx_fail("Invalid collation character");
                start = (unsigned char)txx.p[q + 2];
            }
            q = k + 2;
        } else {
            start = c;
            q++;
        }
        if (q + 1 < txx.len && txx.p[q] == '-' && txx.p[q + 1] != ']') {
            if (is_class >= 0) txx_fail("Invalid range end");
            q++;
            int end;
            if (txx.p[q] == '[' && q + 1 < txx.len && txx.p[q + 1] == '.') {
                size_t k = q + 2;
                while (k + 1 < txx.len && !(txx.p[k] == '.' && txx.p[k + 1] == ']')) k++;
                if (k + 1 >= txx.len) txx_fail("Unmatched [, [^, [:, [., or [=");
                if (k - (q + 2) != 1) txx_fail("Invalid collation character");
                end = (unsigned char)txx.p[q + 2];
                q = k + 2;
            } else if (txx.p[q] == '[' && q + 1 < txx.len &&
                       (txx.p[q + 1] == ':' || txx.p[q + 1] == '=')) {
                txx_fail("Invalid range end");
            } else {
                end = (unsigned char)txx.p[q];
                q++;
            }
            for (int ch = start; ch <= end; ch++) txx_setbit(set, ch);
        } else if (is_class >= 0) {
            for (int ch = 0; ch < 256; ch++) if (txx_in_class(is_class, ch)) txx_setbit(set, ch);
        } else {
            txx_setbit(set, start);
        }
    }
    if (neg) for (int i = 0; i < 32; i++) set[i] = (unsigned char)~set[i];
    memcpy(txx.n[node].set, set, sizeof set);
    txx.pos = q;
    return node;
}

static int txx_alt(void);

/* \{M,N\} after an atom: M and N, or a message */
static void txx_interval(int *min, int *max) {
    size_t q = txx.pos + 2, close = q;
    while (close < txx.len && !txx_esc_at(close, '}')) close++;
    if (close >= txx.len) txx_fail("Unmatched \\{");
    size_t i = q;
    long a = -1, b = -1;
    int comma = 0, big = 0;
    if (i < close && isdigit((unsigned char)txx.p[i])) {
        a = 0;
        for (; i < close && isdigit((unsigned char)txx.p[i]); i++) {
            a = a * 10 + (txx.p[i] - '0');
            if (a > 32767) { big = 1; a = 32768; }
        }
    }
    if (i < close && txx.p[i] == ',') {
        comma = 1;
        i++;
        if (i < close && isdigit((unsigned char)txx.p[i])) {
            b = 0;
            for (; i < close && isdigit((unsigned char)txx.p[i]); i++) {
                b = b * 10 + (txx.p[i] - '0');
                if (b > 32767) { big = 1; b = 32768; }
            }
        }
    }
    if (i != close || (a < 0 && !comma)) txx_fail("Invalid content of \\{\\}");
    if (big) txx_fail("Regular expression too big");
    *min = a < 0 ? 0 : (int)a;
    *max = comma ? (b < 0 ? -1 : (int)b) : *min;
    if (*max >= 0 && *min > *max) txx_fail("Invalid content of \\{\\}");
    txx.pos = close + 2;
}

static int txx_branch(void) {
    int cat = txx_new(XN_CAT);
    int start = 1;
    while (txx.pos < txx.len) {
        if (txx_esc_at(txx.pos, '|')) break;
        if (txx_esc_at(txx.pos, ')')) {
            if (txx.depth == 0) txx_fail("Unmatched ) or \\)");
            break;
        }
        char c = txx.p[txx.pos];
        int atom = -1, anchor = 0;
        if (c == '^' && start) {
            atom = txx_new(XN_BOL);
            txx.pos++;
            anchor = 1;
        } else if (c == '$' && (txx.pos + 1 == txx.len || txx_esc_at(txx.pos + 1, '|') ||
                                txx_esc_at(txx.pos + 1, ')'))) {
            atom = txx_new(XN_EOL);
            txx.pos++;
        } else if (c == '.') {
            atom = txx_new(XN_SET);
            memset(txx.n[atom].set, 0xff, 32);
            txx.pos++;
        } else if (c == '[') {
            atom = txx_bracket();
        } else if (c == '\\') {
            if (txx.pos + 1 >= txx.len) txx_fail("Trailing backslash");
            char d = txx.p[txx.pos + 1];
            txx.pos += 2;
            if (d == '(') {
                int g = ++txx.ngroups;
                txx.depth++;
                int child = txx_alt();
                txx.depth--;
                if (!txx_esc_at(txx.pos, ')')) txx_fail("Unmatched ( or \\(");
                txx.pos += 2;
                if (g < 32) txx.completed |= 1u << g;
                atom = txx_new(XN_GROUP);
                txx.n[atom].group = g;
                txx.n[atom].child = child;
            } else if (d >= '1' && d <= '9') {
                int g = d - '0';
                if (!(txx.completed & (1u << g))) txx_fail("Invalid back reference");
                atom = txx_new(XN_BREF);
                txx.n[atom].group = g;
            } else if (d == 'w' || d == 'W' || d == 's' || d == 'S') {
                atom = txx_new(XN_SET);
                for (int ch = 0; ch < 256; ch++) {
                    int in = (d == 'w' || d == 'W') ? txx_wordc(ch) : txx_in_class(9, ch);
                    if (d == 'W' || d == 'S') in = !in;
                    if (in) txx_setbit(txx.n[atom].set, ch);
                }
            } else if (d == 'b') atom = txx_new(XN_WB);
            else if (d == 'B') atom = txx_new(XN_NWB);
            else if (d == '<') atom = txx_new(XN_WBEG);
            else if (d == '>') atom = txx_new(XN_WEND);
            else if (d == '`') atom = txx_new(XN_BUFB);
            else if (d == '\'') atom = txx_new(XN_BUFE);
            else {
                atom = txx_new(XN_CHAR);           /* \{ \+ \? first, \} \. \* ... */
                txx.n[atom].c = (unsigned char)d;
            }
        } else {
            atom = txx_new(XN_CHAR);               /* '*' first in a branch too */
            txx.n[atom].c = (unsigned char)c;
            txx.pos++;
        }
        if (!anchor) {
            for (;;) {
                int min, max;
                if (txx_at(txx.pos, '*')) { min = 0; max = -1; txx.pos++; }
                else if (txx_esc_at(txx.pos, '+')) { min = 1; max = -1; txx.pos += 2; }
                else if (txx_esc_at(txx.pos, '?')) { min = 0; max = 1; txx.pos += 2; }
                else if (txx_esc_at(txx.pos, '{')) txx_interval(&min, &max);
                else break;
                int rep = txx_new(XN_REP);
                txx.n[rep].child = atom;
                txx.n[rep].min = min;
                txx.n[rep].max = max;
                atom = rep;
            }
            start = 0;
        }
        txx_kid(cat, atom);
    }
    return cat;
}

static int txx_alt(void) {
    unsigned saved = txx.completed, all = 0;
    int first = txx_branch();
    all |= txx.completed;
    if (!txx_esc_at(txx.pos, '|')) return first;
    int alt = txx_new(XN_ALT);
    txx_kid(alt, first);
    while (txx_esc_at(txx.pos, '|')) {
        txx.pos += 2;
        txx.completed = saved;
        txx_kid(alt, txx_branch());
        all |= txx.completed;
    }
    txx.completed = all;
    return alt;
}

static int txx_compile(const char *re) {
    memset(&txx, 0, sizeof txx);
    txx.p = re;
    txx.len = strlen(re);
    int root = txx_alt();
    if (txx.pos < txx.len) txx_fail("Unmatched ) or \\)");
    return root;
}

/* ---- the match ---- */

enum { XK_CAT, XK_GEND, XK_REP };
struct txx_k {
    int kind, node, idx, count;
    size_t start;
    const struct txx_k *next;
};

static struct {
    const unsigned char *s;
    size_t len;
    long cap[10][2], bestcap[10][2];
    long best;
    long steps;
    int done;
} txm;

static void txx_cont(size_t pos, const struct txx_k *k);

static int txx_one(const struct txx_node *n, unsigned char c) {
    return n->type == XN_CHAR ? c == n->c : txx_bit(n->set, c);
}

static void txx_rep(int node, int count, size_t pos, const struct txx_k *k);

static void txx_run(int ni, size_t pos, const struct txx_k *k) {
    if (txm.done) return;
    if (++txm.steps > 20000000) { txm.done = 1; return; }
    const struct txx_node *n = &txx.n[ni];
    int before = pos > 0 && txx_wordc(txm.s[pos - 1]);
    int after = pos < txm.len && txx_wordc(txm.s[pos]);
    switch (n->type) {
    case XN_CHAR: case XN_SET:
        if (pos < txm.len && txx_one(n, txm.s[pos])) txx_cont(pos + 1, k);
        return;
    case XN_BOL: if (pos == 0) txx_cont(pos, k); return;
    case XN_EOL: if (pos == txm.len) txx_cont(pos, k); return;
    case XN_BUFB: if (pos == 0) txx_cont(pos, k); return;
    case XN_BUFE: if (pos == txm.len) txx_cont(pos, k); return;
    case XN_WB: if (before != after) txx_cont(pos, k); return;
    case XN_NWB: if (before == after) txx_cont(pos, k); return;
    case XN_WBEG: if (!before && after) txx_cont(pos, k); return;
    case XN_WEND: if (before && !after) txx_cont(pos, k); return;
    case XN_BREF: {
        int g = n->group;
        if (txm.cap[g][0] < 0) return;
        size_t l = (size_t)(txm.cap[g][1] - txm.cap[g][0]);
        if (pos + l <= txm.len && !memcmp(txm.s + pos, txm.s + txm.cap[g][0], l))
            txx_cont(pos + l, k);
        return;
    }
    case XN_GROUP: {
        struct txx_k nk = { XK_GEND, ni, 0, 0, pos, k };
        txx_run(n->child, pos, &nk);
        return;
    }
    case XN_CAT: {
        if (!n->nkids) { txx_cont(pos, k); return; }
        struct txx_k nk = { XK_CAT, ni, 1, 0, pos, k };
        txx_run(n->kids[0], pos, &nk);
        return;
    }
    case XN_ALT:
        for (int i = 0; i < n->nkids && !txm.done; i++) txx_run(n->kids[i], pos, k);
        return;
    case XN_REP:
        txx_rep(ni, 0, pos, k);
        return;
    }
}

static void txx_rep(int ni, int count, size_t pos, const struct txx_k *k) {
    const struct txx_node *n = &txx.n[ni];
    const struct txx_node *c = &txx.n[n->child];
    if (c->type == XN_CHAR || c->type == XN_SET) {
        /* one character at a time: as many as fit, then fewer */
        size_t m = 0;
        while ((n->max < 0 || count + (long)m < n->max) && pos + m < txm.len &&
               txx_one(c, txm.s[pos + m]))
            m++;
        for (size_t i = m + 1; i-- > 0 && !txm.done;)
            if (count + (long)i >= n->min) txx_cont(pos + i, k);
        return;
    }
    if (n->max < 0 || count < n->max) {
        struct txx_k nk = { XK_REP, ni, 0, count, pos, k };
        txx_run(n->child, pos, &nk);
    }
    if (count >= n->min) txx_cont(pos, k);
}

static void txx_cont(size_t pos, const struct txx_k *k) {
    if (txm.done) return;
    if (!k) {
        if ((long)pos > txm.best) {
            txm.best = (long)pos;
            memcpy(txm.bestcap, txm.cap, sizeof txm.cap);
            if (pos == txm.len) txm.done = 1;      /* nothing can be longer */
        }
        return;
    }
    switch (k->kind) {
    case XK_CAT: {
        const struct txx_node *n = &txx.n[k->node];
        if (k->idx < n->nkids) {
            struct txx_k nk = { XK_CAT, k->node, k->idx + 1, 0, k->start, k->next };
            txx_run(n->kids[k->idx], pos, &nk);
        } else {
            txx_cont(pos, k->next);
        }
        return;
    }
    case XK_GEND: {
        int g = txx.n[k->node].group;
        if (g > 9) { txx_cont(pos, k->next); return; }
        long s0 = txm.cap[g][0], s1 = txm.cap[g][1];
        txm.cap[g][0] = (long)k->start;
        txm.cap[g][1] = (long)pos;
        txx_cont(pos, k->next);
        txm.cap[g][0] = s0;
        txm.cap[g][1] = s1;
        return;
    }
    case XK_REP:
        /* an empty iteration counts only toward the minimum */
        if (pos == k->start && k->count >= txx.n[k->node].min) return;
        txx_rep(k->node, k->count + 1, pos, k->next);
        return;
    }
}

/* STR : RE */
static char *txe_match(const char *str, const char *re) {
    int root = txx_compile(re);
    memset(&txm, 0, sizeof txm);
    txm.s = (const unsigned char *)str;
    txm.len = strlen(str);
    txm.best = -1;
    for (int g = 0; g < 10; g++) txm.cap[g][0] = txm.cap[g][1] = -1;
    txx_run(root, 0, 0);
    char *out;
    if (txx.ngroups > 0) {
        if (txm.best >= 0 && txm.bestcap[1][0] >= 0) {
            size_t l = (size_t)(txm.bestcap[1][1] - txm.bestcap[1][0]);
            out = (char *)malloc(l + 1);
            if (!out) tx_error(3, ENOMEM, "memory exhausted");
            memcpy(out, str + txm.bestcap[1][0], l);
            out[l] = 0;
        } else {
            out = strdup("");
        }
    } else {
        out = (char *)malloc(24);
        if (out) snprintf(out, 24, "%ld", txm.best < 0 ? 0L : txm.best);
    }
    if (!out) tx_error(3, ENOMEM, "memory exhausted");
    for (int i = 0; i < txx.nn; i++) free(txx.n[i].kids);
    free(txx.n);
    txx.n = 0;
    return out;
}

/* ======================================================================
 * Integers of any size, as decimal strings.
 * ====================================================================== */

static int txe_isint(const char *s) {
    if (*s == '-') s++;
    if (!*s) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

/* the magnitude's digits without leading zeros ("0" for zero) */
static const char *txe_mag(const char *s, int *neg) {
    *neg = *s == '-';
    if (*neg) s++;
    while (s[0] == '0' && s[1]) s++;
    if (s[0] == '0') *neg = 0;
    return s;
}

static int txe_ucmp(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return la < lb ? -1 : 1;
    int c = strcmp(a, b);
    return c < 0 ? -1 : c > 0;
}

static char *txe_alloc(size_t n) {
    char *s = (char *)malloc(n);
    if (!s) tx_error(3, ENOMEM, "memory exhausted");
    return s;
}

/* digits from the end of a buffer: strip leading zeros, copy out */
static char *txe_trim(char *buf, size_t start, size_t end) {
    while (start + 1 < end && buf[start] == '0') start++;
    char *r = txe_alloc(end - start + 1);
    memcpy(r, buf + start, end - start);
    r[end - start] = 0;
    free(buf);
    return r;
}

static char *txe_uadd(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b), n = (la > lb ? la : lb) + 1;
    char *r = txe_alloc(n + 1);
    int carry = 0;
    for (size_t i = 0; i < n; i++) {
        int d = carry;
        if (i < la) d += a[la - 1 - i] - '0';
        if (i < lb) d += b[lb - 1 - i] - '0';
        r[n - 1 - i] = (char)('0' + d % 10);
        carry = d / 10;
    }
    r[n] = 0;
    return txe_trim(r, 0, n);
}

/* a - b, a >= b */
static char *txe_usub(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *r = txe_alloc(la + 1);
    int borrow = 0;
    for (size_t i = 0; i < la; i++) {
        int d = a[la - 1 - i] - '0' - borrow - (i < lb ? b[lb - 1 - i] - '0' : 0);
        borrow = d < 0;
        if (d < 0) d += 10;
        r[la - 1 - i] = (char)('0' + d);
    }
    r[la] = 0;
    return txe_trim(r, 0, la);
}

static char *txe_umul(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b), n = la + lb;
    int *acc = (int *)calloc(n, sizeof *acc);
    if (!acc) tx_error(3, ENOMEM, "memory exhausted");
    for (size_t i = 0; i < la; i++)
        for (size_t j = 0; j < lb; j++)
            acc[i + j + 1] += (a[i] - '0') * (b[j] - '0');
    for (size_t i = n; i-- > 1;) {
        acc[i - 1] += acc[i] / 10;
        acc[i] %= 10;
    }
    char *r = txe_alloc(n + 1);
    for (size_t i = 0; i < n; i++) r[i] = (char)('0' + acc[i]);
    r[n] = 0;
    free(acc);
    return txe_trim(r, 0, n);
}

/* a / b and a % b, b not zero */
static void txe_udivmod(const char *a, const char *b, char **q, char **rem) {
    size_t la = strlen(a);
    char *qq = txe_alloc(la + 1);
    char *r = strdup("0");
    if (!r) tx_error(3, ENOMEM, "memory exhausted");
    for (size_t i = 0; i < la; i++) {
        size_t lr = strlen(r);
        char *t = txe_alloc(lr + 2);
        memcpy(t, r, lr);
        t[lr] = a[i];
        t[lr + 1] = 0;
        free(r);
        r = txe_trim(t, 0, lr + 1);
        int d = 0;
        while (txe_ucmp(r, b) >= 0) {
            char *t2 = txe_usub(r, b);
            free(r);
            r = t2;
            d++;
        }
        qq[i] = (char)('0' + d);
    }
    qq[la] = 0;
    *q = txe_trim(qq, 0, la);
    *rem = r;
}

static char *txe_signed(int neg, char *mag) {
    if (!neg || !strcmp(mag, "0")) return mag;
    size_t l = strlen(mag);
    char *r = txe_alloc(l + 2);
    r[0] = '-';
    memcpy(r + 1, mag, l + 1);
    free(mag);
    return r;
}

static int txe_cmpint(const char *a, const char *b) {
    int na, nb;
    const char *ma = txe_mag(a, &na), *mb = txe_mag(b, &nb);
    if (na != nb) return na ? -1 : 1;
    int c = txe_ucmp(ma, mb);
    return na ? -c : c;
}

/* A OP B for + - * / % */
static char *txe_arith(const char *a, const char *b, char op) {
    if (!txe_isint(a) || !txe_isint(b)) tx_error(2, 0, "non-integer argument");
    int na, nb;
    const char *ma = txe_mag(a, &na), *mb = txe_mag(b, &nb);
    if (op == '-') nb = !nb && strcmp(mb, "0");
    switch (op) {
    case '+': case '-':
        if (na == nb) return txe_signed(na, txe_uadd(ma, mb));
        if (txe_ucmp(ma, mb) >= 0) return txe_signed(na, txe_usub(ma, mb));
        return txe_signed(nb, txe_usub(mb, ma));
    case '*':
        return txe_signed(na != nb, txe_umul(ma, mb));
    default: {
        if (!strcmp(mb, "0")) tx_error(2, 0, "division by zero");
        char *q, *r;
        txe_udivmod(ma, mb, &q, &r);
        if (op == '/') { free(r); return txe_signed(na != nb, q); }
        free(q);
        return txe_signed(na, r);
    }
    }
}

/* ======================================================================
 * The expression.
 * ====================================================================== */

static char **txe_av;
static int txe_ac, txe_i;

static void txe_syntax(const char *fmt, const char *arg) __attribute__((noreturn));
static void txe_syntax(const char *fmt, const char *arg) {
    tx_oflush();
    fprintf(stderr, "%s: syntax error: ", su_short);
    fprintf(stderr, fmt, arg ? tx_quote(arg) : "");
    fputc('\n', stderr);
    exit(2);
}

static int txe_null(const char *s) {
    if (!*s) return 1;
    if (*s == '-') s++;
    if (!*s) return 0;
    for (; *s; s++) if (*s != '0') return 0;
    return 1;
}

static int txe_is(const char *tok) { return txe_i < txe_ac && !strcmp(txe_av[txe_i], tok); }

static const char *txe_or(int ev);

static const char *txe_primary(int ev) {
    if (txe_i >= txe_ac) txe_syntax("missing argument after %s", txe_av[txe_i - 1]);
    const char *t = txe_av[txe_i];
    if (!strcmp(t, "(")) {
        txe_i++;
        const char *v = txe_or(ev);
        if (txe_i >= txe_ac) txe_syntax("expecting ')' after %s", txe_av[txe_i - 1]);
        if (strcmp(txe_av[txe_i], ")")) txe_syntax("expecting ')' instead of %s", txe_av[txe_i]);
        txe_i++;
        return v;
    }
    if (!strcmp(t, ")")) txe_syntax("unexpected ')'", 0);
    if (!strcmp(t, "+")) {
        txe_i++;
        if (txe_i >= txe_ac) txe_syntax("missing argument after %s", txe_av[txe_i - 1]);
        return txe_av[txe_i++];
    }
    if (!strcmp(t, "match")) {
        txe_i++;
        const char *s = txe_primary(ev), *r = txe_primary(ev);
        return ev ? txe_match(s, r) : "";
    }
    if (!strcmp(t, "length")) {
        txe_i++;
        const char *s = txe_primary(ev);
        char *o = txe_alloc(24);
        snprintf(o, 24, "%zu", strlen(s));
        return o;
    }
    if (!strcmp(t, "index")) {
        txe_i++;
        const char *s = txe_primary(ev), *chars = txe_primary(ev);
        size_t at = 0;
        for (size_t k = 0; s[k]; k++)
            if (strchr(chars, s[k])) { at = k + 1; break; }
        char *o = txe_alloc(24);
        snprintf(o, 24, "%zu", at);
        return o;
    }
    if (!strcmp(t, "substr")) {
        txe_i++;
        const char *s = txe_primary(ev), *ps = txe_primary(ev), *ls = txe_primary(ev);
        size_t sl = strlen(s);
        if (!txe_isint(ps) || !txe_isint(ls)) return "";
        char lenbuf[24];
        snprintf(lenbuf, sizeof lenbuf, "%zu", sl);
        /* POS from 1 to the length, LEN at least 1 */
        if (txe_cmpint(ps, "1") < 0 || txe_cmpint(ps, lenbuf) > 0 || txe_cmpint(ls, "1") < 0)
            return "";
        int neg;
        size_t pos = (size_t)strtoull(txe_mag(ps, &neg), 0, 10);
        size_t want = sl - pos + 1;
        snprintf(lenbuf, sizeof lenbuf, "%zu", want);
        if (txe_cmpint(ls, lenbuf) < 0) want = (size_t)strtoull(txe_mag(ls, &neg), 0, 10);
        char *o = txe_alloc(want + 1);
        memcpy(o, s + pos - 1, want);
        o[want] = 0;
        return o;
    }
    txe_i++;
    return t;
}

static const char *txe_colon(int ev) {
    const char *l = txe_primary(ev);
    while (txe_is(":")) {
        txe_i++;
        const char *r = txe_primary(ev);
        if (ev) l = txe_match(l, r);
    }
    return l;
}

static const char *txe_mul(int ev) {
    const char *l = txe_colon(ev);
    while (txe_is("*") || txe_is("/") || txe_is("%")) {
        char op = txe_av[txe_i++][0];
        const char *r = txe_colon(ev);
        if (ev) l = txe_arith(l, r, op);
    }
    return l;
}

static const char *txe_add(int ev) {
    const char *l = txe_mul(ev);
    while (txe_is("+") || txe_is("-")) {
        char op = txe_av[txe_i++][0];
        const char *r = txe_mul(ev);
        if (ev) l = txe_arith(l, r, op);
    }
    return l;
}

static const char *txe_cmp(int ev) {
    static const char *const ops[] = { "<", "<=", "=", "==", "!=", ">=", ">", 0 };
    const char *l = txe_add(ev);
    for (;;) {
        int k = -1;
        for (int m = 0; ops[m]; m++) if (txe_is(ops[m])) k = m;
        if (k < 0) return l;
        txe_i++;
        const char *r = txe_add(ev);
        if (!ev) continue;
        int c = (txe_isint(l) && txe_isint(r)) ? txe_cmpint(l, r) : strcmp(l, r);
        if (c < 0) c = -1; else if (c > 0) c = 1;
        int res = 0;
        switch (k) {
        case 0: res = c < 0; break;
        case 1: res = c <= 0; break;
        case 2: case 3: res = c == 0; break;
        case 4: res = c != 0; break;
        case 5: res = c >= 0; break;
        case 6: res = c > 0; break;
        }
        l = res ? "1" : "0";
    }
}

static const char *txe_and(int ev) {
    const char *l = txe_cmp(ev);
    while (txe_is("&")) {
        txe_i++;
        const char *r = txe_cmp(ev && !txe_null(l));
        if (ev && (txe_null(l) || txe_null(r))) l = "0";
    }
    return l;
}

static const char *txe_or(int ev) {
    const char *l = txe_and(ev);
    while (txe_is("|")) {
        txe_i++;
        const char *r = txe_and(ev && txe_null(l));
        if (ev && txe_null(l)) l = txe_null(r) ? "0" : r;
    }
    return l;
}

static void txe_help(void) {
    tx_printf("Usage: %s EXPRESSION\n"
              "  or:  %s OPTION\n"
              "\n"
              "      --help        show this help and exit\n"
              "      --version     show the version and exit\n"
              "\n"
              "Print the value of EXPRESSION. The groups below are separated by blank\n"
              "lines, each binding tighter than the one before. EXPRESSION may be:\n"
              "\n"
              "  ARG1 | ARG2       ARG1 if it is neither null nor 0, else ARG2\n"
              "\n"
              "  ARG1 & ARG2       ARG1 if neither is null or 0, else 0\n"
              "\n"
              "  ARG1 < ARG2       whether ARG1 is less than ARG2\n"
              "  ARG1 <= ARG2      whether ARG1 is less than or equal to ARG2\n"
              "  ARG1 = ARG2       whether ARG1 equals ARG2\n"
              "  ARG1 != ARG2      whether ARG1 differs from ARG2\n"
              "  ARG1 >= ARG2      whether ARG1 is greater than or equal to ARG2\n"
              "  ARG1 > ARG2       whether ARG1 is greater than ARG2\n"
              "\n"
              "  ARG1 + ARG2       the sum of ARG1 and ARG2\n"
              "  ARG1 - ARG2       ARG1 less ARG2\n"
              "\n"
              "  ARG1 * ARG2       the product of ARG1 and ARG2\n"
              "  ARG1 / ARG2       ARG1 divided by ARG2, the quotient\n"
              "  ARG1 %% ARG2       the remainder of ARG1 divided by ARG2\n"
              "\n"
              "  STRING : REGEXP   REGEXP matched at the start of STRING\n"
              "\n"
              "  match STRING REGEXP        STRING : REGEXP\n"
              "  substr STRING POS LENGTH   LENGTH characters of STRING from POS (from 1)\n"
              "  index STRING CHARS         where in STRING the first of CHARS is, or 0\n"
              "  length STRING              the length of STRING\n"
              "  + TOKEN                    TOKEN as a string, even a keyword like\n"
              "                               'match' or an operator like '/'\n"
              "\n"
              "  ( EXPRESSION )             the value of EXPRESSION\n"
              "\n"
              "Most operators must be quoted or escaped from the shell. A comparison is\n"
              "numeric when both ARGs are integers, else by the strings. A match gives\n"
              "the text matched by the first \\( \\) group (or null), or with no group\n"
              "the number of characters matched (or 0).\n"
              "\n"
              "The exit status is 0 if EXPRESSION is neither null nor 0, 1 if it is\n"
              "null or 0, 2 if it is not a valid expression, 3 on any other error.\n",
              su_name, su_name);
}

static int do_expr(int argc, char **argv) {
    tx_begin(3);
    if (argc == 2 && !strncmp(argv[1], "--", 2) && argv[1][2] && !strchr(argv[1], '=')) {
        const char *o = argv[1] + 2;
        size_t l = strlen(o);
        if (!strncmp("help", o, l)) { txe_help(); return 0; }
        if (!strncmp("version", o, l)) return tx_version();
    }
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "--")) first = 2;
    if (first >= argc) {
        tx_error(0, 0, "missing operand");
        tx_try_help(2);
    }
    txe_av = argv;
    txe_ac = argc;
    txe_i = first;
    const char *v = txe_or(1);
    if (txe_i < txe_ac) txe_syntax("unexpected argument %s", txe_av[txe_i]);
    tx_puts(v);
    tx_putc('\n');
    return txe_null(v) ? 1 : 0;
}
