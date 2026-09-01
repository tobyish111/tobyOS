/* deflate.h -- gzip (RFC 1952) compression.
 *
 * The DECOMPRESSION side already existed: src/puff.c has been inflating
 * Content-Encoding: gzip for the browser for a long time, and userland
 * compiles that same source rather than carrying a second copy. Only the
 * compressor was missing, so this is the other half.
 */
#ifndef TOBY_DEFLATE_H
#define TOBY_DEFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Compress `in` into a complete gzip stream. On success returns 0 and sets
 * *out to a malloc'd buffer the caller frees, with *outlen its length.
 * `level` 1..9 tunes how hard the matcher looks (it does not change the
 * format). `name`, when non-NULL, is stored in the header as gzip does for
 * a named input file; `mtime` is stored as-is (0 means "not available",
 * which is what RFC 1952 says). Returns -1 on allocation failure. */
int gz_compress(const uint8_t *in, size_t inlen, int level,
                const char *name, uint32_t mtime,
                uint8_t **out, size_t *outlen);

/* CRC-32 (RFC 1952 / IEEE 802.3). Seed 0 for a fresh run. */
uint32_t gz_crc32(const uint8_t *p, size_t n, uint32_t seed);

/* Read the original size out of a gzip stream's trailer. That field is the
 * length modulo 2^32, so it is a starting point, not a guarantee -- callers
 * that must handle >4 GiB grow past it instead of trusting it. Returns 0 if
 * the buffer is too short to hold a trailer. */
uint32_t gz_isize(const uint8_t *p, size_t n);

#endif /* TOBY_DEFLATE_H */
