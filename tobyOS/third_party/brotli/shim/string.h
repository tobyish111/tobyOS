/* Freestanding <string.h> shim for the vendored brotli decoder: the
 * kernel has no hosted libc, so map the mem* functions brotli needs
 * (memcpy/memmove/memset/memcmp) onto the kernel's klibc. On the brotli
 * include path only (see the src/brotli.o make rule). */
#ifndef TOBYOS_BROTLI_SHIM_STRING_H
#define TOBYOS_BROTLI_SHIM_STRING_H
#include <tobyos/klibc.h>
#endif
