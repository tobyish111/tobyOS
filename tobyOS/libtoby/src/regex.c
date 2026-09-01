/* libtoby/src/regex.c -- POSIX BRE/ERE regex engine.
 *
 * NFA-based with backtracking. Compiles to a small IR of node structs,
 * then runs a recursive matcher. Supports: . * + ? | [] [^] ^ $ {n,m}
 * () groups, backrefs \1-\9, \d \w \s shortcuts. */

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
    ND_GROUP_S,    /* start of group i */
    ND_GROUP_E,    /* end of group i */
    ND_BACKREF,    /* \1..\9 */
    ND_BRANCH,     /* alternation: try .out first, then .out2 */
    ND_SPLIT,      /* like BRANCH but for quantifiers */
    ND_MATCH,      /* accept state */
    ND_NOP,        /* pass-through: the EXIT of a quantified
                    * fragment. It cannot be ND_MATCH, because
                    * ND_MATCH accepts on arrival and `foo.*bar`
                    * would then match "fooX" -- the star would
                    * declare success before `bar` was required. */
};

#define CLASS_BITMAP_SIZE 32  /* 256 bits */

typedef struct nd {
    int           type;
    int           ch;             /* ND_LIT char, or group index, or backref */
    unsigned char cclass[CLASS_BITMAP_SIZE];
    int           greedy;
    struct nd    *out;
    struct nd    *out2;
} nd_t;

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
} compiled_t;

/* ---- bit helpers for character classes --------------------------- */

static void cls_set(unsigned char *c, int b) {
    c[(unsigned)b >> 3] |= (unsigned char)(1 << ((unsigned)b & 7));
}

static int cls_test(const unsigned char *c, int b) {
    return (c[(unsigned)b >> 3] >> ((unsigned)b & 7)) & 1;
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
    return n;
}

/* Copy the nodes in [lo,hi) to the end of the array and return the copy of
 * `entry`. Pointers INTO the range are rebased onto the copy; pointers out
 * of it (to a shared tail) are left alone, which is what the caller wants.
 * This is what makes {n,m} possible: an NFA cannot express "exactly three"
 * without three copies of the thing being repeated. */
static nd_t *clone_range(compiled_t *c, int lo, int hi, nd_t *entry) {
    int n = hi - lo;
    if (n <= 0) return entry;
    if (c->nnodes + n > c->cap) { c->err = REG_ESPACE; return 0; }
    int base = c->nnodes;
    for (int i = 0; i < n; i++) c->nodes[base + i] = c->nodes[lo + i];
    c->nnodes += n;
    for (int i = 0; i < n; i++) {
        nd_t *d = &c->nodes[base + i];
        if (d->out  && d->out  >= &c->nodes[lo] && d->out  < &c->nodes[hi])
            d->out  = &c->nodes[base + (int)(d->out  - &c->nodes[lo])];
        if (d->out2 && d->out2 >= &c->nodes[lo] && d->out2 < &c->nodes[hi])
            d->out2 = &c->nodes[base + (int)(d->out2 - &c->nodes[lo])];
    }
    if (entry >= &c->nodes[lo] && entry < &c->nodes[hi])
        return &c->nodes[base + (int)(entry - &c->nodes[lo])];
    return entry;
}

/* Forward decl */
static nd_t *parse_alt(compiled_t *c, const char **pp, int ere);
static nd_t *chain_end(compiled_t *c, nd_t *n);

static int is_meta(char ch, int ere) {
    if (ere) return ch == '|' || ch == ')';
    return 0;
}

static int parse_count(const char **pp) {
    int v = 0;
    while (**pp >= '0' && **pp <= '9') {
        v = v * 10 + (**pp - '0');
        (*pp)++;
    }
    return v;
}

/* Build a quantified chain: repeat `body` between min and max times,
 * where max == -1 means infinity. */
static nd_t *quantify(compiled_t *c, nd_t *body, nd_t *tail,
                      int min, int max, int greedy, int lo, int hi) {
    /* For min copies, just chain body->...->body */
    nd_t *first = 0;
    nd_t *prev_tail = 0;

    /* We can't easily duplicate NFA nodes, so for the simple common
     * cases (*, +, ?, {n,m}) we use split nodes. For complex {n,m}
     * with n > 0 we simply unroll by running the body multiple times. */

    /* Simple cases */
    if (min == 0 && max == -1) {
        /* * : split -> body -> back to split, or split -> tail */
        nd_t *sp = new_node(c);
        if (!sp) return 0;
        sp->type = ND_SPLIT;
        sp->greedy = greedy;
        sp->out = greedy ? body : tail;
        sp->out2 = greedy ? tail : body;
        /* Point body end back to split */
        nd_t *n = chain_end(c, body);
        n->out = sp;
        return sp;
    }
    if (min == 1 && max == -1) {
        /* + : body -> split(body|tail) */
        nd_t *sp = new_node(c);
        if (!sp) return 0;
        sp->type = ND_SPLIT;
        sp->greedy = greedy;
        sp->out = greedy ? body : tail;
        sp->out2 = greedy ? tail : body;
        nd_t *n = chain_end(c, body);
        n->out = sp;
        return body;
    }
    if (min == 0 && max == 1) {
        /* ? : split(body|tail) */
        nd_t *sp = new_node(c);
        if (!sp) return 0;
        sp->type = ND_SPLIT;
        sp->greedy = greedy;
        sp->out = greedy ? body : tail;
        sp->out2 = greedy ? tail : body;
        nd_t *n = chain_end(c, body);
        n->out = tail;
        return sp;
    }

    /* GENERAL {n,m}. This used to fall through to a `*`, so `x{3}` matched
     * ANY number of x -- wrong output rather than an error, which is why it
     * survived. An NFA cannot say "exactly three" without three copies, so
     * the fragment is cloned.
     *
     * Built from the inside out: the optional copies first, each wrapped in
     * a split that can skip to what follows, then the required ones in
     * front. The ORIGINAL fragment is used as the outermost copy and is
     * therefore mutated last, so every clone is taken from a pristine
     * range. */
    if (min < 0) min = 0;
    if (max >= 0 && max < min) max = min;
    if (max == 0) return tail;          /* {0} matches empty */

    nd_t *cur = tail;

    if (max < 0) {
        /* {n,} -- an unbounded tail, so the last copy loops. */
        nd_t *b = clone_range(c, lo, hi, body);
        if (!b) return 0;
        nd_t *sp = new_node(c);
        if (!sp) return 0;
        sp->type = ND_SPLIT;
        sp->greedy = greedy;
        sp->out  = greedy ? b : cur;
        sp->out2 = greedy ? cur : b;
        nd_t *be = chain_end(c, b);
        if (!be) return 0;
        be->out = sp;
        cur = (min > 0) ? b : sp;       /* n>0: one copy is mandatory */
        if (min > 0) min--;
    } else {
        for (int k = 0; k < max - min; k++) {
            nd_t *b = clone_range(c, lo, hi, body);
            if (!b) return 0;
            nd_t *be = chain_end(c, b);
            if (!be) return 0;
            be->out = cur;
            nd_t *sp = new_node(c);
            if (!sp) return 0;
            sp->type = ND_SPLIT;
            sp->greedy = greedy;
            sp->out  = greedy ? b : cur;
            sp->out2 = greedy ? cur : b;
            cur = sp;
        }
    }

    for (int k = 0; k < min; k++) {
        nd_t *b = (k == min - 1) ? body : clone_range(c, lo, hi, body);
        if (!b) return 0;
        nd_t *be = chain_end(c, b);
        if (!be) return 0;
        be->out = cur;
        cur = b;
    }
    return cur;
}

/* Walk to the end of an acyclic chain.
 *
 * The code used `while (n->out) n = n->out;` in six places. quantify()
 * builds a star as a LOOP (body -> split -> body), which is correct for an
 * NFA and fatal for that walk: regcomp hung forever on any pattern
 * containing `*` -- a*, ab*, [0-9]*, aa*. That is the most common construct
 * in regex, so libtoby regex has never worked for it, which is why
 * ports/sbase/grep.c was written fixed-string-only.
 *
 * Bounded by the node pool, so a cycle stops the walk instead of hanging.
 * Callers that KNOW their fragment exit (a quantified fragment exits at the
 * ND_MATCH node handed to quantify) pass it explicitly and never come here. */
static nd_t *chain_end(compiled_t *c, nd_t *n) {
    int guard = 0;
    while (n && n->out) {
        if (++guard > c->nnodes) return n;   /* cycle: stop, do not spin */
        n = n->out;
    }
    return n;
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
        int lo = (unsigned char)*p++;
        int hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            hi = (unsigned char)*p++;
        }
        for (int ch = lo; ch <= hi; ch++) {
            cls_set(node->cclass, ch);
            if (icase) {
                if (ch >= 'a' && ch <= 'z') cls_set(node->cclass, ch - 32);
                if (ch >= 'A' && ch <= 'Z') cls_set(node->cclass, ch + 32);
            }
        }
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

/* Parse a single atom */
static nd_t *parse_atom(compiled_t *c, const char **pp, int ere) {
    const char *p = *pp;
    nd_t *node = 0;

    switch (*p) {
    case '.':
        node = new_node(c);
        if (!node) return 0;
        node->type = ND_DOT;
        p++;
        break;
    case '^':
        node = new_node(c);
        if (!node) return 0;
        node->type = ND_ANCHOR_BOL;
        p++;
        break;
    case '$':
        node = new_node(c);
        if (!node) return 0;
        node->type = ND_ANCHOR_EOL;
        p++;
        break;
    case '[':
        p++;
        node = parse_class(c, &p, c->cflags & REG_ICASE);
        break;
    case '\\':
        p++;
        if (*p >= '1' && *p <= '9') {
            node = new_node(c);
            if (!node) return 0;
            node->type = ND_BACKREF;
            node->ch = *p - '0';
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
                for (int i = 'a'; i <= 'z'; i++) cls_set(node->cclass, i);
                for (int i = 'A'; i <= 'Z'; i++) cls_set(node->cclass, i);
                for (int i = '0'; i <= '9'; i++) cls_set(node->cclass, i);
                cls_set(node->cclass, '_');
                node->type = (*p == 'w') ? ND_CLASS : ND_NCLASS;
            } else {
                cls_set(node->cclass, ' ');
                cls_set(node->cclass, '\t');
                cls_set(node->cclass, '\n');
                cls_set(node->cclass, '\r');
                cls_set(node->cclass, '\f');
                cls_set(node->cclass, '\v');
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
            default:     node->ch = 7;  break;
            }
            p++;
        } else if (*p) {
            node = new_node(c);
            if (!node) return 0;
            node->type = ND_LIT;
            node->ch = (unsigned char)*p++;
        }
        break;
    case '(':
        if (!ere) {
            /* BRE: literal ( */
            node = new_node(c);
            if (!node) return 0;
            node->type = ND_LIT;
            node->ch = (unsigned char)*p++;
            break;
        }
        /* ERE: group */
        p++;
        {
            int gi = ++c->nsub;
            nd_t *gs = new_node(c);
            if (!gs) return 0;
            gs->type = ND_GROUP_S;
            gs->ch = gi;

            nd_t *inner = parse_alt(c, &p, ere);
            if (!inner) return 0;

            nd_t *ge = new_node(c);
            if (!ge) return 0;
            ge->type = ND_GROUP_E;
            ge->ch = gi;

            gs->out = inner;
            nd_t *n = chain_end(c, inner);
            n->out = ge;
            node = gs;
        }
        if (*p == ')') p++;
        break;
    default:
        if (*p && !is_meta(*p, ere) && *p != '*' && *p != '+' && *p != '?') {
            node = new_node(c);
            if (!node) return 0;
            node->type = ND_LIT;
            node->ch = (unsigned char)*p;
            if (c->cflags & REG_ICASE) {
                if (node->ch >= 'A' && node->ch <= 'Z')
                    node->ch += 32;
            }
            p++;
        }
        break;
    }

    *pp = p;
    return node;
}

/* Parse a BRE group \(...\) */
static nd_t *parse_bre_group(compiled_t *c, const char **pp) {
    const char *p = *pp;
    int gi = ++c->nsub;
    nd_t *gs = new_node(c);
    if (!gs) return 0;
    gs->type = ND_GROUP_S;
    gs->ch = gi;

    nd_t *inner = parse_alt(c, &p, 0);
    if (!inner) return 0;

    nd_t *ge = new_node(c);
    if (!ge) return 0;
    ge->type = ND_GROUP_E;
    ge->ch = gi;

    gs->out = inner;
    nd_t *n = chain_end(c, inner);
    n->out = ge;

    if (*p == '\\' && p[1] == ')') p += 2;
    *pp = p;
    return gs;
}

/* Parse a sequence of atoms with optional quantifiers */
static nd_t *parse_seq(compiled_t *c, const char **pp, int ere) {
    const char *p = *pp;

    /* Dummy head to chain from */
    nd_t head;
    memset(&head, 0, sizeof(head));
    nd_t *tail = &head;

    while (*p && *p != '|' && *p != ')' && !(*p == '\\' && p[1] == ')') &&
           !(*p == '\\' && p[1] == '|' && !ere)) {

        /* The atom occupies nodes [atom_lo, atom_hi); a bounded
         * quantifier has to CLONE that range. */
        int atom_lo = c->nnodes, atom_hi;
        nd_t *atom = 0;
        nd_t *frag_end = 0;   /* known exit for quantified fragments */

        /* BRE: \( starts a group */
        if (!ere && *p == '\\' && p[1] == '(') {
            p += 2;
            atom = parse_bre_group(c, &p);
        } else {
            atom = parse_atom(c, &p, ere);
        }
        if (!atom) break;

        /* Check for quantifiers */
        int greedy = 1;
        if (*p == '*' || (ere && (*p == '+' || *p == '?'))) {
            char q = *p++;
            if (*p == '?') { greedy = 0; p++; }

            atom_hi = c->nnodes;
            nd_t *match = new_node(c);
            if (!match) return 0;
            match->type = ND_NOP;

            int qmin = 0, qmax = -1;
            if (q == '*') { qmin = 0; qmax = -1; }
            else if (q == '+') { qmin = 1; qmax = -1; }
            else { qmin = 0; qmax = 1; }

            atom = quantify(c, atom, match, qmin, qmax, greedy, atom_lo, atom_hi);
            if (!atom) return 0;
            frag_end = match;
        }
        /* BRE quantifiers: \{n,m\} */
        else if (!ere && *p == '\\' && p[1] == '{') {
            p += 2;
            int qmin = parse_count(&p);
            int qmax = qmin;
            if (*p == ',') {
                p++;
                if (*p == '\\') { qmax = -1; }
                else { qmax = parse_count(&p); }
            }
            if (*p == '\\' && p[1] == '}') p += 2;
            atom_hi = c->nnodes;
            nd_t *match = new_node(c);
            if (!match) return 0;
            match->type = ND_NOP;
            atom = quantify(c, atom, match, qmin, qmax, greedy, atom_lo, atom_hi);
            if (!atom) return 0;
            frag_end = match;
        }
        /* ERE: {n,m} */
        else if (ere && *p == '{') {
            p++;
            int qmin = parse_count(&p);
            int qmax = qmin;
            if (*p == ',') {
                p++;
                if (*p == '}') { qmax = -1; }
                else { qmax = parse_count(&p); }
            }
            if (*p == '}') p++;
            greedy = 1;
            if (*p == '?') { greedy = 0; p++; }

            atom_hi = c->nnodes;
            nd_t *match = new_node(c);
            if (!match) return 0;
            match->type = ND_NOP;
            atom = quantify(c, atom, match, qmin, qmax, greedy, atom_lo, atom_hi);
            if (!atom) return 0;
        }

        /* A quantified fragment EXITS at the ND_MATCH node handed to
         * quantify(); its body is a loop and walking it never terminates.
         * Using that known exit is both correct and the reason regcomp no
         * longer hangs on `*`. Plain atoms are acyclic, so the guarded walk
         * finds their end. */
        tail->out = atom;
        if (frag_end) tail = frag_end;
        else          tail = chain_end(c, tail);
    }

    *pp = p;
    return head.out;
}

/* Parse alternatives (a|b) */
static nd_t *parse_alt(compiled_t *c, const char **pp, int ere) {
    nd_t *left = parse_seq(c, pp, ere);
    if (!left) {
        left = new_node(c);
        if (!left) return 0;
        left->type = ND_MATCH;
    }

    const char *p = *pp;
    char sep = ere ? '|' : '\\';

    while ((ere && *p == '|') || (!ere && *p == '\\' && p[1] == '|')) {
        if (ere) p++;
        else p += 2;

        nd_t *right = parse_seq(c, &p, ere);
        if (!right) {
            right = new_node(c);
            if (!right) return 0;
            right->type = ND_MATCH;
        }

        nd_t *br = new_node(c);
        if (!br) return 0;
        br->type = ND_BRANCH;
        br->out = left;
        br->out2 = right;
        left = br;
    }

    *pp = p;
    (void)sep;
    return left;
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
} match_ctx;

#define MAX_DEPTH 4096

static int match_node(match_ctx *ctx, nd_t *node, const char *sp);

static int match_node(match_ctx *ctx, nd_t *node, const char *sp) {
    if (ctx->depth++ > MAX_DEPTH) return 0;

    while (node) {
        switch (node->type) {
        case ND_MATCH:
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
        case ND_LIT: {
            if (!*sp) { ctx->depth--; return 0; }
            int ch = (unsigned char)*sp;
            int want = node->ch;
            if (ctx->cflags & REG_ICASE) {
                if (ch >= 'A' && ch <= 'Z') ch += 32;
                if (want >= 'A' && want <= 'Z') want += 32;
            }
            if (ch != want) { ctx->depth--; return 0; }
            sp++;
            node = node->out;
            continue;
        }
        case ND_DOT:
            if (!*sp) { ctx->depth--; return 0; }
            if ((ctx->cflags & REG_NEWLINE) && *sp == '\n') {
                ctx->depth--; return 0;
            }
            sp++;
            node = node->out;
            continue;
        case ND_CLASS:
            if (!*sp || !cls_test(node->cclass, (unsigned char)*sp)) {
                ctx->depth--; return 0;
            }
            sp++;
            node = node->out;
            continue;
        case ND_NCLASS:
            if (!*sp || cls_test(node->cclass, (unsigned char)*sp)) {
                ctx->depth--; return 0;
            }
            sp++;
            node = node->out;
            continue;
        case ND_ANCHOR_BOL:
            if (sp != ctx->bol) {
                if ((ctx->cflags & REG_NEWLINE) && sp > ctx->bol && sp[-1] == '\n') {
                    /* ok */
                } else if (ctx->eflags & REG_NOTBOL) {
                    ctx->depth--; return 0;
                } else if (sp != ctx->bol) {
                    ctx->depth--; return 0;
                }
            }
            node = node->out;
            continue;
        case ND_ANCHOR_EOL:
            if (*sp != '\0') {
                if ((ctx->cflags & REG_NEWLINE) && *sp == '\n') {
                    /* ok */
                } else {
                    ctx->depth--; return 0;
                }
            }
            node = node->out;
            continue;
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
            if (strncmp(sp, ctx->group_start[gi], len) != 0) {
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
 * a couple of nodes; each {n,m} multiplies the fragment before it by up to
 * m. Both the per-interval count and the product are capped, so a pattern
 * like a{1000}{1000} asks for a bounded amount and gets REG_ESPACE rather
 * than trying to allocate the world. */
static int estimate_nodes(const char *pat, int ere) {
    long len = 0;
    for (const char *q = pat; *q; q++) len++;
    long base = len * 3 + 32;
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
    long total = base * factor + 32;
    if (total > 300000) total = 300000;
    return (int)total;
}

int regcomp(regex_t *preg, const char *pattern, int cflags) {
    if (!preg || !pattern) return REG_BADPAT;

    compiled_t *comp = (compiled_t *)calloc(1, sizeof(compiled_t));
    if (!comp) return REG_ESPACE;
    comp->cflags = cflags;

    const char *p = pattern;
    int ere = (cflags & REG_EXTENDED) != 0;

    /* Size the node array ONCE. new_node() no longer grows it, because
     * growing it moved every node and invalidated the pointers the parser
     * holds. A bounded repetition duplicates its fragment, so the bound has
     * to account for the repeat counts in the pattern. */
    comp->cap = estimate_nodes(pattern, ere);
    comp->nodes = (nd_t *)malloc((size_t)comp->cap * sizeof(nd_t));
    if (!comp->nodes) { free(comp); return REG_ESPACE; }

    nd_t *root = parse_alt(comp, &p, ere);
    if (comp->err) {
        int e = comp->err;
        free(comp->nodes);
        free(comp);
        return e;
    }
    if (!root) {
        free(comp->nodes);
        free(comp);
        return REG_BADPAT;
    }

    /* Append match node at end of all branches */
    nd_t *end = new_node(comp);
    if (!end) {
        free(comp->nodes);
        free(comp);
        return REG_ESPACE;
    }
    end->type = ND_MATCH;

    /* Wire dangling ends to the match node */
    for (int i = 0; i < comp->nnodes - 1; i++) {
        nd_t *n = &comp->nodes[i];
        if (n->type != ND_BRANCH && n->type != ND_SPLIT && !n->out)
            n->out = end;
    }

    comp->start = root;
    preg->priv = comp;
    preg->re_nsub = comp->nsub;
    return 0;
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

    /* Try match at each position */
    const char *sp = string;
    while (1) {
        memset(ctx.group_start, 0, sizeof(ctx.group_start));
        memset(ctx.group_end, 0, sizeof(ctx.group_end));
        ctx.group_start[0] = sp;
        ctx.depth = 0;
        ctx.match_end = sp;

        if (match_node(&ctx, comp->start, sp)) {
            if (pmatch && nmatch > 0 && !(comp->cflags & REG_NOSUB)) {
                /* THE MATCH END COMES FROM THE MATCHER, NOT A GUESS.
                 *
                 * This block used to hunt for the end by re-running
                 * match_node in a loop -- but every call in that loop
                 * started from `sp` and ignored the loop variable, so the
                 * result never varied and `best_end` simply walked to the
                 * END OF THE STRING. rm_eo was therefore strlen(string)
                 * for EVERY successful match, whatever it actually
                 * matched. Nothing in the tree read rm_eo until tvi's
                 * :s did, which is why it survived: a substitute of
                 * /a/b/ on "aaa" produced "b", silently eating the tail.
                 *
                 * ND_MATCH is reached with sp exactly at the end of the
                 * accepting path, so the matcher records it and we read
                 * it here. One traversal, and it is correct by
                 * construction rather than by search. */
                pmatch[0].rm_so = (regoff_t)(sp - string);
                pmatch[0].rm_eo = (regoff_t)(ctx.match_end - string);
                if (pmatch[0].rm_eo < pmatch[0].rm_so)
                    pmatch[0].rm_eo = pmatch[0].rm_so;
                for (size_t i = 1; i < nmatch; i++) {
                    if ((int)i <= comp->nsub && ctx.group_start[i] && ctx.group_end[i]) {
                        pmatch[i].rm_so = (regoff_t)(ctx.group_start[i] - string);
                        pmatch[i].rm_eo = (regoff_t)(ctx.group_end[i] - string);
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
