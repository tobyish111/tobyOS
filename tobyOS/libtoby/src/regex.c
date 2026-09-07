/* libtoby/src/regex.c -- POSIX BRE/ERE regex engine.
 *
 * NFA-based with backtracking. Compiles to a small IR of node structs,
 * then runs a recursive matcher. Supports: . * + ? | [] [^] ^ $ {n,m}
 * () groups, backrefs \1-\9, POSIX [:classes:], \b \B \< \> word
 * boundaries, and the \d \w \s shortcuts.
 *
 * ------------------------------------------------------------------
 * THE FRAGMENT-EXIT REWRITE (2026-09-06), and why it had to happen.
 *
 * The parser used to hand fragments around as a single `nd_t *entry` and
 * rediscover the other end by walking `->out` (`chain_end`). That works
 * only while the graph is a tree, and quantifiers make it a GRAPH:
 * quantify() builds `x*` as a LOOP (split -> body -> back to split).
 *
 * So for `\([0-9.]*\)` the group's own wiring --
 *
 *     nd_t *n = chain_end(c, inner);   n->out = ge;
 *
 * -- walked into the star's cycle, ran until its cycle guard tripped,
 * and then overwrote a LOOP EDGE with the group-end pointer. The star
 * was destroyed (it could match at most one character), the fragment's
 * real exit was orphaned with a NULL ->out, and an orphaned ND_NOP is
 * read by the matcher as "accept here" -- so the zero-iteration branch
 * accepted the whole pattern immediately and \1 was never closed.
 *
 * Visible as: the `ip route` idiom -- a sed -n script whose pattern is
 *   ^default via \([0-9.]*\) .\*  and whose replacement is \1 -- printing
 * the rest of the line instead of the address. (Spelled with the star
 * escaped because the real thing ends in a comment terminator.) It was
 * filed as "bracket
 * expressions need a look", but brackets were never the problem: EVERY
 * group whose last element is quantified was miscompiled, `(a*)` and
 * `\(x\)*` alike, in both BRE and ERE.
 *
 * The fix is structural, not a patch: parse_atom/parse_seq/parse_alt now
 * return a frag_t {entry, exit}, every construct wires its own exit, and
 * chain_end is GONE. Nothing walks a graph it cannot know is acyclic.
 *
 * ------------------------------------------------------------------
 * ND_REPEAT, and why the old matcher could not survive a long line.
 *
 * A greedy `.*` used to recurse once PER CHARACTER CONSUMED (split ->
 * dot -> split -> ...). A native user stack is 64 pages = 256 KiB and a
 * frame here is ~96 bytes, so a 4 KiB line -- ordinary for sed -- came
 * within a hair of running off the bottom of it, and MAX_DEPTH's only
 * defence was to report a false "no match".
 *
 * When the body of a quantifier is a single consuming node (a literal, a
 * dot, a class -- which covers `.*`, `[0-9]+`, `[^,]*`, `a{3}`, nearly
 * everything real patterns do), it is now compiled to ONE ND_REPEAT node
 * that consumes iteratively and backtracks by decrementing a count. Depth
 * became a function of pattern complexity instead of input length, and
 * bounded repetition of a simple body no longer clones anything.
 */

#include <regex.h>
#include <stdlib.h>
#include <string.h>

/* ---- NFA node types --------------------------------------------- */

enum {
    ND_LIT,        /* literal byte */
    ND_DOT,        /* any char (except \n in REG_NEWLINE mode) */
    ND_CLASS,      /* character class [abc] */
    ND_NCLASS,     /* negated class [^abc] */
    ND_ANCHOR_BOL, /* ^ */
    ND_ANCHOR_EOL, /* $ */
    ND_WORDB,      /* \b */
    ND_NWORDB,     /* \B */
    ND_WSTART,     /* \< */
    ND_WEND,       /* \> */
    ND_GROUP_S,    /* start of group i */
    ND_GROUP_E,    /* end of group i */
    ND_BACKREF,    /* \1..\9 */
    ND_BRANCH,     /* alternation: try .out first, then .out2 */
    ND_SPLIT,      /* like BRANCH but for quantifiers */
    ND_REPEAT,     /* iterative {min,max} over a single-node body */
    ND_MATCH,      /* accept state */
    ND_NOP,        /* pass-through: a fragment's exit / a join point */
};

#define CLASS_BITMAP_SIZE 32  /* 256 bits */

typedef struct nd {
    int           type;
    int           ch;             /* ND_LIT char, or group index, or backref */
    unsigned char cclass[CLASS_BITMAP_SIZE];
    int           greedy;
    /* ND_REPEAT only: the body it repeats, folded into this node. */
    int           btype;
    int           rmin, rmax;     /* rmax < 0 == unbounded */
    struct nd    *out;
    struct nd    *out2;
} nd_t;

/* A parsed piece of the pattern: where control ENTERS it, and the node
 * whose ->out is where control LEAVES it. Every producer sets both; no
 * consumer ever has to go looking. */
typedef struct {
    nd_t *entry;
    nd_t *exit;
} frag_t;

typedef struct {
    /* A parse error has to be RECORDED, not just returned: parse_alt
     * substitutes an empty-match node when a sub-parse yields NULL, so
     * a bad pattern silently became "matches everywhere" instead of an
     * error. regcomp checks this before accepting the compile. */
    int          err;
    nd_t        *nodes;
    int          nnodes;
    int          cap;
    nd_t        *start;
    int          nsub;
    int          cflags;
    const char  *pat;      /* the whole pattern, for BRE ^/$ position rules */
    int          has_alt;  /* pattern contains alternation */
    int          has_lazy; /* pattern contains a non-greedy quantifier */
} compiled_t;

/* ---- bit helpers for character classes --------------------------- */

static void cls_set(unsigned char *c, int b) {
    c[(unsigned)b >> 3] |= (unsigned char)(1 << ((unsigned)b & 7));
}

static int cls_test(const unsigned char *c, int b) {
    return (c[(unsigned)b >> 3] >> ((unsigned)b & 7)) & 1;
}

static void cls_add(unsigned char *c, int ch, int icase) {
    cls_set(c, ch);
    if (icase) {
        if (ch >= 'a' && ch <= 'z') cls_set(c, ch - 32);
        if (ch >= 'A' && ch <= 'Z') cls_set(c, ch + 32);
    }
}

/* ---- ctype, spelled out ------------------------------------------
 * libtoby has <ctype.h>, but this file is also linked into freestanding
 * builds; the definitions are two lines each and locale-free (the C
 * locale is the only locale this system has). */
static int rx_upper(int c) { return c >= 'A' && c <= 'Z'; }
static int rx_lower(int c) { return c >= 'a' && c <= 'z'; }
static int rx_alpha(int c) { return rx_upper(c) || rx_lower(c); }
static int rx_digit(int c) { return c >= '0' && c <= '9'; }
static int rx_alnum(int c) { return rx_alpha(c) || rx_digit(c); }
static int rx_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' ||
           c == '\r' || c == '\f' || c == '\v';
}
static int rx_blank(int c) { return c == ' ' || c == '\t'; }
static int rx_cntrl(int c) { return c < 32 || c == 127; }
static int rx_print(int c) { return c >= 32 && c < 127; }
static int rx_graph(int c) { return c > 32 && c < 127; }
static int rx_punct(int c) { return rx_graph(c) && !rx_alnum(c); }
static int rx_xdigit(int c) {
    return rx_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int rx_word(int c) { return rx_alnum(c) || c == '_'; }

static int name_is(const char *s, size_t len, const char *lit) {
    size_t i;
    for (i = 0; i < len; i++)
        if (!lit[i] || s[i] != lit[i]) return 0;
    return lit[len] == '\0';
}

/* [:alpha:] and friends. Returns 0 for an unknown class name, which is
 * REG_ECTYPE and not "matches nothing": `grep '[[:alpah:]]'` should say
 * so rather than quietly finding zero lines.
 *
 * Before this existed, parse_class read `[[:alpha:]]` one byte at a time
 * and produced the set { [ : a l p h }, then left a stray `]` in the
 * pattern -- a wrong answer, never a diagnostic. */
static int add_named_class(unsigned char *bm, const char *nm, size_t len,
                           int icase) {
    int (*fn)(int) = 0;
    if      (name_is(nm, len, "alpha"))  fn = rx_alpha;
    else if (name_is(nm, len, "digit"))  fn = rx_digit;
    else if (name_is(nm, len, "alnum"))  fn = rx_alnum;
    else if (name_is(nm, len, "upper"))  fn = rx_upper;
    else if (name_is(nm, len, "lower"))  fn = rx_lower;
    else if (name_is(nm, len, "space"))  fn = rx_space;
    else if (name_is(nm, len, "blank"))  fn = rx_blank;
    else if (name_is(nm, len, "punct"))  fn = rx_punct;
    else if (name_is(nm, len, "print"))  fn = rx_print;
    else if (name_is(nm, len, "graph"))  fn = rx_graph;
    else if (name_is(nm, len, "cntrl"))  fn = rx_cntrl;
    else if (name_is(nm, len, "xdigit")) fn = rx_xdigit;
    else return 0;

    for (int c = 0; c < 256; c++)
        if (fn(c)) cls_set(bm, c);
    /* [:upper:] under -i has to admit lower case too, and vice versa;
     * without this `grep -i '[[:upper:]]'` disagreed with GNU. */
    if (icase && (fn == rx_upper || fn == rx_lower))
        for (int c = 'a'; c <= 'z'; c++) { cls_set(bm, c); cls_set(bm, c - 32); }
    return 1;
}

/* ---- compiler --------------------------------------------------- */

/* The node array is allocated ONCE, by regcomp, and never grows.
 *
 * It used to realloc here, which silently invalidated every nd_t* the
 * parser was holding at that moment -- `tail`, `atom`, the group ends, the
 * fragment being quantified. Nothing had ever noticed, because a pattern
 * needing fewer than 64 nodes never grew the array; but bounded repetition
 * duplicates whole fragments, so the first `x{20}` would have been a
 * use-after-free. Running out of room is now a clean REG_ESPACE. */
static nd_t *new_node(compiled_t *c) {
    if (c->nnodes >= c->cap) { c->err = REG_ESPACE; return 0; }
    nd_t *n = &c->nodes[c->nnodes++];
    memset(n, 0, sizeof(*n));
    n->greedy = 1;
    n->rmax   = -1;
    return n;
}

static nd_t *new_nop(compiled_t *c) {
    nd_t *n = new_node(c);
    if (n) n->type = ND_NOP;
    return n;
}

/* Map one pointer from the range [lo,hi) onto its copy at `base`.
 * Pointers outside the range (a shared tail, or NULL) pass through. */
static nd_t *clone_map(compiled_t *c, int lo, int hi, int base, nd_t *p) {
    if (p >= &c->nodes[lo] && p < &c->nodes[hi])
        return &c->nodes[base + (int)(p - &c->nodes[lo])];
    return p;
}

/* Copy the nodes in [lo,hi) to the end of the array, rebasing every
 * internal pointer onto the copy, and report where the fragment's entry
 * and exit ended up. This is what makes {n,m} possible for a compound
 * body: an NFA cannot express "exactly three" without three copies. */
static int clone_frag(compiled_t *c, int lo, int hi,
                      const frag_t *in, frag_t *out) {
    int n = hi - lo;
    if (n <= 0) { *out = *in; return 1; }
    if (c->nnodes + n > c->cap) { c->err = REG_ESPACE; return 0; }
    int base = c->nnodes;
    for (int i = 0; i < n; i++) c->nodes[base + i] = c->nodes[lo + i];
    c->nnodes += n;
    for (int i = 0; i < n; i++) {
        nd_t *d = &c->nodes[base + i];
        d->out  = clone_map(c, lo, hi, base, d->out);
        d->out2 = clone_map(c, lo, hi, base, d->out2);
    }
    out->entry = clone_map(c, lo, hi, base, in->entry);
    out->exit  = clone_map(c, lo, hi, base, in->exit);
    return 1;
}

/* Forward decls */
static int parse_alt(compiled_t *c, const char **pp, int ere, frag_t *out);

static int parse_count(const char **pp) {
    int v = 0;
    while (**pp >= '0' && **pp <= '9') {
        v = v * 10 + (**pp - '0');
        (*pp)++;
    }
    return v;
}

/* A body of exactly one consuming node can be repeated by counting
 * instead of by looping through the graph -- see the ND_REPEAT note at
 * the top of the file. */
static int simple_body(const frag_t *f) {
    if (f->entry != f->exit) return 0;
    switch (f->entry->type) {
    case ND_LIT: case ND_DOT: case ND_CLASS: case ND_NCLASS:
        return 1;
    default:
        return 0;
    }
}

/* Build a quantified fragment: repeat `body` between min and max times,
 * where max == -1 means infinity. */
static int quantify(compiled_t *c, frag_t body, int min, int max, int greedy,
                    int lo, int hi, frag_t *out) {
    if (min < 0) min = 0;
    if (max >= 0 && max < min) max = min;

    /* THE FAST PATH, and the one nearly every real pattern takes. */
    if (simple_body(&body)) {
        nd_t *r = body.entry;
        r->btype  = r->type;
        r->type   = ND_REPEAT;
        r->rmin   = min;
        r->rmax   = max;
        r->greedy = greedy;
        out->entry = r;
        out->exit  = r;
        return 1;
    }

    nd_t *join = new_nop(c);
    if (!join) return 0;

    if (max == 0) {                     /* {0} matches empty */
        out->entry = join;
        out->exit  = join;
        return 1;
    }
    if (min == 0 && max == -1) {        /* * */
        nd_t *sp = new_node(c);
        if (!sp) return 0;
        sp->type = ND_SPLIT;
        sp->greedy = greedy;
        sp->out  = greedy ? body.entry : join;
        sp->out2 = greedy ? join : body.entry;
        body.exit->out = sp;
        out->entry = sp;
        out->exit  = join;
        return 1;
    }
    if (min == 1 && max == -1) {        /* + */
        nd_t *sp = new_node(c);
        if (!sp) return 0;
        sp->type = ND_SPLIT;
        sp->greedy = greedy;
        sp->out  = greedy ? body.entry : join;
        sp->out2 = greedy ? join : body.entry;
        body.exit->out = sp;
        out->entry = body.entry;
        out->exit  = join;
        return 1;
    }
    if (min == 0 && max == 1) {         /* ? */
        nd_t *sp = new_node(c);
        if (!sp) return 0;
        sp->type = ND_SPLIT;
        sp->greedy = greedy;
        sp->out  = greedy ? body.entry : join;
        sp->out2 = greedy ? join : body.entry;
        body.exit->out = join;
        out->entry = sp;
        out->exit  = join;
        return 1;
    }

    /* GENERAL {n,m} over a compound body. This used to fall through to a
     * `*`, so `(ab){3}` matched ANY number of them -- wrong output rather
     * than an error, which is why it survived so long.
     *
     * Built from the inside out: the optional copies first, each wrapped
     * in a split that can skip to what follows, then the required ones in
     * front. The ORIGINAL fragment is used as the outermost copy and is
     * therefore mutated last, so every clone is taken from a pristine
     * range. */
    nd_t *cur = join;

    if (max < 0) {
        /* {n,} -- an unbounded tail, so the last copy loops. */
        frag_t b;
        if (!clone_frag(c, lo, hi, &body, &b)) return 0;
        nd_t *sp = new_node(c);
        if (!sp) return 0;
        sp->type = ND_SPLIT;
        sp->greedy = greedy;
        sp->out  = greedy ? b.entry : cur;
        sp->out2 = greedy ? cur : b.entry;
        b.exit->out = sp;
        cur = (min > 0) ? b.entry : sp;   /* n>0: one copy is mandatory */
        if (min > 0) min--;
    } else {
        for (int k = 0; k < max - min; k++) {
            frag_t b;
            if (!clone_frag(c, lo, hi, &body, &b)) return 0;
            b.exit->out = cur;
            nd_t *sp = new_node(c);
            if (!sp) return 0;
            sp->type = ND_SPLIT;
            sp->greedy = greedy;
            sp->out  = greedy ? b.entry : cur;
            sp->out2 = greedy ? cur : b.entry;
            cur = sp;
        }
    }

    for (int k = 0; k < min; k++) {
        frag_t b;
        if (k == min - 1) b = body;                    /* pristine until now */
        else if (!clone_frag(c, lo, hi, &body, &b)) return 0;
        b.exit->out = cur;
        cur = b.entry;
    }
    out->entry = cur;
    out->exit  = join;
    return 1;
}

/* One character inside a bracket expression, honouring backslash escapes
 * only when the caller asked for them (see REG_ESCBRACKET in regex.h --
 * awk wants them, sed and grep must not have them). */
static int bracket_char(compiled_t *c, const char **pp) {
    const char *p = *pp;
    if ((c->cflags & REG_ESCBRACKET) && *p == '\\' && p[1]) {
        p++;
        int v;
        switch (*p) {
        case 'n': v = '\n'; break;
        case 't': v = '\t'; break;
        case 'r': v = '\r'; break;
        case 'f': v = '\f'; break;
        case 'v': v = '\v'; break;
        case 'a': v = '\a'; break;
        case 'b': v = '\b'; break;
        case '0': v = '\0'; break;
        default:  v = (unsigned char)*p; break;
        }
        p++;
        *pp = p;
        return v;
    }
    int v = (unsigned char)*p++;
    *pp = p;
    return v;
}

/* Parse a character class [... ] */
static nd_t *parse_class(compiled_t *c, const char **pp, int icase) {
    const char *p = *pp;
    int negate = 0;
    if (*p == '^') { negate = 1; p++; }

    nd_t *node = new_node(c);
    if (!node) return 0;
    node->type = negate ? ND_NCLASS : ND_CLASS;
    memset(node->cclass, 0, CLASS_BITMAP_SIZE);

    /* First char can be ] without closing */
    int first = 1;
    while (*p && (*p != ']' || first)) {
        first = 0;

        /* [:class:], [.collating.], [=equivalence=] */
        if (p[0] == '[' && (p[1] == ':' || p[1] == '.' || p[1] == '=')) {
            char kind = p[1];
            const char *nm = p + 2, *e = nm;
            while (*e && !(e[0] == kind && e[1] == ']')) e++;
            if (!*e) { c->err = REG_EBRACK; return 0; }
            size_t nlen = (size_t)(e - nm);
            if (kind == ':') {
                if (!add_named_class(node->cclass, nm, nlen, icase)) {
                    c->err = REG_ECTYPE; return 0;
                }
            } else {
                /* Single characters collate to themselves in the C
                 * locale, which is the only locale this system has. */
                if (nlen != 1) { c->err = REG_ECOLLATE; return 0; }
                cls_add(node->cclass, (unsigned char)nm[0], icase);
            }
            p = e + 2;
            continue;
        }

        int lo = bracket_char(c, &p);
        int hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            hi = bracket_char(c, &p);
        }
        if (hi < lo) { c->err = REG_ERANGE; return 0; }
        for (int ch = lo; ch <= hi; ch++) cls_add(node->cclass, ch, icase);
    }
    /* An unterminated class is an ERROR. It used to compile and match
     * whatever had been accumulated, so `grep '['` searched for something
     * instead of reporting a bad pattern -- a typo turning into a wrong
     * answer rather than a diagnostic. */
    if (*p != ']') { c->err = REG_EBRACK; return 0; }
    p++;
    *pp = p;
    return node;
}

/* Does `p` sit on a well-formed interval? GNU treats a `{` that does not
 * open one as a literal brace -- `grep -cE 'a{'` finds the line rather
 * than reporting a bad pattern, and the old code silently deleted the
 * atom in front of it instead. */
static int is_interval(const char *p, int ere) {
    if (ere) { if (*p != '{') return 0; p++; }
    else     { if (p[0] != '\\' || p[1] != '{') return 0; p += 2; }
    int sawlo = 0;
    while (rx_digit((unsigned char)*p)) { p++; sawlo = 1; }
    if (*p == ',') { p++; while (rx_digit((unsigned char)*p)) p++; }
    else if (!sawlo) return 0;
    if (ere) return *p == '}';
    return p[0] == '\\' && p[1] == '}';
}

/* In a BRE, ^ is an anchor only where a pattern can start, and $ only
 * where one can end; everywhere else they are literal characters. GNU
 * agrees: `echo 'a$b' | sed 's/a$b/X/'` prints X. */
static int bre_bol_here(compiled_t *c, const char *p) {
    if (p == c->pat) return 1;
    if (p - c->pat >= 2 && p[-2] == '\\' && (p[-1] == '(' || p[-1] == '|'))
        return 1;
    return 0;
}

static int bre_eol_here(const char *p) {
    if (p[1] == '\0') return 1;
    if (p[1] == '\\' && (p[2] == ')' || p[2] == '|')) return 1;
    return 0;
}

/* Parse a single atom. Returns 0 with out->entry == 0 when there is
 * nothing here to parse (the caller ends its sequence). */
static int parse_atom(compiled_t *c, const char **pp, int ere, frag_t *out) {
    const char *p = *pp;
    nd_t *node = 0;

    out->entry = out->exit = 0;

    switch (*p) {
    case '.':
        node = new_node(c);
        if (!node) return 0;
        node->type = ND_DOT;
        p++;
        break;
    case '^':
        if (!ere && !bre_bol_here(c, p)) goto literal;
        node = new_node(c);
        if (!node) return 0;
        node->type = ND_ANCHOR_BOL;
        p++;
        break;
    case '$':
        if (!ere && !bre_eol_here(p)) goto literal;
        node = new_node(c);
        if (!node) return 0;
        node->type = ND_ANCHOR_EOL;
        p++;
        break;
    case '[':
        p++;
        node = parse_class(c, &p, c->cflags & REG_ICASE);
        if (!node) return 0;
        break;
    case '\\':
        p++;
        if (*p >= '1' && *p <= '9') {
            node = new_node(c);
            if (!node) return 0;
            node->type = ND_BACKREF;
            node->ch = *p - '0';
            if (node->ch > c->nsub) { c->err = REG_ESUBREG; return 0; }
            p++;
        } else if (*p == 'b' || *p == 'B' || *p == '<' || *p == '>') {
            /* GNU's word operators. sed and grep scripts in the wild use
             * \< and \> constantly; without them they matched the LETTER
             * b, or the literal characters < and >. */
            node = new_node(c);
            if (!node) return 0;
            node->type = (*p == 'b') ? ND_WORDB  :
                         (*p == 'B') ? ND_NWORDB :
                         (*p == '<') ? ND_WSTART : ND_WEND;
            p++;
        } else if (*p == 'd' || *p == 'w' || *p == 's' ||
                   *p == 'D' || *p == 'W' || *p == 'S') {
            node = new_node(c);
            if (!node) return 0;
            memset(node->cclass, 0, CLASS_BITMAP_SIZE);
            if (*p == 'd' || *p == 'D') {
                for (int i = '0'; i <= '9'; i++) cls_set(node->cclass, i);
                node->type = (*p == 'd') ? ND_CLASS : ND_NCLASS;
            } else if (*p == 'w' || *p == 'W') {
                for (int i = 0; i < 256; i++)
                    if (rx_word(i)) cls_set(node->cclass, i);
                node->type = (*p == 'w') ? ND_CLASS : ND_NCLASS;
            } else {
                for (int i = 0; i < 256; i++)
                    if (rx_space(i)) cls_set(node->cclass, i);
                node->type = (*p == 's') ? ND_CLASS : ND_NCLASS;
            }
            p++;
        } else if (*p == 'n' || *p == 't' || *p == 'r' ||
                   *p == 'f' || *p == 'v' || *p == 'a') {
            /* GNU regex maps these to the control characters they name.
             * POSIX leaves them undefined, so this is a superset -- and it
             * is what `sed 's/\n/+/'` needs to see the newline
             * that N puts in the pattern space, and what `grep '\t'`
             * needs to find a tab. Without it they matched the LETTER. */
            node = new_node(c);
            if (!node) return 0;
            node->type = ND_LIT;
            switch (*p) {
            case 'n': node->ch = 10; break;
            case 't': node->ch = 9;  break;
            case 'r': node->ch = 13; break;
            case 'f': node->ch = 12; break;
            case 'v': node->ch = 11; break;
            default:  node->ch = 7;  break;
            }
            p++;
        } else if (*p) {
            node = new_node(c);
            if (!node) return 0;
            node->type = ND_LIT;
            node->ch = (unsigned char)*p++;
            if ((c->cflags & REG_ICASE) && rx_upper(node->ch)) node->ch += 32;
        } else {
            /* A pattern ending in a lone backslash. */
            c->err = REG_EESCAPE;
            return 0;
        }
        break;
    case '(':
        if (!ere) goto literal;          /* BRE: a plain ( is a character */
        p++;
        {
            int gi = ++c->nsub;
            nd_t *gs = new_node(c);
            if (!gs) return 0;
            gs->type = ND_GROUP_S;
            gs->ch = gi;

            frag_t inner;
            if (!parse_alt(c, &p, ere, &inner)) return 0;

            nd_t *ge = new_node(c);
            if (!ge) return 0;
            ge->type = ND_GROUP_E;
            ge->ch = gi;

            gs->out = inner.entry;
            inner.exit->out = ge;        /* the exit is KNOWN, not hunted */
            if (*p != ')') { c->err = REG_EPAREN; return 0; }
            p++;
            out->entry = gs;
            out->exit  = ge;
            *pp = p;
            return 1;
        }
    default:
    literal:
        if (*p && !(ere && (*p == '|' || *p == ')')) &&
            *p != '*' && !(ere && (*p == '+' || *p == '?'))) {
            node = new_node(c);
            if (!node) return 0;
            node->type = ND_LIT;
            node->ch = (unsigned char)*p;
            if ((c->cflags & REG_ICASE) && rx_upper(node->ch)) node->ch += 32;
            p++;
        }
        break;
    }

    *pp = p;
    if (!node) return 1;                 /* nothing parsed; entry stays 0 */
    out->entry = node;
    out->exit  = node;
    return 1;
}

/* Parse a BRE group \(...\) */
static int parse_bre_group(compiled_t *c, const char **pp, frag_t *out) {
    const char *p = *pp;
    int gi = ++c->nsub;
    nd_t *gs = new_node(c);
    if (!gs) return 0;
    gs->type = ND_GROUP_S;
    gs->ch = gi;

    frag_t inner;
    if (!parse_alt(c, &p, 0, &inner)) return 0;

    nd_t *ge = new_node(c);
    if (!ge) return 0;
    ge->type = ND_GROUP_E;
    ge->ch = gi;

    gs->out = inner.entry;
    inner.exit->out = ge;
    if (!(p[0] == '\\' && p[1] == ')')) { c->err = REG_EPAREN; return 0; }
    p += 2;

    *pp = p;
    out->entry = gs;
    out->exit  = ge;
    return 1;
}

/* Parse a sequence of atoms with optional quantifiers */
static int parse_seq(compiled_t *c, const char **pp, int ere, frag_t *out) {
    const char *p = *pp;
    nd_t *entry = 0, *tail = 0;

    for (;;) {
        if (!*p) break;
        if (ere && (*p == '|' || *p == ')')) break;
        if (!ere && *p == '\\' && (p[1] == ')' || p[1] == '|')) break;

        /* The atom occupies nodes [atom_lo, atom_hi); a bounded
         * quantifier over a compound body has to CLONE that range. */
        int atom_lo = c->nnodes, atom_hi;
        frag_t atom;

        if (!ere && *p == '\\' && p[1] == '(') {
            p += 2;
            if (!parse_bre_group(c, &p, &atom)) return 0;
        } else if (!ere && *p == '*' && !entry) {
            /* A BRE that STARTS with * matches a literal asterisk --
             * a BRE of star-a against the text star-a-b prints Xb. Previously the atom
             * came back empty and the whole rest of the pattern was
             * silently dropped. */
            nd_t *n = new_node(c);
            if (!n) return 0;
            n->type = ND_LIT;
            n->ch = '*';
            p++;
            atom.entry = atom.exit = n;
        } else {
            if (!parse_atom(c, &p, ere, &atom)) return 0;
            if (!atom.entry) break;
        }
        atom_hi = c->nnodes;

        /* Quantifiers. In a BRE only `*` and the GNU escapes \+ \? and
         * the interval \{n,m\} are quantifiers: a bare + or ? there is
         * an ordinary character, which is why they are not tested for
         * unless `ere`. */
        int qmin = -1, qmax = -1, greedy = 1;

        if (*p == '*' || (ere && (*p == '+' || *p == '?'))) {
            char q = *p++;
            if (ere && *p == '?') { greedy = 0; p++; c->has_lazy = 1; }
            if (q == '*')      { qmin = 0; qmax = -1; }
            else if (q == '+') { qmin = 1; qmax = -1; }
            else               { qmin = 0; qmax = 1;  }
        } else if (!ere && p[0] == '\\' && (p[1] == '+' || p[1] == '?')) {
            qmin = (p[1] == '+') ? 1 : 0;
            qmax = (p[1] == '+') ? -1 : 1;
            p += 2;
        } else if (is_interval(p, ere)) {
            p += ere ? 1 : 2;
            qmin = parse_count(&p);
            qmax = qmin;
            if (*p == ',') {
                p++;
                if (ere ? (*p == '}') : (*p == '\\')) qmax = -1;
                else qmax = parse_count(&p);
            }
            p += ere ? 1 : 2;            /* past } or \} */
            if (ere && *p == '?') { greedy = 0; p++; c->has_lazy = 1; }
            if (qmax >= 0 && qmax < qmin) { c->err = REG_BADBR; return 0; }
        }

        if (qmin >= 0) {
            frag_t q;
            if (!quantify(c, atom, qmin, qmax, greedy, atom_lo, atom_hi, &q))
                return 0;
            atom = q;
        }

        if (!entry) entry = atom.entry;
        else        tail->out = atom.entry;
        tail = atom.exit;
    }

    if (!entry) {                        /* an empty branch matches empty */
        nd_t *n = new_nop(c);
        if (!n) return 0;
        entry = tail = n;
    }

    *pp = p;
    out->entry = entry;
    out->exit  = tail;
    return 1;
}

/* Parse alternatives (a|b) */
static int parse_alt(compiled_t *c, const char **pp, int ere, frag_t *out) {
    frag_t left;
    if (!parse_seq(c, pp, ere, &left)) return 0;

    const char *p = *pp;

    while ((ere && *p == '|') || (!ere && p[0] == '\\' && p[1] == '|')) {
        p += ere ? 1 : 2;
        c->has_alt = 1;

        frag_t right;
        if (!parse_seq(c, &p, ere, &right)) return 0;

        nd_t *br = new_node(c);
        if (!br) return 0;
        br->type = ND_BRANCH;
        br->out  = left.entry;
        br->out2 = right.entry;

        nd_t *join = new_nop(c);
        if (!join) return 0;
        left.exit->out  = join;
        right.exit->out = join;

        left.entry = br;
        left.exit  = join;
    }

    *pp = p;
    *out = left;
    return 1;
}

/* ---- matcher (backtracking) ------------------------------------- */

#define MAX_GROUPS 10

typedef struct {
    const char  *str;
    const char  *bol;
    int          eflags;
    int          cflags;
    const char  *group_start[MAX_GROUPS];
    const char  *group_end[MAX_GROUPS];
    int          depth;
    /* Where the accepting path ended. regexec used to try to discover
     * this by re-running the matcher, and got it wrong; the matcher
     * already knows, because ND_MATCH is reached with sp sitting exactly
     * at the end of the match. */
    const char  *match_end;
    /* POSIX leftmost-LONGEST mode: keep exploring past an accept and
     * remember the longest one instead of taking the first. */
    int          longest;
    int          have_match;
    long         steps;
    long         budget;
    const char  *best_start[MAX_GROUPS];
    const char  *best_end[MAX_GROUPS];
} match_ctx;

/* One frame is ~96 bytes and a native user stack is 64 pages (256 KiB),
 * so this ceiling is a hard safety property, not a tuning knob. It used
 * to be 4096 -- reachable, because a greedy `.*` recursed once per
 * character and sed lines are routinely 4 KiB. ND_REPEAT removed that
 * whole class of recursion; what is left is one frame per group and per
 * repetition of a COMPOUND body, which no sane pattern drives to 1500. */
#define MAX_DEPTH 1500

static int rx_is_word_at(const char *p) { return rx_word((unsigned char)*p); }

static int match_body(const match_ctx *ctx, const nd_t *n, int btype, int c) {
    if (c == '\0') return 0;
    switch (btype) {
    case ND_LIT: {
        int a = c, b = n->ch;
        if (ctx->cflags & REG_ICASE) {
            if (rx_upper(a)) a += 32;
            if (rx_upper(b)) b += 32;
        }
        return a == b;
    }
    case ND_DOT:
        return !((ctx->cflags & REG_NEWLINE) && c == '\n');
    case ND_CLASS:
        return cls_test(n->cclass, c);
    case ND_NCLASS:
        if ((ctx->cflags & REG_NEWLINE) && c == '\n') return 0;
        return !cls_test(n->cclass, c);
    default:
        return 0;
    }
}

static int match_node(match_ctx *ctx, nd_t *node, const char *sp);

static int match_node(match_ctx *ctx, nd_t *node, const char *sp) {
    if (++ctx->depth > MAX_DEPTH) { ctx->depth--; return 0; }
    if (ctx->budget && ++ctx->steps > ctx->budget) { ctx->depth--; return 0; }

    while (node) {
        switch (node->type) {
        case ND_MATCH:
            if (ctx->longest) {
                if (!ctx->have_match || sp > ctx->match_end) {
                    ctx->have_match = 1;
                    ctx->match_end  = sp;
                    memcpy(ctx->best_start, ctx->group_start,
                           sizeof ctx->best_start);
                    memcpy(ctx->best_end, ctx->group_end,
                           sizeof ctx->best_end);
                }
                ctx->depth--;
                return 0;             /* keep looking for a longer one */
            }
            ctx->match_end = sp;
            ctx->depth--;
            return 1;
        case ND_NOP:
            if (!node->out) {   /* nothing follows: this IS the end */
                ctx->match_end = sp;
                ctx->depth--;
                return 1;
            }
            node = node->out;
            continue;
        case ND_LIT:
        case ND_DOT:
        case ND_CLASS:
        case ND_NCLASS:
            if (!match_body(ctx, node, node->type, (unsigned char)*sp)) {
                ctx->depth--; return 0;
            }
            sp++;
            node = node->out;
            continue;
        case ND_REPEAT: {
            /* Consume greedily by COUNTING, then hand the continuation
             * successively shorter prefixes. One frame per attempt
             * instead of one per character. */
            long n = 0;
            while ((node->rmax < 0 || n < node->rmax) &&
                   match_body(ctx, node, node->btype, (unsigned char)sp[n]))
                n++;
            if (n < node->rmin) { ctx->depth--; return 0; }
            if (node->greedy) {
                for (long k = n; k >= node->rmin; k--)
                    if (match_node(ctx, node->out, sp + k)) {
                        ctx->depth--; return 1;
                    }
            } else {
                for (long k = node->rmin; k <= n; k++)
                    if (match_node(ctx, node->out, sp + k)) {
                        ctx->depth--; return 1;
                    }
            }
            ctx->depth--;
            return 0;
        }
        case ND_ANCHOR_BOL:
            if (sp == ctx->bol) {
                if (ctx->eflags & REG_NOTBOL) { ctx->depth--; return 0; }
            } else if ((ctx->cflags & REG_NEWLINE) && sp[-1] == '\n') {
                /* ok: start of an embedded line */
            } else {
                ctx->depth--; return 0;
            }
            node = node->out;
            continue;
        case ND_ANCHOR_EOL:
            if (*sp == '\0') {
                if (ctx->eflags & REG_NOTEOL) { ctx->depth--; return 0; }
            } else if ((ctx->cflags & REG_NEWLINE) && *sp == '\n') {
                /* ok: end of an embedded line */
            } else {
                ctx->depth--; return 0;
            }
            node = node->out;
            continue;
        case ND_WORDB:
        case ND_NWORDB: {
            int before = (sp != ctx->bol) && rx_is_word_at(sp - 1);
            int after  = rx_is_word_at(sp);
            int b = (before != after);
            if ((node->type == ND_WORDB) != (b != 0)) { ctx->depth--; return 0; }
            node = node->out;
            continue;
        }
        case ND_WSTART: {
            int before = (sp != ctx->bol) && rx_is_word_at(sp - 1);
            if (before || !rx_is_word_at(sp)) { ctx->depth--; return 0; }
            node = node->out;
            continue;
        }
        case ND_WEND: {
            int before = (sp != ctx->bol) && rx_is_word_at(sp - 1);
            if (!before || rx_is_word_at(sp)) { ctx->depth--; return 0; }
            node = node->out;
            continue;
        }
        case ND_GROUP_S: {
            const char *save = ctx->group_start[node->ch];
            ctx->group_start[node->ch] = sp;
            if (match_node(ctx, node->out, sp)) { ctx->depth--; return 1; }
            ctx->group_start[node->ch] = save;
            ctx->depth--; return 0;
        }
        case ND_GROUP_E: {
            const char *save = ctx->group_end[node->ch];
            ctx->group_end[node->ch] = sp;
            if (match_node(ctx, node->out, sp)) { ctx->depth--; return 1; }
            ctx->group_end[node->ch] = save;
            ctx->depth--; return 0;
        }
        case ND_BACKREF: {
            int gi = node->ch;
            if (!ctx->group_start[gi] || !ctx->group_end[gi]) {
                ctx->depth--; return 0;
            }
            size_t len = (size_t)(ctx->group_end[gi] - ctx->group_start[gi]);
            if (ctx->cflags & REG_ICASE) {
                size_t i;
                for (i = 0; i < len; i++) {
                    int a = (unsigned char)sp[i];
                    int b = (unsigned char)ctx->group_start[gi][i];
                    if (rx_upper(a)) a += 32;
                    if (rx_upper(b)) b += 32;
                    if (!a || a != b) break;
                }
                if (i != len) { ctx->depth--; return 0; }
            } else if (strncmp(sp, ctx->group_start[gi], len) != 0) {
                ctx->depth--; return 0;
            }
            sp += len;
            node = node->out;
            continue;
        }
        case ND_BRANCH:
        case ND_SPLIT:
            if (match_node(ctx, node->out, sp)) { ctx->depth--; return 1; }
            node = node->out2;
            continue;
        default:
            ctx->depth--; return 0;
        }
    }
    ctx->depth--;
    return 1;
}

/* ---- public API ------------------------------------------------- */

/* An upper bound on the nodes a pattern needs. Each character can produce
 * a couple of nodes; each {n,m} over a COMPOUND body multiplies the
 * fragment before it by up to m. Both the per-interval count and the
 * product are capped, so a pattern like a{1000}{1000} asks for a bounded
 * amount and gets REG_ESPACE rather than trying to allocate the world. */
static int estimate_nodes(const char *pat, int ere) {
    long len = 0;
    for (const char *q = pat; *q; q++) len++;
    long base = len * 4 + 64;
    long factor = 1;
    for (const char *q = pat; *q; q++) {
        int is_open = ere ? (*q == '{') : (*q == '\\' && q[1] == '{');
        if (!is_open) continue;
        const char *r = ere ? q + 1 : q + 2;
        long lo = 0, hi = 0;
        int gotlo = 0, gothi = 0;
        while (*r >= '0' && *r <= '9') { lo = lo * 10 + (*r++ - '0'); gotlo = 1; }
        if (*r == ',') {
            r++;
            while (*r >= '0' && *r <= '9') { hi = hi * 10 + (*r++ - '0'); gothi = 1; }
        }
        long m = gothi ? hi : (gotlo ? lo : 1);
        if (m < 1) m = 1;
        if (m > 256) m = 256;
        factor *= m;
        if (factor > 2048) { factor = 2048; break; }
    }
    long total = base * factor + 64;
    if (total > 300000) total = 300000;
    return (int)total;
}

int regcomp(regex_t *preg, const char *pattern, int cflags) {
    if (!preg || !pattern) return REG_BADPAT;

    compiled_t *comp = (compiled_t *)calloc(1, sizeof(compiled_t));
    if (!comp) return REG_ESPACE;
    comp->cflags = cflags;
    comp->pat    = pattern;

    const char *p = pattern;
    int ere = (cflags & REG_EXTENDED) != 0;

    /* Size the node array ONCE. new_node() no longer grows it, because
     * growing it moved every node and invalidated the pointers the parser
     * holds. A bounded repetition duplicates its fragment, so the bound has
     * to account for the repeat counts in the pattern. */
    comp->cap = estimate_nodes(pattern, ere);
    comp->nodes = (nd_t *)malloc((size_t)comp->cap * sizeof(nd_t));
    if (!comp->nodes) { free(comp); return REG_ESPACE; }

    frag_t root;
    int ok = parse_alt(comp, &p, ere, &root);
    if (ok && *p) {
        /* Something stopped the parse that is not the end of the pattern:
         * an unbalanced ) in an ERE. Silently ignoring the tail meant a
         * bad pattern matched a PREFIX of what was asked for. */
        comp->err = REG_EPAREN;
        ok = 0;
    }
    if (!ok || comp->err) {
        int e = comp->err ? comp->err : REG_BADPAT;
        free(comp->nodes);
        free(comp);
        return e;
    }

    nd_t *end = new_node(comp);
    if (!end) {
        free(comp->nodes);
        free(comp);
        return REG_ESPACE;
    }
    end->type = ND_MATCH;
    /* THE ONE PLACE the accept state gets wired. There used to be a
     * sweep here that pointed every node with a NULL ->out at `end`,
     * which is how a fragment exit orphaned by chain_end() turned into
     * "accept immediately" instead of a compile error. Every node's
     * successor is now set by the construct that created it. */
    root.exit->out = end;

    comp->start = root.entry;
    preg->priv = comp;
    preg->re_nsub = comp->nsub;
    return 0;
}

static void run_from(match_ctx *ctx, compiled_t *comp, const char *sp,
                     int longest, long budget) {
    memset(ctx->group_start, 0, sizeof ctx->group_start);
    memset(ctx->group_end, 0, sizeof ctx->group_end);
    ctx->group_start[0] = sp;
    ctx->depth      = 0;
    ctx->steps      = 0;
    ctx->budget     = budget;
    ctx->longest    = longest;
    ctx->have_match = 0;
    ctx->match_end  = sp;
    if (match_node(ctx, comp->start, sp)) {
        ctx->have_match = 1;
        memcpy(ctx->best_start, ctx->group_start, sizeof ctx->best_start);
        memcpy(ctx->best_end, ctx->group_end, sizeof ctx->best_end);
    }
}

int regexec(const regex_t *preg, const char *string,
            size_t nmatch, regmatch_t pmatch[], int eflags) {
    if (!preg || !preg->priv || !string) return REG_NOMATCH;

    compiled_t *comp = (compiled_t *)preg->priv;
    match_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.str = string;
    ctx.bol = string;
    ctx.eflags = eflags;
    ctx.cflags = comp->cflags;

    /* POSIX says leftmost-LONGEST; a backtracking engine naturally gives
     * leftmost-FIRST. With greedy quantifiers the two agree, so the only
     * patterns that need the slower answer are the ones with alternation:
     * `echo ab | grep -oE 'a|ab'` prints ab in GNU grep and printed a
     * here. Explicitly not done when the pattern uses a non-greedy
     * quantifier, where taking the longest is the opposite of what the
     * author asked for. */
    int want_longest = comp->has_alt && !comp->has_lazy;

    const char *sp = string;
    while (1) {
        run_from(&ctx, comp, sp, 0, 0);
        if (ctx.have_match) {
            if (want_longest) {
                const char *first_end = ctx.match_end;
                const char *fs[MAX_GROUPS], *fe[MAX_GROUPS];
                memcpy(fs, ctx.best_start, sizeof fs);
                memcpy(fe, ctx.best_end, sizeof fe);

                /* Bounded: a pathological pattern falls back to the
                 * leftmost-first answer rather than running forever. */
                run_from(&ctx, comp, sp, 1, 200000);
                if (!ctx.have_match || ctx.match_end < first_end) {
                    ctx.match_end = first_end;
                    memcpy(ctx.best_start, fs, sizeof fs);
                    memcpy(ctx.best_end, fe, sizeof fe);
                }
            }
            if (pmatch && nmatch > 0 && !(comp->cflags & REG_NOSUB)) {
                /* THE MATCH END COMES FROM THE MATCHER, NOT A GUESS.
                 *
                 * This block used to hunt for the end by re-running
                 * match_node in a loop -- but every call in that loop
                 * started from `sp` and ignored the loop variable, so the
                 * result never varied and `best_end` simply walked to the
                 * END OF THE STRING. rm_eo was therefore strlen(string)
                 * for EVERY successful match, whatever it actually
                 * matched. */
                pmatch[0].rm_so = (regoff_t)(sp - string);
                pmatch[0].rm_eo = (regoff_t)(ctx.match_end - string);
                if (pmatch[0].rm_eo < pmatch[0].rm_so)
                    pmatch[0].rm_eo = pmatch[0].rm_so;
                for (size_t i = 1; i < nmatch; i++) {
                    if ((int)i <= comp->nsub && i < MAX_GROUPS &&
                        ctx.best_start[i] && ctx.best_end[i]) {
                        pmatch[i].rm_so = (regoff_t)(ctx.best_start[i] - string);
                        pmatch[i].rm_eo = (regoff_t)(ctx.best_end[i] - string);
                    } else {
                        pmatch[i].rm_so = -1;
                        pmatch[i].rm_eo = -1;
                    }
                }
            }
            return 0;
        }
        if (!*sp) break;
        sp++;
    }
    return REG_NOMATCH;
}

void regfree(regex_t *preg) {
    if (!preg || !preg->priv) return;
    compiled_t *comp = (compiled_t *)preg->priv;
    free(comp->nodes);
    free(comp);
    preg->priv = 0;
}

size_t regerror(int errcode, const regex_t *preg,
                char *errbuf, size_t errbuf_size) {
    (void)preg;
    static const char *msgs[] = {
        "Success",
        "No match",
        "Invalid regex",
        "Invalid collation",
        "Invalid character class",
        "Trailing backslash",
        "Invalid back reference",
        "Unmatched [",
        "Unmatched (",
        "Unmatched {",
        "Invalid range in {}",
        "Invalid range endpoint",
        "Out of memory",
        "Invalid repetition",
    };
    const char *msg = (errcode >= 0 && errcode <= 13) ? msgs[errcode] : "Unknown error";
    size_t len = strlen(msg) + 1;
    if (errbuf && errbuf_size > 0) {
        size_t copy = len < errbuf_size ? len : errbuf_size;
        memcpy(errbuf, msg, copy);
        errbuf[copy - 1] = '\0';
    }
    return len;
}
