/* libtoby/src/image.c -- Image decoding for tobyOS userland (Phase 6).
 *
 * Wraps stb_image to decode JPEG, PNG, BMP, GIF, TGA, PSD, PIC, and
 * PNM images into ARGB8888 pixel buffers. Configured for freestanding:
 * no stdio, no HDR/linear (avoids math.h), custom allocator.
 */

#include <toby/image.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* stb_image configuration for freestanding environment. */
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS

#define STBI_MALLOC(sz)          malloc(sz)
#define STBI_REALLOC(p, newsz)   realloc(p, newsz)
#define STBI_FREE(p)             free(p)
#define STBI_ASSERT(x)           ((void)(x))

/* Provide abs() that stb_image may need. */
#ifndef abs
#define abs(x) ((x) < 0 ? -(x) : (x))
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#include "../../third_party/stb_image.h"

/* WebP (stage 13): the vendored libwebp v1.4.0 decoder handles what
 * stb_image can't -- lossy VP8, lossless VP8L, and alpha. */
#include "../../third_party/libwebp/src/webp/decode.h"

/* AVIF (AV1 still image): the vendored libgav1 decoder via a bridge. */
#include <toby/avif_decode.h>

/* Slice 62: last decode-failure reason (stbi's static string, or 0). */
static const char *g_last_error;
const char *toby_image_error(void) { return g_last_error; }

static int webp_sniff(const uint8_t *d, size_t n) {
    return n >= 12 &&
           d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' &&
           d[8] == 'W' && d[9] == 'E' && d[10] == 'B' && d[11] == 'P';
}

toby_image_t *toby_image_load(const uint8_t *data, size_t len) {
    if (!data || len == 0) return NULL;

    int w, h, channels;
    unsigned char *rgba = NULL;
    int from_webp = 0, from_avif = 0;
    if (webp_sniff(data, len)) {
        rgba = WebPDecodeRGBA(data, len, &w, &h);
        from_webp = (rgba != NULL);
    }
    if (!rgba && avif_sniff(data, len)) {
        rgba = avif_decode_rgba(data, len, &w, &h);   /* malloc'd RGBA */
        from_avif = (rgba != NULL);
    }
    /* Request 4 channels (RGBA). */
    if (!rgba)
        rgba = stbi_load_from_memory(data, (int)len,
                                     &w, &h, &channels, 4);
    if (!rgba) {
        /* Slice 62: surface WHY. A run produced 1035 bare decode failures
         * and the cause (allocator vs codec vs data) was undiagnosable from
         * the outside. stbi keeps a static reason string; hand it out. */
        g_last_error = stbi_failure_reason();
        return NULL;
    }
    g_last_error = 0;

    toby_image_t *img = (toby_image_t *)malloc(sizeof(toby_image_t));
    if (!img) {
        if (from_webp) WebPFree(rgba); else if (from_avif) free(rgba); else stbi_image_free(rgba);
        return NULL;
    }

    img->width  = w;
    img->height = h;

    /* Convert RGBA -> ARGB8888 (stbi emits R,G,B,A bytes; tobyOS wants
     * 0xAARRGGBB). Slice 63b: swizzle IN PLACE as u32 ops. The old path
     * malloc'd a SECOND full-size buffer and converted byte-by-byte into
     * it -- for chromewin's frame stream that was an extra 1.9-3.6 MiB
     * allocation + free per frame (double the allocator churn that
     * triggered the 62b malloc bug) plus a scalar pass clang cannot
     * vectorize. As u32 loads (RGBA little-endian == 0xAABBGGRR) the
     * transform is keep A+G, swap R<->B -- three ANDs, two shifts, two
     * ORs -- which clang auto-vectorizes to 4-8 pixels per instruction
     * under -msse2. stbi's buffer comes from OUR malloc (STBI_MALLOC), so
     * adopting it as img->pixels is free()-compatible; WebP/AVIF buffers
     * come from foreign allocators and keep the copy path. */
    if (!from_webp && !from_avif) {
        uint32_t *px = (uint32_t *)rgba;
        size_t    npx = (size_t)w * (size_t)h;
        for (size_t i = 0; i < npx; i++) {
            uint32_t p = px[i];
            px[i] = (p & 0xFF00FF00u)            /* A and G stay */
                  | ((p & 0x00FF0000u) >> 16)    /* B down to low byte */
                  | ((p & 0x000000FFu) << 16);   /* R up to bits 16-23 */
        }
        img->pixels = px;                        /* adopt stbi's buffer */
        return img;
    }

    img->pixels = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!img->pixels) {
        if (from_webp) WebPFree(rgba); else free(rgba);
        free(img);
        return NULL;
    }
    for (int i = 0; i < w * h; i++) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        uint8_t a = rgba[i * 4 + 3];
        img->pixels[i] = ((uint32_t)a << 24) |
                          ((uint32_t)r << 16) |
                          ((uint32_t)g << 8)  |
                          ((uint32_t)b);
    }
    if (from_webp) WebPFree(rgba); else free(rgba);
    return img;
}

toby_image_t *toby_image_load_file(const char *path) {
    if (!path) return NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    /* Read the file in chunks. */
    size_t cap  = 64 * 1024;
    size_t used = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) { close(fd); return NULL; }

    for (;;) {
        if (used >= cap) {
            size_t newcap = cap * 2;
            uint8_t *nb = (uint8_t *)realloc(buf, newcap);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
            cap = newcap;
        }
        ssize_t n = read(fd, buf + used, cap - used);
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd);

    toby_image_t *img = toby_image_load(buf, used);
    free(buf);
    return img;
}

void toby_image_free(toby_image_t *img) {
    if (!img) return;
    if (img->pixels) free(img->pixels);
    free(img);
}
