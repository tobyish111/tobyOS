/* su_sort.c -- sort, as GNU coreutils 9.7's (logs/utilgate/sort.sh), in the
 * C locale: bytes compare as unsigned chars, no thousands separator, '.' the
 * decimal point.
 *
 *   sort [OPTION]... [FILE]...
 *   sort [OPTION]... --files0-from=F
 *
 *   - Keys: -k POS1[,POS2] with POS = F[.C][bdfgiMhnRrV], and the old
 *     +POS1 [-POS2] (0-based), as GNU reads them; fields split at -t SEP or
 *     at each blank-to-nonblank change; -b, -d, -f, -i, -n, -g, -h, -M, -V,
 *     -R and -r for a key or, globally, for every key that names none of its
 *     own; the whole line, byte by byte, last (unless -s or -u), reversed
 *     with -r.
 *   - -c/-C (--check[=quiet|silent|diagnose-first]) "FILE:N: disorder:
 *     LINE"; -m merges sorted inputs; -u keeps the first of each run of
 *     equal lines; -z; -o FILE (which may be an input); --files0-from;
 *     -S, -T, --parallel, --batch-size, --compress-program taken and checked
 *     as GNU checks them (everything here is sorted in memory); -y ignored.
 *   - --debug: GNU's warnings and each line's keys underlined.
 *   - GNU's messages and exit statuses (2 for trouble, 1 for disorder and
 *     for an argmatch list).
 *
 * The sort is a stable merge sort; GNU's is too, which is what makes -u keep
 * the first of a run and -s keep input order. */

#define SO_NONE ((size_t)-1)

struct so_key {
    size_t sword, schar;     /* start: field, characters into it (0-based) */
    size_t eword, echar;     /* end: field (0-based; SO_NONE: the line's end),
                              * through character echar of it (0: all of it) */
    int skipsblanks, skipeblanks, translate;
    int ignore;              /* 0, 'd' (dictionary order) or 'i' (printable) */
    int numeric, general, human, month, random, version, reverse;
    int obsolete;           /* given as +POS1 [-POS2] */
};

struct so_line { const unsigned char *t; size_t n; };

static struct so_key *so_keys;
static size_t so_nkeys, so_keycap;
static int so_explicit_keys;          /* -k or +POS given */
static struct so_key so_gkey;          /* the global options */
static int so_tab = -1;                /* -t's byte, or -1: blanks */
static unsigned char so_eol = '\n';
static int so_stable, so_unique, so_debug;
static uint64_t so_salt[2];

static int so_blank(unsigned char c) { return c == ' ' || c == '\t' || c == '\n'; }

static int so_has_order(const struct so_key *k) {
    return k->skipsblanks || k->skipeblanks || k->ignore || k->translate ||
           k->numeric || k->general || k->human || k->month || k->random ||
           k->version;
}

static int so_numeric_kind(const struct so_key *k) {
    return k->numeric || k->general || k->human;
}

/* ---- fields ---------------------------------------------------------------- */

static size_t so_skip_fields(const unsigned char *t, size_t len, size_t p, size_t w) {
    if (so_tab >= 0) {
        while (p < len && w--) {
            while (p < len && t[p] != (unsigned char)so_tab) p++;
            if (p < len) p++;
        }
    } else {
        while (p < len && w--) {
            while (p < len && so_blank(t[p])) p++;
            while (p < len && !so_blank(t[p])) p++;
        }
    }
    return p;
}

static size_t so_begfield(const unsigned char *t, size_t len, const struct so_key *k) {
    size_t p = so_skip_fields(t, len, 0, k->sword);
    if (k->skipsblanks) while (p < len && so_blank(t[p])) p++;
    return len - p > k->schar ? p + k->schar : len;
}

static size_t so_limfield(const unsigned char *t, size_t len, const struct so_key *k) {
    size_t p = so_skip_fields(t, len, 0, k->eword);
    if (k->echar == 0) {                    /* to the end of that field */
        if (so_tab >= 0) {
            while (p < len && t[p] != (unsigned char)so_tab) p++;
        } else {
            while (p < len && so_blank(t[p])) p++;
            while (p < len && !so_blank(t[p])) p++;
        }
        return p;
    }
    if (k->skipeblanks) while (p < len && so_blank(t[p])) p++;
    return len - p > k->echar ? p + k->echar : len;
}

static void so_span(const struct so_line *l, const struct so_key *k, size_t *b, size_t *e) {
    size_t beg = so_begfield(l->t, l->n, k);
    size_t lim = k->eword == SO_NONE ? l->n : so_limfield(l->t, l->n, k);
    if (lim < beg) lim = beg;
    *b = beg;
    *e = lim;
}

/* ---- comparisons ------------------------------------------------------------ */

static int so_sign(long v) { return (v > 0) - (v < 0); }

/* -n: [-]digits[.digits] after blanks; anything else is zero */
struct so_num { int neg; const unsigned char *ip, *fp; size_t il, fl; };

static void so_scan_num(const unsigned char *t, size_t n, struct so_num *o, size_t *endp, size_t *digits) {
    size_t p = 0, d = 0;
    while (p < n && so_blank(t[p])) p++;
    o->neg = 0;
    if (p < n && t[p] == '-') { o->neg = 1; p++; }
    size_t i0 = p;
    while (p < n && isdigit(t[p])) p++;
    d += p - i0;
    o->ip = t + i0;
    o->il = p - i0;
    o->fp = 0;
    o->fl = 0;
    if (p < n && t[p] == '.') {
        size_t f0 = ++p;
        while (p < n && isdigit(t[p])) p++;
        d += p - f0;
        o->fp = t + f0;
        o->fl = p - f0;
    }
    /* leading zeros of the whole part and trailing zeros of the fraction
     * change nothing */
    while (o->il && *o->ip == '0') { o->ip++; o->il--; }
    while (o->fl && o->fp[o->fl - 1] == '0') o->fl--;
    if (endp) *endp = p;
    if (digits) *digits = d;
}

static int so_mag(const struct so_num *a, const struct so_num *b) {
    if (a->il != b->il) return a->il < b->il ? -1 : 1;
    int c = memcmp(a->ip, b->ip, a->il);
    if (c) return so_sign(c);
    size_t m = a->fl > b->fl ? a->fl : b->fl;
    for (size_t i = 0; i < m; i++) {
        int x = i < a->fl ? a->fp[i] : '0', y = i < b->fl ? b->fp[i] : '0';
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

static int so_numcmp(const unsigned char *a, size_t an, const unsigned char *b, size_t bn) {
    struct so_num x, y;
    so_scan_num(a, an, &x, 0, 0);
    so_scan_num(b, bn, &y, 0, 0);
    int xs = (x.il || x.fl) ? (x.neg ? -1 : 1) : 0;
    int ys = (y.il || y.fl) ? (y.neg ? -1 : 1) : 0;
    if (xs != ys) return xs < ys ? -1 : 1;
    if (!xs) return 0;
    int m = so_mag(&x, &y);
    return xs < 0 ? -m : m;
}

/* -h: the suffix after the number (KMGTPEZYRQ, k) orders first */
static int so_unit(unsigned char c) {
    static const char units[] = "KMGTPEZYRQ";
    if (c == 'k') return 1;
    const char *u = c ? strchr(units, c) : 0;
    return u ? (int)(u - units) + 1 : 0;
}

static int so_unit_order(const unsigned char *t, size_t n) {
    struct so_num x;
    size_t end;
    so_scan_num(t, n, &x, &end, 0);
    int u = end < n ? so_unit(t[end]) : 0;
    return x.neg ? -u : u;
}

static int so_humcmp(const unsigned char *a, size_t an, const unsigned char *b, size_t bn) {
    int d = so_unit_order(a, an) - so_unit_order(b, bn);
    return d ? so_sign(d) : so_numcmp(a, an, b, bn);
}

/* -g: strtold's numbers; what is not one first, then NaNs, then the rest */
static char *so_cstr(const unsigned char *t, size_t n) {
    static char *buf[2];
    static size_t cap[2];
    static int which;
    which ^= 1;
    if (n + 1 > cap[which]) {
        cap[which] = n + 64;
        buf[which] = (char *)realloc(buf[which], cap[which]);
        if (!buf[which]) su_error(2, ENOMEM, "memory exhausted");
    }
    memcpy(buf[which], t, n);
    buf[which][n] = 0;
    return buf[which];
}

static int so_gencmp(const unsigned char *a, size_t an, const unsigned char *b, size_t bn) {
    char *sa = so_cstr(a, an), *ea;
    long double x = su_strtold(sa, &ea);
    char *sb = so_cstr(b, bn), *eb;
    long double y = su_strtold(sb, &eb);
    if (sa == ea) return sb == eb ? 0 : -1;
    if (sb == eb) return 1;
    if (x < y) return -1;
    if (x > y) return 1;
    if (x == y) return 0;
    if (x != x) return y != y ? 0 : -1;      /* NaN */
    return 1;
}

/* -M: JAN..DEC in the first three characters after blanks, 0 if none */
static int so_getmonth(const unsigned char *t, size_t n, size_t *at) {
    static const char months[12][4] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    size_t p = 0;
    while (p < n && so_blank(t[p])) p++;
    if (at) *at = p;
    if (n - p < 3) return 0;
    for (int m = 0; m < 12; m++)
        if (toupper(t[p]) == months[m][0] && toupper(t[p + 1]) == months[m][1] &&
            toupper(t[p + 2]) == months[m][2])
            return m + 1;
    return 0;
}

/* -V: the Debian version order (digits as numbers, '~' before anything,
 * letters before other characters), first with each name's suffix --
 * (\.[A-Za-z~][A-Za-z0-9~]*)* at its end -- set aside; "" first, then "."
 * and "..", then the other names starting with '.'. */
static int so_vorder(int c) {
    if (isdigit(c)) return 0;
    if (isalpha(c)) return c;
    if (c == '~') return -1;
    return c + 256;
}

static int so_verrevcmp(const unsigned char *a, size_t an, const unsigned char *b, size_t bn) {
    size_t i = 0, j = 0;
    while (i < an || j < bn) {
        while ((i < an && !isdigit(a[i])) || (j < bn && !isdigit(b[j]))) {
            int x = i < an ? so_vorder(a[i]) : 0, y = j < bn ? so_vorder(b[j]) : 0;
            if (x != y) return x - y;
            i++;
            j++;
        }
        while (i < an && a[i] == '0') i++;
        while (j < bn && b[j] == '0') j++;
        int first = 0;
        while (i < an && j < bn && isdigit(a[i]) && isdigit(b[j])) {
            if (!first) first = a[i] - b[j];
            i++;
            j++;
        }
        if (i < an && isdigit(a[i])) return 1;
        if (j < bn && isdigit(b[j])) return -1;
        if (first) return first;
    }
    return 0;
}

static size_t so_prefixlen(const unsigned char *s, size_t n) {
    size_t prefix = 0;
    for (size_t i = 0;;) {
        if (i == n) return prefix;
        i++;
        prefix = i;
        while (i + 1 < n && s[i] == '.' && (isalpha(s[i + 1]) || s[i + 1] == '~'))
            for (i += 2; i < n && (isalnum(s[i]) || s[i] == '~'); i++)
                ;
    }
}

static int so_vercmp(const unsigned char *a, size_t an, const unsigned char *b, size_t bn) {
    if (!an) return bn ? -1 : 0;
    if (!bn) return 1;
    if (a[0] == '.' || b[0] == '.') {
        if (a[0] != '.') return 1;
        if (b[0] != '.') return -1;
        int adot = an == 1, bdot = bn == 1;
        if (adot || bdot) return (int)bdot - (int)adot;
        int add = an == 2 && a[1] == '.', bdd = bn == 2 && b[1] == '.';
        if (add || bdd) return (int)bdd - (int)add;
    }
    size_t ap = so_prefixlen(a, an), bp = so_prefixlen(b, bn);
    int r = so_verrevcmp(a, ap, b, bp);
    if (r || (ap == an && bp == bn)) return so_sign(r);
    return so_sign(so_verrevcmp(a, an, b, bn));
}

/* -d, -i, -f: the bytes compared */
static int so_ignored(int ignore, unsigned char c) {
    if (ignore == 'd') return !isalnum(c) && !so_blank(c);
    if (ignore == 'i') return !isprint(c);
    return 0;
}

static int so_textcmp(const unsigned char *a, size_t an, const unsigned char *b, size_t bn,
                      int ignore, int fold) {
    if (!ignore && !fold) {
        int c = memcmp(a, b, an < bn ? an : bn);
        if (c) return so_sign(c);
        return an < bn ? -1 : an > bn;
    }
    size_t i = 0, j = 0;
    for (;;) {
        while (i < an && so_ignored(ignore, a[i])) i++;
        while (j < bn && so_ignored(ignore, b[j])) j++;
        if (i >= an || j >= bn) break;
        int x = fold ? toupper(a[i]) : a[i], y = fold ? toupper(b[j]) : b[j];
        if (x != y) return x < y ? -1 : 1;
        i++;
        j++;
    }
    return (int)(i < an) - (int)(j < bn);
}

/* -R: a keyed hash of the key as -d -i -f see it; equal keys hash alike */
static uint64_t so_hash(const unsigned char *t, size_t n, int ignore, int fold) {
    uint64_t h = 0xcbf29ce484222325ULL ^ so_salt[0];
    for (size_t i = 0; i < n; i++) {
        unsigned char c = t[i];
        if (so_ignored(ignore, c)) continue;
        if (fold) c = (unsigned char)toupper(c);
        h = (h ^ c) * 0x100000001b3ULL;
    }
    h ^= so_salt[1];
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

static int so_keycmp(const struct so_key *k, const unsigned char *a, size_t an,
                     const unsigned char *b, size_t bn) {
    if (k->random) {
        uint64_t x = so_hash(a, an, k->ignore, k->translate);
        uint64_t y = so_hash(b, bn, k->ignore, k->translate);
        if (x != y) return x < y ? -1 : 1;
        return so_textcmp(a, an, b, bn, k->ignore, k->translate);
    }
    if (k->numeric) return so_numcmp(a, an, b, bn);
    if (k->general) return so_gencmp(a, an, b, bn);
    if (k->human) return so_humcmp(a, an, b, bn);
    if (k->version) return so_vercmp(a, an, b, bn);
    if (k->month) {
        int x = so_getmonth(a, an, 0), y = so_getmonth(b, bn, 0);
        return x < y ? -1 : x > y;
    }
    return so_textcmp(a, an, b, bn, k->ignore, k->translate);
}

static int so_compare(const struct so_line *a, const struct so_line *b) {
    if (so_nkeys) {
        for (size_t i = 0; i < so_nkeys; i++) {
            const struct so_key *k = &so_keys[i];
            size_t ab, ae, bb, be;
            so_span(a, k, &ab, &ae);
            so_span(b, k, &bb, &be);
            int d = so_keycmp(k, a->t + ab, ae - ab, b->t + bb, be - bb);
            if (d) return k->reverse ? -d : d;
        }
        if (so_unique || so_stable) return 0;
    }
    int d = so_textcmp(a->t, a->n, b->t, b->n, 0, 0);
    return so_gkey.reverse ? -d : d;
}

/* ---- the sort ---------------------------------------------------------------- */

static void so_msort(struct so_line *v, struct so_line *tmp, size_t n) {
    if (n < 2) return;
    if (n < 12) {                               /* insertion, stable */
        for (size_t i = 1; i < n; i++) {
            struct so_line x = v[i];
            size_t j = i;
            while (j > 0 && so_compare(&v[j - 1], &x) > 0) { v[j] = v[j - 1]; j--; }
            v[j] = x;
        }
        return;
    }
    size_t h = n / 2;
    so_msort(v, tmp, h);
    so_msort(v + h, tmp, n - h);
    if (so_compare(&v[h - 1], &v[h]) <= 0) return;
    memcpy(tmp, v, h * sizeof *v);
    size_t i = 0, j = h, o = 0;
    while (i < h && j < n) {
        if (so_compare(&v[j], &tmp[i]) < 0) v[o++] = v[j++];
        else v[o++] = tmp[i++];
    }
    while (i < h) v[o++] = tmp[i++];
}

/* ---- input ------------------------------------------------------------------- */

struct so_input { const char *name; struct so_line *v; size_t n; unsigned char *buf; };

static void so_push_line(struct so_line **v, size_t *n, size_t *cap, const unsigned char *t, size_t len) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 256;
        *v = (struct so_line *)realloc(*v, *cap * sizeof **v);
        if (!*v) su_error(2, ENOMEM, "memory exhausted");
    }
    (*v)[*n].t = t;
    (*v)[(*n)++].n = len;
}

/* the whole of NAME ("-": standard input); OPENMSG names a failed open */
static unsigned char *so_slurp(const char *name, const char *openmsg, size_t *len) {
    int fd = strcmp(name, "-") ? su_open_read(name, 0) : 0;
    if (fd < 0) su_error(2, errno, "%s: %s", openmsg, su_quotef(name));
    size_t cap = 65536, n = 0;
    unsigned char *b = (unsigned char *)malloc(cap + 1);
    if (!b) su_error(2, ENOMEM, "memory exhausted");
    for (;;) {
        if (n == cap) {
            cap *= 2;
            b = (unsigned char *)realloc(b, cap + 1);
            if (!b) su_error(2, ENOMEM, "memory exhausted");
        }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0) {
            if (errno == EINTR) continue;
            su_error(2, errno, "read failed: %s", su_quotef(name));
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    if (fd) close(fd);
    *len = n;
    return b;
}

static void so_read_input(struct so_input *in, const char *openmsg) {
    size_t len, cap = 0;
    in->buf = so_slurp(in->name, openmsg, &len);
    in->v = 0;
    in->n = 0;
    size_t s = 0;
    for (size_t i = 0; i < len; i++)
        if (in->buf[i] == so_eol) {
            so_push_line(&in->v, &in->n, &cap, in->buf + s, i - s);
            s = i + 1;
        }
    if (s < len) so_push_line(&in->v, &in->n, &cap, in->buf + s, len - s);
}

/* ---- --debug ----------------------------------------------------------------- */

static size_t so_width(const unsigned char *t, size_t n) {
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        if (t[i] == '\t' || (t[i] >= 0x20 && t[i] != 0x7f)) w++;
    return w;
}

static void so_mark(FILE *o, const unsigned char *t, size_t beg, size_t end, int match) {
    for (size_t i = so_width(t, beg); i; i--) putc(' ', o);
    if (!match) { fputs("^ no match for key\n", o); return; }
    for (size_t i = so_width(t + beg, end - beg); i; i--) putc('_', o);
    putc('\n', o);
}

/* the part of the key a numeric or month key reads */
static void so_debug_key(FILE *o, const struct so_line *l, const struct so_key *k) {
    size_t beg, lim;
    so_span(l, k, &beg, &lim);
    const unsigned char *t = l->t + beg;
    size_t n = lim - beg;
    if (k->numeric || k->human) {
        struct so_num x;
        size_t p = 0, end, digits;
        while (p < n && so_blank(t[p])) p++;
        so_scan_num(t, n, &x, &end, &digits);
        if (digits && k->human && end < n && so_unit(t[end])) end++;
        so_mark(o, l->t, beg + p, beg + end, digits != 0);
    } else if (k->general) {
        size_t p = 0;
        while (p < n && so_blank(t[p])) p++;
        char *s = so_cstr(t + p, n - p), *e;
        (void)su_strtold(s, &e);
        so_mark(o, l->t, beg + p, beg + p + (size_t)(e - s), e != s);
    } else if (k->month) {
        size_t at;
        int m = so_getmonth(t, n, &at);
        so_mark(o, l->t, beg + at, beg + at + 3, m != 0);
    } else
        so_mark(o, l->t, beg, lim, beg < lim);
}

static void so_debug_line(FILE *o, const struct so_line *l) {
    for (size_t i = 0; i < l->n; i++) putc(l->t[i] == '\t' ? '>' : l->t[i], o);
    putc('\n', o);
    for (size_t i = 0; i < so_nkeys; i++) so_debug_key(o, l, &so_keys[i]);
    if (!so_nkeys || !(so_unique || so_stable))
        so_mark(o, l->t, 0, l->n, l->n != 0);
}

/* the options a key names, in GNU's order (b d f g h i M n R r V) */
static void so_opts(const struct so_key *k, int order_only, int with_r, char *out) {
    char *p = out;
    if (!order_only && (k->skipsblanks || k->skipeblanks)) *p++ = 'b';
    if (k->ignore == 'd') *p++ = 'd';
    if (k->translate) *p++ = 'f';
    if (k->general) *p++ = 'g';
    if (k->human) *p++ = 'h';
    if (k->ignore == 'i') *p++ = 'i';
    if (k->month) *p++ = 'M';
    if (k->numeric) *p++ = 'n';
    if (k->random) *p++ = 'R';
    if (with_r && k->reverse) *p++ = 'r';
    if (k->version) *p++ = 'V';
    *p = 0;
}

static void so_warnings(void) {
    fflush(stdout);
    su_error(0, 0, "text ordering performed using simple byte comparison");
    int numbers = 0;
    for (size_t i = 0; i < so_nkeys; i++) {
        const struct so_key *k = &so_keys[i];
        size_t ki = i + 1;
        if (so_numeric_kind(k)) numbers = 1;
        if (!so_explicit_keys) continue;
        if (k->obsolete) {
            char obs[64], sug[64];
            int n = snprintf(obs, sizeof obs, "+%zu", k->sword);
            int m = snprintf(sug, sizeof sug, "-k %zu", k->sword + 1);
            if (k->eword != SO_NONE) {
                snprintf(obs + n, sizeof obs - (size_t)n, " -%zu", k->eword + 1);
                snprintf(sug + m, sizeof sug - (size_t)m, ",%zu", k->eword + 1);
            }
            su_error(0, 0, "obsolescent key %s used; consider %s instead",
                     su_quote(obs), su_quote(sug));
        }
        if (k->eword != SO_NONE && k->eword < k->sword) {
            su_error(0, 0, "key %zu has zero width and will be ignored", ki);
            continue;
        }
        int implicit_skip = so_numeric_kind(k) || k->month;
        int line_offset = k->eword == 0 && k->echar != 0;
        if (so_tab < 0 && !line_offset &&
            ((!k->skipsblanks && !implicit_skip) ||
             (!k->skipsblanks && k->schar) ||
             (!k->skipeblanks && k->echar)))
            su_error(0, 0, "leading blanks are significant in key %zu; "
                     "consider also specifying 'b'", ki);
        if (so_numeric_kind(k) && (k->eword == SO_NONE || k->sword < k->eword))
            su_error(0, 0, "key %zu is numeric and spans multiple fields", ki);
    }
    if (numbers)
        su_error(0, 0, "numbers use '.' as a decimal point in this locale");
    if (!so_explicit_keys) return;
    /* the global options no key took */
    struct so_key ug = so_gkey;
    for (size_t i = 0; i < so_nkeys; i++) {
        const struct so_key *k = &so_keys[i];
        if (k->skipsblanks) ug.skipsblanks = 0;
        if (k->skipeblanks) ug.skipeblanks = 0;
        if (k->ignore) ug.ignore = 0;
        if (k->translate) ug.translate = 0;
        if (k->numeric) ug.numeric = 0;
        if (k->general) ug.general = 0;
        if (k->human) ug.human = 0;
        if (k->month) ug.month = 0;
        if (k->random) ug.random = 0;
        if (k->version) ug.version = 0;
        if (k->reverse) ug.reverse = 0;
    }
    int last_resort = !(so_stable || so_unique);
    char opts[16];
    so_opts(&ug, 0, !last_resort, opts);
    if (opts[0])
        su_error(0, 0, "option%s '-%s' %s ignored", opts[1] ? "s" : "", opts,
                 opts[1] ? "are" : "is");
    if (ug.reverse && last_resort)
        su_error(0, 0, "option '-r' only applies to last-resort comparison");
}

/* ---- options ----------------------------------------------------------------- */

static void so_badspec(const char *spec, const char *why) {
    su_error(2, 0, "%s: invalid field specification %s", why, su_quote(spec));
}

/* a count, as xstrtoumax reads one (blanks, '+'); saturates; 0 if none */
static const char *so_count(const char *s, size_t *out, const char *why) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p))
        su_error(2, 0, "%s: invalid count at start of %s", why, su_quote(s));
    size_t v = 0;
    for (; isdigit((unsigned char)*p); p++) {
        size_t d = (size_t)(*p - '0');
        v = v > (SO_NONE - d) / 10 ? SO_NONE : v * 10 + d;
    }
    *out = v;
    return p;
}

/* ordering letters after a POS; BL: which end 'b' is for (1 start, 2 end, 3 both) */
static const char *so_letters(const char *s, struct so_key *k, int bl) {
    for (;; s++) {
        switch (*s) {
        case 'b':
            if (bl & 1) k->skipsblanks = 1;
            if (bl & 2) k->skipeblanks = 1;
            break;
        case 'd': k->ignore = 'd'; break;
        case 'f': k->translate = 1; break;
        case 'g': k->general = 1; break;
        case 'h': k->human = 1; break;
        case 'i': if (!k->ignore) k->ignore = 'i'; break;
        case 'M': k->month = 1; break;
        case 'n': k->numeric = 1; break;
        case 'R': k->random = 1; break;
        case 'r': k->reverse = 1; break;
        case 'V': k->version = 1; break;
        default: return s;
        }
    }
}

static struct so_key *so_newkey(void) {
    if (so_nkeys == so_keycap) {
        so_keycap = so_keycap ? so_keycap * 2 : 8;
        so_keys = (struct so_key *)realloc(so_keys, so_keycap * sizeof *so_keys);
        if (!so_keys) su_error(2, ENOMEM, "memory exhausted");
    }
    struct so_key *k = &so_keys[so_nkeys++];
    memset(k, 0, sizeof *k);
    k->eword = SO_NONE;
    so_explicit_keys = 1;
    return k;
}

static void so_parse_k(const char *spec) {
    struct so_key *k = so_newkey();
    size_t v;
    const char *s = so_count(spec, &v, "invalid number at field start");
    if (v == 0) so_badspec(spec, "field number is zero");
    k->sword = v - 1;
    if (*s == '.') {
        s = so_count(s + 1, &v, "invalid number after '.'");
        if (v == 0) so_badspec(spec, "character offset is zero");
        k->schar = v - 1;
    }
    s = so_letters(s, k, 1);
    if (*s == ',') {
        s = so_count(s + 1, &v, "invalid number after ','");
        if (v == 0) so_badspec(spec, "field number is zero");
        k->eword = v - 1;
        if (*s == '.') s = so_count(s + 1, &k->echar, "invalid number after '.'");
        s = so_letters(s, k, 2);
    }
    if (*s) so_badspec(spec, "stray character in field spec");
}

/* +POS1: a key if it reads as one, else (0) a file */
static int so_parse_obs1(const char *arg) {
    const char *s = arg + 1;
    if (!isdigit((unsigned char)*s)) return 0;
    struct so_key k;
    memset(&k, 0, sizeof k);
    k.eword = SO_NONE;
    k.obsolete = 1;
    size_t v = 0;
    for (; isdigit((unsigned char)*s); s++) {
        size_t d = (size_t)(*s - '0');
        v = v > (SO_NONE - d) / 10 ? SO_NONE : v * 10 + d;
    }
    k.sword = v;
    if (*s == '.') {
        if (!isdigit((unsigned char)s[1])) return 0;
        v = 0;
        for (s++; isdigit((unsigned char)*s); s++) {
            size_t d = (size_t)(*s - '0');
            v = v > (SO_NONE - d) / 10 ? SO_NONE : v * 10 + d;
        }
        k.schar = v;
    }
    s = so_letters(s, &k, 1);
    if (*s) return 0;
    *so_newkey() = k;
    return 1;
}

/* -POS2 after a +POS1 */
static void so_parse_obs2(const char *arg) {
    struct so_key *k = &so_keys[so_nkeys - 1];
    const char *s = so_count(arg + 1, &k->eword, "invalid number after '-'");
    k->echar = 0;
    if (*s == '.') s = so_count(s + 1, &k->echar, "invalid number after '.'");
    if (!k->echar && k->eword) k->eword--;
    s = so_letters(s, k, 2);
    if (*s) so_badspec(arg, "stray character in field spec");
}

/* -S SIZE: checked, as GNU checks it */
static void so_check_size(const char *s) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) su_error(2, 0, "invalid -S argument %s", su_quote(s));
    uintmax_t v = 0;
    int big = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINTMAX_MAX - d) / 10) big = 1;
        else v = v * 10 + d;
    }
    int power = 1;                              /* no suffix: KiB */
    if (*p) {
        const char *suf = "bkKmMgGtTPEZYRQ%";
        if (!strchr(suf, *p) || p[1]) su_error(2, 0, "invalid suffix in -S argument %s", su_quote(s));
        switch (*p) {
        case 'b': power = 0; if (!big && v > UINTMAX_MAX / 512) big = 1; break;
        case '%': power = 0; break;
        case 'k': case 'K': power = 1; break;
        case 'm': case 'M': power = 2; break;
        case 'g': case 'G': power = 3; break;
        case 't': case 'T': power = 4; break;
        case 'P': power = 5; break;
        case 'E': power = 6; break;
        case 'Z': power = 7; break;
        case 'Y': power = 8; break;
        case 'R': power = 9; break;
        case 'Q': power = 10; break;
        }
    }
    for (int i = 0; i < power && !big; i++) {
        if (v > UINTMAX_MAX / 1024) big = 1;
        else v *= 1024;
    }
    if (big) su_error(2, 0, "-S argument %s too large", su_quote(s));
}

static void so_random_init(const char *source) {
    unsigned char b[16];
    if (source) {
        int fd = su_open_read(source, 0);
        if (fd < 0) su_error(2, errno, "open failed: %s", su_quotef(source));
        size_t n = 0;
        while (n < sizeof b) {
            ssize_t r = read(fd, b + n, sizeof b - n);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) su_error(2, errno, "%s", su_quotef(source));
            if (r == 0) su_error(2, 0, "%s: end of file", su_quote(source));
            n += (size_t)r;
        }
        close(fd);
    } else {
        int fd = open("/dev/urandom", O_RDONLY);
        ssize_t r = fd >= 0 ? read(fd, b, sizeof b) : -1;
        if (fd >= 0) close(fd);
        if (r != (ssize_t)sizeof b) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t x = (uint64_t)ts.tv_sec * 1000000007ULL ^ (uint64_t)ts.tv_nsec ^ (uint64_t)getpid() << 20;
            for (size_t i = 0; i < sizeof b; i++) { x = x * 6364136223846793005ULL + 1442695040888963407ULL; b[i] = (unsigned char)(x >> 56); }
        }
    }
    memcpy(&so_salt[0], b, 8);
    memcpy(&so_salt[1], b + 8, 8);
}

/* the argmatch lists */
static int so_argmatch(const char *arg, const char *opt, const char *const *names,
                       const int *vals, const char *const *groups) {
    size_t len = strlen(arg);
    int found = -1;
    int ambig = 0;
    for (int i = 0; names[i]; i++) {
        if (!strcmp(names[i], arg)) return vals[i];
        if (!strncmp(names[i], arg, len)) {
            if (found >= 0 && vals[found] != vals[i]) ambig = 1;
            found = i;
        }
    }
    if (found >= 0 && !ambig) return vals[found];
    fflush(stdout);
    fprintf(stderr, "%s: %s argument %s for %s\nValid arguments are:\n", su_short,
            ambig ? "ambiguous" : "invalid", su_quote(arg), su_quote(opt));
    for (int i = 0; groups[i]; i++) fprintf(stderr, "  - %s\n", groups[i]);
    su_try_help(1);
}

static void so_incompatible(const char *opts) {
    su_error(2, 0, "options '-%s' are incompatible", opts);
}

static void so_check_key(const struct so_key *k) {
    int kinds = k->numeric + k->general + k->human + k->month +
                (k->version || k->random || k->ignore);
    if (kinds > 1) {
        char o[16];
        so_opts(k, 1, 0, o);
        so_incompatible(o);
    }
}

static int do_sort(int argc, char **argv) {
    enum { SO_RANDSRC = 0x100, SO_SORTWORD, SO_PARALLEL, SO_BATCH, SO_COMPRESS,
           SO_FILES0, SO_DEBUG, SO_CHECK };
    static const struct su_option longs[] = {
        { "ignore-leading-blanks", SU_NO_ARG, 0, 'b' },
        { "check", SU_OPT_ARG, 0, SO_CHECK },
        { "compress-program", SU_REQ_ARG, 0, SO_COMPRESS },
        { "debug", SU_NO_ARG, 0, SO_DEBUG },
        { "dictionary-order", SU_NO_ARG, 0, 'd' },
        { "ignore-case", SU_NO_ARG, 0, 'f' },
        { "files0-from", SU_REQ_ARG, 0, SO_FILES0 },
        { "general-numeric-sort", SU_NO_ARG, 0, 'g' },
        { "ignore-nonprinting", SU_NO_ARG, 0, 'i' },
        { "key", SU_REQ_ARG, 0, 'k' },
        { "merge", SU_NO_ARG, 0, 'm' },
        { "month-sort", SU_NO_ARG, 0, 'M' },
        { "numeric-sort", SU_NO_ARG, 0, 'n' },
        { "human-numeric-sort", SU_NO_ARG, 0, 'h' },
        { "version-sort", SU_NO_ARG, 0, 'V' },
        { "random-sort", SU_NO_ARG, 0, 'R' },
        { "random-source", SU_REQ_ARG, 0, SO_RANDSRC },
        { "sort", SU_REQ_ARG, 0, SO_SORTWORD },
        { "output", SU_REQ_ARG, 0, 'o' },
        { "reverse", SU_NO_ARG, 0, 'r' },
        { "stable", SU_NO_ARG, 0, 's' },
        { "buffer-size", SU_REQ_ARG, 0, 'S' },
        { "field-separator", SU_REQ_ARG, 0, 't' },
        { "temporary-directory", SU_REQ_ARG, 0, 'T' },
        { "unique", SU_NO_ARG, 0, 'u' },
        { "zero-terminated", SU_NO_ARG, 0, 'z' },
        { "parallel", SU_REQ_ARG, 0, SO_PARALLEL },
        { "batch-size", SU_REQ_ARG, 0, SO_BATCH },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&so_gkey, 0, sizeof so_gkey);
    so_gkey.eword = SO_NONE;
    int checkonly = 0, c;
    int mergeonly = 0, posixly = getenv("POSIXLY_CORRECT") != 0, traditional = 0;
    const char *outfile = 0, *files0 = 0, *randsrc = 0, *compress = 0;
    char **files = (char **)malloc((size_t)(argc + 1) * sizeof *files);
    size_t nfiles = 0;
    if (!files) su_error(2, ENOMEM, "memory exhausted");
    for (;;) {
        /* POSIXLY_CORRECT: the options end at the first file -- but the old
         * `sort +POS FILE -o OUT` keeps its -o */
        if (posixly && nfiles && !(traditional && !checkonly && su_nextind < argc &&
                                   argv[su_nextind][0] == '-' && argv[su_nextind][1] == 'o' &&
                                   (argv[su_nextind][2] || su_nextind + 1 != argc))) {
            if (su_nextind < argc && !strcmp(argv[su_nextind], "--")) su_nextind++;
            while (su_nextind < argc) files[nfiles++] = argv[su_nextind++];
            break;
        }
        c = su_getopt_long(argc, argv, "-bcCdfghik:mMno:rRsS:t:T:uVy:z", longs, 0);
        if (c == -1) break;
        switch (c) {
        case 1:
            if (su_optarg[0] == '+' && so_parse_obs1(su_optarg)) {
                traditional = 1;
                if (su_nextind < argc && argv[su_nextind][0] == '-' &&
                    isdigit((unsigned char)argv[su_nextind][1]))
                    so_parse_obs2(argv[su_nextind++]);
            } else
                files[nfiles++] = su_optarg;
            break;
        case 'b': so_gkey.skipsblanks = so_gkey.skipeblanks = 1; break;
        case 'd': case 'f': case 'g': case 'h': case 'i': case 'M': case 'n':
        case 'r': case 'R': case 'V': {
            char l[2] = { (char)c, 0 };
            so_letters(l, &so_gkey, 3);
            break;
        }
        case 'c': case 'C': case SO_CHECK: {
            int want = c;
            if (c == SO_CHECK) {
                want = 'c';
                if (su_optarg) {
                    static const char *const names[] = { "quiet", "silent", "diagnose-first", 0 };
                    static const int vals[] = { 'C', 'C', 'c' };
                    static const char *const groups[] = { "'quiet', 'silent'", "'diagnose-first'", 0 };
                    want = so_argmatch(su_optarg, "--check", names, vals, groups);
                }
            }
            if (checkonly && checkonly != want) so_incompatible("cC");
            checkonly = want;
            break;
        }
        case SO_COMPRESS:
            if (compress && strcmp(compress, su_optarg))
                su_error(2, 0, "multiple compress programs specified");
            compress = su_optarg;
            break;
        case SO_DEBUG: so_debug = 1; break;
        case SO_FILES0: files0 = su_optarg; break;
        case 'k': so_parse_k(su_optarg); break;
        case 'm': mergeonly = 1; break;
        case SO_RANDSRC:
            if (randsrc && strcmp(randsrc, su_optarg))
                su_error(2, 0, "multiple random sources specified");
            randsrc = su_optarg;
            break;
        case 's': so_stable = 1; break;
        case SO_SORTWORD: {
            static const char *const names[] = { "general-numeric", "human-numeric", "month",
                                                 "numeric", "random", "version", 0 };
            static const int vals[] = { 'g', 'h', 'M', 'n', 'R', 'V' };
            static const char *const groups[] = { "'general-numeric'", "'human-numeric'",
                                                  "'month'", "'numeric'", "'random'",
                                                  "'version'", 0 };
            char l[2] = { (char)so_argmatch(su_optarg, "--sort", names, vals, groups), 0 };
            so_letters(l, &so_gkey, 3);
            break;
        }
        case 'o':
            if (outfile && strcmp(outfile, su_optarg))
                su_error(2, 0, "multiple output files specified");
            outfile = su_optarg;
            break;
        case 'S': so_check_size(su_optarg); break;
        case 't': {
            int newtab = (unsigned char)su_optarg[0];
            if (!newtab) su_error(2, 0, "empty tab");
            if (su_optarg[1]) {
                if (!strcmp(su_optarg, "\\0")) newtab = 0;
                else su_error(2, 0, "multi-character tab %s", su_quote(su_optarg));
            }
            if (so_tab >= 0 && so_tab != newtab) su_error(2, 0, "incompatible tabs");
            so_tab = newtab;
            break;
        }
        case 'T': break;
        case SO_PARALLEL: {
            uintmax_t v;
            int r = su_strtoumax(su_optarg, 0, &v, "");
            if (r == SU_NUM_INVALID || r == SU_NUM_BADSUFFIX)
                su_error(2, 0, "invalid --parallel argument %s", su_quote(su_optarg));
            if (r == SU_NUM_OK && v == 0) su_error(2, 0, "number in parallel must be nonzero");
            break;
        }
        case SO_BATCH: {
            uintmax_t v;
            int r = su_strtoumax(su_optarg, 0, &v, "");
            if (r == SU_NUM_INVALID || r == SU_NUM_BADSUFFIX)
                su_error(2, 0, "invalid --batch-size argument %s", su_quote(su_optarg));
            if (r == SU_NUM_OK && v < 2) {
                su_error(0, 0, "invalid --batch-size argument %s", su_quote(su_optarg));
                su_error(2, 0, "minimum --batch-size argument is %s", su_quote("2"));
            }
            /* GNU's limit: the open-file limit less three */
            uintmax_t max = 1021;                  /* tobyOS: 1024 descriptors */
#ifdef SU_HOST
            struct rlimit rl;
            if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 3)
                max = (uintmax_t)rl.rlim_cur - 3;
#endif
            if (r == SU_NUM_OVERFLOW || v > max) {
                su_error(0, 0, "--batch-size argument %s too large", su_quote(su_optarg));
                su_error(2, 0, "maximum --batch-size argument with current rlimit is %ju", max);
            }
            break;
        }
        case 'u': so_unique = 1; break;
        case 'y':
            /* Solaris's -y[N], taken and ignored; a separate N only if digits */
            if (su_optarg == argv[su_nextind - 1] && su_optarg[strspn(su_optarg, "0123456789")])
                su_nextind--;
            break;
        case 'z': so_eol = 0; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n"
                   "  or:  %s [OPTION]... --files0-from=F\n"
                   "Put the lines of the FILEs together, in order, on standard output.\n"
                   "With no FILE, or when FILE is -, read standard input.\n"
                   "\n"
                   "How to order:\n"
                   "  -b, --ignore-leading-blanks  blanks at the start of a key do not count\n"
                   "  -d, --dictionary-order      only blanks, letters and digits count\n"
                   "  -f, --ignore-case           lower case as upper case\n"
                   "  -g, --general-numeric-sort  as floating-point numbers\n"
                   "  -i, --ignore-nonprinting    only printable characters count\n"
                   "  -M, --month-sort            JAN < ... < DEC, anything else first\n"
                   "  -h, --human-numeric-sort    numbers with K, M, G... after them\n"
                   "  -n, --numeric-sort          as numbers\n"
                   "  -R, --random-sort           shuffled, equal keys together\n"
                   "      --random-source=FILE    take the shuffle's randomness from FILE\n"
                   "  -r, --reverse               the other way round\n"
                   "      --sort=WORD             general-numeric -g, human-numeric -h,\n"
                   "                              month -M, numeric -n, random -R,\n"
                   "                              version -V\n"
                   "  -V, --version-sort          as version numbers in names\n"
                   "\n"
                   "Other options:\n"
                   "      --batch-size=NMERGE     merge at most NMERGE inputs at once\n"
                   "  -c, --check, --check=diagnose-first  say whether the input is in\n"
                   "                              order, naming the first line that is not\n"
                   "  -C, --check=quiet, --check=silent  the same, silently\n"
                   "      --compress-program=PROG  for temporary files (none are needed)\n"
                   "      --debug                 show each key, and warn of doubtful use\n"
                   "      --files0-from=F         the inputs are named in F, NUL-separated;\n"
                   "                              - reads the names from standard input\n"
                   "  -k, --key=KEYDEF            order by a key (below); more than one\n"
                   "                              key: the next breaks the last one's ties\n"
                   "  -m, --merge                 merge inputs already in order\n"
                   "  -o, --output=FILE           write to FILE (it may be an input)\n"
                   "  -s, --stable                no last-resort comparison\n"
                   "  -S, --buffer-size=SIZE      a memory hint (sorting is in memory)\n"
                   "  -t, --field-separator=SEP   fields end at SEP, not at blanks\n"
                   "  -T, --temporary-directory=DIR  for temporary files\n"
                   "      --parallel=N            sorts to run at once\n"
                   "  -u, --unique                one line of each run of equal ones\n"
                   "  -z, --zero-terminated       lines end at NUL, not newline\n"
                   "      --help        show this help and exit\n"
                   "      --version     show the version and exit\n"
                   "\n"
                   "KEYDEF is F[.C][OPTS][,F[.C][OPTS]]: from field F's character C to the\n"
                   "end of the second F (its character C), fields and characters counted\n"
                   "from 1; no second position is the line's end, a C of 0 there the\n"
                   "field's end. OPTS are ordering letters [bdfgiMhnRrV] for that key\n"
                   "alone. Without -t, a field's leading blanks are part of it.\n"
                   "\n"
                   "SIZE takes a suffix: %% of memory, b, K, M, G, T, P, E, Z, Y, R, Q.\n"
                   "Without -s or -u, lines whose keys are equal are put in order by\n"
                   "the whole line, byte by byte.\n",
                   su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(2);
        }
    }
    if (c == -1) while (su_optind < argc) files[nfiles++] = argv[su_optind++];

    /* keys with no ordering options of their own take the global ones */
    for (size_t i = 0; i < so_nkeys; i++) {
        struct so_key *k = &so_keys[i];
        if (!so_has_order(k) && !k->reverse) {
            k->skipsblanks = so_gkey.skipsblanks;
            k->skipeblanks = so_gkey.skipeblanks;
            k->ignore = so_gkey.ignore;
            k->translate = so_gkey.translate;
            k->numeric = so_gkey.numeric;
            k->general = so_gkey.general;
            k->human = so_gkey.human;
            k->month = so_gkey.month;
            k->random = so_gkey.random;
            k->version = so_gkey.version;
            k->reverse = so_gkey.reverse;
        }
    }
    so_check_key(&so_gkey);
    for (size_t i = 0; i < so_nkeys; i++) so_check_key(&so_keys[i]);
    if (!so_nkeys && so_has_order(&so_gkey)) {        /* the whole line, so ordered */
        so_keys = (struct so_key *)malloc(sizeof *so_keys);
        if (!so_keys) su_error(2, ENOMEM, "memory exhausted");
        so_keys[0] = so_gkey;
        so_nkeys = 1;
    }
    if (so_debug) {
        if (checkonly) {
            char o[] = "-? --debug";
            o[1] = (char)checkonly;
            su_error(2, 0, "options '%s' are incompatible", o);
        }
        if (outfile) su_error(2, 0, "options '-o --debug' are incompatible");
    }
    if (checkonly && outfile) {
        char o[3] = { (char)checkonly, 'o', 0 };
        so_incompatible(o);
    }

    /* --files0-from */
    unsigned char *names = 0;
    if (files0) {
        if (nfiles) {
            fflush(stdout);
            fprintf(stderr, "%s: extra operand %s\n", su_short, su_quote(files[0]));
            fprintf(stderr, "file operands cannot be combined with --files0-from\n");
            su_try_help(2);
        }
        size_t len;
        names = so_slurp(files0, "open failed", &len);
        size_t s = 0, idx = 0;
        int from_stdin = !strcmp(files0, "-");
        files = (char **)realloc(files, (len + 2) * sizeof *files);
        if (!files) su_error(2, ENOMEM, "memory exhausted");
        for (size_t i = 0; i <= len; i++) {
            if (i < len && names[i]) continue;
            if (i == len && s == len) break;          /* no name after the last NUL */
            names[i] = 0;
            idx++;
            const char *nm = (const char *)names + s;
            if (!*nm) su_error(2, 0, "%s:%zu: invalid zero-length file name", su_quotef(files0), idx);
            if (from_stdin && !strcmp(nm, "-"))
                su_error(2, 0, "when reading file names from stdin, no file name of %s allowed",
                         su_quoteaf("-"));
            files[nfiles++] = (char *)nm;
            s = i + 1;
        }
        if (!nfiles) su_error(2, 0, "no input from %s", su_quote(files0));
    }
    if (!nfiles) files[nfiles++] = (char *)"-";

    if (checkonly && nfiles > 1)
        su_error(2, 0, "extra operand %s not allowed with -%c", su_quote(files[1]), checkonly);

    for (size_t i = 0; i < so_nkeys; i++)
        if (so_keys[i].random) { so_random_init(randsrc); break; }
    if (so_debug) so_warnings();

    /* check */
    if (checkonly) {
        struct so_input in = { files[0], 0, 0, 0 };
        so_read_input(&in, "open failed");
        for (size_t i = 1; i < in.n; i++) {
            int d = so_compare(&in.v[i - 1], &in.v[i]);
            if (d > 0 || (so_unique && d == 0)) {
                if (checkonly == 'c') {
                    fflush(stdout);
                    fprintf(stderr, "%s: %s:%zu: disorder: ", su_short, files[0], i + 1);
                    fwrite(in.v[i].t, 1, in.v[i].n, stderr);
                    putc(so_eol, stderr);
                }
                return 1;
            }
        }
        return 0;
    }

    struct so_input *in = (struct so_input *)calloc(nfiles, sizeof *in);
    if (!in) su_error(2, ENOMEM, "memory exhausted");
    size_t total = 0;
    for (size_t i = 0; i < nfiles; i++) {
        in[i].name = files[i];
        so_read_input(&in[i], "cannot read");
        total += in[i].n;
    }
    struct so_line *all = (struct so_line *)malloc((total + 1) * sizeof *all);
    if (!all) su_error(2, ENOMEM, "memory exhausted");
    size_t n = 0;
    if (mergeonly) {
        /* already in order: take the least head each time, the earlier
         * input first on a tie */
        size_t *pos = (size_t *)calloc(nfiles, sizeof *pos);
        if (!pos) su_error(2, ENOMEM, "memory exhausted");
        for (;;) {
            size_t best = SO_NONE;
            for (size_t i = 0; i < nfiles; i++)
                if (pos[i] < in[i].n &&
                    (best == SO_NONE || so_compare(&in[i].v[pos[i]], &in[best].v[pos[best]]) < 0))
                    best = i;
            if (best == SO_NONE) break;
            all[n++] = in[best].v[pos[best]++];
        }
        free(pos);
    } else {
        for (size_t i = 0; i < nfiles; i++)
            for (size_t j = 0; j < in[i].n; j++) all[n++] = in[i].v[j];
        struct so_line *tmp = (struct so_line *)malloc((n / 2 + 1) * sizeof *tmp);
        if (!tmp) su_error(2, ENOMEM, "memory exhausted");
        so_msort(all, tmp, n);
        free(tmp);
    }

    FILE *o = stdout;
    if (outfile) {
        o = fopen(outfile, "w");
        if (!o) su_error(2, errno, "open failed: %s", su_quotef(outfile));
    }
    const struct so_line *last = 0;
    for (size_t i = 0; i < n; i++) {
        if (so_unique && last && so_compare(last, &all[i]) == 0) continue;
        last = &all[i];
        if (so_debug) so_debug_line(o, &all[i]);
        else {
            fwrite(all[i].t, 1, all[i].n, o);
            putc(so_eol, o);
        }
    }
    if (fflush(o) != 0 || ferror(o)) {
        int e = errno;
        if (o == stdout) {
            su_error(0, e, "fflush failed: %s", su_quote("standard output"));
            su_error(2, 0, "write error");
        }
        su_error(2, e, "fflush failed: %s", su_quotef(outfile));
    }
    if (o != stdout && fclose(o) != 0) su_error(2, errno, "close failed: %s", su_quotef(outfile));
    return 0;
}
