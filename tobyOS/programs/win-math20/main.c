/* win-math20/main.c -- Track C/C20: proves the float-return marshalling gate.
 *
 * Calls the REAL ucrt libm (sqrt/sin/cos/pow/exp/log/...), each of which returns
 * a `double` in xmm0. Built -fno-builtin so the compiler emits genuine imported
 * calls (not an inlined sqrtsd / constant fold), and the inputs are `volatile`
 * so every result is computed at runtime through the kernel's float-return gate
 * + kmath libm. Each result is checked against its known value within a
 * tolerance using only integer/relational ops (NO float printing, which is a
 * separate gap), so exit 20 is reachable only if doubles round-tripped
 * arg -> xmm -> kernel -> xmm0 correctly. A failing check returns 100+index. */
#include <stdint.h>

extern __declspec(dllimport) double sqrt(double);
extern __declspec(dllimport) double sin(double);
extern __declspec(dllimport) double cos(double);
extern __declspec(dllimport) double tan(double);
extern __declspec(dllimport) double exp(double);
extern __declspec(dllimport) double exp2(double);
extern __declspec(dllimport) double log(double);
extern __declspec(dllimport) double log10(double);
extern __declspec(dllimport) double pow(double, double);
extern __declspec(dllimport) double floor(double);
extern __declspec(dllimport) double ceil(double);
extern __declspec(dllimport) double fmod(double, double);
extern __declspec(dllimport) double fabs(double);
extern __declspec(dllimport) double atan2(double, double);
extern __declspec(dllimport) double ldexp(double, int);

static double myabs(double x) { return x < 0 ? -x : x; }
static int    close_to(double got, double want, double tol) { return myabs(got - want) <= tol; }

/* volatile so the compiler cannot constant-fold any of the calls away. */
static volatile double two   = 2.0;
static volatile double ten   = 10.0;
static volatile double three = 3.0;
static volatile double one   = 1.0;
static volatile double half_pi6 = 0.52359877559829887308;  /* pi/6  -> sin = 0.5 */
static volatile double pi3   = 1.04719755119659774615;     /* pi/3  -> cos = 0.5 */
static volatile double pi4   = 0.78539816339744830961;     /* pi/4  -> tan = 1   */
static volatile double negv  = -5.5;
static volatile double f37   = 3.7;
static volatile double f32   = 3.2;
static volatile double v15   = 1.5;

int main(void) {
    const double T = 1e-6;
    int i = 0;

    i++; if (!close_to(sqrt(two), 1.41421356237309504880, T)) return 100 + i;   /* 1  */
    i++; if (!close_to(pow(two, ten), 1024.0, 1e-3))          return 100 + i;   /* 2  */
    i++; if (!close_to(sin(half_pi6), 0.5, T))                return 100 + i;   /* 3  */
    i++; if (!close_to(cos(pi3), 0.5, T))                     return 100 + i;   /* 4  */
    i++; if (!close_to(tan(pi4), 1.0, 1e-5))                  return 100 + i;   /* 5  */
    i++; if (!close_to(exp(one), 2.71828182845904523536, T)) return 100 + i;    /* 6  */
    i++; if (!close_to(log(exp(one)), 1.0, T))                return 100 + i;   /* 7  */
    i++; if (!close_to(log10(pow(ten, three)), 3.0, 1e-5))    return 100 + i;   /* 8  */
    i++; if (!close_to(exp2(ten), 1024.0, 1e-6))              return 100 + i;   /* 9  */
    i++; if (!close_to(floor(f37), 3.0, 0))                   return 100 + i;   /* 10 */
    i++; if (!close_to(ceil(f32), 4.0, 0))                    return 100 + i;   /* 11 */
    i++; if (!close_to(fmod(ten, three), 1.0, T))             return 100 + i;   /* 12 */
    i++; if (!close_to(fabs(negv), 5.5, 0))                   return 100 + i;   /* 13 */
    i++; if (!close_to(atan2(one, one), 0.78539816339744830961, T)) return 100 + i; /* 14 */
    i++; if (!close_to(ldexp(v15, 3), 12.0, 0))               return 100 + i;   /* 15 */

    return 20;   /* all float round-trips correct */
}
