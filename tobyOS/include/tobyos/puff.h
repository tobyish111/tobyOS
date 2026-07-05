/* puff.h -- compact DEFLATE/gzip/zlib inflate (see src/puff.c). */
#ifndef TOBYOS_PUFF_H
#define TOBYOS_PUFF_H

#include <tobyos/types.h>

#define PUFF_OK     0   /* fully decompressed */
#define PUFF_TRUNC  1   /* output buffer filled; *destlen holds the prefix */
#define PUFF_ERR   -1   /* malformed / unsupported stream */

/* Inflate a gzip (RFC 1952) stream into dest. On entry *destlen is the
 * capacity; on return it holds the bytes written. */
int puff_gzip(uint8_t *dest, unsigned long *destlen,
              const uint8_t *src, unsigned long srclen);

/* Inflate a zlib (RFC 1950) stream. Same in/out convention. */
int puff_zlib(uint8_t *dest, unsigned long *destlen,
              const uint8_t *src, unsigned long srclen);

#endif /* TOBYOS_PUFF_H */
