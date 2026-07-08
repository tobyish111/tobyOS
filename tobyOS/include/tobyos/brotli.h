/* brotli.h -- kernel-side brotli (RFC 7932) decode for Content-Encoding:
 * br (and, later, WOFF2 fonts). Thin wrapper over the vendored brotli
 * v1.0.9 reference decoder (third_party/brotli, amalgamated in
 * src/brotli.c). Same in/out convention as puff (see puff.h). */
#ifndef TOBYOS_BROTLI_H
#define TOBYOS_BROTLI_H

#include <tobyos/types.h>

#define BROTLI_OK     0   /* fully decompressed */
#define BROTLI_TRUNC  1   /* output buffer filled; *destlen holds the prefix */
#define BROTLI_ERR   -1   /* malformed / truncated / unsupported stream */

/* Decode a raw brotli stream into dest. On entry *destlen is the
 * capacity; on return it holds the bytes written (the decompressed
 * prefix on BROTLI_TRUNC). */
int brotli_decompress(uint8_t *dest, unsigned long *destlen,
                      const uint8_t *src, unsigned long srclen);

#endif /* TOBYOS_BROTLI_H */
