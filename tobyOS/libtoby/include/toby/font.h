/* toby/font.h -- TrueType font rendering API (Phase 2 M2.1).
 *
 * Provides scalable anti-aliased text rendering for tobyOS applications
 * using stb_truetype as the backend rasterizer. Supports:
 *   - Loading TrueType (.ttf) font files
 *   - Rendering glyphs at arbitrary sizes
 *   - Glyph caching for performance
 *   - Alpha-blended text onto ARGB surfaces
 *   - Text measurement (width, height, baseline)
 */

#ifndef TOBY_FONT_H
#define TOBY_FONT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque font handle */
typedef struct toby_font toby_font_t;

/* Load a font from a TrueType file buffer. Returns NULL on failure.
 * The font data must remain valid for the lifetime of the font object. */
toby_font_t *toby_font_load(const uint8_t *ttf_data, size_t ttf_size);

/* Free a loaded font */
void toby_font_free(toby_font_t *font);

/* Set the rendering size in pixels (height) */
void toby_font_set_size(toby_font_t *font, float size_px);

/* Render a string onto an ARGB8888 surface.
 *   dst:    destination pixel buffer (ARGB format)
 *   dst_w:  destination width in pixels
 *   dst_h:  destination height in pixels
 *   x, y:  top-left position to start rendering
 *   text:  UTF-8 encoded string to render
 *   color: text color (0xAARRGGBB format)
 * Returns the x-advance (width of rendered text). */
int toby_font_draw_text(toby_font_t *font, uint32_t *dst,
                        int dst_w, int dst_h,
                        int x, int y,
                        const char *text, uint32_t color);

/* Measure text without rendering.
 *   width:  output width in pixels (may be NULL)
 *   height: output height in pixels (may be NULL)
 * Returns the x-advance. */
int toby_font_measure_text(toby_font_t *font, const char *text,
                           int *width, int *height);

/* Get font metrics at current size */
void toby_font_metrics(toby_font_t *font, int *ascent, int *descent,
                       int *line_gap);

/* ---- Glyph cache control ---- */

/* Pre-cache ASCII glyphs for the current size (improves first-draw perf) */
void toby_font_precache_ascii(toby_font_t *font);

/* Clear the glyph cache (useful when changing sizes frequently) */
void toby_font_clear_cache(toby_font_t *font);

/* ---- Subpixel rendering (Phase 2 M2.2) ---- */

/* LCD subpixel rendering mode */
typedef enum {
    TOBY_FONT_SP_NONE = 0,   /* Grayscale AA (default) */
    TOBY_FONT_SP_RGB,        /* LCD RGB subpixel (most common) */
    TOBY_FONT_SP_BGR,        /* LCD BGR subpixel (rare) */
    TOBY_FONT_SP_VRGB,       /* Vertical RGB (rotated displays) */
    TOBY_FONT_SP_VBGR,       /* Vertical BGR */
} toby_font_subpixel_t;

/* Set subpixel rendering mode */
void toby_font_set_subpixel(toby_font_t *font, toby_font_subpixel_t mode);

/* Set gamma correction (default 1.8, range 1.0-3.0) */
void toby_font_set_gamma(toby_font_t *font, float gamma);

/* Render text with subpixel positioning (fractional x offset for
 * tighter kerning). Same parameters as toby_font_draw_text. */
int toby_font_draw_text_subpixel(toby_font_t *font, uint32_t *dst,
                                  int dst_w, int dst_h,
                                  int x, int y,
                                  const char *text, uint32_t color);

/* ---- HiDPI support (Phase 2 M2.6) ---- */

/* Set DPI scale factor (1.0 = 96 DPI, 2.0 = 192 DPI) */
void toby_font_set_dpi_scale(toby_font_t *font, float scale);

/* ---- Font fallback (Phase 5 M5.6) ---- */

/* Set a fallback font to use when a glyph is not found in the primary.
 * If the primary font lacks a glyph, the fallback is consulted before
 * returning a missing-glyph placeholder. */
void toby_font_set_fallback(toby_font_t *primary, toby_font_t *fallback);

/* Measure a single codepoint. Returns the horizontal advance in pixels
 * at the current font size. */
int toby_font_measure_char(toby_font_t *font, int codepoint, float size);

/* Measure a UTF-8 text run of `len` bytes (or until NUL if len < 0).
 * Returns the total width in pixels at the given size. */
int toby_font_measure_text_ex(toby_font_t *font, const char *text,
                              int len, float size);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_FONT_H */
