/* su_cut.c -- cut, as GNU coreutils 9.7's (logs/utilgate/staging/
 * cut.sh): the selected bytes, characters or fields of each line of each
 * FILE (or standard input, or "-").
 *
 * WHAT WAS MEASURED, in the C locale (-c is -b; -n is accepted and does
 * nothing):
 *   - A LIST is ranges N, N-, -M, N-M, separated by commas or blanks.
 *     Its errors, word for word: an empty range or a 0 is "byte/character
 *     positions are numbered from 1" ("fields are numbered from 1"); "-"
 *     alone "invalid range with no endpoint: -"; M < N "invalid
 *     decreasing range"; a second '-' "invalid byte or character range"
 *     ("invalid field range"); any other character "invalid byte/
 *     character position 'REST'" ("invalid field value 'REST'", quoting
 *     the list from that character on); a number of 2^64-1 or more
 *     "byte/character offset 'N' is too large" ("field number 'N' is too
 *     large"). Each is followed by the "Try" line.
 *   - Ranges that overlap are one; ranges that only touch are two, and
 *     with --output-delimiter the string goes between them (-b 1,2 is
 *     "a-b"). --complement selects the rest. With -f the delimiter (or
 *     the --output-delimiter, a NUL when empty) goes between every two
 *     fields printed; a line with no delimiter is printed whole, unless
 *     -s. Every line printed ends with the line delimiter (-z: NUL), the
 *     last too.
 *   - -d takes one byte ("" is NUL; more is "the delimiter must be a
 *     single character", at once). After the options: no list is "you
 *     must specify a list of bytes, characters, or fields", two lists
 *     "only one list may be specified" (at once), -d without -f "an
 *     input delimiter may be specified only when operating on fields",
 *     -s without -f "suppressing non-delimited lines makes sense\n\tonly
 *     when operating on fields"; then the list is read.
 *   - A file that cannot be read is "cut: NAME: REASON" (standard input
 *     is "-"), and the rest are still cut; status 1.
 *
 * The old cut (sbase's) had no --complement, --output-delimiter or -z,
 * merged touching ranges and said "cut: NAME:" with its own words. */

#include "su_txio.c"

struct txc_range { uintmax_t lo, hi; };

static struct {
    struct txc_range *r;
    size_t n, cap;
    int fields;             /* -f, not -b/-c */
    int complement;
    int suppress;           /* -s */
    char delim;             /* -d */
    char eol;               /* '\n', or NUL with -z */
    const char *odelim;     /* --output-delimiter, or 0 */
    size_t odelim_len;
} txc;

static void txc_add(uintmax_t lo, uintmax_t hi) {
    if (txc.n == txc.cap) {
        txc.cap = txc.cap ? txc.cap * 2 : 16;
        txc.r = (struct txc_range *)realloc(txc.r, txc.cap * sizeof *txc.r);
        if (!txc.r) tx_error(1, ENOMEM, "memory exhausted");
    }
    txc.r[txc.n].lo = lo;
    txc.r[txc.n].hi = hi;
    txc.n++;
}

static void txc_bad(const char *msg) __attribute__((noreturn));
static void txc_bad(const char *msg) {
    tx_error(0, 0, "%s", msg);
    tx_try_help(1);
}

static int txc_cmp(const void *a, const void *b) {
    const struct txc_range *x = (const struct txc_range *)a, *y = (const struct txc_range *)b;
    if (x->lo != y->lo) return x->lo < y->lo ? -1 : 1;
    return x->hi < y->hi ? -1 : x->hi > y->hi;
}

/* the LIST, read as GNU reads it, into sorted, merged ranges */
static void txc_list(const char *spec) {
    int f = txc.fields;
    const char *from1 = f ? "fields are numbered from 1"
                          : "byte/character positions are numbered from 1";
    uintmax_t value = 0, initial = 1;
    int lhs = 0, rhs = 0, dash = 0;
    for (const char *p = spec;; p++) {
        if (*p == '-') {
            if (dash) txc_bad(f ? "invalid field range" : "invalid byte or character range");
            if (lhs && value == 0) txc_bad(from1);
            dash = 1;
            initial = lhs ? value : 1;
            value = 0;
        } else if (*p == ',' || *p == ' ' || *p == '\t' || *p == 0) {
            if (dash) {
                dash = 0;
                if (!lhs && !rhs) txc_bad("invalid range with no endpoint: -");
                if (!rhs) txc_add(initial, UINTMAX_MAX);
                else {
                    if (value < initial) txc_bad("invalid decreasing range");
                    txc_add(initial, value);
                }
            } else {
                if (value == 0) txc_bad(from1);
                txc_add(value, value);
            }
            value = 0;
            lhs = rhs = 0;
            if (!*p) break;
        } else if (isdigit((unsigned char)*p)) {
            const char *start = p;
            uintmax_t v = 0;
            int big = 0;
            for (; isdigit((unsigned char)*p); p++) {
                unsigned d = (unsigned)(*p - '0');
                if (v > (UINTMAX_MAX - 1 - d) / 10) big = 1;
                else v = v * 10 + d;
            }
            if (big) {
                char *num = (char *)malloc((size_t)(p - start) + 1);
                if (!num) tx_error(1, ENOMEM, "memory exhausted");
                memcpy(num, start, (size_t)(p - start));
                num[p - start] = 0;
                tx_error(0, 0, f ? "field number %s is too large"
                                 : "byte/character offset %s is too large", tx_quote(num));
                tx_try_help(1);
            }
            p--;
            value = v;
            if (dash) rhs = 1;
            else lhs = 1;
        } else {
            tx_error(0, 0, f ? "invalid field value %s" : "invalid byte/character position %s",
                     tx_quote(p));
            tx_try_help(1);
        }
    }
    qsort(txc.r, txc.n, sizeof *txc.r, txc_cmp);
    size_t m = 0;
    for (size_t i = 0; i < txc.n; i++) {
        if (m && txc.r[i].lo <= txc.r[m - 1].hi) {
            if (txc.r[i].hi > txc.r[m - 1].hi) txc.r[m - 1].hi = txc.r[i].hi;
        } else {
            txc.r[m++] = txc.r[i];
        }
    }
    txc.n = m;
    if (txc.complement) {
        struct txc_range *old = txc.r;
        size_t on = txc.n;
        txc.r = 0;
        txc.n = txc.cap = 0;
        uintmax_t next = 1;
        for (size_t i = 0; i < on; i++) {
            if (old[i].lo > next) txc_add(next, old[i].lo - 1);
            if (old[i].hi == UINTMAX_MAX) { next = 0; break; }
            next = old[i].hi + 1;
        }
        if (next) txc_add(next, UINTMAX_MAX);
        free(old);
    }
}

/* which range holds POS, advancing *IDX (POS only grows within a line);
 * -1 when none */
static long txc_find(uintmax_t pos, size_t *idx) {
    while (*idx < txc.n && pos > txc.r[*idx].hi) (*idx)++;
    if (*idx < txc.n && pos >= txc.r[*idx].lo) return (long)*idx;
    return -1;
}

/* a write that fails ends cut at once: "write error: Broken pipe" */
static void txc_wfail(void) __attribute__((noreturn));
static void txc_wfail(void) {
    int e = errno;
    tx_oerr = 0;
    tx_error(1, e, "write error");
    exit(1);
}
static void txc_putc(int c) { if (tx_putc(c) < 0) txc_wfail(); }
static void txc_write(const void *p, size_t n) { if (tx_fwrite(p, n) < n) txc_wfail(); }

static void txc_odelim(void) {
    if (txc.odelim_len) txc_write(txc.odelim, txc.odelim_len);
    else txc_putc(0);
}

/* -b / -c: byte by byte, a line at a time */
static int txc_bytes(int fd, const char *name) {
    unsigned char buf[8192];
    uintmax_t pos = 0;
    size_t idx = 0;
    long last = -1;
    int any = 0;            /* bytes in this line not yet ended */
    for (;;) {
        ssize_t r = tx_read(fd, buf, sizeof buf);
        if (r < 0) {
            tx_error(0, errno, "%s", tx_quotef(name));
            if (any) txc_putc(txc.eol);
            return 0;
        }
        if (r == 0) break;
        for (ssize_t i = 0; i < r; i++) {
            unsigned char c = buf[i];
            if (c == (unsigned char)txc.eol) {
                txc_putc(txc.eol);
                pos = 0;
                idx = 0;
                last = -1;
                any = 0;
                continue;
            }
            any = 1;
            if (pos < UINTMAX_MAX) pos++;
            long k = txc_find(pos, &idx);
            if (k < 0) continue;
            if (txc.odelim && last >= 0 && k != last) txc_odelim();
            last = k;
            txc_putc(c);
        }
    }
    if (any) txc_putc(txc.eol);
    return 1;
}

/* -f: a line at a time */
static void txc_line_fields(const char *s, size_t n) {
    if (!memchr(s, txc.delim, n)) {
        if (!txc.suppress) {
            txc_write(s, n);
            txc_putc(txc.eol);
        }
        return;
    }
    uintmax_t field = 1;
    size_t idx = 0, start = 0;
    int printed = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && s[i] != txc.delim) continue;
        if (txc_find(field, &idx) >= 0) {
            if (printed) {
                if (txc.odelim) txc_odelim();
                else txc_putc(txc.delim);
            }
            txc_write(s + start, i - start);
            printed = 1;
        }
        start = i + 1;
        if (field < UINTMAX_MAX) field++;
    }
    txc_putc(txc.eol);
}

static int txc_fields(int fd, const char *name) {
    unsigned char buf[8192];
    size_t cap = 4096, len = 0;
    char *line = (char *)malloc(cap);
    if (!line) tx_error(1, ENOMEM, "memory exhausted");
    int ok = 1;
    for (;;) {
        ssize_t r = tx_read(fd, buf, sizeof buf);
        if (r < 0) {
            tx_error(0, errno, "%s", tx_quotef(name));
            ok = 0;
            break;
        }
        if (r == 0) break;
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == (unsigned char)txc.eol) {
                txc_line_fields(line, len);
                len = 0;
                continue;
            }
            if (len == cap) {
                cap *= 2;
                char *nl = (char *)realloc(line, cap);
                if (!nl) tx_error(1, ENOMEM, "memory exhausted");
                line = nl;
            }
            line[len++] = (char)buf[i];
        }
    }
    if (len) txc_line_fields(line, len);
    free(line);
    return ok;
}

static int txc_file(const char *name) {
    int fd = 0;
    if (strcmp(name, "-")) {
        fd = su_open_read(name, 0);
        if (fd < 0) {
            tx_error(0, errno, "%s", tx_quotef(name));
            return 0;
        }
    }
    int ok = txc.fields ? txc_fields(fd, name) : txc_bytes(fd, name);
    if (fd != 0 && close(fd) < 0) {
        tx_error(0, errno, "%s", tx_quotef(name));
        ok = 0;
    }
    return ok;
}

static void txc_help(void) {
    tx_printf("Usage: %s OPTION... [FILE]...\n"
              "Print the chosen parts of each line of each FILE to standard output.\n"
              "\n"
              "With no FILE, or when FILE is -, read standard input.\n"
              "\n"
              "Mandatory arguments to long options are mandatory for short options too.\n"
              "  -b, --bytes=LIST        only these bytes\n"
              "  -c, --characters=LIST   only these characters\n"
              "  -d, --delimiter=DELIM   fields are separated by DELIM, not TAB\n"
              "  -f, --fields=LIST       only these fields; a line with no delimiter in\n"
              "                            it is printed whole, unless -s is given\n"
              "  -n                      (does nothing)\n"
              "      --complement        print what the LIST does not choose instead\n"
              "  -s, --only-delimited    leave out the lines with no delimiter\n"
              "      --output-delimiter=STRING  put STRING between the parts printed;\n"
              "                            by default it is the input delimiter\n"
              "  -z, --zero-terminated   lines end in NUL, not newline\n"
              "      --help        show this help and exit\n"
              "      --version     show the version and exit\n"
              "\n"
              "Give exactly one of -b, -c and -f. A LIST is one range, or several\n"
              "separated by commas; what it chooses is printed in the order of the\n"
              "input, each part once. A range is one of:\n"
              "\n"
              "  N     the Nth byte, character or field, counting from 1\n"
              "  N-    from the Nth to the end of the line\n"
              "  N-M   from the Nth to the Mth, both included\n"
              "  -M    from the first to the Mth, included\n", su_name);
}

static int do_cut(int argc, char **argv) {
    enum { COMPLEMENT_OPT = 0x100, ODELIM_OPT };
    static const struct su_option longs[] = {
        { "bytes", SU_REQ_ARG, 0, 'b' },
        { "characters", SU_REQ_ARG, 0, 'c' },
        { "fields", SU_REQ_ARG, 0, 'f' },
        { "delimiter", SU_REQ_ARG, 0, 'd' },
        { "only-delimited", SU_NO_ARG, 0, 's' },
        { "output-delimiter", SU_REQ_ARG, 0, ODELIM_OPT },
        { "complement", SU_NO_ARG, 0, COMPLEMENT_OPT },
        { "zero-terminated", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    tx_begin(1);
    memset(&txc, 0, sizeof txc);
    txc.delim = '\t';
    txc.eol = '\n';
    const char *spec = 0;
    int have_delim = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, "b:c:d:f:nsz", longs, 0)) != -1) {
        switch (c) {
        case 'b': case 'c': case 'f':
            if (spec) txc_bad("only one list may be specified");
            spec = su_optarg;
            txc.fields = c == 'f';
            break;
        case 'd':
            if (su_optarg[0] && su_optarg[1]) txc_bad("the delimiter must be a single character");
            txc.delim = su_optarg[0];
            have_delim = 1;
            break;
        case 'n': break;
        case 's': txc.suppress = 1; break;
        case 'z': txc.eol = 0; break;
        case ODELIM_OPT:
            txc.odelim = su_optarg;
            txc.odelim_len = strlen(su_optarg);
            break;
        case COMPLEMENT_OPT: txc.complement = 1; break;
        case SU_HELP_OPT: txc_help(); return 0;
        case SU_VERSION_OPT: return tx_version();
        default: tx_try_help(1);
        }
    }
    if (!spec) txc_bad("you must specify a list of bytes, characters, or fields");
    if (have_delim && !txc.fields)
        txc_bad("an input delimiter may be specified only when operating on fields");
    if (txc.suppress && !txc.fields)
        txc_bad("suppressing non-delimited lines makes sense\n\tonly when operating on fields");
    txc_list(spec);
    int ok = 1;
    if (su_optind == argc) ok = txc_file("-");
    for (int i = su_optind; i < argc; i++) ok &= txc_file(argv[i]);
    return ok ? 0 : 1;
}
