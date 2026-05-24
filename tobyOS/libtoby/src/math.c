/* libtoby/src/math.c -- software floating-point math library.
 *
 * Pure C implementations: Taylor series for trig/exp/log, Newton's
 * method for sqrt, argument reduction for range. No SSE intrinsics;
 * the compiler's x87 codegen handles double arithmetic.
 *
 * Accuracy goal: ~1 ULP for common inputs. Not IEEE-754 correctly
 * rounded everywhere, but good enough for the userland programs
 * tobyOS ships. */

#include <math.h>
#include <stdint.h>

/* ---- helpers ---------------------------------------------------- */

typedef union {
    double   d;
    uint64_t u;
} dbl_bits;

static double _fabs(double x) { return x < 0.0 ? -x : x; }

/* ---- floor / ceil / trunc / round ------------------------------- */

double trunc(double x) {
    if (x != x) return x;          /* NaN */
    if (x >= 9007199254740992.0 || x <= -9007199254740992.0) return x;
    return (double)(long long)x;
}

double floor(double x) {
    double t = trunc(x);
    if (x < t) return t - 1.0;
    return t;
}

double ceil(double x) {
    double t = trunc(x);
    if (x > t) return t + 1.0;
    return t;
}

double round(double x) {
    if (x >= 0.0) return floor(x + 0.5);
    return ceil(x - 0.5);
}

double fabs(double x) { return _fabs(x); }

double fmod(double x, double y) {
    if (y == 0.0 || x != x || y != y) return NAN;
    if (isinf(x)) return NAN;
    if (isinf(y)) return x;
    double q = trunc(x / y);
    double r = x - q * y;
    return r;
}

double remainder(double x, double y) {
    if (y == 0.0) return NAN;
    double q = round(x / y);
    return x - q * y;
}

double copysign(double x, double y) {
    dbl_bits bx, by;
    bx.d = x;
    by.d = y;
    bx.u = (bx.u & 0x7FFFFFFFFFFFFFFFULL) | (by.u & 0x8000000000000000ULL);
    return bx.d;
}

double modf(double x, double *iptr) {
    double t = trunc(x);
    if (iptr) *iptr = t;
    return x - t;
}

/* ---- frexp / ldexp ---------------------------------------------- */

double frexp(double x, int *exp) {
    if (x == 0.0) { if (exp) *exp = 0; return 0.0; }
    if (x != x || isinf(x)) { if (exp) *exp = 0; return x; }
    dbl_bits b;
    b.d = x;
    int e = (int)((b.u >> 52) & 0x7FF) - 1022;
    if (exp) *exp = e;
    b.u = (b.u & 0x800FFFFFFFFFFFFFULL) | 0x3FE0000000000000ULL;
    return b.d;
}

double ldexp(double x, int exp) {
    if (x == 0.0 || x != x || isinf(x)) return x;
    while (exp > 1023) { x *= 8.98846567431158e+307; exp -= 1023; }
    while (exp < -1022) { x *= 1.1125369292536007e-308; exp += 1022; }
    dbl_bits b;
    b.u = (uint64_t)(exp + 1023) << 52;
    return x * b.d;
}

/* ---- sqrt (Newton-Raphson with bit-level seed) ------------------- */

double sqrt(double x) {
    if (x < 0.0) return NAN;
    if (x == 0.0 || x != x || isinf(x)) return x;

    dbl_bits b;
    b.d = x;
    b.u = (b.u >> 1) + 0x1FF8000000000000ULL;
    double g = b.d;

    for (int i = 0; i < 8; i++) {
        g = 0.5 * (g + x / g);
    }
    return g;
}

double cbrt(double x) {
    if (x == 0.0 || x != x) return x;
    int neg = (x < 0.0);
    double a = neg ? -x : x;
    double g = a;
    for (int i = 0; i < 60; i++) {
        g = (2.0 * g + a / (g * g)) / 3.0;
    }
    return neg ? -g : g;
}

/* ---- exp (Taylor series with range reduction) ------------------- */

double exp(double x) {
    if (x != x) return NAN;
    if (x > 709.0) return INFINITY;
    if (x < -745.0) return 0.0;

    /* Reduce: e^x = 2^k * e^r where r = x - k*ln(2), |r| <= ln(2)/2 */
    double k = floor(x / M_LN2 + 0.5);
    double r = x - k * M_LN2;
    int ki = (int)k;

    /* Taylor: e^r = 1 + r + r^2/2! + ... */
    double term = 1.0;
    double sum = 1.0;
    for (int i = 1; i < 30; i++) {
        term *= r / (double)i;
        sum += term;
        if (_fabs(term) < 1e-17 * _fabs(sum)) break;
    }

    return ldexp(sum, ki);
}

/* ---- log (natural logarithm) ------------------------------------ */

double log(double x) {
    if (x < 0.0) return NAN;
    if (x == 0.0) return -INFINITY;
    if (x != x) return NAN;
    if (isinf(x)) return INFINITY;

    /* Decompose: x = m * 2^e where 0.5 <= m < 1.0
     * log(x) = log(m) + e*log(2)
     * Let f = (m - 1)/(m + 1), then log(m) = 2*(f + f^3/3 + f^5/5 + ...) */
    int e;
    double m = frexp(x, &e);

    /* Shift m to [sqrt(2)/2, sqrt(2)] for better convergence */
    if (m < M_SQRT2 * 0.5) { m *= 2.0; e--; }

    double f = (m - 1.0) / (m + 1.0);
    double f2 = f * f;
    double sum = 0.0;
    double term = f;
    for (int i = 0; i < 30; i++) {
        sum += term / (double)(2 * i + 1);
        term *= f2;
        if (_fabs(term) < 1e-17) break;
    }

    return 2.0 * sum + (double)e * M_LN2;
}

double log10(double x) { return log(x) * M_LOG10E; }
double log2(double x)  { return log(x) * M_LOG2E; }

/* ---- pow -------------------------------------------------------- */

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (x == 0.0) {
        if (y > 0.0) return 0.0;
        return INFINITY;
    }
    if (y != y || x != x) return NAN;

    /* Integer exponent fast path */
    if (y == floor(y) && _fabs(y) < 1e15) {
        long long n = (long long)y;
        int neg = 0;
        if (n < 0) { neg = 1; n = -n; }
        double result = 1.0;
        double base = x;
        while (n > 0) {
            if (n & 1) result *= base;
            base *= base;
            n >>= 1;
        }
        return neg ? 1.0 / result : result;
    }

    if (x < 0.0) return NAN;
    return exp(y * log(x));
}

/* ---- sin / cos (argument reduction + Taylor) -------------------- */

/* Reduce angle to [-pi, pi] */
static double reduce_angle(double x) {
    if (_fabs(x) <= M_PI) return x;
    x = fmod(x, 2.0 * M_PI);
    if (x > M_PI) x -= 2.0 * M_PI;
    if (x < -M_PI) x += 2.0 * M_PI;
    return x;
}

/* Core sine via Taylor: x - x^3/3! + x^5/5! - ...
 * Input should be in [-pi/2, pi/2] for best accuracy. */
static double sin_taylor(double x) {
    double x2 = x * x;
    double term = x;
    double sum = x;
    for (int i = 1; i < 16; i++) {
        term *= -x2 / (double)(2 * i * (2 * i + 1));
        sum += term;
        if (_fabs(term) < 1e-17) break;
    }
    return sum;
}

double sin(double x) {
    if (x != x) return NAN;
    x = reduce_angle(x);

    /* Reduce to [-pi/2, pi/2] */
    if (x > M_PI_2) return sin_taylor(M_PI - x);
    if (x < -M_PI_2) return -sin_taylor(M_PI + x);
    return sin_taylor(x);
}

double cos(double x) {
    return sin(x + M_PI_2);
}

double tan(double x) {
    double c = cos(x);
    if (_fabs(c) < 1e-300) return copysign(INFINITY, sin(x));
    return sin(x) / c;
}

/* ---- inverse trig ----------------------------------------------- */

/* atan via polynomial approximation for |x| <= 1, identity reduction
 * for |x| > 1: atan(x) = pi/2 - atan(1/x) for x > 1. */
double atan(double x) {
    if (x != x) return NAN;
    if (isinf(x)) return copysign(M_PI_2, x);

    int neg = (x < 0.0);
    if (neg) x = -x;

    int invert = 0;
    if (x > 1.0) { x = 1.0 / x; invert = 1; }

    /* Taylor: atan(x) = x - x^3/3 + x^5/5 - x^7/7 + ... for |x| <= 1 */
    double x2 = x * x;
    double term = x;
    double sum = x;
    for (int i = 1; i < 50; i++) {
        term *= -x2;
        sum += term / (double)(2 * i + 1);
        if (_fabs(term) < 1e-17) break;
    }

    if (invert) sum = M_PI_2 - sum;
    return neg ? -sum : sum;
}

double atan2(double y, double x) {
    if (y != y || x != x) return NAN;
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) {
        if (y >= 0.0) return atan(y / x) + M_PI;
        return atan(y / x) - M_PI;
    }
    /* x == 0 */
    if (y > 0.0) return M_PI_2;
    if (y < 0.0) return -M_PI_2;
    return 0.0;
}

double asin(double x) {
    if (x < -1.0 || x > 1.0 || x != x) return NAN;
    if (x == 1.0) return M_PI_2;
    if (x == -1.0) return -M_PI_2;
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    if (x < -1.0 || x > 1.0 || x != x) return NAN;
    return M_PI_2 - asin(x);
}

/* ---- float variants --------------------------------------------- */

float sinf(float x)  { return (float)sin((double)x); }
float cosf(float x)  { return (float)cos((double)x); }
float sqrtf(float x) { return (float)sqrt((double)x); }
float fabsf(float x) { return x < 0.0f ? -x : x; }
float floorf(float x) { return (float)floor((double)x); }
float ceilf(float x)  { return (float)ceil((double)x); }
float roundf(float x) { return (float)round((double)x); }
float truncf(float x) { return (float)trunc((double)x); }
float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }
float powf(float x, float y)  { return (float)pow((double)x, (double)y); }
float expf(float x)   { return (float)exp((double)x); }
float logf(float x)   { return (float)log((double)x); }
