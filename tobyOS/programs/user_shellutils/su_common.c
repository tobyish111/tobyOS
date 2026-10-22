/* su_common.c -- what the commands in this program share: GNU's option
 * parsing and its messages, error reporting, and number parsing.
 *
 * Included by main.c (the build compiles one translation unit per
 * program). Every name here starts su_.
 *
 * THE OPTION PARSER IS GNU getopt_long's BEHAVIOUR, written here because
 * libtoby has only POSIX getopt: options and operands may be mixed (the
 * operands are moved after the options, so `seq 1 -s, 3` works); a
 * leading '+' in the short-option string, or POSIXLY_CORRECT, stops at the
 * first operand; "--" ends the options; a long option may be abbreviated
 * to any prefix that names one option (or several that all mean the same
 * thing); `--name=value` or `--name value` for a required argument, only
 * `--name=value` for an optional one; and the error messages are glibc's,
 * word for word, naming argv[0] as typed. */

static const char *su_name = "shellutils";   /* argv[0] as typed */
static const char *su_short = "shellutils";  /* its last component */

static void su_set_name(const char *argv0) {
    su_name = (argv0 && *argv0) ? argv0 : "shellutils";
    const char *s = strrchr(su_name, '/');
    su_short = s ? s + 1 : su_name;
}

/* GNU error(): "PROG: MESSAGE[: strerror(ERRNUM)]", then exit(STATUS)
 * unless STATUS is 0. stdout is flushed first so the two streams come out
 * in the order they were written. */
static void su_error(int status, int errnum, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void su_error(int status, int errnum, const char *fmt, ...) {
    fflush(stdout);
    fprintf(stderr, "%s: ", su_short);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (errnum) fprintf(stderr, ": %s", strerror(errnum));
    fputc('\n', stderr);
    if (status) exit(status);
}

/* "Try 'PROG --help' for more information.", and exit(STATUS). */
static void su_try_help(int status) __attribute__((noreturn));
static void su_try_help(int status) {
    fflush(stdout);
    fprintf(stderr, "Try '%s --help' for more information.\n", su_name);
    exit(status);
}

/* open(PATH, O_RDONLY) as Linux answers it: a directory opens, and read
 * says EISDIR -- GNU's messages ("read error: Is a directory") and its
 * sync of a directory depend on it. The native open refuses a directory
 * unless asked with O_DIRECTORY, so a refusal is asked again that way. */
static int su_open_read(const char *path, int extra) {
    int fd = open(path, O_RDONLY | extra);
#ifndef SU_HOST
    if (fd < 0 && errno == EISDIR) fd = open(path, O_RDONLY | O_DIRECTORY | extra);
#endif
    return fd;
}

/* ---- quoting for messages ------------------------------------------------
 *
 * GNU quotes a name in a message one of two ways, both measured in the C
 * locale: su_quote is quote(), su_quotef quotef() (each described below).
 * The result lives in one of a few rotating buffers, so two can appear in
 * one message. */
static char su_qbuf[4][4096];
static int su_qslot;

/* a byte the C locale cannot print: a control byte, DEL, or past 0x7f */
static int su_unprintable(unsigned char c) { return c < 0x20 || c >= 0x7f; }

/* C's escape for an unprintable byte, into B at *N */
static void su_cesc(char *b, size_t *n, unsigned char c) {
    static const char named[] = "\aa\bb\ff\nn\rr\tt\vv";
    const char *p = c ? strchr(named, (char)c) : 0;
    b[(*n)++] = '\\';
    if (p && (p - named) % 2 == 0) {
        b[(*n)++] = p[1];
    } else {
        b[(*n)++] = (char)('0' + (c >> 6 & 7));
        b[(*n)++] = (char)('0' + (c >> 3 & 7));
        b[(*n)++] = (char)('0' + (c & 7));
    }
}

/* quote(): 'ARG', a quote or a backslash after a backslash, an
 * unprintable byte as C writes it (\t, \001, \303) */
static const char *su_quote(const char *s) {
    char *b = su_qbuf[su_qslot++ & 3];
    size_t cap = sizeof su_qbuf[0] - 16, n = 0;
    b[n++] = '\'';
    for (; *s && n < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\'' || c == '\\') { b[n++] = '\\'; b[n++] = (char)c; }
        else if (su_unprintable(c)) su_cesc(b, &n, c);
        else b[n++] = (char)c;
    }
    b[n++] = '\'';
    b[n] = 0;
    return b;
}

/* quotef(): shell-escape style. Bare when every byte is safe in a shell
 * word (letters, digits, % + , - . / @ ] _ { }, and # or ~ past the
 * start); in double quotes when it holds a single quote and nothing else
 * double quotes would not cover (letters, digits, % + , - . / : @ ] _ and
 * blanks; # ~ at the start); else in single quotes, a single quote as
 * '\'' and a run of unprintable bytes as $'\n...'. Measured against GNU
 * 9.7 on every printable punctuation character, quotes, control bytes and
 * UTF-8 (finding 209). */
static const char *su_quotef(const char *s) {
    char *b = su_qbuf[su_qslot++ & 3];
    size_t cap = sizeof su_qbuf[0] - 16, n = 0;
    int bare = *s != 0, dq = 1, squote = 0, ctrl = 0;
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        int safe = isalnum(c) || strchr("%+,-./@]_{}", c) || ((c == '#' || c == '~') && i);
        if (!safe) bare = 0;
        if (c == '\'') squote = 1;
        else if (su_unprintable(c)) ctrl = 1;
        else if (!(isalnum(c) || strchr("%+,-./:@]_ ", c) || ((c == '#' || c == '~') && !i)))
            dq = 0;
    }
    if (bare) {
        snprintf(b, cap, "%s", s);
        return b;
    }
    if (squote && dq && !ctrl) {
        snprintf(b, cap, "\"%s\"", s);
        return b;
    }
    int q = 1;      /* inside single quotes */
    b[n++] = '\'';
    for (size_t i = 0; s[i] && n < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (su_unprintable(c)) {
            if (q) b[n++] = '\'';
            b[n++] = '$';
            b[n++] = '\'';
            for (; s[i] && su_unprintable((unsigned char)s[i]) && n < cap; i++)
                su_cesc(b, &n, (unsigned char)s[i]);
            i--;
            b[n++] = '\'';
            q = 0;
        } else if (c == '\'') {
            if (q) b[n++] = '\'';
            b[n++] = '\\';
            b[n++] = '\'';
            b[n++] = '\'';
            q = 1;
        } else {
            if (!q) { b[n++] = '\''; q = 1; }
            b[n++] = (char)c;
        }
    }
    if (q) b[n++] = '\'';
    b[n] = 0;
    return b;
}

/* quoteaf(): shell-escape style, quoted even when nothing needs it --
 * 'NAME', or "NAME" when it holds a single quote. GNU's choice for most
 * file names in messages ("failed to remove 'a'"). */
static const char *su_quoteaf(const char *s) {
    const char *q = su_quotef(s);
    if (*q == '\'' || *q == '"') return q;
    char *b = su_qbuf[su_qslot++ & 3];
    snprintf(b, sizeof su_qbuf[0], "'%s'", s);
    return b;
}

/* ---- modes ----------------------------------------------------------------
 *
 * chmod's MODE, as GNU's mode_compile and mode_adjust read it: octal (at
 * most 07777), or comma-separated clauses [ugoa]*([-+=]([rwxXst]*|[ugo]))+.
 * A clause with no [ugoa] is masked by the umask; `=` clears what the
 * clause names; [ugo] after the operator copies that class's bits. */
#define SU_MODE_MAX 64
struct su_mode_change {
    char op;                /* '=', '+' or '-' */
    char copy;              /* 'u', 'g', 'o', or 0 */
    char xifany;            /* X */
    mode_t affected;        /* the [ugoa] bits; 0 when none were given */
    mode_t value;           /* the permission bits named */
};
struct su_mode {
    int n;
    int octal;
    struct su_mode_change c[SU_MODE_MAX];
};

/* 0, or -1 when MODE is not one */
static int su_mode_compile(const char *s, struct su_mode *m) {
    m->n = 0;
    m->octal = 0;
    if (*s >= '0' && *s <= '7') {
        unsigned long v = 0;
        const char *p = s;
        for (; *p >= '0' && *p <= '7'; p++) {
            v = v * 8 + (unsigned long)(*p - '0');
            if (v > 07777) return -1;
        }
        if (*p) return -1;
        m->octal = 1;
        m->n = 1;
        m->c[0].op = '=';
        m->c[0].copy = 0;
        m->c[0].xifany = 0;
        m->c[0].affected = 07777;
        m->c[0].value = (mode_t)v;
        return 0;
    }
    const char *p = s;
    for (;;) {
        mode_t who = 0;
        for (;; p++) {
            if (*p == 'u') who |= 04700;
            else if (*p == 'g') who |= 02070;
            else if (*p == 'o') who |= 01007;
            else if (*p == 'a') who |= 07777;
            else break;
        }
        if (*p != '=' && *p != '+' && *p != '-') return -1;
        while (*p == '=' || *p == '+' || *p == '-') {
            if (m->n >= SU_MODE_MAX) return -1;
            struct su_mode_change *c = &m->c[m->n++];
            c->op = *p++;
            c->copy = 0;
            c->xifany = 0;
            c->affected = who;
            c->value = 0;
            if (*p == 'u' || *p == 'g' || *p == 'o') {
                c->copy = *p++;
            } else {
                for (;; p++) {
                    if (*p == 'r') c->value |= 0444;
                    else if (*p == 'w') c->value |= 0222;
                    else if (*p == 'x') c->value |= 0111;
                    else if (*p == 'X') c->xifany = 1;
                    else if (*p == 's') c->value |= 06000;
                    else if (*p == 't') c->value |= 01000;
                    else break;
                }
            }
        }
        if (*p == ',') { p++; continue; }
        if (*p) return -1;
        return 0;
    }
}

/* OLD changed by M: what chmod MODE makes of a file whose mode is OLD. */
static mode_t su_mode_adjust(mode_t old, int is_dir, mode_t umask_v,
                             const struct su_mode *m) {
    mode_t nm = old & 07777;
    for (int i = 0; i < m->n; i++) {
        const struct su_mode_change *c = &m->c[i];
        mode_t value = c->value;
        if (c->copy) {
            mode_t src = c->copy == 'u' ? (nm >> 6) & 7
                       : c->copy == 'g' ? (nm >> 3) & 7 : nm & 7;
            value = (mode_t)(src << 6 | src << 3 | src);
        }
        if (c->xifany && (is_dir || (nm & 0111))) value |= 0111;
        if (m->octal) {
            nm = value;
            continue;
        }
        /* the umask masks what a clause with no [ugoa] SETS; `=` still
         * clears everything it names (`=r` under umask 022 is 0444) */
        if (c->affected) value &= c->affected;
        else value &= ~umask_v;
        switch (c->op) {
        case '=': {
            mode_t preserved = c->affected ? ~c->affected : 0;
            nm = (nm & preserved) | value;
            break;
        }
        case '+': nm |= value; break;
        case '-': nm &= ~value; break;
        }
    }
    return nm & 07777;
}

/* ---- getopt_long ---------------------------------------------------------- */

#define SU_NO_ARG 0
#define SU_REQ_ARG 1
#define SU_OPT_ARG 2

struct su_option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

static int su_optind = 1;      /* the next argv element to look at */
static char *su_optarg;        /* the current option's argument */
static int su_optopt;          /* the option character in error */
static int su_opterr = 1;      /* print messages */

/* scanner state */
static int su_nextind = 1;       /* where scanning resumes */
static const char *su_cluster; /* rest of a short-option cluster */
static int *su_nonopt;         /* positions of operands set aside */
static int su_nnonopt, su_nonopt_cap;
static int *su_optelem;        /* positions of option elements, in order */
static int su_noptelem, su_optelem_cap;

static void su_push(int **v, int *n, int *cap, int x) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *v = (int *)realloc(*v, (size_t)*cap * sizeof **v);
        if (!*v) { fputs("out of memory\n", stderr); exit(1); }
    }
    (*v)[(*n)++] = x;
}

/* Put the option elements first and the operands after them, both in
 * their original order, and point su_optind at the first operand. */
static void su_permute_done(int argc, char **argv) {
    int n = 1;
    char **tmp = (char **)malloc((size_t)(argc + 1) * sizeof *tmp);
    if (!tmp) return;
    for (int i = 0; i < su_noptelem; i++) tmp[n++] = argv[su_optelem[i]];
    int first_operand = n;
    for (int i = 0; i < su_nnonopt; i++) tmp[n++] = argv[su_nonopt[i]];
    for (int i = su_nextind; i < argc; i++) tmp[n++] = argv[i];
    for (int i = 1; i < n && i < argc; i++) argv[i] = tmp[i];
    free(tmp);
    su_optind = first_operand;
}

static int su_getopt_long(int argc, char **argv, const char *shorts,
                          const struct su_option *longs, int *longind) {
    int posix = 0, inorder = 0, colon = 0;
    if (*shorts == '+') { posix = 1; shorts++; }
    else if (*shorts == '-') { inorder = 1; shorts++; }
    if (getenv("POSIXLY_CORRECT")) posix = 1;
    if (*shorts == ':') { colon = 1; shorts++; }
    su_optarg = 0;

    if (su_cluster && *su_cluster) goto short_opt;
    su_cluster = 0;

    for (;;) {
        if (su_nextind >= argc) {
            if (!posix && !inorder) su_permute_done(argc, argv);
            else su_optind = su_nextind;
            return -1;
        }
        char *a = argv[su_nextind];
        if (strcmp(a, "--") == 0) {
            su_push(&su_optelem, &su_noptelem, &su_optelem_cap, su_nextind);
            su_nextind++;
            if (!posix && !inorder) su_permute_done(argc, argv);
            else su_optind = su_nextind;
            return -1;
        }
        if (a[0] != '-' || a[1] == 0) {
            if (inorder) {
                su_optarg = a;
                su_optind = ++su_nextind;
                return 1;
            }
            if (posix) { su_optind = su_nextind; return -1; }
            su_push(&su_nonopt, &su_nnonopt, &su_nonopt_cap, su_nextind);
            su_nextind++;
            continue;
        }
        su_push(&su_optelem, &su_noptelem, &su_optelem_cap, su_nextind);
        su_nextind++;
        su_optind = su_nextind;
        if (a[1] == '-' && longs) {
            /* a long option */
            const char *name = a + 2;
            size_t len = strcspn(name, "=");
            int found = -1, exact = 0, ambig = 0;
            for (int i = 0; longs[i].name; i++) {
                if (strncmp(longs[i].name, name, len) != 0) continue;
                if (strlen(longs[i].name) == len) { found = i; exact = 1; break; }
                if (found < 0) found = i;
                else if (longs[i].has_arg != longs[found].has_arg ||
                         longs[i].flag != longs[found].flag ||
                         longs[i].val != longs[found].val)
                    ambig = 1;
            }
            if (ambig && !exact) {
                if (su_opterr) {
                    /* the option as written, =VALUE and all, as GNU's
                     * getopt names it (`--relative=d` is ambiguous) */
                    fprintf(stderr, "%s: option '--%s' is ambiguous; possibilities:",
                            argv[0], name);
                    for (int i = 0; longs[i].name; i++)
                        if (strncmp(longs[i].name, name, len) == 0)
                            fprintf(stderr, " '--%s'", longs[i].name);
                    fputc('\n', stderr);
                }
                su_optopt = 0;
                return '?';
            }
            if (found < 0) {
                if (su_opterr)
                    fprintf(stderr, "%s: unrecognized option '--%s'\n", argv[0], name);
                su_optopt = 0;
                return '?';
            }
            const struct su_option *o = &longs[found];
            if (longind) *longind = found;
            if (name[len] == '=') {
                if (o->has_arg == SU_NO_ARG) {
                    if (su_opterr)
                        fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n",
                                argv[0], o->name);
                    su_optopt = o->val;
                    return '?';
                }
                su_optarg = (char *)name + len + 1;
            } else if (o->has_arg == SU_REQ_ARG) {
                if (su_nextind >= argc) {
                    if (su_opterr)
                        fprintf(stderr, "%s: option '--%s' requires an argument\n",
                                argv[0], o->name);
                    su_optopt = o->val;
                    return colon ? ':' : '?';
                }
                su_push(&su_optelem, &su_noptelem, &su_optelem_cap, su_nextind);
                su_optarg = argv[su_nextind++];
                su_optind = su_nextind;
            }
            if (o->flag) { *o->flag = o->val; return 0; }
            return o->val;
        }
        su_cluster = a + 1;
        break;
    }

short_opt:;
    char c = *su_cluster++;
    const char *spec = c == ':' ? 0 : strchr(shorts, c);
    if (!spec) {
        if (su_opterr)
            fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
        su_optopt = (unsigned char)c;
        if (!*su_cluster) su_cluster = 0;
        return '?';
    }
    if (spec[1] == ':') {
        if (*su_cluster) {                  /* attached: -s, or -sVALUE */
            su_optarg = (char *)su_cluster;
            su_cluster = 0;
        } else if (spec[2] == ':') {        /* optional, not attached: none */
            su_cluster = 0;
        } else if (su_nextind < argc) {
            su_push(&su_optelem, &su_noptelem, &su_optelem_cap, su_nextind);
            su_optarg = argv[su_nextind++];
            su_optind = su_nextind;
            su_cluster = 0;
        } else {
            if (su_opterr)
                fprintf(stderr, "%s: option requires an argument -- '%c'\n",
                        argv[0], c);
            su_optopt = (unsigned char)c;
            su_cluster = 0;
            return colon ? ':' : '?';
        }
    } else if (!*su_cluster) {
        su_cluster = 0;
    }
    return (unsigned char)c;
}

/* GNU's --help/--version long options, as most commands list them. */
#define SU_HELP_OPT    0x10000
#define SU_VERSION_OPT 0x10001
#define SU_LONG_HV { "help", SU_NO_ARG, 0, SU_HELP_OPT }, \
                   { "version", SU_NO_ARG, 0, SU_VERSION_OPT }

/* NAME (tobyOS) 1.0 -- the version line every command here prints. */
static int su_version(void) {
    printf("%s (tobyOS) 1.0\n", su_short);
    return 0;
}

/* ---- numbers ---------------------------------------------------------------
 *
 * su_strtoumax: an unsigned decimal (leading blanks and a '+' allowed, as
 * strtoumax) with an optional multiplier suffix from VALID -- GNU's
 * xstrtoumax: b 512, c 1, w 2, and k K M G T P E Z Y R Q, each 1024-based,
 * or 1000-based when followed by B ("KB"), 1024-based again with "iB"
 * ("KiB"); 'k' alone is 1024 too. Returns 0, or SU_NUM_INVALID (not a
 * number, or a suffix not in VALID), or SU_NUM_OVERFLOW (the value is set
 * to UINTMAX_MAX).
 */
#define SU_NUM_OK 0
#define SU_NUM_INVALID 1
#define SU_NUM_OVERFLOW 2
#define SU_NUM_BADSUFFIX 3

static int su_mul(uintmax_t *v, uintmax_t m) {
    if (*v && m > UINTMAX_MAX / *v) { *v = UINTMAX_MAX; return SU_NUM_OVERFLOW; }
    *v *= m;
    return SU_NUM_OK;
}

static int su_strtoumax(const char *s, char **endp, uintmax_t *out, const char *valid) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') { if (endp) *endp = (char *)s; return SU_NUM_INVALID; }
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) {
        /* GNU: a suffix alone means one of it ("k" = 1024) when allowed */
        if (*p && valid && strchr(valid, *p)) {
            *out = 1;
            goto suffix;
        }
        if (endp) *endp = (char *)s;
        return SU_NUM_INVALID;
    }
    {
        uintmax_t v = 0;
        int over = 0;
        for (; isdigit((unsigned char)*p); p++) {
            unsigned d = (unsigned)(*p - '0');
            if (v > (UINTMAX_MAX - d) / 10) over = 1;
            else v = v * 10 + d;
        }
        *out = over ? UINTMAX_MAX : v;
        if (over) { if (endp) *endp = (char *)p; return SU_NUM_OVERFLOW; }
    }
suffix:
    if (!*p || !valid) { if (endp) *endp = (char *)p; return SU_NUM_OK; }
    if (!strchr(valid, *p)) { if (endp) *endp = (char *)p; return SU_NUM_BADSUFFIX; }
    {
        int base = 1024, extra = 1;
        if (strchr(valid, '0')) {          /* the B/iB forms are allowed */
            if (p[1] == 'i' && p[2] == 'B') extra = 3;
            else if (p[1] == 'B' || p[1] == 'D') { base = 1000; extra = 2; }
        }
        int rc = SU_NUM_OK, power = 0;
        switch (*p) {
        case 'b': rc = su_mul(out, 512); break;
        case 'c': break;
        case 'w': rc = su_mul(out, 2); break;
        case 'k': case 'K': power = 1; break;
        case 'M': power = 2; break;
        case 'G': power = 3; break;
        case 'T': power = 4; break;
        case 'P': power = 5; break;
        case 'E': power = 6; break;
        case 'Z': power = 7; break;
        case 'Y': power = 8; break;
        case 'R': power = 9; break;
        case 'Q': power = 10; break;
        default: if (endp) *endp = (char *)p; return SU_NUM_BADSUFFIX;
        }
        for (int i = 0; i < power && rc == SU_NUM_OK; i++) rc = su_mul(out, (uintmax_t)base);
        p += (*p == 'b' || *p == 'c' || *p == 'w') ? 1 : extra;
        if (endp) *endp = (char *)p;
        if (rc != SU_NUM_OK) return rc;
        return *p ? SU_NUM_BADSUFFIX : SU_NUM_OK;
    }
}
