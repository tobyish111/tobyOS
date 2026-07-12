/* toby/avif_decode.h -- AVIF (AV1-in-ISO-BMFF) still-image decode for
 * tobyOS userland. A C-linkage bridge over the vendored libgav1 (used by
 * libtoby's image loader, next to WebP/stb). */
#ifndef TOBY_AVIF_DECODE_H
#define TOBY_AVIF_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if the buffer looks like an AVIF file (ftyp with an avif/avis brand). */
int avif_sniff(const uint8_t *data, size_t len);

/* Decode an AVIF still image to RGBA8888 (R,G,B,A byte order, like
 * WebPDecodeRGBA). Returns a malloc'd buffer (free with free()) and sets
 * *w/*h, or NULL on failure. */
uint8_t *avif_decode_rgba(const uint8_t *data, size_t len, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_AVIF_DECODE_H */
