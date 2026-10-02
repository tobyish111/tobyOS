/* ---- bash's grammar, as a check (B9, 2026-10-01) ---------------------------
 *
 * bash READS A WHOLE COMMAND BEFORE IT RUNS ANY OF IT. Its parser (parse.y:
 * an LALR grammar fed by a context-sensitive lexer) takes one complete
 * command -- everything up to the newline that ends it at the top level,
 * here-document bodies included -- and only then executes it. A command
 * that does not parse does not run at all, and bash names the first token
 * its grammar cannot accept:
 *
 *     if true; then echo; fi fi   bash: syntax error near unexpected token `fi'
 *     a &&                        bash: syntax error: unexpected end of file
 *
 * tsh executes text as it reads it, with syntax checks spread through the
 * executor in its own words, so it ran `if ...; fi` and then complained in
 * its own way, ran `a`, and accepted `;` alone. This is the front half that
 * was missing: a RECOGNIZER for bash's grammar, run on each command before
 * tsh executes any of it. It builds nothing. It answers where a command
 * ends, whether it parses, and if not what bash says and on which line:
 *   - the lexer is bash's (read_token, read_token_word, parse_matched_pair):
 *     reserved words only where a command may start, the case/for/function/
 *     time special cases, quoting, $( ) parsed as a command list, $(( )),
 *     ${ }, $[ ], `...`, <( ) >( ), extglob patterns, array literals, (( )),
 *     [[ ]] with its own sub-grammar, here-documents and alias expansion,
 *     with bash's line counting (it counts a line when it READS it);
 *   - the parser recognizes the same language as bash's yacc grammar. An
 *     LR parser stops at the first token that cannot extend a valid prefix;
 *     so does this, and that token is the one bash names;
 *   - what happens next is bash's too: a grammar error prints the message
 *     and the offending line and ends the input with status 2; a lexer error
 *     (an unclosed quote, a bad [[ ]]) prints only its own message and leaves
 *     $? alone unless the input then ends; an error in an array literal
 *     drops the rest of the line and goes on; one inside $( ) ends the whole
 *     shell input, eval or not.
 * Written from bash-5.2.15's source as the specification, and checked
 * against bash 5.2.15 itself (tools/agent/bashsyn/): the same verdict,
 * message and line for every script of the Oils corpus and for thousands
 * of damaged ones.
 *
 * Self-contained: it allocates through BSYN_ALLOC/BSYN_FREE and reaches the
 * shell only through the alias lookup it is handed, so the host harness
 * compiles this very file. Out of memory or nesting too deep, it answers
 * BSYN_SKIP -- "not checked" -- and the shell runs the text as before. */

#ifndef BSYN_ALLOC
#define BSYN_ALLOC(n) kmalloc(n)
#define BSYN_FREE(p)  kfree(p)
#endif

/* ---- the interface ---------------------------------------------------- */

struct bsyn_env {
    bool posix;             /* set -o posix */
    bool extglob;           /* shopt extglob, as it stands now */
    bool expand_aliases;    /* aliases expand while reading */
    bool interactive;       /* no offending-line echo */
    bool is_string;         /* eval / -c text, not a file */
    /* The alias NAME's value, or 0. *expand_next: it ends in a blank. */
    const char *(*alias)(void *ctx, const char *name, bool *expand_next);
    void *ctx;
    int max_depth;          /* nesting bound; beyond it, BSYN_SKIP */
};

#define BSYN_OK       0     /* one complete command was read */
#define BSYN_EOF      1     /* nothing but blanks and comments to the end */
#define BSYN_ERROR    2     /* a syntax error: the input stops here */
#define BSYN_DISCARD  3     /* an array-literal error: resume at `end` */
#define BSYN_SKIP     4     /* not checked (memory, depth): run as before */

#define BSYN_MSG_PARSER  0  /* parser_error: "NAME: [eval: ]line N: TEXT" */
#define BSYN_MSG_WARNING 1  /* internal_warning: "NAME: line N: warning: TEXT" */

#define BSYN_MAX_MSG 8

struct bsyn_msg {
    unsigned long line;
    int kind;
    char *text;
};

struct bsyn_result {
    int kind;
    size_t end;               /* offset just past the command (OK, DISCARD) */
    unsigned long end_line;   /* bash's line counter after it */
    /* $? afterwards: status_set -> status; else status_ifzero, when $? is
     * 0, becomes it; else $? is left as it is (a [[ ]] error). */
    bool status_set;
    int status;
    int status_ifzero;
    bool fatal;               /* an error inside $( ): ends the shell input */
    int nmsg;
    struct bsyn_msg msg[BSYN_MAX_MSG];
};

/* ---- tokens: bash's, with bash's numbers (y.tab.h) -- a [[ ]] message
 * prints one -- and the one-character ones as themselves --------------- */

enum {
    BT_ERROR = -1,
    BT_IF = 258, BT_THEN, BT_ELSE, BT_ELIF, BT_FI, BT_CASE, BT_ESAC, BT_FOR,
    BT_SELECT, BT_WHILE, BT_UNTIL, BT_DO, BT_DONE, BT_FUNCTION, BT_COPROC,
    BT_COND_START, BT_COND_END, BT_COND_ERROR, BT_IN, BT_BANG, BT_TIME,
    BT_TIMEOPT, BT_TIMEIGN,
    BT_WORD, BT_ASSIGNMENT_WORD, BT_REDIR_WORD, BT_NUMBER, BT_ARITH_CMD,
    BT_ARITH_FOR_EXPRS, BT_COND_CMD,
    BT_AND_AND, BT_OR_OR, BT_GREATER_GREATER, BT_LESS_LESS, BT_LESS_AND,
    BT_LESS_LESS_LESS, BT_GREATER_AND, BT_SEMI_SEMI, BT_SEMI_AND,
    BT_SEMI_SEMI_AND, BT_LESS_LESS_MINUS, BT_AND_GREATER,
    BT_AND_GREATER_GREATER, BT_LESS_GREATER, BT_GREATER_BAR, BT_BAR_AND,
    BT_DOLPAREN,              /* 304 */
    BT_EOF,                   /* 305: yacc_EOF */
    BT_REREAD                 /* an alias was pushed: read the token again */
};

struct bsyn_tokname { const char *w; int t; };

static const struct bsyn_tokname bsyn_reserved[] = {
    {"if", BT_IF}, {"then", BT_THEN}, {"else", BT_ELSE}, {"elif", BT_ELIF},
    {"fi", BT_FI}, {"case", BT_CASE}, {"esac", BT_ESAC}, {"for", BT_FOR},
    {"select", BT_SELECT}, {"while", BT_WHILE}, {"until", BT_UNTIL},
    {"do", BT_DO}, {"done", BT_DONE}, {"in", BT_IN},
    {"function", BT_FUNCTION}, {"time", BT_TIME}, {"{", '{'}, {"}", '}'},
    {"!", BT_BANG}, {"[[", BT_COND_START}, {"]]", BT_COND_END},
    {"coproc", BT_COPROC}, {0, 0}
};

static const struct bsyn_tokname bsyn_others[] = {
    {"--", BT_TIMEIGN}, {"-p", BT_TIMEOPT}, {"&&", BT_AND_AND},
    {"||", BT_OR_OR}, {">>", BT_GREATER_GREATER}, {"<<", BT_LESS_LESS},
    {"<&", BT_LESS_AND}, {">&", BT_GREATER_AND}, {";;", BT_SEMI_SEMI},
    {";&", BT_SEMI_AND}, {";;&", BT_SEMI_SEMI_AND},
    {"<<-", BT_LESS_LESS_MINUS}, {"<<<", BT_LESS_LESS_LESS},
    {"&>", BT_AND_GREATER}, {"&>>", BT_AND_GREATER_GREATER},
    {"<>", BT_LESS_GREATER}, {">|", BT_GREATER_BAR}, {"|&", BT_BAR_AND},
    {"EOF", BT_EOF}, {">", '>'}, {"<", '<'}, {"-", '-'}, {"{", '{'},
    {"}", '}'}, {";", ';'}, {"(", '('}, {")", ')'}, {"|", '|'}, {"&", '&'},
    {"newline", '\n'}, {0, 0}
};

/* The parser-state bits the lexer consults (bash's PST_*). */
#define BP_CASEPAT     0x000001u
#define BP_ALEXPNEXT   0x000002u
#define BP_ALLOWOPNBRC 0x000004u
#define BP_SUBSHELL    0x000020u
#define BP_CMDSUBST    0x000040u
#define BP_CASESTMT    0x000080u
#define BP_CONDCMD     0x000100u
#define BP_CONDEXPR    0x000200u
#define BP_EXTPAT      0x001000u
#define BP_COMPASSIGN  0x002000u
#define BP_ASSIGNOK    0x004000u
#define BP_EOFTOKEN    0x008000u
#define BP_REGEXP      0x010000u
#define BP_REDIRLIST   0x080000u
#define BP_COMMENT     0x100000u
#define BP_ENDALIAS    0x400000u
#define BP_NOEXPAND    0x800000u

/* parse_matched_pair's flags */
#define BM_FIRSTCLOSE 0x01
#define BM_ALLOWESC   0x02
#define BM_DQUOTE     0x04
#define BM_COMMAND    0x08
#define BM_ARRAYSUB   0x20
#define BM_DOLBRACE   0x40
#define BM_ARITH      0x80

#define BSYN_EOFC (-1)
#define BSYN_HEREDOC_MAX 16

struct bsyn_buf { char *p; size_t len, cap; };

/* A string pushed in front of the input: an alias's value, or the text a
 * `((` gave back when it turned out to be two subshells. */
struct bsyn_push {
    struct bsyn_push *prev;
    char *text;
    size_t tlen;
    /* the line it interrupted */
    const char *sl;
    size_t slen, sidx;
    int sterm;
    bool seof;
    int ovr_idx, ovr_c;
    bool dparen;
    bool expand_next;
    char *alias;            /* the alias being expanded, by name */
};

struct bsyn_heredoc { char *delim; bool quoted, strip; };

struct bsyn {
    const struct bsyn_env *env;
    struct bsyn_result *res;
    const char *buf;
    size_t len;
    size_t next;              /* the next physical line to read */
    /* shell_input_line: a physical line, its newline put back virtually */
    const char *sl;
    size_t slen, sidx;
    int sterm;                /* the character after the line: '\n', or 0 */
    bool seof;
    bool have_line;
    int ovr_idx, ovr_c;       /* a different character ungotten here */
    int eol_unget;
    bool unq_bs;
    struct bsyn_push *push;
    unsigned long lineno;
    /* capture of what getc returns, for the text of a $( ) */
    struct bsyn_buf *cap;
    /* the lexer */
    int cur, last, before, two_ago;
    int to_read;
    unsigned pst;
    int esacs, expect_in, braces;
    int eof_tok;
    bool extglob, expand_aliases;
    struct bsyn_buf tok;       /* the word being built */
    struct bsyn_buf wtext;     /* yylval: the last word's text */
    bool wquoted;
    long number;
    char *dstack;
    int ddepth, dcap;
    struct bsyn_heredoc hd[BSYN_HEREDOC_MAX];
    int nhd;
    int cond_token;
    unsigned long cond_lineno;
    bool eof;                  /* EOF_Reached */
    /* the parser */
    bool have_la;
    int depth;
    bool failed;               /* an error: unwinding */
    bool lexfail;              /* ...from the lexer (bison's YYerror) */
    bool discard;              /* ...an array literal's: resume next line */
    bool fatal;                /* ...inside $( ) */
    bool oom;                  /* give up: BSYN_SKIP */
    bool st_set;
    int st_val, st_ifzero;
};

/* ---- small helpers ------------------------------------------------------ */

static size_t bsyn_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int bsyn_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static bool bsyn_blank(int c) { return c == ' ' || c == '\t'; }
static bool bsyn_meta(int c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == ';' ||
           c == '&' || c == '|';
}
static bool bsyn_break(int c) {
    return bsyn_meta(c) || c == ' ' || c == '\t' || c == '\n';
}
static bool bsyn_quotec(int c) { return c == '"' || c == '\'' || c == '`'; }
static bool bsyn_digit(int c) { return c >= '0' && c <= '9'; }
static bool bsyn_alpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool bsyn_var_start(int c) { return bsyn_alpha(c) || c == '_'; }
static bool bsyn_var_char(int c) { return bsyn_alpha(c) || bsyn_digit(c) || c == '_'; }

static void bsyn_nomem(struct bsyn *st) { st->oom = true; st->failed = true; }

static bool bsyn_room(struct bsyn *st, struct bsyn_buf *b, size_t more) {
    if (b->len + more + 1 <= b->cap) return true;
    size_t nc = b->cap ? b->cap * 2 : 64;
    while (nc < b->len + more + 1) nc *= 2;
    char *np = (char *)BSYN_ALLOC(nc);
    if (!np) { bsyn_nomem(st); return false; }
    if (b->len) memcpy(np, b->p, b->len);
    np[b->len] = '\0';
    if (b->p) BSYN_FREE(b->p);
    b->p = np;
    b->cap = nc;
    return true;
}

static void bsyn_putc(struct bsyn *st, struct bsyn_buf *b, int c) {
    if (!bsyn_room(st, b, 1)) return;
    b->p[b->len++] = (char)c;
    b->p[b->len] = '\0';
}

static void bsyn_putn(struct bsyn *st, struct bsyn_buf *b, const char *s, size_t n) {
    if (!n) { if (!b->p) (void)bsyn_room(st, b, 0); return; }
    if (!bsyn_room(st, b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void bsyn_puts(struct bsyn *st, struct bsyn_buf *b, const char *s) {
    bsyn_putn(st, b, s, bsyn_strlen(s));
}

static void bsyn_buf_free(struct bsyn_buf *b) {
    if (b->p) BSYN_FREE(b->p);
    b->p = 0;
    b->len = b->cap = 0;
}

static char *bsyn_strdup(struct bsyn *st, const char *s, size_t n) {
    char *r = (char *)BSYN_ALLOC(n + 1);
    if (!r) { bsyn_nomem(st); return 0; }
    if (n) memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static void bsyn_put_ulong(struct bsyn *st, struct bsyn_buf *b, unsigned long v) {
    char d[24];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) bsyn_putc(st, b, d[--n]);
}

/* A message, as bash words it; the caller adds the "NAME: line N: ". */
static void bsyn_msg(struct bsyn *st, unsigned long line, int kind,
                     const char *a, const char *b, const char *c) {
    struct bsyn_result *r = st->res;
    if (r->nmsg >= BSYN_MAX_MSG) return;
    struct bsyn_buf m = {0, 0, 0};
    if (a) bsyn_puts(st, &m, a);
    if (b) bsyn_puts(st, &m, b);
    if (c) bsyn_puts(st, &m, c);
    if (!m.p) { bsyn_buf_free(&m); return; }
    r->msg[r->nmsg].line = line;
    r->msg[r->nmsg].kind = kind;
    r->msg[r->nmsg].text = m.p;
    r->nmsg++;
}

/* $? bookkeeping: bash's last_command_exit_value as the parse leaves it. */
static void bsyn_status_set(struct bsyn *st, int v) { st->st_set = true; st->st_val = v; }
static void bsyn_status_ifzero(struct bsyn *st, int v) {
    if (st->st_set) { if (st->st_val == 0) st->st_val = v; }
    else if (!st->st_ifzero) st->st_ifzero = v;
}

/* ---- the delimiter stack (bash's dstack) -------------------------------- */

static void bsyn_dpush(struct bsyn *st, int c) {
    if (st->ddepth + 1 >= st->dcap) {
        int nc = st->dcap ? st->dcap * 2 : 16;
        char *np = (char *)BSYN_ALLOC((size_t)nc);
        if (!np) { bsyn_nomem(st); return; }
        if (st->ddepth) memcpy(np, st->dstack, (size_t)st->ddepth);
        if (st->dstack) BSYN_FREE(st->dstack);
        st->dstack = np;
        st->dcap = nc;
    }
    st->dstack[st->ddepth++] = (char)c;
}
static void bsyn_dpop(struct bsyn *st) { if (st->ddepth > 0) st->ddepth--; }
static int bsyn_curdelim(struct bsyn *st) {
    return st->ddepth ? (unsigned char)st->dstack[st->ddepth - 1] : 0;
}

/* ---- input: bash's shell_getc ------------------------------------------- *
 * bash reads a LINE at a time into shell_input_line, puts the newline back
 * on the end, and counts the line when it reads it -- so a message's line
 * number is the line the lexer is on, and every read at end of input
 * counts one more. A backslash-newline is removed here, before the lexer
 * sees it, unless the caller is inside single quotes or escaping. */

static int bsyn_at(struct bsyn *st, size_t i) {
    if ((int)i == st->ovr_idx) return st->ovr_c;
    if (i < st->slen) return (unsigned char)st->sl[i];
    if (i == st->slen) return st->sterm;
    return 0;
}

static void bsyn_read_line(struct bsyn *st, int rmnl) {
    st->ovr_idx = -1;
    st->sidx = 0;
    st->have_line = true;
    if (st->next >= st->len) {
        st->sl = "";
        st->slen = 0;
        st->sterm = 0;
        st->seof = true;
        return;
    }
    size_t s = st->next, e = s;
    while (e < st->len && st->buf[e] != '\n' && st->buf[e] != '\0') e++;
    bool had_nl = (e < st->len && st->buf[e] == '\n');
    st->next = had_nl ? e + 1 : (e < st->len ? st->len : e);
    size_t n = e - s;
    /* A CRLF line is a line: tsh's reader has always taken "\r\n" as a line
     * end, and bash would choke on the `\r`. Kept, so the check never
     * rejects a script tsh ran before. */
    if (had_nl && n && st->buf[s + n - 1] == '\r') n--;
    st->sl = st->buf + s;
    st->slen = n;
    st->seof = false;
    st->sterm = '\n';
    /* eval / -c text whose last line ends in an unpaired backslash: bash
     * puts a second backslash there instead of the newline. */
    if (!had_nl && st->env->is_string && rmnl && !st->push) {
        size_t k = 0;
        while (k < n && st->sl[n - 1 - k] == '\\') k++;
        if (k & 1u) st->sterm = '\\';
    }
}

static bool bsyn_expanding_alias(struct bsyn *st) {
    return st->push && !st->push->dparen;
}

static void bsyn_pop_string(struct bsyn *st);

static int bsyn_getc(struct bsyn *st, int rmnl) {
    int uc;
    if (st->eol_unget) {
        int c = st->eol_unget;
        st->eol_unget = 0;
        if (st->cap) bsyn_putc(st, st->cap, c);
        return c;
    }
    if (!st->have_line || (bsyn_at(st, st->sidx) == 0 && !st->push)) {
        st->lineno++;
restart_read:
        bsyn_read_line(st, rmnl);
    }
next_alias_char:
    if (st->sidx == 0) st->unq_bs = false;
    uc = bsyn_at(st, st->sidx);
    if (uc) {
        st->unq_bs = !st->unq_bs && uc == '\\';
        st->sidx++;
    }
    /* The end of an alias's text reads as one blank first, so the alias is
     * still "being expanded" while its last word is checked. */
    if (uc == 0 && st->push && !st->push->dparen &&
        !(st->pst & (BP_COMMENT | BP_ENDALIAS)) && st->sidx > 0) {
        int pc = bsyn_at(st, st->sidx - 1);
        int cd = bsyn_curdelim(st);
        if (!bsyn_blank(pc) && pc != '\n' && !st->unq_bs && !bsyn_meta(pc) &&
            cd != '\'' && cd != '"') {
            st->pst |= BP_ENDALIAS;
            if (st->cap) bsyn_putc(st, st->cap, ' ');
            return ' ';
        }
    }
pop_alias:
    if (uc == 0 && st->push) {
        st->pst &= ~BP_ENDALIAS;
        bsyn_pop_string(st);
        uc = bsyn_at(st, st->sidx);
        if (uc) st->sidx++;
    }
    if (uc == '\\' && rmnl && bsyn_at(st, st->sidx) == '\n') {
        st->lineno++;
        if (bsyn_expanding_alias(st) && bsyn_at(st, st->sidx + 1) == 0) {
            uc = 0;
            goto pop_alias;
        } else if (bsyn_expanding_alias(st)) {
            st->sidx++;
            goto next_alias_char;
        }
        goto restart_read;
    }
    if (uc == 0 && st->seof) return st->sidx != 0 ? '\n' : BSYN_EOFC;
    if (st->cap && uc) bsyn_putc(st, st->cap, uc);
    return uc;
}

static void bsyn_ungetc(struct bsyn *st, int c) {
    if (c == BSYN_EOFC) return;
    if (st->cap && st->cap->len) st->cap->p[--st->cap->len] = '\0';
    if (st->have_line && st->sidx) {
        st->sidx--;
        if (bsyn_at(st, st->sidx) != c) {
            st->ovr_idx = (int)st->sidx;
            st->ovr_c = c;
        }
    } else {
        st->eol_unget = c;
    }
}

static void bsyn_push_string(struct bsyn *st, const char *s, size_t n,
                             bool dparen, bool expand_next, const char *alias) {
    struct bsyn_push *p = (struct bsyn_push *)BSYN_ALLOC(sizeof *p);
    if (!p) { bsyn_nomem(st); return; }
    p->text = bsyn_strdup(st, s, n);
    p->alias = alias ? bsyn_strdup(st, alias, bsyn_strlen(alias)) : 0;
    if (!p->text || (alias && !p->alias)) {
        if (p->text) BSYN_FREE(p->text);
        if (p->alias) BSYN_FREE(p->alias);
        BSYN_FREE(p);
        return;
    }
    p->tlen = n;
    p->sl = st->sl; p->slen = st->slen; p->sidx = st->sidx;
    p->sterm = st->sterm; p->seof = st->seof;
    p->ovr_idx = st->ovr_idx; p->ovr_c = st->ovr_c;
    p->dparen = dparen;
    p->expand_next = expand_next;
    p->prev = st->push;
    st->push = p;
    st->sl = p->text; st->slen = n; st->sidx = 0;
    st->sterm = 0; st->seof = false; st->ovr_idx = -1;
    st->have_line = true;
    /* bash's push_string leaves PST_ALEXPNEXT alone (its clearing is
     * `#if 0`): an alias ending in a blank reaches into the next alias's
     * own text. */
}

static void bsyn_pop_string(struct bsyn *st) {
    struct bsyn_push *p = st->push;
    if (!p) return;
    st->sl = p->sl; st->slen = p->slen; st->sidx = p->sidx;
    st->sterm = p->sterm; st->seof = p->seof;
    st->ovr_idx = p->ovr_idx; st->ovr_c = p->ovr_c;
    if (p->expand_next) st->pst |= BP_ALEXPNEXT;
    else st->pst &= ~BP_ALEXPNEXT;
    st->push = p->prev;
    BSYN_FREE(p->text);
    if (p->alias) BSYN_FREE(p->alias);
    BSYN_FREE(p);
}

static bool bsyn_alias_in_use(struct bsyn *st, const char *name) {
    for (struct bsyn_push *p = st->push; p; p = p->prev)
        if (p->alias && bsyn_streq(p->alias, name)) return true;
    return false;
}

/* ---- the token tables' rules -------------------------------------------- */

static int bsyn_reserved_ok(struct bsyn *st, int t) {
    switch (t) {
    case '\n': case ';': case '(': case ')': case '|': case '&': case '{':
    case '}': case BT_AND_AND: case BT_ARITH_CMD: case BT_BANG: case BT_BAR_AND:
    case BT_COND_END: case BT_DO: case BT_DONE: case BT_ELIF: case BT_ELSE:
    case BT_ESAC: case BT_FI: case BT_IF: case BT_OR_OR: case BT_SEMI_SEMI:
    case BT_SEMI_AND: case BT_SEMI_SEMI_AND: case BT_THEN: case BT_TIME:
    case BT_TIMEOPT: case BT_TIMEIGN: case BT_COPROC: case BT_UNTIL:
    case BT_WHILE: case 0: case BT_DOLPAREN:
        return 1;
    default:
        if (st->last == BT_WORD && st->before == BT_COPROC) return 1;
        if (st->last == BT_WORD && st->before == BT_FUNCTION) return 1;
        return 0;
    }
}

static bool bsyn_parsing_redir(int t) {
    return t == '<' || t == '>' || t == BT_GREATER_GREATER || t == BT_GREATER_BAR ||
           t == BT_LESS_GREATER || t == BT_LESS_LESS_MINUS || t == BT_LESS_LESS ||
           t == BT_LESS_LESS_LESS || t == BT_LESS_AND || t == BT_GREATER_AND ||
           t == BT_AND_GREATER;
}

static bool bsyn_command_pos(struct bsyn *st, int t) {
    return t == BT_ASSIGNMENT_WORD ||
           ((st->pst & BP_REDIRLIST) && !bsyn_parsing_redir(t)) ||
           (t != BT_SEMI_SEMI && t != BT_SEMI_AND && t != BT_SEMI_SEMI_AND &&
            bsyn_reserved_ok(st, t));
}

static bool bsyn_assign_ok(struct bsyn *st, int t) {
    return bsyn_command_pos(st, t) && !(st->pst & BP_CASEPAT);
}

static int bsyn_time_ok(struct bsyn *st) {
    if (st->env->posix) {
        size_t i = st->sidx;
        while (bsyn_blank(bsyn_at(st, i))) i++;
        if (bsyn_at(st, i) == '-') return 0;
    }
    switch (st->last) {
    case 0: case ';': case '\n':
        if (st->before == '|') return 0;
        /* fall through */
    case BT_AND_AND: case BT_OR_OR: case '&': case BT_WHILE: case BT_DO:
    case BT_UNTIL: case BT_IF: case BT_THEN: case BT_ELIF: case BT_ELSE:
    case '{': case '(': case ')': case BT_BANG: case BT_TIME:
    case BT_TIMEOPT: case BT_TIMEIGN:
        return 1;
    default:
        return 0;
    }
}

/* CHECK_FOR_RESERVED_WORD; -1 when TOK is not one here. */
static int bsyn_check_reserved(struct bsyn *st, const char *tok, bool dollar,
                               bool quoted) {
    if (dollar || quoted || !bsyn_reserved_ok(st, st->last)) return -1;
    for (int i = 0; bsyn_reserved[i].w; i++) {
        if (!bsyn_streq(tok, bsyn_reserved[i].w)) continue;
        int t = bsyn_reserved[i].t;
        if ((st->pst & BP_CASEPAT) && t != BT_ESAC) return -1;
        if (t == BT_TIME && !bsyn_time_ok(st)) return -1;
        if ((st->pst & BP_CASEPAT) && st->last == '|' && t == BT_ESAC) return -1;
        if ((st->pst & BP_CASEPAT) && st->last == '(' && t == BT_ESAC) return -1;
        if (t == BT_ESAC) {
            st->pst &= ~(BP_CASEPAT | BP_CASESTMT);
            st->esacs--;
        } else if (t == BT_CASE) {
            st->pst |= BP_CASESTMT;
        } else if (t == BT_COND_END) {
            st->pst &= ~(BP_CONDCMD | BP_CONDEXPR);
        } else if (t == BT_COND_START) {
            st->pst |= BP_CONDCMD;
        } else if (t == '{') {
            st->braces++;
        } else if (t == '}' && st->braces) {
            st->braces--;
        }
        return t;
    }
    return -1;
}

static int bsyn_special_tokens(struct bsyn *st, const char *t) {
    if (st->last == BT_WORD &&
        (st->before == BT_FOR || st->before == BT_CASE || st->before == BT_SELECT) &&
        bsyn_streq(t, "in")) {
        if (st->before == BT_CASE) {
            st->pst |= BP_CASEPAT;
            st->esacs++;
        }
        if (st->expect_in) st->expect_in--;
        return BT_IN;
    }
    if (st->expect_in && (st->last == BT_WORD || st->last == '\n') &&
        bsyn_streq(t, "in")) {
        if (st->pst & BP_CASESTMT) {
            st->pst |= BP_CASEPAT;
            st->esacs++;
        }
        st->expect_in--;
        return BT_IN;
    } else if (st->expect_in && (st->last == '\n' || st->last == ';') &&
               bsyn_streq(t, "do")) {
        st->expect_in--;
        return BT_DO;
    }
    if (st->last == BT_WORD &&
        (st->before == BT_FOR || st->before == BT_SELECT) && bsyn_streq(t, "do")) {
        if (st->expect_in) st->expect_in--;
        return BT_DO;
    }
    if (st->esacs && st->last == BT_IN && bsyn_streq(t, "esac")) {
        st->esacs--;
        st->pst &= ~BP_CASEPAT;
        return BT_ESAC;
    }
    if (st->pst & BP_ALLOWOPNBRC) {
        st->pst &= ~BP_ALLOWOPNBRC;
        if (bsyn_streq(t, "{")) {
            st->braces++;
            return '{';
        }
    }
    if (st->last == BT_ARITH_FOR_EXPRS && bsyn_streq(t, "do")) return BT_DO;
    if (st->last == BT_ARITH_FOR_EXPRS && bsyn_streq(t, "{")) {
        st->braces++;
        return '{';
    }
    if (st->braces && bsyn_reserved_ok(st, st->last) && bsyn_streq(t, "}")) {
        st->braces--;
        return '}';
    }
    if (st->last == BT_TIME && bsyn_streq(t, "-p")) return BT_TIMEOPT;
    if (st->last == BT_TIME && bsyn_streq(t, "--")) return BT_TIMEIGN;
    if (st->last == BT_TIMEOPT && bsyn_streq(t, "--")) return BT_TIMEIGN;
    if ((st->pst & BP_CONDEXPR) && bsyn_streq(t, "]]")) return BT_COND_END;
    return -1;
}

/* legal_identifier */
static bool bsyn_ident(const char *s, size_t n) {
    if (!n || !bsyn_var_start((unsigned char)s[0])) return false;
    for (size_t i = 1; i < n; i++)
        if (!bsyn_var_char((unsigned char)s[i])) return false;
    return true;
}

/* skip_matched_pair for a subscript: the index of the `]` that closes the
 * `[` at s[i], or of the terminating NUL. */
static size_t bsyn_skip_subscript(const char *s, size_t i) {
    int depth = 0;
    size_t n = bsyn_strlen(s);
    while (i < n) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) { i += 2; continue; }
        if (c == '\'') {
            i++;
            while (i < n && s[i] != '\'') i++;
            if (i < n) i++;
            continue;
        }
        if (c == '"') {
            i++;
            while (i < n && s[i] != '"') { if (s[i] == '\\' && i + 1 < n) i++; i++; }
            if (i < n) i++;
            continue;
        }
        if (c == '[') depth++;
        else if (c == ']') { if (--depth == 0) return i; }
        i++;
    }
    return n;
}

/* assignment(): the index of the `=` of NAME=, NAME+=, NAME[...]=, or 0. */
static size_t bsyn_assignment(const char *s, bool compassign) {
    size_t i = 0;
    unsigned char c = (unsigned char)s[0];
    if (compassign && c != '[') return 0;
    if (!compassign && !bsyn_var_start(c)) return 0;
    while ((c = (unsigned char)s[i]) != 0) {
        if (c == '=') return i;
        if (c == '[') {
            size_t ni = bsyn_skip_subscript(s, i);
            if (s[ni] != ']') return 0;
            ni++;
            if (s[ni] == '+' && s[ni + 1] == '=') return ni + 1;
            return s[ni] == '=' ? ni : 0;
        }
        if (c == '+' && s[i + 1] == '=') return i + 1;
        if (!bsyn_var_char(c)) return 0;
        i++;
    }
    return 0;
}

static bool bsyn_token_is_assignment(struct bsyn *st) {
    /* the token so far, with an `=` after it */
    struct bsyn_buf t = {0, 0, 0};
    bsyn_putn(st, &t, st->tok.p, st->tok.len);
    bsyn_putc(st, &t, '=');
    if (st->oom) { bsyn_buf_free(&t); return false; }
    size_t r = bsyn_assignment(t.p, (st->pst & BP_COMPASSIGN) != 0);
    bool ok = r > 0 && r == st->tok.len;
    bsyn_buf_free(&t);
    return ok;
}

/* valid_array_reference: NAME[SUB] with a non-empty subscript. */
static bool bsyn_array_ref(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i] != '[') i++;
    if (i == n || !bsyn_ident(s, i)) return false;
    size_t e = bsyn_skip_subscript(s, i);
    return e == n - 1 && e > i + 1;
}

/* ---- ANSI-C quoting, for $'...' and for a message's token --------------- */

static int bsyn_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ansicstr: decode the escapes of a $'...' body. */
static void bsyn_ansi_decode(struct bsyn *st, const char *s, size_t n, struct bsyn_buf *out) {
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (c != '\\' || i + 1 >= n) { bsyn_putc(st, out, c); continue; }
        c = (unsigned char)s[++i];
        switch (c) {
        case 'a': bsyn_putc(st, out, 7); break;
        case 'b': bsyn_putc(st, out, 8); break;
        case 'e': case 'E': bsyn_putc(st, out, 27); break;
        case 'f': bsyn_putc(st, out, 12); break;
        case 'n': bsyn_putc(st, out, '\n'); break;
        case 'r': bsyn_putc(st, out, '\r'); break;
        case 't': bsyn_putc(st, out, '\t'); break;
        case 'v': bsyn_putc(st, out, 11); break;
        case '\\': case '\'': case '"': case '?': bsyn_putc(st, out, c); break;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
            int v = c - '0', k = 1;
            while (k < 3 && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '7') {
                v = v * 8 + (s[++i] - '0');
                k++;
            }
            bsyn_putc(st, out, v & 0xff);
            break;
        }
        case 'x': {
            int v = 0, k = 0;
            while (k < 2 && i + 1 < n && bsyn_hexval((unsigned char)s[i + 1]) >= 0) {
                v = v * 16 + bsyn_hexval((unsigned char)s[++i]);
                k++;
            }
            if (!k) { bsyn_putc(st, out, '\\'); bsyn_putc(st, out, 'x'); }
            else bsyn_putc(st, out, v);
            break;
        }
        case 'c':
            if (i + 1 < n) { bsyn_putc(st, out, (unsigned char)s[++i] & 0x1f); break; }
            bsyn_putc(st, out, '\\');
            bsyn_putc(st, out, c);
            break;
        default:
            bsyn_putc(st, out, '\\');
            bsyn_putc(st, out, c);
            break;
        }
    }
}

/* sh_single_quote */
static void bsyn_single_quote(struct bsyn *st, const char *s, size_t n, struct bsyn_buf *out) {
    bsyn_putc(st, out, '\'');
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\'') bsyn_puts(st, out, "'\\''");
        else bsyn_putc(st, out, s[i]);
    }
    bsyn_putc(st, out, '\'');
}

static bool bsyn_isprint(int c) { return c >= 0x20 && c < 0x7f; }

/* ansic_shouldquote + ansic_quote, for the token in a message. */
static char *bsyn_msg_token(struct bsyn *st, char *t) {
    bool q = false;
    for (const char *s = t; *s; s++) if (!bsyn_isprint((unsigned char)*s)) { q = true; break; }
    if (!q) return t;
    struct bsyn_buf o = {0, 0, 0};
    bsyn_puts(st, &o, "$'");
    for (const char *s = t; *s; s++) {
        int c = (unsigned char)*s;
        int e = 0;
        switch (c) {
        case 27: e = 'E'; break;
        case 7: e = 'a'; break;
        case 11: e = 'v'; break;
        case 8: e = 'b'; break;
        case 12: e = 'f'; break;
        case '\n': e = 'n'; break;
        case '\r': e = 'r'; break;
        case '\t': e = 't'; break;
        case '\\': case '\'': e = c; break;
        }
        if (e) { bsyn_putc(st, &o, '\\'); bsyn_putc(st, &o, e); continue; }
        if (!bsyn_isprint(c)) {
            bsyn_putc(st, &o, '\\');
            bsyn_putc(st, &o, '0' + ((c >> 6) & 7));
            bsyn_putc(st, &o, '0' + ((c >> 3) & 7));
            bsyn_putc(st, &o, '0' + (c & 7));
            continue;
        }
        bsyn_putc(st, &o, c);
    }
    bsyn_putc(st, &o, '\'');
    BSYN_FREE(t);
    return o.p;
}

/* string_quote_removal, for a here-document's delimiter. */
static void bsyn_quote_removal(struct bsyn *st, const char *s, struct bsyn_buf *out) {
    size_t n = bsyn_strlen(s);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\') {
            if (i + 1 < n) bsyn_putc(st, out, s[++i]);
            continue;
        }
        if (c == '\'') {
            i++;
            while (i < n && s[i] != '\'') bsyn_putc(st, out, s[i++]);
            continue;
        }
        if (c == '"') {
            i++;
            while (i < n && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < n &&
                    (s[i + 1] == '\\' || s[i + 1] == '$' || s[i + 1] == '`' ||
                     s[i + 1] == '"' || s[i + 1] == '\n'))
                    i++;
                bsyn_putc(st, out, s[i++]);
            }
            continue;
        }
        bsyn_putc(st, out, c);
    }
    if (!out->p) (void)bsyn_room(st, out, 0);
}

/* ---- error reporting ---------------------------------------------------- */

static void bsyn_lexfail(struct bsyn *st) {
    /* bash's lexer returned -1: no message from the grammar (bison's
     * YYerror), the lexer has said what it had to. */
    st->failed = true;
    st->lexfail = true;
}

/* error_token_from_token */
static char *bsyn_tok_text(struct bsyn *st, int t) {
    for (int i = 0; bsyn_reserved[i].w; i++)
        if (bsyn_reserved[i].t == t) return bsyn_strdup(st, bsyn_reserved[i].w, bsyn_strlen(bsyn_reserved[i].w));
    for (int i = 0; bsyn_others[i].w; i++)
        if (bsyn_others[i].t == t) return bsyn_strdup(st, bsyn_others[i].w, bsyn_strlen(bsyn_others[i].w));
    switch (st->cur) {
    case BT_WORD: case BT_ASSIGNMENT_WORD:
    case BT_ARITH_CMD: case BT_ARITH_FOR_EXPRS:
        if (st->wtext.p) return bsyn_strdup(st, st->wtext.p, st->wtext.len);
        return 0;
    case BT_NUMBER: {
        struct bsyn_buf b = {0, 0, 0};
        long v = st->number;
        if (v < 0) { bsyn_putc(st, &b, '-'); v = -v; }
        bsyn_put_ulong(st, &b, (unsigned long)v);
        return b.p;
    }
    }
    return 0;
}

/* error_token_from_text */
static char *bsyn_text_token(struct bsyn *st) {
    const char *t = st->sl;
    long i = (long)st->sidx;
    long tend = 0;
    if (i && bsyn_at(st, (size_t)i) == 0) i--;
    while (i && (bsyn_blank(bsyn_at(st, (size_t)i)) || bsyn_at(st, (size_t)i) == '\n')) i--;
    if (i) tend = i + 1;
    while (i) {
        int c = bsyn_at(st, (size_t)i);
        if (c == ' ' || c == '\n' || c == '\t' || c == ';' || c == '|' || c == '&') break;
        i--;
    }
    while (i != tend && (bsyn_blank(bsyn_at(st, (size_t)i)) || bsyn_at(st, (size_t)i) == '\n')) i++;
    if (tend || (i == 0 && tend == 0)) {
        if (tend) {
            struct bsyn_buf b = {0, 0, 0};
            for (long k = i; k < tend; k++) bsyn_putc(st, &b, bsyn_at(st, (size_t)k));
            return b.p;
        }
        char one[2] = { (char)bsyn_at(st, (size_t)i), 0 };
        (void)t;
        return bsyn_strdup(st, one, one[0] ? 1 : 0);
    }
    return 0;
}

static void bsyn_offending_line(struct bsyn *st) {
    if (st->env->interactive) return;
    struct bsyn_buf b = {0, 0, 0};
    bsyn_putc(st, &b, '`');
    size_t n = st->slen;
    while (n && st->sl[n - 1] == '\n') n--;
    /* bash's shell_ungetc writes into the line it reads from, so a blank
     * given back at the end of an alias shows in the echo */
    for (size_t i = 0; i < n; i++)
        bsyn_putc(st, &b, (int)i == st->ovr_idx ? st->ovr_c : (unsigned char)st->sl[i]);
    bsyn_putc(st, &b, '\'');
    if (b.p) bsyn_msg(st, st->lineno, BSYN_MSG_PARSER, b.p, 0, 0);
    bsyn_buf_free(&b);
}

static void bsyn_reset_parser(struct bsyn *st);

/* yyerror -> report_syntax_error (NULL), then reset_parser. */
static void bsyn_syntax_error(struct bsyn *st) {
    if (st->failed) return;
    char *m;
    if (st->cur != 0 && !st->eof && (m = bsyn_tok_text(st, st->cur)) != 0) {
        m = bsyn_msg_token(st, m);
        if (m) {
            bsyn_msg(st, st->lineno, BSYN_MSG_PARSER,
                     "syntax error near unexpected token `", m, "'");
            BSYN_FREE(m);
        }
        bsyn_offending_line(st);
    } else if (st->have_line && !st->seof && (st->slen || st->sterm)) {
        m = bsyn_text_token(st);
        if (m) {
            bsyn_msg(st, st->lineno, BSYN_MSG_PARSER, "syntax error near `", m, "'");
            BSYN_FREE(m);
        }
        bsyn_offending_line(st);
    } else if (st->eof && st->eof_tok && st->cur != st->eof_tok) {
        char c[2] = { (char)st->eof_tok, 0 };
        bsyn_msg(st, st->lineno, BSYN_MSG_PARSER,
                 "unexpected EOF while looking for matching `", c, "'");
    } else {
        bsyn_msg(st, st->lineno, BSYN_MSG_PARSER,
                 st->eof ? "syntax error: unexpected end of file" : "syntax error", 0, 0);
    }
    bsyn_status_set(st, 2);
    st->failed = true;
    bsyn_reset_parser(st);
}

/* reset_parser: the rest of the line is thrown away. */
static void bsyn_reset_parser(struct bsyn *st) {
    st->ddepth = 0;
    st->braces = 0;
    st->pst = 0;
    while (st->push) bsyn_pop_string(st);
    st->sl = "";
    st->slen = 0;
    st->sidx = 0;
    st->sterm = 0;
    st->seof = false;
    st->have_line = false;
    st->ovr_idx = -1;
    st->eol_unget = 0;
    for (int i = 0; i < st->nhd; i++) if (st->hd[i].delim) BSYN_FREE(st->hd[i].delim);
    st->nhd = 0;
    st->esacs = st->expect_in = 0;
    st->cur = '\n';
    st->last = '\n';
    st->to_read = '\n';
}

/* ---- here-documents ----------------------------------------------------- */

static void bsyn_gather_heredocs(struct bsyn *st) {
    for (int k = 0; k < st->nhd; k++) {
        struct bsyn_heredoc *h = &st->hd[k];
        unsigned long at = st->lineno;
        size_t dl = bsyn_strlen(h->delim);
        bool found = false;
        struct bsyn_buf line = {0, 0, 0};
        for (;;) {
            /* read_a_line: raw input, a backslash-newline joins lines when
             * the delimiter was not quoted */
            line.len = 0;
            if (line.p) line.p[0] = '\0';
            if (st->next >= st->len) break;
            bool pass = false;
            while (st->next < st->len) {
                int c = (unsigned char)st->buf[st->next++];
                if (c == 0) continue;
                if (pass) { bsyn_putc(st, &line, c); pass = false; if (c == '\n') break; continue; }
                if (c == '\\' && !h->quoted) {
                    if (st->next < st->len && st->buf[st->next] == '\n') {
                        st->next++;
                        st->lineno++;
                        continue;
                    }
                    pass = true;
                    bsyn_putc(st, &line, c);
                    continue;
                }
                bsyn_putc(st, &line, c);
                if (c == '\n') break;
            }
            if (st->oom) break;
            if (!line.len || line.p[line.len - 1] != '\n') bsyn_putc(st, &line, '\n');
            /* CRLF, as the reader */
            if (line.len >= 2 && line.p[line.len - 2] == '\r') {
                line.p[line.len - 2] = '\n';
                line.p[--line.len] = '\0';
            }
            st->lineno++;
            const char *l = line.p;
            if (h->strip && *l) {
                if (line.len == dl + 1 && memcmp(l, h->delim, dl) == 0) { found = true; break; }
                while (*l == '\t') l++;
            }
            if (*l == 0) continue;
            size_t ll = bsyn_strlen(l);
            if (ll == dl + 1 && memcmp(l, h->delim, dl) == 0) { found = true; break; }
            /* inside $( ): "EOF)" ends the document and gives back ")" */
            if ((st->pst & BP_EOFTOKEN) && st->eof_tok && ll > dl &&
                memcmp(l, h->delim, dl) == 0) {
                bool has = false;
                for (const char *q = l + dl; *q; q++) if (*q == st->eof_tok) { has = true; break; }
                if (has) {
                    bsyn_push_string(st, l + dl, ll - dl, true, false, 0);
                    found = true;
                    break;
                }
            }
        }
        bsyn_buf_free(&line);
        if (!found && !st->oom) {
            struct bsyn_buf w = {0, 0, 0};
            bsyn_puts(st, &w, "here-document at line ");
            bsyn_put_ulong(st, &w, at);
            bsyn_puts(st, &w, " delimited by end-of-file (wanted `");
            bsyn_puts(st, &w, h->delim);
            bsyn_putc(st, &w, '\'');
            bsyn_puts(st, &w, ")");
            if (w.p) bsyn_msg(st, st->lineno, BSYN_MSG_WARNING, w.p, 0, 0);
            bsyn_buf_free(&w);
        }
        BSYN_FREE(h->delim);
        h->delim = 0;
    }
    st->nhd = 0;
}

static void bsyn_push_heredoc(struct bsyn *st, bool strip) {
    if (st->nhd >= BSYN_HEREDOC_MAX) {
        bsyn_msg(st, st->lineno, BSYN_MSG_PARSER, "maximum here-document count exceeded", 0, 0);
        bsyn_status_set(st, 2);
        st->failed = true;
        st->fatal = true;
        return;
    }
    struct bsyn_buf d = {0, 0, 0};
    if (st->wquoted) bsyn_quote_removal(st, st->wtext.p ? st->wtext.p : "", &d);
    else bsyn_puts(st, &d, st->wtext.p ? st->wtext.p : "");
    if (st->oom) { bsyn_buf_free(&d); return; }
    st->hd[st->nhd].delim = d.p;
    st->hd[st->nhd].quoted = st->wquoted;
    st->hd[st->nhd].strip = strip;
    st->nhd++;
}

/* ---- the lexer ---------------------------------------------------------- */

static int bsyn_read_token(struct bsyn *st);
static bool bsyn_pmp(struct bsyn *st, int qc, int open, int close, int flags, struct bsyn_buf *out);
static bool bsyn_comsub(struct bsyn *st, int qc, int flags, struct bsyn_buf *out);
static void bsyn_cond_or(struct bsyn *st);

#define BSYN_DEPTH_IN(st)  do { if (++(st)->depth > (st)->env->max_depth) { bsyn_nomem(st); } } while (0)
#define BSYN_DEPTH_OUT(st) ((st)->depth--)

/* parse_matched_pair: read up to the CLOSE that matches, appending the text
 * (CLOSE included) to OUT. */
static bool bsyn_pmp(struct bsyn *st, int qc, int open, int close, int flags,
                     struct bsyn_buf *out) {
    enum { DB_PARAM = 1, DB_OP, DB_WORD, DB_QUOTE, DB_QUOTE2 };
    int dolbrace = (flags & BM_DOLBRACE) ? DB_PARAM : 0;
    int count = 1;
    bool ckcomment = false, incomment = false, passnext = false, wasdol = false, gtlt = false;
    int rflags;
    int ch = BSYN_EOFC;
    struct bsyn_buf ret = {0, 0, 0};
    unsigned long start = st->lineno;
    bool ok = false;

    BSYN_DEPTH_IN(st);
    if (st->failed) goto out;
    if ((flags & BM_COMMAND) && qc != '`' && qc != '\'' && qc != '"' && !(flags & BM_DQUOTE))
        ckcomment = true;
    rflags = (qc == '"') ? BM_DQUOTE : (flags & BM_DQUOTE);
    (void)bsyn_room(st, &ret, 0);
    while (count) {
        ch = bsyn_getc(st, qc != '\'' && !passnext);
        if (st->oom) goto out;
        if (ch == BSYN_EOFC) {
            char c[2] = { (char)close, 0 };
            bsyn_msg(st, start, BSYN_MSG_PARSER,
                     "unexpected EOF while looking for matching `", c, "'");
            st->eof = true;
            goto out;
        }
        if (incomment) {
            bsyn_putc(st, &ret, ch);
            if (ch == '\n') incomment = false;
            continue;
        } else if (ckcomment && ch == '#' &&
                   (ret.len == 0 || ret.p[ret.len - 1] == '\n' || bsyn_blank((unsigned char)ret.p[ret.len - 1]))) {
            incomment = true;
        }
        if (passnext) {
            passnext = false;
            if (qc != '\'' && ch == '\n') {
                if (ret.len > 0) ret.p[--ret.len] = '\0';
                continue;
            }
            bsyn_putc(st, &ret, ch);
            continue;
        } else if (ch == close) {
            count--;
        } else if (open != close && wasdol && open == '{' && ch == open) {
            count++;
        } else if (!(flags & BM_FIRSTCLOSE) && ch == open) {
            count++;
        }
        bsyn_putc(st, &ret, ch);
        if (count == 0) break;
        if (open == '\'') {
            if ((flags & BM_ALLOWESC) && ch == '\\') passnext = true;
            continue;
        }
        if (ch == '\\') passnext = true;
        if (flags & BM_DOLBRACE) {
            const char *ops = "#%^,~:-=?+/";
            bool isop = false;
            for (const char *o = ops; *o; o++) if (*o == ch) { isop = true; break; }
            if (dolbrace == DB_PARAM && ch == '%' && ret.len > 1) dolbrace = DB_QUOTE;
            else if (dolbrace == DB_PARAM && ch == '#' && ret.len > 1) dolbrace = DB_QUOTE;
            else if (dolbrace == DB_PARAM && ch == '/' && ret.len > 1) dolbrace = DB_QUOTE2;
            else if (dolbrace == DB_PARAM && ch == '^' && ret.len > 1) dolbrace = DB_QUOTE;
            else if (dolbrace == DB_PARAM && ch == ',' && ret.len > 1) dolbrace = DB_QUOTE;
            else if (dolbrace == DB_PARAM && isop) dolbrace = DB_OP;
            else if (dolbrace == DB_OP && !isop) dolbrace = DB_WORD;
        }
        if (st->env->posix && dolbrace != DB_QUOTE && dolbrace != DB_QUOTE2 &&
            (flags & BM_DQUOTE) && (flags & BM_DOLBRACE) && ch == '\'')
            continue;
        bool dollar_word = false;
        if (open != close) {
            if (bsyn_quotec(ch)) {
                struct bsyn_buf nest = {0, 0, 0};
                bsyn_dpush(st, ch);
                bool nok;
                if (wasdol && ch == '\'')
                    nok = bsyn_pmp(st, ch, ch, ch, BM_ALLOWESC | rflags, &nest);
                else
                    nok = bsyn_pmp(st, ch, ch, ch, rflags, &nest);
                bsyn_dpop(st);
                if (!nok) { bsyn_buf_free(&nest); goto out; }
                if (wasdol && ch == '\'') {
                    /* $'...' inside a grouping construct is translated here
                     * (extquote is on by default) */
                    struct bsyn_buf tr = {0, 0, 0};
                    bsyn_ansi_decode(st, nest.p, nest.len ? nest.len - 1 : 0, &tr);
                    struct bsyn_buf q = {0, 0, 0};
                    if ((rflags & BM_DQUOTE) && (dolbrace == DB_QUOTE2 || dolbrace == DB_QUOTE) &&
                        (flags & BM_DOLBRACE))
                        bsyn_single_quote(st, tr.p ? tr.p : "", tr.len, &q);
                    else if (!(rflags & BM_DQUOTE))
                        bsyn_single_quote(st, tr.p ? tr.p : "", tr.len, &q);
                    else
                        bsyn_putn(st, &q, tr.p ? tr.p : "", tr.len);
                    bsyn_buf_free(&tr);
                    bsyn_buf_free(&nest);
                    nest = q;
                    if (ret.len >= 2) { ret.len -= 2; ret.p[ret.len] = '\0'; }
                } else if (wasdol && ch == '"') {
                    /* $"..." -- no translation catalogue: "..." as written */
                    struct bsyn_buf q = {0, 0, 0};
                    bsyn_putc(st, &q, '"');
                    bsyn_putn(st, &q, nest.p ? nest.p : "", nest.len ? nest.len - 1 : 0);
                    bsyn_putc(st, &q, '"');
                    bsyn_buf_free(&nest);
                    nest = q;
                    if (ret.len >= 2) { ret.len -= 2; ret.p[ret.len] = '\0'; }
                }
                bsyn_putn(st, &ret, nest.p ? nest.p : "", nest.len);
                bsyn_buf_free(&nest);
            } else if ((flags & (BM_ARRAYSUB | BM_DOLBRACE)) && wasdol &&
                       (ch == '(' || ch == '{' || ch == '[')) {
                dollar_word = true;
            } else if ((flags & BM_ARITH) && wasdol && ch == '(') {
                dollar_word = true;
            } else if ((flags & (BM_ARRAYSUB | BM_DOLBRACE)) && gtlt && ch == '(') {
                dollar_word = true;
            }
        } else if (open == '"' && ch == '`') {
            struct bsyn_buf nest = {0, 0, 0};
            bool nok = bsyn_pmp(st, 0, '`', '`', rflags, &nest);
            if (!nok) { bsyn_buf_free(&nest); goto out; }
            bsyn_putn(st, &ret, nest.p ? nest.p : "", nest.len);
            bsyn_buf_free(&nest);
        } else if (open != '`' && wasdol && (ch == '(' || ch == '{' || ch == '[')) {
            dollar_word = true;
        }
        if (dollar_word) {
            struct bsyn_buf nest = {0, 0, 0};
            bool nok;
            if (open == ch) count--;
            if (ch == '(')
                nok = bsyn_comsub(st, 0, (rflags | BM_COMMAND) & ~BM_DQUOTE, &nest);
            else if (ch == '{')
                nok = bsyn_pmp(st, 0, '{', '}', BM_FIRSTCLOSE | BM_DOLBRACE | rflags, &nest);
            else
                nok = bsyn_pmp(st, 0, '[', ']', rflags | BM_ARITH, &nest);
            if (!nok) { bsyn_buf_free(&nest); goto out; }
            bsyn_putn(st, &ret, nest.p ? nest.p : "", nest.len);
            bsyn_buf_free(&nest);
        }
        if ((ch == '<' || ch == '>') && !gtlt) gtlt = true;
        else gtlt = false;
        if (ch == '$' && !wasdol) wasdol = true;
        else wasdol = false;
    }
    if (st->oom) goto out;
    bsyn_putn(st, out, ret.p ? ret.p : "", ret.len);
    ok = !st->oom;
out:
    bsyn_buf_free(&ret);
    BSYN_DEPTH_OUT(st);
    return ok;
}

static void bsyn_list(struct bsyn *st);
static int bsyn_la(struct bsyn *st);
static void bsyn_shift(struct bsyn *st);
static void bsyn_recover(struct bsyn *st);

/* parse_comsub: the text of a $( ) is a command list, parsed now, with
 * the whole grammar. */
static bool bsyn_comsub(struct bsyn *st, int qc, int flags, struct bsyn_buf *out) {
    (void)flags;
    int pk = bsyn_getc(st, 1);
    bsyn_ungetc(st, pk);
    if (pk == '(') return bsyn_pmp(st, qc, '(', ')', BM_ARITH, out);

    BSYN_DEPTH_IN(st);
    if (st->failed) { BSYN_DEPTH_OUT(st); return false; }
    /* save_parser_state */
    unsigned s_pst = st->pst;
    int s_cur = st->cur, s_last = st->last, s_before = st->before, s_two = st->two_ago;
    int s_toread = st->to_read;
    int s_esacs = st->esacs, s_expin = st->expect_in;
    int s_eoftok = st->eof_tok;
    bool s_exp = st->expand_aliases;
    bool s_la = st->have_la;
    struct bsyn_buf s_tok = st->tok;
    struct bsyn_buf s_wtext = st->wtext;
    bool s_wq = st->wquoted;
    long s_num = st->number;
    int s_nhd = st->nhd;
    struct bsyn_heredoc s_hd[BSYN_HEREDOC_MAX];
    for (int i = 0; i < s_nhd; i++) s_hd[i] = st->hd[i];
    struct bsyn_buf *s_cap = st->cap;
    struct bsyn_buf captured = {0, 0, 0};

    st->tok.p = 0; st->tok.len = st->tok.cap = 0;
    st->wtext.p = 0; st->wtext.len = st->wtext.cap = 0;
    st->pst &= ~(BP_REGEXP | BP_EXTPAT | BP_CONDCMD | BP_CONDEXPR | BP_COMPASSIGN |
                 BP_CASEPAT | BP_ALEXPNEXT | BP_SUBSHELL | BP_REDIRLIST);
    st->pst |= BP_CMDSUBST | BP_EOFTOKEN | BP_NOEXPAND;
    st->eof_tok = ')';
    st->nhd = 0;
    st->esacs = st->expect_in = 0;
    if (st->expand_aliases) st->expand_aliases = st->env->posix;
    st->cur = '\n';
    st->to_read = BT_DOLPAREN;
    st->have_la = false;
    st->cap = &captured;

    /* comsub: DOLPAREN compound_list ')' | DOLPAREN newline_list ')' */
    (void)bsyn_la(st);
    bsyn_shift(st);
    while (!st->failed && bsyn_la(st) == '\n') bsyn_shift(st);
    if (!st->failed && bsyn_la(st) != ')') {
        bsyn_list(st);
    }
    if (!st->failed) {
        if (bsyn_la(st) != ')') bsyn_syntax_error(st);
        else bsyn_shift(st);
    }
    bool inner_failed = st->failed && !st->oom && !st->discard && !st->fatal;
    if (inner_failed) {
        /* the inner parse's own recovery: after a grammar error the
         * newline reset_parser left behind ends it; after a lexer error it
         * reads on to a newline or the end of input */
        if (st->lexfail) bsyn_recover(st);
        else st->to_read = 0;
    }
    if (!st->oom && !st->discard && !st->fatal && st->nhd > 0) {
        /* parse_comsub, straight after yyparse returns -- error or not */
        struct bsyn_buf w = {0, 0, 0};
        bsyn_puts(st, &w, "command substitution: ");
        bsyn_put_ulong(st, &w, (unsigned long)st->nhd);
        bsyn_puts(st, &w, st->nhd == 1 ? " unterminated here-document" : " unterminated here-documents");
        if (w.p) bsyn_msg(st, st->lineno, BSYN_MSG_WARNING, w.p, 0, 0);
        bsyn_buf_free(&w);
        bool f = st->failed;
        st->failed = false;
        bsyn_gather_heredocs(st);
        st->failed = f || st->oom;
    }
    bool ok = !st->failed;
    if (inner_failed) {
        if (st->oom) {
            /* give up */
        } else if (st->eof) {
            /* yyparse met the end of input: the caller sees a lexer error,
             * and its own recovery reads the end of input again */
            st->failed = false;
            st->lexfail = false;
        } else {
            /* any other error inside $( ) ends the shell input */
            bsyn_status_ifzero(st, 1);
            st->fatal = true;
        }
    }
    st->cap = s_cap;
    bsyn_buf_free(&st->tok);
    bsyn_buf_free(&st->wtext);
    st->tok = s_tok;
    st->wtext = s_wtext;
    st->wquoted = s_wq;
    st->number = s_num;
    if (ok || (!st->failed && !st->oom)) {
        /* restore_parser_state (bash restores on success; on the end-of-
         * input failure only the eof token and aliases) */
        if (ok) {
            st->pst = s_pst;
            st->cur = s_cur; st->last = s_last; st->before = s_before; st->two_ago = s_two;
            st->to_read = s_toread;
            st->esacs = s_esacs; st->expect_in = s_expin;
            st->have_la = s_la;
            for (int i = 0; i < st->nhd; i++) if (st->hd[i].delim) BSYN_FREE(st->hd[i].delim);
            st->nhd = s_nhd;
            for (int i = 0; i < s_nhd; i++) st->hd[i] = s_hd[i];
            s_nhd = 0;
        }
        st->eof_tok = s_eoftok;
        st->expand_aliases = s_exp;
    }
    for (int i = 0; i < s_nhd; i++) if (s_hd[i].delim) BSYN_FREE(s_hd[i].delim);
    if (s_cap && captured.p) bsyn_putn(st, s_cap, captured.p, captured.len);
    if (ok) {
        if (captured.p && captured.p[0] == '(') bsyn_putc(st, out, ' ');
        bsyn_putn(st, out, captured.p ? captured.p : ")", captured.p ? captured.len : 1);
        ok = !st->oom;
    }
    bsyn_buf_free(&captured);
    BSYN_DEPTH_OUT(st);
    return ok;
}

/* parse_arith_cmd: 1 = (( expr )), 0 = two subshells, -1 = error */
static int bsyn_arith_cmd(struct bsyn *st, struct bsyn_buf *text) {
    struct bsyn_buf t = {0, 0, 0};
    if (!bsyn_pmp(st, 0, '(', ')', BM_ARITH, &t)) { bsyn_buf_free(&t); return -1; }
    int c = bsyn_getc(st, 0);
    text->len = 0;
    size_t inner = t.len ? t.len - 1 : 0;
    if (c != ')') {
        bsyn_putc(st, text, '(');
        bsyn_putn(st, text, t.p ? t.p : "", inner);
        bsyn_putc(st, text, ')');
        bsyn_putc(st, text, c == BSYN_EOFC ? 0xff : c);
        bsyn_buf_free(&t);
        return 0;
    }
    bsyn_putn(st, text, t.p ? t.p : "", inner);
    bsyn_buf_free(&t);
    return 1;
}

static int bsyn_parse_dparen(struct bsyn *st, int c) {
    struct bsyn_buf text = {0, 0, 0};
    int r;
    if (st->last == BT_FOR) {
        r = bsyn_arith_cmd(st, &text);
        if (r == 1) {
            bsyn_buf_free(&st->wtext);
            st->wtext = text;
            return BT_ARITH_FOR_EXPRS;
        }
        bsyn_buf_free(&text);
        return BT_ERROR;
    }
    if (bsyn_reserved_ok(st, st->last)) {
        r = bsyn_arith_cmd(st, &text);
        if (r == 1) {
            bsyn_buf_free(&st->wtext);
            st->wtext = text;
            return BT_ARITH_CMD;
        } else if (r == 0) {
            bsyn_push_string(st, text.p ? text.p : "", text.len, true, false, 0);
            bsyn_buf_free(&text);
            if (!(st->pst & BP_CASEPAT)) st->pst |= BP_SUBSHELL;
            return c;
        }
        bsyn_buf_free(&text);
        return BT_ERROR;
    }
    return -2;
}

/* parse_compound_assignment: NAME=( words ). false: the error is reported
 * and the line is to be discarded. */
static bool bsyn_compound_assignment(struct bsyn *st, struct bsyn_buf *out) {
    unsigned long orig = st->lineno;
    unsigned s_pst = st->pst;
    int s_cur = st->cur, s_last = st->last, s_before = st->before, s_two = st->two_ago;
    int s_esacs = st->esacs, s_expin = st->expect_in;
    struct bsyn_buf s_tok = st->tok;
    bool assignok = (st->pst & BP_ASSIGNOK) != 0;
    bool ok = true;

    BSYN_DEPTH_IN(st);
    st->tok.p = 0; st->tok.len = st->tok.cap = 0;
    st->last = BT_WORD;
    st->pst &= ~(BP_NOEXPAND | BP_CONDCMD | BP_CONDEXPR | BP_REGEXP | BP_EXTPAT);
    st->pst |= BP_COMPASSIGN;
    st->esacs = st->expect_in = 0;
    bool first = true;
    bool passthru = false;
    while (!st->oom) {
        int tok = bsyn_read_token(st);
        if (st->oom) break;
        if (st->failed) { passthru = true; ok = false; break; }
        if (tok == ')') break;
        if (tok == '\n') continue;
        if (tok != BT_WORD && tok != BT_ASSIGNMENT_WORD) {
            st->cur = tok;
            if (tok == BT_EOF)
                bsyn_msg(st, orig, BSYN_MSG_PARSER,
                         "unexpected EOF while looking for matching `)'", 0, 0);
            else
                bsyn_syntax_error(st);
            ok = false;
            break;
        }
        if (!first) bsyn_putc(st, out, ' ');
        bsyn_putn(st, out, st->wtext.p ? st->wtext.p : "", st->wtext.len);
        first = false;
    }
    bsyn_buf_free(&st->tok);
    st->tok = s_tok;
    if (!st->oom) {
        /* restore_parser_state */
        st->pst = s_pst;
        st->cur = s_cur; st->last = s_last; st->before = s_before; st->two_ago = s_two;
        st->esacs = s_esacs; st->expect_in = s_expin;
    }
    if (!ok && !st->oom && !passthru) {
        /* jump_to_top_level (DISCARD): $? 1, the rest of the line is gone
         * (posix mode, non-interactive: the input ends) */
        if (!st->failed) bsyn_reset_parser(st);
        st->failed = true;
        bsyn_status_set(st, 1);
        if (st->env->posix && !st->env->interactive) st->fatal = true;
        else st->discard = true;
    }
    if (ok && assignok) st->pst |= BP_ASSIGNOK;
    BSYN_DEPTH_OUT(st);
    return ok && !st->oom;
}

/* read_token_word */
static int bsyn_read_word(struct bsyn *st, int c) {
    bool all_digit = bsyn_digit(c), dollar = false, quoted = false;
    bool passnext = false, compassign = false;
    struct bsyn_buf *t = &st->tok;
    int cd, peek;
    t->len = 0;
    (void)bsyn_room(st, t, 0);
    if (st->oom) return BT_ERROR;

    for (;;) {
        if (st->oom) return BT_ERROR;
        if (c == BSYN_EOFC) goto got_token;
        if (passnext) {
            passnext = false;
            goto got_char;
        }
        cd = bsyn_curdelim(st);
        if (c == '\\') {
            if (st->pst & BP_NOEXPAND) {
                passnext = true;
                quoted = true;
                goto got_char;
            }
            peek = bsyn_getc(st, 0);
            if (peek == '\n') {
                c = '\n';
                goto next_char;
            }
            bsyn_ungetc(st, peek);
            if (cd == 0 || cd == '`' ||
                (cd == '"' && peek >= 0 &&
                 (peek == '\\' || peek == '`' || peek == '$' || peek == '"' || peek == '\n')))
                passnext = true;
            quoted = true;
            goto got_char;
        }
        if (bsyn_quotec(c)) {
            struct bsyn_buf nest = {0, 0, 0};
            bsyn_dpush(st, c);
            bool ok = bsyn_pmp(st, c, c, c, c == '`' ? BM_COMMAND : 0, &nest);
            bsyn_dpop(st);
            if (!ok) { bsyn_buf_free(&nest); return BT_ERROR; }
            bsyn_putc(st, t, c);
            bsyn_putn(st, t, nest.p ? nest.p : "", nest.len);
            all_digit = false;
            if (c != '`') quoted = true;
            if (c == '"' && nest.p)
                for (size_t i = 0; i < nest.len; i++) if (nest.p[i] == '$') { dollar = true; break; }
            bsyn_buf_free(&nest);
            goto next_char;
        }
        if ((st->pst & BP_REGEXP) && (c == '(' || c == '|')) {
            if (c == '|') goto got_char;
            struct bsyn_buf nest = {0, 0, 0};
            bsyn_dpush(st, c);
            bool ok = bsyn_pmp(st, cd, '(', ')', 0, &nest);
            bsyn_dpop(st);
            if (!ok) { bsyn_buf_free(&nest); return BT_ERROR; }
            bsyn_putc(st, t, c);
            bsyn_putn(st, t, nest.p ? nest.p : "", nest.len);
            bsyn_buf_free(&nest);
            dollar = all_digit = false;
            goto next_char;
        }
        if (st->extglob && (c == '@' || c == '*' || c == '+' || c == '?' || c == '!')) {
            peek = bsyn_getc(st, 1);
            if (peek == '(') {
                struct bsyn_buf nest = {0, 0, 0};
                bsyn_dpush(st, peek);
                bool ok = bsyn_pmp(st, cd, '(', ')', 0, &nest);
                bsyn_dpop(st);
                if (!ok) { bsyn_buf_free(&nest); return BT_ERROR; }
                bsyn_putc(st, t, c);
                bsyn_putc(st, t, peek);
                bsyn_putn(st, t, nest.p ? nest.p : "", nest.len);
                bsyn_buf_free(&nest);
                dollar = all_digit = false;
                goto next_char;
            }
            bsyn_ungetc(st, peek);
        }
        if (c == '$' || c == '<' || c == '>') {
            peek = bsyn_getc(st, 1);
            if (peek == '(' || ((peek == '{' || peek == '[') && c == '$')) {
                struct bsyn_buf nest = {0, 0, 0};
                bool ok;
                if (peek == '{') {
                    ok = bsyn_pmp(st, cd, '{', '}', BM_FIRSTCLOSE | BM_DOLBRACE, &nest);
                } else if (peek == '(') {
                    bsyn_dpush(st, peek);
                    ok = bsyn_comsub(st, cd, BM_COMMAND, &nest);
                    bsyn_dpop(st);
                } else {
                    ok = bsyn_pmp(st, cd, '[', ']', BM_ARITH, &nest);
                }
                if (!ok) { bsyn_buf_free(&nest); return BT_ERROR; }
                bsyn_putc(st, t, c);
                bsyn_putc(st, t, peek);
                bsyn_putn(st, t, nest.p ? nest.p : "", nest.len);
                bsyn_buf_free(&nest);
                dollar = true;
                all_digit = false;
                goto next_char;
            } else if (c == '$' && (peek == '\'' || peek == '"')) {
                struct bsyn_buf nest = {0, 0, 0};
                bsyn_dpush(st, peek);
                bool ok = bsyn_pmp(st, peek, peek, peek, peek == '\'' ? BM_ALLOWESC : 0, &nest);
                bsyn_dpop(st);
                if (!ok) { bsyn_buf_free(&nest); return BT_ERROR; }
                if (peek == '\'') {
                    struct bsyn_buf tr = {0, 0, 0};
                    bsyn_ansi_decode(st, nest.p ? nest.p : "", nest.len ? nest.len - 1 : 0, &tr);
                    bsyn_single_quote(st, tr.p ? tr.p : "", tr.len, t);
                    bsyn_buf_free(&tr);
                } else {
                    bsyn_putc(st, t, '"');
                    bsyn_putn(st, t, nest.p ? nest.p : "", nest.len ? nest.len - 1 : 0);
                    bsyn_putc(st, t, '"');
                }
                bsyn_buf_free(&nest);
                quoted = true;
                all_digit = false;
                goto next_char;
            } else if (c == '$' && peek == '$') {
                bsyn_putc(st, t, '$');
                bsyn_putc(st, t, '$');
                dollar = true;
                all_digit = false;
                goto next_char;
            }
            bsyn_ungetc(st, peek);
        } else if (c == '[' &&
                   ((t->len > 0 && bsyn_assign_ok(st, st->last) && bsyn_ident(t->p, t->len)) ||
                    (t->len == 0 && (st->pst & BP_COMPASSIGN)))) {
            struct bsyn_buf nest = {0, 0, 0};
            bool ok = bsyn_pmp(st, cd, '[', ']', BM_ARRAYSUB, &nest);
            if (!ok) { bsyn_buf_free(&nest); return BT_ERROR; }
            bsyn_putc(st, t, c);
            bsyn_putn(st, t, nest.p ? nest.p : "", nest.len);
            bsyn_buf_free(&nest);
            all_digit = false;
            goto next_char;
        } else if (c == '=' && t->len > 0 &&
                   (bsyn_assign_ok(st, st->last) || (st->pst & BP_ASSIGNOK)) &&
                   bsyn_token_is_assignment(st)) {
            peek = bsyn_getc(st, 1);
            if (peek == '(') {
                struct bsyn_buf words = {0, 0, 0};
                bool ok = bsyn_compound_assignment(st, &words);
                if (!ok) { bsyn_buf_free(&words); return BT_ERROR; }
                bsyn_putc(st, t, '=');
                bsyn_putc(st, t, '(');
                bsyn_putn(st, t, words.p ? words.p : "", words.len);
                bsyn_putc(st, t, ')');
                bsyn_buf_free(&words);
                all_digit = false;
                compassign = true;
                goto next_char;
            }
            bsyn_ungetc(st, peek);
        }
        if (bsyn_break(c)) {
            bsyn_ungetc(st, c);
            goto got_token;
        }
got_char:
        bsyn_putc(st, t, c);
        all_digit = all_digit && bsyn_digit(c);
        if (c == '$') dollar = true;
next_char:
        cd = bsyn_curdelim(st);
        c = bsyn_getc(st, cd != '\'' && !passnext);
    }

got_token:
    if (st->oom) return BT_ERROR;
    (void)compassign;
    if (all_digit && t->len && (c == '<' || c == '>' || st->last == BT_LESS_AND ||
                                st->last == BT_GREATER_AND)) {
        long v = 0;
        bool fits = true;
        for (size_t i = 0; i < t->len; i++) {
            if (v > 214748364L || (v == 214748364L && t->p[i] > '7')) { fits = false; break; }
            v = v * 10 + (t->p[i] - '0');
        }
        if (fits) {
            st->number = v;
            return BT_NUMBER;
        }
    }
    int r = bsyn_special_tokens(st, t->p);
    if (r >= 0) return r;
    if (st->env->posix) {
        r = bsyn_check_reserved(st, t->p, dollar, quoted);
        if (r >= 0) return r;
    }
    if (st->expand_aliases && !quoted && st->env->alias) {
        if ((st->pst & BP_ALEXPNEXT) || bsyn_assign_ok(st, st->last)) {
            bool en = false;
            const char *v = st->env->alias(st->env->ctx, t->p, &en);
            if (v && !bsyn_alias_in_use(st, t->p)) {
                bsyn_push_string(st, v, bsyn_strlen(v), false, en, t->p);
                if (st->oom) return BT_ERROR;
                return BT_REREAD;
            }
        }
        st->pst &= ~BP_ALEXPNEXT;
    }
    if (!st->env->posix) {
        r = bsyn_check_reserved(st, t->p, dollar, quoted);
        if (r >= 0) return r;
    }
    /* yylval */
    st->wtext.len = 0;
    bsyn_putn(st, &st->wtext, t->p, t->len);
    st->wquoted = quoted;
    bool assign = bsyn_assignment(t->p, (st->pst & BP_COMPASSIGN) != 0) > 0;
    bool nosplit = assign && (bsyn_assign_ok(st, st->last) || (st->pst & BP_COMPASSIGN));
    if (bsyn_command_pos(st, st->last)) {
        if (bsyn_streq(t->p, "alias") || bsyn_streq(t->p, "declare") ||
            bsyn_streq(t->p, "export") || bsyn_streq(t->p, "local") ||
            bsyn_streq(t->p, "readonly") || bsyn_streq(t->p, "typeset") ||
            bsyn_streq(t->p, "eval") || bsyn_streq(t->p, "let"))
            st->pst |= BP_ASSIGNOK;
    }
    if (t->len >= 2 && t->p[0] == '{' && t->p[t->len - 1] == '}' && (c == '<' || c == '>')) {
        if (bsyn_ident(t->p + 1, t->len - 2) || bsyn_array_ref(t->p + 1, t->len - 2)) {
            st->wtext.len = 0;
            bsyn_putn(st, &st->wtext, t->p + 1, t->len - 2);
            return BT_REDIR_WORD;
        }
    }
    r = nosplit ? BT_ASSIGNMENT_WORD : BT_WORD;
    switch (st->last) {
    case BT_FUNCTION:
        st->pst |= BP_ALLOWOPNBRC;
        break;
    case BT_CASE: case BT_SELECT: case BT_FOR:
        st->expect_in++;
        break;
    }
    return r;
}

static int bsyn_cond_error_tok(struct bsyn *st);

/* read_token */
static int bsyn_read_token(struct bsyn *st) {
    int c, peek, r;
    if (st->to_read) {
        r = st->to_read;
        st->to_read = 0;
        return r;
    }
    if ((st->pst & (BP_CONDCMD | BP_CONDEXPR)) == BP_CONDCMD) {
        st->cond_lineno = st->lineno;
        st->pst |= BP_CONDEXPR;
        BSYN_DEPTH_IN(st);
        if (!st->oom) bsyn_cond_or(st);
        BSYN_DEPTH_OUT(st);
        if (st->oom || st->failed) return BT_ERROR;
        if (st->cond_token != BT_COND_END) {
            (void)bsyn_cond_error_tok(st);
            return BT_ERROR;
        }
        st->to_read = BT_COND_END;
        st->pst &= ~(BP_CONDEXPR | BP_CONDCMD);
        return BT_COND_CMD;
    }
re_read:
    while ((c = bsyn_getc(st, 1)) != BSYN_EOFC && bsyn_blank(c))
        ;
    if (st->oom) return BT_ERROR;
    if (c == BSYN_EOFC) {
        st->eof = true;
        return BT_EOF;
    }
    if (c == '#') {
        st->pst |= BP_COMMENT;
        while ((c = bsyn_getc(st, 0)) != BSYN_EOFC && c != '\n')
            ;
        if (c != BSYN_EOFC) bsyn_ungetc(st, c);
        (void)bsyn_getc(st, 0);
        st->pst &= ~BP_COMMENT;
        c = '\n';
    }
    if (c == '\n') {
        if (st->nhd) bsyn_gather_heredocs(st);
        st->pst &= ~(BP_ALEXPNEXT | BP_ASSIGNOK);
        return '\n';
    }
    if (st->pst & BP_REGEXP) goto tokword;
    if (bsyn_meta(c)) {
        if (c == '<' || c == '>') st->pst &= ~BP_ALEXPNEXT;
        st->pst &= ~BP_ASSIGNOK;
        if ((st->pst & BP_CMDSUBST) && c == st->eof_tok) peek = bsyn_getc(st, 0);
        else peek = bsyn_getc(st, 1);
        if (c == peek) {
            switch (c) {
            case '<':
                peek = bsyn_getc(st, 1);
                if (peek == '-') return BT_LESS_LESS_MINUS;
                if (peek == '<') return BT_LESS_LESS_LESS;
                bsyn_ungetc(st, peek);
                return BT_LESS_LESS;
            case '>':
                return BT_GREATER_GREATER;
            case ';':
                st->pst |= BP_CASEPAT;
                st->pst &= ~BP_ALEXPNEXT;
                peek = bsyn_getc(st, 1);
                if (peek == '&') return BT_SEMI_SEMI_AND;
                bsyn_ungetc(st, peek);
                return BT_SEMI_SEMI;
            case '&':
                return BT_AND_AND;
            case '|':
                return BT_OR_OR;
            case '(':
                r = bsyn_parse_dparen(st, c);
                if (r != -2) return r;
                break;
            }
        } else if (c == '<' && peek == '&') {
            return BT_LESS_AND;
        } else if (c == '>' && peek == '&') {
            return BT_GREATER_AND;
        } else if (c == '<' && peek == '>') {
            return BT_LESS_GREATER;
        } else if (c == '>' && peek == '|') {
            return BT_GREATER_BAR;
        } else if (c == '&' && peek == '>') {
            peek = bsyn_getc(st, 1);
            if (peek == '>') return BT_AND_GREATER_GREATER;
            bsyn_ungetc(st, peek);
            return BT_AND_GREATER;
        } else if (c == '|' && peek == '&') {
            return BT_BAR_AND;
        } else if (c == ';' && peek == '&') {
            st->pst |= BP_CASEPAT;
            st->pst &= ~BP_ALEXPNEXT;
            return BT_SEMI_AND;
        }
        bsyn_ungetc(st, peek);
        if (c == ')' && st->last == '(' && st->before == BT_WORD) {
            st->pst |= BP_ALLOWOPNBRC;
            st->pst &= ~BP_ALEXPNEXT;
        }
        if (c == '(' && !(st->pst & BP_CASEPAT)) st->pst |= BP_SUBSHELL;
        else if ((st->pst & BP_CASEPAT) && c == ')') st->pst &= ~BP_CASEPAT;
        else if ((st->pst & BP_SUBSHELL) && c == ')') st->pst &= ~BP_SUBSHELL;
        if ((c != '>' && c != '<') || peek != '(') return c;
    }
    if (c == '-' && (st->last == BT_LESS_AND || st->last == BT_GREATER_AND)) return c;
tokword:
    r = bsyn_read_word(st, c);
    if (r == BT_REREAD) goto re_read;
    return r;
}

/* ---- [[ ... ]] ---------------------------------------------------------- */

static bool bsyn_test_unop(const char *op) {
    if (op[0] != '-' || (op[1] && op[2] != 0)) return false;
    switch (op[1]) {
    case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': case 'g':
    case 'h': case 'k': case 'n': case 'o': case 'p': case 'r': case 's':
    case 't': case 'u': case 'v': case 'w': case 'x': case 'z': case 'G':
    case 'L': case 'O': case 'S': case 'N': case 'R':
        return true;
    }
    return false;
}

static bool bsyn_test_binop(const char *op) {
    if (op[0] == '=' && op[1] == 0) return true;
    if ((op[0] == '<' || op[0] == '>') && op[1] == 0) return true;
    if ((op[0] == '=' || op[0] == '!') && op[1] == '=' && op[2] == 0) return true;
    if (op[0] && op[1] == '~' && op[2] == 0 && (op[0] == '=' || op[0] == '!')) return true;
    if (op[0] != '-' || op[1] == 0 || op[2] == 0 || op[3] != 0) return false;
    if (op[2] == 't') return op[1] == 'n' || op[1] == 'o' || op[1] == 'l' || op[1] == 'g';
    if (op[1] == 'e') return op[2] == 'q' || op[2] == 'f';
    if (op[2] == 'e') return op[1] == 'n' || op[1] == 'g' || op[1] == 'l';
    return false;
}

#define BSYN_COND_FAIL(st) do { (st)->cond_token = BT_COND_ERROR; return; } while (0)

static int bsyn_cond_skip_nl(struct bsyn *st) {
    while ((st->cond_token = bsyn_read_token(st)) == '\n' && !st->oom)
        ;
    return st->cond_token;
}

static void bsyn_cond_msg_tok(struct bsyn *st, unsigned long line, int tok,
                              const char *a, const char *b, const char *plain) {
    char *e = bsyn_tok_text(st, tok);
    if (e) {
        bsyn_msg(st, line, BSYN_MSG_PARSER, a, e, b);
        BSYN_FREE(e);
    } else {
        bsyn_msg(st, line, BSYN_MSG_PARSER, plain, 0, 0);
    }
}

static void bsyn_cond_term(struct bsyn *st) {
    int tok = bsyn_cond_skip_nl(st);
    unsigned long lineno = st->lineno;
    /* A lexer error here (an unclosed quote) is not silent: bash's
     * cond_term takes the -1 as one more token it cannot use, and says so.
     * An error that ENDS the input (inside a $( )) leaves without a word. */
    if (st->oom || st->failed) BSYN_COND_FAIL(st);
    if (tok == BT_COND_END) BSYN_COND_FAIL(st);
    if (tok == '(') {
        BSYN_DEPTH_IN(st);
        if (!st->oom) bsyn_cond_or(st);
        BSYN_DEPTH_OUT(st);
        if (st->oom || st->failed) BSYN_COND_FAIL(st);
        if (st->cond_token != ')') {
            bsyn_cond_msg_tok(st, lineno, st->cond_token, "unexpected token `",
                              "', expected `)'", "expected `)'");
            BSYN_COND_FAIL(st);
        }
        (void)bsyn_cond_skip_nl(st);
        return;
    }
    if (tok == BT_BANG || (tok == BT_WORD && st->wtext.p && bsyn_streq(st->wtext.p, "!"))) {
        BSYN_DEPTH_IN(st);
        if (!st->oom) bsyn_cond_term(st);
        BSYN_DEPTH_OUT(st);
        return;
    }
    if (tok == BT_WORD && st->wtext.p && st->wtext.p[0] == '-' && st->wtext.p[1] &&
        st->wtext.p[2] == 0 && bsyn_test_unop(st->wtext.p)) {
        tok = bsyn_read_token(st);
        if (st->oom || st->failed) BSYN_COND_FAIL(st);
        if (tok != BT_WORD) {
            bsyn_cond_msg_tok(st, st->lineno, tok, "unexpected argument `",
                              "' to conditional unary operator",
                              "unexpected argument to conditional unary operator");
            BSYN_COND_FAIL(st);
        }
        (void)bsyn_cond_skip_nl(st);
        return;
    }
    if (tok == BT_WORD) {
        tok = bsyn_read_token(st);
        if (st->oom || st->failed) BSYN_COND_FAIL(st);
        if (tok == BT_WORD && st->wtext.p && bsyn_test_binop(st->wtext.p)) {
            const char *op = st->wtext.p;
            if ((op[0] == '=' && (op[1] == 0 || (op[1] == '=' && op[2] == 0))) ||
                (op[0] == '!' && op[1] == '=' && op[2] == 0))
                st->pst |= BP_EXTPAT;
            if (bsyn_streq(op, "=~")) st->pst |= BP_REGEXP;
        } else if (tok == '<' || tok == '>') {
            /* a string comparison */
        } else if (tok == BT_COND_END || tok == BT_AND_AND || tok == BT_OR_OR || tok == ')') {
            st->cond_token = tok;       /* [[ x ]] is [[ -n x ]] */
            return;
        } else {
            bsyn_cond_msg_tok(st, st->lineno, tok, "unexpected token `",
                              "', conditional binary operator expected",
                              "conditional binary operator expected");
            BSYN_COND_FAIL(st);
        }
        bool saved = st->extglob;
        if (st->pst & BP_EXTPAT) st->extglob = true;
        tok = bsyn_read_token(st);
        if (st->pst & BP_EXTPAT) st->extglob = saved;
        st->pst &= ~(BP_REGEXP | BP_EXTPAT);
        if (st->oom || st->failed) BSYN_COND_FAIL(st);
        if (tok != BT_WORD) {
            bsyn_cond_msg_tok(st, st->lineno, tok, "unexpected argument `",
                              "' to conditional binary operator",
                              "unexpected argument to conditional binary operator");
            BSYN_COND_FAIL(st);
        }
        (void)bsyn_cond_skip_nl(st);
        return;
    }
    if (tok < 256) {
        /* bash prints the token with %c: a lexer error's -1 is byte 0xff */
        char ch[2] = { (char)(unsigned char)tok, 0 };
        bsyn_msg(st, st->lineno, BSYN_MSG_PARSER, "unexpected token `", ch,
                 "' in conditional command");
    } else {
        char *e = bsyn_tok_text(st, tok);
        if (e) {
            bsyn_msg(st, st->lineno, BSYN_MSG_PARSER, "unexpected token `", e,
                     "' in conditional command");
            BSYN_FREE(e);
        } else {
            struct bsyn_buf n = {0, 0, 0};
            bsyn_put_ulong(st, &n, (unsigned long)tok);
            if (n.p) bsyn_msg(st, st->lineno, BSYN_MSG_PARSER, "unexpected token ", n.p,
                              " in conditional command");
            bsyn_buf_free(&n);
        }
    }
    BSYN_COND_FAIL(st);
}

static void bsyn_cond_and(struct bsyn *st) {
    bsyn_cond_term(st);
    if (st->cond_token == BT_AND_AND && !st->oom && !st->failed) {
        BSYN_DEPTH_IN(st);
        if (!st->oom) bsyn_cond_and(st);
        BSYN_DEPTH_OUT(st);
    }
}

static void bsyn_cond_or(struct bsyn *st) {
    bsyn_cond_and(st);
    if (st->cond_token == BT_OR_OR && !st->oom && !st->failed) {
        BSYN_DEPTH_IN(st);
        if (!st->oom) bsyn_cond_or(st);
        BSYN_DEPTH_OUT(st);
    }
}

/* cond_error */
static int bsyn_cond_error_tok(struct bsyn *st) {
    if (st->eof && st->cond_token != BT_COND_ERROR) {
        bsyn_msg(st, st->cond_lineno, BSYN_MSG_PARSER, "unexpected EOF while looking for `]]'", 0, 0);
    } else if (st->cond_token != BT_COND_ERROR) {
        char *e = bsyn_tok_text(st, st->cond_token);
        if (e) {
            bsyn_msg(st, st->cond_lineno, BSYN_MSG_PARSER,
                     "syntax error in conditional expression: unexpected token `", e, "'");
            BSYN_FREE(e);
        } else {
            bsyn_msg(st, st->cond_lineno, BSYN_MSG_PARSER, "syntax error in conditional expression", 0, 0);
        }
    }
    return 0;
}

/* ---- the parser: bash's grammar, by recursive descent -------------------
 *
 * A token is read when the grammar needs to look at it (bsyn_la), exactly
 * where bash's parser reads its lookahead -- which matters, because the
 * lexer's answers depend on what the parser has done by then: a
 * redirection at the start of a simple command lets an assignment follow
 * it (REDIRLIST), a here-document must be queued before the newline that
 * collects it is read. */

static int bsyn_la(struct bsyn *st) {
    if (st->have_la) return st->cur;
    st->two_ago = st->before;
    st->before = st->last;
    st->last = st->cur;
    int t = bsyn_read_token(st);
    st->cur = t;
    st->have_la = true;
    if (st->oom) { st->failed = true; return BT_ERROR; }
    if (t == BT_ERROR && !st->failed) bsyn_lexfail(st);
    return t;
}

static void bsyn_shift(struct bsyn *st) { st->have_la = false; }

static bool bsyn_redir_op(int t) {
    return t == '>' || t == '<' || t == BT_GREATER_GREATER || t == BT_GREATER_BAR ||
           t == BT_LESS_GREATER || t == BT_LESS_LESS || t == BT_LESS_LESS_MINUS ||
           t == BT_LESS_LESS_LESS || t == BT_LESS_AND || t == BT_GREATER_AND ||
           t == BT_AND_GREATER || t == BT_AND_GREATER_GREATER;
}

static bool bsyn_redir_start(int t) {
    return bsyn_redir_op(t) || t == BT_NUMBER || t == BT_REDIR_WORD;
}

static bool bsyn_shell_cmd_start(int t) {
    return t == BT_FOR || t == BT_CASE || t == BT_WHILE || t == BT_UNTIL ||
           t == BT_SELECT || t == BT_IF || t == '(' || t == '{' ||
           t == BT_ARITH_CMD || t == BT_COND_START;
}

static bool bsyn_cmd_start(int t) {
    return bsyn_shell_cmd_start(t) || bsyn_redir_start(t) || t == BT_WORD ||
           t == BT_ASSIGNMENT_WORD || t == BT_FUNCTION || t == BT_COPROC;
}

static bool bsyn_pipeline_start(int t) {
    return bsyn_cmd_start(t) || t == BT_BANG || t == BT_TIME;
}

/* Take the token if it is T; otherwise it is the syntax error. */
static bool bsyn_expect(struct bsyn *st, int t) {
    if (st->failed) return false;
    if (bsyn_la(st) != t) {
        if (!st->failed) bsyn_syntax_error(st);
        return false;
    }
    bsyn_shift(st);
    return true;
}

static void bsyn_newlines(struct bsyn *st) {
    while (!st->failed && bsyn_la(st) == '\n') bsyn_shift(st);
}

static void bsyn_pipeline_command(struct bsyn *st);
static void bsyn_command(struct bsyn *st);

/* a redirection; the lookahead starts one */
static void bsyn_redirection(struct bsyn *st) {
    int t = bsyn_la(st);
    bool prefixed = false;
    if (t == BT_NUMBER || t == BT_REDIR_WORD) {
        bsyn_shift(st);
        prefixed = true;
        t = bsyn_la(st);
        if (st->failed) return;
    }
    if (!bsyn_redir_op(t) || (prefixed && (t == BT_AND_GREATER || t == BT_AND_GREATER_GREATER))) {
        bsyn_syntax_error(st);
        return;
    }
    bsyn_shift(st);
    int target = bsyn_la(st);
    if (st->failed) return;
    if (t == BT_LESS_AND || t == BT_GREATER_AND) {
        if (target != BT_NUMBER && target != BT_WORD && target != '-') {
            bsyn_syntax_error(st);
            return;
        }
    } else if (target != BT_WORD) {
        bsyn_syntax_error(st);
        return;
    }
    bsyn_shift(st);
    if (t == BT_LESS_LESS || t == BT_LESS_LESS_MINUS)
        bsyn_push_heredoc(st, t == BT_LESS_LESS_MINUS);
}

static void bsyn_redirection_list_opt(struct bsyn *st) {
    while (!st->failed && bsyn_redir_start(bsyn_la(st))) bsyn_redirection(st);
}

/* the rest of a simple command: (WORD | ASSIGNMENT_WORD | redirection)* */
static void bsyn_simple_rest(struct bsyn *st) {
    for (;;) {
        if (st->failed) return;
        int t = bsyn_la(st);
        if (t == BT_WORD || t == BT_ASSIGNMENT_WORD) {
            bsyn_shift(st);
            st->pst &= ~BP_REDIRLIST;
            continue;
        }
        if (bsyn_redir_start(t)) {
            bsyn_redirection(st);
            continue;
        }
        break;
    }
    st->pst &= ~BP_REDIRLIST;          /* clean_simple_command */
}

/* a simple command whose first element is the lookahead */
static void bsyn_simple_command(struct bsyn *st) {
    int t = bsyn_la(st);
    if (t == BT_WORD || t == BT_ASSIGNMENT_WORD) {
        bsyn_shift(st);
        st->pst &= ~BP_REDIRLIST;
    } else {
        bsyn_redirection(st);
        if (!st->failed) st->pst |= BP_REDIRLIST;
    }
    bsyn_simple_rest(st);
}

static void bsyn_compound_list(struct bsyn *st) {
    bsyn_newlines(st);
    if (st->failed) return;
    bsyn_list(st);
}

/* list1 [ list0's terminator ], with the leading newlines already gone:
 * pipelines joined by && || ; & newline. */
static void bsyn_list(struct bsyn *st) {
    BSYN_DEPTH_IN(st);
    for (;;) {
        if (st->failed) break;
        if (!bsyn_pipeline_start(bsyn_la(st))) {
            if (!st->failed) bsyn_syntax_error(st);
            break;
        }
        bsyn_pipeline_command(st);
        if (st->failed) break;
        int t = bsyn_la(st);
        if (t == BT_AND_AND || t == BT_OR_OR) {
            bsyn_shift(st);
            bsyn_newlines(st);
            continue;
        }
        if (t == ';' || t == '&' || t == '\n') {
            bsyn_shift(st);
            bsyn_newlines(st);
            if (st->failed) break;
            if (bsyn_pipeline_start(bsyn_la(st))) continue;
        }
        break;
    }
    BSYN_DEPTH_OUT(st);
}

static void bsyn_body(struct bsyn *st, int open) {
    /* DO compound_list DONE | '{' compound_list '}' */
    bsyn_shift(st);
    bsyn_compound_list(st);
    bsyn_expect(st, open == BT_DO ? BT_DONE : '}');
}

/* The parts of a for (( ; ; )), split as bash's skip_to_delim splits
 * them (SD_NOJMP|SD_NOPROCSUB): a backslash, `...`, '...', "..." and a
 * $( ) or ${ } are skipped; a plain ( [ { is not. */
static size_t bsyn_skip_group(const char *s, size_t i, char open, char close) {
    int depth = 1;
    while (s[i] && depth) {
        char c = s[i];
        if (c == '\\' && s[i + 1]) { i += 2; continue; }
        if (c == '\'' || c == '"' || c == '`') {
            i++;
            while (s[i] && s[i] != c) { if (c != '\'' && s[i] == '\\' && s[i + 1]) i++; i++; }
            if (s[i]) i++;
            continue;
        }
        if (c == open) depth++;
        else if (c == close) depth--;
        i++;
    }
    return i;
}

static int bsyn_arith_for_parts(const char *s) {
    int parts = 1;
    size_t i = 0;
    while (s[i]) {
        char c = s[i];
        if (c == '\\') { i += s[i + 1] ? 2 : 1; continue; }
        if (c == '`') {
            i++;
            while (s[i] && s[i] != '`') { if (s[i] == '\\' && s[i + 1]) i++; i++; }
            if (s[i]) i++;
            continue;
        }
        if (c == ';') { parts++; i++; continue; }
        if (c == '\'' || c == '"') {
            i++;
            while (s[i] && s[i] != c) { if (c == '"' && s[i] == '\\' && s[i + 1]) i++; i++; }
            if (s[i]) i++;
            continue;
        }
        if (c == '$' && (s[i + 1] == '(' || s[i + 1] == '{')) {
            i = bsyn_skip_group(s, i + 2, s[i + 1], s[i + 1] == '(' ? ')' : '}');
            continue;
        }
        i++;
    }
    return parts;
}

static void bsyn_for(struct bsyn *st, bool select) {
    unsigned long for_line = st->lineno;
    bsyn_shift(st);
    int t = bsyn_la(st);
    if (st->failed) return;
    if (!select && t == BT_ARITH_FOR_EXPRS) {
        struct bsyn_buf ex = {0, 0, 0};
        bsyn_putn(st, &ex, st->wtext.p ? st->wtext.p : "", st->wtext.len);
        bsyn_shift(st);
        t = bsyn_la(st);
        if (!st->failed && (t == ';' || t == '\n' || t == BT_EOF)) {
            bsyn_shift(st);
            bsyn_newlines(st);
            t = bsyn_la(st);
        }
        if (!st->failed) {
            if (t == BT_DO || t == '{') bsyn_body(st, t);
            else bsyn_syntax_error(st);
        }
        if (!st->failed && ex.p) {
            /* make_arith_for_command: exactly three expressions */
            int n = bsyn_arith_for_parts(ex.p);
            if (n != 3) {
                bsyn_msg(st, for_line, BSYN_MSG_PARSER,
                         n < 3 ? "syntax error: arithmetic expression required"
                               : "syntax error: `;' unexpected", 0, 0);
                bsyn_msg(st, for_line, BSYN_MSG_PARSER, "syntax error: `((", ex.p, "))'");
                bsyn_status_set(st, 2);
                bsyn_lexfail(st);       /* YYERROR: recovery, no message */
            }
        }
        bsyn_buf_free(&ex);
        return;
    }
    if (t != BT_WORD) { bsyn_syntax_error(st); return; }
    bsyn_shift(st);
    t = bsyn_la(st);
    if (st->failed) return;
    if (t == ';') {
        bsyn_shift(st);
        bsyn_newlines(st);
        t = bsyn_la(st);
        if (st->failed) return;
        if (t == BT_DO || t == '{') bsyn_body(st, t);
        else bsyn_syntax_error(st);
        return;
    }
    bsyn_newlines(st);
    t = bsyn_la(st);
    if (st->failed) return;
    if (t == BT_DO || t == '{') { bsyn_body(st, t); return; }
    if (t != BT_IN) { bsyn_syntax_error(st); return; }
    bsyn_shift(st);
    while (!st->failed && bsyn_la(st) == BT_WORD) bsyn_shift(st);
    t = bsyn_la(st);
    if (st->failed) return;
    if (t != ';' && t != '\n' && t != BT_EOF) { bsyn_syntax_error(st); return; }
    bsyn_shift(st);
    bsyn_newlines(st);
    t = bsyn_la(st);
    if (st->failed) return;
    if (t == BT_DO || t == '{') bsyn_body(st, t);
    else bsyn_syntax_error(st);
}

static void bsyn_case(struct bsyn *st) {
    bsyn_shift(st);
    if (bsyn_la(st) != BT_WORD) { if (!st->failed) bsyn_syntax_error(st); return; }
    bsyn_shift(st);
    bsyn_newlines(st);
    if (!bsyn_expect(st, BT_IN)) return;
    for (;;) {
        bsyn_newlines(st);
        if (st->failed) return;
        int t = bsyn_la(st);
        if (t == BT_ESAC) { bsyn_shift(st); return; }
        if (t == '(') { bsyn_shift(st); t = bsyn_la(st); }
        if (st->failed) return;
        if (t != BT_WORD) { bsyn_syntax_error(st); return; }
        bsyn_shift(st);
        while (!st->failed && bsyn_la(st) == '|') {
            bsyn_shift(st);
            if (bsyn_la(st) != BT_WORD) { if (!st->failed) bsyn_syntax_error(st); return; }
            bsyn_shift(st);
        }
        if (!bsyn_expect(st, ')')) return;
        bsyn_newlines(st);
        if (st->failed) return;
        if (bsyn_pipeline_start(bsyn_la(st))) bsyn_list(st);
        if (st->failed) return;
        t = bsyn_la(st);
        if (t == BT_SEMI_SEMI || t == BT_SEMI_AND || t == BT_SEMI_SEMI_AND) {
            bsyn_shift(st);
            continue;
        }
        if (t == BT_ESAC) { bsyn_shift(st); return; }
        bsyn_syntax_error(st);
        return;
    }
}

static void bsyn_if(struct bsyn *st) {
    bsyn_shift(st);
    bsyn_compound_list(st);
    if (!bsyn_expect(st, BT_THEN)) return;
    bsyn_compound_list(st);
    for (;;) {
        if (st->failed) return;
        int t = bsyn_la(st);
        if (t == BT_ELIF) {
            bsyn_shift(st);
            bsyn_compound_list(st);
            if (!bsyn_expect(st, BT_THEN)) return;
            bsyn_compound_list(st);
            continue;
        }
        if (t == BT_ELSE) {
            bsyn_shift(st);
            bsyn_compound_list(st);
        }
        bsyn_expect(st, BT_FI);
        return;
    }
}

/* shell_command: the lookahead starts one */
static void bsyn_shell_command(struct bsyn *st) {
    int t = bsyn_la(st);
    BSYN_DEPTH_IN(st);
    switch (t) {
    case BT_FOR: bsyn_for(st, false); break;
    case BT_SELECT: bsyn_for(st, true); break;
    case BT_CASE: bsyn_case(st); break;
    case BT_IF: bsyn_if(st); break;
    case BT_WHILE: case BT_UNTIL:
        bsyn_shift(st);
        bsyn_compound_list(st);
        if (!bsyn_expect(st, BT_DO)) break;
        bsyn_compound_list(st);
        bsyn_expect(st, BT_DONE);
        break;
    case '(':
        bsyn_shift(st);
        bsyn_compound_list(st);
        bsyn_expect(st, ')');
        break;
    case '{':
        bsyn_shift(st);
        bsyn_compound_list(st);
        bsyn_expect(st, '}');
        break;
    case BT_ARITH_CMD:
        bsyn_shift(st);
        break;
    case BT_COND_START:
        bsyn_shift(st);
        if (!bsyn_expect(st, BT_COND_CMD)) break;
        bsyn_expect(st, BT_COND_END);
        break;
    default:
        bsyn_syntax_error(st);
        break;
    }
    BSYN_DEPTH_OUT(st);
}

static void bsyn_function_body(struct bsyn *st) {
    if (!bsyn_shell_cmd_start(bsyn_la(st))) {
        if (!st->failed) bsyn_syntax_error(st);
        return;
    }
    bsyn_shell_command(st);
    bsyn_redirection_list_opt(st);
}

static void bsyn_command(struct bsyn *st) {
    int t = bsyn_la(st);
    if (st->failed) return;
    if (bsyn_shell_cmd_start(t)) {
        bsyn_shell_command(st);
        bsyn_redirection_list_opt(st);
        return;
    }
    if (t == BT_FUNCTION) {
        bsyn_shift(st);
        if (bsyn_la(st) != BT_WORD) { if (!st->failed) bsyn_syntax_error(st); return; }
        bsyn_shift(st);
        t = bsyn_la(st);
        if (st->failed) return;
        if (t == '(') {
            bsyn_shift(st);
            if (!bsyn_expect(st, ')')) return;
            bsyn_newlines(st);
        } else if (t == '\n') {
            bsyn_shift(st);
            bsyn_newlines(st);
        }
        if (st->failed) return;
        bsyn_function_body(st);
        return;
    }
    if (t == BT_COPROC) {
        bsyn_shift(st);
        t = bsyn_la(st);
        if (st->failed) return;
        if (bsyn_shell_cmd_start(t)) {
            bsyn_shell_command(st);
            bsyn_redirection_list_opt(st);
            return;
        }
        if (t == BT_WORD) {
            bsyn_shift(st);
            t = bsyn_la(st);
            if (st->failed) return;
            if (bsyn_shell_cmd_start(t)) {
                bsyn_shell_command(st);
                bsyn_redirection_list_opt(st);
                return;
            }
            st->pst &= ~BP_REDIRLIST;
            bsyn_simple_rest(st);
            return;
        }
        if (t == BT_ASSIGNMENT_WORD || bsyn_redir_start(t)) {
            bsyn_simple_command(st);
            return;
        }
        bsyn_syntax_error(st);
        return;
    }
    if (t == BT_WORD) {
        bsyn_shift(st);
        t = bsyn_la(st);
        if (st->failed) return;
        if (t == '(') {
            /* function_def: WORD '(' ')' newline_list function_body */
            bsyn_shift(st);
            if (!bsyn_expect(st, ')')) return;
            bsyn_newlines(st);
            if (st->failed) return;
            bsyn_function_body(st);
            return;
        }
        st->pst &= ~BP_REDIRLIST;
        bsyn_simple_rest(st);
        return;
    }
    bsyn_simple_command(st);
}

static void bsyn_pipeline(struct bsyn *st) {
    for (;;) {
        bsyn_command(st);
        if (st->failed) return;
        int t = bsyn_la(st);
        if (t != '|' && t != BT_BAR_AND) return;
        bsyn_shift(st);
        bsyn_newlines(st);
        if (st->failed) return;
        if (!bsyn_cmd_start(bsyn_la(st))) {
            if (!st->failed) bsyn_syntax_error(st);
            return;
        }
    }
}

static void bsyn_pipeline_command(struct bsyn *st) {
    BSYN_DEPTH_IN(st);
    int t = bsyn_la(st);
    if (!st->failed && (t == BT_BANG || t == BT_TIME)) {
        bsyn_shift(st);
        if (t == BT_TIME) {
            if (bsyn_la(st) == BT_TIMEOPT) bsyn_shift(st);
            if (!st->failed && bsyn_la(st) == BT_TIMEIGN) bsyn_shift(st);
        }
        int u = bsyn_la(st);
        if (!st->failed) {
            if (u == ';' || u == '\n' || u == BT_EOF) {
                /* `time` or `!` alone: bash pushes the terminator back */
                bsyn_shift(st);
                if (u != BT_EOF) st->to_read = u;
                st->pst &= ~BP_REDIRLIST;
            } else if (bsyn_pipeline_start(u)) {
                bsyn_pipeline_command(st);
            } else {
                bsyn_syntax_error(st);
            }
        }
        BSYN_DEPTH_OUT(st);
        return;
    }
    if (!st->failed) bsyn_pipeline(st);
    BSYN_DEPTH_OUT(st);
}

/* inputunit: simple_list simple_list_terminator | '\n' | yacc_EOF */
static int bsyn_inputunit(struct bsyn *st) {
    int t = bsyn_la(st);
    if (st->failed) return BSYN_ERROR;
    if (t == '\n') { bsyn_shift(st); return BSYN_OK; }
    if (t == BT_EOF) return BSYN_EOF;
    if (!bsyn_pipeline_start(t)) { bsyn_syntax_error(st); return BSYN_ERROR; }
    for (;;) {
        bsyn_pipeline_command(st);
        if (st->failed) return BSYN_ERROR;
        t = bsyn_la(st);
        if (st->failed) return BSYN_ERROR;
        if (t == BT_AND_AND || t == BT_OR_OR) {
            bsyn_shift(st);
            bsyn_newlines(st);
            if (st->failed) return BSYN_ERROR;
            if (!bsyn_pipeline_start(bsyn_la(st))) {
                if (!st->failed) bsyn_syntax_error(st);
                return BSYN_ERROR;
            }
            continue;
        }
        if (t == ';' || t == '&') {
            bsyn_shift(st);
            t = bsyn_la(st);
            if (st->failed) return BSYN_ERROR;
            if (bsyn_pipeline_start(t)) continue;
        }
        break;
    }
    t = bsyn_la(st);
    if (st->failed) return BSYN_ERROR;
    if (t == '\n') {
        bsyn_shift(st);
        return BSYN_OK;
    }
    /* Anything else reduces the list to simple_list first -- bison's
     * default reductions -- and that rule's action reads any here-document
     * still pending. Then the end of input is a terminator; any other token
     * is the error, reported after the documents were read. */
    if (st->nhd) bsyn_gather_heredocs(st);
    if (st->oom) return BSYN_ERROR;
    if (t == BT_EOF) return BSYN_OK;
    bsyn_syntax_error(st);
    return BSYN_ERROR;
}

/* After a lexer error bison discards tokens up to a newline or the end of
 * input; at the end of input `error yacc_EOF` makes $? 2 if it was 0. */
static void bsyn_recover(struct bsyn *st) {
    int t = st->cur;
    int guard = 0;
    st->failed = false;          /* let the lexer run */
    st->have_la = false;
    while (t != '\n' && t != BT_EOF && !st->oom && !st->failed && guard++ < 100000) {
        st->two_ago = st->before;
        st->before = st->last;
        st->last = st->cur;
        t = bsyn_read_token(st);
        st->cur = t;
    }
    st->failed = true;
    if (t == BT_EOF) bsyn_status_ifzero(st, 2);
}

static void bsyn_result_free(struct bsyn_result *r) {
    for (int i = 0; i < r->nmsg; i++) if (r->msg[i].text) BSYN_FREE(r->msg[i].text);
    r->nmsg = 0;
}

/* ONE PARSER FOR A WHOLE INPUT. bash's lexer keeps its state from one
 * command to the next -- the line it is on, text an alias or a `((`
 * pushed back, the last token read -- so the check reads a script through
 * one handle, a command at a time, as bash does:
 *
 *     h = bsyn_open(text, len, off, line_before, &env);
 *     while (bsyn_next(h, &res) == BSYN_OK) ...
 *     bsyn_close(h);
 *
 * The environment is read again for each command: a `shopt -s extglob`
 * that ran changes how the next command is read, and only that one. */
static struct bsyn *bsyn_open(const char *buf, size_t len, size_t off,
                              unsigned long line_before, const struct bsyn_env *env) {
    struct bsyn *st = (struct bsyn *)BSYN_ALLOC(sizeof *st);
    if (!st) return 0;
    memset(st, 0, sizeof *st);
    st->env = env;
    st->buf = buf;
    st->len = len;
    st->next = off;
    st->sl = "";
    st->ovr_idx = -1;
    st->lineno = line_before;
    st->cur = '\n';
    st->last = '\n';
    st->before = '\n';
    return st;
}

/* The offset of the next character of BUF that has not been read. */
static size_t bsyn_pos(struct bsyn *st) {
    const char *sl = st->sl;
    size_t slen = st->slen, sidx = st->sidx;
    struct bsyn_push *p = st->push;
    while (p && p->prev) p = p->prev;
    if (p) { sl = p->sl; slen = p->slen; sidx = p->sidx; }
    if (!st->have_line || sidx > slen || sl < st->buf || sl > st->buf + st->len)
        return st->next;
    return (size_t)(sl - st->buf) + sidx;
}

static void bsyn_close(struct bsyn *st) {
    if (!st) return;
    while (st->push) bsyn_pop_string(st);
    for (int i = 0; i < st->nhd; i++) if (st->hd[i].delim) BSYN_FREE(st->hd[i].delim);
    bsyn_buf_free(&st->tok);
    bsyn_buf_free(&st->wtext);
    if (st->dstack) BSYN_FREE(st->dstack);
    BSYN_FREE(st);
}

/* Read the next command. After BSYN_ERROR, BSYN_EOF or BSYN_SKIP there is
 * nothing more to read. */
static int bsyn_next(struct bsyn *st, struct bsyn_result *res) {
    memset(res, 0, sizeof *res);
    st->res = res;
    st->failed = st->lexfail = st->discard = st->fatal = false;
    st->st_set = false;
    st->st_val = st->st_ifzero = 0;
    st->have_la = false;
    st->depth = 0;
    st->extglob = st->env->extglob;
    st->expand_aliases = st->env->expand_aliases;
    /* parse_command: no here-document is pending at the start */
    for (int i = 0; i < st->nhd; i++) if (st->hd[i].delim) BSYN_FREE(st->hd[i].delim);
    st->nhd = 0;

    int kind;
    if (st->oom) {
        kind = BSYN_SKIP;
    } else if (st->eof) {
        kind = BSYN_EOF;
    } else {
        kind = bsyn_inputunit(st);
        if (st->oom) {
            kind = BSYN_SKIP;
        } else if (st->failed) {
            if (st->lexfail && !st->discard && !st->fatal) bsyn_recover(st);
            if (st->oom) kind = BSYN_SKIP;
            else kind = st->discard ? BSYN_DISCARD : BSYN_ERROR;
        }
        /* parse_command reads any here-document still pending once the
         * parse is over, error or not (a grammar error has dropped them) */
        if (!st->oom && st->nhd) {
            bool f = st->failed;
            st->failed = false;
            bsyn_gather_heredocs(st);
            st->failed = f;
            if (st->oom) kind = BSYN_SKIP;
        }
    }
    res->kind = kind;
    res->fatal = st->fatal;
    res->status_set = st->st_set;
    res->status = st->st_val;
    res->status_ifzero = st->st_ifzero;
    res->end = (kind == BSYN_EOF) ? st->len : bsyn_pos(st);
    res->end_line = st->lineno;
    if (kind == BSYN_SKIP) bsyn_result_free(res);
    st->res = 0;
    return kind;
}

/* BASH'S PARSER STATE IS ONE GLOBAL, and a [[ ]] error leaves two of its
 * bits set: the lexer error skips reset_parser, so "inside [[" outlives
 * the command. The NEXT parse anywhere -- the script's next line, another
 * eval, a substitution's text -- then reads `[[` and `]]` wrongly and
 * fails, until a grammar error resets the parser (measured on 5.2.15,
 * tools/agent/bashsyn: eval "[[ a b ]]"; [[ x ]] is a syntax error). The
 * shell carries those bits from one check to the next with these. */
#define BSYN_LEAK_BITS (BP_CONDCMD | BP_CONDEXPR)
static void bsyn_set_leak(struct bsyn *st, unsigned bits) {
    st->pst = (st->pst & ~BSYN_LEAK_BITS) | (bits & BSYN_LEAK_BITS);
}
static unsigned bsyn_get_leak(struct bsyn *st) { return st->pst & BSYN_LEAK_BITS; }
