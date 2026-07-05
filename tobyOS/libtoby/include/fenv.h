/* libtoby/include/fenv.h -- minimal floating-point environment stub.
 * tobyOS user programs run with the default SSE MXCSR (round-to-
 * nearest); nothing here actually changes hardware state. Added for
 * the QuickJS port (it includes <fenv.h> but never calls fe*() on our
 * build paths). */

#ifndef _FENV_H
#define _FENV_H

#define FE_TONEAREST  0
#define FE_DOWNWARD   1
#define FE_UPWARD     2
#define FE_TOWARDZERO 3

static inline int fegetround(void) { return FE_TONEAREST; }
static inline int fesetround(int mode) { (void)mode; return 0; }

#endif /* _FENV_H */
