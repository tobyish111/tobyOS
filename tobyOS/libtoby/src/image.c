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

toby_image_t *toby_image_load(const uint8_t *data, size_t len) {
    if (!data || len == 0) return NULL;

    int w, h, channels;
    /* Request 4 channels (RGBA). */
    unsigned char *rgba = stbi_load_from_memory(data, (int)len,
                                                 &w, &h, &channels, 4);
    if (!rgba) return NULL;

    toby_image_t *img = (toby_image_t *)malloc(sizeof(toby_image_t));
    if (!img) {
        stbi_image_free(rgba);
        return NULL;
    }

    img->width  = w;
    img->height = h;
    img->pixels = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!img->pixels) {
        stbi_image_free(rgba);
        free(img);
        return NULL;
    }

    /* Convert RGBA -> ARGB8888. stbi outputs R,G,B,A bytes in order.
     * tobyOS expects 0xAARRGGBB packed uint32_t. */
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

    stbi_image_free(rgba);
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
