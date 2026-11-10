/* shell_ldouble.inc -- printf's floating conversions as bash makes them.
 *
 * bash reads a %f %e %g %a operand with strtold and prints it through the C
 * library's printf with an L modifier: the number is an x87 LONG DOUBLE, 64
 * significant bits, and the digits printed are the exact decimal expansion
 * of that binary value, rounded half to even. The guest's bash links musl,
 * so its strtold and fmt_fp are the oracle (logs/b13 q50):
 *
 *     printf '%.30f' 0.1     0.100000000000000000001355252716
 *     printf '%.0f' 2.5      2
 *     printf '%a' 255        0x1.fep+7
 *     printf '%f' 0x10       16.000000
 *     printf '%f' 1e         1.000000, and "1e: invalid number"
 *
 * tsh carried the operand's decimal digits through instead -- exact for the
 * decimal, so the binary artifacts above never appeared, ties rounded away
 * from zero, %a and inf/nan did not exist, and 1e-310 came out as 1e-3310.
 *
 * Both shell builds use this, and the kernel's has no floating point: it is
 * all integer arithmetic, on little-endian 32-bit-limb bignums. */

/* ---- bignums ------------------------------------------------------------ */

struct ld_big { uint32_t *d; int n, cap; };

static void ldb_free(struct ld_big *b) {
    if (b->d) kfree(b->d);
    b->d = 0;
    b->n = b->cap = 0;
}

static bool ldb_reserve(struct ld_big *b, int need) {
    if (need <= b->cap) return true;
    int nc = b->cap ? b->cap * 2 : 8;
    while (nc < need) nc *= 2;
    uint32_t *nd = (uint32_t *)kmalloc((size_t)nc * sizeof *nd);
    if (!nd) return false;
    for (int i = 0; i < b->n; i++) nd[i] = b->d[i];
    if (b->d) kfree(b->d);
    b->d = nd;
    b->cap = nc;
    return true;
}

static void ldb_trim(struct ld_big *b) {
    while (b->n > 0 && b->d[b->n - 1] == 0) b->n--;
}

static bool ldb_set_u64(struct ld_big *b, uint64_t v) {
    if (!ldb_reserve(b, 2)) return false;
    b->d[0] = (uint32_t)v;
    b->d[1] = (uint32_t)(v >> 32);
    b->n = 2;
    ldb_trim(b);
    return true;
}

static bool ldb_copy(struct ld_big *dst, const struct ld_big *src) {
    if (!ldb_reserve(dst, src->n > 0 ? src->n : 1)) return false;
    for (int i = 0; i < src->n; i++) dst->d[i] = src->d[i];
    dst->n = src->n;
    return true;
}

/* b = b * m + add */
static bool ldb_mul_small(struct ld_big *b, uint32_t m, uint32_t add) {
    uint64_t carry = add;
    for (int i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->d[i] * m + carry;
        b->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        if (!ldb_reserve(b, b->n + 1)) return false;
        b->d[b->n++] = (uint32_t)carry;
    }
    return true;
}

/* b /= d, the remainder returned */
static uint32_t ldb_divmod_small(struct ld_big *b, uint32_t d) {
    uint64_t rem = 0;
    for (int i = b->n - 1; i >= 0; i--) {
        uint64_t cur = (rem << 32) | b->d[i];
        b->d[i] = (uint32_t)(cur / d);
        rem = cur % d;
    }
    ldb_trim(b);
    return (uint32_t)rem;
}

static int ldb_bitlen(const struct ld_big *b) {
    if (b->n == 0) return 0;
    uint32_t top = b->d[b->n - 1];
    int bits = 0;
    while (top) { bits++; top >>= 1; }
    return (b->n - 1) * 32 + bits;
}

static bool ldb_bit(const struct ld_big *b, int i) {
    if (i < 0 || i / 32 >= b->n) return false;
    return (b->d[i / 32] >> (i % 32)) & 1u;
}

/* Is any bit below position I set? */
static bool ldb_any_below(const struct ld_big *b, int i) {
    if (i <= 0) return false;
    int full = i / 32;
    for (int k = 0; k < full && k < b->n; k++)
        if (b->d[k]) return true;
    if (full < b->n && (i % 32) &&
        (b->d[full] & ((1u << (i % 32)) - 1u)))
        return true;
    return false;
}

/* CNT (at most 64) bits from position LO up */
static uint64_t ldb_bits(const struct ld_big *b, int lo, int cnt) {
    uint64_t v = 0;
    for (int k = cnt - 1; k >= 0; k--) v = (v << 1) | (ldb_bit(b, lo + k) ? 1u : 0u);
    return v;
}

static bool ldb_shl(struct ld_big *b, int bits) {
    if (b->n == 0 || bits <= 0) return true;
    int limbs = bits / 32, sh = bits % 32;
    if (!ldb_reserve(b, b->n + limbs + 1)) return false;
    int n = b->n;
    b->d[n + limbs] = 0;
    for (int i = n - 1; i >= 0; i--) {
        uint32_t v = b->d[i];
        if (sh) {
            b->d[i + limbs + 1] |= v >> (32 - sh);
            b->d[i + limbs] = v << sh;
        } else {
            b->d[i + limbs] = v;
        }
    }
    for (int i = 0; i < limbs; i++) b->d[i] = 0;
    b->n = n + limbs + 1;
    ldb_trim(b);
    return true;
}

static void ldb_shr(struct ld_big *b, int bits) {
    if (bits <= 0 || b->n == 0) return;
    int limbs = bits / 32, sh = bits % 32;
    if (limbs >= b->n) { b->n = 0; return; }
    for (int i = 0; i < b->n - limbs; i++) {
        uint32_t lo = b->d[i + limbs] >> sh;
        uint32_t hi = (sh && i + limbs + 1 < b->n) ? b->d[i + limbs + 1] << (32 - sh) : 0;
        b->d[i] = lo | hi;
    }
    b->n -= limbs;
    ldb_trim(b);
}

static int ldb_cmp(const struct ld_big *a, const struct ld_big *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

/* a -= b, with a >= b */
static void ldb_sub(struct ld_big *a, const struct ld_big *b) {
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - (i < b->n ? (int64_t)b->d[i] : 0) - borrow;
        borrow = t < 0;
        if (t < 0) t += (int64_t)1 << 32;
        a->d[i] = (uint32_t)t;
    }
    ldb_trim(a);
}

/* b = 10^k */
static bool ldb_pow10(struct ld_big *b, int k) {
    if (!ldb_set_u64(b, 1)) return false;
    while (k >= 9) { if (!ldb_mul_small(b, 1000000000u, 0)) return false; k -= 9; }
    static const uint32_t p10[] = { 1, 10, 100, 1000, 10000, 100000, 1000000,
                                    10000000, 100000000 };
    return ldb_mul_small(b, p10[k], 0);
}

/* a = a / b (the quotient, which here has some 70 bits), *rem_nz whether
 * anything was left over. Binary long division: cheap for a quotient this
 * short whatever the size of the operands. */
static bool ldb_div(struct ld_big *a, const struct ld_big *b, bool *rem_nz) {
    struct ld_big q = {0}, sb = {0};
    int shift = ldb_bitlen(a) - ldb_bitlen(b);
    bool ok = true;
    if (shift < 0) {
        *rem_nz = a->n != 0;
        a->n = 0;
        return true;
    }
    ok = ldb_copy(&sb, b) && ldb_shl(&sb, shift) && ldb_reserve(&q, shift / 32 + 2);
    if (ok) {
        for (int i = 0; i < shift / 32 + 2; i++) q.d[i] = 0;
        q.n = shift / 32 + 2;
        for (int i = shift; i >= 0; i--) {
            if (ldb_cmp(a, &sb) >= 0) {
                ldb_sub(a, &sb);
                q.d[i / 32] |= 1u << (i % 32);
            }
            ldb_shr(&sb, 1);
        }
        ldb_trim(&q);
        *rem_nz = a->n != 0;
        ok = ldb_copy(a, &q);
    }
    ldb_free(&q);
    ldb_free(&sb);
    return ok;
}

/* ---- the value ------------------------------------------------------------ */

#define LD_ZERO 0
#define LD_FIN  1
#define LD_INF  2
#define LD_NAN  3

/* A long double: LD_FIN is m * 2^e, m nonzero; the others carry a sign. */
struct sh_ld {
    int      cls;
    bool     neg;
    uint64_t m;
    int      e;
};

#define LD_EMIN_LSB (-16445)     /* the least subnormal: 2^-16445 */
#define LD_EMIN     (-16382)     /* the least normal exponent */
#define LD_EMAX     16383

/* Round Q * 2^-S -- plus a little more when STICKY -- to a long double:
 * 64 significant bits, half to even, subnormal below 2^-16382. *ERANGE as
 * musl sets it: on overflow, and for a subnormal result that is inexact. */
static void ld_round(const struct ld_big *q, long s, bool sticky, struct sh_ld *v,
                     bool *erange) {
    int L = ldb_bitlen(q);
    if (L == 0) {
        v->cls = LD_ZERO;
        if (sticky) *erange = true;
        return;
    }
    long eb = (long)L - 1 - s;                  /* the leading bit's exponent */
    long lsb = eb - 63;
    if (lsb < LD_EMIN_LSB) lsb = LD_EMIN_LSB;
    long drop = lsb + s;
    uint64_t m;
    bool inexact = sticky;
    if (drop <= 0) {
        m = ldb_bits(q, 0, L) << (-drop);
    } else {
        int keep = L - (int)drop;
        m = keep > 0 ? ldb_bits(q, (int)drop, keep) : 0;
        bool guard = ldb_bit(q, (int)drop - 1);
        bool rest = ldb_any_below(q, (int)drop - 1) || sticky;
        inexact = guard || rest;
        if (guard && (rest || (m & 1u))) {
            m++;
            if (m == 0) {                       /* carried out of 64 bits */
                m = (uint64_t)1 << 63;
                lsb++;
            }
        }
    }
    if (m == 0) {
        v->cls = LD_ZERO;
        *erange = true;
        return;
    }
    int mb = 0;
    for (uint64_t t = m; t; t >>= 1) mb++;
    long top = mb - 1 + lsb;
    if (top > LD_EMAX) {
        v->cls = LD_INF;
        *erange = true;
        return;
    }
    if (top < LD_EMIN && inexact) *erange = true;
    v->cls = LD_FIN;
    v->m = m;
    v->e = (int)lsb;
}

static bool ld_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static int ld_lower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static int ld_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = ld_lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* An exponent's digits, saturated far beyond any that matters. */
static long ld_exp_digits(const char **pp) {
    const char *p = *pp;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if (*p < '0' || *p > '9') return 0;
    long e = 0;
    while (*p >= '0' && *p <= '9') {
        if (e < 100000000L) e = e * 10 + (*p - '0');
        p++;
    }
    *pp = p;
    return neg ? -e : e;
}

/* strtold as the guest's C library has it: blanks, a sign, then inf or
 * infinity, nan or nan(chars), a hexadecimal number (0x, digits and a
 * point, an optional p exponent) or a decimal one (an optional e exponent),
 * rounded correctly to a long double. The return is where it stopped: the
 * start, when nothing was a number. */
static const char *ld_scan(const char *s, struct sh_ld *v, bool *erange) {
    const char *p = s;
    *erange = false;
    v->cls = LD_ZERO;
    v->neg = false;
    while (ld_space(*p)) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';

    static const char kinf[] = "infinity", knan[] = "nan";
    int i = 0;
    while (i < 8 && ld_lower((unsigned char)p[i]) == kinf[i]) i++;
    if (i >= 3) {
        v->cls = LD_INF;
        v->neg = neg;
        return p + (i == 8 ? 8 : 3);
    }
    if (i == 0) {
        while (i < 3 && ld_lower((unsigned char)p[i]) == knan[i]) i++;
        if (i == 3) {
            /* musl returns NAN for "-nan", unsigned: bash prints "nan" */
            v->cls = LD_NAN;
            v->neg = false;
            const char *q = p + 3;
            if (*q == '(') {
                const char *r = q + 1;
                while ((*r >= '0' && *r <= '9') || (*r >= 'a' && *r <= 'z') ||
                       (*r >= 'A' && *r <= 'Z') || *r == '_')
                    r++;
                if (*r == ')') return r + 1;
            }
            return q;
        }
    }
    if (i) return s;                            /* "in", "na": not a number */

    struct ld_big n = {0};
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        /* hexadecimal: the digits as one integer, the point and the p
         * exponent as a power of two */
        const char *q = p + 2;
        bool got = false, point = false;
        long fbits = 0;
        (void)ldb_set_u64(&n, 0);
        for (;; q++) {
            if (*q == '.' && !point) { point = true; continue; }
            int h = ld_hexval((unsigned char)*q);
            if (h < 0) break;
            got = true;
            if (!ldb_mul_small(&n, 16, (uint32_t)h)) break;
            if (point) fbits += 4;
        }
        if (!got) {                             /* "0x" is the 0 */
            ldb_free(&n);
            v->neg = neg;
            return p + 1;
        }
        long pe = 0;
        if (*q == 'p' || *q == 'P') {
            const char *r = q + 1;
            long x = ld_exp_digits(&r);
            if (r != q + 1 && (r[-1] >= '0' && r[-1] <= '9')) { pe = x; q = r; }
        }
        v->neg = neg;
        if (n.n == 0) {
            v->cls = LD_ZERO;
        } else {
            long s2 = fbits - pe;
            long lead = (long)ldb_bitlen(&n) - 1 - s2;
            if (lead > LD_EMAX + 1) { v->cls = LD_INF; *erange = true; }
            else if (lead < LD_EMIN_LSB - 2) { v->cls = LD_ZERO; *erange = true; }
            else ld_round(&n, s2, false, v, erange);
        }
        ldb_free(&n);
        return q;
    }

    /* decimal: the significant digits as one integer, the point and the e
     * exponent as a power of ten */
    const char *q = p;
    bool got = false, point = false;
    long fdig = 0, nd = 0;
    (void)ldb_set_u64(&n, 0);
    for (;; q++) {
        if (*q == '.' && !point) { point = true; continue; }
        if (*q < '0' || *q > '9') break;
        got = true;
        if (point) fdig++;
        if (n.n == 0 && *q == '0') continue;    /* leading zeros */
        nd++;
        if (!ldb_mul_small(&n, 10, (uint32_t)(*q - '0'))) break;
    }
    if (!got) {
        ldb_free(&n);
        return s;
    }
    long ex = 0;
    if (*q == 'e' || *q == 'E') {
        const char *r = q + 1;
        long x = ld_exp_digits(&r);
        if (r != q + 1 && (r[-1] >= '0' && r[-1] <= '9')) { ex = x; q = r; }
    }
    v->neg = neg;
    if (n.n == 0) {
        v->cls = LD_ZERO;
        ldb_free(&n);
        return q;
    }
    long e10 = ex - fdig;                       /* value = n * 10^e10 */
    /* far out of range either way: no need for the arithmetic */
    if (nd + e10 > 4934) {
        v->cls = LD_INF;
        *erange = true;
        ldb_free(&n);
        return q;
    }
    if (nd + e10 < -4953) {
        v->cls = LD_ZERO;
        *erange = true;
        ldb_free(&n);
        return q;
    }
    struct ld_big t = {0};
    if (e10 >= 0) {
        /* n * 10^e10 by small multiplications */
        long k = e10;
        bool ok = true;
        while (ok && k >= 9) { ok = ldb_mul_small(&n, 1000000000u, 0); k -= 9; }
        static const uint32_t p10[] = { 1, 10, 100, 1000, 10000, 100000,
                                        1000000, 10000000, 100000000 };
        if (ok) ok = ldb_mul_small(&n, p10[k], 0);
        if (ok) ld_round(&n, 0, false, v, erange);
    } else {
        bool rem = false;
        if (ldb_pow10(&t, (int)-e10)) {
            long s2 = (long)ldb_bitlen(&t) - (long)ldb_bitlen(&n) + 70;
            if (s2 < 0) s2 = 0;
            if (ldb_shl(&n, (int)s2) && ldb_div(&n, &t, &rem))
                ld_round(&n, s2, rem, v, erange);
        }
    }
    ldb_free(&t);
    ldb_free(&n);
    return q;
}

/* ---- printing ----------------------------------------------------------------- */

/* The exact decimal expansion of a finite value, a digit at a time: the
 * integer part's digits all at once (INT, most significant first, none for
 * 0), the fraction's on demand. */
struct ld_digits {
    char *ip;                 /* integer digits, NUL-terminated */
    int   ipn;
    struct ld_big frac;       /* the fraction, over 2^fbits */
    int   fbits;
};

static bool ld_digits_open(const struct sh_ld *v, struct ld_digits *g) {
    g->ip = 0;
    g->ipn = 0;
    g->frac.d = 0; g->frac.n = g->frac.cap = 0;
    g->fbits = 0;
    struct ld_big in = {0};
    bool ok = ldb_set_u64(&in, v->cls == LD_FIN ? v->m : 0);
    if (ok && v->cls == LD_FIN && v->e >= 0) {
        ok = ldb_shl(&in, v->e);
    } else if (ok && v->cls == LD_FIN) {
        g->fbits = -v->e;
        ok = ldb_copy(&g->frac, &in);
        if (ok) ldb_shr(&in, g->fbits);
        /* frac = m mod 2^fbits */
        if (ok && g->fbits < 64) {
            uint64_t mask = ((uint64_t)1 << g->fbits) - 1;
            ok = ldb_set_u64(&g->frac, v->m & mask);
        }
    }
    /* the integer part in decimal, by nine digits at a time */
    if (ok) {
        int cap = ldb_bitlen(&in) / 3 + 16;
        g->ip = (char *)kmalloc((size_t)cap);
        if (!g->ip) ok = false;
        int n = 0;
        while (ok && in.n) {
            uint32_t r = ldb_divmod_small(&in, 1000000000u);
            for (int k = 0; k < 9 && n < cap - 1; k++) {
                g->ip[n++] = (char)('0' + r % 10);
                r /= 10;
            }
        }
        while (ok && n > 0 && g->ip[n - 1] == '0') n--;
        for (int a = 0, b = n - 1; ok && a < b; a++, b--) {
            char c = g->ip[a]; g->ip[a] = g->ip[b]; g->ip[b] = c;
        }
        if (ok) { g->ip[n] = 0; g->ipn = n; }
    }
    ldb_free(&in);
    return ok;
}

static void ld_digits_close(struct ld_digits *g) {
    if (g->ip) kfree(g->ip);
    g->ip = 0;
    ldb_free(&g->frac);
}

/* The next digit of the fraction (0 once it has run out). */
static int ld_digits_next(struct ld_digits *g) {
    if (g->frac.n == 0) return 0;
    if (!ldb_mul_small(&g->frac, 10, 0)) return 0;
    int d = (int)ldb_bits(&g->frac, g->fbits, 4);
    /* clear what is at and above fbits */
    int limb = g->fbits / 32, sh = g->fbits % 32;
    if (limb < g->frac.n) {
        g->frac.d[limb] &= sh ? ((1u << sh) - 1u) : 0u;
        g->frac.n = limb + 1;
        ldb_trim(&g->frac);
    }
    return d;
}

/* Round the digit string D (N digits) at keeping K of them, half to even
 * on what follows -- NEXT the first dropped digit, MORE whether any after it
 * is nonzero. True when the rounding carried out of the first digit (D is
 * then all zeros: the caller puts a 1 in front). */
static bool ld_round_digits(char *d, int k, int next, bool more) {
    bool up;
    if (next > 5) up = true;
    else if (next < 5) up = false;
    else if (more) up = true;
    else up = k > 0 ? ((d[k - 1] - '0') & 1) : false;
    if (!up) return false;
    for (int i = k - 1; i >= 0; i--) {
        if (d[i] == '9') { d[i] = '0'; continue; }
        d[i]++;
        return false;
    }
    return true;
}

/* %f: the digits of the integer part and PREC of the fraction, rounded. */
static bool ld_fmt_f(struct shell_sb *b, const struct sh_ld *v, int prec, bool alt) {
    struct ld_digits g;
    if (!ld_digits_open(v, &g)) { ld_digits_close(&g); return false; }
    int total = g.ipn + prec;
    char *d = (char *)kmalloc((size_t)total + 2);
    if (!d) { ld_digits_close(&g); return false; }
    for (int i = 0; i < g.ipn; i++) d[i] = g.ip[i];
    for (int i = 0; i < prec; i++) d[g.ipn + i] = (char)('0' + ld_digits_next(&g));
    int next = ld_digits_next(&g);
    bool more = g.frac.n != 0;
    bool carry = ld_round_digits(d, total, next, more);
    if (carry) (void)sb_putc(b, '1');
    if (g.ipn == 0 && !carry) (void)sb_putc(b, '0');
    (void)sb_putn(b, d, (size_t)g.ipn);
    if (prec > 0 || alt) (void)sb_putc(b, '.');
    (void)sb_putn(b, d + g.ipn, (size_t)prec);
    kfree(d);
    ld_digits_close(&g);
    return true;
}

/* The first P significant digits of V, rounded, into D (P bytes), and the
 * decimal exponent of the first; 0 is all zeros, exponent 0. */
static bool ld_sig_digits(const struct sh_ld *v, int P, char *d, int *x10) {
    if (v->cls != LD_FIN) {
        for (int i = 0; i < P; i++) d[i] = '0';
        *x10 = 0;
        return true;
    }
    struct ld_digits g;
    if (!ld_digits_open(v, &g)) { ld_digits_close(&g); return false; }
    int have = 0, X;
    int next;
    if (g.ipn > 0) {
        X = g.ipn - 1;
        int i = 0;
        for (; i < g.ipn && have < P; i++) d[have++] = g.ip[i];
        if (have < P) {
            while (have < P) d[have++] = (char)('0' + ld_digits_next(&g));
            next = ld_digits_next(&g);
        } else {
            next = i < g.ipn ? g.ip[i++] - '0' : ld_digits_next(&g);
        }
        bool more = g.frac.n != 0;
        for (; i < g.ipn; i++) if (g.ip[i] != '0') more = true;
        if (ld_round_digits(d, P, next, more)) {
            d[0] = '1';
            X++;
        }
    } else {
        int z = 0, c;
        while ((c = ld_digits_next(&g)) == 0) z++;
        X = -(z + 1);
        d[have++] = (char)('0' + c);
        while (have < P) d[have++] = (char)('0' + ld_digits_next(&g));
        next = ld_digits_next(&g);
        if (ld_round_digits(d, P, next, g.frac.n != 0)) {
            d[0] = '1';
            X++;
        }
    }
    *x10 = X;
    ld_digits_close(&g);
    return true;
}

static void ld_put_exp(struct shell_sb *b, char E, int x) {
    (void)sb_putc(b, E);
    (void)sb_putc(b, x < 0 ? '-' : '+');
    unsigned ux = x < 0 ? (unsigned)-x : (unsigned)x;
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + ux % 10); ux /= 10; } while (ux);
    if (n < 2) t[n++] = '0';
    while (n) (void)sb_putc(b, t[--n]);
}

/* %e: one digit, the point, PREC digits, the exponent. */
static bool ld_fmt_e(struct shell_sb *b, const struct sh_ld *v, int prec, bool alt,
                     char E) {
    char *d = (char *)kmalloc((size_t)prec + 2);
    if (!d) return false;
    int x = 0;
    if (!ld_sig_digits(v, prec + 1, d, &x)) { kfree(d); return false; }
    (void)sb_putc(b, d[0]);
    if (prec > 0 || alt) (void)sb_putc(b, '.');
    (void)sb_putn(b, d + 1, (size_t)prec);
    ld_put_exp(b, E, x);
    kfree(d);
    return true;
}

/* %g: P significant digits, as %e when the exponent is below -4 or not
 * below P, else as %f -- and without trailing zeros unless `#`. */
static bool ld_fmt_g(struct shell_sb *b, const struct sh_ld *v, int prec, bool alt,
                     char E) {
    int P = prec < 0 ? 6 : (prec == 0 ? 1 : prec);
    char *d = (char *)kmalloc((size_t)P + 2);
    if (!d) return false;
    int x = 0;
    if (!ld_sig_digits(v, P, d, &x)) { kfree(d); return false; }
    int keep = P;
    if (!alt) while (keep > 1 && d[keep - 1] == '0') keep--;
    if (x < -4 || x >= P) {
        (void)sb_putc(b, d[0]);
        if (keep > 1 || alt) (void)sb_putc(b, '.');
        (void)sb_putn(b, d + 1, (size_t)(keep - 1));
        ld_put_exp(b, E, x);
    } else if (x >= 0) {
        /* x+1 digits before the point */
        (void)sb_putn(b, d, (size_t)(x + 1 <= keep ? x + 1 : keep));
        for (int i = keep; i < x + 1; i++) (void)sb_putc(b, '0');
        if (keep > x + 1 || alt) (void)sb_putc(b, '.');
        if (keep > x + 1) (void)sb_putn(b, d + x + 1, (size_t)(keep - x - 1));
    } else {
        (void)sb_puts(b, "0.");
        for (int i = 0; i < -x - 1; i++) (void)sb_putc(b, '0');
        (void)sb_putn(b, d, (size_t)keep);
    }
    kfree(d);
    return true;
}

/* %a: 0x1.hhhhp+N -- the leading digit 1 (0 for zero), the rest of the 64
 * bits in hex, as many digits as they need or PREC rounded half to even. */
static void ld_fmt_a(struct shell_sb *b, const struct sh_ld *v, int prec, bool alt,
                     bool upper) {
    const char *xd = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint64_t m = 0;
    int e2 = 0;
    if (v->cls == LD_FIN) {
        m = v->m;
        e2 = v->e;
        while (!(m >> 63)) { m <<= 1; e2--; }
        e2 += 63;
    }
    int lead = v->cls == LD_FIN ? 1 : 0;
    uint64_t frac = m << 1;                     /* the 63 bits after the 1, as 16 digits */
    /* musl rounds only below 15 digits (LDBL_MANT_DIG/4-1); from 15 on,
     * every digit the value has is printed, and zeros up to PREC */
    if (prec >= 0 && prec < 15 && v->cls == LD_FIN) {
        int dropbits = 64 - 4 * prec;
        uint64_t kept = dropbits >= 64 ? 0 : frac >> dropbits;
        uint64_t rest = dropbits >= 64 ? frac : frac & (((uint64_t)1 << dropbits) - 1);
        uint64_t half = (uint64_t)1 << (dropbits - 1);
        bool up = rest > half || (rest == half && ((prec == 0 ? (uint64_t)lead : kept) & 1));
        if (up) {
            kept++;
            if (prec == 0 || (kept >> (4 * prec))) { lead++; kept = 0; }
        }
        frac = prec == 0 ? 0 : kept << dropbits;
    }
    (void)sb_putc(b, xd[lead]);
    int nd = 16;
    while (nd > 0 && ((frac >> (64 - 4 * nd)) & 15u) == 0) nd--;
    if (prec > nd) nd = prec;
    if (nd > 0 || alt) (void)sb_putc(b, '.');
    for (int i = 0; i < nd; i++) {
        int digit = i < 16 ? (int)((frac >> (60 - 4 * i)) & 15u) : 0;
        (void)sb_putc(b, xd[digit]);
    }
    (void)sb_putc(b, upper ? 'P' : 'p');
    (void)sb_putc(b, e2 < 0 ? '-' : '+');
    char t[12];
    int n = 0;
    unsigned ue = e2 < 0 ? (unsigned)-e2 : (unsigned)e2;
    do { t[n++] = (char)('0' + ue % 10); ue /= 10; } while (ue);
    while (n) (void)sb_putc(b, t[--n]);
}

/* One floating conversion, the field and all, as musl's printf writes it:
 * the sign (`-`, or `+` or a blank when asked), 0x for %a, zeros after
 * those when `0` pads and the value is finite, blanks before or after for
 * the width. inf and nan are words, upper case for the upper-case
 * conversions. */
static void ld_format(struct shell_sb *out, const struct sh_ld *v, char conv,
                      int prec, bool alt, bool plus, bool space, bool left,
                      bool zero, int width) {
    bool upper = conv == 'F' || conv == 'E' || conv == 'G' || conv == 'A';
    SH_SB(body);
    char pfx[4];
    int pl = 0;
    if (v->neg) pfx[pl++] = '-';
    else if (plus) pfx[pl++] = '+';
    else if (space) pfx[pl++] = ' ';
    if (v->cls == LD_INF || v->cls == LD_NAN) {
        (void)sb_puts(&body, v->cls == LD_INF ? (upper ? "INF" : "inf")
                                              : (upper ? "NAN" : "nan"));
        zero = false;
    } else {
        char c = (char)ld_lower(conv);
        if (c == 'a') {
            pfx[pl++] = '0';
            pfx[pl++] = upper ? 'X' : 'x';
            ld_fmt_a(&body, v, prec, alt, upper);
        } else if (c == 'f') {
            (void)ld_fmt_f(&body, v, prec < 0 ? 6 : prec, alt);
        } else if (c == 'e') {
            (void)ld_fmt_e(&body, v, prec < 0 ? 6 : prec, alt, upper ? 'E' : 'e');
        } else {
            (void)ld_fmt_g(&body, v, prec, alt, upper ? 'E' : 'e');
        }
    }
    if (left) zero = false;
    int len = pl + (int)body.len;
    int pad = width > len ? width - len : 0;
    if (!left && !zero) for (int i = 0; i < pad; i++) (void)sb_putc(out, ' ');
    (void)sb_putn(out, pfx, (size_t)pl);
    if (zero) for (int i = 0; i < pad; i++) (void)sb_putc(out, '0');
    if (body.p) (void)sb_putn(out, body.p, body.len);
    if (left) for (int i = 0; i < pad; i++) (void)sb_putc(out, ' ');
}
