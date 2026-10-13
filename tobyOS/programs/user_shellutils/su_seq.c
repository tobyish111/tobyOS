/* su_seq.c -- seq, as GNU coreutils': numbers from FIRST (1) by INCREMENT
 * (1) to LAST, measured on coreutils 9.7 (logs/utilgate/seq.sh).
 *
 *   - Counted exactly in decimal, any size, when GNU's seq counts so --
 *     no -w, no -f, a one-character separator, INCREMENT whole and at
 *     most 200, and either every operand plain digits (counted from the
 *     text: `seq 99999999999999999999 100000000000000000002`) or every
 *     operand integral and FIRST not negative (counted from the values
 *     printed whole: `seq 1e20 2e20` steps by exactly 1, `seq 1e400
 *     1e400` prints the one number).
 *   - Anything else is long double, as GNU's -- integers too, rounding
 *     and all: x = FIRST + i * INCREMENT,
 *     printed "%.PLf" where P is the most digits after the point (an
 *     exponent counted in) of FIRST and INCREMENT -- LAST's never counts --
 *     or "%Lg" when either is hex, inf or nan. The number just past LAST
 *     is printed after all when it prints as LAST does, as GNU's (so `seq
 *     0 0.1 1` ends at 1.0). The conversions are exact (su_num.c).
 *   - -w pads with zeros to the widest of FIRST and LAST as written,
 *     leading zeros counted, at the precision used; -s STRING between the
 *     numbers, a newline after the last; -f FORMAT one %a %e %f %g (any
 *     case) with flags, width and precision, and GNU's complaints about
 *     any other.
 *   - Options stop at the first operand, and an argument like -3 or -.5
 *     is an operand.
 *
 * It parsed whole numbers only (`seq 1 0.5 2` said "increment must not
 * be zero"), took -w and printed no padding, had no -f, overflowed past
 * 2^63, and gave none of GNU's messages. */

struct seq_arg {
    long double v;
    int width;
    int prec;                   /* INT_MAX: not known (hex, inf) */
    int isint;                  /* [+-]digits */
    int neg;                    /* written with a minus */
    const char *digits;         /* isint: the digits */
};

static void seq_scan(const char *arg, struct seq_arg *a) {
    char *end;
    a->v = su_strtold(arg, &end);
    const char *q = end;
    while (isspace((unsigned char)*q)) q++;
    if (end == arg || *q || !*arg) {
        su_error(0, 0, "invalid floating point argument: %s", su_quote(arg));
        su_try_help(1);
    }
    if (a->v != a->v) {
        su_error(0, 0, "invalid 'not-a-number' argument: %s", su_quote(arg));
        su_try_help(1);
    }
    const char *s = arg;
    while (isspace((unsigned char)*s) || *s == '+') s++;
    a->neg = *s == '-';
    a->isint = 0;
    {
        const char *d = s + (*s == '-' || *s == '+');
        size_t k = strspn(d, "0123456789");
        if (k && !d[k]) { a->isint = 1; a->digits = d; }
    }
    a->width = 0;
    a->prec = INT_MAX;
    union sn_ld u;
    u.v = a->v;
    int finite = (u.p.se & 0x7fff) != 0x7fff;
    if (!strpbrk(s, "xX") && finite) {
        const char *dp = strchr(s, '.');
        int frac = 0;
        const char *e = strpbrk(s, "eE");
        size_t mant = e ? (size_t)(e - s) : strlen(s);
        a->width = (int)mant;
        if (!dp) {
            a->prec = 0;
        } else {
            frac = (int)strcspn(dp + 1, "eE");
            a->prec = frac;
            if (frac == 0) a->width--;                       /* "5." prints 5 */
            if (dp == s || !isdigit((unsigned char)dp[-1])) a->width++;  /* ".5" 0.5 */
        }
        if (e) {
            long x = strtol(e + 1, 0, 10);
            if (x < 0) a->prec += (int)-x;
            else a->prec -= (int)(a->prec < x ? a->prec : x);
            /* the width the value takes printed fixed at its precision */
            struct su_fmtspec fs = { 0, 0, 0, 0, 0, -1, a->prec, 'f' };
            struct sn_sb b = { 0, 0, 0 };
            su_ldfmt(&b, a->v, &fs);
            a->width = (int)b.n;
            free(b.s);
        }
    }
}

/* ---- the exact integer path ---------------------------------------------- */

/* decimal magnitudes, no leading zeros ("0" for zero) */
static char *seq_norm(const char *d) {
    while (*d == '0' && d[1]) d++;
    char *s = strdup(d);
    if (!s) su_error(1, ENOMEM, "memory exhausted");
    return s;
}
static int seq_cmpmag(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return la < lb ? -1 : 1;
    int c = strcmp(a, b);
    return c < 0 ? -1 : c > 0;
}
static char *seq_addmag(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b), n = (la > lb ? la : lb) + 1;
    char *r = (char *)malloc(n + 1);
    if (!r) su_error(1, ENOMEM, "memory exhausted");
    int carry = 0;
    for (size_t i = 0; i < n; i++) {
        int x = (i < la ? a[la - 1 - i] - '0' : 0) + (i < lb ? b[lb - 1 - i] - '0' : 0) + carry;
        r[n - 1 - i] = (char)('0' + x % 10);
        carry = x / 10;
    }
    r[n] = 0;
    char *t = seq_norm(r);
    free(r);
    return t;
}
/* GNU's seq_fast: from A to B (decimal magnitudes) by INC (1..200) */
static int seq_count(const char *a0, const char *b0, unsigned inc, const char *sep) {
    char *x = seq_norm(a0), *z = seq_norm(b0);
    char step[8];
    snprintf(step, sizeof step, "%u", inc);
    int printed = 0;
    while (seq_cmpmag(x, z) <= 0) {
        if (printed) fputs(sep, stdout);
        fputs(x, stdout);
        printed = 1;
        char *nx = seq_addmag(x, step);
        free(x);
        x = nx;
        if (ferror(stdout)) break;
    }
    if (printed) putchar('\n');
    free(x);
    free(z);
    if (fflush(stdout) != 0) su_error(1, errno, "write error");
    return 0;
}

static int seq_digits(const char *s) {
    return *s && strspn(s, "0123456789") == strlen(s);
}

/* ---- the long double path ---------------------------------------------- */

struct seq_fmt {
    char *pre, *post;           /* the literal text, %% already % */
    struct su_fmtspec fs;
};

/* FORMAT checked as GNU checks it; 0, or exit with its message */
static void seq_parse_fmt(const char *fmt, struct seq_fmt *f) {
    struct sn_sb pre = { 0, 0, 0 }, post = { 0, 0, 0 };
    sn_puts(&pre, "");
    sn_puts(&post, "");
    const char *p = fmt;
    int found = 0;
    memset(&f->fs, 0, sizeof f->fs);
    f->fs.width = -1;
    f->fs.prec = -1;
    while (*p) {
        if (*p != '%') { sn_putc(found ? &post : &pre, *p++); continue; }
        if (p[1] == '%') { sn_putc(found ? &post : &pre, '%'); p += 2; continue; }
        if (found) su_error(1, 0, "format %s has too many %% directives", su_quote(fmt));
        p++;
        for (;; p++) {
            if (*p == '-') f->fs.left = 1;
            else if (*p == '+') f->fs.plus = 1;
            else if (*p == ' ') f->fs.space = 1;
            else if (*p == '#') f->fs.alt = 1;
            else if (*p == '0') f->fs.zero = 1;
            else if (*p == '\'') ;
            else break;
        }
        if (isdigit((unsigned char)*p)) {
            f->fs.width = 0;
            while (isdigit((unsigned char)*p)) f->fs.width = f->fs.width * 10 + (*p++ - '0');
        }
        if (*p == '.') {
            p++;
            f->fs.prec = 0;
            while (isdigit((unsigned char)*p)) f->fs.prec = f->fs.prec * 10 + (*p++ - '0');
        }
        if (!*p) su_error(1, 0, "format %s ends in %%", su_quote(fmt));
        if (!strchr("aAeEfFgG", *p))
            su_error(1, 0, "format %s has unknown %%%c directive", su_quote(fmt), *p);
        f->fs.conv = *p++;
        found = 1;
    }
    if (!found) su_error(1, 0, "format %s has no %% directive", su_quote(fmt));
    f->pre = pre.s;
    f->post = post.s;
}

static char *seq_str(const struct seq_fmt *f, long double v) {
    struct sn_sb b = { 0, 0, 0 };
    sn_puts(&b, f->pre);
    su_ldfmt(&b, v, &f->fs);
    sn_puts(&b, f->post);
    return b.s;
}

static int do_seq(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "format", SU_REQ_ARG, 0, 'f' },
        { "separator", SU_REQ_ARG, 0, 's' },
        { "equal-width", SU_NO_ARG, 0, 'w' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    const char *fmt = 0, *sep = "\n";
    int width = 0, c;
    for (;;) {
        /* an argument like -3 or -.5 is a number, not options */
        if (!su_cluster && su_nextind < argc && argv[su_nextind][0] == '-' &&
            (argv[su_nextind][1] == '.' || isdigit((unsigned char)argv[su_nextind][1]))) {
            su_optind = su_nextind;
            break;
        }
        c = su_getopt_long(argc, argv, "+f:s:w", longs, 0);
        if (c == -1) break;
        switch (c) {
        case 'f': fmt = su_optarg; break;
        case 's': sep = su_optarg; break;
        case 'w': width = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... LAST\n"
                   "  or:  %s [OPTION]... FIRST LAST\n"
                   "  or:  %s [OPTION]... FIRST INCREMENT LAST\n"
                   "Print the numbers from FIRST to LAST, INCREMENT apart; FIRST\n"
                   "and INCREMENT are 1 unless given.\n"
                   "\n"
                   "  -f, --format=FORMAT      print each with FORMAT, a printf\n"
                   "                           format with one %%a %%e %%f or %%g\n"
                   "  -s, --separator=STRING   put STRING between the numbers, not\n"
                   "                           a newline\n"
                   "  -w, --equal-width        pad with leading zeros to one width\n"
                   "      --help               show this help and exit\n"
                   "      --version            show the version and exit\n"
                   "\n"
                   "Numbers with no fractional part are counted exactly; others\n"
                   "print with as many places as FIRST or INCREMENT has.\n",
                   su_name, su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    int n = argc - su_optind;
    if (n < 1) {
        su_error(0, 0, "missing operand");
        su_try_help(1);
    }
    if (n > 3) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind + 3]));
        su_try_help(1);
    }
    if (fmt && width) {
        su_error(0, 0, "format string may not be specified when printing equal width strings");
        su_try_help(1);
    }
    struct seq_fmt F;
    if (fmt) seq_parse_fmt(fmt, &F);

    /* plain digits: counted from the text, before anything is read */
    if (!fmt && !width && strlen(sep) == 1) {
        char **o = argv + su_optind;
        const char *A = n == 1 ? "1" : o[0], *B = o[n - 1];
        const char *I = n == 3 ? o[1] : "1";
        if (seq_digits(A) && seq_digits(B) && seq_digits(I)) {
            char *e;
            long double iv = su_strtold(I, &e);
            if (iv > 0 && iv <= 200) return seq_count(A, B, (unsigned)iv, sep);
        }
    }

    struct seq_arg first = { 1.0L, 1, 0, 1, 0, "1" }, step = { 1.0L, 1, 0, 1, 0, "1" }, last;
    char **ops = argv + su_optind;
    if (n == 1) {
        seq_scan(ops[0], &last);
    } else {
        seq_scan(ops[0], &first);
        if (n == 3) {
            seq_scan(ops[1], &step);
            if (step.v == 0) {
                su_error(0, 0, "invalid Zero increment value: %s", su_quote(ops[1]));
                su_try_help(1);
            }
        }
        seq_scan(ops[n - 1], &last);
    }

    /* the precision and the width */
    int prec = (first.prec == INT_MAX || step.prec == INT_MAX) ? INT_MAX
             : (first.prec > step.prec ? first.prec : step.prec);
    int w = 0;
    if (width && prec != INT_MAX) {
        const struct seq_arg *ends[2] = { &first, &last };
        for (int i = 0; i < 2; i++) {
            const struct seq_arg *a = ends[i];
            if (a->prec == INT_MAX) continue;
            int aw = a->width + (prec - a->prec);
            if (a->prec == 0 && prec > 0) aw++;
            if (a->prec > 0 && prec == 0) aw--;
            if (aw > w) w = aw;
        }
    }

    union sn_ld fu;
    fu.v = first.v;
    if (!fmt && !width && strlen(sep) == 1 && first.prec == 0 && step.prec == 0 &&
        last.prec == 0 && first.v >= 0 && !(fu.p.se & 0x8000) &&
        step.v > 0 && step.v <= 200) {
        /* GNU's seq_fast again, from the values printed whole */
        struct su_fmtspec whole = { 0, 0, 0, 0, 0, -1, 0, 'f' };
        struct sn_sb a = { 0, 0, 0 }, b = { 0, 0, 0 };
        su_ldfmt(&a, first.v, &whole);
        su_ldfmt(&b, last.v, &whole);
        int r = seq_count(a.s, b.s, (unsigned)step.v, sep);
        free(a.s);
        free(b.s);
        return r;
    }

    if (!fmt) {
        memset(&F.fs, 0, sizeof F.fs);
        F.pre = (char *)"";
        F.post = (char *)"";
        F.fs.width = -1;
        F.fs.prec = -1;
        if (prec == INT_MAX) {
            F.fs.conv = 'g';
        } else {
            F.fs.conv = 'f';
            F.fs.prec = prec;
            if (w) { F.fs.zero = 1; F.fs.width = w; }
        }
    }
    long double a = first.v, st = step.v, z = last.v;
    if (st < 0 ? a < z : z < a) return 0;
    char *prev = seq_str(&F, a);
    fputs(prev, stdout);
    for (long double i = 1;; i++) {
        long double x = a + i * st;
        int out = st < 0 ? x < z : z < x;
        char *cur = seq_str(&F, x);
        if (out) {
            /* one past LAST that prints as LAST does -- rounding -- is LAST */
            char *e;
            long double back = su_strtold(cur, &e);
            int extra = back == z && strcmp(cur, prev) != 0;
            if (extra) {
                fputs(sep, stdout);
                fputs(cur, stdout);
            }
            free(cur);
            break;
        }
        fputs(sep, stdout);
        fputs(cur, stdout);
        free(prev);
        prev = cur;
        if (ferror(stdout)) break;
    }
    free(prev);
    putchar('\n');
    if (fflush(stdout) != 0) su_error(1, errno, "write error");
    return 0;
}
