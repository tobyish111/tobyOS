/* sed -- stream editor.
 *
 *     sed [-nrEsz] [-i[SUF]] [-e script] [-f file] [script] [file ...]
 *
 * There was no sed in this tree at all. busybox has one, but it is a
 * gitignored, opt-in Linux-ABI demo that a plain build does not stage, so
 * nothing in the userland could depend on it -- which meant the single
 * most-used text tool in any distro simply was not there.
 *
 * Addresses: N, $, /re/, \cREc, first~step, a1,a2, a1,+N, a1,~N, 0,/re/,
 * and ! to negate. Commands: s y p P d D n N g G h H x a i c b t T : q Q
 * = r R w W l F z { } #. s/// flags: g, a repeat count, p, i/I, m/M, w.
 * Replacements understand &, \1..\9, \n \t \\, and GNU's \U \L \u \l \E.
 *
 * The regex engine underneath is libtoby's. Pointing sed at it is the
 * second real consumer it has ever had; the first (grep) found three bugs
 * in it, so anything odd here is worth suspecting there too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <regex.h>

/* ----------------------------------------------------------- dynamic string */

struct str { char *p; size_t len, cap; };

static void str_free(struct str *s) { free(s->p); s->p = NULL; s->len = s->cap = 0; }

static int str_reserve(struct str *s, size_t need) {
    if (s->len + need + 1 <= s->cap) return 1;
    size_t nc = s->cap ? s->cap : 128;
    while (nc < s->len + need + 1) nc *= 2;
    char *np = (char *)realloc(s->p, nc);
    if (!np) return 0;
    s->p = np; s->cap = nc;
    return 1;
}

static int str_addn(struct str *s, const char *b, size_t n) {
    if (!str_reserve(s, n)) return 0;
    memcpy(s->p + s->len, b, n);
    s->len += n;
    s->p[s->len] = 0;
    return 1;
}

static int str_addc(struct str *s, char c) { return str_addn(s, &c, 1); }
static int str_set(struct str *s, const char *b, size_t n) { s->len = 0; return str_addn(s, b, n); }

/* ------------------------------------------------------------------ options */

static int nflag, Eflag, sflag, zflag, iflag, posix_mode;
static const char *isuffix = "";
static int exit_status;
static char line_sep = '\n';
/* Whether the last line READ ended with the separator, so the last line
 * WRITTEN can end the same way. */
static int  input_had_final_sep = 1;

/* ------------------------------------------------------------------ commands */

enum {
    A_NONE = 0, A_LINE, A_LAST, A_RE, A_STEP, A_PLUS, A_MULT, A_ZERO
};

struct cmd {
    int      a1type, a2type;
    long     a1line, a2line;
    regex_t  a1re, a2re;
    int      a1has, a2has;
    int      negate;
    int      active;          /* a range is currently open */
    long     range_end;       /* for +N and ~N, once resolved */

    char     name;
    /* s and y */
    regex_t  re;
    int      re_ok;
    char    *repl;
    int      gflag, pflag, occurrence;
    char    *wfile;
    FILE    *wfp;
    char    *ytab_from, *ytab_to;
    /* a i c r R text or filename */
    char    *text;
    /* b t T : */
    char    *label;
    int      target;          /* resolved index, -1 == end of script */
    /* { } */
    int      block_end;
    /* q Q exit code, l width */
    int      num;
};

static struct cmd *cmds;
static int          ncmds, cmds_cap;

static struct cmd *new_cmd(void) {
    if (ncmds == cmds_cap) {
        cmds_cap = cmds_cap ? cmds_cap * 2 : 32;
        cmds = (struct cmd *)realloc(cmds, (size_t)cmds_cap * sizeof *cmds);
        if (!cmds) { fprintf(stderr, "sed: out of memory\n"); exit(4); }
    }
    struct cmd *c = &cmds[ncmds++];
    memset(c, 0, sizeof *c);
    c->target = -1;
    c->occurrence = 1;
    return c;
}

static void bad(const char *msg) {
    fprintf(stderr, "sed: -e expression #1, char 0: %s\n", msg);
    exit(1);
}

/* -------------------------------------------------------------- script parse */

static struct str script;      /* all -e / -f text, joined by newlines */

/* Reads a delimited regex or replacement, honouring backslash escapes and
 * an arbitrary delimiter, and returns a malloc'd copy. `*pp` is advanced
 * past the closing delimiter. */
static char *take_delim(const char **pp, char delim, int keep_escapes) {
    const char *p = *pp;
    struct str out;
    memset(&out, 0, sizeof out);
    while (*p && *p != delim) {
        if (*p == '\\' && p[1]) {
            if (p[1] == delim && !keep_escapes) {
                /* An escaped delimiter is just that character. */
                str_addc(&out, p[1]);
                p += 2;
                continue;
            }
            str_addc(&out, *p++);
            str_addc(&out, *p++);
            continue;
        }
        str_addc(&out, *p++);
    }
    if (*p != delim) { str_free(&out); return NULL; }
    *pp = p + 1;
    if (!out.p) { out.p = (char *)calloc(1, 1); }
    return out.p;
}

/* GNU sed lets a bracket expression contain the delimiter; a regex is
 * compiled with REG_EXTENDED only under -E/-r. */
static int compile_re(regex_t *re, const char *pat, int icase) {
    int flags = 0;
    if (Eflag) flags |= REG_EXTENDED;
    if (icase) flags |= REG_ICASE;
    return regcomp(re, pat, flags);
}

/* The most recent //-style empty regex reuses the last one compiled. */
static regex_t  last_re;
static int      last_re_ok;

/* Collapse backslash escapes in place. Used for the y/// operands, whose
 * two sides must be the same length AFTER unescaping. */
static void unescape(char *t) {
    char *w = t;
    for (const char *r = t; *r; r++) {
        if (*r != 92 || !r[1]) { *w++ = *r; continue; }
        r++;
        switch (*r) {
        case 'n': *w++ = 10; break;
        case 't': *w++ = 9;  break;
        case 'r': *w++ = 13; break;
        case 'a': *w++ = 7;  break;
        case 'f': *w++ = 12; break;
        case 'v': *w++ = 11; break;
        default:  *w++ = *r; break;
        }
    }
    *w = 0;
}

static void skip_ws(const char **pp) {
    while (**pp == ' ' || **pp == '\t') (*pp)++;
}

static int parse_addr(const char **pp, int *type, long *line,
                      regex_t *re, int *has, long *step_out)
{
    const char *p = *pp;
    *type = A_NONE; *has = 0;

    if (*p >= '0' && *p <= '9') {
        long v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        if (*p == '~') {
            p++;
            long step = 0;
            while (*p >= '0' && *p <= '9') step = step * 10 + (*p++ - '0');
            /* The step goes out through step_out. It used to be written to
             * cmds[ncmds], which new_cmd() had already advanced PAST -- an
             * out-of-bounds write, and the value was then read back from a
             * field that had never been set. */
            *type = A_STEP;
            *line = v;
            if (step_out) *step_out = step;
            *pp = p;
            return 1;
        }
        *type = (v == 0) ? A_ZERO : A_LINE;
        *line = v;
        *pp = p;
        return 1;
    }
    if (*p == '$') { *type = A_LAST; *pp = p + 1; return 1; }
    if (*p == '/' || *p == '\\') {
        char delim = '/';
        if (*p == '\\') { p++; delim = *p; if (!delim) bad("unterminated address regex"); }
        p++;
        char *pat = take_delim(&p, delim, 1);
        if (!pat) bad("unterminated address regex");
        int icase = 0;
        while (*p == 'I' || *p == 'M') { if (*p == 'I') icase = 1; p++; }
        if (!*pat) {
            /* // means "the last regex used" */
            if (!last_re_ok) bad("no previous regular expression");
            *re = last_re; *has = 1;
        } else {
            if (compile_re(re, pat, icase) != 0) bad("invalid address regex");
            *has = 1;
            last_re = *re; last_re_ok = 1;
        }
        free(pat);
        *type = A_RE;
        *pp = p;
        return 1;
    }
    return 0;
}

/* `a`, `i` and `c` take their text either after a backslash-newline (POSIX)
 * or straight after the command on the same line (GNU). */
static char *take_text(const char **pp) {
    const char *p = *pp;
    struct str out;
    memset(&out, 0, sizeof out);
    if (*p == '\\') { p++; if (*p == '\n') p++; }
    else skip_ws(&p);
    while (*p) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n')      str_addc(&out, '\n');
            else if (*p == 't') str_addc(&out, '\t');
            else                str_addc(&out, *p);
            p++;
            continue;
        }
        if (*p == '\n') break;
        str_addc(&out, *p++);
    }
    *pp = p;
    if (!out.p) out.p = (char *)calloc(1, 1);
    return out.p;
}

static char *take_word(const char **pp) {
    skip_ws(pp);
    const char *p = *pp;
    const char *s = p;
    while (*p && *p != '\n' && *p != ';' && *p != '}') p++;
    /* a filename or label runs to end of line; trailing spaces are dropped */
    size_t n = (size_t)(p - s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t')) n--;
    char *w = (char *)malloc(n + 1);
    memcpy(w, s, n); w[n] = 0;
    *pp = p;
    return w;
}

static void parse_script(const char *p) {
    int blockstack[64], nblock = 0;

    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';') p++;
        if (!*p) break;
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (*p == '}') {
            p++;
            if (!nblock) bad("unexpected `}'");
            struct cmd *c = new_cmd();
            c->name = '}';
            cmds[blockstack[--nblock]].block_end = ncmds - 1;
            continue;
        }

        struct cmd *c = new_cmd();
        long step = 0;
        if (parse_addr(&p, &c->a1type, &c->a1line, &c->a1re, &c->a1has, &step)) {
            skip_ws(&p);
            if (*p == ',') {
                p++;
                skip_ws(&p);
                if (*p == '+') {
                    p++;
                    long v = 0;
                    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
                    c->a2type = A_PLUS; c->a2line = v;
                } else if (*p == '~') {
                    p++;
                    long v = 0;
                    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
                    c->a2type = A_MULT; c->a2line = v;
                } else if (!parse_addr(&p, &c->a2type, &c->a2line, &c->a2re,
                                       &c->a2has, NULL)) {
                    bad("expected context address");
                }
            }
        }
        if (c->a1type == A_STEP) c->a2line = step;

        skip_ws(&p);
        while (*p == '!') { c->negate = !c->negate; p++; skip_ws(&p); }
        if (!*p) bad("missing command");

        c->name = *p++;
        switch (c->name) {
        case '{':
            if (nblock >= 64) bad("blocks nested too deep");
            blockstack[nblock++] = ncmds - 1;
            break;

        case 's': {
            char delim = *p++;
            if (!delim) bad("unterminated `s' command");
            char *pat = take_delim(&p, delim, 1);
            if (!pat) bad("unterminated `s' command");
            char *rep = take_delim(&p, delim, 0);
            if (!rep) bad("unterminated `s' command");
            int icase = 0;
            for (;;) {
                if (*p == 'g') { c->gflag = 1; p++; continue; }
                if (*p == 'p') { c->pflag = 1; p++; continue; }
                if (*p == 'i' || *p == 'I') { icase = 1; p++; continue; }
                if (*p == 'm' || *p == 'M') { p++; continue; }
                if (*p >= '0' && *p <= '9') {
                    long v = 0;
                    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
                    c->occurrence = (int)v;
                    continue;
                }
                if (*p == 'w') { p++; c->wfile = take_word(&p); continue; }
                break;
            }
            if (!*pat) {
                if (!last_re_ok) bad("no previous regular expression");
                c->re = last_re;
            } else {
                if (compile_re(&c->re, pat, icase) != 0) bad("invalid regex in `s'");
                last_re = c->re; last_re_ok = 1;
            }
            c->re_ok = 1;
            c->repl = rep;
            free(pat);
            break;
        }

        case 'y': {
            char delim = *p++;
            if (!delim) bad("unterminated `y' command");
            char *from = take_delim(&p, delim, 0);
            char *to   = take_delim(&p, delim, 0);
            if (!from || !to) bad("unterminated `y' command");
            /* The lengths that must match are of the UNESCAPED strings:
             * y/abc/
	Z/ maps three characters to three, even though the
             * right-hand side is five characters of source. */
            unescape(from);
            unescape(to);
            if (strlen(from) != strlen(to))
                bad("strings for `y' command are different lengths");
            c->ytab_from = from; c->ytab_to = to;
            break;
        }

        case 'a': case 'i': case 'c':
            c->text = take_text(&p);
            break;

        case 'b': case 't': case 'T': case ':':
            c->label = take_word(&p);
            break;

        case 'r': case 'R': case 'w': case 'W':
            c->text = take_word(&p);
            break;

        case 'q': case 'Q': case 'l': {
            skip_ws(&p);
            long v = 0; int got = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); got = 1; }
            c->num = got ? (int)v : (c->name == 'l' ? 70 : 0);
            break;
        }

        case 'p': case 'P': case 'd': case 'D': case 'n': case 'N':
        case 'g': case 'G': case 'h': case 'H': case 'x': case '=':
        case 'F': case 'z':
            break;

        default: {
            char m[64];
            snprintf(m, sizeof m, "unknown command: `%c'", c->name);
            bad(m);
        }
        }
        /* Allow a trailing ; or newline. */
        skip_ws(&p);
        if (*p == ';') p++;
    }
    if (nblock) bad("unmatched `{'");

    /* Resolve b/t/T labels. */
    for (int i = 0; i < ncmds; i++) {
        struct cmd *c = &cmds[i];
        if (c->name != 'b' && c->name != 't' && c->name != 'T') continue;
        if (!c->label || !*c->label) { c->target = -1; continue; }
        int found = -1;
        for (int j = 0; j < ncmds; j++)
            if (cmds[j].name == ':' && cmds[j].label &&
                !strcmp(cmds[j].label, c->label)) { found = j; break; }
        if (found < 0) {
            fprintf(stderr, "sed: can't find label for jump to `%s'\n", c->label);
            exit(1);
        }
        c->target = found;
    }
}

/* ------------------------------------------------------------------ execute */

static struct str pattern, hold;
static long   lineno;
static int    last_line;
static int    tflag_set;            /* a substitution happened since the last t */
static int    quit_requested, quit_code;
static struct str appends;          /* text queued by `a` and `r` */

static FILE  *cur_out;

/* The separator after a line is written LAZILY, just before whatever comes
 * next. That is what lets a file whose last line has no newline come out
 * the same way: at the end the owed separator is simply dropped. Writing it
 * eagerly appended a newline the input never had. */
static int out_pending;

static void emit(const char *b, size_t n) {
    if (out_pending) { fputc(line_sep, cur_out); out_pending = 0; }
    fwrite(b, 1, n, cur_out);
}
static void emit_line(const char *b, size_t n) { emit(b, n); out_pending = 1; }

static void emit_finish(void) {
    if (out_pending && input_had_final_sep) fputc(line_sep, cur_out);
    out_pending = 0;
}

static void flush_appends(void) {
    if (!appends.len) return;
    emit(appends.p, appends.len);
    appends.len = 0;
    if (appends.p) appends.p[0] = 0;
}

/* `l` prints the pattern space unambiguously. */
static void do_list(const struct str *s, int width) {
    int col = 0;
    for (size_t i = 0; i < s->len; i++) {
        unsigned char ch = (unsigned char)s->p[i];
        char buf[8];
        int n;
        switch (ch) {
        case '\\': n = snprintf(buf, sizeof buf, "\\\\"); break;
        case '\a': n = snprintf(buf, sizeof buf, "\\a");  break;
        case '\b': n = snprintf(buf, sizeof buf, "\\b");  break;
        case '\f': n = snprintf(buf, sizeof buf, "\\f");  break;
        case '\n': n = snprintf(buf, sizeof buf, "\\n");  break;
        case '\r': n = snprintf(buf, sizeof buf, "\\r");  break;
        case '\t': n = snprintf(buf, sizeof buf, "\\t");  break;
        case '\v': n = snprintf(buf, sizeof buf, "\\v");  break;
        default:
            if (ch < 32 || ch >= 127) n = snprintf(buf, sizeof buf, "\\%03o", ch);
            else { buf[0] = (char)ch; buf[1] = 0; n = 1; }
        }
        if (width > 1 && col + n > width - 1) { emit("\\\n", 2); col = 0; }
        emit(buf, (size_t)n);
        col += n;
    }
    emit("$\n", 2);
}

/* Expand a replacement, honouring &, \1..\9 and GNU's case operators. */
static void expand_repl(struct str *out, const char *rep, const char *src,
                        regmatch_t *m, int nm)
{
    int mode = 0;      /* 0 none, 'U' 'L' upper/lower until \E */
    int once = 0;      /* 'u' or 'l' for the next character only */

    for (const char *p = rep; *p; p++) {
        const char *piece = NULL;
        size_t plen = 0;
        char one;

        if (*p == '\\' && p[1]) {
            p++;
            if (*p >= '0' && *p <= '9') {
                int g = *p - '0';
                if (g < nm && m[g].rm_so >= 0) {
                    piece = src + m[g].rm_so;
                    plen  = (size_t)(m[g].rm_eo - m[g].rm_so);
                } else { piece = ""; plen = 0; }
            } else if (*p == 'n') { one = '\n'; piece = &one; plen = 1; }
            else if (*p == 't')   { one = '\t'; piece = &one; plen = 1; }
            else if (*p == 'r')   { one = '\r'; piece = &one; plen = 1; }
            else if (*p == 'U' || *p == 'L') { mode = *p; continue; }
            else if (*p == 'E')   { mode = 0; continue; }
            else if (*p == 'u' || *p == 'l') { once = *p; continue; }
            else { one = *p; piece = &one; plen = 1; }
        } else if (*p == '&') {
            piece = src + m[0].rm_so;
            plen  = (size_t)(m[0].rm_eo - m[0].rm_so);
        } else {
            one = *p; piece = &one; plen = 1;
        }

        for (size_t i = 0; i < plen; i++) {
            char ch = piece[i];
            if (once) {
                ch = (once == 'u') ? (char)toupper((unsigned char)ch)
                                   : (char)tolower((unsigned char)ch);
                once = 0;
            } else if (mode == 'U') ch = (char)toupper((unsigned char)ch);
            else if (mode == 'L')   ch = (char)tolower((unsigned char)ch);
            str_addc(out, ch);
        }
    }
}

static int do_subst(struct cmd *c) {
    regmatch_t m[10];
    struct str out;
    memset(&out, 0, sizeof out);

    size_t pos = 0;
    int    count = 0, did = 0;

    while (pos <= pattern.len) {
        int eflags = pos ? REG_NOTBOL : 0;
        if (regexec(&c->re, pattern.p + pos, 10, m, eflags) != 0) break;

        size_t mstart = pos + (size_t)m[0].rm_so;
        size_t mend   = pos + (size_t)m[0].rm_eo;
        count++;

        int replace_this = c->gflag ? (count >= c->occurrence)
                                    : (count == c->occurrence);
        str_addn(&out, pattern.p + pos, mstart - pos);
        if (replace_this) {
            /* The group offsets are relative to pattern.p + pos. */
            regmatch_t adj[10];
            for (int i = 0; i < 10; i++) {
                adj[i].rm_so = m[i].rm_so >= 0 ? m[i].rm_so + (long)pos : -1;
                adj[i].rm_eo = m[i].rm_eo >= 0 ? m[i].rm_eo + (long)pos : -1;
            }
            expand_repl(&out, c->repl, pattern.p, adj, 10);
            did = 1;
        } else {
            str_addn(&out, pattern.p + mstart, mend - mstart);
        }

        if (mend == mstart) {
            /* An empty match must still advance, or this loops forever. */
            if (mstart < pattern.len) str_addc(&out, pattern.p[mstart]);
            pos = mstart + 1;
            if (pos > pattern.len) break;
        } else {
            pos = mend;
        }
        if (!c->gflag && count >= c->occurrence) break;
    }
    if (!did) { str_free(&out); return 0; }

    if (pos <= pattern.len) str_addn(&out, pattern.p + pos, pattern.len - pos);
    str_free(&pattern);
    pattern = out;
    return 1;
}

/* --- address matching --- */

static int addr1_matches(struct cmd *c) {
    switch (c->a1type) {
    case A_NONE: return 1;
    case A_LINE: return lineno == c->a1line;
    case A_ZERO: return 0;                    /* 0 only opens a 0,/re/ range */
    case A_LAST: return last_line;
    case A_STEP:
        if (c->a2line <= 0) return lineno == c->a1line;
        return lineno >= c->a1line && ((lineno - c->a1line) % c->a2line) == 0;
    case A_RE:
        return c->a1has && regexec(&c->a1re, pattern.p, 0, NULL, 0) == 0;
    }
    return 0;
}

static int selected(struct cmd *c) {
    int r;
    if (c->a1type == A_NONE) {
        r = 1;
    } else if (c->a2type == A_NONE) {
        r = addr1_matches(c);
    } else {
        if (!c->active) {
            /* GNU's 0,/re/ can end on the very first line. */
            int start = (c->a1type == A_ZERO) ? (lineno == 1) : addr1_matches(c);
            if (!start) { r = 0; }
            else {
                c->active = 1;
                if (c->a2type == A_PLUS)      c->range_end = lineno + c->a2line;
                else if (c->a2type == A_MULT) {
                    long n = c->a2line > 0 ? c->a2line : 1;
                    c->range_end = ((lineno + n - 1) / n) * n;
                    if (c->range_end == lineno) c->active = 0;
                } else if (c->a2type == A_LINE && c->a2line <= lineno) {
                    c->active = 0;            /* a one-line range */
                }
                if (c->a1type == A_ZERO) {
                    /* 0,/re/ tests the END address on this same line. */
                    if (c->a2type == A_RE && c->a2has &&
                        regexec(&c->a2re, pattern.p, 0, NULL, 0) == 0)
                        c->active = 0;
                }
                r = 1;
            }
        } else {
            r = 1;
            switch (c->a2type) {
            case A_LINE: if (lineno >= c->a2line) c->active = 0; break;
            case A_LAST: if (last_line) c->active = 0; break;
            case A_PLUS: case A_MULT:
                if (lineno >= c->range_end) c->active = 0;
                break;
            case A_RE:
                if (c->a2has && regexec(&c->a2re, pattern.p, 0, NULL, 0) == 0)
                    c->active = 0;
                break;
            default: c->active = 0;
            }
        }
    }
    return c->negate ? !r : r;
}

/* --- input --- */


static FILE **infiles;
static char **innames;
static int    ninfiles, curfile;
static FILE  *inf;

static int next_line(void);

/* Reads one line into `pattern`. Returns 0 at the end of all input. */
static int read_line(struct str *dst) {
    for (;;) {
        if (!inf) return 0;
        int c;
        int got = 0;
        dst->len = 0;
        if (dst->p) dst->p[0] = 0;
        int ended_with_sep = 0;
        while ((c = fgetc(inf)) != EOF) {
            got = 1;
            if (c == line_sep) { ended_with_sep = 1; break; }
            str_addc(dst, (char)c);
        }
        if (got) {
            /* Remembered so the final separator can be dropped if the input
             * never had one. */
            input_had_final_sep = ended_with_sep;
            lineno++;
            /* Peek to learn whether this was the last line of the LAST file. */
            int nc = fgetc(inf);
            if (nc == EOF) {
                int more = 0;
                if (!sflag) {
                    for (int k = curfile + 1; k < ninfiles; k++) if (infiles[k]) { more = 1; break; }
                }
                last_line = !more;
            } else {
                ungetc(nc, inf);
                last_line = 0;
            }
            return 1;
        }
        if (!next_line()) return 0;
    }
}

static int next_line(void) {
    if (inf && inf != stdin) fclose(inf);
    inf = NULL;
    while (++curfile < ninfiles) {
        if (!infiles[curfile]) continue;
        inf = infiles[curfile];
        if (sflag) lineno = 0;
        return 1;
    }
    return 0;
}

static void run_cycle(void) {
    int i = 0;
    int deleted = 0;
    tflag_set = 0;

restart:
    for (; i < ncmds; i++) {
        struct cmd *c = &cmds[i];
        if (c->name == ':' || c->name == '}') continue;

        int sel = selected(c);
        if (!sel) {
            if (c->name == '{') i = c->block_end;   /* skip the block */
            continue;
        }

        switch (c->name) {
        case '{': break;
        case '=': fprintf(cur_out, "%ld\n", lineno); break;
        case 'p': emit_line(pattern.p ? pattern.p : "", pattern.len); break;
        case 'P': {
            const char *nl = pattern.p ? memchr(pattern.p, '\n', pattern.len) : NULL;
            size_t n = nl ? (size_t)(nl - pattern.p) : pattern.len;
            emit_line(pattern.p ? pattern.p : "", n);
            break;
        }
        case 'd': deleted = 1; goto done;
        case 'D': {
            const char *nl = pattern.p ? memchr(pattern.p, '\n', pattern.len) : NULL;
            if (!nl) { deleted = 1; goto done; }
            size_t off = (size_t)(nl - pattern.p) + 1;
            memmove(pattern.p, pattern.p + off, pattern.len - off);
            pattern.len -= off;
            pattern.p[pattern.len] = 0;
            /* Restart the script WITHOUT reading a new line. */
            flush_appends();
            i = 0;
            tflag_set = 0;
            goto restart;
        }
        case 'g': str_set(&pattern, hold.p ? hold.p : "", hold.len); break;
        case 'G': str_addc(&pattern, '\n'); str_addn(&pattern, hold.p ? hold.p : "", hold.len); break;
        case 'h': str_set(&hold, pattern.p ? pattern.p : "", pattern.len); break;
        case 'H': str_addc(&hold, '\n'); str_addn(&hold, pattern.p ? pattern.p : "", pattern.len); break;
        case 'x': { struct str t = pattern; pattern = hold; hold = t; break; }
        case 'z': pattern.len = 0; if (pattern.p) pattern.p[0] = 0; break;
        case 'F': fprintf(cur_out, "%s\n",
                          (curfile < ninfiles && innames[curfile]) ? innames[curfile] : "-");
                  break;
        case 'n':
            if (!nflag) emit_line(pattern.p ? pattern.p : "", pattern.len);
            flush_appends();
            if (!read_line(&pattern)) { deleted = 1; quit_requested = 1; goto done; }
            break;
        case 'N': {
            flush_appends();
            struct str nxt;
            memset(&nxt, 0, sizeof nxt);
            if (!read_line(&nxt)) {
                str_free(&nxt);
                /* GNU prints the pattern space and stops; POSIX deletes it. */
                if (posix_mode) deleted = 1;
                quit_requested = 1;
                goto done;
            }
            str_addc(&pattern, '\n');
            str_addn(&pattern, nxt.p ? nxt.p : "", nxt.len);
            str_free(&nxt);
            break;
        }
        case 's':
            if (do_subst(c)) {
                tflag_set = 1;
                if (c->pflag) emit_line(pattern.p, pattern.len);
                if (c->wfile) {
                    if (!c->wfp) c->wfp = fopen(c->wfile, "w");
                    if (c->wfp) { fwrite(pattern.p, 1, pattern.len, c->wfp); fputc('\n', c->wfp); }
                }
            }
            break;
        case 'y': {
            size_t n = strlen(c->ytab_from);
            for (size_t k = 0; k < pattern.len; k++)
                for (size_t j = 0; j < n; j++)
                    if (pattern.p[k] == c->ytab_from[j]) { pattern.p[k] = c->ytab_to[j]; break; }
            break;
        }
        case 'a': str_addn(&appends, c->text, strlen(c->text)); str_addc(&appends, '\n'); break;
        case 'i': emit(c->text, strlen(c->text)); emit("\n", 1); break;
        case 'c':
            /* For a range, the text is printed once, at the end. */
            if (!c->active) { emit(c->text, strlen(c->text)); emit("\n", 1); }
            deleted = 1;
            goto done;
        case 'r': {
            FILE *f = fopen(c->text, "r");
            if (f) {
                char b[4096]; size_t n;
                while ((n = fread(b, 1, sizeof b, f)) > 0) str_addn(&appends, b, n);
                fclose(f);
            }
            break;
        }
        case 'R': {
            static FILE *rf; static char *rname;
            if (!rf || !rname || strcmp(rname, c->text)) {
                if (rf) fclose(rf);
                rf = fopen(c->text, "r");
                free(rname); rname = strdup(c->text);
            }
            if (rf) {
                struct str l; memset(&l, 0, sizeof l);
                int ch, any = 0;
                while ((ch = fgetc(rf)) != EOF) { any = 1; if (ch == '\n') break; str_addc(&l, (char)ch); }
                if (any) { str_addn(&appends, l.p ? l.p : "", l.len); str_addc(&appends, '\n'); }
                str_free(&l);
            }
            break;
        }
        case 'w': case 'W': {
            if (!c->wfp) {
                c->wfp = (!strcmp(c->text, "/dev/stdout")) ? stdout : fopen(c->text, "w");
            }
            if (c->wfp) {
                size_t n = pattern.len;
                if (c->name == 'W') {
                    const char *nl = pattern.p ? memchr(pattern.p, '\n', pattern.len) : NULL;
                    if (nl) n = (size_t)(nl - pattern.p);
                }
                fwrite(pattern.p ? pattern.p : "", 1, n, c->wfp);
                fputc('\n', c->wfp);
            }
            break;
        }
        case 'l': do_list(&pattern, c->num); break;
        case 'b': if (c->target < 0) goto done; i = c->target; break;
        case 't': if (tflag_set) { tflag_set = 0; if (c->target < 0) goto done; i = c->target; } break;
        case 'T': if (!tflag_set) { if (c->target < 0) goto done; i = c->target; } else tflag_set = 0; break;
        case 'q':
            if (!nflag) emit_line(pattern.p ? pattern.p : "", pattern.len);
            flush_appends();
            quit_requested = 1; quit_code = c->num; deleted = 1;
            goto done;
        case 'Q':
            quit_requested = 1; quit_code = c->num; deleted = 1;
            goto done;
        }
    }

done:
    if (!deleted && !nflag) emit_line(pattern.p ? pattern.p : "", pattern.len);
    flush_appends();
}

/* ------------------------------------------------------------------- driver */

static void reset_ranges(void) {
    for (int i = 0; i < ncmds; i++) cmds[i].active = 0;
}

static int run_over(FILE **files, char **names, int n) {
    infiles = files; innames = names; ninfiles = n;
    curfile = -1; inf = NULL; lineno = 0;
    if (!next_line()) return 0;
    while (!quit_requested && read_line(&pattern)) run_cycle();
    emit_finish();
    return 1;
}

static void usage(void) {
    fprintf(stderr,
        "usage: sed [-nrEsz] [-i[SUFFIX]] [-e script] [-f file] [script] [file ...]\n");
    exit(1);
}

int main(int argc, char **argv) {
    struct str scr;
    memset(&scr, 0, sizeof scr);
    int have_script = 0;
    /* Whether a BARE argument is the script depends on the whole command
     * line, not on what has been seen so far: `sed -n SCRIPT -e p f` gives
     * the script twice, so GNU treats the bare word as a FILE whenever any
     * -e or -f appears ANYWHERE. Scanning left to right and taking the
     * first bare word made argument ORDER change the meaning. */
    int script_from_opt = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) break;
        if (!strncmp(a, "--expression", 12) || !strncmp(a, "--file", 6))
            { script_from_opt = 1; break; }
        if (a[0] != '-' || !a[1]) continue;
        if (a[1] == '-') continue;
        for (const char *q = a + 1; *q; q++) {
            if (*q == 'e' || *q == 'f') { script_from_opt = 1; break; }
            if (*q == 'i') break;   /* -i takes the rest as its suffix */
        }
        if (script_from_opt) break;
    }
    char *files[256]; int nfiles = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--")) {
            for (int k = i + 1; k < argc && nfiles < 256; k++) files[nfiles++] = argv[k];
            break;
        }
        if (!strcmp(a, "--version")) { printf("sed (tobyOS) 1.0\n"); return 0; }
        if (!strcmp(a, "--help"))    { usage(); }
        if (!strcmp(a, "--posix"))   { posix_mode = 1; continue; }
        if (!strcmp(a, "--quiet") || !strcmp(a, "--silent")) { nflag = 1; continue; }
        if (!strncmp(a, "--expression=", 13)) {
            if (scr.len) str_addc(&scr, '\n');
            str_addn(&scr, a + 13, strlen(a + 13)); have_script = 1; continue;
        }
        if (!strncmp(a, "--in-place", 10)) {
            iflag = 1;
            if (a[10] == '=') isuffix = a + 11;
            continue;
        }
        if (!strcmp(a, "--regexp-extended")) { Eflag = 1; continue; }
        if (!strcmp(a, "--separate")) { sflag = 1; continue; }
        if (!strcmp(a, "--null-data")) { zflag = 1; continue; }

        if (a[0] == '-' && a[1]) {
            for (char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'n': nflag = 1; break;
                case 'r': case 'E': Eflag = 1; break;
                case 's': sflag = 1; break;
                case 'z': zflag = 1; break;
                case 'i':
                    iflag = 1;
                    if (p[1]) { isuffix = p + 1; p += strlen(p) - 1; }
                    break;
                case 'e': {
                    const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                    if (!v) usage();
                    if (scr.len) str_addc(&scr, '\n');
                    str_addn(&scr, v, strlen(v));
                    have_script = 1;
                    p += strlen(p) - 1;
                    break;
                }
                case 'f': {
                    const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                    if (!v) usage();
                    FILE *f = (!strcmp(v, "-")) ? stdin : fopen(v, "r");
                    if (!f) { fprintf(stderr, "sed: can't read %s\n", v); return 1; }
                    char b[4096]; size_t n;
                    if (scr.len) str_addc(&scr, '\n');
                    while ((n = fread(b, 1, sizeof b, f)) > 0) str_addn(&scr, b, n);
                    if (f != stdin) fclose(f);
                    have_script = 1;
                    p += strlen(p) - 1;
                    break;
                }
                default: usage();
                }
            }
            continue;
        }
        if (!have_script && !script_from_opt) {
            str_addn(&scr, a, strlen(a)); have_script = 1; continue;
        }
        if (nfiles < 256) files[nfiles++] = a;
    }
    if (!have_script) usage();
    if (zflag) line_sep = '\0';

    script = scr;
    parse_script(script.p ? script.p : "");

    cur_out = stdout;

    if (iflag && nfiles > 0) {
        /* In place: each file is edited on its own, into a temp file that
         * then replaces it. -s is implied, because the files are separate
         * streams by definition. */
        sflag = 1;
        for (int k = 0; k < nfiles; k++) {
            FILE *f = fopen(files[k], "r");
            if (!f) { fprintf(stderr, "sed: can't read %s\n", files[k]); exit_status = 2; continue; }
            char tmp[1088];
            snprintf(tmp, sizeof tmp, "%s.sedtmp", files[k]);
            FILE *o = fopen(tmp, "w");
            if (!o) { fclose(f); fprintf(stderr, "sed: can't create %s\n", tmp); exit_status = 4; continue; }
            cur_out = o;
            quit_requested = 0;
            reset_ranges();
            FILE *one[1]; char *onen[1];
            one[0] = f; onen[0] = files[k];
            run_over(one, onen, 1);
            fclose(o);
            if (isuffix && *isuffix) {
                char bak[1088];
                snprintf(bak, sizeof bak, "%s%s", files[k], isuffix);
                rename(files[k], bak);
            } else {
                unlink(files[k]);
            }
            rename(tmp, files[k]);
            cur_out = stdout;
        }
        return exit_status;
    }

    FILE *fps[257]; char *names[257];
    int n = 0;
    if (nfiles == 0) { fps[n] = stdin; names[n] = (char *)"-"; n++; }
    else {
        for (int k = 0; k < nfiles; k++) {
            FILE *f = (!strcmp(files[k], "-")) ? stdin : fopen(files[k], "r");
            if (!f) {
                fprintf(stderr, "sed: can't read %s: No such file or directory\n", files[k]);
                exit_status = 2;
                continue;
            }
            fps[n] = f; names[n] = files[k]; n++;
        }
    }
    if (n == 0) return exit_status;
    run_over(fps, names, n);

    for (int i = 0; i < ncmds; i++)
        if (cmds[i].wfp && cmds[i].wfp != stdout) fclose(cmds[i].wfp);
    fflush(stdout);
    return quit_code ? quit_code : exit_status;
}
