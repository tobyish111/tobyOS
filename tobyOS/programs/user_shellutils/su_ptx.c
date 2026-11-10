/* su_ptx.c -- ptx, as GNU coreutils 9.7's (logs/utilgate/ptx.sh): a
 * permuted index -- every keyword of the input, sorted, each on a line of
 * its own with what came before and after it.
 *
 * Everything here was measured against gnuptx, most of it by differential
 * runs on thousands of random texts; the rules, as found:
 *
 * INPUT. Each FILE (or standard input, or -) is read whole; all are read
 * before anything is written, and one that cannot be read is fatal. A
 * file is cut into contexts by the sentence regex (-S): GNU's default is
 * [.?!][]"')}]*\($\|\t\|  \)[ \t\n]* (a sentence end), "\n" with -r or
 * -G, and an empty -S makes the whole file one context. A context runs to
 * the END of its terminator; the next one starts there. A terminator
 * found exactly where the search for it began is refused ("error: regular
 * expression has a match of length zero") -- which a file starting with
 * ".  ", or an empty line under "\n", hits. Keywords are matches of the
 * word regex (-W), searched in the context with its trailing white space
 * cut off; with no -W a word is a run of letters (or of characters not in
 * the -b file; under -G, not space, TAB or newline either). The regexes
 * are GNU emacs syntax (\( \) \| groups and alternation, + ? * operators,
 * { } literal, \w \W \s \S \< \> \b \B \` \' \1..\9, no [:class:]),
 * anchored where each search starts, the longest match winning; -f makes
 * them ignore case. -F -S -W take C escapes first (\a \b \f \n \r \t \v,
 * \0ooo, \xhhh, \c ends the string; others are kept as they are). With -r
 * the first field of each line is its reference: words in it are not
 * keywords, and a context that starts a line starts after it. -i and -o
 * files hold one word a line; an empty list is no list.
 *
 * LAYOUT. The width W (72) is cut in two halves of W/2 (W first loses the
 * widest reference and a gap when references go on the left), and every
 * field is measured in "steps" -- a whole word, or one other character:
 *
 *   keyafter  the keyword and what follows, up to W/2 - 2*|flag| columns;
 *   before    what precedes, up to W/2 - gap - 2*|flag|, found by stepping
 *             forward from a left limit at keyword - W/2 - LONGEST, where
 *             LONGEST is the longest word anywhere in the input (refused
 *             or ignored words too) -- GNU's own way, and why one long word
 *             changes every line;
 *   tail      the rest of the context after keyafter, wrapped into the
 *             left half's free columns (less the gap and one more);
 *   head      the context before "before", from the left limit, wrapped
 *             into the right half's free columns (less the gap).
 *
 * A field cut short gets the flag (-F, '/') on the side where text is
 * missing (TeX shows none). Lines sort by keyword (bytes, or folded with
 * -f), then by input order. Output is a dumb terminal's columns (the
 * default), roff ".xx" lines (-O, -G's default) or TeX "\xx" lines (-T).
 *
 * The old ptx (programs/user_coreutil) printed roff-like lines of its own
 * and took no options. */

/* ---- an emacs-syntax regex, compiled to a small NFA program ------------ */

enum { FM_PX_CHAR, FM_PX_ANY, FM_PX_SET, FM_PX_SPLIT, FM_PX_JMP, FM_PX_MATCH,
       FM_PX_ASSERT, FM_PX_SAVE, FM_PX_BREF };
enum { FM_PX_A_BOL, FM_PX_A_EOL, FM_PX_A_BUFB, FM_PX_A_BUFE, FM_PX_A_WB,
       FM_PX_A_NWB, FM_PX_A_WBEG, FM_PX_A_WEND };

struct fm_px_ins { int op, x, y; };

struct fm_px_re {
    struct fm_px_ins *prog;
    int n, cap;
    unsigned char (*sets)[32];
    int nsets, setcap;
    int ngroups, has_bref, fold;
    unsigned char first[32];    /* bytes a match can start with */
    int nullable;               /* a match can be empty */
    int *mark, *l1, *l2, gen;   /* the state lists */
    long *onpath, *caps;        /* the backtracker's (back-references) */
    long best;
};

static void fm_px_oom(void) { su_error(1, 0, "memory exhausted"); }

static void *fm_px_alloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) fm_px_oom();
    return p;
}

#define FM_PX_BIT(s, c) ((s)[(unsigned char)(c) >> 3] & (1u << ((unsigned char)(c) & 7)))
#define FM_PX_SETBIT(s, c) ((s)[(unsigned char)(c) >> 3] |= (unsigned char)(1u << ((unsigned char)(c) & 7)))

static int fm_px_isword(int c) { return isalnum(c) || c == '_'; }

static int fm_px_emit(struct fm_px_re *re, int op, int x, int y) {
    if (re->n == re->cap) {
        re->cap = re->cap ? re->cap * 2 : 64;
        re->prog = (struct fm_px_ins *)realloc(re->prog, (size_t)re->cap * sizeof *re->prog);
        if (!re->prog) fm_px_oom();
    }
    re->prog[re->n].op = op;
    re->prog[re->n].x = x;
    re->prog[re->n].y = y;
    return re->n++;
}

static int fm_px_newset(struct fm_px_re *re) {
    if (re->nsets == re->setcap) {
        re->setcap = re->setcap ? re->setcap * 2 : 8;
        re->sets = (unsigned char (*)[32])realloc(re->sets, (size_t)re->setcap * 32);
        if (!re->sets) fm_px_oom();
    }
    memset(re->sets[re->nsets], 0, 32);
    return re->nsets++;
}

/* the parse tree, emitted once the whole pattern is read */
enum { FM_PX_N_LIT, FM_PX_N_ANY, FM_PX_N_SET, FM_PX_N_CAT, FM_PX_N_ALT,
       FM_PX_N_STAR, FM_PX_N_PLUS, FM_PX_N_QUEST, FM_PX_N_GROUP,
       FM_PX_N_ASSERT, FM_PX_N_BREF, FM_PX_N_EMPTY };

struct fm_px_node {
    int kind, v;                /* the byte, set, group or assertion */
    struct fm_px_node *a, *b;   /* CAT and ALT: a then b; the rest: a */
};

struct fm_px_parse {
    const char *p, *end;
    struct fm_px_re *re;
    const char *err;
    unsigned completed;         /* groups closed so far, a bit each */
};

static struct fm_px_node *fm_px_node(int kind, int v, struct fm_px_node *a, struct fm_px_node *b) {
    struct fm_px_node *n = (struct fm_px_node *)fm_px_alloc(sizeof *n);
    n->kind = kind;
    n->v = v;
    n->a = a;
    n->b = b;
    return n;
}

static void fm_px_freenode(struct fm_px_node *n) {
    while (n) {
        struct fm_px_node *b = n->b;
        fm_px_freenode(n->a);
        free(n);
        n = b;
    }
}

static struct fm_px_node *fm_px_palt(struct fm_px_parse *ps);

#define FM_PX_EBRACK "Unmatched [, [^, [:, [., or [="

/* one element of a bracket expression -- a byte, [.c.] or [=c=] -- or -1 */
static int fm_px_belem(struct fm_px_parse *ps) {
    if (ps->p >= ps->end) { ps->err = FM_PX_EBRACK; return -1; }
    if (*ps->p == '[' && ps->p + 1 < ps->end && (ps->p[1] == '.' || ps->p[1] == '=')) {
        char kind = ps->p[1];
        const char *q = ps->p + 2, *close = 0;
        /* the element is at least one byte, so "[.].]" names ']' */
        for (const char *r = q + 1; r + 1 < ps->end; r++)
            if (r[0] == kind && r[1] == ']') { close = r; break; }
        if (!close) { ps->err = FM_PX_EBRACK; return -1; }
        if (close - q != 1) { ps->err = "Invalid collation character"; return -1; }
        ps->p = close + 2;
        return (unsigned char)*q;
    }
    return (unsigned char)*ps->p++;
}

static struct fm_px_node *fm_px_bracket(struct fm_px_parse *ps) {
    /* ps->p is past the '['; "[" and "[^" alone are another error than an
     * unclosed list, as glibc's */
    if (ps->p >= ps->end) { ps->err = "Invalid regular expression"; return 0; }
    int neg = 0;
    if (*ps->p == '^') {
        neg = 1;
        ps->p++;
        if (ps->p >= ps->end) { ps->err = "Invalid regular expression"; return 0; }
    }
    unsigned char set[32];
    memset(set, 0, sizeof set);
    int first = 1;
    for (;;) {
        if (ps->p >= ps->end) { ps->err = FM_PX_EBRACK; return 0; }
        if (*ps->p == ']' && !first) { ps->p++; break; }
        first = 0;
        int lo = fm_px_belem(ps);
        if (lo < 0) return 0;
        if (ps->p < ps->end && *ps->p == '-' && ps->p + 1 < ps->end && ps->p[1] != ']') {
            ps->p++;
            int hi = fm_px_belem(ps);
            if (hi < 0) return 0;
            for (int c = lo; c <= hi; c++) FM_PX_SETBIT(set, c);    /* z-a is empty */
        } else {
            FM_PX_SETBIT(set, lo);
        }
    }
    if (ps->re->fold)
        for (int c = 0; c < 256; c++)
            if (FM_PX_BIT(set, c)) { FM_PX_SETBIT(set, toupper(c)); FM_PX_SETBIT(set, tolower(c)); }
    if (neg) for (int i = 0; i < 32; i++) set[i] = (unsigned char)~set[i];
    int si = fm_px_newset(ps->re);
    memcpy(ps->re->sets[si], set, 32);
    return fm_px_node(FM_PX_N_SET, si, 0, 0);
}

static struct fm_px_node *fm_px_classset(struct fm_px_parse *ps, int which) {
    int si = fm_px_newset(ps->re);
    for (int c = 0; c < 256; c++) {
        int in = (which == 'w' || which == 'W') ? fm_px_isword(c) : isspace(c) != 0;
        if (which == 'W' || which == 'S') in = !in;
        if (in) FM_PX_SETBIT(ps->re->sets[si], c);
    }
    return fm_px_node(FM_PX_N_SET, si, 0, 0);
}

/* Q ends an alternative: the pattern's end, \) or \| */
static int fm_px_altend(struct fm_px_parse *ps, const char *q) {
    return q >= ps->end || (q[0] == '\\' && q + 1 < ps->end && (q[1] == ')' || q[1] == '|'));
}

/* a run of pieces, up to \|, \) or the end */
static struct fm_px_node *fm_px_pcat(struct fm_px_parse *ps) {
    struct fm_px_node **v = 0;
    int n = 0, cap = 0;
    int lastanchor = 0;         /* the last piece is a leading ^ */
    while (ps->p < ps->end && !ps->err) {
        const char *q = ps->p;
        unsigned char c = (unsigned char)*q;
        struct fm_px_node *atom = 0;
        if (c == '\\' && q + 1 < ps->end && (q[1] == '|' || q[1] == ')')) break;
        /* an operator with nothing before it is a literal, as GNU's */
        if ((c == '*' || c == '+' || c == '?') && n > 0 && !lastanchor) {
            ps->p++;
            int k = c == '*' ? FM_PX_N_STAR : c == '+' ? FM_PX_N_PLUS : FM_PX_N_QUEST;
            v[n - 1] = fm_px_node(k, 0, v[n - 1], 0);
            continue;
        }
        int anchor = 0;
        if (c == '^' && n == 0) {
            ps->p++;
            atom = fm_px_node(FM_PX_N_ASSERT, FM_PX_A_BOL, 0, 0);
            anchor = 1;
        } else if (c == '$' && fm_px_altend(ps, q + 1)) {
            ps->p++;
            atom = fm_px_node(FM_PX_N_ASSERT, FM_PX_A_EOL, 0, 0);
        } else if (c == '.') {
            ps->p++;
            atom = fm_px_node(FM_PX_N_ANY, 0, 0, 0);
        } else if (c == '[') {
            ps->p++;
            atom = fm_px_bracket(ps);
            if (!atom) break;
        } else if (c == '\\') {
            if (q + 1 >= ps->end) { ps->err = "Trailing backslash"; break; }
            unsigned char e = (unsigned char)q[1];
            ps->p += 2;
            if (e == '(') {
                int g = ++ps->re->ngroups;
                struct fm_px_node *in = fm_px_palt(ps);
                if (ps->err) { fm_px_freenode(in); break; }
                if (ps->p + 1 < ps->end && ps->p[0] == '\\' && ps->p[1] == ')') {
                    ps->p += 2;
                } else {
                    ps->err = "Unmatched ( or \\(";
                    fm_px_freenode(in);
                    break;
                }
                if (g < 32) ps->completed |= 1u << g;
                atom = fm_px_node(FM_PX_N_GROUP, g, in, 0);
            } else if (e >= '1' && e <= '9') {
                int g = e - '0';
                if (!(ps->completed & (1u << g))) { ps->err = "Invalid back reference"; break; }
                ps->re->has_bref = 1;
                atom = fm_px_node(FM_PX_N_BREF, g, 0, 0);
            } else if (e == 'w' || e == 'W' || e == 's' || e == 'S') {
                atom = fm_px_classset(ps, e);
            } else if (e == '<' || e == '>' || e == 'b' || e == 'B' || e == '`' || e == '\'') {
                int k = e == '<' ? FM_PX_A_WBEG : e == '>' ? FM_PX_A_WEND
                      : e == 'b' ? FM_PX_A_WB : e == 'B' ? FM_PX_A_NWB
                      : e == '`' ? FM_PX_A_BUFB : FM_PX_A_BUFE;
                atom = fm_px_node(FM_PX_N_ASSERT, k, 0, 0);
            } else {
                atom = fm_px_node(FM_PX_N_LIT, e, 0, 0);   /* \{ \. \\ ... */
            }
        } else {
            ps->p++;
            atom = fm_px_node(FM_PX_N_LIT, c, 0, 0);
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            v = (struct fm_px_node **)realloc(v, (size_t)cap * sizeof *v);
            if (!v) fm_px_oom();
        }
        v[n++] = atom;
        lastanchor = anchor;
    }
    struct fm_px_node *r = 0;
    if (ps->err) {
        for (int i = 0; i < n; i++) fm_px_freenode(v[i]);
    } else if (n == 0) {
        r = fm_px_node(FM_PX_N_EMPTY, 0, 0, 0);
    } else {
        for (int i = n - 1; i >= 0; i--) r = r ? fm_px_node(FM_PX_N_CAT, 0, v[i], r) : v[i];
    }
    free(v);
    return r;
}

static struct fm_px_node *fm_px_palt(struct fm_px_parse *ps) {
    /* a group closed in one alternative cannot be referred to in the next */
    unsigned start = ps->completed, all = start;
    struct fm_px_node *r = fm_px_pcat(ps);
    all |= ps->completed;
    while (!ps->err && ps->p + 1 < ps->end && ps->p[0] == '\\' && ps->p[1] == '|') {
        ps->p += 2;
        ps->completed = start;
        struct fm_px_node *b = fm_px_pcat(ps);
        all |= ps->completed;
        if (ps->err) { fm_px_freenode(r); fm_px_freenode(b); return 0; }
        r = fm_px_node(FM_PX_N_ALT, 0, r, b);
    }
    ps->completed = all;
    return r;
}

static void fm_px_gen(struct fm_px_re *re, struct fm_px_node *t) {
    int s, j;
    switch (t->kind) {
    case FM_PX_N_LIT: fm_px_emit(re, FM_PX_CHAR, t->v, 0); break;
    case FM_PX_N_ANY: fm_px_emit(re, FM_PX_ANY, 0, 0); break;
    case FM_PX_N_SET: fm_px_emit(re, FM_PX_SET, t->v, 0); break;
    case FM_PX_N_CAT: fm_px_gen(re, t->a); fm_px_gen(re, t->b); break;
    case FM_PX_N_ALT:
        s = fm_px_emit(re, FM_PX_SPLIT, 0, 0);
        re->prog[s].x = re->n;
        fm_px_gen(re, t->a);
        j = fm_px_emit(re, FM_PX_JMP, 0, 0);
        re->prog[s].y = re->n;
        fm_px_gen(re, t->b);
        re->prog[j].x = re->n;
        break;
    case FM_PX_N_STAR:
        s = fm_px_emit(re, FM_PX_SPLIT, 0, 0);
        re->prog[s].x = re->n;
        fm_px_gen(re, t->a);
        fm_px_emit(re, FM_PX_JMP, s, 0);
        re->prog[s].y = re->n;
        break;
    case FM_PX_N_PLUS:
        j = re->n;
        fm_px_gen(re, t->a);
        s = fm_px_emit(re, FM_PX_SPLIT, j, 0);
        re->prog[s].y = re->n;
        break;
    case FM_PX_N_QUEST:
        s = fm_px_emit(re, FM_PX_SPLIT, 0, 0);
        re->prog[s].x = re->n;
        fm_px_gen(re, t->a);
        re->prog[s].y = re->n;
        break;
    case FM_PX_N_GROUP:
        fm_px_emit(re, FM_PX_SAVE, 2 * t->v, 0);
        fm_px_gen(re, t->a);
        fm_px_emit(re, FM_PX_SAVE, 2 * t->v + 1, 0);
        break;
    case FM_PX_N_ASSERT: fm_px_emit(re, FM_PX_ASSERT, t->v, 0); break;
    case FM_PX_N_BREF: fm_px_emit(re, FM_PX_BREF, t->v, 0); break;
    default: break;
    }
}

/* what the first byte of a match can be (assertions taken as passing) */
static void fm_px_firstset(struct fm_px_re *re, int pc, unsigned char *seen) {
    while (pc < re->n && !seen[pc]) {
        seen[pc] = 1;
        struct fm_px_ins *in = &re->prog[pc];
        switch (in->op) {
        case FM_PX_CHAR:
            FM_PX_SETBIT(re->first, in->x);
            if (re->fold) { FM_PX_SETBIT(re->first, toupper(in->x)); FM_PX_SETBIT(re->first, tolower(in->x)); }
            return;
        case FM_PX_ANY:
            for (int c = 0; c < 256; c++) if (c != '\n') FM_PX_SETBIT(re->first, c);
            return;
        case FM_PX_SET:
            for (int i = 0; i < 32; i++) re->first[i] |= re->sets[in->x][i];
            return;
        case FM_PX_SPLIT: fm_px_firstset(re, in->x, seen); pc = in->y; break;
        case FM_PX_JMP: pc = in->x; break;
        case FM_PX_MATCH: case FM_PX_BREF: re->nullable = 1; return;
        default: pc++; break;
        }
    }
}

/* 0, or glibc's message for what is wrong with PAT */
static const char *fm_px_compile(struct fm_px_re *re, const char *pat, int fold) {
    memset(re, 0, sizeof *re);
    re->fold = fold;
    struct fm_px_parse ps;
    ps.p = pat;
    ps.end = pat + strlen(pat);
    ps.re = re;
    ps.err = 0;
    ps.completed = 0;
    struct fm_px_node *t = fm_px_palt(&ps);
    if (!ps.err && ps.p < ps.end) ps.err = "Unmatched ) or \\)";
    if (ps.err) { fm_px_freenode(t); return ps.err; }
    fm_px_gen(re, t);
    fm_px_emit(re, FM_PX_MATCH, 0, 0);
    fm_px_freenode(t);
    unsigned char *seen = (unsigned char *)fm_px_alloc((size_t)re->n);
    memset(seen, 0, (size_t)re->n);
    fm_px_firstset(re, 0, seen);
    free(seen);
    re->mark = (int *)fm_px_alloc((size_t)re->n * sizeof(int));
    re->l1 = (int *)fm_px_alloc((size_t)re->n * sizeof(int));
    re->l2 = (int *)fm_px_alloc((size_t)re->n * sizeof(int));
    for (int i = 0; i < re->n; i++) re->mark[i] = -1;
    re->onpath = (long *)fm_px_alloc((size_t)re->n * sizeof(long));
    for (int i = 0; i < re->n; i++) re->onpath[i] = -1;
    re->caps = (long *)fm_px_alloc((size_t)(2 * re->ngroups + 2) * sizeof(long));
    for (int i = 0; i < 2 * re->ngroups + 2; i++) re->caps[i] = -1;
    return 0;
}

/* the string being matched is TEXT[S, E) -- anchors are relative to it */
static int fm_px_assert(int kind, const char *text, long s, long e, long pos) {
    int pw = pos > s && fm_px_isword((unsigned char)text[pos - 1]);
    int nw = pos < e && fm_px_isword((unsigned char)text[pos]);
    switch (kind) {
    case FM_PX_A_BOL: return pos == s || text[pos - 1] == '\n';
    case FM_PX_A_EOL: return pos == e || text[pos] == '\n';
    case FM_PX_A_BUFB: return pos == s;
    case FM_PX_A_BUFE: return pos == e;
    case FM_PX_A_WB: return pw != nw;
    case FM_PX_A_NWB: return pw == nw;
    case FM_PX_A_WBEG: return !pw && nw;
    default: return pw && !nw;
    }
}

static int fm_px_chok(const struct fm_px_re *re, const struct fm_px_ins *in, unsigned char c) {
    switch (in->op) {
    case FM_PX_CHAR:
        return re->fold ? toupper(c) == toupper(in->x) : c == in->x;
    case FM_PX_ANY: return c != '\n';
    default: return FM_PX_BIT(re->sets[in->x], c) != 0;
    }
}

static void fm_px_add(struct fm_px_re *re, int *list, int *cnt, int pc,
                      const char *text, long s, long e, long pos) {
    if (re->mark[pc] == re->gen) return;
    re->mark[pc] = re->gen;
    struct fm_px_ins *in = &re->prog[pc];
    switch (in->op) {
    case FM_PX_JMP: fm_px_add(re, list, cnt, in->x, text, s, e, pos); break;
    case FM_PX_SPLIT:
        fm_px_add(re, list, cnt, in->x, text, s, e, pos);
        fm_px_add(re, list, cnt, in->y, text, s, e, pos);
        break;
    case FM_PX_SAVE: fm_px_add(re, list, cnt, pc + 1, text, s, e, pos); break;
    case FM_PX_ASSERT:
        if (fm_px_assert(in->x, text, s, e, pos)) fm_px_add(re, list, cnt, pc + 1, text, s, e, pos);
        break;
    case FM_PX_MATCH: if (pos > re->best) re->best = pos; break;
    default: list[(*cnt)++] = pc; break;
    }
}

/* back-references: every path tried, the longest end kept */
static long fm_px_bt(struct fm_px_re *re, int pc, long pos, const char *text, long s, long e) {
    for (;;) {
        struct fm_px_ins *in = &re->prog[pc];
        switch (in->op) {
        case FM_PX_CHAR: case FM_PX_ANY: case FM_PX_SET:
            if (pos < e && fm_px_chok(re, in, (unsigned char)text[pos])) { pc++; pos++; continue; }
            return -1;
        case FM_PX_JMP: pc = in->x; continue;
        case FM_PX_SPLIT: {
            /* the same split again with nothing read: a loop of nothing */
            if (re->onpath[pc] == pos) return -1;
            long old = re->onpath[pc];
            re->onpath[pc] = pos;
            long a = fm_px_bt(re, in->x, pos, text, s, e);
            long b = fm_px_bt(re, in->y, pos, text, s, e);
            re->onpath[pc] = old;
            return a > b ? a : b;
        }
        case FM_PX_SAVE: {
            long old = re->caps[in->x];
            re->caps[in->x] = pos;
            long r = fm_px_bt(re, pc + 1, pos, text, s, e);
            re->caps[in->x] = old;
            return r;
        }
        case FM_PX_ASSERT:
            if (!fm_px_assert(in->x, text, s, e, pos)) return -1;
            pc++;
            continue;
        case FM_PX_BREF: {
            long bs = re->caps[2 * in->x], be = re->caps[2 * in->x + 1];
            if (bs < 0 || be < bs) return -1;
            long len = be - bs;
            if (pos + len > e) return -1;
            for (long i = 0; i < len; i++) {
                unsigned char a = (unsigned char)text[bs + i], b = (unsigned char)text[pos + i];
                if (re->fold ? toupper(a) != toupper(b) : a != b) return -1;
            }
            pos += len;
            pc++;
            continue;
        }
        default: return pos;
        }
    }
}

/* the longest match starting at P of the string TEXT[S, E), or -1 */
static long fm_px_match(struct fm_px_re *re, const char *text, long s, long e, long p) {
    if (re->has_bref) return fm_px_bt(re, 0, p, text, s, e);
    re->best = -1;
    re->gen++;
    int n1 = 0, *l1 = re->l1, *l2 = re->l2;
    fm_px_add(re, l1, &n1, 0, text, s, e, p);
    for (long pos = p; n1 && pos < e; pos++) {
        unsigned char c = (unsigned char)text[pos];
        int n2 = 0;
        re->gen++;
        for (int i = 0; i < n1; i++) {
            struct fm_px_ins *in = &re->prog[l1[i]];
            if (fm_px_chok(re, in, c)) fm_px_add(re, l2, &n2, l1[i] + 1, text, s, e, pos + 1);
        }
        int *t = l1; l1 = l2; l2 = t;
        n1 = n2;
    }
    return re->best;
}

/* the leftmost-longest match in TEXT[S, E): 1 and *MS, *ME, or 0 */
static int fm_px_search(struct fm_px_re *re, const char *text, long s, long e, long *ms, long *me) {
    for (long p = s; p <= e; p++) {
        if (!re->nullable && (p == e || !FM_PX_BIT(re->first, (unsigned char)text[p]))) continue;
        long r = fm_px_match(re, text, s, e, p);
        if (r >= 0) { *ms = p; *me = r; return 1; }
    }
    return 0;
}

/* ---- ptx ------------------------------------------------------------------ */

struct fm_px_file { const char *name; char *buf; long len; };
struct fm_px_occ { int file; long ks, ke, cs, ce, line, rs, re_, seq; };
struct fm_px_list { char **w; size_t *len; size_t n, cap; };

static struct {
    int gnu, auto_ref, input_ref, right_refs, fold, format;    /* 0 dumb, 1 roff, 2 tex */
    long width, gap, half, before_max, keyafter_max, ref_max, maxlen;
    char *trunc;
    long tlen;
    const char *macro;
    char *sre_str, *wre_str;
    int have_sre, have_wre;
    struct fm_px_re sre, wre;
    unsigned char fastmap[256];
    struct fm_px_list ignore, only;
    struct fm_px_file *files;
    int nfiles;
    struct fm_px_occ *occ;
    long nocc, occcap;
} fm_px;

/* C escapes in an option's string, as GNU's ptx reads them: \a \b \f \n
 * \r \t \v, \0 and up to three octal digits, \x and up to three hex digits
 * (the value cut to a byte), \c ends the string, a lone \ at the end is
 * dropped, anything else is kept with its backslash. */
static char *fm_px_unescape(const char *s) {
    size_t n = strlen(s);
    char *o = (char *)fm_px_alloc(n + 1), *d = o;
    while (*s) {
        if (*s != '\\') { *d++ = *s++; continue; }
        char c = s[1];
        if (!c) { s++; break; }
        const char *map = strchr("abfnrtv", c);
        if (map && c) {
            static const char val[] = "\a\b\f\n\r\t\v";
            *d++ = val[map - "abfnrtv"];
            s += 2;
        } else if (c == 'c') {
            break;
        } else if (c == '0') {
            int v = 0, k = 0;
            s += 2;
            while (k < 3 && *s >= '0' && *s <= '7') { v = v * 8 + (*s - '0'); s++; k++; }
            *d++ = (char)v;
            if (!(v & 0xff)) { d--; break; }   /* a NUL ends it, as in C */
        } else if (c == 'x' && isxdigit((unsigned char)s[2])) {
            int v = 0, k = 0;
            s += 2;
            while (k < 3 && isxdigit((unsigned char)*s)) {
                int h = *s;
                v = v * 16 + (h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                s++;
                k++;
            }
            if (!(v & 0xff)) break;
            *d++ = (char)v;
        } else {
            *d++ = *s++;
            *d++ = *s++;
        }
    }
    *d = 0;
    return o;
}

/* -w and -g: a C integer (0x and 0 prefixes too) above zero, or -1 */
static long fm_px_num(const char *s) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) { base = 16; p += 2; }
    else if (p[0] == '0') base = 8;
    if (!isxdigit((unsigned char)*p)) return -1;
    unsigned long long v = 0;
    int any = 0;
    for (;; p++) {
        int c = (unsigned char)*p, d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && isxdigit(c)) d = (c | 0x20) - 'a' + 10;
        else break;
        if (d >= base) break;
        if (v > (0x7fffffffffffffffULL - (unsigned)d) / (unsigned)base) return -1;
        v = v * (unsigned)base + (unsigned)d;
        any = 1;
    }
    if (!any || *p || neg || v == 0) return -1;
    return (long)v;
}

/* the whole of NAME (- is standard input), or a fatal message */
static void fm_px_slurp(const char *name, char **bufp, long *lenp) {
    int fd = 0;
    if (strcmp(name, "-")) {
        fd = su_open_read(name, 0);
        if (fd < 0) su_error(1, errno, "%s", su_quotef(name));
    }
    size_t cap = 65536, len = 0;
    char *buf = (char *)fm_px_alloc(cap + 1);
    for (;;) {
        if (cap - len < 4096) {
            cap *= 2;
            buf = (char *)realloc(buf, cap + 1);
            if (!buf) fm_px_oom();
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) su_error(1, errno, "%s", su_quotef(name));
        if (r == 0) break;
        len += (size_t)r;
    }
    if (fd > 0) close(fd);
    buf[len] = 0;
    *bufp = buf;
    *lenp = (long)len;
}

static int fm_px_wcmp(const char *a, size_t la, const char *b, size_t lb) {
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (fm_px.fold) { x = toupper(x); y = toupper(y); }
        if (x != y) return x - y;
    }
    return la < lb ? -1 : la > lb;
}

static struct fm_px_list *fm_px_sortlist;
static int fm_px_lcmp(const void *a, const void *b) {
    size_t i = *(const size_t *)a, j = *(const size_t *)b;
    return fm_px_wcmp(fm_px_sortlist->w[i], fm_px_sortlist->len[i],
                      fm_px_sortlist->w[j], fm_px_sortlist->len[j]);
}

/* -i and -o: a word a line; empty lines are no words */
static void fm_px_readlist(const char *name, struct fm_px_list *l) {
    char *buf;
    long len;
    fm_px_slurp(name, &buf, &len);
    for (long i = 0; i < len;) {
        long j = i;
        while (j < len && buf[j] != '\n') j++;
        if (j > i) {
            if (l->n == l->cap) {
                l->cap = l->cap ? l->cap * 2 : 64;
                l->w = (char **)realloc(l->w, l->cap * sizeof *l->w);
                l->len = (size_t *)realloc(l->len, l->cap * sizeof *l->len);
                if (!l->w || !l->len) fm_px_oom();
            }
            l->w[l->n] = buf + i;
            l->len[l->n] = (size_t)(j - i);
            l->n++;
        }
        i = j + 1;
    }
    /* sorted, for a binary search */
    size_t *idx = (size_t *)fm_px_alloc(l->n * sizeof *idx);
    for (size_t k = 0; k < l->n; k++) idx[k] = k;
    fm_px_sortlist = l;
    qsort(idx, l->n, sizeof *idx, fm_px_lcmp);
    char **w = (char **)fm_px_alloc(l->n * sizeof *w);
    size_t *ln = (size_t *)fm_px_alloc(l->n * sizeof *ln);
    for (size_t k = 0; k < l->n; k++) { w[k] = l->w[idx[k]]; ln[k] = l->len[idx[k]]; }
    free(l->w);
    free(l->len);
    free(idx);
    l->w = w;
    l->len = ln;
}

static int fm_px_inlist(const struct fm_px_list *l, const char *s, size_t n) {
    size_t lo = 0, hi = l->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = fm_px_wcmp(l->w[mid], l->len[mid], s, n);
        if (!c) return 1;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* the length of the word at P in the string BUF[P, LIM), or 0 */
static long fm_px_at(const char *buf, long p, long lim) {
    if (fm_px.have_wre) {
        long r = fm_px_match(&fm_px.wre, buf, p, lim, p);
        return r < 0 ? 0 : r - p;
    }
    long q = p;
    while (q < lim && fm_px.fastmap[(unsigned char)buf[q]]) q++;
    return q - p;
}

/* one step: past a whole word, or one character */
static long fm_px_step(const char *buf, long p, long lim) {
    long n = fm_px_at(buf, p, lim);
    return n > 0 ? p + n : p + 1;
}

/* the next word of BUF[P, LIM): 1 and *WS, *WE, or 0 */
static int fm_px_findword(const char *buf, long p, long lim, long *ws, long *we) {
    if (fm_px.have_wre) return fm_px_search(&fm_px.wre, buf, p, lim, ws, we);
    while (p < lim && !fm_px.fastmap[(unsigned char)buf[p]]) p++;
    if (p >= lim) return 0;
    *ws = p;
    while (p < lim && fm_px.fastmap[(unsigned char)buf[p]]) p++;
    *we = p;
    return 1;
}

static int fm_px_isspace(int c) { return isspace(c) != 0; }

static void fm_px_addocc(int file, long ks, long ke, long cs, long ce, long line, long rs, long re_) {
    if (fm_px.nocc == fm_px.occcap) {
        fm_px.occcap = fm_px.occcap ? fm_px.occcap * 2 : 256;
        fm_px.occ = (struct fm_px_occ *)realloc(fm_px.occ, (size_t)fm_px.occcap * sizeof *fm_px.occ);
        if (!fm_px.occ) fm_px_oom();
    }
    struct fm_px_occ *o = &fm_px.occ[fm_px.nocc];
    o->file = file;
    o->ks = ks;
    o->ke = ke;
    o->cs = cs;
    o->ce = ce;
    o->line = line;
    o->rs = rs;
    o->re_ = re_;
    o->seq = fm_px.nocc++;
}

/* every keyword of file F.
 *
 * Lines and references (-r) are followed as GNU's ptx follows them. Line
 * 1's reference is skipped with the white space after it -- newlines too,
 * uncounted -- so a first line holding only a reference merges with the
 * next ("a NEWLINE b c": b is a keyword of line 1, its reference "a"),
 * and a word starting in that skipped stretch is no keyword. After that
 * each newline starts a line whose first field is its reference. A
 * context that starts where a counted line starts loses that reference
 * (and the white space after it) -- but only once a keyword on that
 * first line has been taken: until then the context keeps its start. */
static void fm_px_scan(int f) {
    const char *buf = fm_px.files[f].buf;
    long len = fm_px.files[f].len;
    long tpos = 0, line = 1, lstart = 0, ref_e = 0, init_end = 0;
    while (ref_e < len && !fm_px_isspace((unsigned char)buf[ref_e])) ref_e++;
    if (fm_px.input_ref) {
        tpos = ref_e;
        while (tpos < len && fm_px_isspace((unsigned char)buf[tpos])) tpos++;
        init_end = tpos;
    }
    /* counts the newlines up to TO: each starts a line and its reference */
#define FM_PX_ADVANCE(to) \
    for (; tpos < (to); tpos++) \
        if (buf[tpos] == '\n') { \
            line++; \
            lstart = tpos + 1; \
            for (ref_e = lstart; ref_e < len && !fm_px_isspace((unsigned char)buf[ref_e]); ref_e++) {} \
        }
    for (long pos = 0; pos < len;) {
        long ce = len;
        if (fm_px.have_sre) {
            long ms, me;
            if (fm_px_search(&fm_px.sre, buf, pos, len, &ms, &me)) {
                if (ms == pos)
                    su_error(1, 0, "error: regular expression has a match of length zero: %s",
                             su_quote(fm_px.sre_str));
                ce = me;
            }
        }
        long cs = pos, ctrim = ce;
        while (ctrim > cs && fm_px_isspace((unsigned char)buf[ctrim - 1])) ctrim--;
        FM_PX_ADVANCE(cs)
        long cadj = cs, start = cs;
        int adjustable = fm_px.input_ref && cs == lstart;
        if (adjustable) {
            cadj = ref_e < ce ? ref_e : ce;
            while (cadj < ce && fm_px_isspace((unsigned char)buf[cadj])) cadj++;
        }
        for (long p = cs; p < ctrim;) {
            long ws, we;
            if (!fm_px_findword(buf, p, ctrim, &ws, &we)) break;
            if (we == ws) { p = ws + 1; continue; }   /* GNU loops forever here */
            p = we;
            if (we - ws > fm_px.maxlen) fm_px.maxlen = we - ws;
            FM_PX_ADVANCE(ws)
            if (fm_px.input_ref && (ws < init_end || (ws >= lstart && ws < ref_e))) continue;
            if (fm_px.only.n && !fm_px_inlist(&fm_px.only, buf + ws, (size_t)(we - ws))) continue;
            if (fm_px.ignore.n && fm_px_inlist(&fm_px.ignore, buf + ws, (size_t)(we - ws))) continue;
            if (adjustable && lstart == cs) { start = cadj; adjustable = 0; }
            fm_px_addocc(f, ws, we, start, ce, line, lstart, ref_e);
        }
        pos = ce;
    }
#undef FM_PX_ADVANCE
}

static int fm_px_ocmp(const void *a, const void *b) {
    const struct fm_px_occ *x = (const struct fm_px_occ *)a, *y = (const struct fm_px_occ *)b;
    int c = fm_px_wcmp(fm_px.files[x->file].buf + x->ks, (size_t)(x->ke - x->ks),
                       fm_px.files[y->file].buf + y->ks, (size_t)(y->ke - y->ks));
    if (c) return c;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* an occurrence's reference: "FILE:LINE" for -A, the field for -r */
static long fm_px_ref(const struct fm_px_occ *o, char *out, size_t cap, const char **text) {
    if (fm_px.auto_ref) {
        const char *name = fm_px.files[o->file].name;
        if (!strcmp(name, "-")) name = "";
        long n = snprintf(out, cap, "%s:%ld", name, o->line);
        *text = out;
        return n < (long)cap ? n : (long)cap - 1;
    }
    *text = fm_px.files[o->file].buf + o->rs;
    return o->re_ - o->rs;
}

/* text through a format's escaping */
static void fm_px_put(const char *s, long n) {
    for (long i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (fm_px_isspace(c)) { putchar(' '); continue; }
        if (fm_px.format == 1 && c == '"') { fputs("\"\"", stdout); continue; }
        if (fm_px.format == 2) {
            if (strchr("$%&#_", c) && c) { putchar('\\'); putchar(c); continue; }
            if (c == '\\') { fputs("\\backslash{}", stdout); continue; }
            if (c == '{') { fputs("$\\{$", stdout); continue; }
            if (c == '}') { fputs("$\\}$", stdout); continue; }
        }
        putchar(c);
    }
}

static void fm_px_spaces(long n) { for (; n > 0; n--) putchar(' '); }

static void fm_px_output(const struct fm_px_occ *o) {
    const char *buf = fm_px.files[o->file].buf;
    long ks = o->ks, ke = o->ke, cs = o->cs, ce = o->ce;
    long half = fm_px.half, before_max = fm_px.before_max, keyafter_max = fm_px.keyafter_max;
    long gap = fm_px.gap;

    /* keyafter: the keyword and the steps after it that fit */
    long cee = ce;
    while (cee > cs && fm_px_isspace((unsigned char)buf[cee - 1])) cee--;
    long cur = ke;
    while (cur < cee) {
        long nxt = fm_px_step(buf, cur, cee);
        if (nxt - ks > keyafter_max) break;
        cur = nxt;
    }
    long ka_e = cur;
    while (ka_e > ks && fm_px_isspace((unsigned char)buf[ka_e - 1])) ka_e--;
    int ka_trunc = ka_e < cee;

    /* the left limit: half a line and the longest word back */
    long lfs = ks - half - fm_px.maxlen;
    if (lfs < cs) lfs = cs;
    else if (lfs > cs) lfs = fm_px_step(buf, lfs, ks);   /* a word, not into the keyword */
    /* the before field's end: white space before the keyword cut off, but
     * not back past the left limit */
    long b_e = ks;
    while (b_e > lfs && fm_px_isspace((unsigned char)buf[b_e - 1])) b_e--;

    /* before: stepped forward from the limit until it fits */
    long b_s = lfs;
    /* -G steps on past the keyword when the width leaves no room */
    while ((b_s < b_e || !fm_px.gnu) && b_s + before_max < b_e) b_s = fm_px_step(buf, b_s, b_e);
    long x = b_s;
    while (x > cs && fm_px_isspace((unsigned char)buf[x - 1])) x--;
    int b_trunc = x > cs;
    while (b_s < cee && fm_px_isspace((unsigned char)buf[b_s])) b_s++;
    /* B_S may pass B_E (white space alone before the keyword): the length
     * is then negative, and the tail gets the column, as GNU's does */

    long t_s = 0, t_e = 0;
    int t_trunc = 0;
    long tail_max = before_max - (b_e - b_s) - gap - 1;
    if (tail_max > 0) {
        t_s = ka_e;
        while (t_s < cee && fm_px_isspace((unsigned char)buf[t_s])) t_s++;
        cur = t_s;
        while (cur < cee) {
            long nxt = fm_px_step(buf, cur, cee);
            if (nxt - t_s > tail_max) break;
            cur = nxt;
        }
        t_e = cur;
        while (t_e > t_s && fm_px_isspace((unsigned char)buf[t_e - 1])) t_e--;
        t_trunc = t_e < cee && t_e > t_s;
    }

    long h_s = 0, h_e = 0;
    int h_trunc = 0;
    long head_max = keyafter_max - (ka_e - ks) - gap;
    if (head_max > 0) {
        h_e = b_s;
        while (h_e > cs && fm_px_isspace((unsigned char)buf[h_e - 1])) h_e--;
        h_s = lfs < h_e ? lfs : h_e;
        while (h_s + head_max < h_e) h_s = fm_px_step(buf, h_s, h_e);
        h_trunc = h_s > cs;
        while (h_s < h_e && fm_px_isspace((unsigned char)buf[h_s])) h_s++;
        h_trunc = h_trunc && h_e > h_s;
    }
    long tlen = t_e > t_s ? t_e - t_s : 0, blen = b_e > b_s ? b_e - b_s : 0;
    long hlen = h_e > h_s ? h_e - h_s : 0, klen = ka_e - ks;
    int fl = fm_px.tlen > 0;
    int f_tail = fl && t_trunc, f_before = fl && b_trunc && !hlen;
    int f_ka = fl && ka_trunc && !tlen, f_head = fl && h_trunc;

    int refs = fm_px.auto_ref || fm_px.input_ref;
    char rbuf[4200];
    const char *rtext = 0;
    long rlen = refs ? fm_px_ref(o, rbuf, sizeof rbuf, &rtext) : 0;

    if (fm_px.format == 1) {
        printf(".%s \"", fm_px.macro);
        fm_px_put(buf + t_s, tlen);
        if (f_tail) fputs(fm_px.trunc, stdout);
        fputs("\" \"", stdout);
        if (f_before) fputs(fm_px.trunc, stdout);
        fm_px_put(buf + b_s, blen);
        fputs("\" \"", stdout);
        fm_px_put(buf + ks, klen);
        if (f_ka) fputs(fm_px.trunc, stdout);
        fputs("\" \"", stdout);
        if (f_head) fputs(fm_px.trunc, stdout);
        fm_px_put(buf + h_s, hlen);
        putchar('"');
        if (refs) {
            fputs(" \"", stdout);
            fm_px_put(rtext, rlen);
            putchar('"');
        }
        putchar('\n');
        return;
    }
    if (fm_px.format == 2) {
        printf("\\%s {", fm_px.macro);
        fm_px_put(buf + t_s, tlen);
        fputs("}{", stdout);
        fm_px_put(buf + b_s, blen);
        fputs("}{", stdout);
        /* the keyword as far as keyafter goes: white space ending a word
         * (-W '[^ ]+' takes a newline) is trimmed off both */
        long kend = ke < ka_e ? ke : ka_e;
        fm_px_put(buf + ks, kend - ks);
        fputs("}{", stdout);
        fm_px_put(buf + kend, ka_e - kend);
        fputs("}{", stdout);
        fm_px_put(buf + h_s, hlen);
        putchar('}');
        if (refs) {
            putchar('{');
            fm_px_put(rtext, rlen);
            putchar('}');
        }
        putchar('\n');
        return;
    }
    /* the dumb terminal: [reference area] left half, gap, right half */
    if (!fm_px.right_refs) {
        long used = 0;
        if (refs) {
            fm_px_put(rtext, rlen);
            used = rlen;
            if (fm_px.auto_ref) { putchar(':'); used++; }
        }
        fm_px_spaces((refs ? fm_px.ref_max : 0) + gap - used);
    }
    /* the padding counts the before field's SIGNED length: white space
     * alone before the keyword makes it -1, and GNU's line one wider */
    long tw = tlen + (f_tail ? fm_px.tlen : 0), bw = (b_e - b_s) + (f_before ? fm_px.tlen : 0);
    fm_px_put(buf + t_s, tlen);
    if (f_tail) fputs(fm_px.trunc, stdout);
    fm_px_spaces(half - gap - tw - bw);
    if (f_before) fputs(fm_px.trunc, stdout);
    fm_px_put(buf + b_s, blen);
    fm_px_spaces(gap);
    long kw = klen + (f_ka ? fm_px.tlen : 0), hw = hlen + (f_head ? fm_px.tlen : 0);
    fm_px_put(buf + ks, klen);
    if (f_ka) fputs(fm_px.trunc, stdout);
    if (hlen || (refs && fm_px.right_refs)) {
        fm_px_spaces(half - kw - hw);
        if (f_head) fputs(fm_px.trunc, stdout);
        fm_px_put(buf + h_s, hlen);
    }
    if (refs && fm_px.right_refs) {
        fm_px_spaces(gap);
        fm_px_put(rtext, rlen);
    }
    putchar('\n');
}

/* --help and --version end as the output does: a write error is one */
static int fm_px_done(void) {
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return 0;
}

static int do_ptx(int argc, char **argv) {
    enum { FM_PX_FORMAT = 0x100 };
    /* in alphabetical order: what an ambiguous abbreviation lists */
    static const struct su_option longs[] = {
        { "auto-reference", SU_NO_ARG, 0, 'A' },
        { "break-file", SU_REQ_ARG, 0, 'b' },
        { "flag-truncation", SU_REQ_ARG, 0, 'F' },
        { "format", SU_REQ_ARG, 0, FM_PX_FORMAT },
        { "gap-size", SU_REQ_ARG, 0, 'g' },
        { "ignore-case", SU_NO_ARG, 0, 'f' },
        { "ignore-file", SU_REQ_ARG, 0, 'i' },
        { "macro-name", SU_REQ_ARG, 0, 'M' },
        { "only-file", SU_REQ_ARG, 0, 'o' },
        { "references", SU_NO_ARG, 0, 'r' },
        { "right-side-refs", SU_NO_ARG, 0, 'R' },
        { "sentence-regexp", SU_REQ_ARG, 0, 'S' },
        { "traditional", SU_NO_ARG, 0, 'G' },
        { "typeset-mode", SU_NO_ARG, 0, 't' },
        { "width", SU_REQ_ARG, 0, 'w' },
        { "word-regexp", SU_REQ_ARG, 0, 'W' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&fm_px, 0, sizeof fm_px);
    fm_px.gnu = 1;
    fm_px.width = 72;
    fm_px.gap = 3;
    fm_px.macro = "xx";
    const char *trunc_arg = "/", *sre_arg = 0, *wre_arg = 0;
    const char *break_file = 0, *ignore_file = 0, *only_file = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, "AF:GM:ORS:TW:b:i:fg:o:trw:", longs, 0)) != -1) {
        switch (c) {
        case 'A': fm_px.auto_ref = 1; break;
        case 'F': trunc_arg = su_optarg; break;
        case 'G': fm_px.gnu = 0; break;
        case 'M': fm_px.macro = su_optarg; break;
        case 'O': fm_px.format = 1; break;
        case 'R': fm_px.right_refs = 1; break;
        case 'S': sre_arg = su_optarg; break;
        case 'T': fm_px.format = 2; break;
        case 'W': wre_arg = su_optarg; break;
        case 'b': break_file = su_optarg; break;
        case 'f': fm_px.fold = 1; break;
        case 'g':
            fm_px.gap = fm_px_num(su_optarg);
            if (fm_px.gap < 0) su_error(1, 0, "invalid gap width: %s", su_quote(su_optarg));
            break;
        case 'i': ignore_file = su_optarg; break;
        case 'o': only_file = su_optarg; break;
        case 'r': fm_px.input_ref = 1; break;
        case 't': break;                    /* GNU's does nothing with it */
        case 'w':
            fm_px.width = fm_px_num(su_optarg);
            if (fm_px.width < 0) su_error(1, 0, "invalid line width: %s", su_quote(su_optarg));
            break;
        case FM_PX_FORMAT: {
            static const char *const fmts[] = { "roff", "tex", 0 };
            size_t n = strlen(su_optarg);
            int hit = -1, hits = 0;
            for (int i = 0; fmts[i]; i++) {
                if (!strcmp(fmts[i], su_optarg)) { hit = i; hits = 1; break; }
                if (!strncmp(fmts[i], su_optarg, n)) { hit = i; hits++; }
            }
            if (hits != 1) {
                fflush(stdout);
                fprintf(stderr, "%s: %s argument %s for %s\nValid arguments are:\n", su_short,
                        hits ? "ambiguous" : "invalid", su_quote(su_optarg), su_quote("--format"));
                for (int i = 0; fmts[i]; i++) fprintf(stderr, "  - %s\n", su_quote(fmts[i]));
                su_try_help(1);
            }
            fm_px.format = hit + 1;
            break;
        }
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [INPUT]...   (without -G)\n"
                   "  or:  %s -G [OPTION]... [INPUT [OUTPUT]]\n"
                   "Write a permuted index of the words of the input: each keyword in its\n"
                   "context, one line each, sorted by keyword.\n"
                   "\n"
                   "With no FILE, or when FILE is -, read standard input.\n"
                   "\n"
                   "Long options take the same arguments as their short forms.\n"
                   "  -A, --auto-reference           give each line a FILE:LINE reference\n"
                   "  -G, --traditional              work as System V's ptx does\n"
                   "  -F, --flag-truncation=STRING   mark cut-off text with STRING ('/' by default)\n"
                   "  -M, --macro-name=STRING        name the roff or TeX macro STRING, not 'xx'\n"
                   "  -O, --format=roff              write roff directives\n"
                   "  -R, --right-side-refs          put references on the right, outside -w\n"
                   "  -S, --sentence-regexp=REGEXP   REGEXP ends a line or sentence\n"
                   "  -T, --format=tex               write TeX directives\n"
                   "  -W, --word-regexp=REGEXP       REGEXP matches a keyword\n"
                   "  -b, --break-file=FILE          the characters in FILE separate words\n"
                   "  -f, --ignore-case              sort and match without regard to case\n"
                   "  -g, --gap-size=NUMBER          leave NUMBER columns between fields\n"
                   "  -i, --ignore-file=FILE         never index the words listed in FILE\n"
                   "  -o, --only-file=FILE           index only the words listed in FILE\n"
                   "  -r, --references               each line's first field is its reference\n"
                   "  -t, --typeset-mode             accepted, and does nothing\n"
                   "  -w, --width=NUMBER             lines of NUMBER columns, references aside\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n", su_name, su_name);
            return fm_px_done();
        case SU_VERSION_OPT: su_version(); return fm_px_done();
        default: su_try_help(1);
        }
    }

    /* operands: input files; under -G, INPUT and OUTPUT, the output opened
     * as soon as it is named (before an extra operand is refused) */
    int nops = argc - su_optind;
    char **ops = argv + su_optind;
    static char *fm_px_stdin[] = { (char *)"-" };
    if (fm_px.gnu) {
        if (!nops) { ops = fm_px_stdin; nops = 1; }
    } else {
        if (nops >= 2) {
            int fd = open(ops[1], O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0) su_error(1, errno, "%s", su_quotef(ops[1]));
            fflush(stdout);
            if (fd != 1) { dup2(fd, 1); close(fd); }
        }
        if (nops >= 3) {
            su_error(0, 0, "extra operand %s", su_quote(ops[2]));
            su_try_help(1);
        }
        if (!nops) { ops = fm_px_stdin; nops = 1; }
        else nops = 1;
        if (!fm_px.format) fm_px.format = 1;
    }

    fm_px.trunc = fm_px_unescape(trunc_arg);
    fm_px.tlen = (long)strlen(fm_px.trunc);

    /* the regexes: -S's default is a sentence end, or a line end with -r
     * or -G; an empty one means none */
    if (sre_arg) fm_px.sre_str = fm_px_unescape(sre_arg);
    else if (fm_px.gnu && !fm_px.input_ref) fm_px.sre_str = (char *)"[.?!][]\"')}]*\\($\\|\t\\|  \\)[ \t\n]*";
    else fm_px.sre_str = (char *)"\n";
    if (*fm_px.sre_str) {
        const char *e = fm_px_compile(&fm_px.sre, fm_px.sre_str, fm_px.fold);
        if (e) su_error(1, 0, "%s (for regexp %s)", e, su_quote(fm_px.sre_str));
        fm_px.have_sre = 1;
    }
    if (wre_arg) {
        fm_px.wre_str = fm_px_unescape(wre_arg);
        if (*fm_px.wre_str) {
            const char *e = fm_px_compile(&fm_px.wre, fm_px.wre_str, fm_px.fold);
            if (e) su_error(1, 0, "%s (for regexp %s)", e, su_quote(fm_px.wre_str));
            fm_px.have_wre = 1;
        }
    }
    if (!fm_px.have_wre) {
        if (break_file) {
            char *b;
            long bl;
            fm_px_slurp(break_file, &b, &bl);
            for (int i = 0; i < 256; i++) fm_px.fastmap[i] = 1;
            for (long i = 0; i < bl; i++) fm_px.fastmap[(unsigned char)b[i]] = 0;
            free(b);
            if (!fm_px.gnu) fm_px.fastmap[' '] = fm_px.fastmap['\t'] = fm_px.fastmap['\n'] = 0;
        } else if (fm_px.gnu) {
            for (int i = 0; i < 256; i++) fm_px.fastmap[i] = isalpha(i) != 0;
        } else {
            for (int i = 0; i < 256; i++) fm_px.fastmap[i] = 1;
            fm_px.fastmap[' '] = fm_px.fastmap['\t'] = fm_px.fastmap['\n'] = 0;
        }
    }
    if (ignore_file) fm_px_readlist(ignore_file, &fm_px.ignore);
    if (only_file) fm_px_readlist(only_file, &fm_px.only);

    fm_px.nfiles = nops;
    fm_px.files = (struct fm_px_file *)fm_px_alloc((size_t)nops * sizeof *fm_px.files);
    for (int i = 0; i < nops; i++) {
        fm_px.files[i].name = ops[i];
        fm_px_slurp(ops[i], &fm_px.files[i].buf, &fm_px.files[i].len);
        fm_px_scan(i);
    }
    qsort(fm_px.occ, (size_t)fm_px.nocc, sizeof *fm_px.occ, fm_px_ocmp);

    /* the widths: the widest reference comes off the line when the
     * references go on the left */
    int refs = fm_px.auto_ref || fm_px.input_ref;
    long line_width = fm_px.width;
    if (refs) {
        /* -A: every file counts as "NAME:1" at least, keywords or not */
        if (fm_px.auto_ref)
            for (int i = 0; i < fm_px.nfiles; i++) {
                long n = strcmp(fm_px.files[i].name, "-") ? (long)strlen(fm_px.files[i].name) + 2 : 2;
                if (n > fm_px.ref_max) fm_px.ref_max = n;
            }
        for (long i = 0; i < fm_px.nocc; i++) {
            char rb[4200];
            const char *t;
            long n = fm_px_ref(&fm_px.occ[i], rb, sizeof rb, &t);
            if (n > fm_px.ref_max) fm_px.ref_max = n;
        }
        /* -G's references go last too, but take their room unless -R */
        if (!fm_px.right_refs) line_width -= fm_px.ref_max + fm_px.gap;
    }
    fm_px.half = line_width / 2;
    if (fm_px.gnu) {
        fm_px.before_max = fm_px.half - fm_px.gap - 2 * fm_px.tlen;
        if (fm_px.before_max < 0) fm_px.before_max = 0;     /* -G's is not clamped */
        fm_px.keyafter_max = fm_px.half - 2 * fm_px.tlen;
    } else {
        /* System V's proportions, as -G measures */
        fm_px.before_max = fm_px.half - fm_px.gap;
        fm_px.keyafter_max = fm_px.half - 1 - 2 * fm_px.tlen;
    }
    for (long i = 0; i < fm_px.nocc; i++) fm_px_output(&fm_px.occ[i]);
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return 0;
}
