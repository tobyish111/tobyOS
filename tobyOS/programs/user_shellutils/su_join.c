/* su_join.c -- join, as GNU coreutils 9.7's (logs/utilgate/staging/
 * join.sh): a merge join of two files sorted on their join fields, in the
 * C locale. Everything below was measured against gnujoin in WSL.
 *
 * FIELDS. Without -t a field is a run of non-blanks (space, tab, and the
 * newline, which only -z lets into a line): leading and trailing blanks
 * make no field, so a line of blanks has none. With -t C every C splits,
 * so "a,,b" is three fields and "," two; an empty line has none. -t ''
 * makes the whole line one field (none when empty) and -t '\0' (the two
 * characters) splits at NUL bytes; the two are different tabs to the
 * "incompatible tabs" check. The output separator is C, or a space.
 * Keys compare as bytes; -i compares them upper-cased ('_' sorts after
 * 'A' then). A missing join field is an empty key, and empty keys pair.
 *
 * OUTPUT. A pair is the join field (FILE1's), then FILE1's other fields,
 * then FILE2's. An unpaired line (-a, -v) is its join field and then its
 * other fields. -o lists FILENUM.FIELD or 0 (the join field: FILE1's, or
 * FILE2's on a FILE2-only line); several -o add up, `-o auto` takes the
 * shape from each file's first line and loses to any explicit list. -e
 * replaces EVERY empty or missing output field, in the default format
 * too ("a,,x" with -e @ prints "a,@,x").
 *
 * ORDER CHECKING is where GNU is particular. Each line read is compared
 * with the line read before it from the same file. --check-order makes
 * the first disorder fatal ("FILE:N: is not sorted: LINE", the line up
 * to a NUL). By default a file's first disorder is a warning and the run
 * ends "input is not in sorted order", status 1 -- but only once a line
 * has been found unpairable in the merge, and the flag is raised AFTER
 * the read that follows that line, so a disorder on that very read goes
 * unreported. The lines left in one file when the other ends are read
 * (and printed with -a) without raising the flag. --nocheck-order turns
 * it all off.
 *
 * OPERANDS. The options are read in order (they may follow the files).
 * A third operand is not at once an error: if the first of the two kept
 * came after -o (or after an operand that did), it is one more -o list
 * (`-o 1.1 2.2 f1 f2`); if it came right after an attached -j1 or -j2,
 * it is the old `-j1 FIELD` (-1 FIELD); else "extra operand" names the
 * newest. An -j1/-j2 not consumed that way acts as -j 1 / -j 2 after all
 * the options (so `-1 3 -j1` is "incompatible join fields 2, 0"). Field
 * numbers are zero-based in that message, old one first. */

struct fm_jn_field { const char *s; size_t n; };

struct fm_jn_line {
    char *buf;
    size_t len, cap;
    struct fm_jn_field *f;
    size_t nf, fcap;
};

struct fm_jn_seq {
    struct fm_jn_line *l;
    size_t count, alloc;
};

struct fm_jn_file {
    const char *name;
    int fd;
    unsigned char *rb;
    size_t rpos, rlen;
    int eof;
    uintmax_t lineno;
    int reported;
    size_t jf;              /* its join field, zero-based */
};

struct fm_jn_spec { int file; size_t field; };   /* file 0: the join field */

#define FM_JN_UNSET ((size_t)-1)
#define FM_JN_TAB_BLANKS (-1)
#define FM_JN_TAB_WHOLE  (-2)
#define FM_JN_TAB_UNSET  (-3)

enum { FM_JN_CHECK_DEFAULT, FM_JN_CHECK_ON, FM_JN_CHECK_OFF };
enum { FM_JN_TAG_NONE, FM_JN_TAG_O, FM_JN_TAG_J1, FM_JN_TAG_J2 };

static struct {
    int tab;                 /* a byte, or FM_JN_TAB_* */
    int eol;
    int ignore_case;
    int check;
    int header;
    int print_pairs;
    int print_unp[3];
    const char *empty;       /* -e, or 0 */
    int autoformat;
    struct fm_jn_spec *spec;
    size_t nspec, specap;
    size_t j1, j2;
    int seen_unpairable;
    int disorder;
} fm_jn;

static void fm_jn_oflush(void);
static void fm_jn_nomem(void) { su_error(1, 0, "memory exhausted"); }

/* ---- reading -------------------------------------------------------------- */

static int fm_jn_fill(struct fm_jn_file *f) {
    if (f->eof) return 0;
    if (!f->rb) {
        f->rb = (unsigned char *)malloc(65536);
        if (!f->rb) fm_jn_nomem();
    }
    for (;;) {
        ssize_t r = read(f->fd, f->rb, 65536);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { int e = errno; fm_jn_oflush(); su_error(1, e, "read error"); }
        if (r == 0) { f->eof = 1; return 0; }
        f->rpos = 0;
        f->rlen = (size_t)r;
        return 1;
    }
}

static void fm_jn_addc(struct fm_jn_line *l, const unsigned char *p, size_t n) {
    if (l->len + n + 1 > l->cap) {
        size_t c = l->cap ? l->cap : 128;
        while (c < l->len + n + 1) c *= 2;
        char *nb = (char *)realloc(l->buf, c);
        if (!nb) fm_jn_nomem();
        l->buf = nb;
        l->cap = c;
    }
    memcpy(l->buf + l->len, p, n);
    l->len += n;
}

static void fm_jn_addfield(struct fm_jn_line *l, const char *s, size_t n) {
    if (l->nf == l->fcap) {
        l->fcap = l->fcap ? l->fcap * 2 : 8;
        l->f = (struct fm_jn_field *)realloc(l->f, l->fcap * sizeof *l->f);
        if (!l->f) fm_jn_nomem();
    }
    l->f[l->nf].s = s;
    l->f[l->nf].n = n;
    l->nf++;
}

static int fm_jn_isblank(unsigned char c) { return c == ' ' || c == '\t' || c == '\n'; }

static void fm_jn_split(struct fm_jn_line *l) {
    l->nf = 0;
    const char *p = l->buf, *e = l->buf + l->len;
    if (fm_jn.tab == FM_JN_TAB_BLANKS) {
        while (p < e) {
            while (p < e && fm_jn_isblank((unsigned char)*p)) p++;
            if (p == e) break;
            const char *q = p;
            while (q < e && !fm_jn_isblank((unsigned char)*q)) q++;
            fm_jn_addfield(l, p, (size_t)(q - p));
            p = q;
        }
    } else if (fm_jn.tab == FM_JN_TAB_WHOLE) {
        if (l->len) fm_jn_addfield(l, p, l->len);
    } else if (l->len) {
        for (;;) {
            const char *q = (const char *)memchr(p, fm_jn.tab, (size_t)(e - p));
            if (!q) { fm_jn_addfield(l, p, (size_t)(e - p)); break; }
            fm_jn_addfield(l, p, (size_t)(q - p));
            p = q + 1;
        }
    }
}

/* the next line of F into L: 1, or 0 at the end */
static int fm_jn_readline(struct fm_jn_file *f, struct fm_jn_line *l) {
    l->len = 0;
    int any = 0;
    for (;;) {
        if (f->rpos >= f->rlen && !fm_jn_fill(f)) break;
        unsigned char *s = f->rb + f->rpos;
        size_t avail = f->rlen - f->rpos;
        unsigned char *nl = (unsigned char *)memchr(s, fm_jn.eol, avail);
        size_t take = nl ? (size_t)(nl - s) : avail;
        fm_jn_addc(l, s, take);
        any = 1;
        f->rpos += take;
        if (nl) { f->rpos++; break; }
    }
    if (!any) return 0;
    if (!l->buf) fm_jn_addc(l, (const unsigned char *)"", 0);
    l->buf[l->len] = 0;
    f->lineno++;
    fm_jn_split(l);
    return 1;
}

/* ---- comparing ------------------------------------------------------------ */

static void fm_jn_key(const struct fm_jn_line *l, size_t jf, const char **s, size_t *n) {
    if (l && jf < l->nf) { *s = l->f[jf].s; *n = l->f[jf].n; }
    else { *s = ""; *n = 0; }
}

static int fm_jn_cmp(const struct fm_jn_line *a, size_t ja, const struct fm_jn_line *b, size_t jb) {
    const char *s1, *s2;
    size_t n1, n2;
    fm_jn_key(a, ja, &s1, &n1);
    fm_jn_key(b, jb, &s2, &n2);
    size_t m = n1 < n2 ? n1 : n2;
    int d = 0;
    if (fm_jn.ignore_case) {
        for (size_t i = 0; i < m && !d; i++)
            d = toupper((unsigned char)s1[i]) - toupper((unsigned char)s2[i]);
    } else if (m) {
        d = memcmp(s1, s2, m);
    }
    if (d) return d < 0 ? -1 : 1;
    return n1 < n2 ? -1 : n1 > n2;
}

/* CUR has just been read from F after PREV: complain if it sorts first */
static void fm_jn_check(struct fm_jn_file *f, const struct fm_jn_line *prev,
                        const struct fm_jn_line *cur) {
    if (fm_jn.check == FM_JN_CHECK_OFF) return;
    if (fm_jn.check == FM_JN_CHECK_DEFAULT && !fm_jn.seen_unpairable) return;
    if (f->reported) return;
    if (fm_jn_cmp(prev, f->jf, cur, f->jf) <= 0) return;
    /* the line as a C string: GNU's message stops at a NUL */
    const char *z = (const char *)memchr(cur->buf, 0, cur->len);
    size_t n = z ? (size_t)(z - cur->buf) : cur->len;
    fm_jn_oflush();
    fprintf(stderr, "%s: %s:%ju: is not sorted: ", su_short, f->name, f->lineno);
    fwrite(cur->buf, 1, n, stderr);
    fputc('\n', stderr);
    if (fm_jn.check == FM_JN_CHECK_ON) exit(1);
    f->reported = 1;
    fm_jn.disorder = 1;
}

static void fm_jn_slot(struct fm_jn_seq *q, size_t i) {
    if (i < q->alloc) return;
    size_t na = q->alloc ? q->alloc * 2 : 4;
    while (na <= i) na *= 2;
    q->l = (struct fm_jn_line *)realloc(q->l, na * sizeof *q->l);
    if (!q->l) fm_jn_nomem();
    memset(q->l + q->alloc, 0, (na - q->alloc) * sizeof *q->l);
    q->alloc = na;
}

static void fm_jn_swap(struct fm_jn_seq *q, size_t a, size_t b) {
    struct fm_jn_line t = q->l[a];
    q->l[a] = q->l[b];
    q->l[b] = t;
}

/* one more line of F after Q's last: 1, or 0 at the end */
static int fm_jn_extend(struct fm_jn_file *f, struct fm_jn_seq *q) {
    fm_jn_slot(q, q->count);
    if (!fm_jn_readline(f, &q->l[q->count])) return 0;
    if (q->count) fm_jn_check(f, &q->l[q->count - 1], &q->l[q->count]);
    q->count++;
    return 1;
}

/* Q's one line replaced by F's next: 1, or 0 (and Q empty) at the end */
static int fm_jn_advance(struct fm_jn_file *f, struct fm_jn_seq *q) {
    fm_jn_slot(q, 1);
    if (!fm_jn_readline(f, &q->l[1])) { q->count = 0; return 0; }
    if (q->count) fm_jn_check(f, &q->l[0], &q->l[1]);
    fm_jn_swap(q, 0, 1);
    q->count = 1;
    return 1;
}

/* ---- writing --------------------------------------------------------------
 *
 * Output is buffered here, not by stdio, so that a failing standard output
 * reads the same on every libc: as glibc's, a 4096-byte buffer is written
 * when full and before every message to standard error, and a failed write
 * drops what it held. GNU checks after every output line, and once more
 * at exit: "write error" with the failed write's reason -- or with none
 * when the only failure was the flush before the very last message
 * ("input is not in sorted order"), as `join u1 o2 > /dev/full` says. */
static char fm_jn_ob[4096];
static size_t fm_jn_on;
static int fm_jn_ofail;

static void fm_jn_oflush(void) {
    size_t off = 0;
    while (off < fm_jn_on) {
        ssize_t w = write(1, fm_jn_ob + off, fm_jn_on - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { fm_jn_ofail = errno ? errno : EIO; break; }
        off += (size_t)w;
    }
    fm_jn_on = 0;
}

static void fm_jn_ow(const char *p, size_t n) {
    while (n) {
        if (fm_jn_on == sizeof fm_jn_ob) fm_jn_oflush();
        size_t k = sizeof fm_jn_ob - fm_jn_on;
        if (k > n) k = n;
        memcpy(fm_jn_ob + fm_jn_on, p, k);
        fm_jn_on += k;
        p += k;
        n -= k;
    }
}

static void fm_jn_oc(int c) {
    char ch = (char)c;
    fm_jn_ow(&ch, 1);
}

/* at the end: the last write, and GNU's verdict on them all */
static void fm_jn_oclose(void) {
    int prev = fm_jn_ofail;
    size_t pending = fm_jn_on;
    fm_jn_ofail = 0;
    fm_jn_oflush();
    if (pending && fm_jn_ofail) su_error(1, fm_jn_ofail, "write error");
    if (prev) su_error(1, 0, "write error");
}

static void fm_jn_sep(void) {
    fm_jn_oc(fm_jn.tab >= 0 ? fm_jn.tab : ' ');
}

static void fm_jn_putfield(const struct fm_jn_line *l, size_t i) {
    if (l && i < l->nf && l->f[i].n) fm_jn_ow(l->f[i].s, l->f[i].n);
    else if (fm_jn.empty) fm_jn_ow(fm_jn.empty, strlen(fm_jn.empty));
}

/* a pair, or (one side null) an unpaired line */
static void fm_jn_out(const struct fm_jn_line *a, const struct fm_jn_line *b) {
    if (fm_jn.nspec) {
        for (size_t i = 0; i < fm_jn.nspec; i++) {
            const struct fm_jn_spec *s = &fm_jn.spec[i];
            if (i) fm_jn_sep();
            if (s->file == 0) {
                if (a) fm_jn_putfield(a, fm_jn.j1);
                else fm_jn_putfield(b, fm_jn.j2);
            } else {
                fm_jn_putfield(s->file == 1 ? a : b, s->field);
            }
        }
    } else {
        if (a) fm_jn_putfield(a, fm_jn.j1);
        else fm_jn_putfield(b, fm_jn.j2);
        if (a)
            for (size_t k = 0; k < a->nf; k++)
                if (k != fm_jn.j1) { fm_jn_sep(); fm_jn_putfield(a, k); }
        if (b)
            for (size_t k = 0; k < b->nf; k++)
                if (k != fm_jn.j2) { fm_jn_sep(); fm_jn_putfield(b, k); }
    }
    fm_jn_oc(fm_jn.eol);
    /* GNU looks at standard output after every line: a write that failed
     * earlier -- the flush before a message, or a full buffer -- ends the
     * run here, with that write's reason */
    if (fm_jn_ofail) su_error(1, fm_jn_ofail, "write error");
}

/* ---- options -------------------------------------------------------------- */

static void fm_jn_addspec(int file, size_t field) {
    if (fm_jn.nspec == fm_jn.specap) {
        fm_jn.specap = fm_jn.specap ? fm_jn.specap * 2 : 8;
        fm_jn.spec = (struct fm_jn_spec *)realloc(fm_jn.spec, fm_jn.specap * sizeof *fm_jn.spec);
        if (!fm_jn.spec) fm_jn_nomem();
    }
    fm_jn.spec[fm_jn.nspec].file = file;
    fm_jn.spec[fm_jn.nspec].field = field;
    fm_jn.nspec++;
}

/* a decimal as strtoumax reads one (blanks and '+' first): -1 when not */
static int fm_jn_num(const char *s, uintmax_t *v) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    uintmax_t x = 0;
    int over = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (x > (UINTMAX_MAX - d) / 10) over = 1;
        else x = x * 10 + d;
    }
    if (*p) return -1;
    *v = over ? UINTMAX_MAX : x;
    return over;
}

/* a field number, 1 or more (huge ones clamped): zero-based */
static size_t fm_jn_field(const char *s) {
    uintmax_t v;
    int r = fm_jn_num(s, &v);
    if (r < 0 || v == 0) su_error(1, 0, "invalid field number: %s", su_quote(s));
    if (r > 0 || v > SIZE_MAX) return SIZE_MAX - 1;
    return (size_t)(v - 1);
}

static void fm_jn_setfield(size_t *var, size_t val) {
    if (*var != FM_JN_UNSET && *var != val)
        su_error(1, 0, "incompatible join fields %zu, %zu", *var, val);
    *var = val;
}

static int fm_jn_filenum(const char *s) {
    uintmax_t v;
    int r = fm_jn_num(s, &v);
    if (r != 0 || (v != 1 && v != 2)) su_error(1, 0, "invalid file number: %s", su_quote(s));
    return (int)v;
}

/* one FILENUM.FIELD or 0, LEN bytes at S */
static void fm_jn_onespec(const char *s, size_t len) {
    char *t = (char *)malloc(len + 1);
    if (!t) fm_jn_nomem();
    memcpy(t, s, len);
    t[len] = 0;
    if (t[0] == '0') {
        if (t[1]) su_error(1, 0, "invalid field specifier: %s", su_quote(t));
        fm_jn_addspec(0, 0);
    } else if (t[0] == '1' || t[0] == '2') {
        if (t[1] != '.') su_error(1, 0, "invalid field specifier: %s", su_quote(t));
        fm_jn_addspec(t[0] - '0', fm_jn_field(t + 2));
    } else {
        su_error(1, 0, "invalid file number in field spec: %s", su_quote(t));
    }
    free(t);
}

/* a -o list: specifications split at commas and blanks, one trailing
 * separator allowed, an empty one anywhere else refused */
static void fm_jn_speclist(const char *s) {
    const char *p = s;
    for (;;) {
        size_t n = strcspn(p, ", \t");
        fm_jn_onespec(p, n);
        if (!p[n]) break;
        p += n + 1;
        if (!*p) break;
    }
}

static void fm_jn_settab(const char *a, int *tab) {
    int t = 0;
    if (!a[0]) t = FM_JN_TAB_WHOLE;
    else if (!a[1]) t = (unsigned char)a[0];
    else if (!strcmp(a, "\\0")) t = 0;
    else su_error(1, 0, "multi-character tab %s", su_quote(a));
    if (*tab != FM_JN_TAB_UNSET && *tab != t) su_error(1, 0, "incompatible tabs");
    *tab = t;
}

/* the operands so far (two at most), and what option came before each */
static const char *fm_jn_names[2];
static int fm_jn_ntag[2], fm_jn_nfiles, fm_jn_jpend[2];

/* One operand OP, TAG saying what came before it. A third one makes the
 * oldest kept one more of -o's list, or the FIELD of an old -j1/-j2 --
 * or is an extra operand. */
static void fm_jn_operand(const char *op, int tag) {
    if (fm_jn_nfiles < 2) {
        fm_jn_names[fm_jn_nfiles] = op;
        fm_jn_ntag[fm_jn_nfiles++] = tag;
        return;
    }
    /* the first of the two kept that came after -o or -j1/-j2 is taken
     * for that option ("f1 -o 1.1 2.2 f2" is a -o list and two files) */
    int k = fm_jn_ntag[0] != FM_JN_TAG_NONE ? 0 : fm_jn_ntag[1] != FM_JN_TAG_NONE ? 1 : -1;
    if (k < 0) {
        fm_jn_oflush();
        su_error(0, 0, "extra operand %s", su_quote(op));
        su_try_help(1);
    }
    if (fm_jn_ntag[k] == FM_JN_TAG_O) {
        fm_jn_speclist(fm_jn_names[k]);
    } else {
        int w = fm_jn_ntag[k] == FM_JN_TAG_J2;
        fm_jn_setfield(w ? &fm_jn.j2 : &fm_jn.j1, fm_jn_field(fm_jn_names[k]));
        fm_jn_jpend[w]--;
    }
    if (k == 0) {
        fm_jn_names[0] = fm_jn_names[1];
        fm_jn_ntag[0] = fm_jn_ntag[1];
    }
    fm_jn_names[1] = op;
    fm_jn_ntag[1] = tag;
}

static int do_join(int argc, char **argv) {
    enum { CHECK_OPT = 0x100, NOCHECK_OPT, HEADER_OPT };
    static const struct su_option longs[] = {
        { "ignore-case", SU_NO_ARG, 0, 'i' },
        { "check-order", SU_NO_ARG, 0, CHECK_OPT },
        { "nocheck-order", SU_NO_ARG, 0, NOCHECK_OPT },
        { "zero-terminated", SU_NO_ARG, 0, 'z' },
        { "header", SU_NO_ARG, 0, HEADER_OPT },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&fm_jn, 0, sizeof fm_jn);
    fm_jn.eol = '\n';
    fm_jn.print_pairs = 1;
    fm_jn.check = FM_JN_CHECK_DEFAULT;
    fm_jn.j1 = fm_jn.j2 = FM_JN_UNSET;
    int tab = FM_JN_TAB_UNSET;
    fm_jn_nfiles = 0;
    fm_jn_jpend[0] = fm_jn_jpend[1] = 0;
    int c, tag = FM_JN_TAG_NONE;
    while ((c = su_getopt_long(argc, argv, "-a:e:ij:o:t:v:z1:2:", longs, 0)) != -1) {
        switch (c) {
        case 1:
            fm_jn_operand(su_optarg, tag);
            /* an operand after -o may be more of it; the one right after
             * an -j1 is the only one that may be its field */
            if (tag != FM_JN_TAG_O) tag = FM_JN_TAG_NONE;
            continue;
        case 'a': case 'v': {
            int n = fm_jn_filenum(su_optarg);
            fm_jn.print_unp[n] = 1;
            if (c == 'v') fm_jn.print_pairs = 0;
            break;
        }
        case 'e':
            if (fm_jn.empty && strcmp(fm_jn.empty, su_optarg))
                su_error(1, 0, "conflicting empty-field replacement strings");
            fm_jn.empty = su_optarg;
            break;
        case 'i': fm_jn.ignore_case = 1; break;
        case 'j':
            /* -j1 FIELD / -j2 FIELD, the old forms: only when attached */
            if ((su_optarg[0] == '1' || su_optarg[0] == '2') && !su_optarg[1] &&
                su_optind > 0 && su_optarg == argv[su_optind - 1] + 2) {
                int w = su_optarg[0] == '2';
                fm_jn_jpend[w]++;
                tag = w ? FM_JN_TAG_J2 : FM_JN_TAG_J1;
                continue;
            } else {
                size_t f = fm_jn_field(su_optarg);
                fm_jn_setfield(&fm_jn.j1, f);
                fm_jn_setfield(&fm_jn.j2, f);
            }
            break;
        case '1': fm_jn_setfield(&fm_jn.j1, fm_jn_field(su_optarg)); break;
        case '2': fm_jn_setfield(&fm_jn.j2, fm_jn_field(su_optarg)); break;
        case 'o':
            if (!strcmp(su_optarg, "auto")) fm_jn.autoformat = 1;
            else fm_jn_speclist(su_optarg);
            tag = FM_JN_TAG_O;
            continue;
        case 't': fm_jn_settab(su_optarg, &tab); break;
        case 'z': fm_jn.eol = 0; break;
        case CHECK_OPT: fm_jn.check = FM_JN_CHECK_ON; break;
        case NOCHECK_OPT: fm_jn.check = FM_JN_CHECK_OFF; break;
        case HEADER_OPT: fm_jn.header = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... FILE1 FILE2\n"
                   "Write one line for each pair of lines of FILE1 and FILE2 whose join fields\n"
                   "are the same. The join field is the first, and fields are separated by\n"
                   "blanks, unless options say otherwise.\n"
                   "\n"
                   "One of FILE1 and FILE2 (not both) may be -, standard input.\n"
                   "\n"
                   "  -a FILENUM        also write the lines of file FILENUM (1 or 2) that\n"
                   "                      pair with nothing\n"
                   "  -e STRING         write STRING for each output field that is empty or\n"
                   "                      missing\n"
                   "  -i, --ignore-case  compare join fields regardless of letter case\n"
                   "  -j FIELD          the same as '-1 FIELD -2 FIELD'\n"
                   "  -o FORMAT         build each output line as FORMAT says\n"
                   "  -t CHAR           fields are separated by CHAR, in and out\n"
                   "  -v FILENUM        like -a FILENUM, without the paired lines\n"
                   "  -1 FIELD          the join field of file 1\n"
                   "  -2 FIELD          the join field of file 2\n"
                   "      --check-order    stop at the first line out of order, even when\n"
                   "                         every line pairs\n"
                   "      --nocheck-order  never check the order of the input\n"
                   "      --header     the first line of each file holds the field names:\n"
                   "                     write them together, unpaired\n"
                   "  -z, --zero-terminated  lines end in NUL, not newline\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "Without -t, leading blanks are skipped and runs of blanks separate the\n"
                   "fields; with -t CHAR every CHAR does. FIELD counts from 1. FORMAT is a\n"
                   "list of FILENUM.FIELD and 0 (the join field), separated by commas or\n"
                   "blanks; 'auto' takes the number of fields of each file from its first\n"
                   "line. The default is the join field, then the other fields of FILE1,\n"
                   "then those of FILE2.\n"
                   "\n"
                   "Both files must be sorted on their join fields (as by \"sort -k 1b,1\",\n"
                   "or with \"join -t ''\" when sort is given no options). Lines that are\n"
                   "out of order are reported when some line cannot be paired.\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
        tag = FM_JN_TAG_NONE;
    }
    /* after "--", the rest are operands, none of them tagged */
    for (; su_optind < argc; su_optind++) fm_jn_operand(argv[su_optind], FM_JN_TAG_NONE);
    const char **names = fm_jn_names;
    int nfiles = fm_jn_nfiles;
    int *jpend = fm_jn_jpend;
    if (nfiles == 0) {
        su_error(0, 0, "missing operand");
        su_try_help(1);
    }
    if (nfiles == 1) {
        su_error(0, 0, "missing operand after %s", su_quote(names[0]));
        su_try_help(1);
    }
    /* an -j1 or -j2 that took no operand is -j 1 or -j 2 */
    for (int w = 0; w < 2; w++)
        if (jpend[w] > 0) {
            fm_jn_setfield(&fm_jn.j1, (size_t)w);
            fm_jn_setfield(&fm_jn.j2, (size_t)w);
        }
    if (fm_jn.j1 == FM_JN_UNSET) fm_jn.j1 = 0;
    if (fm_jn.j2 == FM_JN_UNSET) fm_jn.j2 = 0;
    fm_jn.tab = tab == FM_JN_TAB_UNSET ? FM_JN_TAB_BLANKS : tab;
    if (!strcmp(names[0], "-") && !strcmp(names[1], "-"))
        su_error(1, 0, "both files cannot be standard input");

    struct fm_jn_file F[2];
    memset(F, 0, sizeof F);
    for (int i = 0; i < 2; i++) {
        F[i].name = names[i];
        F[i].jf = i ? fm_jn.j2 : fm_jn.j1;
        if (!strcmp(names[i], "-")) F[i].fd = 0;
        else {
            F[i].fd = su_open_read(names[i], 0);
            if (F[i].fd < 0) su_error(1, errno, "%s", su_quotef(names[i]));
        }
    }
    struct fm_jn_seq Q[2];
    memset(Q, 0, sizeof Q);
    fm_jn_extend(&F[0], &Q[0]);
    fm_jn_extend(&F[1], &Q[1]);
    if (fm_jn.autoformat && !fm_jn.nspec) {
        size_t n1 = Q[0].count ? Q[0].l[0].nf : 0;
        size_t n2 = Q[1].count ? Q[1].l[0].nf : 0;
        fm_jn_addspec(0, 0);
        for (size_t k = 0; k < n1; k++) if (k != fm_jn.j1) fm_jn_addspec(1, k);
        for (size_t k = 0; k < n2; k++) if (k != fm_jn.j2) fm_jn_addspec(2, k);
    }
    if (fm_jn.header) {
        /* the first lines go out together, whatever their keys */
        if (Q[0].count || Q[1].count)
            fm_jn_out(Q[0].count ? &Q[0].l[0] : 0, Q[1].count ? &Q[1].l[0] : 0);
        Q[0].count = Q[1].count = 0;
        fm_jn_extend(&F[0], &Q[0]);
        fm_jn_extend(&F[1], &Q[1]);
    }
    while (Q[0].count && Q[1].count) {
        int d = fm_jn_cmp(&Q[0].l[0], fm_jn.j1, &Q[1].l[0], fm_jn.j2);
        if (d < 0) {
            if (fm_jn.print_unp[1]) fm_jn_out(&Q[0].l[0], 0);
            fm_jn_advance(&F[0], &Q[0]);
            fm_jn.seen_unpairable = 1;
            continue;
        }
        if (d > 0) {
            if (fm_jn.print_unp[2]) fm_jn_out(0, &Q[1].l[0]);
            fm_jn_advance(&F[1], &Q[1]);
            fm_jn.seen_unpairable = 1;
            continue;
        }
        /* a run of equal keys on each side: every pairing of the two */
        int eof1 = 0, eof2 = 0;
        for (;;) {
            if (!fm_jn_extend(&F[0], &Q[0])) { eof1 = 1; break; }
            if (fm_jn_cmp(&Q[0].l[Q[0].count - 1], fm_jn.j1, &Q[1].l[0], fm_jn.j2)) break;
        }
        for (;;) {
            if (!fm_jn_extend(&F[1], &Q[1])) { eof2 = 1; break; }
            if (fm_jn_cmp(&Q[0].l[0], fm_jn.j1, &Q[1].l[Q[1].count - 1], fm_jn.j2)) break;
        }
        size_t g1 = eof1 ? Q[0].count : Q[0].count - 1;
        size_t g2 = eof2 ? Q[1].count : Q[1].count - 1;
        if (fm_jn.print_pairs)
            for (size_t a = 0; a < g1; a++)
                for (size_t b = 0; b < g2; b++)
                    fm_jn_out(&Q[0].l[a], &Q[1].l[b]);
        if (eof1) Q[0].count = 0;
        else { fm_jn_swap(&Q[0], 0, Q[0].count - 1); Q[0].count = 1; }
        if (eof2) Q[1].count = 0;
        else { fm_jn_swap(&Q[1], 0, Q[1].count - 1); Q[1].count = 1; }
    }
    /* what is left of one file: written with -a, and read for the check */
    int checking = fm_jn.check == FM_JN_CHECK_ON ||
                   (fm_jn.check == FM_JN_CHECK_DEFAULT && fm_jn.seen_unpairable);
    for (int i = 0; i < 2; i++) {
        if (!Q[i].count || !(fm_jn.print_unp[i + 1] || checking)) continue;
        do {
            if (fm_jn.print_unp[i + 1])
                fm_jn_out(i ? 0 : &Q[i].l[0], i ? &Q[i].l[0] : 0);
        } while (fm_jn_advance(&F[i], &Q[i]));
    }
    for (int i = 0; i < 2; i++) if (F[i].fd > 0) close(F[i].fd);
    if (fm_jn.disorder) {
        fm_jn_oflush();
        su_error(0, 0, "input is not in sorted order");
        fm_jn_oclose();
        return 1;
    }
    fm_jn_oclose();
    return 0;
}

#undef FM_JN_UNSET
#undef FM_JN_TAB_BLANKS
#undef FM_JN_TAB_WHOLE
#undef FM_JN_TAB_UNSET
