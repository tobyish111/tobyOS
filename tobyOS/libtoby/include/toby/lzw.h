/* toby/lzw.h -- the .Z (compress(1)) format.
 *
 * `uncompress` was on PATH as another name for gunzip, which cannot read
 * a .Z file: the name existed and the format did not. Both directions are
 * here so `compress` and `uncompress` mean what they say.
 */
#ifndef TOBY_LZW_H
#define TOBY_LZW_H

#include <stddef.h>
#include <stdint.h>

/* compress(1)'s own default and ceiling. */
#define LZW_MAX_BITS 16

/* Both return 0 on success and fill *out (malloc'd, caller frees) and
 * *outlen. On failure they return -1 and point *why at a reason. */
int lzw_compress(const uint8_t *in, size_t inlen, int maxbits,
                 uint8_t **out, size_t *outlen, const char **why);
int lzw_decompress(const uint8_t *in, size_t inlen,
                   uint8_t **out, size_t *outlen, const char **why);

#endif /* TOBY_LZW_H */
