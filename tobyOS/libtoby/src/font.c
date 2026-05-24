/* libtoby/src/font.c -- TrueType font rendering (Phase 2 M2.1).
 *
 * Uses stb_truetype.h for parsing and rasterizing TrueType glyphs.
 * Provides a simple caching layer for glyph bitmaps and alpha-blended
 * text rendering onto ARGB8888 surfaces.
 */

#include <toby/font.h>
#include <stdlib.h>
#include <string.h>

/* stb_truetype configuration for freestanding environment */
#define STBTT_ifloor(x)    ((int)(x) - ((x) < 0 && (x) != (int)(x)))
#define STBTT_iceil(x)     ((int)(x) + ((x) > 0 && (x) != (int)(x)))

static void *toby_stbtt_malloc(size_t sz, void *ctx) {
    (void)ctx;
    return malloc(sz);
}
static void toby_stbtt_free(void *ptr, void *ctx) {
    (void)ctx;
    free(ptr);
}

#define STBTT_malloc(x,u)  toby_stbtt_malloc(x,u)
#define STBTT_free(x,u)    toby_stbtt_free(x,u)
#define STBTT_assert(x)    ((void)(x))

/* Use integer-only rasterizer (no SSE required) */
#define STBTT_RASTERIZER_VERSION 1

/* We need basic math functions. Provide minimal versions. */
static double toby_sqrt(double x) {
    if (x <= 0) return 0;
    double r = x;
    for (int i = 0; i < 20; i++) r = (r + x / r) * 0.5;
    return r;
}
static double toby_fabs(double x) { return x < 0 ? -x : x; }
static double toby_floor(double x) { return (double)(long long)x - (x < 0 && x != (long long)x); }
static double toby_ceil(double x) { return (double)(long long)x + (x > 0 && x != (long long)x); }
static double toby_fmod(double a, double b) { return a - b * toby_floor(a / b); }
static double toby_pow(double base, double exp_) {
    if (exp_ == 0) return 1;
    if (exp_ == 1) return base;
    if (exp_ == 2) return base * base;
    double result = 1;
    int iexp = (int)exp_;
    for (int i = 0; i < iexp; i++) result *= base;
    return result;
}
static double toby_acos(double x) {
    /* Approximate acos using polynomial */
    if (x >= 1) return 0;
    if (x <= -1) return 3.14159265358979;
    return 1.5707963 - x * (1.0 + x * x * (-0.1667 + x * x * 0.075));
}
static double toby_cos(double x) {
    x = toby_fmod(x + 3.14159265, 6.28318530) - 3.14159265;
    double x2 = x * x;
    return 1.0 - x2 * (0.5 - x2 * (0.04166667 - x2 * 0.001388889));
}

#define STBTT_sqrt(x)     toby_sqrt(x)
#define STBTT_fabs(x)     toby_fabs(x)
#define STBTT_fmod(x,y)   toby_fmod(x,y)
#define STBTT_pow(x,y)    toby_pow(x,y)
#define STBTT_acos(x)     toby_acos(x)
#define STBTT_cos(x)      toby_cos(x)

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb_truetype.h"

/* ---- Glyph cache ---- */

#define CACHE_SIZE 128

struct cached_glyph {
    int      codepoint;
    float    size;
    uint8_t *bitmap;
    int      w, h;
    int      xoff, yoff;
    int      advance;
    int      lsb;
};

struct toby_font {
    stbtt_fontinfo info;
    const uint8_t *data;
    size_t         data_size;
    float          size_px;
    float          scale;
    float          dpi_scale;       /* HiDPI multiplier (default 1.0) */
    float          gamma;           /* Gamma correction (default 1.8) */
    int            subpixel_mode;   /* toby_font_subpixel_t */
    toby_font_t   *fallback;       /* fallback font for missing glyphs */

    struct cached_glyph cache[CACHE_SIZE];
    int cache_count;
};

/* ---- Implementation ---- */

toby_font_t *toby_font_load(const uint8_t *ttf_data, size_t ttf_size) {
    if (!ttf_data || ttf_size < 12) return 0;

    toby_font_t *f = (toby_font_t *)malloc(sizeof(toby_font_t));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));

    f->data = ttf_data;
    f->data_size = ttf_size;
    f->dpi_scale = 1.0f;
    f->gamma = 1.8f;
    f->subpixel_mode = 0;
    f->fallback = 0;

    if (!stbtt_InitFont(&f->info, ttf_data,
                        stbtt_GetFontOffsetForIndex(ttf_data, 0))) {
        free(f);
        return 0;
    }

    f->size_px = 16.0f;
    f->scale = stbtt_ScaleForPixelHeight(&f->info, f->size_px);
    return f;
}

void toby_font_free(toby_font_t *font) {
    if (!font) return;
    toby_font_clear_cache(font);
    free(font);
}

void toby_font_set_size(toby_font_t *font, float size_px) {
    if (!font || size_px <= 0) return;
    if (font->size_px != size_px) {
        font->size_px = size_px;
        font->scale = stbtt_ScaleForPixelHeight(&font->info, size_px);
        toby_font_clear_cache(font);
    }
}

void toby_font_clear_cache(toby_font_t *font) {
    if (!font) return;
    for (int i = 0; i < font->cache_count; i++) {
        if (font->cache[i].bitmap)
            free(font->cache[i].bitmap);
    }
    font->cache_count = 0;
}

static struct cached_glyph *get_glyph(toby_font_t *font, int cp) {
    /* Search cache */
    for (int i = 0; i < font->cache_count; i++) {
        if (font->cache[i].codepoint == cp &&
            font->cache[i].size == font->size_px)
            return &font->cache[i];
    }

    /* Rasterize */
    if (font->cache_count >= CACHE_SIZE) {
        /* Evict oldest */
        if (font->cache[0].bitmap) free(font->cache[0].bitmap);
        memmove(&font->cache[0], &font->cache[1],
                (CACHE_SIZE - 1) * sizeof(struct cached_glyph));
        font->cache_count = CACHE_SIZE - 1;
    }

    struct cached_glyph *g = &font->cache[font->cache_count++];
    g->codepoint = cp;
    g->size = font->size_px;

    int advance, lsb;
    stbtt_GetCodepointHMetrics(&font->info, cp, &advance, &lsb);
    g->advance = (int)(advance * font->scale);
    g->lsb = (int)(lsb * font->scale);

    g->bitmap = stbtt_GetCodepointBitmap(&font->info, 0, font->scale,
                                          cp, &g->w, &g->h,
                                          &g->xoff, &g->yoff);

    /* If glyph not found in primary, try fallback */
    if (!g->bitmap && font->fallback) {
        toby_font_t *fb = font->fallback;
        float saved_size = fb->size_px;
        if (fb->size_px != font->size_px) {
            fb->size_px = font->size_px;
            fb->scale = stbtt_ScaleForPixelHeight(&fb->info, font->size_px);
        }
        int fb_advance, fb_lsb;
        stbtt_GetCodepointHMetrics(&fb->info, cp, &fb_advance, &fb_lsb);
        g->advance = (int)(fb_advance * fb->scale);
        g->lsb = (int)(fb_lsb * fb->scale);
        g->bitmap = stbtt_GetCodepointBitmap(&fb->info, 0, fb->scale,
                                              cp, &g->w, &g->h,
                                              &g->xoff, &g->yoff);
        if (saved_size != font->size_px) {
            fb->size_px = saved_size;
            fb->scale = stbtt_ScaleForPixelHeight(&fb->info, saved_size);
        }
    }

    return g;
}

int toby_font_draw_text(toby_font_t *font, uint32_t *dst,
                        int dst_w, int dst_h,
                        int x, int y,
                        const char *text, uint32_t color) {
    if (!font || !dst || !text) return 0;

    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &line_gap);
    int baseline = y + (int)(ascent * font->scale);

    uint8_t cr = (color >> 16) & 0xFF;
    uint8_t cg = (color >> 8) & 0xFF;
    uint8_t cb = color & 0xFF;

    int start_x = x;

    while (*text) {
        int cp = (unsigned char)*text++;

        struct cached_glyph *g = get_glyph(font, cp);
        if (!g || !g->bitmap) {
            x += g ? g->advance : (int)(font->size_px * 0.5f);
            continue;
        }

        /* Blit with alpha blending */
        int bx = x + g->xoff;
        int by = baseline + g->yoff;

        for (int row = 0; row < g->h; row++) {
            int dy = by + row;
            if (dy < 0 || dy >= dst_h) continue;
            for (int col = 0; col < g->w; col++) {
                int dx = bx + col;
                if (dx < 0 || dx >= dst_w) continue;

                uint8_t alpha = g->bitmap[row * g->w + col];
                if (alpha == 0) continue;

                uint32_t *px = &dst[dy * dst_w + dx];
                if (alpha == 255) {
                    *px = 0xFF000000u | ((uint32_t)cr << 16) |
                          ((uint32_t)cg << 8) | cb;
                } else {
                    /* Alpha blend */
                    uint32_t bg = *px;
                    uint8_t br_ = (bg >> 16) & 0xFF;
                    uint8_t bg_ = (bg >> 8) & 0xFF;
                    uint8_t bb_ = bg & 0xFF;

                    uint8_t inv = 255 - alpha;
                    uint8_t or_ = (cr * alpha + br_ * inv) / 255;
                    uint8_t og  = (cg * alpha + bg_ * inv) / 255;
                    uint8_t ob  = (cb * alpha + bb_ * inv) / 255;

                    *px = 0xFF000000u | ((uint32_t)or_ << 16) |
                          ((uint32_t)og << 8) | ob;
                }
            }
        }

        x += g->advance;

        /* Kerning */
        if (*text) {
            int next_cp = (unsigned char)*text;
            int kern = stbtt_GetCodepointKernAdvance(&font->info, cp, next_cp);
            x += (int)(kern * font->scale);
        }
    }

    return x - start_x;
}

int toby_font_measure_text(toby_font_t *font, const char *text,
                           int *width, int *height) {
    if (!font || !text) return 0;

    int total_w = 0;
    const char *p = text;
    while (*p) {
        int cp = (unsigned char)*p++;
        struct cached_glyph *g = get_glyph(font, cp);
        total_w += g ? g->advance : (int)(font->size_px * 0.5f);

        if (*p && g) {
            int next_cp = (unsigned char)*p;
            int kern = stbtt_GetCodepointKernAdvance(&font->info, cp, next_cp);
            total_w += (int)(kern * font->scale);
        }
    }

    if (width) *width = total_w;
    if (height) *height = (int)font->size_px;
    return total_w;
}

void toby_font_metrics(toby_font_t *font, int *ascent, int *descent,
                       int *line_gap) {
    if (!font) return;
    int a, d, lg;
    stbtt_GetFontVMetrics(&font->info, &a, &d, &lg);
    if (ascent) *ascent = (int)(a * font->scale);
    if (descent) *descent = (int)(d * font->scale);
    if (line_gap) *line_gap = (int)(lg * font->scale);
}

void toby_font_precache_ascii(toby_font_t *font) {
    if (!font) return;
    for (int cp = 32; cp < 127; cp++) {
        get_glyph(font, cp);
    }
}

/* ---- Subpixel rendering (Phase 2 M2.2) ---- */

void toby_font_set_subpixel(toby_font_t *font, toby_font_subpixel_t mode) {
    if (font) font->subpixel_mode = (int)mode;
}

void toby_font_set_gamma(toby_font_t *font, float gamma) {
    if (font && gamma >= 1.0f && gamma <= 3.0f) font->gamma = gamma;
}

void toby_font_set_dpi_scale(toby_font_t *font, float scale) {
    if (font && scale > 0.0f) font->dpi_scale = scale;
}

/* Gamma-correct blending for ClearType-style subpixel rendering.
 * Linearizes both source and dest, blends, then re-gamma encodes. */
static uint8_t gamma_blend(uint8_t dst_srgb, uint8_t src_srgb,
                           uint8_t alpha, float gamma) {
    if (alpha == 0) return dst_srgb;
    if (alpha == 255) return src_srgb;

    /* Approximate linearize: (x/255)^gamma * 255 */
    float d = (float)dst_srgb / 255.0f;
    float s = (float)src_srgb / 255.0f;
    float a = (float)alpha / 255.0f;

    /* Simple power-curve approximation */
    float dl = d * d;  /* ~gamma 2.0 approximation */
    float sl = s * s;

    float blended = sl * a + dl * (1.0f - a);
    if (blended < 0.0f) blended = 0.0f;
    if (blended > 1.0f) blended = 1.0f;

    /* Square root to re-encode (~gamma 2.0 inverse) */
    float result = blended * 255.0f;
    /* Cheap sqrt approximation: Newton's method one iteration */
    float x = result;
    if (x > 0.0f) {
        float guess = x * 0.5f;
        guess = (guess + x / guess) * 0.5f;
        result = guess;
    }

    (void)gamma;
    return (uint8_t)(result > 255.0f ? 255.0f : result);
}

int toby_font_draw_text_subpixel(toby_font_t *font, uint32_t *dst,
                                  int dst_w, int dst_h,
                                  int x, int y,
                                  const char *text, uint32_t color) {
    if (!font || !dst || !text) return 0;

    /* If no subpixel mode, fall back to regular rendering */
    if (font->subpixel_mode == 0) {
        return toby_font_draw_text(font, dst, dst_w, dst_h, x, y, text, color);
    }

    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &line_gap);
    int baseline = y + (int)(ascent * font->scale);

    uint8_t cr = (color >> 16) & 0xFF;
    uint8_t cg = (color >> 8) & 0xFF;
    uint8_t cb = color & 0xFF;

    int start_x = x;

    while (*text) {
        int cp = (unsigned char)*text++;

        struct cached_glyph *g = get_glyph(font, cp);
        if (!g || !g->bitmap) {
            x += g ? g->advance : (int)(font->size_px * 0.5f);
            continue;
        }

        int bx = x + g->xoff;
        int by = baseline + g->yoff;

        /* Subpixel rendering: treat each grayscale pixel as 3 subpixels.
         * For RGB mode: the alpha value is distributed across R, G, B
         * channels with a 1/3 pixel horizontal offset each.
         * Simplified: use the grayscale bitmap but apply per-channel
         * alpha with a box filter ([-1, 0, +1] pixel offsets). */
        for (int row = 0; row < g->h; row++) {
            int dy = by + row;
            if (dy < 0 || dy >= dst_h) continue;

            for (int col = 0; col < g->w; col++) {
                int dx = bx + col;
                if (dx < 0 || dx >= dst_w) continue;

                uint8_t center = g->bitmap[row * g->w + col];
                if (center == 0) continue;

                /* Sample neighbors for subpixel filter */
                uint8_t left = (col > 0) ? g->bitmap[row * g->w + col - 1] : 0;
                uint8_t right = (col < g->w - 1) ? g->bitmap[row * g->w + col + 1] : 0;

                /* RGB subpixel weights (1/3 filter) */
                uint8_t alpha_r, alpha_g, alpha_b;
                if (font->subpixel_mode == 1) { /* RGB */
                    alpha_r = (uint8_t)((left + 2 * center) / 3);
                    alpha_g = center;
                    alpha_b = (uint8_t)((right + 2 * center) / 3);
                } else { /* BGR */
                    alpha_b = (uint8_t)((left + 2 * center) / 3);
                    alpha_g = center;
                    alpha_r = (uint8_t)((right + 2 * center) / 3);
                }

                uint32_t *px = &dst[dy * dst_w + dx];
                uint32_t bg = *px;
                uint8_t bgr = (bg >> 16) & 0xFF;
                uint8_t bgg = (bg >> 8) & 0xFF;
                uint8_t bgb = bg & 0xFF;

                /* Per-channel gamma-corrected blend */
                uint8_t out_r = gamma_blend(bgr, cr, alpha_r, font->gamma);
                uint8_t out_g = gamma_blend(bgg, cg, alpha_g, font->gamma);
                uint8_t out_b = gamma_blend(bgb, cb, alpha_b, font->gamma);

                *px = 0xFF000000u | ((uint32_t)out_r << 16) |
                      ((uint32_t)out_g << 8) | out_b;
            }
        }

        x += g->advance;

        if (*text) {
            int next_cp = (unsigned char)*text;
            int kern = stbtt_GetCodepointKernAdvance(&font->info, cp, next_cp);
            x += (int)(kern * font->scale);
        }
    }

    return x - start_x;
}

/* ---- Font fallback and extended measurement (Phase 5 M5.6) ---- */

void toby_font_set_fallback(toby_font_t *primary, toby_font_t *fallback) {
    if (primary) primary->fallback = fallback;
}

int toby_font_measure_char(toby_font_t *font, int codepoint, float size) {
    if (!font) return 0;

    float saved = font->size_px;
    if (size > 0 && size != font->size_px) {
        toby_font_set_size(font, size);
    }

    struct cached_glyph *g = get_glyph(font, codepoint);
    int advance = g ? g->advance : (int)(font->size_px * 0.5f);

    if (size > 0 && size != saved) {
        toby_font_set_size(font, saved);
    }
    return advance;
}

int toby_font_measure_text_ex(toby_font_t *font, const char *text,
                              int len, float size) {
    if (!font || !text) return 0;

    float saved = font->size_px;
    if (size > 0 && size != font->size_px) {
        toby_font_set_size(font, size);
    }

    int total_w = 0;
    int consumed = 0;
    const char *p = text;

    while (*p && (len < 0 || consumed < len)) {
        int cp = (unsigned char)*p++;
        consumed++;

        struct cached_glyph *g = get_glyph(font, cp);
        total_w += g ? g->advance : (int)(font->size_px * 0.5f);

        if (*p && g && (len < 0 || consumed < len)) {
            int next_cp = (unsigned char)*p;
            int kern = stbtt_GetCodepointKernAdvance(&font->info, cp, next_cp);
            total_w += (int)(kern * font->scale);
        }
    }

    if (size > 0 && size != saved) {
        toby_font_set_size(font, saved);
    }
    return total_w;
}
