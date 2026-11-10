/* su_num.c -- exact long double: read one as strtold does, write one as
 * glibc's printf does (%f %e %g %a and their capitals, with flags, width
 * and precision). Included by main.c; every name starts su_ or sn_.
 *
 * WHY NOT libtoby's strtod AND printf: GNU seq works in long double and
 * prints through glibc, which converts EXACTLY -- `seq 1e400 1e400` prints
 * the 401 digits of the binary value nearest 1e400 -- and rounds a tie to
 * even. libtoby's printf takes doubles, rounds through double arithmetic
 * and stops at 60 digits; its strtod is double too. So the conversions
 * here go through big integers: a long double is m * 2^e with a 64-bit m,
 * which is m * 5^-e / 10^-e exactly when e < 0 -- a finite decimal, cut
 * and rounded half-even by string. Reading is the reverse: the decimal
 * as a big integer times a power of ten, divided or multiplied out, and
 * rounded once to 64 bits, half-even. x87 long double on both the guest
 * and the Linux build. */

/* ---- big integers: little-endian 32-bit limbs ------------------------------ */

struct sn_big {
    uint32_t *d;
    size_t n, cap;
};

static void sn_fail(void) {
    fputs("memory exhausted\n", stderr);
    exit(1);
}

static void sn_reserve(struct sn_big *b, size_t n) {
    if (n <= b->cap) return;
    size_t c = b->cap ? b->cap : 8;
    while (c < n) c *= 2;
    uint32_t *nd = (uint32_t *)realloc(b->d, c * sizeof *nd);
    if (!nd) sn_fail();
    b->d = nd;
    b->cap = c;
}

static void sn_trim(struct sn_big *b) {
    while (b->n && b->d[b->n - 1] == 0) b->n--;
}

static void sn_free(struct sn_big *b) {
    free(b->d);
    b->d = 0;
    b->n = b->cap = 0;
}

static void sn_set_u64(struct sn_big *b, uint64_t v) {
    sn_reserve(b, 2);
    b->d[0] = (uint32_t)v;
    b->d[1] = (uint32_t)(v >> 32);
    b->n = 2;
    sn_trim(b);
}

static int sn_is_zero(const struct sn_big *b) { return b->n == 0; }

static void sn_mul_small(struct sn_big *b, uint32_t m) {
    uint64_t carry = 0;
    for (size_t i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->d[i] * m + carry;
        b->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        sn_reserve(b, b->n + 1);
        b->d[b->n++] = (uint32_t)carry;
    }
    sn_trim(b);
}

static void sn_add_small(struct sn_big *b, uint32_t a) {
    uint64_t carry = a;
    for (size_t i = 0; i < b->n && carry; i++) {
        uint64_t t = (uint64_t)b->d[i] + carry;
        b->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        sn_reserve(b, b->n + 1);
        b->d[b->n++] = (uint32_t)carry;
    }
}

static size_t sn_bitlen(const struct sn_big *b) {
    if (!b->n) return 0;
    uint32_t top = b->d[b->n - 1];
    size_t bits = (b->n - 1) * 32;
    while (top) { bits++; top >>= 1; }
    return bits;
}

static int sn_bit(const struct sn_big *b, size_t i) {
    size_t w = i / 32;
    if (w >= b->n) return 0;
    return (b->d[w] >> (i % 32)) & 1;
}

static void sn_shl(struct sn_big *b, size_t bits) {
    if (!b->n || !bits) return;
    size_t w = bits / 32, s = bits % 32;
    sn_reserve(b, b->n + w + 1);
    b->d[b->n + w] = 0;
    for (size_t i = b->n; i-- > 0;) {
        uint64_t v = (uint64_t)b->d[i] << s;
        b->d[i + w + 1] |= (uint32_t)(v >> 32);
        b->d[i + w] = (uint32_t)v;
    }
    for (size_t i = 0; i < w; i++) b->d[i] = 0;
    b->n += w + 1;
    sn_trim(b);
}

/* B >>= BITS; whether any 1 bit was shifted out */
static int sn_shr(struct sn_big *b, size_t bits) {
    if (!bits || !b->n) return 0;
    int sticky = 0;
    for (size_t i = 0; i < bits && i < sn_bitlen(b); i++)
        if (sn_bit(b, i)) { sticky = 1; break; }
    size_t w = bits / 32, s = bits % 32;
    if (w >= b->n) { b->n = 0; return sticky; }
    for (size_t i = 0; i + w < b->n; i++) {
        uint64_t lo = b->d[i + w] >> s;
        uint64_t hi = (s && i + w + 1 < b->n) ? (uint64_t)b->d[i + w + 1] << (32 - s) : 0;
        b->d[i] = (uint32_t)(lo | hi);
    }
    b->n -= w;
    sn_trim(b);
    return sticky;
}

static int sn_cmp(const struct sn_big *a, const struct sn_big *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (size_t i = a->n; i-- > 0;)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

/* A -= B, A >= B */
static void sn_sub(struct sn_big *a, const struct sn_big *b) {
    int64_t borrow = 0;
    for (size_t i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - (i < b->n ? (int64_t)b->d[i] : 0) - borrow;
        borrow = t < 0;
        a->d[i] = (uint32_t)(t + (borrow ? ((int64_t)1 << 32) : 0));
    }
    sn_trim(a);
}

/* Q = A / B, R = A % B (B nonzero), bit by bit */
static void sn_divmod(const struct sn_big *a, const struct sn_big *b,
                      struct sn_big *q, struct sn_big *r) {
    size_t L = sn_bitlen(a);
    q->n = 0;
    r->n = 0;
    sn_reserve(q, a->n + 1);
    memset(q->d, 0, (a->n + 1) * sizeof *q->d);
    for (size_t i = L; i-- > 0;) {
        sn_shl(r, 1);
        if (sn_bit(a, i)) {
            if (!r->n) { sn_reserve(r, 1); r->d[0] = 0; r->n = 1; }
            r->d[0] |= 1;
        }
        if (sn_cmp(r, b) >= 0) {
            sn_sub(r, b);
            q->d[i / 32] |= 1u << (i % 32);
            if (q->n < i / 32 + 1) q->n = i / 32 + 1;
        }
    }
    sn_trim(q);
}

static uint32_t sn_divmod_small(struct sn_big *b, uint32_t d) {
    uint64_t rem = 0;
    for (size_t i = b->n; i-- > 0;) {
        uint64_t cur = (rem << 32) | b->d[i];
        b->d[i] = (uint32_t)(cur / d);
        rem = cur % d;
    }
    sn_trim(b);
    return (uint32_t)rem;
}

/* B in decimal, a malloc'd string ("0" for zero); B is consumed */
static char *sn_to_dec(struct sn_big *b) {
    size_t cap = b->n * 10 + 16, n = 0;
    char *s = (char *)malloc(cap);
    if (!s) sn_fail();
    if (!b->n) { strcpy(s, "0"); return s; }
    while (b->n) {
        uint32_t chunk = sn_divmod_small(b, 1000000000u);
        for (int k = 0; k < 9; k++) {
            s[n++] = (char)('0' + chunk % 10);
            chunk /= 10;
            if (!b->n && !chunk) break;
        }
    }
    while (n > 1 && s[n - 1] == '0') n--;
    for (size_t i = 0; i < n / 2; i++) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; }
    s[n] = 0;
    return s;
}

static void sn_pow_mul(struct sn_big *b, uint32_t base, long k) {
    while (k >= 9 && base == 10) { sn_mul_small(b, 1000000000u); k -= 9; }
    while (k >= 13 && base == 5) { sn_mul_small(b, 1220703125u); k -= 13; }
    while (k-- > 0) sn_mul_small(b, base);
}

/* ---- the x87 long double --------------------------------------------------- */

union sn_ld {
    long double v;
    struct { uint64_t m; uint16_t se; } p;
};

/* SIGN * M * 2^E, M 64 bits with its top bit set, as a long double:
 * infinity past the largest, a denormal (rounded again, half-even) below
 * the smallest normal. */
static long double sn_make(int sign, uint64_t m, long e) {
    union sn_ld u;
    memset(&u, 0, sizeof u);
    if (!m) { u.p.se = sign ? 0x8000 : 0; return u.v; }
    long field = e + 63 + 16383;
    if (field >= 0x7fff) {
        u.p.m = 0x8000000000000000ULL;
        u.p.se = (uint16_t)(0x7fff | (sign ? 0x8000 : 0));
        return u.v;
    }
    if (field <= 0) {
        long sh = 1 - field;
        if (sh > 64) { u.p.se = sign ? 0x8000 : 0; return u.v; }
        uint64_t rb = sh <= 64 ? (m >> (sh - 1)) & 1 : 0;
        uint64_t sticky = sh >= 2 ? (m & ((sh - 1 >= 64) ? ~0ULL : ((1ULL << (sh - 1)) - 1))) : 0;
        uint64_t mm = sh >= 64 ? 0 : m >> sh;
        if (rb && (sticky || (mm & 1))) mm++;
        u.p.m = mm;
        u.p.se = (uint16_t)((mm >> 63 ? 1 : 0) | (sign ? 0x8000 : 0));
        return u.v;
    }
    u.p.m = m;
    u.p.se = (uint16_t)(field | (sign ? 0x8000 : 0));
    return u.v;
}

/* Set by su_strtold when what it read is out of long double's range, as
 * strtold's ERANGE: past the largest (infinity), or below the smallest
 * normal and not exact there -- a denormal that lost bits, or zero.
 * Tininess is judged after rounding, as x86 does: a value just below the
 * smallest normal that 64 bits would round up to it is not tiny. GNU
 * printf reports it; the other callers read the value alone. */
static int sn_range;

/* N (consumed) * 2^E, rounded half-even ONCE to the bits a long double
 * has there: 64, or fewer below the smallest normal, where the last place
 * is 2^-16445 (STICKY: bits already lost below N were not all zero) */
static long double sn_round_big(int sign, struct sn_big *n, long e, int sticky) {
    if (sn_is_zero(n)) return sn_make(sign, 0, 0);
    long L = (long)sn_bitlen(n);
    long top = L - 1 + e;                  /* the place of the leading bit */
    if (top > 16383) { sn_range = 1; return sn_make(sign, 0x8000000000000000ULL, 20000); }
    long lsb = top - 63;
    if (lsb < -16445) lsb = -16445;
    long sh = lsb - e;                     /* bits of N below the last place */
    int tiny = top < -16382;
    if (tiny && top == -16383 && sh >= 2) {
        /* 64 bits from the leading one, all ones, and a one after them:
         * rounding to 64 bits would carry up to the smallest normal */
        int ones = 1;
        for (long i = sh - 1; i < L && ones; i++) ones = sn_bit(n, (size_t)i);
        if (ones && sn_bit(n, (size_t)(sh - 2))) tiny = 0;
    }
    uint64_t m = 0;
    int inexact = sticky;
    if (sh <= 0) {
        for (size_t i = n->n; i-- > 0;) m = (m << 32) | n->d[i];
        m <<= -sh;
    } else {
        int rb = sh - 1 < L ? sn_bit(n, (size_t)(sh - 1)) : 0;
        int below = sticky;
        for (long i = 0; i < sh - 1 && i < L && !below; i++) if (sn_bit(n, (size_t)i)) below = 1;
        inexact = rb || below;
        sn_shr(n, (size_t)sh);
        for (size_t i = n->n; i-- > 0;) m = (m << 32) | n->d[i];
        if (rb && (below || (m & 1))) {
            m++;
            if (m == 0) { m = 0x8000000000000000ULL; lsb++; }
        }
    }
    if (tiny && inexact) sn_range = 1;
    union sn_ld u;
    memset(&u, 0, sizeof u);
    if (lsb + 63 > 16383) { sn_range = 1; return sn_make(sign, 0x8000000000000000ULL, 20000); }
    u.p.m = m;
    /* a denormal (top bit clear) keeps the zero exponent field */
    u.p.se = (uint16_t)(((m >> 63) ? lsb + 63 + 16383 : 0) | (sign ? 0x8000 : 0));
    return u.v;
}

/* strtold: blanks, a sign, then inf/infinity/nan (any case), a hex float
 * (0x..., with a p exponent), or a decimal with an e exponent. *END after
 * what was read (S itself when nothing was). */
static long double su_strtold(const char *s, char **end) {
    const char *p = s;
    sn_range = 0;
    while (isspace((unsigned char)*p)) p++;
    int sign = 0;
    if (*p == '+' || *p == '-') { sign = *p == '-'; p++; }
    if (strncasecmp(p, "inf", 3) == 0) {
        p += strncasecmp(p, "infinity", 8) == 0 ? 8 : 3;
        if (end) *end = (char *)p;
        union sn_ld u;
        memset(&u, 0, sizeof u);
        u.p.m = 0x8000000000000000ULL;
        u.p.se = (uint16_t)(0x7fff | (sign ? 0x8000 : 0));
        return u.v;
    }
    if (strncasecmp(p, "nan", 3) == 0) {
        p += 3;
        if (*p == '(') {
            const char *q = p + 1;
            while (isalnum((unsigned char)*q) || *q == '_') q++;
            if (*q == ')') p = q + 1;
        }
        if (end) *end = (char *)p;
        union sn_ld u;
        memset(&u, 0, sizeof u);
        u.p.m = 0xC000000000000000ULL;
        u.p.se = (uint16_t)(0x7fff | (sign ? 0x8000 : 0));
        return u.v;
    }
    struct sn_big n = { 0, 0, 0 };
    int digits = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        (isxdigit((unsigned char)p[2]) || (p[2] == '.' && isxdigit((unsigned char)p[3])))) {
        const char *q = p + 2;
        long e2 = 0, sig = 0;
        int lost = 0;
        /* 40 significant digits are 160 bits; past them a digit only says
         * whether the rest was zero (the rounding's sticky bit) */
        for (; isxdigit((unsigned char)*q); q++, digits++) {
            if (!sig && *q == '0') continue;
            if (sig < 40) {
                sn_mul_small(&n, 16);
                sn_add_small(&n, (uint32_t)(isdigit((unsigned char)*q) ? *q - '0' : (tolower(*q) - 'a' + 10)));
                sig++;
            } else { e2 += 4; if (*q != '0') lost = 1; }
        }
        if (*q == '.') {
            for (q++; isxdigit((unsigned char)*q); q++, digits++) {
                if (!sig && *q == '0') { e2 -= 4; continue; }
                if (sig < 40) {
                    sn_mul_small(&n, 16);
                    sn_add_small(&n, (uint32_t)(isdigit((unsigned char)*q) ? *q - '0' : (tolower(*q) - 'a' + 10)));
                    sig++;
                    e2 -= 4;
                } else if (*q != '0') lost = 1;
            }
        }
        if ((*q == 'p' || *q == 'P') &&
            (isdigit((unsigned char)q[1]) || ((q[1] == '+' || q[1] == '-') && isdigit((unsigned char)q[2])))) {
            const char *r = q + 1;
            int es = 0;
            if (*r == '+' || *r == '-') { es = *r == '-'; r++; }
            long ev = 0;
            for (; isdigit((unsigned char)*r); r++) if (ev < 100000) ev = ev * 10 + (*r - '0');
            e2 += es ? -ev : ev;
            q = r;
        }
        if (end) *end = (char *)q;
        long double v = sn_round_big(sign, &n, e2, lost);
        sn_free(&n);
        return v;
    }
    /* The significant digits as an integer, to SN_KEEP of them: past that
     * a digit only says whether the rest was zero, kept as one more digit
     * 1 -- which sits strictly between the cut value and the next one up,
     * and so rounds as the whole string would (a tie between two long
     * doubles has fewer than 11,500 significant digits). */
    enum { SN_KEEP = 12000 };
    long e10 = 0, sig = 0;
    int lost = 0;
    const char *q = p;
    for (; isdigit((unsigned char)*q); q++, digits++) {
        if (!sig && *q == '0') continue;
        if (sig < SN_KEEP) { sn_mul_small(&n, 10); sn_add_small(&n, (uint32_t)(*q - '0')); sig++; }
        else { e10++; if (*q != '0') lost = 1; }
    }
    if (*q == '.') {
        for (q++; isdigit((unsigned char)*q); q++, digits++) {
            if (!sig && *q == '0') { e10--; continue; }
            if (sig < SN_KEEP) { sn_mul_small(&n, 10); sn_add_small(&n, (uint32_t)(*q - '0')); sig++; e10--; }
            else if (*q != '0') lost = 1;
        }
    }
    if (lost) { sn_mul_small(&n, 10); sn_add_small(&n, 1); sig++; e10--; }
    if (!digits) {
        if (end) *end = (char *)s;
        sn_free(&n);
        return 0;
    }
    if ((*q == 'e' || *q == 'E') &&
        (isdigit((unsigned char)q[1]) || ((q[1] == '+' || q[1] == '-') && isdigit((unsigned char)q[2])))) {
        const char *r = q + 1;
        int es = 0;
        if (*r == '+' || *r == '-') { es = *r == '-'; r++; }
        long ev = 0;
        for (; isdigit((unsigned char)*r); r++) if (ev < 1000000) ev = ev * 10 + (*r - '0');
        e10 += es ? -ev : ev;
        q = r;
    }
    if (end) *end = (char *)q;
    long double v;
    /* the value is below 10^(SIG + E10) and at least a tenth of that */
    if (sn_is_zero(&n)) {
        v = sn_make(sign, 0, 0);
    } else if (sig + e10 > 4933) {
        sn_range = 1;
        v = sn_make(sign, 0x8000000000000000ULL, 20000);
    } else if (sig + e10 < -4951) {
        sn_range = 1;
        v = sn_make(sign, 0, 0);
    } else if (e10 >= 0) {
        sn_pow_mul(&n, 10, e10);
        v = sn_round_big(sign, &n, 0, 0);
    } else {
        struct sn_big den = { 0, 0, 0 }, qt = { 0, 0, 0 }, rm = { 0, 0, 0 };
        sn_set_u64(&den, 1);
        sn_pow_mul(&den, 10, -e10);
        long s2 = (long)sn_bitlen(&den) - (long)sn_bitlen(&n) + 66;
        if (s2 < 0) s2 = 0;
        sn_shl(&n, (size_t)s2);
        sn_divmod(&n, &den, &qt, &rm);
        v = sn_round_big(sign, &qt, -s2, !sn_is_zero(&rm));
        sn_free(&den);
        sn_free(&qt);
        sn_free(&rm);
    }
    sn_free(&n);
    return v;
}

/* ---- printing -------------------------------------------------------------- */

struct su_fmtspec {
    int left, plus, space, alt, zero;
    int width;              /* -1 none */
    int prec;               /* -1 none */
    char conv;              /* a A e E f F g G */
};

/* the decimal digits of V's magnitude, and K: V = DIGITS * 10^-K */
static char *sn_exact(long double v, long *k) {
    union sn_ld u;
    u.v = v;
    uint64_t m = u.p.m;
    long field = u.p.se & 0x7fff;
    long e = field ? field - 16383 - 63 : 1 - 16383 - 63;
    struct sn_big n = { 0, 0, 0 };
    sn_set_u64(&n, m);
    if (e >= 0) {
        sn_shl(&n, (size_t)e);
        *k = 0;
    } else {
        sn_pow_mul(&n, 5, -e);
        *k = -e;
    }
    char *s = sn_to_dec(&n);
    sn_free(&n);
    return s;
}

/* DIGITS (malloc'd) rounded half-even to its first KEEP digits; *CARRY
 * set when that made one more digit ("999" -> "1000") */
static char *sn_round_digits(char *dig, size_t keep, int *carry) {
    size_t n = strlen(dig);
    *carry = 0;
    if (keep >= n) return dig;
    int up;
    char d = dig[keep];
    if (d > '5') up = 1;
    else if (d < '5') up = 0;
    else {
        int rest = 0;
        for (size_t i = keep + 1; i < n; i++) if (dig[i] != '0') { rest = 1; break; }
        up = rest || (keep > 0 && ((dig[keep - 1] - '0') & 1));
    }
    dig[keep] = 0;
    if (up) {
        size_t i = keep;
        while (i > 0) {
            i--;
            if (dig[i] == '9') { dig[i] = '0'; continue; }
            dig[i]++;
            return dig;
        }
        /* all nines, or nothing kept */
        char *nd = (char *)malloc(keep + 2);
        if (!nd) sn_fail();
        nd[0] = '1';
        memcpy(nd + 1, dig, keep);
        nd[keep + 1] = 0;
        free(dig);
        *carry = 1;
        return nd;
    }
    return dig;
}

/* string builder */
struct sn_sb { char *s; size_t n, cap; };
static void sn_put(struct sn_sb *b, const char *t, size_t len) {
    if (b->n + len + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 64;
        while (c < b->n + len + 1) c *= 2;
        char *ns = (char *)realloc(b->s, c);
        if (!ns) sn_fail();
        b->s = ns;
        b->cap = c;
    }
    memcpy(b->s + b->n, t, len);
    b->n += len;
    b->s[b->n] = 0;
}
static void sn_putc(struct sn_sb *b, char c) { sn_put(b, &c, 1); }
static void sn_puts(struct sn_sb *b, const char *t) { sn_put(b, t, strlen(t)); }

/* fixed notation of |V| with PREC places (DIG/K: sn_exact's) */
static void sn_fixed(struct sn_sb *out, const char *dig0, long k, int prec, int alt) {
    char *dig = strdup(dig0);
    if (!dig) sn_fail();
    long n = (long)strlen(dig);
    /* the integer T = round(V * 10^PREC) as digits */
    if (k <= prec) {
        char *t = (char *)malloc((size_t)(n + (prec - k) + 1));
        if (!t) sn_fail();
        memcpy(t, dig, (size_t)n);
        memset(t + n, '0', (size_t)(prec - k));
        t[n + (prec - k)] = 0;
        free(dig);
        dig = t;
    } else {
        long cut = k - prec;            /* digits to drop from the right */
        long keep = n - cut;
        if (keep < 0) {
            /* every digit is below the last place: 0, or 1 if it rounds up */
            char *pad = (char *)malloc((size_t)(-keep + n + 2));
            if (!pad) sn_fail();
            memset(pad, '0', (size_t)(-keep));
            memcpy(pad + (-keep), dig, (size_t)n + 1);
            free(dig);
            dig = pad;
            n = (long)strlen(dig);
            keep = 0;
        }
        int carry;
        dig = sn_round_digits(dig, (size_t)keep, &carry);
        if (!*dig) { free(dig); dig = strdup("0"); if (!dig) sn_fail(); }
    }
    n = (long)strlen(dig);
    /* at least PREC + 1 digits, so there is an integer digit */
    if (n < prec + 1) {
        char *t = (char *)malloc((size_t)prec + 2);
        if (!t) sn_fail();
        memset(t, '0', (size_t)(prec + 1 - n));
        memcpy(t + (prec + 1 - n), dig, (size_t)n + 1);
        free(dig);
        dig = t;
        n = prec + 1;
    }
    /* strip leading zeros of the integer part, keeping one */
    long ip = n - prec, lead = 0;
    while (lead < ip - 1 && dig[lead] == '0') lead++;
    sn_put(out, dig + lead, (size_t)(ip - lead));
    if (prec > 0 || alt) sn_putc(out, '.');
    sn_put(out, dig + ip, (size_t)prec);
    free(dig);
}

/* exponent notation of |V|: PREC digits after the first; returns the
 * decimal exponent used */
static long sn_expo_digits(char **out_dig, const char *dig0, long k, int prec) {
    /* first significant digit */
    const char *p = dig0;
    while (*p == '0' && p[1]) p++;
    long n = (long)strlen(p);
    long X = (n - 1) - k;
    if (p[0] == '0') X = 0;           /* zero */
    char *dig = strdup(p);
    if (!dig) sn_fail();
    int carry;
    dig = sn_round_digits(dig, (size_t)prec + 1, &carry);
    if (carry) { X++; dig[prec + 1] = 0; }
    size_t dl = strlen(dig);
    if ((long)dl < prec + 1) {
        char *t = (char *)malloc((size_t)prec + 2);
        if (!t) sn_fail();
        memcpy(t, dig, dl);
        memset(t + dl, '0', (size_t)(prec + 1) - dl);
        t[prec + 1] = 0;
        free(dig);
        dig = t;
    }
    *out_dig = dig;
    return X;
}

static void sn_expo(struct sn_sb *out, const char *dig0, long k, int prec, int alt,
                    char e, int strip) {
    char *dig;
    long X = sn_expo_digits(&dig, dig0, k, prec);
    long dl = prec + 1;
    if (strip) while (dl > 1 && dig[dl - 1] == '0') dl--;
    sn_putc(out, dig[0]);
    if (dl > 1 || alt) sn_putc(out, '.');
    sn_put(out, dig + 1, (size_t)(dl - 1));
    free(dig);
    char eb[32];
    snprintf(eb, sizeof eb, "%c%c%02ld", e, X < 0 ? '-' : '+', X < 0 ? -X : X);
    sn_puts(out, eb);
}

/* %a: glibc's long double form -- the top four bits of the 64-bit
 * mantissa as the digit before the point (8 to f for a normal number) */
static void sn_hex(struct sn_sb *out, long double v, int prec, int alt, int upper) {
    union sn_ld u;
    u.v = v;
    uint64_t m = u.p.m;
    long field = u.p.se & 0x7fff;
    const char *hx = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    sn_puts(out, upper ? "0X" : "0x");
    if (m == 0) {
        sn_putc(out, '0');
        if (prec > 0 || alt) sn_putc(out, '.');
        for (int i = 0; i < prec; i++) sn_putc(out, '0');
        sn_puts(out, upper ? "P+0" : "p+0");
        return;
    }
    long e = (field ? field - 16383 : 1 - 16383) - 3;
    /* 16 nibbles; the first is the integer digit */
    int nib[16];
    for (int i = 0; i < 16; i++) nib[i] = (int)((m >> (60 - 4 * i)) & 0xf);
    int frac = 15;
    if (prec >= 0 && prec < 15) {
        /* round half-even at PREC fractional nibbles */
        int cut = prec + 1;
        int rd = nib[cut];
        int rest = 0;
        for (int i = cut + 1; i < 16; i++) if (nib[i]) rest = 1;
        int up = rd > 8 || (rd == 8 && (rest || (nib[cut - 1] & 1)));
        for (int i = cut; i < 16; i++) nib[i] = 0;
        if (up) {
            int i = cut - 1;
            while (i >= 0) {
                if (nib[i] == 15) { nib[i] = 0; i--; continue; }
                nib[i]++;
                break;
            }
            if (i < 0) { nib[0] = 1; e += 4; }
        }
        frac = prec;
    } else if (prec < 0) {
        while (frac > 0 && nib[frac] == 0) frac--;
    }
    sn_putc(out, hx[nib[0]]);
    if (frac > 0 || alt || prec > 0) sn_putc(out, '.');
    for (int i = 1; i <= frac && i < 16; i++) sn_putc(out, hx[nib[i]]);
    for (int i = 16; i <= prec; i++) sn_putc(out, '0');
    char eb[24];
    snprintf(eb, sizeof eb, "%c%c%ld", upper ? 'P' : 'p', e < 0 ? '-' : '+', e < 0 ? -e : e);
    sn_puts(out, eb);
}

/* V as FS asks, appended to OUT */
static void su_ldfmt(struct sn_sb *out, long double v, const struct su_fmtspec *fs) {
    union sn_ld u;
    u.v = v;
    int neg = (u.p.se & 0x8000) != 0;
    long field = u.p.se & 0x7fff;
    int upper = fs->conv >= 'A' && fs->conv <= 'Z';
    char lc = (char)(fs->conv | 0x20);
    struct sn_sb body = { 0, 0, 0 };
    int special = 0;
    if (field == 0x7fff) {
        special = 1;
        int nan = (u.p.m << 1) != 0;
        sn_puts(&body, nan ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf"));
    } else if (lc == 'a') {
        sn_hex(&body, v, fs->prec, fs->alt, upper);
    } else {
        long k;
        char *dig = sn_exact(v, &k);
        int prec = fs->prec < 0 ? 6 : fs->prec;
        if (lc == 'f') {
            sn_fixed(&body, dig, k, prec, fs->alt);
        } else if (lc == 'e') {
            sn_expo(&body, dig, k, prec, fs->alt, upper ? 'E' : 'e', 0);
        } else {
            int P = prec == 0 ? 1 : prec;
            char *rd;
            long X = sn_expo_digits(&rd, dig, k, P - 1);
            free(rd);
            if (X < -4 || X >= P) {
                sn_expo(&body, dig, k, P - 1, fs->alt, upper ? 'E' : 'e', !fs->alt);
            } else {
                struct sn_sb f = { 0, 0, 0 };
                sn_fixed(&f, dig, k, (int)(P - 1 - X), fs->alt);
                if (!fs->alt && strchr(f.s, '.')) {
                    size_t n = f.n;
                    while (n > 0 && f.s[n - 1] == '0') n--;
                    if (n > 0 && f.s[n - 1] == '.') n--;
                    f.s[n] = 0;
                    f.n = n;
                }
                sn_puts(&body, f.s);
                free(f.s);
            }
        }
        free(dig);
    }
    char sign = neg ? '-' : fs->plus ? '+' : fs->space ? ' ' : 0;
    size_t len = body.n + (sign ? 1 : 0);
    size_t pad = (fs->width > 0 && (size_t)fs->width > len) ? (size_t)fs->width - len : 0;
    int zero = fs->zero && !fs->left && !special;
    if (!fs->left && !zero) for (size_t i = 0; i < pad; i++) sn_putc(out, ' ');
    if (sign) sn_putc(out, sign);
    if (zero) {
        /* zeros after the sign, and after a 0x */
        const char *b = body.s;
        if (lc == 'a') { sn_put(out, b, 2); b += 2; }
        for (size_t i = 0; i < pad; i++) sn_putc(out, '0');
        sn_puts(out, b);
    } else {
        sn_puts(out, body.s);
    }
    if (fs->left) for (size_t i = 0; i < pad; i++) sn_putc(out, ' ');
    free(body.s);
}
