/* cxxrt.cpp -- the freestanding C++ runtime for tobyOS userland
 * (stage 13). Everything a -fno-exceptions -fno-rtti C++ program
 * needs beyond the C library:
 *
 *   - operator new / new[] / delete / delete[] (plain, sized, and
 *     nothrow) over the libtoby heap. Allocation failure calls
 *     abort() -- with exceptions disabled there is nothing to throw,
 *     and a NULL return from plain new is UB for the caller anyway.
 *   - __cxa_pure_virtual: a pure-virtual call is a program bug; trap
 *     it loudly instead of jumping to 0.
 *   - __cxa_guard_acquire/release/abort: function-local static
 *     initialization guards. Single-threaded semantics with the
 *     Itanium ABI guard layout (first byte = initialized flag);
 *     tobyOS user processes are single-threaded per address space
 *     today, matching the rest of libtoby (malloc is not locked
 *     either).
 *   - __cxa_atexit/__dso_handle: static-object destructors, drained
 *     in reverse registration order through one atexit() hook.
 *
 * Global constructors themselves run from __libtoby_init (init.c)
 * via the weak __init_array bounds; a C++ program's linker script
 * must define them (see programs/user_cpptest/program.ld).
 */

#include <stddef.h>
#include <stdlib.h>
#include <new>

namespace std { const nothrow_t nothrow{}; }

/* ---- new / delete ------------------------------------------------ */

void *operator new(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) abort();
    return p;
}

void *operator new[](size_t n) {
    return operator new(n);
}

void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

/* nothrow variants (declared in <new>). */
void *operator new(size_t n, const std::nothrow_t &) noexcept {
    return malloc(n ? n : 1);
}
void *operator new[](size_t n, const std::nothrow_t &) noexcept {
    return malloc(n ? n : 1);
}
void operator delete(void *p, const std::nothrow_t &) noexcept { free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { free(p); }

/* ---- Itanium C++ ABI hooks --------------------------------------- */

extern "C" {

void *__dso_handle = &__dso_handle;

void __cxa_pure_virtual(void) {
    abort();
}

/* Function-local static guards. Guard object: 64 bits, byte 0 is the
 * "initialized" flag (Itanium ABI). Single-threaded: no blocking. */
int __cxa_guard_acquire(unsigned long long *g) {
    return *(volatile unsigned char *)g == 0;
}

void __cxa_guard_release(unsigned long long *g) {
    *(volatile unsigned char *)g = 1;
}

void __cxa_guard_abort(unsigned long long *g) {
    (void)g;                        /* initializer failed; retry later */
}

/* Static-object destructors. Bounded table (a process registers one
 * per static object with a destructor); drained newest-first by an
 * atexit hook installed on first registration. */
#define CXA_ATEXIT_MAX 64

struct cxa_dtor {
    void (*fn)(void *);
    void *arg;
};

static struct cxa_dtor g_cxa_dtors[CXA_ATEXIT_MAX];
static int g_cxa_ndtors;
static int g_cxa_hooked;

static void cxa_run_dtors(void) {
    while (g_cxa_ndtors > 0) {
        struct cxa_dtor d = g_cxa_dtors[--g_cxa_ndtors];
        if (d.fn) d.fn(d.arg);
    }
}

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
    (void)dso;
    if (g_cxa_ndtors >= CXA_ATEXIT_MAX) return -1;
    if (!g_cxa_hooked) {
        g_cxa_hooked = 1;
        atexit(cxa_run_dtors);
    }
    g_cxa_dtors[g_cxa_ndtors].fn = fn;
    g_cxa_dtors[g_cxa_ndtors].arg = arg;
    g_cxa_ndtors++;
    return 0;
}

} /* extern "C" */
