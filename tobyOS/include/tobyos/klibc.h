/* klibc.h -- minimal freestanding libc subset.
 *
 * The compiler is allowed to emit implicit calls to memcpy/memset/memmove/
 * memcmp even with -ffreestanding (e.g. for struct copies), so we MUST
 * provide them with C linkage and the standard names.
 */

#ifndef TOBYOS_KLIBC_H
#define TOBYOS_KLIBC_H

#include <tobyos/types.h>
#include <stdarg.h>

void   *memcpy (void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
void   *memset (void *dst, int c, size_t n);
int     memcmp (const void *a, const void *b, size_t n);

size_t  strlen (const char *s);
int     strcmp (const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);

/* Tiny snprintf for diagnostic strings (M26 devtest, drivers).
 *
 * Supports the exact same conversions as kprintf:
 *   %s %c %d %u %lu %ld %x %lx %p %%
 * Plus zero-pad / width as in "%08x".
 *
 * Always NUL-terminates `dst` if cap > 0. Returns the number of bytes
 * that WOULD have been written (excluding NUL), so callers can detect
 * truncation via `(rv >= cap)` — same semantics as ISO C snprintf. */
int     ksnprintf (char *dst, size_t cap, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));
int     kvsnprintf(char *dst, size_t cap, const char *fmt, va_list ap);

#endif /* TOBYOS_KLIBC_H */
