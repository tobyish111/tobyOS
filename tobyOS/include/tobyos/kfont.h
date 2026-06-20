/* kfont.h -- kernel-side TrueType text rendering (Track C / C14b).
 *
 * A genuine TrueType rasterizer (stb_truetype) running in the kernel, used by
 * the Win32 GDI text path (CreateFontA/TextOutA/DrawTextA/GetTextExtent…) so
 * apps render real antialiased glyphs at arbitrary pixel sizes instead of the
 * scaled 8x8 bitmap font. The font (Lato, OFL) ships in the initrd and is
 * lazy-loaded on first use. stb_truetype is float-heavy and the kernel is
 * -mno-sse, so kfont.c is compiled -msse and brackets every floating-point
 * (rasterization) window in an FXSAVE/FXRSTOR guard so the calling user
 * process's live SSE state is never clobbered.
 */
#ifndef TOBYOS_KFONT_H
#define TOBYOS_KFONT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct window;   /* opaque (compositor window) */

/* Lazy-load the bundled TTF (idempotent). Returns true once a font is ready. */
bool kfont_available(void);

/* Advance width, in pixels, of NUL-terminated `s` rendered at pixel height `px`
 * (`len` >= 0 limits the character count; <0 = whole string). */
int  kfont_text_width(const char *s, int len, int px);

/* Vertical metrics at pixel height `px` (any out-pointer may be NULL). */
void kfont_vmetrics(int px, int *ascent, int *descent, int *line_height);

/* Draw `s` (len<0 = whole string) at (x,y) = the top-left of the text box, in
 * colour `xrgb` (0x00RRGGBB), pixel height `px`, alpha-blended into the
 * window's client backbuffer. Returns the advance width drawn. */
int  kfont_draw_window(struct window *w, int x, int y, const char *s, int len,
                       uint32_t xrgb, int px);

#endif /* TOBYOS_KFONT_H */
