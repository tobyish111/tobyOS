/* su_od.c -- od, as GNU coreutils 9.7's (logs/utilgate/od.sh).
 * Every name here starts od_.
 *
 * Measured against gnuod in WSL, case by case; what follows is what was
 * measured, not what the manual promises.
 *
 * FORMATS. -t TYPE..., and the traditional letters, each add a line per
 * block: a (named characters, high bit ignored), c (characters and C
 * escapes, octal otherwise), d o u x with a size (1 2 4 8, or C S I L;
 * none means 4), f with a size (2 or H for IEEE half, B for bfloat16, 4
 * or F, 8 or D, 16 or L for the x87 long double; none means 8), each with
 * an optional z (the line's bytes as text at its end). -a -b -c -d -f -i
 * -l -o -s -x, and the undocumented -B -D -e -F -h -H -I -L -O -X, are
 * a o1 c u2 fF dI dL o2 d2 x2, o2 u4 fD fD x2 x4 dL dL o4 x4. A bad -t is
 * reported and option parsing goes on; od exits 1 when it is done.
 *
 * LAYOUT. A block is the largest size; every format's line is padded to
 * the widest one's block width, the extra spaces spread over its fields
 * the way GNU spreads them (field i of n gets pad*(n-i)/n - pad*(n-1-i)/n).
 * Integers: d and u right-aligned, o and x zero-filled. Floats print the
 * shortest %g that reads back as the same value, starting at 6, 15 or 18
 * digits (1 for a subnormal), in fields of 15, 24 or 29 characters; half
 * and bfloat16 are printed as the float they widen to. -w is the line
 * width (16; -w alone is 32); one that is not a multiple of the block is
 * replaced by the block, with a warning. -w0 and a negative -w abort GNU's
 * od outright (status 134, nothing printed): that is copied.
 *
 * ADDRESSES. -A d o x n: seven decimal or octal digits, six hex, or none.
 * A line equal to the one before it is a "*" unless -v. The address
 * after the last byte ends the dump.
 *
 * INPUT. The files are one stream ("-" is stdin). -j skips, -N limits;
 * both take 0x hex and 0 octal, and the suffixes b k K m M G T P E Z Y R
 * Q, with B or D after a letter for powers of 1000 and iB for 1024. A
 * file that will not open is reported and skipped; when none opens, od
 * stops there. Skipping past the end is "cannot skip past end of combined
 * input". A big regular file is skipped by its size, a small one is read
 * (GNU's guard against /proc's zero sizes), anything else is sought.
 *
 * TRADITIONAL OPERANDS. With no option but the traditional letters (and
 * --endian), `od FILE [+]OFFSET` and `od +OFFSET` take the offset (octal;
 * 0x hex; b x512, B x1024 -- in 9.7 a '.' makes it a file name, so that
 * is copied); --traditional also takes `[FILE] OFFSET LABEL`, and a label
 * prints as "(LABEL)" after each address -- with -An it prints the real
 * address, in octal, which is what 9.7 does.
 *
 * -S N: the NUL-terminated runs of at least N printable characters, each
 * after its address. With -N the run that the limit cuts is printed too,
 * at an address one too low, and the scan stops string_min bytes short of
 * the limit: both are GNU's, measured, and copied.
 *
 * RELIES ON su_num.c: the big integers (sn_big, sn_pow_mul, sn_shl, ...)
 * that the float conversion is done with -- exactly, in integers, so the
 * guest (no SSE) prints what glibc prints. */

/* ---- output -------------------------------------------------------------- */

struct od_buf { char *s; size_t n, cap; };

static void od_put(struct od_buf *b, const char *t, size_t len) {
    if (b->n + len + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->n + len + 1) c *= 2;
        char *ns = (char *)realloc(b->s, c);
        if (!ns) su_error(1, 0, "memory exhausted");
        b->s = ns;
        b->cap = c;
    }
    memcpy(b->s + b->n, t, len);
    b->n += len;
    b->s[b->n] = 0;
}
static void od_putc(struct od_buf *b, char c) { od_put(b, &c, 1); }
static void od_puts(struct od_buf *b, const char *t) { od_put(b, t, strlen(t)); }
static void od_spaces(struct od_buf *b, size_t n) { while (n--) od_putc(b, ' '); }

/* V in BASE (8, 10 or 16), at least MINDIG digits (zero-filled) */
static int od_utoa(char *out, uintmax_t v, int base, int mindig) {
    char t[72];
    int n = 0;
    do { t[n++] = "0123456789abcdef"[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    while (n < mindig) t[n++] = '0';
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
    return n;
}

/* S right-aligned in WIDTH */
static void od_right(struct od_buf *b, const char *s, int width) {
    int len = (int)strlen(s);
    if (len < width) od_spaces(b, (size_t)(width - len));
    od_puts(b, s);
}

/* ---- numbers in options -------------------------------------------------- */

#define OD_NUM_OK 0
#define OD_NUM_INVALID 1
#define OD_NUM_SUFFIX 2
#define OD_NUM_BIG 3

static int od_digit(int c, int base) {
    int d = c >= '0' && c <= '9' ? c - '0'
          : c >= 'a' && c <= 'f' ? c - 'a' + 10
          : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
    return d < base ? d : -1;
}

static int od_mul(uintmax_t *v, uintmax_t m) {
    if (*v && m > UINTMAX_MAX / *v) { *v = UINTMAX_MAX; return 1; }
    *v *= m;
    return 0;
}

/* -j -N -S: strtoumax base 0 (0x hex, 0 octal), then a multiplier */
static int od_parse_size(const char *s, uintmax_t *out) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') return OD_NUM_INVALID;
    if (*p == '+') p++;
    int base = 10;
    const char *q = p;
    if (q[0] == '0') {
        if ((q[1] == 'x' || q[1] == 'X') && od_digit((unsigned char)q[2], 16) >= 0) { base = 16; q += 2; }
        else base = 8;
    }
    uintmax_t v = 0;
    int over = 0;
    if (od_digit((unsigned char)*q, base) < 0) {
        /* no number: a suffix alone means one of it */
        if (*p && strchr("bkKmMGTPEZYRQ", *p)) { v = 1; q = p; }
        else return OD_NUM_INVALID;
    } else {
        for (int d; (d = od_digit((unsigned char)*q, base)) >= 0; q++) {
            if (v > (UINTMAX_MAX - (uintmax_t)d) / (uintmax_t)base) over = 1;
            else v = v * (uintmax_t)base + (uintmax_t)d;
        }
    }
    if (*q) {
        int power = 0;
        uintmax_t k = 1024;
        switch (*q) {
        case 'b': power = -1; break;
        case 'k': case 'K': power = 1; break;
        case 'm': case 'M': power = 2; break;
        case 'G': power = 3; break;
        case 'T': power = 4; break;
        case 'P': power = 5; break;
        case 'E': power = 6; break;
        case 'Z': power = 7; break;
        case 'Y': power = 8; break;
        case 'R': power = 9; break;
        case 'Q': power = 10; break;
        default: return OD_NUM_SUFFIX;
        }
        q++;
        if (power > 0) {
            if (q[0] == 'i' && q[1] == 'B') q += 2;
            else if (q[0] == 'B' || q[0] == 'D') { k = 1000; q++; }
        }
        if (*q) return OD_NUM_SUFFIX;
        if (power < 0) over |= od_mul(&v, 512);
        for (int i = 0; i < power; i++) over |= od_mul(&v, k);
    }
    *out = over ? UINTMAX_MAX : v;
    return over ? OD_NUM_BIG : OD_NUM_OK;
}

static void od_num_fail(int rc, const char *opt, const char *arg) {
    if (rc == OD_NUM_INVALID) su_error(1, 0, "invalid %s argument %s", opt, su_quote(arg));
    if (rc == OD_NUM_SUFFIX) su_error(1, 0, "invalid suffix in %s argument %s", opt, su_quote(arg));
    su_error(1, 0, "%s argument %s too large", opt, su_quote(arg));
}

/* -w: a signed decimal, no suffixes */
static intmax_t od_parse_width(const char *s, const char *opt) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
    if (!isdigit((unsigned char)*p)) od_num_fail(OD_NUM_INVALID, opt, s);
    uintmax_t v = 0;
    int over = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINTMAX_MAX - d) / 10) over = 1;
        else v = v * 10 + d;
    }
    if (*p) od_num_fail(OD_NUM_SUFFIX, opt, s);
    if (over || v > (uintmax_t)INTMAX_MAX + (neg ? 1 : 0)) od_num_fail(OD_NUM_BIG, opt, s);
    return neg ? (intmax_t)(0 - v) : (intmax_t)v;
}

/* a traditional [+]OFFSET: octal, or 0x hex; b x512, B x1024 */
static int od_parse_offset(const char *s, uintmax_t *out) {
    const char *p = s;
    if (*p == '+') p++;
    if (strchr(p, '.')) return 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    int base = 8;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    if (od_digit((unsigned char)*p, base) < 0) return 0;
    uintmax_t v = 0;
    for (int d; (d = od_digit((unsigned char)*p, base)) >= 0; p++) {
        if (v > (UINTMAX_MAX - (uintmax_t)d) / (uintmax_t)base) return 0;
        v = v * (uintmax_t)base + (uintmax_t)d;
    }
    if (*p == 'b' || *p == 'B') {
        if (od_mul(&v, *p == 'b' ? 512 : 1024)) return 0;
        p++;
    }
    if (*p) return 0;
    *out = v;
    return 1;
}

/* ---- format specifications ----------------------------------------------- */

enum { OD_SDEC, OD_UDEC, OD_OCT, OD_HEX, OD_FLT, OD_NAMED, OD_CHAR };
enum { OD_F_HALF, OD_F_BF16, OD_F_FLOAT, OD_F_DOUBLE, OD_F_LDOUBLE };

struct od_spec {
    int kind, size, fkind;
    int width;              /* one field, its leading blank included */
    int z;                  /* the bytes as text at the end of the line */
    int *pads;              /* the extra blanks before each field of a block */
};

static struct od_spec *od_specs;
static int od_nspecs, od_specs_cap;

static void od_add_spec(int kind, int size, int fkind, int z) {
    if (od_nspecs == od_specs_cap) {
        od_specs_cap = od_specs_cap ? od_specs_cap * 2 : 8;
        od_specs = (struct od_spec *)realloc(od_specs, (size_t)od_specs_cap * sizeof *od_specs);
        if (!od_specs) su_error(1, 0, "memory exhausted");
    }
    struct od_spec *s = &od_specs[od_nspecs++];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->size = size;
    s->fkind = fkind;
    s->z = z;
    static const int sdec[9] = { 0, 4, 6, 0, 11, 0, 0, 0, 20 };
    static const int udec[9] = { 0, 3, 5, 0, 10, 0, 0, 0, 20 };
    static const int oct[9]  = { 0, 3, 6, 0, 11, 0, 0, 0, 22 };
    switch (kind) {
    case OD_SDEC: s->width = 1 + sdec[size]; break;
    case OD_UDEC: s->width = 1 + udec[size]; break;
    case OD_OCT:  s->width = 1 + oct[size]; break;
    case OD_HEX:  s->width = 1 + 2 * size; break;
    case OD_NAMED: case OD_CHAR: s->width = 4; break;
    case OD_FLT:
        /* the longest %g each type can print: -1.17549435e-38 and its kin */
        s->width = 1 + (fkind == OD_F_DOUBLE ? 24 : fkind == OD_F_LDOUBLE ? 29 : 15);
        break;
    }
}

/* one -t argument; 0, or -1 after saying what was wrong with it */
static int od_decode_types(const char *arg) {
    const char *p = arg;
    while (*p) {
        int kind, size = 0, fkind = 0;
        char c = *p++;
        switch (c) {
        case 'a': kind = OD_NAMED; size = 1; break;
        case 'c': kind = OD_CHAR; size = 1; break;
        case 'd': case 'o': case 'u': case 'x':
            kind = c == 'd' ? OD_SDEC : c == 'o' ? OD_OCT : c == 'u' ? OD_UDEC : OD_HEX;
            size = 4;
            if (*p == 'C') { size = 1; p++; }
            else if (*p == 'S') { size = 2; p++; }
            else if (*p == 'I') { size = 4; p++; }
            else if (*p == 'L') { size = 8; p++; }
            else if (isdigit((unsigned char)*p)) {
                uintmax_t v = 0;
                int over = 0;
                for (; isdigit((unsigned char)*p); p++) {
                    unsigned d = (unsigned)(*p - '0');
                    if (v > (UINTMAX_MAX - d) / 10) over = 1;
                    else v = v * 10 + d;
                }
                if (over) {
                    su_error(0, 0, "invalid type string %s", su_quote(arg));
                    return -1;
                }
                if (v != 1 && v != 2 && v != 4 && v != 8) {
                    char nb[32];
                    od_utoa(nb, v, 10, 1);
                    su_error(0, 0, "invalid type string %s;\n"
                             "this system doesn't provide a %s-byte integral type",
                             su_quote(arg), nb);
                    return -1;
                }
                size = (int)v;
            }
            break;
        case 'f':
            kind = OD_FLT;
            size = 8;
            fkind = OD_F_DOUBLE;
            if (*p == 'F') { size = 4; fkind = OD_F_FLOAT; p++; }
            else if (*p == 'D') { size = 8; fkind = OD_F_DOUBLE; p++; }
            else if (*p == 'L') { size = 16; fkind = OD_F_LDOUBLE; p++; }
            else if (*p == 'H') { size = 2; fkind = OD_F_HALF; p++; }
            else if (*p == 'B') { size = 2; fkind = OD_F_BF16; p++; }
            else if (isdigit((unsigned char)*p)) {
                uintmax_t v = 0;
                int over = 0;
                for (; isdigit((unsigned char)*p); p++) {
                    unsigned d = (unsigned)(*p - '0');
                    if (v > (UINTMAX_MAX - d) / 10) over = 1;
                    else v = v * 10 + d;
                }
                if (over) {
                    su_error(0, 0, "invalid type string %s", su_quote(arg));
                    return -1;
                }
                if (v == 2) fkind = OD_F_HALF;
                else if (v == 4) fkind = OD_F_FLOAT;
                else if (v == 8) fkind = OD_F_DOUBLE;
                else if (v == 16) fkind = OD_F_LDOUBLE;
                else {
                    char nb[32];
                    od_utoa(nb, v, 10, 1);
                    su_error(0, 0, "invalid type string %s;\n"
                             "this system doesn't provide a %s-byte floating point type",
                             su_quote(arg), nb);
                    return -1;
                }
                size = (int)v;
            }
            break;
        default:
            su_error(0, 0, "invalid character '%c' in type string %s", c, su_quote(arg));
            return -1;
        }
        int z = 0;
        if (*p == 'z') { z = 1; p++; }
        od_add_spec(kind, size, fkind, z);
    }
    return 0;
}

/* ---- floats: the shortest %g that reads back --------------------------------
 *
 * GNU prints a float with %.*g at the type's DIG digits (1 for a
 * subnormal), one more each time until strtof/strtod/strtold reads it
 * back as the same value. Here the same answer is found in integers: the
 * value x = M * 2^lsb, the two midpoints to its neighbours, and the first
 * ~25 decimal digits of each, as floor(v * 10^s) (and whether that
 * floor dropped anything). A candidate D, x rounded half-even to p
 * digits, reads back as x when it lies strictly between the midpoints --
 * or on one, when M is even (the reader rounds a tie to even). */

static void od_big_copy(struct sn_big *d, const struct sn_big *s) {
    sn_reserve(d, s->n ? s->n : 1);
    if (s->n) memcpy(d->d, s->d, s->n * sizeof *d->d);
    d->n = s->n;
}

/* Q = floor(NUM / DEN) (NUM is consumed), and whether a remainder was left */
static void od_divq(struct sn_big *num, const struct sn_big *den, struct sn_big *q, int *rem) {
    q->n = 0;
    long shift = (long)sn_bitlen(num) - (long)sn_bitlen(den);
    if (shift >= 0) {
        struct sn_big d = { 0, 0, 0 };
        od_big_copy(&d, den);
        sn_shl(&d, (size_t)shift);
        size_t words = (size_t)shift / 32 + 1;
        sn_reserve(q, words);
        memset(q->d, 0, words * sizeof *q->d);
        q->n = words;
        for (long i = shift; i >= 0; i--) {
            if (sn_cmp(num, &d) >= 0) {
                sn_sub(num, &d);
                q->d[i / 32] |= 1u << (i % 32);
            }
            sn_shr(&d, 1);
        }
        sn_trim(q);
        sn_free(&d);
    }
    *rem = !sn_is_zero(num);
}

/* floor(K * 2^E * 10^S) in decimal (malloc'd), *INEXACT when it dropped a part */
static char *od_scaled(const struct sn_big *K, long E, long S, int *inexact) {
    struct sn_big num = { 0, 0, 0 }, den = { 0, 0, 0 }, q = { 0, 0, 0 };
    od_big_copy(&num, K);
    sn_set_u64(&den, 1);
    if (S > 0) sn_pow_mul(&num, 5, S);
    else if (S < 0) sn_pow_mul(&den, 5, -S);
    long e2 = E + S;
    if (e2 > 0) sn_shl(&num, (size_t)e2);
    else if (e2 < 0) sn_shl(&den, (size_t)-e2);
    od_divq(&num, &den, &q, inexact);
    sn_free(&num);
    sn_free(&den);
    return sn_to_dec(&q);
}

/* decimal strings without leading zeros, compared as numbers */
static int od_deccmp(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return la < lb ? -1 : 1;
    int c = strcmp(a, b);
    return c < 0 ? -1 : c > 0;
}

/* NEG M * 2^E (M nonzero) as GNU's od prints a type with P significant
 * bits, normal from 2^EMIN, read at DIG digits up to DECDIG */
static void od_shortest(struct od_buf *out, int neg, uint64_t m, long e,
                        int P, long emin, int dig, int decdig) {
    int bl = 64;
    while (!(m >> (bl - 1))) bl--;
    long t = bl - 1 + e;
    int sub = t < emin;
    long lsb = (sub ? emin : t) - (P - 1);
    uint64_t Mx = m << (e - lsb);
    /* x, and its midpoints, on the 2^(lsb-2) grid */
    struct sn_big kx = { 0, 0, 0 }, khi = { 0, 0, 0 }, klo = { 0, 0, 0 }, one = { 0, 0, 0 };
    sn_set_u64(&kx, Mx);
    sn_shl(&kx, 2);
    od_big_copy(&khi, &kx);
    sn_add_small(&khi, 2);
    od_big_copy(&klo, &kx);
    int bottom = !sub && t > emin && Mx == (uint64_t)1 << (P - 1);
    sn_set_u64(&one, bottom ? 1 : 2);
    sn_sub(&klo, &one);
    sn_free(&one);
    /* about 25 digits of each: X is within one of t*log10(2) */
    long tl = t * 78913;
    long xe = tl >= 0 ? tl / 262144 : -((-tl + 262143) / 262144);
    long S = 24 - xe;
    int ix, ihi, ilo;
    char *nx = od_scaled(&kx, lsb - 2, S, &ix);
    char *nhi = od_scaled(&khi, lsb - 2, S, &ihi);
    char *nlo = od_scaled(&klo, lsb - 2, S, &ilo);
    sn_free(&kx);
    sn_free(&khi);
    sn_free(&klo);
    long nd = (long)strlen(nx);
    long X = nd - 1 - S;
    int even = !(Mx & 1);
    char *R = (char *)malloc((size_t)nd + 2);
    char *D = (char *)malloc((size_t)(nd + 4));
    if (!R || !D) su_error(1, 0, "memory exhausted");
    int p = sub ? 1 : dig;
    long Xp;
    for (;; p++) {
        memcpy(R, nx, (size_t)p);
        R[p] = 0;
        Xp = X;
        char rd = nx[p];
        int rest = ix;
        for (long i = p + 1; i < nd && !rest; i++) if (nx[i] != '0') rest = 1;
        int up = rd > '5' || (rd == '5' && (rest || ((R[p - 1] - '0') & 1)));
        if (up) {
            int i = p - 1;
            while (i >= 0 && R[i] == '9') R[i--] = '0';
            if (i >= 0) R[i]++;
            else {
                /* 99..9 -> 100..0: one digit more, kept to p */
                R[0] = '1';
                for (int j = 1; j < p; j++) R[j] = '0';
                Xp++;
            }
        }
        /* D * 10^S as an integer: R and Xp + S + 1 - p zeros */
        long zeros = Xp + S + 1 - p;
        memcpy(D, R, (size_t)p);
        for (long j = 0; j < zeros; j++) D[p + j] = '0';
        D[p + zeros] = 0;
        int ch = od_deccmp(D, nhi), cl = od_deccmp(D, nlo);
        int below_hi = ch < 0 || (ch == 0 && ihi);
        int on_hi = ch == 0 && !ihi;
        int above_lo = cl > 0;
        int on_lo = cl == 0 && !ilo;
        if (((below_hi || (on_hi && even)) && (above_lo || (on_lo && even))) || p >= decdig)
            break;
    }
    free(D);
    free(nx);
    free(nhi);
    free(nlo);
    /* %g: trailing zeros go, and the point with them */
    int keep = p;
    while (keep > 1 && R[keep - 1] == '0') keep--;
    if (neg) od_putc(out, '-');
    if (Xp < -4 || Xp >= p) {
        od_putc(out, R[0]);
        if (keep > 1) {
            od_putc(out, '.');
            od_put(out, R + 1, (size_t)(keep - 1));
        }
        char eb[32];
        long ax = Xp < 0 ? -Xp : Xp;
        int n = od_utoa(eb + 2, (uintmax_t)ax, 10, 2);
        eb[0] = 'e';
        eb[1] = Xp < 0 ? '-' : '+';
        od_put(out, eb, (size_t)n + 2);
    } else if (Xp >= 0) {
        od_put(out, R, (size_t)Xp + 1);
        if (keep > Xp + 1) {
            od_putc(out, '.');
            od_put(out, R + Xp + 1, (size_t)(keep - Xp - 1));
        }
    } else {
        od_puts(out, "0.");
        for (long j = 0; j < -Xp - 1; j++) od_putc(out, '0');
        od_put(out, R, (size_t)keep);
    }
    free(R);
}

/* the float at B (already in host order) as od prints it, right-aligned */
static void od_float(struct od_buf *out, const unsigned char *b, int fkind, int width) {
    struct od_buf t = { 0, 0, 0 };
    int neg, cls;           /* 0 zero, 1 finite, 2 inf, 3 nan */
    uint64_t m = 0;
    long e = 0;
    int P, dig, decdig;
    long emin;
    if (fkind == OD_F_LDOUBLE) {
        uint64_t mm = 0;
        for (int i = 7; i >= 0; i--) mm = mm << 8 | b[i];
        unsigned se = (unsigned)b[8] | (unsigned)b[9] << 8;
        unsigned ex = se & 0x7fff;
        neg = (se >> 15) & 1;
        if (ex == 0x7fff) cls = mm == 0x8000000000000000ULL ? 2 : 3;
        else if (ex && !(mm >> 63)) cls = 3;            /* an unnormal: x87 says NaN */
        else if (!mm) cls = 0;
        else { cls = 1; m = mm; e = (ex ? (long)ex : 1) - 16383 - 63; }
        P = 64; emin = -16382; dig = 18; decdig = 21;
    } else if (fkind == OD_F_DOUBLE) {
        uint64_t v = 0;
        for (int i = 7; i >= 0; i--) v = v << 8 | b[i];
        neg = (int)(v >> 63);
        unsigned ex = (unsigned)(v >> 52) & 0x7ff;
        uint64_t fr = v & 0xfffffffffffffULL;
        if (ex == 0x7ff) cls = fr ? 3 : 2;
        else if (!ex && !fr) cls = 0;
        else if (!ex) { cls = 1; m = fr; e = -1074; }
        else { cls = 1; m = fr | 1ULL << 52; e = (long)ex - 1075; }
        P = 53; emin = -1022; dig = 15; decdig = 17;
    } else {
        uint32_t v = 0;
        if (fkind == OD_F_FLOAT) v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        else if (fkind == OD_F_BF16) v = ((uint32_t)b[0] | (uint32_t)b[1] << 8) << 16;
        if (fkind == OD_F_HALF) {
            unsigned h = (unsigned)b[0] | (unsigned)b[1] << 8;
            unsigned ex = (h >> 10) & 0x1f, fr = h & 0x3ff;
            neg = (int)(h >> 15);
            if (ex == 0x1f) cls = fr ? 3 : 2;
            else if (!ex && !fr) cls = 0;
            else if (!ex) { cls = 1; m = fr; e = -24; }
            else { cls = 1; m = fr | 0x400; e = (long)ex - 25; }
        } else {
            neg = (int)(v >> 31);
            unsigned ex = (v >> 23) & 0xff, fr = v & 0x7fffff;
            if (ex == 0xff) cls = fr ? 3 : 2;
            else if (!ex && !fr) cls = 0;
            else if (!ex) { cls = 1; m = fr; e = -149; }
            else { cls = 1; m = fr | 0x800000; e = (long)ex - 150; }
        }
        /* half and bfloat16 print as the float they widen to */
        P = 24; emin = -126; dig = 6; decdig = 9;
    }
    if (cls == 0) od_puts(&t, neg ? "-0" : "0");
    else if (cls == 2) od_puts(&t, neg ? "-inf" : "inf");
    else if (cls == 3) od_puts(&t, neg ? "-nan" : "nan");
    else od_shortest(&t, neg, m, e, P, emin, dig, decdig);
    od_right(out, t.s, width);
    free(t.s);
}

/* ---- one field ----------------------------------------------------------- */

static const char *const od_names[33] = {
    "nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel",
    "bs", "ht", "nl", "vt", "ff", "cr", "so", "si",
    "dle", "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb",
    "can", "em", "sub", "esc", "fs", "gs", "rs", "us", "sp"
};

static int od_isprint(int c) { return c >= 0x20 && c < 0x7f; }

static int od_big_endian;

static void od_field(struct od_buf *out, const struct od_spec *s, const unsigned char *p) {
    unsigned char b[16];
    if (od_big_endian && s->size > 1) for (int i = 0; i < s->size; i++) b[i] = p[s->size - 1 - i];
    else memcpy(b, p, (size_t)s->size);
    char t[80];
    switch (s->kind) {
    case OD_NAMED: {
        int c = b[0] & 0x7f;
        if (c <= 0x20) od_right(out, od_names[c], 4);
        else if (c == 0x7f) od_right(out, "del", 4);
        else { t[0] = (char)c; t[1] = 0; od_right(out, t, 4); }
        return;
    }
    case OD_CHAR: {
        int c = b[0];
        const char *esc = c == 0 ? "\\0" : c == '\a' ? "\\a" : c == '\b' ? "\\b" : c == '\f' ? "\\f"
                        : c == '\n' ? "\\n" : c == '\r' ? "\\r" : c == '\t' ? "\\t" : c == '\v' ? "\\v" : 0;
        if (esc) od_right(out, esc, 4);
        else if (od_isprint(c)) { t[0] = (char)c; t[1] = 0; od_right(out, t, 4); }
        else { od_utoa(t, (uintmax_t)c, 8, 3); od_right(out, t, 4); }
        return;
    }
    case OD_FLT:
        od_float(out, b, s->fkind, s->width);
        return;
    default: break;
    }
    uintmax_t v = 0;
    for (int i = s->size - 1; i >= 0; i--) v = v << 8 | b[i];
    if (s->kind == OD_SDEC) {
        int bits = s->size * 8;
        int negv = (int)((v >> (bits - 1)) & 1);
        if (negv) {
            uintmax_t mag = bits < 64 ? ((uintmax_t)1 << bits) - v : (uintmax_t)0 - v;
            t[0] = '-';
            od_utoa(t + 1, mag, 10, 1);
        } else od_utoa(t, v, 10, 1);
        od_right(out, t, s->width);
    } else if (s->kind == OD_UDEC) {
        od_utoa(t, v, 10, 1);
        od_right(out, t, s->width);
    } else {
        od_putc(out, ' ');
        od_utoa(t, v, s->kind == OD_OCT ? 8 : 16, s->width - 1);
        od_puts(out, t);
    }
}

/* ---- input --------------------------------------------------------------- */

static char **od_files;
static int od_nfiles, od_fileidx;
static int od_fd = -1;
static const char *od_fname;
static int od_status;
static int od_limited;          /* -N given */
static uintmax_t od_left;       /* bytes -N still allows */
static unsigned char od_ibuf[65536];
static size_t od_ipos, od_ilen;
/* How much is taken from a pipe matters to whoever reads it next. GNU's
 * od takes exactly what it dumps, but -S reads through a 4096-byte
 * stdio buffer, its skip included (measured: `cat 10k | { od -S 3 -j 5000
 * -N 3; wc -c; }` leaves 1808). -S does the same here; a stdin that can
 * seek is put back where the reading stopped, as glibc's exit does. */
static int od_strmode;

/* a stdin that is left behind is put back where the reading stopped */
static void od_close_cur(void) {
    if (od_fd == 0 && od_ilen > od_ipos)
        lseek(0, -(off_t)(od_ilen - od_ipos), SEEK_CUR);
    if (od_fd > 0) close(od_fd);
    od_fd = -1;
    od_ipos = od_ilen = 0;
}

/* the next file that opens; 0 when there are no more */
static int od_open_next(void) {
    od_close_cur();
    while (od_fileidx < od_nfiles) {
        const char *name = od_files[od_fileidx++];
        int fd = 0;
        if (strcmp(name, "-")) {
            fd = su_open_read(name, 0);
            if (fd < 0) {
                su_error(0, errno, "%s", su_quotef(name));
                od_status = 1;
                continue;
            }
        }
        od_fd = fd;
        od_fname = name;
        return 1;
    }
    return 0;
}

/* refill from the current file (never past -N), moving on at its end;
 * 0 at the end of the input */
static int od_fill(void) {
    for (;;) {
        if (od_fd < 0) return 0;
        size_t want = sizeof od_ibuf;
        if (od_strmode) want = 4096;
        else if (od_limited && od_left < want) want = (size_t)od_left;
        if (want == 0) return 0;
        ssize_t n = read(od_fd, od_ibuf, want);
        if (n > 0) {
            od_ipos = 0;
            od_ilen = (size_t)n;
            if (od_limited) od_left -= (uintmax_t)n;
            return 1;
        }
        if (n < 0) {
            su_error(0, errno, "%s", su_quotef(od_fname));
            od_status = 1;
        }
        if (!od_open_next()) return 0;
    }
}

/* up to WANT bytes of the stream into BUF */
static size_t od_read(unsigned char *buf, size_t want) {
    size_t got = 0;
    while (got < want) {
        if (od_ipos == od_ilen && !od_fill()) break;
        size_t k = od_ilen - od_ipos;
        if (k > want - got) k = want - got;
        memcpy(buf + got, od_ibuf + od_ipos, k);
        od_ipos += k;
        got += k;
    }
    return got;
}

static int od_getc(void) {
    if (od_ipos == od_ilen && !od_fill()) return -1;
    return od_ibuf[od_ipos++];
}

/* -j: 0, or -1 when the input ended first */
static int od_skip(uintmax_t n) {
    while (n > 0) {
        if (od_fd < 0) return -1;
        struct stat st;
        if (fstat(od_fd, &st) == 0) {
            if (S_ISREG(st.st_mode) && st.st_size > (off_t)st.st_blksize) {
                off_t cur = lseek(od_fd, 0, SEEK_CUR);
                if (cur >= 0) {
                    uintmax_t avail = st.st_size > cur ? (uintmax_t)(st.st_size - cur) : 0;
                    if (avail <= n) {
                        n -= avail;
                        if (!od_open_next()) return n ? -1 : 0;
                        continue;
                    }
                    if (lseek(od_fd, (off_t)n, SEEK_CUR) >= 0) return 0;
                }
            } else if (!S_ISREG(st.st_mode) && n <= (uintmax_t)INTMAX_MAX &&
                       lseek(od_fd, (off_t)n, SEEK_CUR) >= 0) {
                return 0;
            }
        }
        /* read it: exactly, or through the -S buffer */
        unsigned char junk[4096];
        for (;;) {
            if (od_strmode) {
                if (od_ipos == od_ilen) {
                    ssize_t k = read(od_fd, od_ibuf, 4096);
                    if (k <= 0) {
                        if (k < 0) {
                            su_error(0, errno, "%s", su_quotef(od_fname));
                            od_status = 1;
                        }
                        break;
                    }
                    od_ipos = 0;
                    od_ilen = (size_t)k;
                }
                size_t take = od_ilen - od_ipos;
                if (take > n) take = (size_t)n;
                od_ipos += take;
                n -= take;
                if (!n) return 0;
                continue;
            }
            size_t want = n < sizeof junk ? (size_t)n : sizeof junk;
            ssize_t k = read(od_fd, junk, want);
            if (k > 0) {
                n -= (uintmax_t)k;
                if (!n) return 0;
                continue;
            }
            if (k < 0) {
                su_error(0, errno, "%s", su_quotef(od_fname));
                od_status = 1;
            }
            break;
        }
        if (!od_open_next()) return -1;
    }
    return 0;
}

/* ---- addresses ----------------------------------------------------------- */

static char od_radix = 'o';
static int od_have_label;
static uintmax_t od_label_off;      /* label - offset */

static int od_addr_width(void) {
    return od_radix == 'n' ? 0 : od_radix == 'x' ? 6 : 7;
}

static void od_addr(struct od_buf *b, uintmax_t a) {
    char t[72];
    if (od_radix != 'n') {
        od_utoa(t, a, od_radix == 'd' ? 10 : od_radix == 'x' ? 16 : 8, od_addr_width());
        od_puts(b, t);
    }
    if (od_have_label) {
        if (od_radix == 'n') {
            od_utoa(t, a, 8, 7);
            od_putc(b, '(');
        } else {
            od_utoa(t, a + od_label_off, od_radix == 'd' ? 10 : od_radix == 'x' ? 16 : 8, od_addr_width());
            od_puts(b, " (");
        }
        od_puts(b, t);
        od_putc(b, ')');
    }
}

static void od_flush(struct od_buf *b) {
    if (b->n) fwrite(b->s, 1, b->n, stdout);
    b->n = 0;
}

/* ---- the dump -------------------------------------------------------------- */

static void od_line(struct od_buf *o, uintmax_t addr, const unsigned char *p, size_t len,
                    size_t width, int block, int maxw) {
    for (int si = 0; si < od_nspecs; si++) {
        const struct od_spec *s = &od_specs[si];
        if (si == 0) od_addr(o, addr);
        else od_spaces(o, (size_t)od_addr_width());
        size_t start = o->n;
        int per = block / s->size;
        size_t nf = (len + (size_t)s->size - 1) / (size_t)s->size;
        unsigned char tmp[16];
        for (size_t i = 0; i < nf; i++) {
            od_spaces(o, (size_t)s->pads[i % (size_t)per]);
            const unsigned char *q = p + i * (size_t)s->size;
            if ((i + 1) * (size_t)s->size > len) {
                memset(tmp, 0, sizeof tmp);
                memcpy(tmp, q, len - i * (size_t)s->size);
                q = tmp;
            }
            od_field(o, s, q);
        }
        if (s->z) {
            size_t full = width / (size_t)block * (size_t)maxw;
            size_t used = o->n - start;
            if (used < full) od_spaces(o, full - used);
            od_puts(o, "  >");
            for (size_t i = 0; i < len; i++) od_putc(o, od_isprint(p[i]) ? (char)p[i] : '.');
            od_putc(o, '<');
        }
        od_putc(o, '\n');
    }
}

static void od_dump(uintmax_t start, size_t width, int abbrev) {
    int block = 1;
    for (int i = 0; i < od_nspecs; i++) if (od_specs[i].size > block) block = od_specs[i].size;
    int maxw = 0;
    for (int i = 0; i < od_nspecs; i++) {
        int w = block / od_specs[i].size * od_specs[i].width;
        if (w > maxw) maxw = w;
    }
    for (int i = 0; i < od_nspecs; i++) {
        struct od_spec *s = &od_specs[i];
        int n = block / s->size;
        int pad = maxw - n * s->width;
        s->pads = (int *)malloc((size_t)n * sizeof *s->pads);
        if (!s->pads) su_error(1, 0, "memory exhausted");
        int prev = pad;
        for (int k = 0; k < n; k++) {
            int next = pad * (n - 1 - k) / n;
            s->pads[k] = prev - next;
            prev = next;
        }
    }
    unsigned char *cur = (unsigned char *)malloc(width), *prev = (unsigned char *)malloc(width);
    if (!cur || !prev) su_error(1, 0, "memory exhausted");
    struct od_buf o = { 0, 0, 0 };
    uintmax_t addr = start;
    int have_prev = 0, dup = 0;
    for (;;) {
        size_t len = od_read(cur, width);
        if (!len) break;
        if (abbrev && have_prev && len == width && !memcmp(cur, prev, width)) {
            if (!dup) od_puts(&o, "*\n");
            dup = 1;
        } else {
            od_line(&o, addr, cur, len, width, block, maxw);
            dup = 0;
        }
        unsigned char *t = prev; prev = cur; cur = t;
        have_prev = 1;
        addr += len;
        od_flush(&o);
        if (len < width) break;
    }
    if (od_radix != 'n' || od_have_label) {
        od_addr(&o, addr);
        od_putc(&o, '\n');
    }
    od_flush(&o);
    free(o.s);
    free(cur);
    free(prev);
}

static void od_strings(uintmax_t address, uintmax_t string_min, int limited, uintmax_t end) {
    size_t cap = 256;
    char *buf = (char *)malloc(cap);
    if (!buf) su_error(1, 0, "memory exhausted");
    struct od_buf o = { 0, 0, 0 };
    for (;;) {
    tryline:
        if (limited && (end < string_min || end - string_min <= address)) break;
        size_t i = 0;
        while (!limited || address < end) {
            if (i + 1 >= cap) {
                cap *= 2;
                buf = (char *)realloc(buf, cap);
                if (!buf) su_error(1, 0, "memory exhausted");
            }
            int c = od_getc();
            address++;
            if (c < 0) goto done;
            buf[i] = (char)c;
            if (c == 0) break;
            if (!od_isprint(c)) goto tryline;
            i++;
        }
        if (i < string_min) goto tryline;
        od_addr(&o, address - i - 1);
        if (od_radix != 'n' || od_have_label) od_putc(&o, ' ');
        od_put(&o, buf, i);
        od_putc(&o, '\n');
        od_flush(&o);
    }
done:
    od_flush(&o);
    free(o.s);
    free(buf);
}

/* ---- main ---------------------------------------------------------------- */

static void od_usage(void) {
    printf("Usage: %s [OPTION]... [FILE]...\n"
           "  or:  %s [-abcdfilosx]... [FILE] [[+]OFFSET[.][b]]\n"
           "  or:  %s --traditional [OPTION]... [FILE] [[+]OFFSET[.][b] [+][LABEL][.][b]]\n"
           "\n"
           "Show the bytes of FILE (standard input when there is none, or for -) in\n"
           "forms that leave nothing ambiguous: octal two-byte words unless told\n"
           "otherwise. Several FILEs are read one after another as a single input.\n"
           "\n"
           "The second form applies when no option but the traditional letters is\n"
           "given, and the last operand starts with + or (of two operands) a digit:\n"
           "that operand is then where to start, as -j. In the third form LABEL is\n"
           "the address shown, in parentheses, for the first byte, counting up from\n"
           "there. OFFSET and LABEL are octal, or hexadecimal after 0x or 0X; a\n"
           "trailing b multiplies by 512.\n"
           "\n"
           "  -A, --address-radix=RADIX   addresses in d(ecimal), o(ctal), x (hex),\n"
           "                                or n (no addresses at all)\n"
           "      --endian={big|little}   read multi-byte values in that byte order\n"
           "  -j, --skip-bytes=BYTES      begin BYTES into the input\n"
           "  -N, --read-bytes=BYTES      show at most BYTES of the input\n"
           "  -S BYTES, --strings[=BYTES]  show only the NUL-ended runs of at least\n"
           "                                BYTES (3) printable characters\n"
           "  -t, --format=TYPE           how to show the data (may be repeated)\n"
           "  -v, --output-duplicates     show every line; no * for repeats\n"
           "  -w[BYTES], --width[=BYTES]  BYTES of input per line (32 when BYTES is\n"
           "                                left out; 16 without -w)\n"
           "      --traditional           take operands in the third form above\n"
           "      --help     show this help and exit\n"
           "      --version  show the version and exit\n"
           "\n"
           "The traditional letters add a TYPE each:\n"
           "  -a   -t a      -b   -t o1     -c   -t c      -d   -t u2\n"
           "  -f   -t fF     -i   -t dI     -l   -t dL     -o   -t o2\n"
           "  -s   -t d2     -x   -t x2\n"
           "\n"
           "TYPE is one or more of:\n"
           "  a          the name of each character, its top bit ignored\n"
           "  c          each character, or its backslash escape, or octal\n"
           "  d[SIZE]    signed decimal integers of SIZE bytes\n"
           "  f[SIZE]    floating point numbers of SIZE bytes\n"
           "  o[SIZE]    octal integers of SIZE bytes\n"
           "  u[SIZE]    unsigned decimal integers of SIZE bytes\n"
           "  x[SIZE]    hexadecimal integers of SIZE bytes\n"
           "SIZE is a byte count; for d o u x it may also be C, S, I or L (char,\n"
           "short, int, long), for f F, D or L (float, double, long double), H (IEEE\n"
           "half precision) or B (bfloat16). A z after any TYPE adds the line's\n"
           "printable characters at its end.\n"
           "\n"
           "BYTES may be hexadecimal after 0x, and may end in a multiplier: b (512),\n"
           "K (1024), KB (1000), M (1024*1024), MB (1000*1000), and on through G, T,\n"
           "P, E, Z, Y, R and Q; KiB is K, MiB is M, and so on.\n",
           su_name, su_name, su_name);
}

#define OD_ENDIAN 0x20000
#define OD_TRAD   0x20001
#define OD_STRL   0x20002
#define OD_WIDTHL 0x20003
#define OD_SKIPL  0x20004
#define OD_READL  0x20005

static int do_od(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "address-radix", SU_REQ_ARG, 0, 'A' },
        { "endian", SU_REQ_ARG, 0, OD_ENDIAN },
        { "format", SU_REQ_ARG, 0, 't' },
        { "output-duplicates", SU_NO_ARG, 0, 'v' },
        { "read-bytes", SU_REQ_ARG, 0, OD_READL },
        { "skip-bytes", SU_REQ_ARG, 0, OD_SKIPL },
        { "strings", SU_OPT_ARG, 0, OD_STRL },
        { "traditional", SU_NO_ARG, 0, OD_TRAD },
        { "width", SU_OPT_ARG, 0, OD_WIDTHL },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    int modern = 0, trad = 0, abbrev = 1, type_err = 0;
    int have_skip = 0, strings = 0;
    uintmax_t skip = 0, limit = 0, string_min = 3;
    intmax_t width = 16;
    int c;
    while ((c = su_getopt_long(argc, argv, "A:aBbcDdeFfHhIij:LlN:OoS:st:vw::Xx", longs, 0)) != -1) {
        uintmax_t v;
        int rc;
        switch (c) {
        case 'A': {
            modern = 1;
            char r = su_optarg[0];
            if (r != 'd' && r != 'o' && r != 'x' && r != 'n') {
                fflush(stdout);
                fprintf(stderr, "%s: invalid output address radix '", su_short);
                fputc(r, stderr);
                fputs("'; it must be one character from [doxn]\n", stderr);
                exit(1);
            }
            od_radix = r;
            break;
        }
        case 'j': case OD_SKIPL:
            modern = 1;
            rc = od_parse_size(su_optarg, &v);
            if (rc) od_num_fail(rc, c == 'j' ? "-j" : "--skip-bytes", su_optarg);
            skip = v;
            have_skip = 1;
            break;
        case 'N': case OD_READL:
            modern = 1;
            rc = od_parse_size(su_optarg, &v);
            if (rc) od_num_fail(rc, c == 'N' ? "-N" : "--read-bytes", su_optarg);
            limit = v;
            od_limited = 1;
            break;
        case 'S': case OD_STRL:
            modern = 1;
            strings = 1;
            if (su_optarg) {
                rc = od_parse_size(su_optarg, &v);
                if (rc) od_num_fail(rc, c == 'S' ? "-S" : "--strings", su_optarg);
                string_min = v;
                /* GNU sizes its buffer from it at once: past INTMAX_MAX that fails */
                if (v > (uintmax_t)INTMAX_MAX) su_error(1, 0, "%s is too large", su_quote(su_optarg));
            } else string_min = 3;
            break;
        case 't':
            modern = 1;
            if (od_decode_types(su_optarg) < 0) type_err = 1;
            break;
        case 'v':
            modern = 1;
            abbrev = 0;
            break;
        case 'w': case OD_WIDTHL:
            modern = 1;
            if (su_optarg) {
                width = od_parse_width(su_optarg, c == 'w' ? "-w" : "--width");
                /* GNU's od asserts here: SIGABRT, and not a byte written */
                if (width <= 0) abort();
            } else width = 32;
            break;
        case 'a': od_decode_types("a"); break;
        case 'b': od_decode_types("o1"); break;
        case 'c': od_decode_types("c"); break;
        case 'd': od_decode_types("u2"); break;
        case 'f': od_decode_types("fF"); break;
        case 'i': od_decode_types("dI"); break;
        case 'l': case 'I': case 'L': od_decode_types("dL"); break;
        case 'o': case 'B': od_decode_types("o2"); break;
        case 's': od_decode_types("d2"); break;
        case 'x': case 'h': od_decode_types("x2"); break;
        case 'D': od_decode_types("u4"); break;
        case 'e': case 'F': od_decode_types("fD"); break;
        case 'H': case 'X': od_decode_types("x4"); break;
        case 'O': od_decode_types("o4"); break;
        case OD_ENDIAN: {
            size_t n = strlen(su_optarg);
            int little = !strncmp("little", su_optarg, n);
            int big = !strncmp("big", su_optarg, n);
            if (little == big) {
                fflush(stdout);
                fprintf(stderr, "%s: %s argument %s for '--endian'\n"
                        "Valid arguments are:\n  - 'little'\n  - 'big'\n",
                        su_short, little ? "ambiguous" : "invalid", su_quote(su_optarg));
                su_try_help(1);
            }
            od_big_endian = big;
            break;
        }
        case OD_TRAD: trad = 1; break;
        case SU_HELP_OPT: od_usage(); return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (type_err) return 1;
    if (strings && od_nspecs) su_error(1, 0, "no type may be specified when dumping strings");

    /* the operands: files, and perhaps an offset and a label */
    char **ops = argv + su_optind;
    int nops = argc - su_optind;
    uintmax_t off, lab;
    int got_off = 0;
    if (trad) {
        int bad = 0;
        if (nops == 1) {
            if (od_parse_offset(ops[0], &off)) { got_off = 1; nops = 0; }
        } else if (nops == 2) {
            if (od_parse_offset(ops[0], &off) && od_parse_offset(ops[1], &lab)) {
                got_off = 1;
                od_have_label = 1;
                nops = 0;
            } else if (od_parse_offset(ops[1], &off)) {
                got_off = 1;
                nops = 1;
            } else bad = 1;
        } else if (nops == 3) {
            if (od_parse_offset(ops[1], &off) && od_parse_offset(ops[2], &lab)) {
                got_off = 1;
                od_have_label = 1;
                nops = 1;
            } else bad = 1;
        } else if (nops > 3) bad = 1;
        if (bad) {
            su_error(0, 0, "extra operand %s", su_quote(ops[1]));
            su_error(0, 0, "compatibility mode supports at most one file");
            su_try_help(1);
        }
    } else if (!modern) {
        if (nops == 1 && ops[0][0] == '+' && od_parse_offset(ops[0], &off)) {
            got_off = 1;
            nops = 0;
        } else if (nops == 2 && (ops[1][0] == '+' || isdigit((unsigned char)ops[1][0])) &&
                   od_parse_offset(ops[1], &off)) {
            got_off = 1;
            nops = 1;
        }
    }
    if (got_off) {
        skip = off;
        have_skip = 1;
        if (od_have_label) od_label_off = lab - off;
    }
    (void)have_skip;
    static char *dash[] = { (char *)"-" };
    od_files = nops ? ops : dash;
    od_nfiles = nops ? nops : 1;

    if (!od_nspecs) od_decode_types("o2");

    if (od_limited && skip > UINTMAX_MAX - limit) su_error(1, 0, "skip-bytes + read-bytes is too large");
    od_strmode = strings;
    if (!od_open_next()) return 1;
    if (od_skip(skip) < 0) su_error(1, 0, "cannot skip past end of combined input");
    if (od_limited) od_left = limit;

    int block = 1;
    for (int i = 0; i < od_nspecs; i++) if (od_specs[i].size > block) block = od_specs[i].size;
    if (width % block) {
        char nb[32];
        od_utoa(nb, (uintmax_t)width, 10, 1);
        su_error(0, 0, "warning: invalid width %s; using %d instead", nb, block);
        width = block;
    }

    if (strings) {
        /* GNU allocates the whole minimum at once; past any address space
         * that is "memory exhausted" */
        if (string_min >= (uintmax_t)1 << 40) {
            void *probe = string_min + 1 < (uintmax_t)SIZE_MAX ? malloc((size_t)(string_min + 1)) : 0;
            if (!probe) su_error(1, 0, "memory exhausted");
            free(probe);
        }
        od_strings(skip, string_min, od_limited, skip + limit);
    } else {
        if ((uintmax_t)width > SIZE_MAX / 2) su_error(1, 0, "memory exhausted");
        od_dump(skip, (size_t)width, abbrev);
    }
    od_close_cur();
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return od_status;
}
