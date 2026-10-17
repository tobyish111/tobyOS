/* su_factor.c -- factor, as GNU coreutils' (logs/utilgate/factor.sh): the
 * prime factors of each NUMBER, or of each word of stdin, as
 * "N: p p q", ascending; -h/--exponents writes a repeated factor once, as
 * "p^e". Numbers of any size up to 4160 bits -- GNU's is GMP-backed and
 * factors 2^64 and 2^128-1 like 12 -- where the old factor took 64 bits
 * and printed 2^64 as 18446744073709551615's factors.
 *
 * A NUMBER is GNU's: leading spaces (spaces only), an optional '+', and
 * digits to the end -- leading zeros allowed, the number printed without
 * them. Anything else is "'W' is not a valid positive integer"; the rest
 * go on and the status is 1. A leading '-' is an option, as getopt has it.
 *
 * THE ARITHMETIC (no GMP here, and no floating point: native programs are
 * -mno-sse). Numbers are 32-bit limbs, least significant first. Trial
 * division by every prime below 2^16; what is left is prime when it is
 * below 2^32 (it has no factor under its square root) or when it passes
 * Miller-Rabin to the first 24 prime bases; otherwise Pollard-Brent rho
 * finds a factor and both halves go round again. The modular arithmetic is
 * Montgomery's -- the number is odd by then -- so nothing but the exact
 * division by a found factor (Hensel's, which needs an odd divisor) ever
 * divides two big numbers. */

#define FC_LIMBS 130                      /* 4160 bits */
struct fc_bn { int n; uint32_t d[FC_LIMBS]; };

static void fc_norm(struct fc_bn *a) { while (a->n > 0 && a->d[a->n - 1] == 0) a->n--; }
static void fc_set(struct fc_bn *a, uint32_t v) { a->n = v ? 1 : 0; a->d[0] = v; }
static int fc_is(const struct fc_bn *a, uint32_t v) {
    return v ? (a->n == 1 && a->d[0] == v) : a->n == 0;
}
static int fc_cmp(const struct fc_bn *a, const struct fc_bn *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}
/* a -= b, a >= b */
static void fc_sub(struct fc_bn *a, const struct fc_bn *b) {
    uint64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        uint64_t s = (uint64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
        a->d[i] = (uint32_t)s;
        borrow = (s >> 63) & 1;
    }
    fc_norm(a);
}
/* a = a * m + add; 0, or -1 on overflow */
static int fc_muladd(struct fc_bn *a, uint32_t m, uint32_t add) {
    uint64_t carry = add;
    for (int i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->d[i] * m + carry;
        a->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        if (a->n >= FC_LIMBS) return -1;
        a->d[a->n++] = (uint32_t)carry;
    }
    return 0;
}
/* a /= m, the remainder returned */
static uint32_t fc_divsmall(struct fc_bn *a, uint32_t m) {
    uint64_t r = 0;
    for (int i = a->n - 1; i >= 0; i--) {
        uint64_t cur = (r << 32) | a->d[i];
        a->d[i] = (uint32_t)(cur / m);
        r = cur % m;
    }
    fc_norm(a);
    return (uint32_t)r;
}
static uint32_t fc_modsmall(const struct fc_bn *a, uint32_t m) {
    uint64_t r = 0;
    for (int i = a->n - 1; i >= 0; i--) r = ((r << 32) | a->d[i]) % m;
    return (uint32_t)r;
}
static void fc_shr1(struct fc_bn *a) {
    for (int i = 0; i < a->n; i++)
        a->d[i] = (a->d[i] >> 1) | (i + 1 < a->n ? a->d[i + 1] << 31 : 0);
    fc_norm(a);
}
static void fc_shl1(struct fc_bn *a) {
    uint32_t carry = 0;
    for (int i = 0; i < a->n; i++) {
        uint32_t c2 = a->d[i] >> 31;
        a->d[i] = (a->d[i] << 1) | carry;
        carry = c2;
    }
    if (carry) a->d[a->n++] = carry;
}
/* q = a / d exactly; d odd and dividing a (Hensel / Jebelean) */
static void fc_divexact(struct fc_bn *q, const struct fc_bn *a, const struct fc_bn *d) {
    struct fc_bn r = *a;
    uint32_t inv = d->d[0];
    for (int k = 0; k < 5; k++) inv *= 2 - d->d[0] * inv;    /* d0^-1 mod 2^32 */
    int qn = a->n - d->n + 1;
    memset(q->d, 0, sizeof q->d);
    for (int i = 0; i < qn; i++) {
        uint32_t qi = r.d[i] * inv;
        q->d[i] = qi;
        uint64_t borrow = 0, carry = 0;
        for (int j = 0; i + j < r.n; j++) {
            uint64_t p = (j < d->n ? (uint64_t)d->d[j] * qi : 0) + carry;
            carry = p >> 32;
            uint64_t s = (uint64_t)r.d[i + j] - (uint32_t)p - borrow;
            r.d[i + j] = (uint32_t)s;
            borrow = (s >> 63) & 1;
        }
    }
    q->n = qn > 0 ? qn : 0;
    fc_norm(q);
}
/* binary gcd */
static void fc_gcd(struct fc_bn *g, const struct fc_bn *a0, const struct fc_bn *b0) {
    struct fc_bn a = *a0, b = *b0;
    if (a.n == 0) { *g = b; return; }
    if (b.n == 0) { *g = a; return; }
    int shift = 0;
    while (!(a.d[0] & 1) && !(b.d[0] & 1)) { fc_shr1(&a); fc_shr1(&b); shift++; }
    while (!(a.d[0] & 1)) fc_shr1(&a);
    while (b.n) {
        while (!(b.d[0] & 1)) fc_shr1(&b);
        if (fc_cmp(&a, &b) > 0) { struct fc_bn t = a; a = b; b = t; }
        fc_sub(&b, &a);
    }
    while (shift--) fc_shl1(&a);
    *g = a;
}

/* ---- Montgomery arithmetic modulo an odd N ---- */
struct fc_mont { struct fc_bn n; int k; uint32_t ninv; struct fc_bn r1, r2; };

/* a = 2a mod n, a < n */
static void fc_dbl_mod(struct fc_bn *a, const struct fc_bn *n) {
    fc_shl1(a);
    if (fc_cmp(a, n) >= 0) fc_sub(a, n);
}
static void fc_mont_init(struct fc_mont *m, const struct fc_bn *n) {
    m->n = *n;
    m->k = n->n;
    uint32_t inv = n->d[0];
    for (int i = 0; i < 5; i++) inv *= 2 - n->d[0] * inv;
    m->ninv = 0u - inv;
    /* R mod n and R^2 mod n by doubling 1 -- R = 2^(32k): no division */
    struct fc_bn x;
    fc_set(&x, 1);
    for (int i = 0; i < 32 * m->k; i++) fc_dbl_mod(&x, n);
    m->r1 = x;
    for (int i = 0; i < 32 * m->k; i++) fc_dbl_mod(&x, n);
    m->r2 = x;
}
/* r = a * b * R^-1 mod n (CIOS); a, b < n */
static void fc_montmul(const struct fc_mont *m, struct fc_bn *r,
                       const struct fc_bn *a, const struct fc_bn *b) {
    int k = m->k;
    uint32_t t[FC_LIMBS + 2];
    memset(t, 0, sizeof(uint32_t) * (size_t)(k + 2));
    for (int i = 0; i < k; i++) {
        uint32_t bi = i < b->n ? b->d[i] : 0;
        uint64_t c = 0;
        for (int j = 0; j < k; j++) {
            uint64_t s = (uint64_t)t[j] + (uint64_t)(j < a->n ? a->d[j] : 0) * bi + c;
            t[j] = (uint32_t)s;
            c = s >> 32;
        }
        uint64_t s = (uint64_t)t[k] + c;
        t[k] = (uint32_t)s;
        t[k + 1] = (uint32_t)(s >> 32);
        uint32_t mm = t[0] * m->ninv;
        s = (uint64_t)t[0] + (uint64_t)mm * m->n.d[0];
        c = s >> 32;
        for (int j = 1; j < k; j++) {
            s = (uint64_t)t[j] + (uint64_t)mm * m->n.d[j] + c;
            t[j - 1] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[k] + c;
        t[k - 1] = (uint32_t)s;
        t[k] = t[k + 1] + (uint32_t)(s >> 32);
    }
    r->n = k + 1;
    for (int i = 0; i <= k; i++) r->d[i] = t[i];
    fc_norm(r);
    if (fc_cmp(r, &m->n) >= 0) fc_sub(r, &m->n);
}
static void fc_to_mont(const struct fc_mont *m, struct fc_bn *r, const struct fc_bn *a) {
    fc_montmul(m, r, a, &m->r2);
}

/* Miller-Rabin to base B, n odd > B; true = probably prime */
static int fc_mr(const struct fc_mont *m, uint32_t base) {
    struct fc_bn nm1 = m->n, d, one, x, b, mone;
    struct fc_bn t1;
    fc_set(&t1, 1);
    fc_sub(&nm1, &t1);                           /* n - 1 */
    d = nm1;
    int s = 0;
    while (!(d.d[0] & 1)) { fc_shr1(&d); s++; }
    fc_set(&b, base);
    fc_to_mont(m, &b, &b);
    one = m->r1;                                  /* 1, Montgomery */
    mone = m->n;
    fc_sub(&mone, &m->r1);                        /* n - 1, Montgomery */
    /* x = b^d */
    x = one;
    int bits = d.n * 32;
    while (bits > 0 && !((d.d[(bits - 1) / 32] >> ((bits - 1) % 32)) & 1)) bits--;
    for (int i = bits - 1; i >= 0; i--) {
        fc_montmul(m, &x, &x, &x);
        if ((d.d[i / 32] >> (i % 32)) & 1) fc_montmul(m, &x, &x, &b);
    }
    if (fc_cmp(&x, &one) == 0 || fc_cmp(&x, &mone) == 0) return 1;
    for (int r = 1; r < s; r++) {
        fc_montmul(m, &x, &x, &x);
        if (fc_cmp(&x, &mone) == 0) return 1;
        if (fc_cmp(&x, &one) == 0) return 0;
    }
    return 0;
}

static const uint32_t fc_bases[24] = {
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89
};

/* N odd, with no factor below 2^16 */
static int fc_isprime(const struct fc_bn *n) {
    if (n->n == 1) return 1;                      /* < 2^32: no factor <= sqrt */
    struct fc_mont m;
    fc_mont_init(&m, n);
    for (int i = 0; i < 24; i++) if (!fc_mr(&m, fc_bases[i])) return 0;
    return 1;
}

/* |a - b| */
static void fc_absdiff(struct fc_bn *r, const struct fc_bn *a, const struct fc_bn *b) {
    if (fc_cmp(a, b) >= 0) { *r = *a; fc_sub(r, b); }
    else { *r = *b; fc_sub(r, a); }
}

/* a nontrivial factor of composite odd N (Pollard-Brent rho) */
static void fc_rho(struct fc_bn *f, const struct fc_bn *n) {
    struct fc_mont m;
    fc_mont_init(&m, n);
    for (uint32_t c0 = 1;; c0++) {
        struct fc_bn c, y, x, ys, q, g, t;
        fc_set(&c, c0);
        fc_to_mont(&m, &c, &c);
        fc_set(&y, 2);
        fc_to_mont(&m, &y, &y);
        q = m.r1;
        fc_set(&g, 1);
        unsigned long r = 1;
        const unsigned long batch = 128;
#define FC_F(v) do { fc_montmul(&m, &(v), &(v), &(v));                    \
                     struct fc_bn s_ = (v); /* v^2 + c mod n */            \
                     uint64_t cy_ = 0;                                     \
                     int L_ = m.k + 1;                                     \
                     for (int i_ = 0; i_ < L_; i_++) {                     \
                         uint64_t a_ = i_ < s_.n ? s_.d[i_] : 0;           \
                         uint64_t b_ = i_ < c.n ? c.d[i_] : 0;             \
                         uint64_t z_ = a_ + b_ + cy_;                      \
                         s_.d[i_] = (uint32_t)z_; cy_ = z_ >> 32;          \
                     }                                                     \
                     s_.n = L_; fc_norm(&s_);                              \
                     if (fc_cmp(&s_, &m.n) >= 0) fc_sub(&s_, &m.n);        \
                     (v) = s_; } while (0)
        do {
            x = y;
            for (unsigned long i = 0; i < r; i++) FC_F(y);
            unsigned long k = 0;
            do {
                ys = y;
                unsigned long lim = batch < r - k ? batch : r - k;
                for (unsigned long i = 0; i < lim; i++) {
                    FC_F(y);
                    fc_absdiff(&t, &x, &y);
                    fc_montmul(&m, &q, &q, &t);
                }
                fc_gcd(&g, &q, n);
                k += batch;
            } while (k < r && fc_is(&g, 1));
            r *= 2;
        } while (fc_is(&g, 1));
        if (fc_cmp(&g, n) == 0) {
            do {
                FC_F(ys);
                fc_absdiff(&t, &x, &ys);
                fc_gcd(&g, &t, n);
            } while (fc_is(&g, 1));
        }
#undef FC_F
        if (fc_cmp(&g, n) != 0) { *f = g; return; }
    }
}

/* the factors found, ascending at print time */
struct fc_fac { struct fc_bn p; unsigned e; };
static struct fc_fac *fc_list;
static int fc_nlist, fc_cap;

static void fc_add(const struct fc_bn *p, unsigned e) {
    for (int i = 0; i < fc_nlist; i++)
        if (fc_cmp(&fc_list[i].p, p) == 0) { fc_list[i].e += e; return; }
    if (fc_nlist == fc_cap) {
        fc_cap = fc_cap ? fc_cap * 2 : 32;
        fc_list = realloc(fc_list, (size_t)fc_cap * sizeof *fc_list);
        if (!fc_list) su_error(1, ENOMEM, "%s", "memory exhausted");
    }
    fc_list[fc_nlist].p = *p;
    fc_list[fc_nlist].e = e;
    fc_nlist++;
}

/* N odd, no factor below 2^16 */
static void fc_split(const struct fc_bn *n) {
    if (fc_is(n, 1)) return;
    if (fc_isprime(n)) { fc_add(n, 1); return; }
    struct fc_bn d, q;
    fc_rho(&d, n);
    fc_divexact(&q, n, &d);
    fc_split(&d);
    fc_split(&q);
}

static void fc_print_bn(const struct fc_bn *a) {
    struct fc_bn t = *a;
    if (t.n == 0) { putchar('0'); return; }
    uint32_t chunk[FC_LIMBS * 2];
    int nc = 0;
    while (t.n) chunk[nc++] = fc_divsmall(&t, 1000000000u);
    printf("%u", chunk[nc - 1]);
    for (int i = nc - 2; i >= 0; i--) printf("%09u", chunk[i]);
}

static int fc_cmp_fac(const void *a, const void *b) {
    return fc_cmp(&((const struct fc_fac *)a)->p, &((const struct fc_fac *)b)->p);
}

/* one word; 0, or 1 for an invalid one (said) */
static int fc_word(const char *w, int expo) {
    const char *p = w;
    while (*p == ' ') p++;
    if (*p == '+') p++;
    if (*p < '0' || *p > '9') goto bad;
    struct fc_bn n;
    n.n = 0;
    for (const char *q = p; *q; q++) {
        if (*q < '0' || *q > '9') goto bad;
        if (fc_muladd(&n, 10, (uint32_t)(*q - '0')) != 0) {
            su_error(0, 0, "%s is too large", su_quote(w));
            return 1;
        }
    }
    fc_print_bn(&n);
    putchar(':');
    fc_nlist = 0;
    if (n.n && !fc_is(&n, 1)) {
        /* the twos, then every prime below 2^16 */
        unsigned e = 0;
        while (!(n.d[0] & 1)) { fc_shr1(&n); e++; }
        if (e) { struct fc_bn two; fc_set(&two, 2); fc_add(&two, e); }
        for (uint32_t d = 3; d < 65536 && !fc_is(&n, 1); d += 2) {
            if (n.n == 1 && (uint64_t)d * d > n.d[0]) break;   /* the rest is prime */
            if (fc_modsmall(&n, d)) continue;
            unsigned k = 0;
            while (!fc_modsmall(&n, d)) { fc_divsmall(&n, d); k++; }
            struct fc_bn pd; fc_set(&pd, d);
            fc_add(&pd, k);
        }
        fc_split(&n);
        qsort(fc_list, (size_t)fc_nlist, sizeof *fc_list, fc_cmp_fac);
    }
    for (int i = 0; i < fc_nlist; i++) {
        if (expo) {
            putchar(' ');
            fc_print_bn(&fc_list[i].p);
            if (fc_list[i].e > 1) printf("^%u", fc_list[i].e);
        } else {
            for (unsigned k = 0; k < fc_list[i].e; k++) { putchar(' '); fc_print_bn(&fc_list[i].p); }
        }
    }
    putchar('\n');
    return 0;
bad:
    su_error(0, 0, "%s is not a valid positive integer", su_quote(w));
    return 1;
}

static int do_factor(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "exponents", SU_NO_ARG, 0, 'h' }, SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int c, expo = 0;
    while ((c = su_getopt_long(argc, argv, "h", longs, 0)) != -1) {
        switch (c) {
        case 'h': expo = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION] [NUMBER]...\n"
                   "Print the prime factors of each NUMBER, or of each number\n"
                   "read from standard input when there is none.\n"
                   "\n"
                   "  -h, --exponents  write a repeated factor once, as p^e\n"
                   "      --help       show this help and exit\n"
                   "      --version    show the version and exit\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    int rc = 0;
    if (su_optind < argc) {
        for (int i = su_optind; i < argc; i++) rc |= fc_word(argv[i], expo);
        return rc;
    }
    /* stdin: whitespace-separated words */
    char word[4096];
    size_t wl = 0;
    int ch;
    for (;;) {
        ch = getchar();
        if (ch == EOF || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
            ch == '\v' || ch == '\f') {
            if (wl) { word[wl] = 0; rc |= fc_word(word, expo); wl = 0; }
            if (ch == EOF) break;
            continue;
        }
        if (wl + 1 < sizeof word) word[wl++] = (char)ch;
    }
    return rc;
}
