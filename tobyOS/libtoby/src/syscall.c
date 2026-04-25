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

long toby_sc0(long n) {
    long r;
    __asm__ volatile ("syscall"
                      : "=a"(r) : "0"(n) : "rcx", "r11", "memory");
    return r;
}
long toby_sc1(long n, long a) {
    long r;
    __asm__ volatile ("syscall"
                      : "=a"(r) : "0"(n), "D"(a) : "rcx", "r11", "memory");
    return r;
}
long toby_sc2(long n, long a, long b) {
    long r;
    __asm__ volatile ("syscall"
                      : "=a"(r) : "0"(n), "D"(a), "S"(b)
                      : "rcx", "r11", "memory");
    return r;
}
long toby_sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall"
                      : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return r;
}
long toby_sc4(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                      : "rcx", "r11", "memory");
    return r;
}
long toby_sc5(long n, long a, long b, long c, long d, long e) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                      : "rcx", "r11", "memory");
    return r;
}
long toby_sc6(long n, long a, long b, long c, long d, long e, long f) {
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
