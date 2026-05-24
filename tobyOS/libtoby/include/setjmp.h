/* setjmp.h -- libtoby's non-local jump facility. */

#ifndef LIBTOBY_SETJMP_H
#define LIBTOBY_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Saves rbx, rbp, r12, r13, r14, r15, rsp, rip (8 * 8 = 64 bytes). */
typedef long jmp_buf[8];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_SETJMP_H */
