/* su_fmt.c -- fmt, as GNU coreutils 9.7's (logs/utilgate/fmt.sh): each
 * paragraph of each FILE (or standard input) refilled to lines of fewer
 * than WIDTH columns (75), as near GOAL columns as the text allows.
 *
 * Everything here was measured on gnufmt, in the C locale; nothing was
 * taken from its source.
 *
 * PARAGRAPHS. A line is blanks, an optional -p PREFIX (looked for after
 * the blanks), more blanks -- its indentation, a column, tabs at
 * multiples of 8 (a tab inside the prefix counts one) -- and its words.
 * The PREFIX's leading spaces ask that a line with text after the prefix
 * be indented at least that far (a prefix of spaces alone is those first
 * columns); its trailing spaces stand for blanks that reach as far, and
 * are not written. A line without the prefix is copied (no newline added
 * when it had none), its leading blanks -- and a refused prefix's -- set
 * anew; a line with nothing after its prefix (or only blanks) ends a
 * paragraph and is written as its prefix (or empty). A paragraph is lines
 * whose prefix column and indentation match; with -c the first line may
 * differ and the second sets the rest; with -t the second MUST differ (or
 * the first line is a paragraph alone, its rest indented as the last
 * paragraph's were); with -s every line is one. Files are formatted
 * apart. A word is a byte and what follows up to a space character:
 * \r \v \f after a word start the next one, with no gap; NUL counts as a
 * closing mark, and as an opening one at a word's start.
 *
 * SPACING. Between words on a line the input's gap is kept as a number
 * of columns; a line's last word is followed by one space, or two when
 * it ends a sentence -- [.?!] then any of )]'" at the end of an input
 * line or before a gap of two or more columns. -u: one space, two after a
 * sentence. Once a tab has been read in a file's indentation or text,
 * output gaps use tabs: one gap crossing tab stops is written as tabs up
 * to the last stop, then spaces -- unless only one column would be left
 * before that stop (from column 7 to 15 is eight spaces).
 *
 * FILLING. Each line costs (in hundredths of a column squared, all
 * integers) 4900 plus 100*(length - GOAL)^2 plus, when the next line is
 * not the paragraph's last, 50*(length - next length)^2; the last line
 * costs nothing. A break after a word adds: -2500 after a sentence end;
 * 360000 after a [.?!] word that does not end one (one space after it);
 * -1600 after any other word ending in punctuation; else 40000/(n+2)
 * after the first word (n bytes) of a sentence that is not the
 * paragraph's first; and -1600 when the next word starts with ( [ ` ' ",
 * else 22500/(n+2) when the next word (n bytes) is the last of its
 * sentence or of the paragraph. A line is shorter than WIDTH unless it is
 * one word. Lines are chosen from the paragraph's end backward: for each
 * word, the cheapest line starting there, given the lines already chosen
 * after it, the shorter on a tie. These constants were found by sweeping
 * GOAL over designed paragraphs and checked on thousands of random ones.
 *
 * A paragraph of more than 998 words or 5000 bytes is formatted in parts
 * (see fm_fmt_flush). GOAL is WIDTH*187/200 when only WIDTH is given;
 * WIDTH is GOAL+10 when only GOAL is. -WIDTH is the old form, only as the
 * first argument. */

#define FM_FMT_LINE_COST 4900LL
#define FM_FMT_SENT_BONUS (-2500LL)
#define FM_FMT_ABBR_COST 360000LL
#define FM_FMT_PUNCT_BONUS (-1600LL)
#define FM_FMT_PAREN_BONUS (-1600LL)
#define FM_FMT_FIRST_NUM 40000LL
#define FM_FMT_LAST_NUM 22500LL
#define FM_FMT_MAXWIDTH 2500
#define FM_FMT_MAXWORDS 998
#define FM_FMT_MAXCHARS 5000

struct fm_fmt_word {
    size_t off, len;        /* bytes in the paragraph's text buffer */
    long space;             /* columns after it on a line */
    unsigned char eol;      /* last word of its input line */
    unsigned char period;   /* ends a sentence */
    unsigned char abbr;     /* [.?!] word that does not */
    unsigned char punct;    /* ends in other punctuation */
    unsigned char paren;    /* starts with an opening mark */
    unsigned char final;    /* last word of a sentence or the paragraph */
    long long best;         /* cost of the lines from here to the end */
    size_t next;            /* first word of the next line */
    long linelen;           /* length of the line starting here */
};

/* one input line and what its start says */
struct fm_fmt_line {
    char *s;
    size_t n, cap;
    int nl;                 /* ended in a newline */
    int kind;               /* FM_FMT_TEXT ... */
    long lead;              /* column of the prefix */
    long indent;            /* column of the first word */
    size_t text;            /* offset of the first word */
};
enum { FM_FMT_TEXT, FM_FMT_BLANK, FM_FMT_VERBATIM, FM_FMT_REFUSED, FM_FMT_NOTHING };

struct fm_fmt_in {
    int fd, eof, err;
    size_t pos, end;
    unsigned char buf[16384];
};

static struct {
    long width, goal;
    int crown, tagged, split, uniform;
    const char *prefix;
    long pk;                /* the prefix's leading spaces: a line must be
                             * indented at least this far to have it */
    size_t plen;            /* the rest of it, matched */
    size_t pout;            /* and written: trailing spaces dropped */
    int pmode;              /* -p was given something */
    int tabs;               /* a tab was read in this file */
    char *text;
    size_t tlen, tcap;
    struct fm_fmt_word *w;
    size_t nw, wcap;
    long first, lead;
    long other;             /* indent of a paragraph's later lines; -t
                             * remembers it from one paragraph to the next */
    long col;               /* output column */
    int cont;               /* the paragraph's first line is out */
    long prevlen;           /* then: the length of the last line written */
} fm_fmt;
static void fm_fmt_flush(int partial);

static void fm_fmt_nomem(void) { su_error(1, 0, "memory exhausted"); }

/* A file name quoted as GNU's messages quote it (C locale, shell style,
 * always quoted): 'NAME'; "NAME" when it holds a single quote and
 * otherwise only letters, digits, blanks, "%+,-./:@]_" (and '#' or '~'
 * first); else '...' with a quote as '\'' and control or non-ASCII bytes
 * as '$'\ooo'' runs. Measured byte by byte. */
static const char *fm_fmt_quoteaf(const char *s) {
    static char *buf;
    size_t n = strlen(s);
    int squote = 0, dq_ok = 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\'') squote = 1;
        else if (!(isalnum(c) || (c && strchr(" %+,-./:@]_", c)) ||
                   ((c == '#' || c == '~') && i == 0)))
            dq_ok = 0;
    }
    free(buf);
    buf = (char *)malloc(n * 7 + 8);
    if (!buf) fm_fmt_nomem();
    char *o = buf;
    if (squote && dq_ok) {
        *o++ = '"';
        memcpy(o, s, n);
        o += n;
        *o++ = '"';
        *o = 0;
        return buf;
    }
    int dollar = 0;
    *o++ = '\'';
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c >= 0x7f) {
            if (!dollar) { memcpy(o, "'$'", 3); o += 3; dollar = 1; }
            if (c >= 7 && c <= 13) { *o++ = '\\'; *o++ = "abtnvfr"[c - 7]; }
            else {
                *o++ = '\\';
                *o++ = (char)('0' + ((c >> 6) & 3));
                *o++ = (char)('0' + ((c >> 3) & 7));
                *o++ = (char)('0' + (c & 7));
            }
            continue;
        }
        if (c == '\'') { memcpy(o, "'\\''", 4); o += 4; dollar = 0; continue; }
        if (dollar) { memcpy(o, "''", 2); o += 2; dollar = 0; }
        *o++ = (char)c;
    }
    *o++ = '\'';
    *o = 0;
    return buf;
}

static long fm_fmt_adv(long col, unsigned char c) {
    return c == '\t' ? (col / 8 + 1) * 8 : col + 1;
}

static int fm_fmt_blank(unsigned char c) { return c == ' ' || c == '\t'; }

/* ---- input ------------------------------------------------------------- */

static int fm_fmt_getc(struct fm_fmt_in *in) {
    if (in->pos == in->end) {
        if (in->eof || in->err) return EOF;
        ssize_t r;
        do r = read(in->fd, in->buf, sizeof in->buf);
        while (r < 0 && errno == EINTR);
        if (r < 0) { in->err = 1; return EOF; }
        if (r == 0) { in->eof = 1; return EOF; }
        in->pos = 0;
        in->end = (size_t)r;
    }
    return in->buf[in->pos++];
}

/* the next line into L; 0 at the end of the input */
static int fm_fmt_read(struct fm_fmt_in *in, struct fm_fmt_line *l) {
    l->n = 0;
    l->nl = 0;
    int c, any = 0;
    while ((c = fm_fmt_getc(in)) != EOF) {
        any = 1;
        if (c == '\n') { l->nl = 1; break; }
        if (l->n + 1 >= l->cap) {
            size_t nc = l->cap ? l->cap * 2 : 256;
            char *ns = (char *)realloc(l->s, nc);
            if (!ns) fm_fmt_nomem();
            l->s = ns;
            l->cap = nc;
        }
        l->s[l->n++] = (char)c;
    }
    return any;
}

/* what kind of line L is; scanning its leading blanks notes a tab */
static void fm_fmt_parse(struct fm_fmt_line *l) {
    size_t i = 0;
    long col = 0;
    const unsigned char *s = (const unsigned char *)l->s;
    l->lead = 0;
    if (fm_fmt.pmode) {
        while (i < l->n && fm_fmt_blank(s[i])) {
            if (s[i] == '\t') fm_fmt.tabs = 1;
            col = fm_fmt_adv(col, s[i++]);
        }
        if (i == l->n) {
            /* blanks alone: an empty line -- or, unterminated, nothing;
             * but under a prefix of spaces alone, blanks reaching past it
             * are a line of that prefix (measured: under -p '  ', a line
             * of two spaces is empty, of three is "  ") */
            if (!fm_fmt.plen && col > fm_fmt.pk) {
                l->kind = FM_FMT_BLANK;
                l->lead = fm_fmt.pk;
                return;
            }
            l->kind = l->nl ? FM_FMT_BLANK : FM_FMT_NOTHING;
            l->lead = -1;
            return;
        }
        /* copied when not prefixed -- its leading blanks written anew */
        l->kind = FM_FMT_VERBATIM;
        l->indent = col;
        l->text = i;
        if (!fm_fmt.plen) {
            if (col < fm_fmt.pk) return;
            /* spaces alone: the prefix is the first PK columns */
            l->kind = FM_FMT_TEXT;
            l->lead = fm_fmt.pk;
            return;
        }
        if (l->n - i < fm_fmt.pout || memcmp(s + i, fm_fmt.prefix, fm_fmt.pout) != 0)
            return;
        /* the prefix is as wide as it is long (a tab in it is one column:
         * "#<tab>x<tab>x" under -p '#<tab>' is 9 columns), and its
         * trailing spaces stand for any blanks that reach as far, or for
         * none at the end of the line: under -p '#  ', "#<tab>a" is
         * prefixed and "# a" is not. Measured. */
        long lead = col;
        col += (long)fm_fmt.pout;
        i += fm_fmt.pout;
        while (i < l->n && fm_fmt_blank(s[i])) {
            if (s[i] == '\t') fm_fmt.tabs = 1;
            col = fm_fmt_adv(col, s[i++]);
        }
        /* nothing after the prefix: its line, however it is indented;
         * text after it needs the prefix's leading spaces' indentation */
        if (i == l->n) { l->kind = FM_FMT_BLANK; l->lead = lead; return; }
        l->lead = lead;
        if (lead < fm_fmt.pk || col < lead + (long)fm_fmt.plen) {
            /* refused after the prefix was read: written as the prefix,
             * blanks anew, then the rest as it is (measured: "# <tab> y"
             * under -p ' #' comes out "#<tab> y") */
            l->kind = FM_FMT_REFUSED;
            l->indent = col;
            l->text = i;
            return;
        }
    } else {
        while (i < l->n && fm_fmt_blank(s[i])) {
            if (s[i] == '\t') fm_fmt.tabs = 1;
            col = fm_fmt_adv(col, s[i++]);
        }
        if (i == l->n) {
            l->kind = FM_FMT_BLANK;
            l->lead = -1;
            return;
        }
    }
    l->kind = FM_FMT_TEXT;
    l->indent = col;
    l->text = i;
}

/* ---- output ------------------------------------------------------------ */

/* blanks from the output column to TARGET: tabs to the last stop before
 * it when tabs were read, unless that stop is only one column away */
static void fm_fmt_ws(long target) {
    if (fm_fmt.tabs) {
        long stop = target / 8 * 8;
        if (fm_fmt.col + 1 < stop)
            while (fm_fmt.col < stop) {
                putchar('\t');
                fm_fmt.col = (fm_fmt.col / 8 + 1) * 8;
            }
    }
    while (fm_fmt.col < target) { putchar(' '); fm_fmt.col++; }
}

/* the blanks and prefix that start an output line, up to INDENT */
static void fm_fmt_start(long lead, long indent) {
    fm_fmt.col = 0;
    if (fm_fmt.pmode && lead >= 0) {
        fm_fmt_ws(lead);
        fwrite(fm_fmt.prefix, 1, fm_fmt.pout, stdout);
        fm_fmt.col += (long)fm_fmt.pout;
    }
    if (indent >= 0) fm_fmt_ws(indent);
}

/* ---- a paragraph ------------------------------------------------------- */

static void fm_fmt_add(const struct fm_fmt_line *l) {
    const unsigned char *s = (const unsigned char *)l->s;
    size_t i = l->text;
    long col = l->indent;
    while (i < l->n) {
        /* a word is its first byte and what follows up to any space
         * character; only blanks are skipped between words, so a \v \f
         * or \r after a word starts the next one (measured: "a\vb" is
         * "a" and "\vb" with no gap, and fmt may break between them) */
        size_t st = i++;
        while (i < l->n && !(s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))) i++;
        size_t len = i - st;
        col += (long)len;
        long c2 = col;
        while (i < l->n && fm_fmt_blank(s[i])) {
            if (s[i] == '\t') fm_fmt.tabs = 1;
            c2 = fm_fmt_adv(c2, s[i++]);
        }
        /* GNU holds at most 998 words or 5000 bytes of a paragraph */
        if (fm_fmt.nw >= FM_FMT_MAXWORDS || fm_fmt.tlen + len > FM_FMT_MAXCHARS) fm_fmt_flush(1);
        if (fm_fmt.nw == fm_fmt.wcap) {
            size_t nc = fm_fmt.wcap ? fm_fmt.wcap * 2 : 64;
            struct fm_fmt_word *nw = (struct fm_fmt_word *)realloc(fm_fmt.w, nc * sizeof *nw);
            if (!nw) fm_fmt_nomem();
            fm_fmt.w = nw;
            fm_fmt.wcap = nc;
        }
        while (fm_fmt.tlen + len > fm_fmt.tcap) {
            size_t nc = fm_fmt.tcap ? fm_fmt.tcap * 2 : 1024;
            char *nt = (char *)realloc(fm_fmt.text, nc);
            if (!nt) fm_fmt_nomem();
            fm_fmt.text = nt;
            fm_fmt.tcap = nc;
        }
        struct fm_fmt_word *w = &fm_fmt.w[fm_fmt.nw++];
        memset(w, 0, sizeof *w);
        w->off = fm_fmt.tlen;
        w->len = len;
        memcpy(fm_fmt.text + fm_fmt.tlen, s + st, len);
        fm_fmt.tlen += len;
        w->space = c2 - col;
        w->eol = i >= l->n;
        col = c2;
    }
}

/* what each word is, now that the paragraph is whole */
static void fm_fmt_classify(void) {
    size_t n = fm_fmt.nw;
    for (size_t i = 0; i < n; i++) {
        struct fm_fmt_word *w = &fm_fmt.w[i];
        const unsigned char *t = (const unsigned char *)fm_fmt.text + w->off;
        /* closing marks, and NUL with them, are looked past; a word of
         * nothing else ends a sentence when it starts with NUL, and a word
         * starting with NUL opens like '(' -- measured, as GNU's tests of
         * these sets answer for a NUL byte */
        size_t k = w->len;
        while (k > 0 && (t[k - 1] == 0 || strchr(")]'\"", t[k - 1]))) k--;
        int per = k > 0 ? (t[k - 1] == '.' || t[k - 1] == '?' || t[k - 1] == '!') : t[0] == 0;
        w->period = per && (w->eol || w->space >= 2);
        w->abbr = per && !w->period;
        unsigned char last = t[w->len - 1];
        w->punct = !per && last < 0x80 && ispunct(last);
        w->paren = t[0] == 0 || strchr("([`'\"", t[0]) != 0;
        w->final = w->period || i + 1 == n;
        if (fm_fmt.uniform || w->eol) w->space = w->period ? 2 : 1;
    }
}

/* the cost of a break between words K and K+1 */
static long long fm_fmt_brk(size_t k) {
    const struct fm_fmt_word *w = fm_fmt.w;
    long long c = 0;
    if (w[k].period) c += FM_FMT_SENT_BONUS;
    else if (w[k].abbr) c += FM_FMT_ABBR_COST;
    else if (w[k].punct) c += FM_FMT_PUNCT_BONUS;
    else if (k > 0 && w[k - 1].period) c += FM_FMT_FIRST_NUM / ((long long)w[k].len + 2);
    if (w[k + 1].paren) c += FM_FMT_PAREN_BONUS;
    else if (w[k + 1].final) c += FM_FMT_LAST_NUM / ((long long)w[k + 1].len + 2);
    return c;
}

static void fm_fmt_choose(void) {
    struct fm_fmt_word *w = fm_fmt.w;
    size_t n = fm_fmt.nw;
    long maxlen = fm_fmt.width - 1;
    for (size_t i = n; i-- > 0;) {
        long len = (i == 0 && !fm_fmt.cont ? fm_fmt.first : fm_fmt.other) + (long)w[i].len;
        long long best = 0;
        size_t bj = n;
        long blen = len;
        int have = 0;
        for (size_t j = i + 1;; j++) {
            /* the line is words i .. j-1, LEN columns */
            long long c;
            if (j == n) {
                c = 0;
            } else {
                long long d = len - fm_fmt.goal;
                c = FM_FMT_LINE_COST + 100 * d * d + fm_fmt_brk(j - 1) + w[j].best;
                if (w[j].next < n) {
                    long long r = len - w[j].linelen;
                    c += 50 * r * r;
                }
            }
            if (i == 0 && fm_fmt.cont) {
                /* the rest of a long paragraph: its first line is ragged
                 * against the last one written (measured) */
                long long r = len - fm_fmt.prevlen;
                c += 50 * r * r;
            }
            if (!have || c < best) { best = c; bj = j; blen = len; have = 1; }
            if (j == n) break;
            len += w[j - 1].space + (long)w[j].len;
            if (len > maxlen) break;
        }
        w[i].best = best;
        w[i].next = bj;
        w[i].linelen = blen;
    }
}

/* Write the paragraph's lines. PARTIAL: the buffer is full and the
 * paragraph goes on -- what has been read is formatted as if it ended
 * there, and the lines are written up to the last break that earned a
 * bonus (the lowest break cost, the later on a tie), or else all but the
 * last line (all of a single line); the words after it start the rest. Measured on paragraphs of
 * thousands of words; GNU's choice of that point is not always matched
 * when sentence ends and other punctuation are mixed (see the report). */
static void fm_fmt_flush(int partial) {
    if (!fm_fmt.nw) return;
    fm_fmt_classify();
    fm_fmt_choose();
    struct fm_fmt_word *w = fm_fmt.w;
    size_t n = fm_fmt.nw;
    size_t stop = n;
    if (partial) {
        long long bestb = 0;
        size_t q = 0, bq = 0;
        for (; w[q].next < n; q = w[q].next) {
            long long b = fm_fmt_brk(w[q].next - 1);
            if (b < 0 && b <= bestb) { bestb = b; bq = w[q].next; }
        }
        /* a buffer that is one line is written whole (measured at -w 2500) */
        stop = bq ? bq : q ? q : n;
    }
    size_t i = 0;
    while (i < stop) {
        size_t e = w[i].next;
        fm_fmt_start(fm_fmt.lead, i == 0 && !fm_fmt.cont ? fm_fmt.first : fm_fmt.other);
        for (size_t k = i; k < e; k++) {
            fwrite(fm_fmt.text + w[k].off, 1, w[k].len, stdout);
            fm_fmt.col += (long)w[k].len;
            if (k + 1 < e) fm_fmt_ws(fm_fmt.col + w[k].space);
        }
        putchar('\n');
        fm_fmt.prevlen = w[i].linelen;
        i = e;
    }
    if (!partial) {
        fm_fmt.nw = 0;
        fm_fmt.tlen = 0;
        fm_fmt.cont = 0;
        return;
    }
    if (i > 0) fm_fmt.cont = 1;
    if (i == n) {
        fm_fmt.nw = 0;
        fm_fmt.tlen = 0;
        return;
    }
    size_t base = w[i].off;
    memmove(fm_fmt.text, fm_fmt.text + base, fm_fmt.tlen - base);
    fm_fmt.tlen -= base;
    for (size_t k = i; k < n; k++) {
        w[k - i] = w[k];
        w[k - i].off -= base;
    }
    fm_fmt.nw = n - i;
}

/* a line that is no paragraph's: copied, or its prefix alone */
static void fm_fmt_other_line(const struct fm_fmt_line *l) {
    if (l->kind == FM_FMT_VERBATIM || l->kind == FM_FMT_REFUSED) {
        if (l->kind == FM_FMT_REFUSED) fm_fmt_start(l->lead, l->indent);
        else {
            fm_fmt.col = 0;
            fm_fmt_ws(l->indent);
        }
        fwrite(l->s + l->text, 1, l->n - l->text, stdout);
        if (l->nl) putchar('\n');
    } else if (l->kind == FM_FMT_BLANK) {
        if (l->lead >= 0) fm_fmt_start(l->lead, -1);
        putchar('\n');
    }
}

/* the lines of a paragraph after its first continue it when this holds */
static int fm_fmt_more(const struct fm_fmt_line *l, long indent) {
    return l->kind == FM_FMT_TEXT && l->lead == fm_fmt.lead && l->indent == indent;
}

/* 0, or 1 when the file could not be read */
static int fm_fmt_file(int fd) {
    static struct fm_fmt_in in;
    static struct fm_fmt_line a, b;
    struct fm_fmt_line *cur = &a, *nxt = &b, *t;
    in.fd = fd;
    in.eof = in.err = 0;
    in.pos = in.end = 0;
    fm_fmt.tabs = 0;
    fm_fmt.other = 0;
    int have = fm_fmt_read(&in, cur);
    if (have) fm_fmt_parse(cur);
    while (have) {
        if (cur->kind != FM_FMT_TEXT) {
            fm_fmt_other_line(cur);
            have = fm_fmt_read(&in, cur);
            if (have) fm_fmt_parse(cur);
            continue;
        }
        fm_fmt.lead = cur->lead;
        fm_fmt.first = cur->indent;
        fm_fmt_add(cur);
        int hn = fm_fmt_read(&in, nxt);
        if (hn) fm_fmt_parse(nxt);
        if (fm_fmt.split) {
            fm_fmt.other = fm_fmt.first;
        } else if (fm_fmt.crown || fm_fmt.tagged) {
            if (hn && nxt->kind == FM_FMT_TEXT && nxt->lead == fm_fmt.lead &&
                (!fm_fmt.tagged || nxt->indent != fm_fmt.first)) {
                fm_fmt.other = nxt->indent;
                fm_fmt_add(nxt);
                hn = fm_fmt_read(&in, nxt);
                if (hn) fm_fmt_parse(nxt);
                while (hn && fm_fmt_more(nxt, fm_fmt.other)) {
                    fm_fmt_add(nxt);
                    hn = fm_fmt_read(&in, nxt);
                    if (hn) fm_fmt_parse(nxt);
                }
            } else if (fm_fmt.tagged) {
                /* a tagged line alone keeps the last paragraph's indent
                 * for the rest (0 at a file's start) -- unless that is
                 * its own; then 3 at the margin, else 0. Measured. */
                if (fm_fmt.other == fm_fmt.first) fm_fmt.other = fm_fmt.first == 0 ? 3 : 0;
            } else {
                fm_fmt.other = fm_fmt.first;
            }
        } else {
            fm_fmt.other = fm_fmt.first;
            while (hn && fm_fmt_more(nxt, fm_fmt.first)) {
                fm_fmt_add(nxt);
                hn = fm_fmt_read(&in, nxt);
                if (hn) fm_fmt_parse(nxt);
            }
        }
        fm_fmt_flush(0);
        t = cur; cur = nxt; nxt = t;
        have = hn;
    }
    fm_fmt_flush(0);
    if (in.err) {
        su_error(0, 0, "read error");
        return 1;
    }
    return 0;
}

/* WIDTH and GOAL: decimal, blanks and '+' first allowed, at most MAX;
 * past it ERRNUM (ERANGE for a width, EOVERFLOW for a goal) */
static long fm_fmt_num(const char *s, long max, int errnum) {
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) su_error(1, 0, "invalid width: %s", su_quote(s));
    uintmax_t v = 0;
    int over = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINTMAX_MAX - d) / 10) over = 1;
        else v = v * 10 + d;
    }
    if (*p) su_error(1, 0, "invalid width: %s", su_quote(s));
    if (over || v > (uintmax_t)max) su_error(1, errnum, "invalid width: %s", su_quote(s));
    return (long)v;
}

static int do_fmt(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "crown-margin", SU_NO_ARG, 0, 'c' },
        { "prefix", SU_REQ_ARG, 0, 'p' },
        { "split-only", SU_NO_ARG, 0, 's' },
        { "tagged-paragraph", SU_NO_ARG, 0, 't' },
        { "uniform-spacing", SU_NO_ARG, 0, 'u' },
        { "width", SU_REQ_ARG, 0, 'w' },
        { "goal", SU_REQ_ARG, 0, 'g' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&fm_fmt, 0, sizeof fm_fmt);
    fm_fmt.prefix = "";
    const char *wstr = 0, *gstr = 0;
    /* -WIDTH, only as the first argument */
    if (argc > 1 && argv[1][0] == '-' && isdigit((unsigned char)argv[1][1])) {
        wstr = argv[1] + 1;
        argv[1] = argv[0];
        argv++;
        argc--;
    }
    int c;
    while ((c = su_getopt_long(argc, argv, "0123456789cstup:w:g:", longs, 0)) != -1) {
        switch (c) {
        case 'c': fm_fmt.crown = 1; break;
        case 's': fm_fmt.split = 1; break;
        case 't': fm_fmt.tagged = 1; break;
        case 'u': fm_fmt.uniform = 1; break;
        case 'p': fm_fmt.prefix = su_optarg; break;
        case 'w': wstr = su_optarg; break;
        case 'g': gstr = su_optarg; break;
        case SU_HELP_OPT:
            printf("Usage: %s [-WIDTH] [OPTION]... [FILE]...\n"
                   "Refill the paragraphs of each FILE, or of standard input, to standard\n"
                   "output. -WIDTH, as the first argument only, is a short way to say\n"
                   "--width=WIDTH.\n"
                   "\n"
                   "Without a FILE, or with -, standard input is read.\n"
                   "\n"
                   "An argument a long option requires is required by its short form too.\n"
                   "  -c, --crown-margin        keep the first two lines' indentation; the\n"
                   "                              rest follow the second\n"
                   "  -p, --prefix=STRING       refill only the lines that start with STRING,\n"
                   "                              and put it back in front of each one\n"
                   "  -s, --split-only          break lines that are too long; join none\n"
                   "  -t, --tagged-paragraph    like -c, but a first line indented as the\n"
                   "                              second is a paragraph of its own\n"
                   "  -u, --uniform-spacing     one space between words, two after a sentence\n"
                   "  -w, --width=WIDTH         lines narrower than WIDTH columns (75)\n"
                   "  -g, --goal=WIDTH          aim for lines this wide (93%% of the width)\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default:
            if (c >= '0' && c <= '9') {
                fflush(stdout);
                fprintf(stderr, "%s: invalid option -- %c; -WIDTH is recognized only when it is the first\n"
                        "option; use -w N instead\n", su_short, c);
            }
            su_try_help(1);
        }
    }
    {
        /* the PREFIX's leading spaces ask for that much indentation (a
         * line's blanks are skipped before the rest is looked for); its
         * trailing spaces are matched but not written -- tabs are neither,
         * measured */
        const char *p = fm_fmt.prefix;
        while (*p == ' ') p++;
        fm_fmt.pk = (long)(p - fm_fmt.prefix);
        fm_fmt.pmode = *fm_fmt.prefix != 0;
        fm_fmt.prefix = p;
        fm_fmt.plen = strlen(p);
        fm_fmt.pout = fm_fmt.plen;
        while (fm_fmt.pout > 0 && p[fm_fmt.pout - 1] == ' ') fm_fmt.pout--;
    }
    /* -c with -t is -c */
    if (fm_fmt.crown) fm_fmt.tagged = 0;
    fm_fmt.width = 75;
    if (wstr) fm_fmt.width = fm_fmt_num(wstr, FM_FMT_MAXWIDTH, ERANGE);
    if (gstr) {
        fm_fmt.goal = fm_fmt_num(gstr, fm_fmt.width, EOVERFLOW);
        if (!wstr) fm_fmt.width = fm_fmt.goal + 10;
    } else {
        fm_fmt.goal = fm_fmt.width * 187 / 200;
    }
    int rc = 0;
    if (su_optind >= argc) {
        rc |= fm_fmt_file(0);
    } else {
        for (int i = su_optind; i < argc; i++) {
            const char *name = argv[i];
            if (!strcmp(name, "-")) { rc |= fm_fmt_file(0); continue; }
            int fd = su_open_read(name, 0);
            if (fd < 0) {
                su_error(0, errno, "cannot open %s for reading", fm_fmt_quoteaf(name));
                rc = 1;
                continue;
            }
            rc |= fm_fmt_file(fd);
            close(fd);
        }
    }
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return rc;
}
