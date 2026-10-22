/* su_pr.c -- pr, as GNU coreutils 9.7's (logs/utilgate/staging/pr.sh):
 * each FILE (or standard input; - is standard input, one reader however
 * often it is named) cut into pages with a header and a trailer, in one
 * column or several, or all the FILEs side by side (-m). Everything here
 * was measured against gnupr in WSL, in the C locale (and differentially
 * fuzzed against it); nothing was read from its source.
 *
 * THE PAGE. 66 lines (-l), of which 5 are the header -- two blank lines,
 * "DATE  HEADER  Page N", two blank lines -- and 5 the trailer (blank
 * lines; with -F/-f one form feed instead, no padding before it), so 56
 * lines of text. -t (and -T, and any -l of 10 or less) drops header,
 * trailer and padding, and the page is then all text: -t -l 20 pages every
 * 20 lines. -d prints a blank line after each line that a newline ended,
 * so a page holds half as many (and pads to twice that). The header line
 * is WIDTH (-w/-W, 72) columns: the date, then HEADER (-h, else the file's
 * name, else nothing) with half the spare room on its left -- rounded
 * down, at least one space each side -- then "Page N". -o MARGIN indents
 * the text and the header. The date is the file's modification time (now
 * for standard input and -m) as -D FORMAT has it (date's conversions), or
 * "%Y-%m-%d %H:%M" ("%b %e %H:%M %Y" when POSIXLY_CORRECT is set).
 *
 * FORM FEEDS in the input end a page (a line holding one ends there; a
 * newline right after the form feed is swallowed); one met just when a
 * page has filled is swallowed, no empty page following; two in a row
 * make an empty page. With -t a page that a form feed ended is followed
 * by one; -T drops them (but a page of 10 lines or fewer is -t's even
 * then). An empty input prints no page at all, and "starting page number
 * N exceeds page count M" counts the page a filled page started.
 *
 * COLUMNS. -N (or --columns=N) prints N columns, down each (balanced: the
 * first L%N columns of a short page get one line more), or across with -a;
 * -m puts each FILE in a column of its own, a file whose page ended (form
 * feed) or whose text ended leaving its column blank while any file is
 * still open. Columns are (WIDTH - (N-1) * separator length) / N wide --
 * "page width too narrow" when that is 0 -- the separator a space, the
 * text cut to the column and padded with tabs and spaces ("output tabs",
 * -i, are always on with columns, and input tabs are expanded, -e, unless
 * a TAB separates the columns). -J, or -s without -w/-W, neither cuts nor
 * pads: the separator is then a TAB (-s CHAR's CHAR; with -w/-W, -s alone
 * is no character at all; -S STRING always wins). A single column is cut
 * only with -W (-m with one file: -w or -W).
 *
 * NUMBERS (-n[SEP[DIGITS]], TAB and 5): DIGITS digits, right-aligned (the
 * last DIGITS digits of a larger number), then SEP; a TAB separator is a
 * real TAB in one column and spaces to the next multiple of 8 in columns,
 * where every column is numbered (with -m only the line is). -N NUMBER
 * numbers the first line of the first page printed; else the count starts
 * at the file's first line, pages skipped by +FIRST[:LAST] (--pages)
 * counting too (with -m: the first file's lines, and one for its end).
 *
 * WHAT A CHARACTER IS. -e[CHAR[WIDTH]] turns CHAR (TAB) into spaces to the
 * next multiple of WIDTH (8) -- and a TAB still to the next multiple of 8
 * when CHAR is another; -i[CHAR[WIDTH]] writes runs of spaces that reach a
 * multiple of WIDTH as CHAR, a run's last stop reached by one space alone
 * staying a space, and trailing spaces not at all. -c shows a control
 * character as ^X (^? for DEL) and a byte past 0x7f as \ooo, -v every one
 * of them as \ooo. Otherwise a control character is written as it is, as
 * wide as nothing; a backspace is one column back, and is dropped at the
 * start of a line; a TAB not expanded moves the input column but not the
 * count of the output's (so the spaces after it tab as if it were not
 * there -- measured).
 *
 * Where a cut line is measured matters, and is GNU's as measured: the
 * first character of a line is measured from the column's start before
 * the number is (and when even that is wider than the column, the line
 * prints nothing at all and takes no number -- GNU loops forever on such
 * a line in one column when it falls at a page's end; this does not);
 * after it the number's width counts as part of the line. With the text
 * not cut the number is not counted, and uncut columns count positions
 * from their own start (for output tabs), as GNU's do.
 *
 * Errors are GNU's words: a file that cannot be opened is reported (not
 * with -r) and skipped, status 1; a read error ends pr there.
 *
 * The old pr took -h, -t and -lN, one column, one file, no date, and no
 * trailer or padding. */

/* ---- settings --------------------------------------------------------------- */

static struct {
    int page_len;            /* -l */
    int width;               /* -w / -W, chars per line */
    int w_given, W_given;
    int cols;                /* -COLUMN, --columns */
    int across, merge, join;
    int s_given, s_char;     /* -s[CHAR]: s_char -1 when no CHAR */
    int S_given;
    const char *S_str;       /* -S[STRING] */
    int e_given, in_ch, in_w;
    int i_given, out_ch, out_w;
    int number, num_sep, num_digits;
    int N_given, N_first;
    int margin;
    int dbl;
    int omit_hdr, omit_pag, t_given;
    int ff;                  /* -F / -f */
    const char *datefmt;
    const char *header;      /* -h, or 0 */
    uintmax_t first_page, last_page;
    int range_given;
    int ctl;                 /* 0, 1 -c, 2 -v */
    int nowarn;              /* -r */
} fm_pr;

/* derived */
static int fm_pr_ncols;      /* columns on the page (files with -m) */
static int fm_pr_cw;         /* one column's width */
static int fm_pr_trunc;      /* cut and pad columns */
static int fm_pr_expand, fm_pr_tabify;
static int fm_pr_nw;         /* the number's width */
static const char *fm_pr_sep;
static int fm_pr_body;       /* text lines on a page */
static int fm_pr_lpc;        /* input lines in a column of a page */
static int fm_pr_status;

/* ---- output ------------------------------------------------------------------
 *
 * Spaces are not written when they are made: they wait (fm_pr_pend) for
 * the next character, which is written where they lead -- as tabs, with
 * output tabs on -- so a line's trailing spaces vanish then. The count is
 * signed: padding to a column that text has already run past is
 * negative, and the spaces that follow fill the debt before any is
 * written (measured: -w 10 -2 -n). fm_pr_op is the column the output is
 * at. */

static int fm_pr_op, fm_pr_pend;

static void fm_pr_move(void) {
    if (fm_pr_pend <= 0) return;
    int p = fm_pr_op, q = p + fm_pr_pend;
    if (fm_pr_tabify) {
        for (;;) {
            int t = (p / fm_pr.out_w + 1) * fm_pr.out_w;
            if (t > q) break;
            /* the last stop, reached by one space alone: a space */
            if (t == q && t - p == 1) break;
            putchar(fm_pr.out_ch);
            p = t;
        }
    }
    for (; p < q; p++) putchar(' ');
    fm_pr_op = q;
    fm_pr_pend = 0;
}

static void fm_pr_spaces(int n) {
    fm_pr_pend += n;
    if (!fm_pr_tabify) fm_pr_move();
}

/* bytes S (N of them), WIDTH columns wide */
static void fm_pr_put(const char *s, int n, int width) {
    fm_pr_move();
    fwrite(s, 1, (size_t)n, stdout);
    fm_pr_op += width;
}

static void fm_pr_putc(int c) {
    char ch = (char)c;
    fm_pr_put(&ch, 1, 1);
}

static void fm_pr_newline(void) {
    fm_pr_pend = 0;
    putchar('\n');
    fm_pr_op = 0;
}

/* ---- input -------------------------------------------------------------------
 *
 * A file read through a buffer of its own; standard input is one reader
 * however often - is named (-m - f - deals its lines out in turn).
 *
 * A reader is OPEN, ON HOLD (a form feed ended its page: it waits for the
 * next one) or DONE. -m looks for the end of every file before each row
 * (it leaves no row of empty columns after the longest file), not after
 * each line; a page that filled starts the next one, whatever follows
 * ("starting page number 4 exceeds page count 3" for 4 lines at -l 2). */

#define FM_PR_OPEN 0
#define FM_PR_HOLD 1
#define FM_PR_DONE 2

struct fm_pr_in {
    int fd;
    const char *name;        /* as messages quote it */
    unsigned char buf[16384];
    size_t pos, len;
    int eof;
    int st;
    int shared;              /* the standard input reader */
};

static struct fm_pr_in *fm_pr_stdin;

static int fm_pr_getc(struct fm_pr_in *in) {
    if (in->pos == in->len) {
        if (in->eof) return EOF;
        for (;;) {
            ssize_t r = read(in->fd, in->buf, sizeof in->buf);
            if (r < 0 && errno == EINTR) continue;
            /* a read error ends pr there, -r or not (measured: "pr dir f"
             * prints nothing of f) */
            if (r < 0) su_error(1, errno, "%s", in->name);
            if (r == 0) { in->eof = 1; return EOF; }
            in->pos = 0;
            in->len = (size_t)r;
            break;
        }
    }
    return in->buf[in->pos++];
}

static int fm_pr_peek(struct fm_pr_in *in) {
    int c = fm_pr_getc(in);
    if (c != EOF) in->pos--;
    return c;
}

/* one line */
struct fm_pr_line {
    unsigned char *s;
    size_t n, cap;
    int term;                /* what ended it: '\n', '\f' or EOF */
};

/* what the last read ended on -- '\n', '\f' or EOF: -d's blank line after
 * the last row of a page depends on it (measured: none when the page's
 * last read met the end of the file, or a form feed) */
static int fm_pr_last;

static void fm_pr_addc(struct fm_pr_line *l, int c) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->s = (unsigned char *)realloc(l->s, l->cap);
        if (!l->s) su_error(1, ENOMEM, "memory exhausted");
    }
    l->s[l->n++] = (unsigned char)c;
}

/* a form feed: the page ends, and a newline right after it is swallowed */
static void fm_pr_hold(struct fm_pr_in *in) {
    if (fm_pr_peek(in) == '\n') fm_pr_getc(in);
    in->st = FM_PR_HOLD;
}

/* the next line of an OPEN reader into L: 1, or 0 when there was none
 * (the file ended, or a form feed began the line) */
static int fm_pr_get_line(struct fm_pr_in *in, struct fm_pr_line *l) {
    l->n = 0;
    int c = fm_pr_getc(in);
    l->term = c;
    fm_pr_last = c;
    if (c == EOF) { in->st = FM_PR_DONE; return 0; }
    if (c == '\f') { fm_pr_hold(in); return 0; }
    for (;;) {
        if (c == '\n') break;
        if (c == '\f') { fm_pr_hold(in); break; }
        fm_pr_addc(l, c);
        c = fm_pr_getc(in);
        if (c == EOF) { in->st = FM_PR_DONE; break; }
    }
    l->term = c;
    fm_pr_last = c;
    return 1;
}

/* a page that filled swallows a form feed that comes next: no empty page
 * follows, and -t writes no form feed for it */
static void fm_pr_after_full(struct fm_pr_in *in) {
    if (in->st == FM_PR_OPEN && fm_pr_peek(in) == '\f') {
        fm_pr_getc(in);
        if (fm_pr_peek(in) == '\n') fm_pr_getc(in);
    }
}

/* -m looks for the end of each file before each row */
static void fm_pr_check_eof(struct fm_pr_in *in) {
    if (in->st == FM_PR_OPEN && fm_pr_peek(in) == EOF) in->st = FM_PR_DONE;
}

/* ---- characters -------------------------------------------------------------- */

/* C at input column IP as written: its bytes in OUT (or spaces when
 * *SPACES), and how wide it is; -2 when it is dropped */
static int fm_pr_clump(int c, int ip, char *out, int *len, int *spaces) {
    *spaces = 0;
    *len = 0;
    if (fm_pr_expand && c == fm_pr.in_ch) {
        int w = fm_pr.in_w - ip % fm_pr.in_w;
        *spaces = 1;
        *len = w;
        return w;
    }
    if (c == '\t') {
        int w = 8 - ip % 8;
        if (fm_pr_expand) { *spaces = 1; *len = w; return w; }
        out[0] = '\t';
        *len = 1;
        *spaces = 2;             /* written, but the output column stays */
        return w;
    }
    if (c == ' ') { *spaces = 1; *len = 1; return 1; }
    int nonprint = c < 0x20 || c >= 0x7f;
    if (nonprint && fm_pr.ctl == 2) {
        *len = snprintf(out, 8, "\\%03o", c);
        return 4;
    }
    if (nonprint && fm_pr.ctl == 1) {
        if (c >= 0x80) { *len = snprintf(out, 8, "\\%03o", c); return 4; }
        out[0] = '^';
        out[1] = c == 0x7f ? '?' : (char)(c + '@');
        *len = 2;
        return 2;
    }
    out[0] = (char)c;
    *len = 1;
    if (c == '\b') return ip > 0 ? -1 : -2;
    if (nonprint) return 0;
    return 1;
}

/* the number field for line N */
static void fm_pr_put_number(intmax_t n, int multi) {
    char b[64];
    int k = snprintf(b, sizeof b, "%*jd", fm_pr.num_digits, n);
    const char *d = b;
    if (k > fm_pr.num_digits) d = b + (k - fm_pr.num_digits);
    for (; *d == ' '; d++) fm_pr_spaces(1);
    for (; *d; d++) fm_pr_putc(*d);
    if (fm_pr.num_sep == '\t') {
        if (multi) fm_pr_spaces(fm_pr_nw - fm_pr.num_digits);
        else {
            /* a real TAB, counted to the next output tab stop */
            fm_pr_move();
            putchar('\t');
            fm_pr_op = (fm_pr_op / fm_pr.out_w + 1) * fm_pr.out_w;
        }
    } else if (fm_pr.num_sep == ' ') {
        fm_pr_spaces(1);
    } else {
        fm_pr_putc(fm_pr.num_sep);
    }
}

/* the margin: in one column it waits like other spaces (so an empty
 * line has none, and spaces that follow join it in one run of tabs);
 * before columns -- and with -S, measured -- it is written at once, a
 * run of its own */
static void fm_pr_put_margin(int multi) {
    if (!fm_pr.margin) return;
    fm_pr_spaces(fm_pr.margin);
    if (multi || fm_pr.S_given) fm_pr_move();
}

/* a line whose first character is wider than a cut column (measured at
 * the column's start, before any number) prints nothing at all: GNU drops
 * it, and gives it no number */
static int fm_pr_vanishes(const struct fm_pr_line *l) {
    char out[8];
    int len, spaces;
    return fm_pr_trunc && l->n && fm_pr_clump(l->s[0], 0, out, &len, &spaces) > fm_pr_cw;
}

/* LINE as text of width CW. WITH_NUMBER: the number NUM first (and the
 * margin before it when MARGIN). Returns the input column it ended at, or
 * -1 when it vanishes (nothing was written). */
static int fm_pr_cell_op;       /* where the output was when a column began */

static int fm_pr_text(const struct fm_pr_line *l, int cw, int margin, int multi,
                      int with_number, intmax_t num) {
    int ip = 0;
    char out[8];
    int len, spaces;
    /* a column's line -- even an empty one, or one that vanishes -- first
     * writes the spaces that lead to it, as a run of their own */
    if (multi) fm_pr_move();
    fm_pr_cell_op = fm_pr_op;
    if (fm_pr_vanishes(l)) return -1;
    if (margin) fm_pr_put_margin(multi);
    if (with_number) {
        fm_pr_put_number(num, multi);
        if (fm_pr_trunc) ip += fm_pr_nw;
    }
    for (size_t i = 0; i < l->n; i++) {
        int c = l->s[i];
        /* the first character is as wide as it is at the column's start
         * (a backspace there is dropped); the rest count the number */
        int w = fm_pr_clump(c, i == 0 ? 0 : ip, out, &len, &spaces);
        if (w == -2) continue;
        if (i > 0 && fm_pr_trunc && ip + w > cw) break;
        if (spaces == 1) fm_pr_spaces(len);
        else fm_pr_put(out, len, spaces == 2 ? 0 : w);
        ip += w;
    }
    return ip;
}

/* ---- the header and the trailer ------------------------------------------------ */

static char *fm_pr_date_str;     /* the current file's date */
static const char *fm_pr_hdr_str; /* its header text */

static void fm_pr_set_date(int fd, int use_mtime) {
    int64_t s;
    long ns;
    tx_now(&s, &ns);
    if (use_mtime) {
        struct stat st;
        if (fstat(fd, &st) == 0) {
            s = (int64_t)st.st_mtime;
#ifdef SU_HOST
            ns = st.st_mtim.tv_nsec;
#else
            ns = 0;
#endif
        }
    }
    const char *fmt = fm_pr.datefmt;
    if (!fmt) fmt = getenv("POSIXLY_CORRECT") ? "%b %e %H:%M %Y" : "%Y-%m-%d %H:%M";
    free(fm_pr_date_str);
    fm_pr_date_str = tx_strftime(fmt, s, ns, 0);
}

static void fm_pr_print_header(uintmax_t page) {
    char pg[64];
    snprintf(pg, sizeof pg, "Page %ju", page);
    /* the first blank line carries the margin, as other text does */
    fm_pr_put_margin(0);
    fm_pr_move();
    fm_pr_newline();
    fm_pr_newline();
    for (int i = 0; i < fm_pr.margin; i++) putchar(' ');
    long dl = (long)strlen(fm_pr_date_str), hl = (long)strlen(fm_pr_hdr_str);
    long pl = (long)strlen(pg);
    long room = (long)fm_pr.width - dl - hl - pl;
    long lhs = room / 2, rhs = room - lhs;
    if (lhs < 1) lhs = 1;
    if (rhs < 1) rhs = 1;
    fputs(fm_pr_date_str, stdout);
    for (long i = 0; i < lhs; i++) putchar(' ');
    fputs(fm_pr_hdr_str, stdout);
    for (long i = 0; i < rhs; i++) putchar(' ');
    fputs(pg, stdout);
    fm_pr_newline();
    fm_pr_newline();
    fm_pr_newline();
}

/* the end of a page that took USED of the body's lines; FULL: it held
 * all it could, EMPTY: nothing */
static void fm_pr_print_trailer(int used, int ended_by_ff, int full, int empty) {
    if (fm_pr.omit_hdr) {
        if (ended_by_ff && !fm_pr.omit_pag) putchar('\f');
        return;
    }
    if (fm_pr.ff) {
        /* measured: an empty page has a blank line before its form feed,
         * and with -d a page that did not fill its lines one more */
        (void)full;
        if (fm_pr.dbl && used < 2 * fm_pr_lpc) putchar('\n');
        if (empty) putchar('\n');
        putchar('\f');
        return;
    }
    /* -d pads to twice the lines a page holds -- one short of an odd
     * body, and past a body of 1 -- measured */
    int body = fm_pr.dbl ? 2 * fm_pr_lpc : fm_pr_body;
    for (int i = used; i < body; i++) putchar('\n');
    for (int i = 0; i < 5; i++) putchar('\n');
}

/* ---- pages ------------------------------------------------------------------
 *
 * A page is gathered first -- its lines, in columns -- and then printed,
 * or only counted when it is before FIRST. */

static struct fm_pr_line *fm_pr_lines;   /* a page's lines, in order */
static intmax_t *fm_pr_nums;
static size_t fm_pr_caplines;

static void fm_pr_room(size_t n) {
    if (n < fm_pr_caplines) return;
    size_t nc = fm_pr_caplines ? fm_pr_caplines * 2 : 64;
    while (nc <= n) nc *= 2;
    fm_pr_lines = (struct fm_pr_line *)realloc(fm_pr_lines, nc * sizeof *fm_pr_lines);
    fm_pr_nums = (intmax_t *)realloc(fm_pr_nums, nc * sizeof *fm_pr_nums);
    if (!fm_pr_lines || !fm_pr_nums) su_error(1, ENOMEM, "memory exhausted");
    memset(fm_pr_lines + fm_pr_caplines, 0, (nc - fm_pr_caplines) * sizeof *fm_pr_lines);
    fm_pr_caplines = nc;
}

static intmax_t fm_pr_lineno;
static int fm_pr_seen_print;     /* a page was printed (for -N) */
static int fm_pr_rows_out;       /* lines of the body written */

/* a row's end; DBL: -d's blank line after it */
static void fm_pr_end_row(int dbl) {
    fm_pr_newline();
    fm_pr_rows_out++;
    if (fm_pr.dbl && dbl) { putchar('\n'); fm_pr_rows_out++; }
}

/* where the output is, as pr counts it, once column C's text ended at
 * input column IP (EMPTY: it had none): cut columns stand where they
 * belong (MERGE_NW: -m's number before the first); uncut ones count from
 * nothing, as if each began at the line's start (the first after the
 * margin) -- measured: -J's third column tabs as if it began at the
 * second's width, whatever the first's */
static int fm_pr_reset_op = -1; /* where an empty column's count goes back to */

static void fm_pr_col_done(int c, int ip, int merge_nw, int empty) {
    if (fm_pr_trunc) {
        fm_pr_op = fm_pr.margin + merge_nw + c * (fm_pr_cw + (int)strlen(fm_pr_sep))
                   + ip - fm_pr_pend;
        return;
    }
    if (fm_pr.merge || fm_pr.across) return;   /* -m and -a count on */
    if (empty) {
        /* an empty line puts the count back where its column began --
         * once the separator after it is out (its number's spaces go out
         * with that, or not at all at the line's end) */
        fm_pr_reset_op = fm_pr_cell_op;
        return;
    }
    /* likewise once the separator is out: the spaces a column ends in go
     * out as counted where they are, and are not counted after */
    fm_pr_reset_op = (c == 0 ? fm_pr.margin : 0) + ip - fm_pr_pend;
}

/* what goes between two columns: the padding of the one before (cut
 * columns are filled out to their width) and the separator -- which a
 * column whose line vanishes does without (SEP 0) */
static void fm_pr_between(int prev_ip, int sep) {
    if (fm_pr_trunc) fm_pr_spaces(fm_pr_cw - prev_ip);
    if (sep) {
        for (const char *p = fm_pr_sep; *p; p++) {
            if (*p == ' ') fm_pr_spaces(1);
            else fm_pr_putc(*p);
        }
    }
    if (fm_pr_reset_op >= 0) {
        fm_pr_op = fm_pr_reset_op + (sep ? (int)strlen(fm_pr_sep) : 0) - fm_pr_pend;
        fm_pr_reset_op = -1;
    }
}

/* the N lines gathered, as page PAGE */
static void fm_pr_print_page(size_t n, uintmax_t page, int ended_ff, int full) {
    int C = fm_pr_ncols;
    if (!fm_pr.omit_hdr) fm_pr_print_header(page);
    fm_pr_rows_out = 0;
    if (C == 1) {
        for (size_t i = 0; i < n; i++) {
            fm_pr_op = 0;
            fm_pr_pend = 0;
            int ip = fm_pr_text(&fm_pr_lines[i], fm_pr_cw, 1, 0, fm_pr.number, fm_pr_lineno);
            if (ip < 0) continue;            /* nothing of it fitted */
            if (fm_pr.number) fm_pr_lineno++;
            /* one column: a blank line after a line that a newline ended */
            fm_pr_end_row(fm_pr_lines[i].term == '\n');
        }
    } else {
        size_t *lens = (size_t *)malloc((size_t)C * sizeof *lens);
        size_t *ix = (size_t *)malloc((size_t)C * sizeof *ix);
        if (!lens || !ix) su_error(1, ENOMEM, "memory exhausted");
        size_t rows;
        if (fm_pr.across) {
            rows = (n + (size_t)C - 1) / (size_t)C;
        } else {
            /* balanced: the first n % C columns one line longer */
            size_t base = n / (size_t)C, extra = n % (size_t)C;
            for (int c = 0; c < C; c++) lens[c] = base + ((size_t)c < extra ? 1 : 0);
            rows = lens[0];
        }
        /* numbers in reading order, none for a line that vanishes */
        for (size_t i = 0; i < n; i++) {
            fm_pr_nums[i] = fm_pr_lineno;
            if (fm_pr.number && !fm_pr_vanishes(&fm_pr_lines[i])) fm_pr_lineno++;
        }
        for (size_t r = 0; r < rows; r++) {
            fm_pr_op = 0;
            fm_pr_pend = 0;
            int k = 0;
            if (fm_pr.across) {
                for (int c = 0; c < C; c++) {
                    size_t i = r * (size_t)C + (size_t)c;
                    if (i >= n) break;
                    ix[k++] = i;
                }
            } else {
                size_t start = 0;
                for (int c = 0; c < C && r < lens[c]; c++) {
                    ix[k++] = start + r;
                    start += lens[c];
                }
            }
            fm_pr_put_margin(1);
            fm_pr_reset_op = -1;
            int ip = 0;
            for (int c = 0; c < k; c++) {
                const struct fm_pr_line *l = &fm_pr_lines[ix[c]];
                /* across, a line that vanishes takes the separator before
                 * it too; down, it does not -- measured */
                if (c > 0) fm_pr_between(ip, !(fm_pr.across && fm_pr_vanishes(l)));
                ip = fm_pr_text(l, fm_pr_cw, 0, 1, fm_pr.number, fm_pr_nums[ix[c]]);
                if (ip < 0) ip = 0;
                fm_pr_col_done(c, ip, 0, l->n == 0);
            }
            /* -d's blank after every row but a page's last; that one has
             * it when the last read (across: of the row, which is read
             * as it is printed; down: of the page, read first) met a
             * newline -- measured */
            int last_dbl = fm_pr.across
                ? k == C && fm_pr_lines[ix[k - 1]].term == '\n'
                : fm_pr_last == '\n';
            fm_pr_end_row(r + 1 < rows || last_dbl);
        }
        free(lens);
        free(ix);
    }
    fm_pr_print_trailer(fm_pr_rows_out, ended_ff, full, n == 0);
}

/* ---- the files ------------------------------------------------------------- */

static const char *fm_pr_qname(const char *name) {
    /* GNU quotes a name holding a colon in these messages too */
    return strchr(name, ':') ? su_quoteaf(name) : su_quotef(name);
}

static struct fm_pr_in *fm_pr_open(const char *name) {
    if (!strcmp(name, "-")) {
        if (!fm_pr_stdin) {
            fm_pr_stdin = (struct fm_pr_in *)calloc(1, sizeof *fm_pr_stdin);
            if (!fm_pr_stdin) su_error(1, ENOMEM, "memory exhausted");
            fm_pr_stdin->fd = 0;
            fm_pr_stdin->name = "-";
            fm_pr_stdin->shared = 1;
        }
        return fm_pr_stdin;
    }
    int fd = su_open_read(name, 0);
    if (fd < 0) {
        int e = errno;
        if (!fm_pr.nowarn) su_error(0, e, "%s", fm_pr_qname(name));
        fm_pr_status = 1;
        return 0;
    }
    struct fm_pr_in *in = (struct fm_pr_in *)calloc(1, sizeof *in);
    if (!in) su_error(1, ENOMEM, "memory exhausted");
    in->fd = fd;
    in->name = strdup(fm_pr_qname(name));
    return in;
}

static void fm_pr_close(struct fm_pr_in *in) {
    if (!in || in->shared) return;
    close(in->fd);
    free((void *)in->name);
    free(in);
}

static void fm_pr_count_warn(uintmax_t count) {
    if (count && fm_pr.first_page > count)
        su_error(0, 0, "starting page number %ju exceeds page count %ju",
                 fm_pr.first_page, count);
}

/* the page about to be gathered: printed or not, and -N's restart */
static int fm_pr_page_start(uintmax_t page) {
    int printing = page >= fm_pr.first_page && page <= fm_pr.last_page;
    if (printing && !fm_pr_seen_print) {
        fm_pr_seen_print = 1;
        if (fm_pr.N_given) fm_pr_lineno = fm_pr.N_first;
    }
    return printing;
}

static void fm_pr_one_file(const char *name) {
    struct fm_pr_in *in = fm_pr_open(name);
    if (!in) return;
    int is_stdin = in->shared;
    fm_pr_set_date(in->fd, !is_stdin);
    fm_pr_hdr_str = fm_pr.header ? fm_pr.header : is_stdin ? "" : name;
    fm_pr_lineno = 1;
    fm_pr_seen_print = 0;
    size_t want = (size_t)fm_pr_lpc * (size_t)fm_pr_ncols;
    uintmax_t page = 1, count = 0;
    for (;;) {
        if (in->st == FM_PR_HOLD) in->st = FM_PR_OPEN;
        if (in->st == FM_PR_DONE) { count = page; break; }
        int printing = fm_pr_page_start(page);
        size_t n = 0;
        while (n < want && in->st == FM_PR_OPEN) {
            fm_pr_room(n);
            if (fm_pr_get_line(in, &fm_pr_lines[n])) n++;
        }
        if (n == want) fm_pr_after_full(in);
        if (n == 0 && in->st == FM_PR_DONE) { count = page; break; }
        if (printing) {
            fm_pr_print_page(n, page, in->st == FM_PR_HOLD, n == want);
        } else if (fm_pr.number) {
            for (size_t i = 0; i < n; i++)
                if (!fm_pr_vanishes(&fm_pr_lines[i])) fm_pr_lineno++;
        }
        if (page >= fm_pr.last_page) break;
        /* done: no page follows -- unless the page filled first, the end
         * unseen (a last line with no newline is the end seen) */
        if (in->st == FM_PR_DONE) { count = page; break; }
        page++;
    }
    fm_pr_count_warn(count);
    fm_pr_close(in);
}

/* ---- -m: the files side by side --------------------------------------------
 *
 * A row is each file's next line, in order. A file with no line for it
 * (done, or waiting for the next page) still has its column -- padding
 * and separator -- as long as some file is still open when its turn
 * comes; once none is, the row ends there (measured: "a2" alone when a's
 * line ended in a form feed and every later file is done). */

static void fm_pr_merge(char **names, int n) {
    struct fm_pr_in **ins = (struct fm_pr_in **)calloc((size_t)n + 1, sizeof *ins);
    struct fm_pr_line *lines = (struct fm_pr_line *)calloc((size_t)n + 1, sizeof *lines);
    int *have = (int *)calloc((size_t)n + 1, sizeof *have);
    int *show = (int *)calloc((size_t)n + 1, sizeof *show);
    if (!ins || !lines || !have || !show) su_error(1, ENOMEM, "memory exhausted");
    int k = 0;
    for (int i = 0; i < n; i++) {
        struct fm_pr_in *in = fm_pr_open(names[i]);
        if (in) ins[k++] = in;
    }
    if (k == 0) goto out;
    fm_pr_set_date(0, 0);
    fm_pr_hdr_str = fm_pr.header ? fm_pr.header : "";
    fm_pr_lineno = 1;
    fm_pr_seen_print = 0;
    uintmax_t page = 1, count = 0;
    for (;;) {
        int open = 0;
        for (int i = 0; i < k; i++) if (ins[i]->st == FM_PR_HOLD) ins[i]->st = FM_PR_OPEN;
        int first_open = ins[0]->st == FM_PR_OPEN;
        for (int i = 0; i < k; i++) fm_pr_check_eof(ins[i]);
        for (int i = 0; i < k; i++) if (ins[i]->st == FM_PR_OPEN) open = 1;
        if (!open) { count = page; break; }
        int printing = fm_pr_page_start(page);
        int rows = 0, header_done = 0;
        fm_pr_rows_out = 0;
        /* skipped pages count the first file's lines (and its end) */
        if (!printing && fm_pr.number && first_open && ins[0]->st == FM_PR_DONE) {
            fm_pr_lineno++;
            first_open = 0;
        }
        while (rows < fm_pr_lpc) {
            int any = 0;
            for (int i = 0; i < k; i++) fm_pr_check_eof(ins[i]);
            for (int i = 0; i < k; i++) {
                have[i] = 0;
                if (ins[i]->st == FM_PR_OPEN) have[i] = fm_pr_get_line(ins[i], &lines[i]);
                if (have[i]) any = 1;
                show[i] = have[i];
                if (!have[i])
                    for (int j = 0; j < k; j++) if (ins[j]->st == FM_PR_OPEN) show[i] = 1;
            }
            if (!printing && fm_pr.number) {
                if (have[0]) fm_pr_lineno++;
                if (first_open && ins[0]->st == FM_PR_DONE) { fm_pr_lineno++; first_open = 0; }
            }
            if (!any) break;
            int content = 0;
            for (int i = 0; i < k; i++) if (have[i] && !fm_pr_vanishes(&lines[i])) content = 1;
            if (!content) continue;      /* every line of it vanished */
            rows++;
            if (!printing) continue;
            if (!header_done) {
                header_done = 1;
                if (!fm_pr.omit_hdr) fm_pr_print_header(page);
            }
            fm_pr_op = 0;
            fm_pr_pend = 0;
            fm_pr_put_margin(fm_pr_ncols > 1);
            if (fm_pr.number) {
                fm_pr_put_number(fm_pr_lineno, fm_pr_ncols > 1);
                fm_pr_lineno++;
            }
            int ip = 0;
            for (int i = 0; i < k; i++) {
                if (!show[i]) break;
                /* uncut, the number's spaces are lost when the first column
                 * is empty and the second is not -- measured */
                if (i == 1 && !fm_pr_trunc && have[1] && !have[0]) fm_pr_pend = 0;
                if (i > 0) fm_pr_between(ip, !(have[i] && fm_pr_vanishes(&lines[i])));
                ip = 0;
                if (have[i] && i == k - 1 && fm_pr_vanishes(&lines[i])) {
                    ;                    /* a last column that vanishes: nothing */
                } else if (have[i]) {
                    ip = fm_pr_text(&lines[i], fm_pr_cw, 0, fm_pr_ncols > 1, 0, 0);
                    if (ip < 0) ip = 0;
                } else if (i > 0 && fm_pr_ncols > 1) {
                    fm_pr_move();        /* an empty column is written out */
                }
                if (fm_pr_ncols > 1) fm_pr_col_done(i, ip, fm_pr.number ? fm_pr_nw : 0, !have[i]);
            }
            /* -m: a blank after every row -- of one file, as one column */
            fm_pr_end_row(fm_pr_ncols > 1 || lines[0].term == '\n');
        }
        if (rows == fm_pr_lpc)
            for (int i = 0; i < k; i++) fm_pr_after_full(ins[i]);
        int held = 0, open_now = 0;
        for (int i = 0; i < k; i++) {
            if (ins[i]->st == FM_PR_HOLD) held = 1;
            if (ins[i]->st == FM_PR_OPEN) open_now = 1;
        }
        if (printing && (rows || held)) {
            if (!header_done && !fm_pr.omit_hdr) fm_pr_print_header(page);
            fm_pr_print_trailer(fm_pr_rows_out, held, rows == fm_pr_lpc, rows == 0);
        }
        if (!rows && !held) { count = page; break; }
        if (page >= fm_pr.last_page) break;
        if (!held && !open_now) { count = page; break; }
        page++;
    }
    fm_pr_count_warn(count);
    for (int i = 0; i < k; i++) fm_pr_close(ins[i]);
out:
    for (int i = 0; i < n; i++) free(lines[i].s);
    free(ins);
    free(lines);
    free(have);
    free(show);
}

/* ---- the options --------------------------------------------------------------- */

/* GNU's number checks: "'-l PAGE_LENGTH' invalid number of lines: 'x'",
 * with ": Numerical result out of range" below MIN and ": Value too large
 * for defined data type" past INT_MAX (or below INT_MIN) */
static int fm_pr_int(const char *arg, long min, const char *what) {
    const char *p = arg;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '-' || *p == '+') { neg = *p == '-'; p++; }
    if (!isdigit((unsigned char)*p)) su_error(1, 0, "%s: %s", what, su_quote(arg));
    uintmax_t v = 0;
    int over = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINTMAX_MAX - d) / 10) over = 1;
        else v = v * 10 + d;
    }
    if (*p) su_error(1, 0, "%s: %s", what, su_quote(arg));
    /* below the least allowed: out of range when the least is 1 */
    if (min == 1 && (neg || v == 0))
        su_error(1, ERANGE, "%s: %s", what, su_quote(arg));
    if (min == 0 && neg && v != 0)
        su_error(1, EOVERFLOW, "%s: %s", what, su_quote(arg));
    if (over || (!neg && v > (uintmax_t)INT_MAX) || (neg && v > (uintmax_t)INT_MAX + 1))
        su_error(1, EOVERFLOW, "%s: %s", what, su_quote(arg));
    return neg ? (int)(-(intmax_t)v) : (int)v;
}

/* -e -i -n's optional CHAR[NUMBER]: 0, or -1 when ARG is bad */
static void fm_pr_charnum(const char *opt, const char *arg, int *ch, int *num) {
    if (!arg) return;
    if (*arg && !isdigit((unsigned char)*arg)) *ch = (unsigned char)*arg++;
    if (*arg) {
        const char *p = arg;
        uintmax_t v = 0;
        int ok = 1;
        if (!isdigit((unsigned char)*p)) ok = 0;
        for (; ok && isdigit((unsigned char)*p); p++) {
            v = v * 10 + (unsigned)(*p - '0');
            if (v > (uintmax_t)INT_MAX) ok = 0;
        }
        if (*p || v == 0) ok = 0;
        if (!ok) {
            fflush(stdout);
            fprintf(stderr, "%s: '-%s' extra characters or invalid number in the argument: %s\n",
                    su_short, opt, su_quote(arg));
            su_try_help(1);
        }
        *num = (int)v;
    }
}

/* +FIRST[:LAST] or --pages's: 1, or 0 when it is not a page range (a '+'
 * argument is then a file) -- the other faults are fatal */
static int fm_pr_range(const char *arg, const char *what) {
    const char *p = arg;
    uintmax_t a, b = UINTMAX_MAX;
    char *end;
    int rc = su_strtoumax(p, &end, &a, 0);
    if (rc == SU_NUM_INVALID) su_error(1, 0, "invalid %s argument %s", what, su_quote(arg));
    if (rc == SU_NUM_OVERFLOW) su_error(1, 0, "%s argument %s too large", what, su_quote(arg));
    int bad = 0;
    if (*end == ':') {
        const char *q = end + 1;
        rc = su_strtoumax(q, &end, &b, 0);
        if (rc == SU_NUM_INVALID) su_error(1, 0, "invalid %s argument %s", what, su_quote(arg));
        if (rc == SU_NUM_OVERFLOW) su_error(1, 0, "%s argument %s too large", what, su_quote(arg));
        if (*end) su_error(1, 0, "invalid suffix in %s argument %s", what, su_quote(arg));
    } else if (*end) {
        bad = 1;
    }
    if (bad || a == 0 || b < a) {
        if (what[0] == '+') return 0;
        su_error(1, 0, "invalid page range %s", su_quote(arg));
    }
    fm_pr.first_page = a;
    fm_pr.last_page = b;
    fm_pr.range_given = 1;
    return 1;
}

static void fm_pr_usage(void) {
    printf("Usage: %s [OPTION]... [FILE]...\n", su_name);
    fputs("Lay out each FILE in pages for printing: a header and a trailer on each\n"
          "page, the text in one column or several.\n"
          "\n"
          "Standard input is read when there is no FILE, and for a FILE of -.\n"
          "\n"
          "An option's argument is the same for its long and short forms.\n"
          "  +FIRST_PAGE[:LAST_PAGE], --pages=FIRST_PAGE[:LAST_PAGE]\n"
          "                    print from page FIRST_PAGE (to page LAST_PAGE)\n"
          "  -COLUMN, --columns=COLUMN\n"
          "                    lay the text out in COLUMN columns, filled downwards\n"
          "                    and evened out at the end (across with -a)\n"
          "  -a, --across      fill the columns across, line by line\n"
          "  -c, --show-control-chars\n"
          "                    show control characters as ^X, other bytes as \\ooo\n"
          "  -d, --double-space\n"
          "                    a blank line after every line\n"
          "  -D, --date-format=FORMAT\n"
          "                    write the header's date as date +FORMAT would\n"
          "  -e[CHAR[WIDTH]], --expand-tabs[=CHAR[WIDTH]]\n"
          "                    turn CHAR (TAB) in the input into spaces, stops every\n"
          "                    WIDTH (8) columns\n"
          "  -F, -f, --form-feed\n"
          "                    end each page with a form feed rather than blank\n"
          "                    lines\n"
          "  -h, --header=HEADER\n"
          "                    put HEADER in the middle of the header, not the file's\n"
          "                    name (-h \"\" for none; -h\"\" is not that)\n"
          "  -i[CHAR[WIDTH]], --output-tabs[=CHAR[WIDTH]]\n"
          "                    write runs of spaces as CHAR (TAB), stops every WIDTH\n"
          "                    (8) columns\n"
          "  -J, --join-lines  with columns, neither cut nor pad lines; separate the\n"
          "                    columns with the --sep-string STRING (TAB)\n"
          "  -l, --length=PAGE_LENGTH\n"
          "                    pages of PAGE_LENGTH (66) lines, 56 of them text; one\n"
          "                    of 10 or fewer has no header (as -t)\n"
          "  -m, --merge       every FILE side by side, a column each, lines cut to\n"
          "                    fit (unless -J)\n"
          "  -n[SEP[DIGITS]], --number-lines[=SEP[DIGITS]]\n"
          "                    number the lines with DIGITS (5) digits and then SEP\n"
          "                    (TAB), counting from the input's first line\n"
          "  -N, --first-line-number=NUMBER\n"
          "                    number the first line of the first page printed\n"
          "                    NUMBER (see +FIRST_PAGE)\n"
          "  -o, --indent=MARGIN\n"
          "                    indent every line MARGIN (0) spaces, on top of the\n"
          "                    page width\n"
          "  -r, --no-file-warnings\n"
          "                    say nothing of a FILE that cannot be opened\n"
          "  -s[CHAR], --separator[=CHAR]\n"
          "                    separate columns with the one character CHAR (TAB,\n"
          "                    or none with -w), and neither cut nor pad lines\n"
          "                    unless -w is given\n"
          "  -S[STRING], --sep-string[=STRING]\n"
          "                    separate columns with STRING (a space; a TAB with -J);\n"
          "                    -S alone is the empty string\n"
          "  -t, --omit-header  no header, no trailer, no padding of short pages;\n"
          "                    as with a PAGE_LENGTH of 10 or fewer\n"
          "  -T, --omit-pagination\n"
          "                    as -t, and the input's form feeds dropped too\n"
          "  -v, --show-nonprinting\n"
          "                    show nonprinting bytes as \\ooo\n"
          "  -w, --width=PAGE_WIDTH\n"
          "                    PAGE_WIDTH (72) columns to a page, for columns only;\n"
          "                    -s turns the default off\n"
          "  -W, --page-width=PAGE_WIDTH\n"
          "                    PAGE_WIDTH (72) columns to a page, lines cut to fit\n"
          "                    (unless -J), with or without columns\n"
          "      --help        show this help and exit\n"
          "      --version     show the version and exit\n", stdout);
}

static int do_pr(int argc, char **argv) {
    enum { PAGES_OPT = 0x100, COLUMNS_OPT };
    static const struct su_option longs[] = {
        { "pages", SU_REQ_ARG, 0, PAGES_OPT },
        { "columns", SU_REQ_ARG, 0, COLUMNS_OPT },
        { "across", SU_NO_ARG, 0, 'a' },
        { "show-control-chars", SU_NO_ARG, 0, 'c' },
        { "double-space", SU_NO_ARG, 0, 'd' },
        { "date-format", SU_REQ_ARG, 0, 'D' },
        { "expand-tabs", SU_OPT_ARG, 0, 'e' },
        { "form-feed", SU_NO_ARG, 0, 'f' },
        { "header", SU_REQ_ARG, 0, 'h' },
        { "output-tabs", SU_OPT_ARG, 0, 'i' },
        { "join-lines", SU_NO_ARG, 0, 'J' },
        { "length", SU_REQ_ARG, 0, 'l' },
        { "merge", SU_NO_ARG, 0, 'm' },
        { "number-lines", SU_OPT_ARG, 0, 'n' },
        { "first-line-number", SU_REQ_ARG, 0, 'N' },
        { "indent", SU_REQ_ARG, 0, 'o' },
        { "no-file-warnings", SU_NO_ARG, 0, 'r' },
        { "separator", SU_OPT_ARG, 0, 's' },
        { "sep-string", SU_OPT_ARG, 0, 'S' },
        { "omit-header", SU_NO_ARG, 0, 't' },
        { "omit-pagination", SU_NO_ARG, 0, 'T' },
        { "show-nonprinting", SU_NO_ARG, 0, 'v' },
        { "width", SU_REQ_ARG, 0, 'w' },
        { "page-width", SU_REQ_ARG, 0, 'W' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&fm_pr, 0, sizeof fm_pr);
    fm_pr.page_len = 66;
    fm_pr.width = 72;
    fm_pr.cols = 1;
    fm_pr.s_char = -1;
    fm_pr.in_ch = '\t';
    fm_pr.in_w = 8;
    fm_pr.out_ch = '\t';
    fm_pr.out_w = 8;
    fm_pr.num_sep = '\t';
    fm_pr.num_digits = 5;
    fm_pr.first_page = 1;
    fm_pr.last_page = UINTMAX_MAX;
    fm_pr_status = 0;
    fm_pr_stdin = 0;

    char **files = (char **)malloc(((size_t)argc + 1) * sizeof *files);
    if (!files) su_error(1, ENOMEM, "memory exhausted");
    int nfiles = 0;
    /* -COLUMN: consecutive digit options make one number, read when the
     * options are done ("-1 -2" is 12 columns; "-0 -5" is 5) */
    char digits[64];
    size_t ndig = 0;
    int last_digit = 0, have_digits = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, "-0123456789aD:cde::Ffh:i::Jl:mN:n::o:rs::S::tTvw:W:", longs, 0)) != -1) {
        if (c >= '0' && c <= '9') {
            if (!last_digit) ndig = 0;
            if (ndig + 1 < sizeof digits) digits[ndig++] = (char)c;
            digits[ndig] = 0;
            last_digit = 1;
            have_digits = 1;
            continue;
        }
        if (have_digits && last_digit) {
            fm_pr.cols = fm_pr_int(digits, 1, "invalid number of columns");
        }
        last_digit = 0;
        have_digits = 0;
        switch (c) {
        case 1:
            if (su_optarg[0] == '+' && !fm_pr.range_given && fm_pr_range(su_optarg + 1, "+"))
                break;
            files[nfiles++] = su_optarg;
            break;
        case PAGES_OPT: fm_pr_range(su_optarg, "--pages"); break;
        case COLUMNS_OPT: fm_pr.cols = fm_pr_int(su_optarg, 1, "invalid number of columns"); break;
        case 'a': fm_pr.across = 1; break;
        case 'c': if (fm_pr.ctl != 2) fm_pr.ctl = 1; break;
        case 'd': fm_pr.dbl = 1; break;
        case 'D': fm_pr.datefmt = su_optarg; break;
        case 'e':
            fm_pr.e_given = 1;
            fm_pr_charnum("e", su_optarg, &fm_pr.in_ch, &fm_pr.in_w);
            break;
        case 'F': case 'f': fm_pr.ff = 1; break;
        case 'h': fm_pr.header = su_optarg; break;
        case 'i':
            fm_pr.i_given = 1;
            fm_pr_charnum("i", su_optarg, &fm_pr.out_ch, &fm_pr.out_w);
            break;
        case 'J': fm_pr.join = 1; break;
        case 'l': fm_pr.page_len = fm_pr_int(su_optarg, 1, "'-l PAGE_LENGTH' invalid number of lines"); break;
        case 'm': fm_pr.merge = 1; break;
        case 'N':
            fm_pr.N_first = fm_pr_int(su_optarg, INT_MIN, "'-N NUMBER' invalid starting line number");
            fm_pr.N_given = 1;
            break;
        case 'n':
            fm_pr.number = 1;
            fm_pr_charnum("n", su_optarg, &fm_pr.num_sep, &fm_pr.num_digits);
            break;
        case 'o': fm_pr.margin = fm_pr_int(su_optarg, 0, "'-o MARGIN' invalid line offset"); break;
        case 'r': fm_pr.nowarn = 1; break;
        case 's':
            fm_pr.s_given = 1;
            if (su_optarg && *su_optarg) fm_pr.s_char = (unsigned char)su_optarg[0];
            break;
        case 'S':
            fm_pr.S_given = 1;
            fm_pr.S_str = su_optarg ? su_optarg : "";
            break;
        case 't': fm_pr.t_given = 1; fm_pr.omit_hdr = 1; break;
        case 'T': fm_pr.omit_pag = 1; fm_pr.omit_hdr = 1; break;
        case 'v': fm_pr.ctl = 2; break;
        case 'w': {
            /* -W wins, whichever comes first */
            int v = fm_pr_int(su_optarg, 1, "'-w PAGE_WIDTH' invalid number of characters");
            if (!fm_pr.W_given) fm_pr.width = v;
            fm_pr.w_given = 1;
            break;
        }
        case 'W':
            fm_pr.width = fm_pr_int(su_optarg, 1, "'-W PAGE_WIDTH' invalid number of characters");
            fm_pr.W_given = 1;
            break;
        case SU_HELP_OPT: fm_pr_usage(); return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (have_digits && last_digit) fm_pr.cols = fm_pr_int(digits, 1, "invalid number of columns");
    for (int i = su_optind; i < argc; i++) files[nfiles++] = argv[i];

    if (fm_pr.merge && fm_pr.cols > 1)
        su_error(1, 0, "cannot specify number of columns when printing in parallel");
    if (fm_pr.merge && fm_pr.across)
        su_error(1, 0, "cannot specify both printing across and printing in parallel");
    /* a page of 10 lines or fewer is -t's, form feeds kept even with -T */
    if (fm_pr.page_len <= 10) { fm_pr.omit_hdr = 1; fm_pr.omit_pag = 0; }

    if (nfiles == 0) files[nfiles++] = (char *)"-";
    fm_pr_ncols = fm_pr.merge ? nfiles : fm_pr.cols;
    int multi = fm_pr_ncols > 1;
    /* the separator */
    static char sepbuf[2];
    if (fm_pr.S_given) fm_pr_sep = fm_pr.S_str;
    else if (fm_pr.s_given) {
        if (fm_pr.s_char >= 0) { sepbuf[0] = (char)fm_pr.s_char; sepbuf[1] = 0; fm_pr_sep = sepbuf; }
        else fm_pr_sep = (fm_pr.w_given || fm_pr.W_given) ? "" : "\t";
    } else if (fm_pr.join) fm_pr_sep = "\t";
    else fm_pr_sep = " ";
    /* columns expand input tabs -- unless a TAB separates them -- and
     * write output tabs */
    fm_pr_expand = fm_pr.e_given || (multi && strcmp(fm_pr_sep, "\t") != 0);
    fm_pr_tabify = fm_pr.i_given || multi;
    if (fm_pr.merge && !multi)
        fm_pr_trunc = (fm_pr.w_given || fm_pr.W_given) && !fm_pr.join;
    else if (multi)
        fm_pr_trunc = !fm_pr.join &&
                      !(fm_pr.s_given && !fm_pr.S_given && !fm_pr.w_given && !fm_pr.W_given);
    else
        fm_pr_trunc = fm_pr.W_given && !fm_pr.join;
    fm_pr_nw = 0;
    if (fm_pr.number)
        fm_pr_nw = fm_pr.num_sep == '\t' ? fm_pr.num_digits + 8 - fm_pr.num_digits % 8
                                         : fm_pr.num_digits + 1;
    if (multi) {
        long room = (long)fm_pr.width - (long)(fm_pr_ncols - 1) * (long)strlen(fm_pr_sep);
        if (fm_pr.merge) room -= fm_pr_nw;
        long cw = room / fm_pr_ncols;
        if (cw < 1) su_error(1, 0, "page width too narrow");
        fm_pr_cw = (int)cw;
    } else {
        fm_pr_cw = fm_pr.width;
        if (fm_pr.merge) {
            fm_pr_cw -= fm_pr_nw;
            if (fm_pr_cw < 1) su_error(1, 0, "page width too narrow");
        }
    }
    fm_pr_body = fm_pr.omit_hdr ? fm_pr.page_len : fm_pr.page_len - 10;
    fm_pr_lpc = fm_pr.dbl ? fm_pr_body / 2 : fm_pr_body;
    if (fm_pr_lpc < 1) fm_pr_lpc = 1;

    if (fm_pr.merge) {
        fm_pr_merge(files, nfiles);
    } else {
        for (int i = 0; i < nfiles; i++) fm_pr_one_file(files[i]);
    }
    if (fm_pr_stdin) { free(fm_pr_stdin); fm_pr_stdin = 0; }
    free(files);
    for (size_t i = 0; i < fm_pr_caplines; i++) free(fm_pr_lines[i].s);
    free(fm_pr_lines);
    free(fm_pr_nums);
    fm_pr_lines = 0;
    fm_pr_nums = 0;
    fm_pr_caplines = 0;
    free(fm_pr_date_str);
    fm_pr_date_str = 0;
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return fm_pr_status;
}
