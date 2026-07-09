/* Freestanding <string.h> shim for the vendored BearSSL X.509/crypto
 * subset: the kernel has no hosted libc. BearSSL uses only
 * memcpy/memmove/memset/memcmp, which the kernel's klibc provides.
 * On the BearSSL include path only (see the src/bearssl.o make rule). */
#ifndef TOBYOS_BEARSSL_SHIM_STRING_H
#define TOBYOS_BEARSSL_SHIM_STRING_H
#include <tobyos/klibc.h>
#endif
