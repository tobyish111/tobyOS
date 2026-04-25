/* libtoby/src/libtoby_internal.h -- shared internals for the libtoby
 * implementation files. NOT installed; consumers use the public
 * headers under libtoby/include/. */

#ifndef LIBTOBY_INTERNAL_H
#define LIBTOBY_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <tobyos/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Raw syscall trampolines (rax=number, args in rdi/rsi/rdx/r10/r8/r9).
 * Return the kernel's natural value, including negative errno codes. */
long toby_sc0(long n);
long toby_sc1(long n, long a);
long toby_sc2(long n, long a, long b);
long toby_sc3(long n, long a, long b, long c);
long toby_sc4(long n, long a, long b, long c, long d);
long toby_sc5(long n, long a, long b, long c, long d, long e);
long toby_sc6(long n, long a, long b, long c, long d, long e, long f);

/* If rv looks like a -ABI_E* code (rv in [-4095, -1]), set errno and
 * return -1. Otherwise return rv unchanged. */
long __toby_check(long rv);

/* Initialised by crt0.S before main is called. */
extern int    __toby_argc;
extern char **__toby_argv;
extern char **__toby_envp;

/* Implemented in init.c. Idempotent (safe to call from any libc
 * entry point that needs the heap or stdio FILEs ready). */
void __libtoby_init(int argc, char **argv, char **envp);

/* Implemented in stdio.c. The internal "raw write to fd" path used
 * by printf so we don't recurse through the FILE * layer. */
long __toby_raw_write(int fd, const void *buf, size_t n);

/* Process-wide list of atexit handlers. */
void __toby_run_atexit(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_INTERNAL_H */
