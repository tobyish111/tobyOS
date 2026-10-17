/* su_printf.c -- printf FORMAT [ARGUMENT]..., as GNU coreutils' printf
 * (logs/utilgate/printf.sh). This is /bin/printf, the one `env printf`,
 * `xargs printf` and a #! line reach; tsh's printf builtin is bash's.
 *
 * A directive is %[N$][FLAGS][WIDTH][.PRECISION][LENGTH]CONVERSION:
 *   N$          take the argument by number; a pass of the format then
 *               uses as many arguments as the highest N, or as the
 *               directives without one took, whichever is more
 *   FLAGS       - + space # 0 ' I, each only where GNU takes it: # not on
 *               d i u c s, 0 not on c s, ' and I only on d i u f F g G
 *   WIDTH       digits, *, or *N$ -- an argument; a negative one means -
 *   PRECISION   the same after a '.'; a negative argument means none
 *   LENGTH      any of h l L j z t, read and ignored
 *   CONVERSION  d i o u x X, f F e E g G a A, c s, and two of GNU's own:
 *               b, the argument with its backslash escapes done, and q,
 *               the argument quoted for a shell -- these two take no
 *               flags, width, precision or length.
 * Anything else ("%z", "%5b", "%#s", a % at the end) is "invalid
 * conversion specification", and printf stops there, status 1.
 *
 * The format is used again while arguments remain and the last pass used
 * any; when a pass used none, the rest are "ignoring excess arguments".
 * A missing argument is 0, "", or for %c a NUL byte.
 *
 * Numbers: d i as strtoimax reads them and o u x X as strtoumax (so -1
 * is 2^64-1), base 0 -- 0x hex, 0 octal; floats as strtold, in x87 long
 * double, read and printed exactly (su_num.c); 'C or "C as the code of
 * the byte C. A bad one is reported -- "expected a numeric value", "value
 * not completely converted", out of range -- printing what was read, and
 * printf goes on, to exit 1.
 *
 * Escapes, in the format: \" \\ \a \b \e \f \n \r \t \v, \NNN (one to
 * three octal digits), \xHH (one or two), \uHHHH and \UHHHHHHHH (a code
 * point: a byte below 128; past that this C locale has no character for
 * it, and it is written back as \uXXXX), and \c, which ends printf on
 * the spot, status 0. Any other \X stays as it is. %b's argument takes
 * the same, and \0NNN as octal too.
 */

static int pf_status;       /* 1 once an argument would not convert */
static int pf_wfail;        /* a directive too wide to write: "write error" */

static void pf_end(int status) {
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    if (pf_wfail) su_error(1, 0, "write error");
    exit(status);
}

static int pf_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* the escape after a backslash at P, written out; returns what follows.
 * IN_B: %b's argument, where \0 starts up to three more octal digits. */
static const char *pf_escape(const char *p, int in_b) {
    int c = (unsigned char)*p;
    static const char named[] = "a\ab\be\033f\fn\nr\rt\tv\v\"\"\\\\";
    for (const char *n = named; *n; n += 2)
        if (c == n[0]) { putchar(n[1]); return p + 1; }
    if (c == 'c') pf_end(0);
    if (c == 'x') {
        int v = 0, k = 0;
        for (p++; k < 2 && pf_hexval(*p) >= 0; p++, k++) v = v * 16 + pf_hexval(*p);
        if (!k) su_error(1, 0, "missing hexadecimal number in escape");
        putchar(v);
        return p;
    }
    if (c == 'u' || c == 'U') {
        int want = c == 'u' ? 4 : 8;
        unsigned long v = 0;
        p++;
        for (int k = 0; k < want; k++, p++) {
            if (pf_hexval(*p) < 0) su_error(1, 0, "missing hexadecimal number in escape");
            v = v * 16 + (unsigned long)pf_hexval(*p);
        }
        if (v >= 0xd800 && v <= 0xdfff)
            su_error(1, 0, "invalid universal character name \\%c%0*lx", c, want, v);
        if (v < 0x80) putchar((int)v);
        else printf("\\%c%0*lX", c, want, v);
        return p;
    }
    if (c >= '0' && c <= '7') {
        int v = 0, k = 0;
        if (in_b && c == '0') p++;
        for (; k < 3 && *p >= '0' && *p <= '7'; p++, k++) v = v * 8 + (*p - '0');
        putchar(v & 0xff);
        return p;
    }
    putchar('\\');
    if (!c) return p;           /* a backslash at the very end */
    putchar(c);
    return p + 1;
}

/* ---- %q ----------------------------------------------------------------
 *
 * S as a shell reads it back, the way GNU's shell-escape quoting writes it
 * (measured byte by byte, logs/b30/qtable*-gnu.out):
 *   - as it is, when nothing in it needs quoting: letters, digits and
 *     % + , - . / : @ ] _ never do; # and ~ do only first; { and } only
 *     alone; every other punctuation mark and the space always do;
 *   - in "...", when it holds a ' and the rest is only letters, digits,
 *     the space and % + , - . / : @ ] _ (# and ~ first);
 *   - else in '...', a ' in it as '\'' -- and each run of control or
 *     non-ASCII bytes as $'...' between the quoted runs: \a \b \t \n \v
 *     \f \r by name, any other as three octal digits. ""  is ''. */
static int pf_ctl(unsigned char c) { return c < 0x20 || c >= 0x7f; }

static void pf_shquote(const char *s) {
    size_t n = strlen(s);
    if (!n) { fputs("''", stdout); return; }
    int needs = 0, squote = 0, ctl = 0, dq_ok = 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int plain = isalnum(c) || strchr("%+,-./:@]_", c);
        if ((c == '#' || c == '~') && i > 0) plain = 1;
        if ((c == '{' || c == '}') && n > 1) plain = 1;
        if (pf_ctl(c)) { ctl = 1; plain = 0; }
        if (!plain) needs = 1;
        if (c == '\'') squote = 1;
        else if (!(isalnum(c) || c == ' ' || strchr("%+,-./:@]_", c) || ((c == '#' || c == '~') && i == 0)))
            dq_ok = 0;
    }
    if (!needs) { fputs(s, stdout); return; }
    if (squote && !ctl && dq_ok) { printf("\"%s\"", s); return; }
    int in_q = 1;
    putchar('\'');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (pf_ctl(c)) {
            if (in_q) { putchar('\''); in_q = 0; }
            fputs("$'", stdout);
            for (; i < n && pf_ctl((unsigned char)s[i]); i++) {
                static const char nm[] = "\aa\bb\tt\nn\vv\ff\rr";
                const char *k = s[i] ? strchr(nm, s[i]) : 0;
                if (k && (k - nm) % 2 == 0) printf("\\%c", k[1]);
                else printf("\\%03o", (unsigned char)s[i]);
            }
            i--;
            putchar('\'');
        } else if (c == '\'') {
            if (in_q) putchar('\'');
            fputs("\\''", stdout);
            in_q = 1;
        } else {
            if (!in_q) { putchar('\''); in_q = 1; }
            putchar(c);
        }
    }
    if (in_q) putchar('\'');
}

/* ---- numbers ---------------------------------------------------------- */

/* Base 0 as strtoumax reads it: blanks, a sign, 0x hex, 0 octal, else
 * decimal. *END after the digits, S when there were none; *OVER when the
 * magnitude passed 2^64-1. */
static uintmax_t pf_mag(const char *s, const char **end, int *neg, int *over) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    *neg = 0;
    *over = 0;
    if (*p == '+' || *p == '-') { *neg = *p == '-'; p++; }
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && pf_hexval(p[2]) >= 0) { base = 16; p += 2; }
    else if (p[0] == '0') base = 8;
    int d = pf_hexval(*p);
    if (d < 0 || d >= base) { *end = s; return 0; }
    uintmax_t v = 0;
    for (; (d = pf_hexval(*p)) >= 0 && d < base; p++) {
        if (v > (UINTMAX_MAX - (uintmax_t)d) / (uintmax_t)base) *over = 1;
        else v = v * (uintmax_t)base + (uintmax_t)d;
    }
    *end = p;
    return *over ? UINTMAX_MAX : v;
}

/* 'C or "C: the code of the byte C, the rest warned about */
static int pf_char_const(const char *s, uintmax_t *v) {
    if ((s[0] != '\'' && s[0] != '"') || !s[1]) return 0;
    *v = (unsigned char)s[1];
    if (s[2])
        su_error(0, 0, "warning: %s: character(s) following character constant have been ignored", s + 2);
    return 1;
}

static void pf_verify(const char *s, const char *end, int range) {
    if (range) su_error(0, ERANGE, "%s", su_quote(s));
    else if (end == s) su_error(0, 0, "%s: expected a numeric value", su_quote(s));
    else if (*end) su_error(0, 0, "%s: value not completely converted", su_quote(s));
    else return;
    pf_status = 1;
}

static intmax_t pf_imax(const char *s) {
    uintmax_t c;
    if (!s) return 0;
    if (pf_char_const(s, &c)) return (intmax_t)c;
    const char *end;
    int neg, over;
    uintmax_t m = pf_mag(s, &end, &neg, &over);
    intmax_t v;
    if (neg) {
        if (m > (uintmax_t)INTMAX_MAX + 1) { over = 1; v = INTMAX_MIN; }
        else v = m == (uintmax_t)INTMAX_MAX + 1 ? INTMAX_MIN : -(intmax_t)m;
    } else {
        if (m > (uintmax_t)INTMAX_MAX) { over = 1; v = INTMAX_MAX; }
        else v = (intmax_t)m;
    }
    pf_verify(s, end, over);
    return v;
}

static uintmax_t pf_umax(const char *s) {
    uintmax_t c;
    if (!s) return 0;
    if (pf_char_const(s, &c)) return c;
    const char *end;
    int neg, over;
    uintmax_t m = pf_mag(s, &end, &neg, &over);
    pf_verify(s, end, over);
    return over ? UINTMAX_MAX : neg ? (uintmax_t)0 - m : m;
}

static long double pf_ld(const char *s) {
    uintmax_t c;
    if (!s) return 0;
    if (pf_char_const(s, &c)) return (long double)c;
    char *end;
    long double v = su_strtold(s, &end);
    pf_verify(s, end, sn_range);
    return v;
}

/* ---- one directive ---------------------------------------------------- */

struct pf_spec {
    int left, plus, space, alt, zero;
    int width;              /* 0 none */
    int prec;               /* -1 none */
    char conv;
};

/* glibc refuses a field whose width or precision is near INT_MAX (it
 * prints nothing for it, and the error is a "write error" at exit) */
#define PF_HUGE (INT_MAX - 32)

static void pf_pad(int n, int c) { while (n-- > 0) putchar(c); }

static void pf_int(const struct pf_spec *f, int neg, uintmax_t mag) {
    char dig[80];
    int nd = 0;
    char conv = f->conv;
    unsigned base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X') ? 16 : 10;
    const char *hx = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    for (uintmax_t v = mag; v; v /= base) dig[nd++] = hx[v % base];
    /* digits, most significant first, at least PREC of them */
    int prec = f->prec < 0 ? 1 : f->prec;
    int zeros = prec > nd ? prec - nd : 0;
    if (conv == 'o' && f->alt && zeros == 0 && (nd == 0 || dig[nd - 1] != '0')) zeros = 1;
    char pre[3] = { 0 };
    if (conv == 'd' || conv == 'i') {
        if (neg) pre[0] = '-';
        else if (f->plus) pre[0] = '+';
        else if (f->space) pre[0] = ' ';
    } else if ((conv == 'x' || conv == 'X') && f->alt && mag) {
        pre[0] = '0';
        pre[1] = conv;
    }
    long len = (long)strlen(pre) + zeros + nd;
    int pad = f->width > len ? f->width - (int)len : 0;
    if (f->zero && !f->left && f->prec < 0) { zeros += pad; pad = 0; }
    if (!f->left) pf_pad(pad, ' ');
    fputs(pre, stdout);
    pf_pad(zeros, '0');
    while (nd > 0) putchar(dig[--nd]);
    if (f->left) pf_pad(pad, ' ');
}

static void pf_str(const struct pf_spec *f, const char *s, size_t len) {
    if (f->prec >= 0 && (size_t)f->prec < len) len = (size_t)f->prec;
    int pad = (size_t)f->width > len ? f->width - (int)len : 0;
    if (!f->left) pf_pad(pad, ' ');
    fwrite(s, 1, len, stdout);
    if (f->left) pf_pad(pad, ' ');
}

static void pf_float(const struct pf_spec *f, long double v) {
    struct su_fmtspec fs;
    fs.left = f->left;
    fs.plus = f->plus;
    fs.space = f->space;
    fs.alt = f->alt;
    fs.zero = f->zero;
    fs.width = f->width;
    fs.prec = f->prec;
    fs.conv = f->conv;
    struct sn_sb b = { 0, 0, 0 };
    su_ldfmt(&b, v, &fs);
    fwrite(b.s, 1, b.n, stdout);
    free(b.s);
}

/* the flags each conversion takes ('I' and '\'' are glibc's and change
 * nothing in this locale) */
static int pf_flag_ok(char conv, char flag) {
    switch (conv) {
    case 'd': case 'i': case 'u':
        return flag != '#';
    case 'o': case 'x': case 'X': case 'e': case 'E': case 'a': case 'A':
        return flag != '\'' && flag != 'I';
    case 'f': case 'F': case 'g': case 'G':
        return 1;
    case 'c': case 's':
        return flag == '-' || flag == '+' || flag == ' ';
    }
    return 0;
}

/* %N$ or *N$ at *P: N (1 or more), *P moved past the $; else 0 */
static int pf_dollar(const char **pp) {
    const char *p = *pp;
    if (*p < '1' || *p > '9') return 0;
    long n = 0;
    for (; isdigit((unsigned char)*p); p++) if (n < INT_MAX / 10) n = n * 10 + (*p - '0'); else n = INT_MAX;
    if (*p != '$') return 0;
    *pp = p + 1;
    return (int)n;
}

static const char *pf_arg(char **args, int n, int k) { return k < n ? args[k] : 0; }

/* one pass of FMT over the N arguments at ARGS; returns how many it used */
static int pf_pass(const char *fmt, char **args, int n) {
    int next = 0, high = 0;
    for (const char *p = fmt; *p;) {
        if (*p == '\\') { p = pf_escape(p + 1, 0); continue; }
        if (*p != '%') { putchar(*p++); continue; }
        const char *start = p++;
        if (*p == '%') { putchar('%'); p++; continue; }
        struct pf_spec f;
        memset(&f, 0, sizeof f);
        f.prec = -1;
        int pos = pf_dollar(&p);
        char flags[16];
        int nf = 0;
        for (; *p && strchr("-+ #0'I", *p); p++) {
            if (nf < (int)sizeof flags) flags[nf++] = *p;
            if (*p == '-') f.left = 1;
            else if (*p == '+') f.plus = 1;
            else if (*p == ' ') f.space = 1;
            else if (*p == '#') f.alt = 1;
            else if (*p == '0') f.zero = 1;
        }
        int wstar = 0, wpos = 0, has_w = 0;
        long wlit = 0;
        if (*p == '*') { p++; wstar = 1; has_w = 1; wpos = pf_dollar(&p); }
        else for (; isdigit((unsigned char)*p); p++) {
            has_w = 1;
            wlit = wlit < INT_MAX / 10 ? wlit * 10 + (*p - '0') : INT_MAX;
        }
        int has_p = 0, pstar = 0, ppos = 0;
        long plit = 0;
        if (*p == '.') {
            p++;
            has_p = 1;
            if (*p == '*') { p++; pstar = 1; ppos = pf_dollar(&p); }
            else for (; isdigit((unsigned char)*p); p++)
                plit = plit < INT_MAX / 10 ? plit * 10 + (*p - '0') : INT_MAX;
        }
        int has_len = 0;
        for (; *p && strchr("hlLjzt", *p); p++) has_len = 1;
        char conv = *p;
        int ok = conv && strchr("diouxXfFeEgGaAcsbq", conv);
        if (ok && (conv == 'b' || conv == 'q'))
            ok = !nf && !has_w && !has_p && !has_len;
        for (int i = 0; ok && i < nf; i++) ok = pf_flag_ok(conv, flags[i]);
        if (ok && conv == 'c' && has_p) ok = 0;
        if (!ok)
            su_error(1, 0, "%.*s: invalid conversion specification",
                     (int)(p - start) + (conv ? 1 : 0), start);
        p++;
        f.conv = conv;
        int fail = 0;
        if (wstar) {
            const char *a;
            if (wpos) { a = pf_arg(args, n, wpos - 1); if (wpos > high) high = wpos; }
            else a = pf_arg(args, n, next++);
            intmax_t w = pf_imax(a);
            if (w < INT_MIN || w > INT_MAX) su_error(1, 0, "invalid field width: %s", su_quote(a));
            if (w < 0) { f.left = 1; w = -w; }
            if (w >= PF_HUGE) fail = 1;
            else f.width = (int)w;
        } else {
            if (wlit >= PF_HUGE) fail = 1;
            else f.width = (int)wlit;
        }
        if (pstar) {
            const char *a;
            if (ppos) { a = pf_arg(args, n, ppos - 1); if (ppos > high) high = ppos; }
            else a = pf_arg(args, n, next++);
            intmax_t pr = pf_imax(a);
            if (pr > INT_MAX) su_error(1, 0, "invalid precision: %s", su_quote(a));
            if (pr >= PF_HUGE) fail = 1;
            else f.prec = pr < 0 ? -1 : (int)pr;
        } else if (has_p) {
            if (plit >= PF_HUGE) fail = 1;
            else f.prec = (int)plit;
        }
        const char *a;
        if (pos) { a = pf_arg(args, n, pos - 1); if (pos > high) high = pos; }
        else a = pf_arg(args, n, next++);
        switch (conv) {
        case 'd': case 'i': {
            intmax_t v = pf_imax(a);
            if (fail) break;
            pf_int(&f, v < 0, v < 0 ? (uintmax_t)0 - (uintmax_t)v : (uintmax_t)v);
            break;
        }
        case 'o': case 'u': case 'x': case 'X': {
            uintmax_t v = pf_umax(a);
            if (!fail) pf_int(&f, 0, v);
            break;
        }
        case 'c': {
            char ch = a ? a[0] : 0;
            if (!fail) pf_str(&f, &ch, 1);
            break;
        }
        case 's':
            if (!fail) pf_str(&f, a ? a : "", a ? strlen(a) : 0);
            break;
        case 'b':
            for (const char *q = a ? a : ""; *q;) {
                if (*q == '\\') q = pf_escape(q + 1, 1);
                else putchar(*q++);
            }
            break;
        case 'q':
            if (a) pf_shquote(a);
            break;
        default: {
            long double v = pf_ld(a);
            if (!fail) pf_float(&f, v);
            break;
        }
        }
        if (fail) pf_wfail = 1;
    }
    return next > high ? next : high;
}

static int do_printf(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        printf("Usage: %s FORMAT [ARGUMENT]...\n"
               "  or:  %s OPTION\n"
               "Write each ARGUMENT as FORMAT says, as C's printf would, using FORMAT\n"
               "again while ARGUMENTs remain.\n"
               "\n"
               "      --help     show this help and exit\n"
               "      --version  show the version and exit\n"
               "\n"
               "Escapes in FORMAT:\n"
               "  \\\" \\\\ \\a \\b \\e \\f \\n \\r \\t \\v   the usual characters\n"
               "  \\NNN        the byte whose octal value is NNN (one to three digits)\n"
               "  \\xHH        the byte whose hexadecimal value is HH (one or two)\n"
               "  \\uHHHH      the character with code point HHHH (\\UHHHHHHHH: eight)\n"
               "  \\c          stop: write nothing more\n"
               "\n"
               "Directives: %%%% is a %%; %%d %%i %%o %%u %%x %%X take an integer, %%f %%F %%e\n"
               "%%E %%g %%G %%a %%A a floating number, %%c a character and %%s a string, with\n"
               "C's flags, width and precision. %%b writes its ARGUMENT with the escapes\n"
               "above done (and \\0NNN as octal), %%q writes it quoted so a shell reads\n"
               "it back as it was. A numeric ARGUMENT may be 'C or \"C: the code of C.\n"
               "\n"
               "Your shell may have its own printf, which this is not; run this one\n"
               "as env printf, or by its path.\n", su_name, su_name);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--version")) return su_version();
    int i = 1;
    if (i < argc && !strcmp(argv[i], "--")) i++;
    if (i >= argc) {
        su_error(0, 0, "missing operand");
        su_try_help(1);
    }
    const char *fmt = argv[i++];
    char **args = argv + i;
    int n = argc - i;
    int used;
    do {
        used = pf_pass(fmt, args, n);
        args += used;
        n -= used;
    } while (used > 0 && n > 0);
    if (n > 0) su_error(0, 0, "warning: ignoring excess arguments, starting with %s", su_quote(args[0]));
    pf_end(pf_status);
    return pf_status;
}
