/* kmath.h -- kernel-side libm for the Win32 float-return shims (Track C / C20).
 *
 * The Win32 CRT math functions (sqrt/sin/cos/pow/...) return a `double` in xmm0.
 * The kernel is otherwise -mno-sse, so the actual FP runs in kmath.c (compiled
 * -msse, bracketed by an FXSAVE/FXRSTOR guard exactly like kfont.c). The shim
 * layer (syscall.c) is -mno-sse and therefore CANNOT pass or return a `double`;
 * it exchanges values with this unit as raw IEEE-754 bit patterns (uint64_t),
 * which travel safely in integer registers. */
#ifndef TOBYOS_KMATH_H
#define TOBYOS_KMATH_H

#include <stdint.h>

/* One-argument double->double operations. */
enum {
    KM_SQRT = 0, KM_SIN, KM_COS, KM_TAN, KM_EXP, KM_EXP2, KM_LOG, KM_LOG10,
    KM_LOG2, KM_FLOOR, KM_CEIL, KM_TRUNC, KM_ROUND, KM_FABS, KM_ATAN, KM_ASIN,
    KM_ACOS, KM_SINH, KM_COSH, KM_TANH, KM_CBRT
};
/* Two-argument double,double->double operations. */
enum { KM2_POW = 0, KM2_FMOD, KM2_ATAN2, KM2_HYPOT };

/* xbits/ybits are the bit patterns of the input doubles; the return is the bit
 * pattern of the result double. */
uint64_t kmath_op1(int op, uint64_t xbits);
uint64_t kmath_op2(int op, uint64_t xbits, uint64_t ybits);
uint64_t kmath_ldexp(uint64_t xbits, long n);   /* ldexp(x, int) -- mixed args */
/* frexp(x, int *e): returns the mantissa bits (|m| in [0.5,1)) and stores the
 * binary exponent through e_out (a KERNEL pointer the shim copies to user). */
uint64_t kmath_frexp(uint64_t xbits, int *e_out);

/* C21: float PRINTING for the Win32 printf engine (win32_vformat). That engine
 * is -mno-sse, so the double->decimal conversion (which needs FP) lives here.
 * Renders the MAGNITUDE of the double `bits` for a %f/%e/%g conversion (and the
 * upper-case variants) into out[] -- NO sign and NO field padding, both of which
 * the engine applies via emit_field exactly like the integer conversions. The
 * sign char ('-','+',' ' or 0) is reported through *sign_out and inf/nan through
 * *special_out (the engine suppresses zero-padding for specials). prec<0 means
 * the default (6). Returns the body length. flags is a bitmask of KMF_*. */
#define KMF_PLUS  1u    /* '+' flag: force a leading sign on non-negative values */
#define KMF_SPACE 2u    /* ' ' flag: leading space on non-negative values        */
#define KMF_HASH  4u    /* '#' flag: keep the '.' and (for %g) trailing zeros     */
int kmath_fmt_double(uint64_t bits, char conv, int prec, unsigned flags,
                     char *out, int outcap, int *sign_out, int *special_out);

#endif
