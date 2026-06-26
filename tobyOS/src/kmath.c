/* kmath.c -- kernel-side libm for the Win32 float-return shims (Track C / C20).
 *
 * FLOATING POINT IN THE KERNEL: like kfont.c, this file is compiled -msse
 * (overriding the kernel's default -mno-sse) and every entry point brackets its
 * FP work in kmfpu_begin/kmfpu_end, which FXSAVE the calling user process's live
 * SSE/x87 state and FXRSTOR it afterwards -- a syscall does not save user FP on
 * entry, so without this guard the math would corrupt the app's registers. The
 * guarded windows are pure computation (no sched_yield), so they are atomic
 * w.r.t. the user's FP state.
 *
 * The result is returned as a bit pattern; the C20 float-return gate does
 * `movq xmm0, rax` after the dispatch syscall so the double lands in xmm0 per
 * the MS-x64 ABI. (kmfpu_end restores the user's xmm0 to its pre-call value, but
 * the gate immediately overwrites it with the result -- and xmm1..xmm5 are
 * volatile across a call, while xmm6..xmm15 are restored, so the ABI holds.)
 *
 * Accuracy target is ~1e-9 relative over the normal range -- ample for real
 * programs (and for C20's tolerance-checked proof). Transcendentals use range
 * reduction + series; sqrt/fabs lower to single SSE2 instructions. */

#include <tobyos/kmath.h>
#include <tobyos/cpu.h>      /* fpu_save / fpu_restore (fxsave/fxrstor) */
#include <stdint.h>

static uint8_t g_kmfpu[512] __attribute__((aligned(16)));
static int     g_kmdepth;
static inline void kmfpu_begin(void) { if (g_kmdepth++ == 0) fpu_save(g_kmfpu); }
static inline void kmfpu_end(void)   { if (--g_kmdepth == 0) fpu_restore(g_kmfpu); }

static inline double   b2d(uint64_t b) { double d; __builtin_memcpy(&d, &b, 8); return d; }
static inline uint64_t d2b(double d)   { uint64_t b; __builtin_memcpy(&b, &d, 8); return b; }

#define KM_PI    3.14159265358979311600
#define KM_2PI   6.28318530717958623200
#define KM_PI_2  1.57079632679489655800
#define KM_PI_6  0.52359877559829887308
#define KM_LN2   0.69314718055994530942
#define KM_LN10  2.30258509299404568402
#define KM_SQRT3 1.73205080756887729353
#define KM_INF   (1e308 * 1e308)
#define KM_NAN   (KM_INF - KM_INF)

static double m_fabs(double x)  { return x < 0 ? -x : x; }
static double m_trunc(double x) { return (double)(long long)x; }
static double m_floor(double x) { double t = (double)(long long)x; return (t > x) ? t - 1.0 : t; }
static double m_ceil(double x)  { double t = (double)(long long)x; return (t < x) ? t + 1.0 : t; }
static double m_round(double x) { return x < 0 ? -m_floor(-x + 0.5) : m_floor(x + 0.5); }
static double m_fmod(double a, double b) { if (b == 0) return KM_NAN; return a - m_trunc(a / b) * b; }

/* e^x: k = round(x/ln2), r = x - k*ln2 in [-ln2/2, ln2/2], Taylor on r, scale 2^k. */
static double m_exp(double x) {
    if (x > 709.0)  return KM_INF;
    if (x < -745.0) return 0.0;
    double k = m_floor(x / KM_LN2 + 0.5);
    double r = x - k * KM_LN2;
    double term = 1.0, sum = 1.0;
    for (int i = 1; i <= 16; i++) { term *= r / i; sum += term; }
    double p2 = 1.0; int n = (int)k;
    if (n >= 0) { for (int i = 0; i < n; i++)  p2 *= 2.0; }
    else        { for (int i = 0; i < -n; i++) p2 *= 0.5; }
    return sum * p2;
}
/* ln(x): x = m * 2^e with m in [sqrt(.5), sqrt(2)); ln(m) via 2*atanh((m-1)/(m+1)). */
static double m_log(double x) {
    if (x < 0)  return KM_NAN;
    if (x == 0) return -KM_INF;
    uint64_t b = d2b(x);
    int e = (int)((b >> 52) & 0x7ff) - 1023;
    uint64_t mb = (b & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
    double m = b2d(mb);
    if (m > 1.41421356237309504880) { m *= 0.5; e++; }
    double t = (m - 1.0) / (m + 1.0), t2 = t * t, tn = t, sum = 0.0;
    for (int i = 1; i <= 25; i += 2) { sum += tn / i; tn *= t2; }
    return 2.0 * sum + (double)e * KM_LN2;
}
/* sin/cos: reduce mod 2pi into [-pi,pi], Taylor (pi^25/25! is negligible). */
static double m_sin(double x) {
    double y = m_fmod(x, KM_2PI);
    if (y > KM_PI) y -= KM_2PI; else if (y < -KM_PI) y += KM_2PI;
    double y2 = y * y, term = y, sum = y;
    for (int i = 1; i <= 13; i++) { term *= -y2 / (double)((2 * i) * (2 * i + 1)); sum += term; }
    return sum;
}
static double m_cos(double x) {
    double y = m_fmod(x, KM_2PI);
    if (y > KM_PI) y -= KM_2PI; else if (y < -KM_PI) y += KM_2PI;
    double y2 = y * y, term = 1.0, sum = 1.0;
    for (int i = 1; i <= 13; i++) { term *= -y2 / (double)((2 * i - 1) * (2 * i)); sum += term; }
    return sum;
}
static double m_tan(double x) { double c = m_cos(x); return (c == 0) ? KM_INF : m_sin(x) / c; }

/* atan: reduce to |x|<=tan(pi/12) via x>1 -> pi/2-atan(1/x) and the pi/6 step. */
static double small_atan(double t) {
    double t2 = t * t, term = t, sum = t;
    for (int i = 1; i <= 12; i++) { term *= -t2; sum += term / (double)(2 * i + 1); }
    return sum;
}
static double atan_core(double x) {     /* x in [0,1] */
    if (x > 0.26794919243112270647)
        return KM_PI_6 + small_atan((KM_SQRT3 * x - 1.0) / (KM_SQRT3 + x));
    return small_atan(x);
}
static double m_atan(double x) {
    int neg = 0; if (x < 0) { neg = 1; x = -x; }
    double r = (x > 1.0) ? (KM_PI_2 - atan_core(1.0 / x)) : atan_core(x);
    return neg ? -r : r;
}
static double m_atan2(double y, double x) {
    if (x > 0) return m_atan(y / x);
    if (x < 0) return (y >= 0) ? m_atan(y / x) + KM_PI : m_atan(y / x) - KM_PI;
    if (y > 0) return KM_PI_2;
    if (y < 0) return -KM_PI_2;
    return 0.0;
}
static double m_asin(double x) {
    if (x >= 1.0)  return KM_PI_2;
    if (x <= -1.0) return -KM_PI_2;
    return m_atan(x / __builtin_sqrt(1.0 - x * x));
}
static double m_pow(double b, double e) {
    if (e == 0.0) return 1.0;
    if (b == 0.0) return 0.0;
    if (b > 0.0)  return m_exp(e * m_log(b));
    double ei = m_trunc(e);
    if (ei != e) return KM_NAN;                 /* negative base, non-integer exp */
    double r = m_exp(e * m_log(-b));
    return ((long)ei & 1) ? -r : r;
}
static double m_cbrt(double x) {
    if (x == 0.0) return 0.0;
    double r = m_exp(m_log(m_fabs(x)) / 3.0);
    return x < 0 ? -r : r;
}
static double m_ldexp(double x, long n) {
    double p = 1.0;
    if (n >= 0) { for (long i = 0; i < n; i++)  p *= 2.0; }
    else        { for (long i = 0; i < -n; i++) p *= 0.5; }
    return x * p;
}

uint64_t kmath_op1(int op, uint64_t xbits) {
    kmfpu_begin();
    double x = b2d(xbits), r;
    switch (op) {
        case KM_SQRT:  r = __builtin_sqrt(x);            break;
        case KM_SIN:   r = m_sin(x);                     break;
        case KM_COS:   r = m_cos(x);                     break;
        case KM_TAN:   r = m_tan(x);                     break;
        case KM_EXP:   r = m_exp(x);                     break;
        case KM_EXP2:  r = m_exp(x * KM_LN2);            break;
        case KM_LOG:   r = m_log(x);                     break;
        case KM_LOG10: r = m_log(x) / KM_LN10;           break;
        case KM_LOG2:  r = m_log(x) / KM_LN2;            break;
        case KM_FLOOR: r = m_floor(x);                   break;
        case KM_CEIL:  r = m_ceil(x);                    break;
        case KM_TRUNC: r = m_trunc(x);                   break;
        case KM_ROUND: r = m_round(x);                   break;
        case KM_FABS:  r = m_fabs(x);                    break;
        case KM_ATAN:  r = m_atan(x);                    break;
        case KM_ASIN:  r = m_asin(x);                    break;
        case KM_ACOS:  r = KM_PI_2 - m_asin(x);          break;
        case KM_SINH:  { double e = m_exp(x); r = (e - 1.0 / e) * 0.5; } break;
        case KM_COSH:  { double e = m_exp(x); r = (e + 1.0 / e) * 0.5; } break;
        case KM_TANH:  { double e = m_exp(2.0 * x); r = (e - 1.0) / (e + 1.0); } break;
        case KM_CBRT:  r = m_cbrt(x);                    break;
        default:       r = KM_NAN;                       break;
    }
    /* `volatile` forces r's bits into an integer location BEFORE kmfpu_end's
     * fxrstor runs: fxrstor's inline asm has a "memory" clobber but does not mark
     * the xmm registers clobbered, so without this the compiler may keep r live in
     * an xmm reg across the fxrstor (which restores/overwrites it) and then read
     * garbage. (kfont avoids this by flushing results to memory inside its guard.) */
    volatile uint64_t rb = d2b(r);
    kmfpu_end();
    return rb;
}

uint64_t kmath_op2(int op, uint64_t xbits, uint64_t ybits) {
    kmfpu_begin();
    double x = b2d(xbits), y = b2d(ybits), r;
    switch (op) {
        case KM2_POW:   r = m_pow(x, y);                          break;
        case KM2_FMOD:  r = m_fmod(x, y);                         break;
        case KM2_ATAN2: r = m_atan2(x, y);                        break;
        case KM2_HYPOT: r = __builtin_sqrt(x * x + y * y);        break;
        default:        r = KM_NAN;                               break;
    }
    volatile uint64_t rb = d2b(r);   /* see kmath_op1: keep r out of xmm across fxrstor */
    kmfpu_end();
    return rb;
}

uint64_t kmath_ldexp(uint64_t xbits, long n) {
    kmfpu_begin();
    volatile uint64_t rb = d2b(m_ldexp(b2d(xbits), n));  /* see kmath_op1 */
    kmfpu_end();
    return rb;
}

/* C21: render a double for %f/%e/%g. Ported from libtoby's conv_float (the
 * native libc float formatter) but emits only the magnitude body + reports the
 * sign separately, so the kernel printf engine reuses its emit_field padding.
 * Like every entry point here it brackets the FP work in kmfpu_begin/end, and
 * all results (body chars, length, sign) are materialised to memory/integers
 * before kmfpu_end's fxrstor runs (see kmath_op1's note on the xmm clobber). */
int kmath_fmt_double(uint64_t bits, char conv, int prec, unsigned flags,
                     char *out, int outcap, int *sign_out, int *special_out) {
    kmfpu_begin();
    int n = 0, special = 0;
    char sign = 0;
    int upper = (conv >= 'A' && conv <= 'Z');
    char lc = (char)(conv | 0x20);              /* 'f' | 'e' | 'g' */
    int hash = (flags & KMF_HASH) != 0;
    int cap = (outcap > 8) ? outcap - 8 : 0;    /* leave slack for exponent tail */
    if (prec < 0) prec = 6;
    if (prec > 60) prec = 60;                   /* bound buffers; doubles show no more */

    double val = b2d(bits);
    uint64_t ab = bits & 0x7fffffffffffffffULL;
    if (bits >> 63)             { sign = '-'; val = -val; }
    else if (flags & KMF_PLUS)  sign = '+';
    else if (flags & KMF_SPACE) sign = ' ';

    if ((ab >> 52) == 0x7ffULL) {               /* Inf / NaN */
        int nan = (ab & 0xfffffffffffffULL) != 0;
        const char *t = nan ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        if (nan) sign = 0;                       /* NaN carries no sign */
        while (*t) out[n++] = *t++;
        special = 1;
    } else {
        int strip = 0;                           /* %g strips trailing zeros */
        if (lc == 'g') {
            int P = (prec == 0) ? 1 : prec;
            int X = 0;
            double t = val;
            if (t != 0.0) { while (t >= 10.0) { t /= 10.0; X++; }
                            while (t <  1.0) { t *= 10.0; X--; } }
            if (X < -4 || X >= P) { lc = 'e'; prec = P - 1; }
            else                  { lc = 'f'; prec = P - 1 - X; if (prec < 0) prec = 0; }
            if (!hash) strip = 1;
        }
        if (lc == 'f' && val >= 1.8e19) lc = 'e';    /* avoid u64 overflow */

        if (lc == 'e') {
            int X = 0;
            double m = val;
            if (m != 0.0) { while (m >= 10.0) { m /= 10.0; X++; }
                            while (m <  1.0) { m *= 10.0; X--; } }
            double scale = 1.0;
            for (int i = 0; i < prec; i++) scale *= 10.0;
            unsigned long long one = (unsigned long long)(scale + 0.5);
            unsigned long long md  = (unsigned long long)(m * scale + 0.5);
            if (md >= 10ULL * one) { md /= 10; X++; }   /* 9.99..->10.0 carry */
            char tb[80]; int tn = 0;
            unsigned long long q = md;
            for (int i = 0; i <= prec && tn < (int)sizeof(tb); i++) { tb[tn++] = (char)('0' + q % 10); q /= 10; }
            if (n < cap) out[n++] = tb[tn - 1];
            if (prec > 0 || hash) { if (n < cap) out[n++] = '.'; }
            for (int i = tn - 2; i >= 0 && n < cap; i--) out[n++] = tb[i];
            if (strip) { while (n > 0 && out[n-1] == '0') n--;
                         if (n > 0 && out[n-1] == '.') n--; }
            out[n++] = upper ? 'E' : 'e';
            out[n++] = (X < 0) ? '-' : '+';
            int ax = X < 0 ? -X : X;
            char eb[8]; int en = 0;
            do { eb[en++] = (char)('0' + ax % 10); ax /= 10; } while (ax && en < (int)sizeof(eb));
            while (en < 2) eb[en++] = '0';
            for (int i = en - 1; i >= 0; i--) out[n++] = eb[i];
        } else {                                  /* %f */
            double scale = 1.0;
            for (int i = 0; i < prec; i++) scale *= 10.0;
            unsigned long long ip  = (unsigned long long) val;
            double frac = val - (double) ip;
            unsigned long long one = (unsigned long long)(scale + 0.5);
            unsigned long long fd  = (unsigned long long)(frac * scale + 0.5);
            if (prec == 0) { if (frac >= 0.5) ip++; }
            else if (fd >= one) { fd -= one; ip++; }  /* fractional carry */
            char tb[24]; int tn = 0;
            unsigned long long q = ip;
            do { tb[tn++] = (char)('0' + q % 10); q /= 10; } while (q && tn < (int)sizeof(tb));
            for (int i = tn - 1; i >= 0 && n < cap; i--) out[n++] = tb[i];
            if (prec > 0 || hash) {
                if (n < cap) out[n++] = '.';
                char fb[80]; int fn = 0;
                unsigned long long fq = fd;
                for (int i = 0; i < prec && fn < (int)sizeof(fb); i++) { fb[fn++] = (char)('0' + fq % 10); fq /= 10; }
                for (int i = fn - 1; i >= 0 && n < cap; i--) out[n++] = fb[i];
                if (strip) { while (n > 0 && out[n-1] == '0') n--;
                             if (n > 0 && out[n-1] == '.') n--; }
            }
        }
    }

    *sign_out = sign;
    *special_out = special;
    kmfpu_end();
    return n;
}

uint64_t kmath_frexp(uint64_t xbits, int *e_out) {
    kmfpu_begin();
    double x = b2d(xbits);
    int e = 0;
    double m = x;
    /* finite non-zero only; 0/inf/nan keep e=0 and m=x (matches C frexp). */
    if (x != 0.0 && (x - x) == 0.0) {
        uint64_t b = d2b(x);
        int exp = (int)((b >> 52) & 0x7ff);
        if (exp == 0) {                         /* subnormal: normalize first */
            x *= 18014398509481984.0;           /* 2^54 */
            b = d2b(x);
            exp = (int)((b >> 52) & 0x7ff);
            e -= 54;
        }
        e += exp - 1022;                        /* so |m| in [0.5,1) */
        b = (b & ~(0x7ffULL << 52)) | ((uint64_t)1022 << 52);
        m = b2d(b);
    }
    *e_out = e;
    volatile uint64_t rb = d2b(m);              /* see kmath_op1 */
    kmfpu_end();
    return rb;
}
