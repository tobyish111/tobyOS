/* su_numfmt.c -- numfmt, as GNU coreutils 9.7's (logs/utilgate/staging/
 * numfmt.sh): numbers read from the operands, or from the fields of
 * standard input's lines, scaled to and from the SI and IEC units and
 * written back, in the C locale. Every name here starts fm_nf_.
 *
 * WHAT A LINE IS. Each operand is one line; with none, standard input is
 * read in lines ending in newline (NUL with -z), the first --header=N of
 * them copied as they are. A line is a C string: what follows a NUL in it
 * is dropped (measured: "1000\0x\n" comes out "1000\n"), and its ending
 * is written back only if it had one. Without -d, a field is the blanks
 * before it and the run of non-blanks after them (with -z a newline is a
 * blank too); ONE blank ends a field and is written back as a space, so
 * "1  2" keeps its spaces and "1\t2" becomes "1 2". With -d X a field is
 * what lies between X's, and X is written back.
 *
 * A NUMBER is [-]digits[.digits] after the field's blanks (spaces and
 * tabs only), at least one digit, and a dot only with a digit after it --
 * no '+', no exponent. More than 33 significant digits before the point
 * is "value too large to be converted"; more than 18 in either part is,
 * with --debug, "possible precision loss". The value is computed as GNU's
 * is, measured on thousands of inputs: the two parts each accumulated
 * digit by digit in long double, the fraction divided by 10^digits. Then
 * a unit letter (K/k M G T P E Z Y R Q, with --from), 'i' after it for
 * iec-i (auto: optional, and it means 1024), and --suffix's text is
 * taken off the end first. Blanks may stand between the number and its
 * letter, and GNU, which reads the field in place, takes the NUL that
 * ends the field after them for a letter and reads on past it into the
 * rest of the line (fm_nf_field). A field that is not a number is an
 * error, stopping numfmt (status 2) unless --invalid says otherwise; the
 * field is then written as it was, less its --suffix.
 *
 * ROUNDING is done the way GNU's is: the value times 10^precision taken
 * to an intmax_t and stepped by --round, so -0 comes out 0. Unscaled, the
 * precision is the input's (0 after a unit letter) or the format's, and
 * a value of more than 19 digits with it is refused. Scaled (--to), a
 * value is divided by 1000 or 1024 while it is at least that, rounded to
 * one decimal below 10 and to an integer above (a --format precision: at
 * most three decimals per power), divided again if the rounding reached
 * the base, and written with one decimal when it is below 10 and scaled
 * (else none), unless --format gives a precision; 1e33 and above cannot
 * be written, and the eleventh power, which only rounding can reach, is
 * GNU's "(error)".
 *
 * PADDING: a width in --format (with '0' it is the zeros' alone), else
 * --padding, else -- without -d -- every field after the first, and the
 * first when blanks start it, is padded to its own width. The number
 * (with its unit and --suffix) is padded, between --format's prefix and
 * suffix. A huge width is written out in full (GNU's is), in pieces,
 * stopping at a write error.
 *
 * The old numfmt (programs/user_coreutil) knew --to/--from si and iec and
 * nothing else. */

enum { FM_NF_NONE, FM_NF_AUTO, FM_NF_SI, FM_NF_IEC, FM_NF_IECI };
enum { FM_NF_UP, FM_NF_DOWN, FM_NF_FROMZERO, FM_NF_TOWARDSZERO, FM_NF_NEAREST };
enum { FM_NF_ABORT, FM_NF_FAIL, FM_NF_WARN, FM_NF_IGNORE };

/* parse results */
enum {
    FM_NF_OK, FM_NF_E_NUMBER, FM_NF_E_SUFFIX, FM_NF_E_SUFFIX2, FM_NF_E_REJECT,
    FM_NF_E_MISSING_I, FM_NF_E_CONVERT
};

struct fm_nf_range { size_t lo, hi; };

static struct {
    int from, to, round, invalid, debug, grouping, zero_term;
    uintmax_t from_unit, to_unit;
    const char *suffix;
    intmax_t padding;            /* 0: none */
    int delim;                   /* -1: blanks */
    size_t header;
    struct fm_nf_range *fields;
    size_t nfields;
    int fields_given;
    /* --format */
    int have_format;
    char *fmt_prefix, *fmt_suffix;
    long fmt_width;              /* 0: none; negative: left */
    long fmt_prec;               /* -1: none */
    int fmt_zero;
    /* state */
    int failed;                  /* a conversion failed */
} fm_nf;

/* ---- output -------------------------------------------------------------- */

/* N copies of ' ', written in pieces so that a width of 2^63 streams (as
 * GNU's does) and a dead stdout stops it */
static void fm_nf_fill(char c, uintmax_t n) {
    char buf[64];
    memset(buf, c, sizeof buf);
    while (n > 0) {
        size_t k = n > sizeof buf ? sizeof buf : (size_t)n;
        if (fwrite(buf, 1, k, stdout) != k || ferror(stdout)) return;
        n -= k;
    }
}

/* GNU's quote() in the C locale: 'TEXT' with a quote or a backslash
 * escaped, the C escapes for \a \b \f \n \r \t \v, and octal for the
 * other control characters and every byte past 0x7e. */
static const char *fm_nf_quote(const char *s, size_t len) {
    static char *bufs[4];
    static size_t caps[4];
    static int slot;
    int k = slot++ & 3;
    size_t need = len * 4 + 3;
    if (need > caps[k]) {
        char *nb = (char *)realloc(bufs[k], need);
        if (!nb) su_error(1, ENOMEM, "memory exhausted");
        bufs[k] = nb;
        caps[k] = need;
    }
    char *b = bufs[k];
    size_t n = 0;
    b[n++] = '\'';
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = 0;
        switch (c) {
        case '\a': esc = "\\a"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        case '\v': esc = "\\v"; break;
        case '\'': esc = "\\'"; break;
        case '\\': esc = "\\\\"; break;
        }
        if (esc) { b[n++] = esc[0]; b[n++] = esc[1]; }
        else if (c < 0x20 || c >= 0x7f) {
            b[n++] = '\\';
            b[n++] = (char)('0' + ((c >> 6) & 7));
            b[n++] = (char)('0' + ((c >> 3) & 7));
            b[n++] = (char)('0' + (c & 7));
        } else b[n++] = (char)c;
    }
    b[n++] = '\'';
    b[n] = 0;
    return b;
}

static const char *fm_nf_q(const char *s) { return fm_nf_quote(s, strlen(s)); }

/* %Lg of V, as GNU's messages show a value */
static const char *fm_nf_lg(long double v) {
    static char buf[2][128];
    static int slot;
    char *b = buf[slot++ & 1];
    struct su_fmtspec fs = { 0, 0, 0, 0, 0, -1, -1, 'g' };
    struct sn_sb sb = { 0, 0, 0 };
    su_ldfmt(&sb, v, &fs);
    snprintf(b, sizeof buf[0], "%s", sb.s ? sb.s : "");
    free(sb.s);
    return b;
}

/* ---- arithmetic ------------------------------------------------------------ */

static long double fm_nf_absl(long double v) { return v < 0 ? -v : v; }

/* 10^N by repeated multiplication; 1 when N <= 0 */
static long double fm_nf_p10(long n) {
    long double r = 1;
    for (long i = 0; i < n; i++) r *= 10;
    return r;
}

static long double fm_nf_powl(long double base, int n) {
    long double r = 1;
    for (int i = 0; i < n; i++) r *= base;
    return r;
}

/* V stepped to an integer by METHOD, through intmax_t as GNU's is (so
 * -0.4 towards zero is +0); past intmax_t's range V is whole already */
static long double fm_nf_round(long double v, int method) {
    if (v < -9223372036854775807.0L || v > 9223372036854775807.0L) return v;
    intmax_t i = (intmax_t)v;
    switch (method) {
    case FM_NF_UP:
        if ((long double)i < v) i++;
        break;
    case FM_NF_DOWN:
        if ((long double)i > v) i--;
        break;
    case FM_NF_FROMZERO:
        if (v < 0) { if ((long double)i > v) i--; }
        else if ((long double)i < v) i++;
        break;
    case FM_NF_TOWARDSZERO:
        break;
    case FM_NF_NEAREST: {
        long double f = v - (long double)i;
        if (f >= 0.5L) i++;
        else if (f <= -0.5L) i--;
        break;
    }
    }
    return (long double)i;
}

/* V rounded by METHOD to PREC decimals */
static long double fm_nf_round_prec(long double v, long prec, int method) {
    long double p = fm_nf_p10(prec);
    return fm_nf_round(v * p, method) / p;
}

/* "%.*Lf" of V into SB; the zeros a format's '0' asks for are counted
 * into fm_nf_zeros, to be written after the sign: GNU writes them all, so
 * %099999999999999999999f streams zeros as long as it is read */
static uintmax_t fm_nf_zeros;
static void fm_nf_put_fixed(struct sn_sb *sb, long double v, int prec) {
    struct su_fmtspec fs = { 0, 0, 0, 0, 0, -1, prec, 'f' };
    su_ldfmt(sb, v, &fs);
    fm_nf_zeros = 0;
    if (fm_nf.fmt_zero && fm_nf.fmt_width > 0 && (uintmax_t)fm_nf.fmt_width > sb->n)
        fm_nf_zeros = (uintmax_t)fm_nf.fmt_width - sb->n;
}

/* ---- option values ---------------------------------------------------------- */

/* --from-unit / --to-unit: a positive integer, an optional unit letter
 * (decimal, or binary with 'i' after it), nothing else */
static int fm_nf_unit_size(const char *s, uintmax_t *out) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') return -1;
    if (*p == '+') p++;
    uintmax_t v = 0;
    int digits = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINTMAX_MAX - d) / 10) return -1;
        v = v * 10 + d;
        digits = 1;
    }
    static const char letters[] = "KMGTPEZYRQ";
    if (*p) {
        const char *l = *p == 'k' ? letters : strchr(letters, *p);
        if (!l || !*p) return -1;
        if (!digits) v = 1;
        int power = (int)(l - letters) + 1;
        p++;
        uintmax_t base = 1000;
        if (*p == 'i') { base = 1024; p++; }
        for (int i = 0; i < power; i++) {
            if (v > UINTMAX_MAX / base) return -1;
            v *= base;
        }
    } else if (!digits) return -1;
    if (*p || v == 0) return -1;
    *out = v;
    return 0;
}

/* a decimal, as xstrtoumax: blanks and '+' before it, nothing after */
static int fm_nf_umax(const char *s, uintmax_t *out) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') return -1;
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    uintmax_t v = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINTMAX_MAX - d) / 10) return -1;
        v = v * 10 + d;
    }
    if (*p) return -1;
    *out = v;
    return 0;
}

/* --padding: a signed decimal; GNU takes one too large for intmax_t as
 * the largest (measured: --padding=99999999999999999999 pads forever) */
static int fm_nf_imax(const char *s, intmax_t *out) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if (!isdigit((unsigned char)*p)) return -1;
    uintmax_t v = 0;
    int over = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINTMAX_MAX - d) / 10) over = 1;
        else v = v * 10 + d;
    }
    if (*p) return -1;
    if (neg) *out = (over || v > (uintmax_t)INTMAX_MAX + 1) ? INTMAX_MIN
                    : v == (uintmax_t)INTMAX_MAX + 1 ? INTMAX_MIN : -(intmax_t)v;
    else *out = (over || v > (uintmax_t)INTMAX_MAX) ? INTMAX_MAX : (intmax_t)v;
    return 0;
}

/* argmatch: an exact name, or a prefix of exactly one (or of several
 * that mean the same) */
static int fm_nf_argmatch(const char *opt, const char *arg,
                          const char *const *names, const int *vals) {
    int found = -1, ambig = 0;
    size_t len = strlen(arg);
    for (int i = 0; names[i]; i++) {
        if (strncmp(names[i], arg, len)) continue;
        if (strlen(names[i]) == len) return vals[i];
        if (found < 0) found = i;
        else if (vals[found] != vals[i]) ambig = 1;
    }
    if (found >= 0 && !ambig) return vals[found];
    fflush(stdout);
    fprintf(stderr, "%s: %s argument %s for %s\nValid arguments are:\n", su_short,
            found >= 0 ? "ambiguous" : "invalid", su_quote(arg), su_quote(opt));
    for (int i = 0; names[i]; i++) fprintf(stderr, "  - %s\n", su_quote(names[i]));
    su_try_help(1);
}

/* --field's list, as cut's: N, N-, -M, N-M or -, separated by commas or
 * blanks (each one: "1  2" has an empty element between them) */
static void fm_nf_field_err(const char *msg) {
    su_error(0, 0, "%s", msg);
    su_try_help(1);
}

static void fm_nf_fields(const char *s) {
    const char *p = s;
    size_t lo = 0, hi = 0;
    int have_lo = 0, have_hi = 0, dash = 0;
    for (;;) {
        char c = *p;
        if (c == '-') {
            if (dash) fm_nf_field_err("invalid field range");
            dash = 1;
            if (have_lo && lo == 0) fm_nf_field_err("fields are numbered from 1");
            p++;
            continue;
        }
        if (isdigit((unsigned char)c)) {
            const char *st = p;
            uintmax_t v = 0;
            int over = 0;
            while (isdigit((unsigned char)*p)) {
                unsigned d = (unsigned)(*p - '0');
                if (v > (UINTMAX_MAX - d) / 10) over = 1;
                else v = v * 10 + d;
                p++;
            }
            if (over || v >= SIZE_MAX) {
                char *num = strndup(st, (size_t)(p - st));
                if (!num) su_error(1, ENOMEM, "memory exhausted");
                fflush(stdout);
                fprintf(stderr, "%s: field number %s is too large\n", su_short, su_quote(num));
                su_try_help(1);
            }
            if (dash) {
                if (have_hi) fm_nf_field_err("invalid field range");
                hi = (size_t)v;
                have_hi = 1;
            } else {
                if (have_lo) fm_nf_field_err("invalid field range");
                lo = (size_t)v;
                have_lo = 1;
            }
            continue;
        }
        if (c == ',' || c == ' ' || c == '\t' || c == 0) {
            size_t a, b;
            if (dash) {
                a = have_lo ? lo : 1;
                if (have_lo && lo == 0) fm_nf_field_err("fields are numbered from 1");
                b = have_hi ? hi : SIZE_MAX;
                if (have_hi && hi < a) fm_nf_field_err("invalid decreasing range");
            } else {
                if (!have_lo || lo == 0) fm_nf_field_err("fields are numbered from 1");
                a = b = lo;
            }
            struct fm_nf_range *nr = (struct fm_nf_range *)realloc(
                fm_nf.fields, (fm_nf.nfields + 1) * sizeof *nr);
            if (!nr) su_error(1, ENOMEM, "memory exhausted");
            fm_nf.fields = nr;
            nr[fm_nf.nfields].lo = a;
            nr[fm_nf.nfields].hi = b;
            fm_nf.nfields++;
            if (!c) break;
            p++;
            have_lo = have_hi = dash = 0;
            lo = hi = 0;
            continue;
        }
        fflush(stdout);
        fprintf(stderr, "%s: invalid field value %s\n", su_short, su_quote(p));
        su_try_help(1);
    }
}

static int fm_nf_selected(size_t n) {
    for (size_t i = 0; i < fm_nf.nfields; i++)
        if (n >= fm_nf.fields[i].lo && n <= fm_nf.fields[i].hi) return 1;
    return 0;
}

/* --format: [prefix]%[0' ]...[N][.N]f[suffix], GNU's way. The prefix
 * keeps its "%%" as written but loses one byte at its end for each (as
 * GNU's does: "a%%b%f" writes "a%%"); the suffix is written as it is. */
static void fm_nf_format(const char *fmt) {
    size_t i = 0, pairs = 0;
    for (;;) {
        if (!fmt[i]) su_error(1, 0, "format %s has no %% directive", fm_nf_q(fmt));
        if (fmt[i] == '%' && fmt[i + 1] == '%') { i += 2; pairs++; continue; }
        if (fmt[i] == '%') break;
        i++;
    }
    size_t plen = i - pairs;
    fm_nf.fmt_prefix = strndup(fmt, plen);
    if (!fm_nf.fmt_prefix) su_error(1, ENOMEM, "memory exhausted");
    i++;
    for (;; i++) {
        if (fmt[i] == '\'') fm_nf.grouping = 1;
        else if (fmt[i] == '0') fm_nf.fmt_zero = 1;
        else if (fmt[i] != ' ') break;
    }
    /* the width as strtol reads it: blanks, a sign, digits; too large
     * is the largest */
    {
        const char *p = fmt + i;
        while (isspace((unsigned char)*p)) p++;
        int neg = 0;
        if (*p == '-' || *p == '+') neg = *p++ == '-';
        if (isdigit((unsigned char)*p)) {
            unsigned long v = 0;
            int over = 0;
            for (; isdigit((unsigned char)*p); p++) {
                unsigned d = (unsigned)(*p - '0');
                if (v > (ULONG_MAX - d) / 10) over = 1;
                else v = v * 10 + d;
            }
            if (neg) fm_nf.fmt_width = (over || v > (unsigned long)LONG_MAX) ? LONG_MIN : -(long)v;
            else fm_nf.fmt_width = (over || v > (unsigned long)LONG_MAX) ? LONG_MAX : (long)v;
            i = (size_t)(p - fmt);
        }
    }
    if (!fmt[i]) su_error(1, 0, "format %s ends in %%", fm_nf_q(fmt));
    if (fmt[i] == '.') {
        i++;
        const char *p = fmt + i;
        /* strtol would take blanks and a sign: GNU refuses a precision
         * that does not start with a digit but would be read */
        const char *q = p;
        while (isspace((unsigned char)*q)) q++;
        if (*q == '-' || *q == '+') q++;
        if (isdigit((unsigned char)*q) && !isdigit((unsigned char)*p))
            su_error(1, 0, "invalid precision in format %s", fm_nf_q(fmt));
        unsigned long v = 0;
        for (; isdigit((unsigned char)*p); p++) {
            unsigned d = (unsigned)(*p - '0');
            if (v > ((unsigned long)LONG_MAX - d) / 10)
                su_error(1, 0, "invalid precision in format %s", fm_nf_q(fmt));
            v = v * 10 + d;
        }
        fm_nf.fmt_prec = (long)v;
        i = (size_t)(p - fmt);
    }
    if (fmt[i] != 'f')
        su_error(1, 0, "invalid format %s, directive must be %%[0]['][-][N][.][N]f",
                 fm_nf_q(fmt));
    i++;
    for (size_t j = i; fmt[j]; j++) {
        if (fmt[j] == '%') {
            if (fmt[j + 1] != '%')
                su_error(1, 0, "format %s has too many %% directives", fm_nf_q(fmt));
            j++;
        }
    }
    fm_nf.fmt_suffix = strdup(fmt + i);
    if (!fm_nf.fmt_suffix) su_error(1, ENOMEM, "memory exhausted");
    fm_nf.have_format = 1;
}

/* ---- conversion ------------------------------------------------------------ */

static int fm_nf_is_sep(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

/* a digit run, accumulated as GNU's is; its significant digits counted */
static const char *fm_nf_digits(const char *p, long double *v, size_t *sig, size_t *n) {
    *v = 0;
    *sig = 0;
    *n = 0;
    for (; isdigit((unsigned char)*p); p++) {
        int d = *p - '0';
        if (*sig || d) (*sig)++;
        *v = *v * 10 + d;
        (*n)++;
    }
    return p;
}

/* TEXT read as a number into *VAL and *PREC; FM_NF_OK or the error,
 * with *BAD at what was wrong for E_SUFFIX2. TEXT is laid out as GNU's
 * line is when it reads a field -- see fm_nf_field: past its NUL lies
 * what followed the field in the line, and GNU can read on into it. */
static int fm_nf_parse(const char *text, long double *val, long *prec,
                       const char **bad) {
    const char *p = text;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    long double iv, fv = 0;
    size_t isig, in, fsig = 0, fn = 0;
    p = fm_nf_digits(p, &iv, &isig, &in);
    if (isig > 33) return FM_NF_E_CONVERT;
    /* each part may be empty only before a point: ".5" is a number and
     * so, measured, is "1.." up to its second point (a bad suffix) */
    if (!in && *p != '.') return FM_NF_E_NUMBER;
    if (*p == '.') {
        p = fm_nf_digits(p + 1, &fv, &fsig, &fn);
        if (fsig > 33) return FM_NF_E_CONVERT;
        if (!fn && *p != '.') return FM_NF_E_NUMBER;
    }
    long double v = iv;
    if (fn) v += fv / fm_nf_p10((long)fn);
    if (neg) v = -v;
    *prec = (long)fn;
    if (*p) {
        /* blanks may come between the number and its letter -- and a
         * NUL after them counts as a letter of power 0, which GNU then
         * steps over (measured: -d, --from=si "1.5 ,2" is "invalid
         * suffix in input '1.5 ': '2'", and "1.5 " alone is 2) */
        while (*p == ' ' || *p == '\t') p++;
        static const char letters[] = "KMGTPEZYRQ";
        int power = -1;
        if (*p == 'k') power = 1;
        else if (!*p) power = 0;
        else {
            const char *l = strchr(letters, *p);
            if (l) power = (int)(l - letters) + 1;
        }
        if (fm_nf.from == FM_NF_NONE)
            return power >= 0 ? FM_NF_E_REJECT : FM_NF_E_SUFFIX;
        if (power < 0) return FM_NF_E_SUFFIX;
        p++;
        int base = 1000;
        if (fm_nf.from == FM_NF_IEC) base = 1024;
        else if (fm_nf.from == FM_NF_IECI) {
            if (*p != 'i') return FM_NF_E_MISSING_I;
            p++;
            base = 1024;
        } else if (fm_nf.from == FM_NF_AUTO && *p == 'i') {
            p++;
            base = 1024;
        }
        v *= fm_nf_powl((long double)base, power);
        *prec = 0;
        if (*p) { *bad = p; return FM_NF_E_SUFFIX2; }
    }
    /* only a number that parsed is warned about */
    if (fm_nf.debug && (isig > 18 || fsig > 18)) {
        fflush(stdout);
        fprintf(stderr, "%s: large input value %s: possible precision loss\n",
                su_short, fm_nf_q(text));
    }
    *val = v;
    return FM_NF_OK;
}

/* report a conversion failure as --invalid says; 0 to go on */
static void fm_nf_fail(const char *msg) {
    fm_nf.failed = 1;
    if (fm_nf.invalid == FM_NF_IGNORE) return;
    fflush(stdout);
    fprintf(stderr, "%s: %s\n", su_short, msg);
    if (fm_nf.invalid == FM_NF_ABORT) exit(2);
}

/* the number V with input precision IPREC as it is written: into SB, or
 * 0 when it cannot be (the message given) */
static int fm_nf_render(struct sn_sb *sb, long double v, long iprec) {
    char msg[512];
    if (fm_nf.to_unit != 1) v /= (long double)fm_nf.to_unit;
    long uprec = fm_nf.fmt_prec;
    if (fm_nf.to == FM_NF_NONE) {
        long prec = uprec >= 0 ? uprec : iprec;
        if (prec > 18 || fm_nf_absl(v) >= fm_nf_p10(19 - prec)) {
            if (prec)
                snprintf(msg, sizeof msg, "value/precision too large to be printed: '%s/%lu' (consider using --to)",
                         fm_nf_lg(v), (unsigned long)prec);
            else
                snprintf(msg, sizeof msg, "value too large to be printed: '%s' (consider using --to)",
                         fm_nf_lg(v));
            fm_nf_fail(msg);
            return 0;
        }
        v = fm_nf_round_prec(v, prec, fm_nf.round);
        fm_nf_put_fixed(sb, v, (int)prec);
        return 1;
    }
    if (fm_nf_absl(v) >= fm_nf_p10(33)) {
        snprintf(msg, sizeof msg, "value too large to be printed: '%s' (cannot handle values > 999Q)",
                 fm_nf_lg(v));
        fm_nf_fail(msg);
        return 0;
    }
    long double base = fm_nf.to == FM_NF_SI ? 1000 : 1024;
    int power = 0;
    while (fm_nf_absl(v) >= base && power < 10) { v /= base; power++; }
    int ip = (int)uprec;       /* GNU passes the precision on as an int */
    /* a format's precision rounds at no more than three decimals a
     * power (measured: --to=si --format=%.5f 1000.0001 is 1.00100k, and
     * 12.04 at %.2f is 13.00); without one, one decimal below 10 */
    long digits = uprec >= 0 ? (ip < 3 * power ? ip : 3 * power)
                             : (fm_nf_absl(v) < 10 ? 1 : 0);
    v = fm_nf_round_prec(v, digits, fm_nf.round);
    if (fm_nf_absl(v) >= base) { v /= base; power++; }
    int pprec = uprec >= 0 ? ip : (power > 0 && fm_nf_absl(v) < 10 ? 1 : 0);
    fm_nf_put_fixed(sb, v, pprec);
    if (power > 0) {
        static const char letters[] = "KMGTPEZYRQ";
        if (power > 10) sn_puts(sb, "(error)");
        else if (power == 1 && fm_nf.to == FM_NF_SI) sn_putc(sb, 'k');
        else sn_putc(sb, letters[power - 1]);
        if (fm_nf.to == FM_NF_IECI) sn_putc(sb, 'i');
    }
    return 1;
}

/* one selected field, [S, S+LEN), the Nth of its line, REST what follows
 * its delimiter (0 at the end of the line): written converted, or as it
 * was.
 *
 * GNU reads a field in place: the delimiter after it becomes a NUL, so
 * does the first byte of a --suffix it ends in, and the line itself ends
 * in two (its newline made one). Its parser can step past the first NUL
 * (see fm_nf_parse), so the text parsed here is laid out the same way:
 * the field, NUL, the rest of the suffix, NUL, the rest of the line, NUL,
 * NUL. (An operand or an unterminated last line has nothing defined after
 * it in GNU -- it reads on into whatever memory follows; here it is NUL.) */
static void fm_nf_field(const char *s, size_t len, size_t n, const char *rest) {
    size_t tlen = len;
    size_t slen = fm_nf.suffix ? strlen(fm_nf.suffix) : 0;
    if (slen && tlen > slen && !memcmp(s + tlen - slen, fm_nf.suffix, slen)) tlen -= slen;
    const char *t = s;
    size_t skip = 0;
    while (skip < tlen && (t[skip] == ' ' || t[skip] == '\t')) skip++;
    size_t tail = tlen < len ? len - tlen - 1 : 0;   /* the suffix after its first byte */
    size_t rlen = rest ? strlen(rest) : 0;
    char *text = (char *)malloc((tlen - skip) + 1 + tail + 1 + rlen + 2);
    if (!text) su_error(1, ENOMEM, "memory exhausted");
    {
        char *w = text;
        memcpy(w, t + skip, tlen - skip);
        w += tlen - skip;
        *w++ = 0;
        if (tlen < len) {
            memcpy(w, s + tlen + 1, tail);
            w += tail;
            *w++ = 0;
        }
        if (rlen) { memcpy(w, rest, rlen); w += rlen; }
        *w++ = 0;
        *w = 0;
    }
    long double v = 0;
    long prec = 0;
    const char *bad = 0;
    int rc = fm_nf_parse(text, &v, &prec, &bad);
    char msg[1024];
    struct sn_sb sb = { 0, 0, 0 };
    int ok = 0;
    switch (rc) {
    case FM_NF_OK:
        if (fm_nf.from_unit != 1) v *= (long double)fm_nf.from_unit;
        ok = fm_nf_render(&sb, v, prec);
        break;
    case FM_NF_E_NUMBER:
        snprintf(msg, sizeof msg, "invalid number: %s", fm_nf_q(text));
        fm_nf_fail(msg);
        break;
    case FM_NF_E_SUFFIX:
        snprintf(msg, sizeof msg, "invalid suffix in input: %s", fm_nf_q(text));
        fm_nf_fail(msg);
        break;
    case FM_NF_E_SUFFIX2:
        snprintf(msg, sizeof msg, "invalid suffix in input %s: %s", fm_nf_q(text), fm_nf_q(bad));
        fm_nf_fail(msg);
        break;
    case FM_NF_E_REJECT:
        snprintf(msg, sizeof msg, "rejecting suffix in input: %s (consider using --from)",
                 fm_nf_q(text));
        fm_nf_fail(msg);
        break;
    case FM_NF_E_MISSING_I:
        snprintf(msg, sizeof msg, "missing 'i' suffix in input: %s (e.g Ki/Mi/Gi)",
                 fm_nf_q(text));
        fm_nf_fail(msg);
        break;
    case FM_NF_E_CONVERT:
        snprintf(msg, sizeof msg, "value too large to be converted: %s", fm_nf_q(text));
        fm_nf_fail(msg);
        break;
    }
    free(text);
    if (!ok) {
        /* as GNU writes it back: without the --suffix its NUL cut off */
        fwrite(s, 1, tlen, stdout);
        free(sb.s);
        return;
    }
    if (fm_nf.suffix) sn_puts(&sb, fm_nf.suffix);
    /* the width: the format's, --padding's, or the field's own when it
     * has blanks before it (whitespace fields only). A format's width
     * with '0' is the zeros' alone, measured: --padding=8 --format=%05f
     * writes "   00001" */
    intmax_t pad = 0;
    if (fm_nf.fmt_width && !(fm_nf.fmt_zero && fm_nf.fmt_width > 0)) pad = fm_nf.fmt_width;
    else if (fm_nf.padding) pad = fm_nf.padding;
    else if (fm_nf.delim < 0 && (n > 1 || (len && fm_nf_is_sep((unsigned char)s[0])))) pad = (intmax_t)tlen;
    fputs(fm_nf.fmt_prefix ? fm_nf.fmt_prefix : "", stdout);
    uintmax_t w = pad < 0 ? (uintmax_t)0 - (uintmax_t)pad : (uintmax_t)pad;
    uintmax_t have = sb.n + fm_nf_zeros;
    if (have < fm_nf_zeros) have = UINTMAX_MAX;
    if (pad > 0 && w > have) fm_nf_fill(' ', w - have);
    size_t sign = sb.s[0] == '-';
    fwrite(sb.s, 1, sign, stdout);
    fm_nf_fill('0', fm_nf_zeros);
    fwrite(sb.s + sign, 1, sb.n - sign, stdout);
    if (pad < 0 && w > have) fm_nf_fill(' ', w - have);
    fputs(fm_nf.fmt_suffix ? fm_nf.fmt_suffix : "", stdout);
    free(sb.s);
}

/* one line (a C string), its fields converted; AFTER is what lies past
 * its NUL in GNU's memory: nothing for a line read (its newline was made
 * a second NUL), the next operand for an operand (they are contiguous) */
static void fm_nf_line(const char *line, const char *after) {
    const char *p = line;
    for (size_t n = 1;; n++) {
        const char *st = p;
        if (fm_nf.delim < 0) {
            while (*p && fm_nf_is_sep((unsigned char)*p)) p++;
            while (*p && !fm_nf_is_sep((unsigned char)*p)) p++;
        } else {
            while (*p && *p != (char)fm_nf.delim) p++;
        }
        if (fm_nf_selected(n)) fm_nf_field(st, (size_t)(p - st), n, *p ? p + 1 : after);
        else fwrite(st, 1, (size_t)(p - st), stdout);
        if (!*p) break;
        putchar(fm_nf.delim < 0 ? ' ' : fm_nf.delim);
        p++;
    }
}

static int do_numfmt(int argc, char **argv) {
    enum {
        FROM_OPT = 0x100, FROM_UNIT_OPT, TO_OPT, TO_UNIT_OPT, ROUND_OPT,
        SUFFIX_OPT, GROUPING_OPT, FIELD_OPT, DEBUG_OPT, PADDING_OPT,
        HEADER_OPT, FORMAT_OPT, INVALID_OPT
    };
    /* in GNU's order: an ambiguous abbreviation lists them so */
    static const struct su_option longs[] = {
        { "from", SU_REQ_ARG, 0, FROM_OPT },
        { "from-unit", SU_REQ_ARG, 0, FROM_UNIT_OPT },
        { "to", SU_REQ_ARG, 0, TO_OPT },
        { "to-unit", SU_REQ_ARG, 0, TO_UNIT_OPT },
        { "round", SU_REQ_ARG, 0, ROUND_OPT },
        { "suffix", SU_REQ_ARG, 0, SUFFIX_OPT },
        { "grouping", SU_NO_ARG, 0, GROUPING_OPT },
        { "delimiter", SU_REQ_ARG, 0, 'd' },
        { "field", SU_REQ_ARG, 0, FIELD_OPT },
        { "debug", SU_NO_ARG, 0, DEBUG_OPT },
        { "padding", SU_REQ_ARG, 0, PADDING_OPT },
        { "header", SU_OPT_ARG, 0, HEADER_OPT },
        { "format", SU_REQ_ARG, 0, FORMAT_OPT },
        { "invalid", SU_REQ_ARG, 0, INVALID_OPT },
        { "zero-terminated", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    static const char *const from_n[] = { "none", "auto", "si", "iec", "iec-i", 0 };
    static const int from_v[] = { FM_NF_NONE, FM_NF_AUTO, FM_NF_SI, FM_NF_IEC, FM_NF_IECI };
    static const char *const to_n[] = { "none", "si", "iec", "iec-i", 0 };
    static const int to_v[] = { FM_NF_NONE, FM_NF_SI, FM_NF_IEC, FM_NF_IECI };
    static const char *const round_n[] = { "up", "down", "from-zero", "towards-zero", "nearest", 0 };
    static const int round_v[] = { FM_NF_UP, FM_NF_DOWN, FM_NF_FROMZERO, FM_NF_TOWARDSZERO, FM_NF_NEAREST };
    static const char *const inv_n[] = { "abort", "fail", "warn", "ignore", 0 };
    static const int inv_v[] = { FM_NF_ABORT, FM_NF_FAIL, FM_NF_WARN, FM_NF_IGNORE };

    memset(&fm_nf, 0, sizeof fm_nf);
    fm_nf.from_unit = fm_nf.to_unit = 1;
    fm_nf.round = FM_NF_FROMZERO;
    fm_nf.delim = -1;
    fm_nf.fmt_prec = -1;
    const char *format = 0;
    int c, grouping_opt = 0;
    while ((c = su_getopt_long(argc, argv, "d:z", longs, 0)) != -1) {
        switch (c) {
        case FROM_OPT: fm_nf.from = fm_nf_argmatch("--from", su_optarg, from_n, from_v); break;
        case FROM_UNIT_OPT:
            if (fm_nf_unit_size(su_optarg, &fm_nf.from_unit))
                su_error(1, 0, "invalid unit size: %s", su_quote(su_optarg));
            break;
        case TO_OPT: fm_nf.to = fm_nf_argmatch("--to", su_optarg, to_n, to_v); break;
        case TO_UNIT_OPT:
            if (fm_nf_unit_size(su_optarg, &fm_nf.to_unit))
                su_error(1, 0, "invalid unit size: %s", su_quote(su_optarg));
            break;
        case ROUND_OPT: fm_nf.round = fm_nf_argmatch("--round", su_optarg, round_n, round_v); break;
        case SUFFIX_OPT: fm_nf.suffix = su_optarg; break;
        case GROUPING_OPT: fm_nf.grouping = grouping_opt = 1; break;
        case 'd':
            if (su_optarg[0] && su_optarg[1])
                su_error(1, 0, "the delimiter must be a single character");
            fm_nf.delim = (unsigned char)su_optarg[0];
            break;
        case FIELD_OPT:
            if (fm_nf.fields_given) su_error(1, 0, "multiple field specifications");
            fm_nf.fields_given = 1;
            fm_nf_fields(su_optarg);
            break;
        case DEBUG_OPT: fm_nf.debug = 1; break;
        case PADDING_OPT:
            if (fm_nf_imax(su_optarg, &fm_nf.padding) || fm_nf.padding == 0)
                su_error(1, 0, "invalid padding value %s", su_quote(su_optarg));
            break;
        case HEADER_OPT:
            if (su_optarg) {
                uintmax_t h;
                if (fm_nf_umax(su_optarg, &h) || h == 0 || h > SIZE_MAX)
                    su_error(1, 0, "invalid header value %s", su_quote(su_optarg));
                fm_nf.header = (size_t)h;
            } else fm_nf.header = 1;
            break;
        case FORMAT_OPT: format = su_optarg; break;
        case INVALID_OPT: fm_nf.invalid = fm_nf_argmatch("--invalid", su_optarg, inv_n, inv_v); break;
        case 'z': fm_nf.zero_term = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [NUMBER]...\n"
                   "Write each NUMBER -- or the numbers in the lines of standard input, when\n"
                   "there are none -- scaled to or from human-readable units.\n"
                   "\n"
                   "A long option's mandatory argument is mandatory for its short form too.\n"
                   "      --debug          warn about input that is odd or not converted\n"
                   "  -d, --delimiter=X    fields are separated by the character X, not blanks\n"
                   "      --field=FIELDS   convert the numbers in these fields (1 by default);\n"
                   "                         FIELDS is described below\n"
                   "      --format=FORMAT  write each number through a printf-like FORMAT\n"
                   "                         holding one floating-point directive (see below)\n"
                   "      --from=UNIT      read numbers scaled in UNITs (default: none)\n"
                   "      --from-unit=N    each input number counts N (default 1)\n"
                   "      --grouping       group digits as the locale says (nothing in C)\n"
                   "      --header[=N]     copy the first N lines (1 if N is not given) as\n"
                   "                         they are\n"
                   "      --invalid=MODE   what a number that does not convert does: abort\n"
                   "                         (the default), fail, warn or ignore\n"
                   "      --padding=N      make each number N characters wide, on the right\n"
                   "                         for N > 0 and on the left for N < 0; by default\n"
                   "                         a field with blanks before it keeps its width\n"
                   "      --round=METHOD   how scaled values are rounded: up, down,\n"
                   "                         from-zero (the default), towards-zero, nearest\n"
                   "      --suffix=SUFFIX  write SUFFIX after each number, and allow it\n"
                   "                         after the input numbers\n"
                   "      --to=UNIT        write numbers scaled to UNITs (see below)\n"
                   "      --to-unit=N      each output number counts N (default 1)\n"
                   "  -z, --zero-terminated  lines end in NUL, not newline\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "UNIT is one of:\n"
                   "  none   no scaling; a unit letter in the input is an error\n"
                   "  auto   one letter is decimal (1K = 1000), a letter and 'i' is binary\n"
                   "           (1Ki = 1024)\n"
                   "  si     one letter, decimal: 1K = 1k = 1000, 1M = 1000000, ...\n"
                   "  iec    one letter, binary: 1K = 1k = 1024, 1M = 1048576, ...\n"
                   "  iec-i  a letter and 'i', binary: 1Ki = 1024, 1Mi = 1048576, ...\n"
                   "\n"
                   "FIELDS is a list, as cut's: N (field N, from 1), N- (N to the end of the\n"
                   "line), N-M (N to M), -M (1 to M) or - (all), separated by commas.\n"
                   "\n"
                   "FORMAT has one %%f, optionally %%'f (grouping), a width (%%10f; %%010f pads\n"
                   "with zeros, %%-10f aligns left) and a precision (%%.1f) that replaces the\n"
                   "one the input implies.\n"
                   "\n"
                   "The exit status is 0 when every number converted. A number that does\n"
                   "not stops the command with status 2 (abort); with --invalid=fail each\n"
                   "is reported and the status is 2; with warn each is reported and the\n"
                   "status is 0; with ignore nothing is said and the status is 0.\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    /* measured order: --grouping against --format before the format is
     * read, its grouping against --to after */
    if (grouping_opt && format)
        su_error(1, 0, "--grouping cannot be combined with --format");
    if (format) fm_nf_format(format);
    if (fm_nf.debug && fm_nf.padding && fm_nf.fmt_width &&
        !(fm_nf.fmt_zero && fm_nf.fmt_width > 0))
        su_error(0, 0, "--format padding overriding --padding");
    if (fm_nf.grouping && fm_nf.to != FM_NF_NONE)
        su_error(1, 0, "grouping cannot be combined with --to");
    if (fm_nf.debug) {
        /* the C locale groups nothing */
        if (fm_nf.grouping) su_error(0, 0, "grouping has no effect in this locale");
        if (fm_nf.from == FM_NF_NONE && fm_nf.to == FM_NF_NONE && !fm_nf.padding &&
            !format && !fm_nf.grouping)
            su_error(0, 0, "no conversion option specified");
    }
    if (!fm_nf.fields_given) {
        fm_nf.fields = (struct fm_nf_range *)malloc(sizeof *fm_nf.fields);
        if (!fm_nf.fields) su_error(1, ENOMEM, "memory exhausted");
        fm_nf.fields[0].lo = fm_nf.fields[0].hi = 1;
        fm_nf.nfields = 1;
    }
    int eol = fm_nf.zero_term ? 0 : '\n';
    if (su_optind < argc) {
        if (fm_nf.debug && fm_nf.header)
            su_error(0, 0, "--header ignored with command-line input");
        for (int i = su_optind; i < argc; i++) {
            fm_nf_line(argv[i], i + 1 < argc ? argv[i + 1] : "");
            putchar(eol);
        }
    } else {
        char *line = 0;
        size_t cap = 0;
        ssize_t n;
        size_t lineno = 0;
        while ((n = getdelim(&line, &cap, eol, stdin)) > 0) {
            lineno++;
            if (lineno <= fm_nf.header) {
                /* as GNU writes a header: up to a NUL, ending and all */
                fputs(line, stdout);
                continue;
            }
            int had = line[n - 1] == (char)eol;
            if (had) line[n - 1] = 0;
            fm_nf_line(line, "");
            if (had) putchar(eol);
        }
        if (ferror(stdin)) su_error(1, errno, "error reading input");
        free(line);
    }
    if (fm_nf.debug && fm_nf.failed)
        su_error(0, 0, "failed to convert some of the input numbers");
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return fm_nf.failed && fm_nf.invalid != FM_NF_WARN && fm_nf.invalid != FM_NF_IGNORE ? 2 : 0;
}
