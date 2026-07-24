/* Freestanding <string.h> shim for the kyber reference sources (they include
 * <string.h> for memcpy/memset). Only used by the -nostdlib tobyOS build
 * (via -I this dir); the host reference build resolves the real header.
 * Implementations live in main.c. */
#ifndef MLKEM_SHIM_STRING_H
#define MLKEM_SHIM_STRING_H
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
#endif
