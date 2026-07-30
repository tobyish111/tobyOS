/* toby/image.h -- Image decoding for tobyOS userland (Phase 6).
 *
 * Uses stb_image under the hood. Supports JPEG, PNG, BMP, GIF, TGA,
 * PSD, HDR, PIC, and PNM formats. All images are decoded to ARGB8888
 * pixel format for direct blitting to tobyOS window surfaces.
 */

#ifndef TOBY_IMAGE_H
#define TOBY_IMAGE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t *pixels;   /* ARGB8888 */
    int       width;
    int       height;
} toby_image_t;

/* Decode an image from a memory buffer. Returns NULL on failure. */
toby_image_t *toby_image_load(const uint8_t *data, size_t len);

/* Slice 62: reason string for the LAST toby_image_load failure (stbi's
 * static message, e.g. "outofmem"), or NULL after a success. */
const char *toby_image_error(void);

/* Read a file from disk and decode it. Returns NULL on failure. */
toby_image_t *toby_image_load_file(const char *path);

/* Free an image returned by toby_image_load*. Safe to pass NULL. */
void toby_image_free(toby_image_t *img);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_IMAGE_H */
