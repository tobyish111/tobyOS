/* math.h -- libtoby's software floating-point math library.
 *
 * All functions are pure software implementations using Taylor series,
 * polynomial approximations, and Newton's method. No FPU/SSE required
 * at the call sites (the compiler may still use x87 for intermediate
 * double arithmetic -- that's fine, x87 is always available on x86_64). */

#ifndef LIBTOBY_MATH_H
#define LIBTOBY_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_E         2.71828182845904523536
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_LOG2E     1.44269504088896340736
#define M_LOG10E    0.43429448190325182765
#define M_SQRT2     1.41421356237309504880

#define HUGE_VAL    __builtin_huge_val()
#define INFINITY    __builtin_inf()
#define NAN         __builtin_nan("")

#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)

#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, \
    FP_NORMAL, FP_SUBNORMAL, FP_ZERO, (x))

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

double sin(double x);
double cos(double x);
double tan(double x);

double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

double sqrt(double x);
double cbrt(double x);

double pow(double x, double y);
double exp(double x);
double log(double x);
double log10(double x);
double log2(double x);

double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);

double fabs(double x);
double fmod(double x, double y);
double remainder(double x, double y);

double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *iptr);
double copysign(double x, double y);

/* M-JS additions (QuickJS Math object) */
double fmin(double a, double b);
double fmax(double a, double b);
double hypot(double x, double y);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
double expm1(double x);
double log1p(double x);
double rint(double x);
double nearbyint(double x);
long   lrint(double x);

float  sinf(float x);
float  cosf(float x);
float  sqrtf(float x);
float  fabsf(float x);
float  floorf(float x);
float  ceilf(float x);
float  roundf(float x);
float  truncf(float x);
float  fmodf(float x, float y);
float  powf(float x, float y);
float  expf(float x);
float  logf(float x);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_MATH_H */
