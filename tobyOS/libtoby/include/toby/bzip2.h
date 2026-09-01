/* bzip2.h -- bzip2 compression and decompression
 * (see libtoby/src/bzip2.c). */
#ifndef TOBY_BZIP2_H
#define TOBY_BZIP2_H

#include <stddef.h>
#include <stdint.h>

/* Decompress a complete bzip2 stream. On success returns 0 and sets *out
 * to a malloc'd buffer the caller frees, with *outlen its length. On
 * failure returns -1 and, when `why` is non-NULL, points *why at a short
 * static string saying what was wrong -- so the caller can report the
 * reason rather than a bare "failed". */
int bz_decompress(const uint8_t *in, size_t inlen,
                  uint8_t **out, size_t *outlen, const char **why);


/* Compress into a complete bzip2 stream. `level` 1..9 selects the block
 * size in units of 100k, exactly as the bzip2 command line does. Returns 0
 * and fills *out (malloc'd, caller frees) / *outlen, or -1 with *why set. */
int bz_compress(const uint8_t *in, size_t inlen, int level,
                uint8_t **out, size_t *outlen, const char **why);

#endif /* TOBY_BZIP2_H */
