/* zstd.h -- Zstandard (RFC 8878). See libtoby/src/zstd.c.
 *
 * Decompression is complete. COMPRESSION EMITS RAW BLOCKS ONLY: a valid
 * .zst frame any zstd decodes, but not actually compressed. gzip and
 * bzip2 in this tree are both real, so anything wanting a smaller file
 * has two working choices.
 */
#ifndef TOBY_ZSTD_H
#define TOBY_ZSTD_H

#include <stddef.h>
#include <stdint.h>

/* Returns 0 and fills *out (malloc'd, caller frees) / *outlen, or -1 with
 * *why pointing at a short static string saying what was wrong. */
int zstd_decompress(const uint8_t *in, size_t inlen,
                    uint8_t **out, size_t *outlen, const char **why);

int zstd_compress(const uint8_t *in, size_t inlen,
                  uint8_t **out, size_t *outlen, const char **why);

#endif /* TOBY_ZSTD_H */
