/* su_wc.c -- wc, as GNU coreutils 9.7's (logs/utilgate/staging/wc.sh):
 * newlines, words, characters, bytes and the longest line's width of each
 * FILE (or standard input, or "-"), and their total.
 *
 * WHAT WAS MEASURED, in the C locale (a character is a byte):
 *   - The columns, in this order whatever the options' order: -l -w -m
 *     -c -L; none given is -l -w -c. A word ends at a byte of 9-13, 32
 *     or 160 (0xA0 separates words even here). -L: a printable ASCII
 *     byte is one column, a tab goes to the next multiple of 8, \n \r \f
 *     end a line, every other byte is no column.
 *   - The width of every number is decided before anything is read:
 *     the digits of the sum of the sizes of the regular files among the
 *     inputs (one that cannot be stat'ed counts nothing), but at least 7
 *     when one is not regular (a pipe, a directory, /dev/null); 1 when
 *     there is one input and one column (`cat f | wc -l` is "20"), with
 *     --total=only, and for names read from a --files0-from stream that
 *     is not a regular file (whose names are not known ahead).
 *   - A name is printed as it is, unless it has a newline in it (then
 *     shell-quoted, 'n'$'\n''l'). A file that cannot be opened is
 *     "wc: NAME: REASON", no line; one that cannot be read gets its line
 *     (zeros) and then the reason ("wc: 'standard input': Is a
 *     directory").
 *   - --total=auto (a total line when more than one input), always,
 *     only (the total alone, no label), never; argmatch's abbreviations.
 *   - --files0-from=F: NUL-terminated names from F ("-": standard input,
 *     where "-" may not be one of them); an empty one is "F:N: invalid
 *     zero-length file name"; with operands, "extra operand" and "file
 *     operands cannot be combined with --files0-from".
 *   - Standard output is line buffered: each line is written as it is
 *     made, so a failed write is remembered and the message at exit is
 *     "write error" without a reason (`wc f > /dev/full`).
 *
 * The old wc (sbase's) had no -L, -m, --files0-from or --total, padded
 * every column to 7 and said "wc: X:" for every error. */

#include "su_txio.c"

static struct {
    int lines, words, chars, bytes, maxlen;
    int ncols;
    int width;
    int total_mode;         /* 0 auto, 1 always, 2 only, 3 never */
    uintmax_t t_lines, t_words, t_chars, t_bytes, t_maxlen;
    int first_col;
} txw;

/* the byte classes: 1 a word separator; for -L, 0 no column, 1 one
 * column, 2 a tab, 3 the end of a line */
static unsigned char txw_sep[256], txw_lw[256];

static void txw_tables(void) {
    for (int c = 0; c < 256; c++) {
        txw_sep[c] = (c >= 9 && c <= 13) || c == 32 || c == 160;
        txw_lw[c] = (c >= 0x20 && c <= 0x7e) ? 1 : 0;
    }
    txw_lw['\t'] = 2;
    txw_lw['\n'] = txw_lw['\r'] = txw_lw['\f'] = 3;
}

static void txw_num(uintmax_t v) {
    tx_printf("%s%*ju", txw.first_col ? "" : " ", txw.width, v);
    txw.first_col = 0;
}

static void txw_line(uintmax_t l, uintmax_t w, uintmax_t ch, uintmax_t b, uintmax_t ml,
                    const char *name) {
    txw.first_col = 1;
    if (txw.lines) txw_num(l);
    if (txw.words) txw_num(w);
    if (txw.chars) txw_num(ch);
    if (txw.bytes) txw_num(b);
    if (txw.maxlen) txw_num(ml);
    if (name) {
        tx_putc(' ');
        tx_puts(strchr(name, '\n') ? tx_quotef(name) : name);
    }
    tx_putc('\n');
}

/* count one input; 0 when it could not be opened or read */
static int txw_file(const char *name, const char *shown) {
    int fd;
    const char *disp;
    if (!strcmp(name, "-")) {
        fd = 0;
        disp = "standard input";
    } else {
        fd = su_open_read(name, 0);
        if (fd < 0) {
            tx_error(0, errno, "%s", tx_quotef(name));
            return 0;
        }
        disp = name;
    }
    uintmax_t l = 0, w = 0, b = 0, ml = 0, pos = 0;
    int inword = 0, err = 0;
    unsigned char buf[16384];
    for (;;) {
        ssize_t r = tx_read(fd, buf, sizeof buf);
        if (r < 0) { err = errno; break; }
        if (r == 0) break;
        b += (uintmax_t)r;
        for (ssize_t i = 0; i < r; i++) {
            unsigned char c = buf[i];
            if (c == '\n') l++;
            if (txw_sep[c]) inword = 0;
            else if (!inword) { inword = 1; w++; }
            switch (txw_lw[c]) {
            case 1: pos++; break;
            case 2: pos += 8 - pos % 8; break;
            case 3: if (pos > ml) ml = pos; pos = 0; break;
            }
        }
    }
    if (pos > ml) ml = pos;
    if (fd != 0) close(fd);
    if (txw.total_mode != 2) txw_line(l, w, b, b, ml, shown);
    txw.t_lines += l;
    txw.t_words += w;
    txw.t_chars += b;
    txw.t_bytes += b;
    if (ml > txw.t_maxlen) txw.t_maxlen = ml;
    if (err) {
        tx_error(0, err, "%s", tx_quotef(disp));
        return 0;
    }
    return 1;
}

static int txw_digits(uintmax_t v) {
    int d = 1;
    while (v >= 10) { v /= 10; d++; }
    return d;
}

/* the column width, from what the inputs are before any is read */
static int txw_width_of(char **names, size_t n, int skip_dash) {
    if (txw.total_mode == 2) return 1;
    if (txw.ncols == 1 && n == 1) return 1;
    int minw = 1;
    uintmax_t total = 0;
    for (size_t i = 0; i < n; i++) {
        struct stat st;
        int r;
        /* an empty name, or "-" among names read from standard input,
         * is refused, and counts nothing here */
        if (!names[i][0] || (skip_dash && !strcmp(names[i], "-"))) continue;
        if (!strcmp(names[i], "-")) r = fstat(0, &st);
        else r = stat(names[i], &st);
        if (r < 0) continue;
        if (S_ISREG(st.st_mode)) total += (uintmax_t)st.st_size;
        else minw = 7;
    }
    int d = txw_digits(total);
    return d > minw ? d : minw;
}

/* --files0-from: the names, NUL-terminated */
struct txw_names { char **v; size_t n, cap; };

static void txw_add(struct txw_names *nm, char *s) {
    if (nm->n == nm->cap) {
        nm->cap = nm->cap ? nm->cap * 2 : 16;
        nm->v = (char **)realloc(nm->v, nm->cap * sizeof *nm->v);
        if (!nm->v) tx_error(1, ENOMEM, "memory exhausted");
    }
    nm->v[nm->n++] = s;
}

/* one name from FD (NUL-terminated, or ended by the end), in a fresh
 * string; 0 at the end. A read error is "F: read error". */
static char *txw_next_name(int fd, const char *f0) {
    static unsigned char buf[8192];
    static size_t bpos, blen;
    static int eof;
    size_t n = 0, cap = 64;
    char *s = (char *)malloc(cap);
    if (!s) tx_error(1, ENOMEM, "memory exhausted");
    for (;;) {
        if (bpos == blen) {
            if (eof) break;
            ssize_t r = tx_read(fd, buf, sizeof buf);
            if (r < 0) tx_error(1, errno, "%s: read error", tx_quotef(f0));
            if (r == 0) { eof = 1; break; }
            bpos = 0;
            blen = (size_t)r;
        }
        unsigned char c = buf[bpos++];
        if (c == 0) { s[n] = 0; return s; }
        if (n + 1 >= cap) {
            cap *= 2;
            char *ns = (char *)realloc(s, cap);
            if (!ns) tx_error(1, ENOMEM, "memory exhausted");
            s = ns;
        }
        s[n++] = (char)c;
    }
    if (n == 0) { free(s); return 0; }
    s[n] = 0;
    return s;
}

/* one name of a --files0-from list: count it (or say why not) */
static int txw_entry(const char *name, const char *f0, uintmax_t idx) {
    if (!name[0]) {
        tx_error(0, 0, "%s:%ju: invalid zero-length file name",
                 tx_quotef(f0), idx);
        return 0;
    }
    if (!strcmp(f0, "-") && !strcmp(name, "-")) {
        tx_error(0, 0, "when reading file names from stdin, no file name of %s allowed",
                 tx_quoteaf("-"));
        return 0;
    }
    return txw_file(name, name);
}

static void txw_help(void) {
    tx_printf("Usage: %s [OPTION]... [FILE]...\n"
              "  or:  %s [OPTION]... --files0-from=F\n"
              "Count the newlines, words and bytes of each FILE, and print a line of\n"
              "totals when there is more than one FILE. A word is a run of characters\n"
              "that are not white space, between white space or the ends of the input.\n"
              "\n"
              "With no FILE, or when FILE is -, read standard input.\n"
              "\n"
              "Options choose the counts printed, which always come in this order:\n"
              "newlines, words, characters, bytes, longest line.\n"
              "  -c, --bytes            the byte counts\n"
              "  -m, --chars            the character counts\n"
              "  -l, --lines            the newline counts\n"
              "      --files0-from=F    count the files named in F, each name ended by\n"
              "                           a NUL; with F -, read the names from standard\n"
              "                           input\n"
              "  -L, --max-line-length  the width of the widest line\n"
              "  -w, --words            the word counts\n"
              "      --total=WHEN       when to print the line of totals: auto, always,\n"
              "                           only or never\n"
              "      --help        show this help and exit\n"
              "      --version     show the version and exit\n", su_name, su_name);
}

static int do_wc(int argc, char **argv) {
    enum { FILES0_OPT = 0x100, TOTAL_OPT };
    static const struct su_option longs[] = {
        { "bytes", SU_NO_ARG, 0, 'c' },
        { "chars", SU_NO_ARG, 0, 'm' },
        { "files0-from", SU_REQ_ARG, 0, FILES0_OPT },
        { "lines", SU_NO_ARG, 0, 'l' },
        { "max-line-length", SU_NO_ARG, 0, 'L' },
        { "words", SU_NO_ARG, 0, 'w' },
        { "total", SU_REQ_ARG, 0, TOTAL_OPT },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    static const char *const total_v[] = { "auto", "always", "only", "never", 0 };
    tx_begin(1);
    tx_olinebuf = 1;
    memset(&txw, 0, sizeof txw);
    txw_tables();
    const char *files0 = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, "clLmw", longs, 0)) != -1) {
        switch (c) {
        case 'c': txw.bytes = 1; break;
        case 'm': txw.chars = 1; break;
        case 'l': txw.lines = 1; break;
        case 'L': txw.maxlen = 1; break;
        case 'w': txw.words = 1; break;
        case FILES0_OPT: files0 = su_optarg; break;
        case TOTAL_OPT: txw.total_mode = tx_argmatch("--total", su_optarg, total_v); break;
        case SU_HELP_OPT: txw_help(); return 0;
        case SU_VERSION_OPT: return tx_version();
        default: tx_try_help(1);
        }
    }
    if (!(txw.lines || txw.words || txw.chars || txw.bytes || txw.maxlen))
        txw.lines = txw.words = txw.bytes = 1;
    txw.ncols = txw.lines + txw.words + txw.chars + txw.bytes + txw.maxlen;

    int ok = 1;
    uintmax_t ninputs = 0;
    if (files0) {
        if (su_optind < argc) {
            tx_error(0, 0, "extra operand %s", tx_quoteaf(argv[su_optind]));
            fprintf(stderr, "file operands cannot be combined with --files0-from\n");
            tx_try_help(1);
        }
        int fd = 0;
        if (strcmp(files0, "-")) {
            fd = su_open_read(files0, 0);
            if (fd < 0) tx_error(1, errno, "cannot open %s for reading", tx_quoteaf(files0));
        }
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
            /* a file: every name first, so the width can be known */
            struct txw_names nm = { 0, 0, 0 };
            char *s;
            while ((s = txw_next_name(fd, files0))) txw_add(&nm, s);
            txw.width = txw_width_of(nm.v, nm.n, !strcmp(files0, "-"));
            for (size_t i = 0; i < nm.n; i++) ok &= txw_entry(nm.v[i], files0, i + 1);
            ninputs = nm.n;
        } else {
            /* a stream: each name as it comes */
            txw.width = 1;
            char *s;
            while ((s = txw_next_name(fd, files0))) {
                ninputs++;
                ok &= txw_entry(s, files0, ninputs);
                free(s);
            }
        }
        if (fd != 0) close(fd);
    } else if (su_optind == argc) {
        static char *dash[] = { (char *)"-" };
        txw.width = txw_width_of(dash, 1, 0);
        ok &= txw_file("-", 0);
        ninputs = 1;
    } else {
        txw.width = txw_width_of(argv + su_optind, (size_t)(argc - su_optind), 0);
        for (int i = su_optind; i < argc; i++) ok &= txw_file(argv[i], argv[i]);
        ninputs = (uintmax_t)(argc - su_optind);
    }
    if (txw.total_mode == 2) {
        txw.width = 1;
        txw_line(txw.t_lines, txw.t_words, txw.t_chars, txw.t_bytes, txw.t_maxlen, 0);
    } else if (txw.total_mode == 1 || (txw.total_mode == 0 && ninputs > 1)) {
        txw_line(txw.t_lines, txw.t_words, txw.t_chars, txw.t_bytes, txw.t_maxlen, "total");
    }
    return ok ? 0 : 1;
}
