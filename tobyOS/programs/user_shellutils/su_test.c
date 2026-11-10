/* su_test.c -- test and [, the binaries, as GNU coreutils' (logs/utilgate/
 * test.sh). tsh's builtins answer `test` in a script; these answer `env
 * test`, `find -exec test`, xargs and #! lines.
 *
 * THE GRAMMAR IS POSIX's ARGUMENT COUNTS FIRST, as GNU's is:
 *   0  false;  1  the argument is not empty;
 *   2  "! X", or a unary operator "-X OPERAND" ("'-q': unary operator
 *      expected" for a -X that is not one), else "missing argument after"
 *      the last argument;
 *   3  a binary operator in the middle, then "! X Y", then "( X )", then
 *      "X -a Y" / "X -o Y", else "'Y': binary operator expected";
 *   4  "! X Y Z" and "( X Y )", else the full grammar;
 *   5+ the full grammar: -o of -a of terms, a term being "!" TERM,
 *      "( ... )", "X binop Y", "-l STR binop Y", "-X OPERAND", or a string.
 * Inside "(" the arguments up to the next ")" -- at most four, else all
 * that are left -- are evaluated by the counts again: `( ( x ) )` is
 * "missing argument after ')'". Arguments left over are "extra argument".
 *
 * Integers are decimal strings of any length -- blanks around them, a sign
 * -- compared without converting ("invalid integer 'X'" else); "-l STR"
 * stands for STR's length. -a is not a unary operator here, as in GNU's
 * (bash's builtin has it). [ needs "]" last ("missing ']'"), and takes
 * --help and --version as its only argument; test takes neither.
 * Status 0 true, 1 false, 2 an error.
 *
 * The old test parsed by its own rules: most errors were "extra argument",
 * integers were bash's ("x: integer expression expected"), 20-digit ones
 * failed, and -l did not exist. */

static int    tt_argc, tt_pos;
static char **tt_argv;
static const char *tt_name;

static void tt_error(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void tt_error(const char *fmt, ...) {
    fflush(stdout);
    fprintf(stderr, "%s: ", tt_name);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static void tt_beyond(void) __attribute__((noreturn));
static void tt_beyond(void) {
    tt_error("missing argument after %s", su_quote(tt_argv[tt_argc - 1]));
}

static void tt_advance(int need_more) {
    tt_pos++;
    if (need_more && tt_pos >= tt_argc) tt_beyond();
}

static int tt_is_binop(const char *s) {
    static const char *const ops[] = { "=", "==", "!=", "<", ">", "-nt", "-ot", "-ef",
                                       "-eq", "-ne", "-lt", "-le", "-gt", "-ge", 0 };
    for (int i = 0; ops[i]; i++) if (!strcmp(s, ops[i])) return 1;
    return 0;
}

static int tt_is_unop(const char *s) {
    return s[0] == '-' && s[1] && !s[2] && strchr("bcdefgGhkLnOprsStuwxz", s[1]);
}

/* an integer: blanks, a sign, digits, blanks -- the digits found, the
 * sign in *neg; "invalid integer" otherwise */
static const char *tt_int(const char *s, int *neg, size_t *len) {
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    *neg = 0;
    if (*p == '-') { *neg = 1; p++; }
    else if (*p == '+') p++;
    const char *d = p;
    while (*p >= '0' && *p <= '9') p++;
    if (p == d) goto bad;
    const char *e = p;
    while (*p == ' ' || *p == '\t') p++;
    if (*p) goto bad;
    while (d < e - 1 && *d == '0') d++;            /* leading zeros */
    *len = (size_t)(e - d);
    if (*len == 1 && *d == '0') *neg = 0;          /* -0 is 0 */
    return d;
bad:
    tt_error("invalid integer %s", su_quote(s));
}

/* compare two integers as strings: <0, 0, >0 */
static int tt_intcmp(const char *a, int an, size_t al, const char *b, int bn, size_t bl) {
    if (an != bn) return an ? -1 : 1;
    int r;
    if (al != bl) r = al < bl ? -1 : 1;
    else { int c = memcmp(a, b, al); r = c < 0 ? -1 : c > 0; }
    return an ? -r : r;
}

static int tt_stat(const char *f, struct stat *st, int follow) {
    return follow ? stat(f, st) : lstat(f, st);
}

/* the operator at tt_pos, with "-l STR" possible on either side */
static int tt_binary(int l_is_l) {
    const char *l = tt_argv[tt_pos];
    char lbuf[32], rbuf[32];
    if (l_is_l) {
        snprintf(lbuf, sizeof lbuf, "%zu", strlen(tt_argv[tt_pos + 1]));
        l = lbuf;
        tt_pos++;
    }
    const char *op = tt_argv[tt_pos + 1];
    int intop = !strcmp(op, "-eq") || !strcmp(op, "-ne") || !strcmp(op, "-lt") ||
                !strcmp(op, "-le") || !strcmp(op, "-gt") || !strcmp(op, "-ge");
    /* "-l STR" on the right too, for the integer comparisons */
    int r_is_l = intop && tt_pos + 3 < tt_argc && !strcmp(tt_argv[tt_pos + 2], "-l");
    const char *r;
    if (r_is_l) {
        snprintf(rbuf, sizeof rbuf, "%zu", strlen(tt_argv[tt_pos + 3]));
        r = rbuf;
    } else {
        r = tt_argv[tt_pos + 2];
    }
    tt_pos += r_is_l ? 4 : 3;
    if (intop) {
        int an, bn;
        size_t al, bl;
        const char *a = tt_int(l, &an, &al), *b = tt_int(r, &bn, &bl);
        int c = tt_intcmp(a, an, al, b, bn, bl);
        if (!strcmp(op, "-eq")) return c == 0;
        if (!strcmp(op, "-ne")) return c != 0;
        if (!strcmp(op, "-lt")) return c < 0;
        if (!strcmp(op, "-le")) return c <= 0;
        if (!strcmp(op, "-gt")) return c > 0;
        return c >= 0;
    }
    if (l_is_l) tt_error("%s: binary operator expected", su_quote(op)); /* -l STR = x */
    if (!strcmp(op, "=") || !strcmp(op, "==")) return strcmp(l, r) == 0;
    if (!strcmp(op, "!=")) return strcmp(l, r) != 0;
    if (!strcmp(op, "<")) return strcmp(l, r) < 0;
    if (!strcmp(op, ">")) return strcmp(l, r) > 0;
    struct stat sa, sb;
    int ea = stat(l, &sa) == 0, eb = stat(r, &sb) == 0;
    if (!strcmp(op, "-ef")) return ea && eb && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
    if (!strcmp(op, "-nt")) {
        if (!ea) return 0;
        if (!eb) return 1;
        return sa.st_mtime > sb.st_mtime;
    }
    /* -ot */
    if (!eb) return 0;
    if (!ea) return 1;
    return sa.st_mtime < sb.st_mtime;
}

static int tt_unary(void) {
    char op = tt_argv[tt_pos][1];
    tt_advance(1);
    const char *a = tt_argv[tt_pos];
    tt_pos++;
    struct stat st;
    switch (op) {
    case 'n': return a[0] != '\0';
    case 'z': return a[0] == '\0';
    case 't': {
        int neg;
        size_t len;
        const char *d = tt_int(a, &neg, &len);
        if (neg || len > 9) return 0;
        return isatty(atoi(d));
    }
    case 'h': case 'L': return tt_stat(a, &st, 0) == 0 && S_ISLNK(st.st_mode);
    }
    if (tt_stat(a, &st, 1) != 0) return 0;
    switch (op) {
    case 'e': return 1;
    case 'f': return S_ISREG(st.st_mode);
    case 'd': return S_ISDIR(st.st_mode);
    case 'b': return S_ISBLK(st.st_mode);
    case 'c': return S_ISCHR(st.st_mode);
    case 'p': return S_ISFIFO(st.st_mode);
    case 'S': return S_ISSOCK(st.st_mode);
    case 's': return st.st_size > 0;
    case 'u': return (st.st_mode & 04000) != 0;
    case 'g': return (st.st_mode & 02000) != 0;
    case 'k': return (st.st_mode & 01000) != 0;
    case 'O': return st.st_uid == geteuid();
    case 'G': return st.st_gid == getegid();
    case 'r': return access(a, R_OK) == 0;
    case 'w': return access(a, W_OK) == 0;
    case 'x': return access(a, X_OK) == 0;
    }
    return 0;
}

static int tt_counts(int n);
static int tt_expr(void);

static int tt_term(void) {
    if (tt_pos >= tt_argc) tt_beyond();
    int neg = 0;
    while (tt_pos < tt_argc && !strcmp(tt_argv[tt_pos], "!")) {
        tt_advance(1);
        neg = !neg;
    }
    int v;
    if (!strcmp(tt_argv[tt_pos], "(")) {
        tt_advance(1);
        int n = 1;
        for (; tt_pos + n < tt_argc && strcmp(tt_argv[tt_pos + n], ")"); n++)
            if (n == 4) { n = tt_argc - tt_pos; break; }
        v = tt_counts(n);
        if (tt_pos >= tt_argc) tt_error("%s expected", su_quote(")"));
        if (strcmp(tt_argv[tt_pos], ")")) {
            char q1[16];
            snprintf(q1, sizeof q1, "%s", su_quote(")"));
            tt_error("%s expected, found %s", q1, su_quote(tt_argv[tt_pos]));
        }
        tt_pos++;
    } else if (tt_argc - tt_pos > 2 && tt_is_binop(tt_argv[tt_pos + 1])) {
        v = tt_binary(0);
    } else if (tt_argc - tt_pos > 3 && !strcmp(tt_argv[tt_pos], "-l") &&
               tt_is_binop(tt_argv[tt_pos + 2])) {
        v = tt_binary(1);
    } else if (tt_is_unop(tt_argv[tt_pos])) {
        v = tt_unary();
    } else {
        v = tt_argv[tt_pos][0] != '\0';
        tt_pos++;
    }
    return neg ? !v : v;
}

static int tt_and(void) {
    int v = tt_term();
    while (tt_pos < tt_argc && !strcmp(tt_argv[tt_pos], "-a")) {
        tt_advance(1);
        int w = tt_term();
        v = v && w;
    }
    return v;
}

static int tt_expr(void) {
    if (tt_pos >= tt_argc) tt_beyond();
    int v = tt_and();
    while (tt_pos < tt_argc && !strcmp(tt_argv[tt_pos], "-o")) {
        tt_advance(1);
        int w = tt_and();
        v = v || w;
    }
    return v;
}

static int tt_one(void) {
    return tt_argv[tt_pos++][0] != '\0';
}

static int tt_two(void) {
    if (!strcmp(tt_argv[tt_pos], "!")) {
        tt_pos++;
        return !tt_one();
    }
    const char *a = tt_argv[tt_pos];
    if (a[0] == '-' && a[1] && !a[2]) {
        if (tt_is_unop(a)) return tt_unary();
        tt_error("%s: unary operator expected", su_quote(a));
    }
    tt_beyond();
}

static int tt_three(void) {
    if (tt_is_binop(tt_argv[tt_pos + 1])) return tt_binary(0);
    if (!strcmp(tt_argv[tt_pos], "!")) {
        tt_pos++;
        return !tt_two();
    }
    if (!strcmp(tt_argv[tt_pos], "(") && !strcmp(tt_argv[tt_pos + 2], ")")) {
        tt_pos++;
        int v = tt_one();
        tt_pos++;
        return v;
    }
    if (!strcmp(tt_argv[tt_pos + 1], "-a") || !strcmp(tt_argv[tt_pos + 1], "-o"))
        return tt_expr();
    tt_error("%s: binary operator expected", su_quote(tt_argv[tt_pos + 1]));
}

static int tt_counts(int n) {
    switch (n) {
    case 1: return tt_one();
    case 2: return tt_two();
    case 3: return tt_three();
    case 4:
        if (!strcmp(tt_argv[tt_pos], "!")) {
            tt_pos++;
            return !tt_three();
        }
        if (!strcmp(tt_argv[tt_pos], "(") && !strcmp(tt_argv[tt_pos + 3], ")")) {
            tt_pos++;
            int v = tt_two();
            tt_pos++;
            return v;
        }
        return tt_expr();
    default: return tt_expr();
    }
}

static int do_test(int argc, char **argv) {
    int bracket = !strcmp(su_short, "[");
    tt_name = bracket ? "[" : "test";
    if (bracket) {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            printf("Usage: test EXPRESSION\n"
                   "  or:  test\n"
                   "  or:  [ EXPRESSION ]\n"
                   "  or:  [ ]\n"
                   "  or:  [ OPTION\n"
                   "Exit with the status EXPRESSION gives: 0 true, 1 false, 2 an error.\n"
                   "\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n");
            return 0;
        }
        if (argc == 2 && !strcmp(argv[1], "--version")) return su_version();
        if (argc < 2 || strcmp(argv[argc - 1], "]")) tt_error("missing %s", su_quote("]"));
        argc--;
    }
    tt_argv = argv + 1;
    tt_argc = argc - 1;
    tt_pos = 0;
    if (tt_argc == 0) return 1;
    int v = tt_counts(tt_argc);
    if (tt_pos != tt_argc) tt_error("extra argument %s", su_quote(tt_argv[tt_pos]));
    return v ? 0 : 1;
}
