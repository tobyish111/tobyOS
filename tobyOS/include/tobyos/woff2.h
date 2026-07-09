/* woff2.h -- decode a WOFF2 web font (W3C WOFF2) to a plain sfnt
 * (TrueType/OpenType) blob the kernel TTF rasterizer can parse. WOFF2 is
 * brotli-compressed sfnt tables plus a glyf/loca (and optional hmtx)
 * transform; this reverses both. Stage: WOFF2 web fonts (post 13H). */
#ifndef TOBYOS_WOFF2_H
#define TOBYOS_WOFF2_H

#include <tobyos/types.h>

/* Returns 1 if buf looks like a WOFF2 file (signature 'wOF2'). */
int woff2_is(const uint8_t *buf, size_t len);

/* Decode a WOFF2 blob into a freshly kmalloc'd sfnt buffer. On success
 * returns 0 and sets *out (caller kfree's it) + *out_len; else negative. */
int woff2_to_sfnt(const uint8_t *woff2, size_t len,
                  uint8_t **out, size_t *out_len);

#endif /* TOBYOS_WOFF2_H */
