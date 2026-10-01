/* libtoby/src/syscall.c -- raw syscall trampolines + errno conversion.
 *
 * One internal helper per arity (sc0..sc6). They invoke the kernel
 * SYSCALL instruction with the tobyOS calling convention specified in
 * <tobyos/abi/abi.h> (rax=number; rdi/rsi/rdx/r10/r8/r9=args; rax=ret).
 *
 * On top of those we expose `__toby_syscall_errno` which translates
 * the kernel's negative -ABI_E* convention into POSIX-style errno
 * setting + -1 return. Higher-level wrappers (in unistd.c, stdio.c,
 * etc.) call this helper so the errno bookkeeping is centralised.
 *
 * The "raw" sc* helpers are also exposed (with the toby_sc* prefix)
 * for the rare wrapper that needs the kernel's natural return value
 * unfiltered (e.g. SYS_BRK queries return the current break). */

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include "libtoby_internal.h"

/* ---- raw syscall trampolines ------------------------------------ */

static inline long raw_sc6(long n, long a, long b, long c, long d, long e,
                           long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return r;
}

/* PENDING STDIO OUTPUT GOES OUT BEFORE THE PROCESS ENTERS THE KERNEL (K5).
 * stdio buffers now (see stdio.c), and the native userland was written
 * against unbuffered stdio: it mixes printf with raw write(2), prints and
 * then forks, spawns, sleeps, waits or reads. Flushing here -- one load of
 * __toby_stdio_pending when nothing is buffered -- keeps every byte a
 * program printed ahead of every effect it causes afterwards, exactly as
 * before; only runs of stdio calls with no syscall between them are
 * coalesced. exit() and _exit() are syscalls too, so nothing printed is
 * lost at exit. */
#define STDIO_SYNC() \
    do { if (__builtin_expect(__toby_stdio_pending, 0)) __toby_stdio_flush_all(); } while (0)

long toby_sc_raw0(long n)                         { return raw_sc6(n, 0, 0, 0, 0, 0, 0); }
long toby_sc_raw3(long n, long a, long b, long c) { return raw_sc6(n, a, b, c, 0, 0, 0); }

long toby_sc0(long n) {
    STDIO_SYNC(); return raw_sc6(n, 0, 0, 0, 0, 0, 0);
}
long toby_sc1(long n, long a) {
    STDIO_SYNC(); return raw_sc6(n, a, 0, 0, 0, 0, 0);
}
long toby_sc2(long n, long a, long b) {
    STDIO_SYNC(); return raw_sc6(n, a, b, 0, 0, 0, 0);
}
long toby_sc3(long n, long a, long b, long c) {
    STDIO_SYNC(); return raw_sc6(n, a, b, c, 0, 0, 0);
}
long toby_sc4(long n, long a, long b, long c, long d) {
    STDIO_SYNC(); return raw_sc6(n, a, b, c, d, 0, 0);
}
long toby_sc5(long n, long a, long b, long c, long d, long e) {
    STDIO_SYNC(); return raw_sc6(n, a, b, c, d, e, 0);
}
long toby_sc6(long n, long a, long b, long c, long d, long e, long f) {
    STDIO_SYNC(); return raw_sc6(n, a, b, c, d, e, f);
}

/* ---- POSIX-style return convention ------------------------------- *
 *
 * The kernel returns a single signed long: the natural success result
 * (>=0) or a negated -ABI_E* code (between -1 and -4095). Wrappers
 * in libtoby's POSIX surface translate that to "set errno + return -1
 * on error, return the natural value otherwise" via this helper. */

long __toby_check(long rv) {
    if ((unsigned long)rv > (unsigned long)-4096L) {
        errno = (int)(-rv);
        return -1;
    }
    return rv;
}
