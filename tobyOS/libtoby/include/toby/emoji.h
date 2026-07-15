/* toby/emoji.h -- color emoji rendering via COLR v0 + CPAL.
 *
 * Renders emoji codepoints to straight-alpha ARGB (0xAARRGGBB) glyphs by
 * compositing the layered outlines a COLR/CPAL color font (e.g. Twemoji
 * Mozilla) defines for each base glyph, each layer tinted by its CPAL
 * palette color. Outline rasterization is stb_truetype (shared with
 * font.c); the COLR/CPAL parsing is here. */

#ifndef TOBY_EMOJI_H
#define TOBY_EMOJI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct toby_emoji toby_emoji_t;

/* Load a COLR/CPAL font (the bytes must stay alive for the handle's
 * lifetime). Returns NULL if the font lacks COLR/CPAL or is invalid. */
toby_emoji_t *toby_emoji_load(const uint8_t *ttf, size_t len);

/* Does this codepoint have a color glyph in the font? */
int  toby_emoji_has(toby_emoji_t *e, int cp);

/* Render codepoint `cp` at `px` pixels tall. On success returns a
 * malloc'd ARGB buffer (w*h, straight alpha, caller frees) and fills
 * *w/*h and the horizontal *advance in px. Returns NULL if `cp` is not a
 * color glyph. */
uint32_t *toby_emoji_render(toby_emoji_t *e, int cp, int px,
                            int *w, int *h, int *advance);

void toby_emoji_free(toby_emoji_t *e);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_EMOJI_H */
