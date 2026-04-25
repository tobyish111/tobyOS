/* gfx.h -- 2D primitives over a Limine 32-bpp linear framebuffer.
 *
 * The compositor (gui.c) and any in-kernel diagnostic code draw into a
 * back buffer through these primitives; a single gfx_flip() then
 * memcpy's the entire back buffer to the framebuffer in one shot. That
 * gives us tear-free updates without the complexity of a real
 * page-flipping driver.
 *
 * Coordinates are signed because windows often live partially off-screen
 * (e.g. mid-drag). Every primitive clips internally to the back buffer
 * bounds; callers don't need to pre-clip.
 *
 * Pixel format: 0x00RRGGBB (matches console_set_color). The Limine
 * framebuffer is BGRX little-endian, so writing 0x00RRGGBB to a uint32_t
 * lays bytes down as B,G,R,X -- exactly what the hardware wants.
 */

#ifndef TOBYOS_GFX_H
#define TOBYOS_GFX_H

#include <tobyos/types.h>

/* Bring up the back buffer + remember the framebuffer geometry. Must be
 * called after heap_init(). Returns false if the framebuffer is unusable
 * or if we couldn't allocate a full back buffer. */
bool gfx_init(void *fb, uint64_t pitch, uint32_t width, uint32_t height);

bool     gfx_ready (void);
uint32_t gfx_width (void);
uint32_t gfx_height(void);

/* Direct back-buffer access -- exposed so the compositor can blit
 * window contents row-by-row without going through fill_rect. Returns 0
 * if the surface isn't initialised. */
uint32_t *gfx_backbuf(void);

/* Primitives. All clip silently to [0..width) x [0..height). */
void gfx_clear    (uint32_t color);
void gfx_set_pixel(int x, int y, uint32_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);

/* 8x8 bitmap text using the existing font8x8_basic table. Each character
 * is drawn at (x + i*8, y) in foreground colour fg over background bg.
 * Use bg = 0xFF000000 (alpha-only sentinel) to skip background fill --
 * useful for drawing labels over arbitrary surfaces. */
#define GFX_TRANSPARENT 0xFF000000u
void gfx_draw_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);

/* ---- M27D: scaled / smoothed bitmap text -------------------------
 *
 * tobyOS deliberately does NOT pull in stb_truetype or FreeType --
 * the dependency surface is too heavy for the scope of M27D and
 * either library would dominate the kernel image. Instead we
 * supersample the existing 8x8 bitmap font:
 *
 *   gfx_draw_text_scaled(x, y, s, fg, bg, scale)
 *
 *     Each source pixel becomes a `scale x scale` block of dest
 *     pixels. scale=1 is bit-identical to gfx_draw_text(); scale=2
 *     produces a 16x16 cell, scale=3 a 24x24 cell, and so on. Useful
 *     for the desktop title bars / launcher labels.
 *
 *   gfx_draw_text_smooth(x, y, s, fg, bg, scale)
 *
 *     Same scaling, but interior corners between adjacent ON pixels
 *     are filled with a 2x2 corner-detection pass that softens the
 *     diagonals. The output looks "anti-aliased" without any actual
 *     subpixel rasterisation. scale must be >= 2 (smoothing is a
 *     no-op at scale=1; the call falls back to gfx_draw_text).
 *
 *   gfx_text_bounds(s, scale, *out_w, *out_h)
 *
 *     Returns the pixel rectangle the call would produce. Used by
 *     button/label layout in gui.c. Pass scale=1 to get the legacy
 *     bitmap-font bounds; the function never NULL-derefs out_w/h.
 *
 * The bitmap font remains the universal fallback: every existing
 * gfx_draw_text() call continues to work unchanged. */
void gfx_draw_text_scaled(int x, int y, const char *s,
                          uint32_t fg, uint32_t bg, int scale);
void gfx_draw_text_smooth(int x, int y, const char *s,
                          uint32_t fg, uint32_t bg, int scale);
void gfx_text_bounds(const char *s, int scale, int *out_w, int *out_h);

/* Blit `src` (an w*h pixel buffer with `src_pitch` pixels per row)
 * into the back buffer at (dst_x, dst_y). Used by the compositor to
 * paint each window's per-window backing store onto the desktop. */
void gfx_blit(int dst_x, int dst_y, int w, int h,
              const uint32_t *src, int src_pitch);

/* ---- M27C: alpha blending ----------------------------------------
 *
 * Pixel layout for the back buffer is XRGB (0x00RRGGBB). The blended
 * variants below read the high byte of each source pixel as ALPHA
 * (0..255), and composite source-over the existing destination.
 *
 *   Final = (Src.RGB * A + Dst.RGB * (255 - A) + 127) / 255
 *
 * Alpha = 0   -> destination preserved (no write)
 * Alpha = 255 -> source overwrites (same as fill_rect / blit)
 * Anything in between blends per-pixel.
 *
 * Source pixels in ARGB form (0xAARRGGBB):
 *   gfx_fill_rect_blend(x, y, w, h, 0x80FF0000)   -> 50% red overlay
 *   gfx_blit_blend(...)                            -> per-pixel alpha
 *
 * Both calls clip to the back buffer extent and update the dirty
 * accumulator the same way the opaque variants do, so the existing
 * present_rect optimisation kicks in for partial overlays. */
void gfx_fill_rect_blend(int x, int y, int w, int h, uint32_t argb);
void gfx_blit_blend     (int dst_x, int dst_y, int w, int h,
                         const uint32_t *src, int src_pitch);

/* Software blend of one ARGB source pixel over an XRGB destination.
 * Exposed so the compositor (and tests) can blend single pixels
 * without spinning up an entire surface. Returns the resulting XRGB
 * pixel (alpha is consumed, not propagated). */
uint32_t gfx_blend_pixel_argb(uint32_t dst_xrgb, uint32_t src_argb);

/* Mouse cursor overlay: draws a 12x19 arrow at (x, y) directly into the
 * back buffer. Two-tone (black outline + white fill) so it stays visible
 * over any background. The compositor calls this last in each frame. */
void gfx_draw_cursor(int x, int y);

/* Push the back buffer to the framebuffer in one pass. Cheap (~3 ms for
 * 1024x768 in QEMU). The compositor calls this once per dirty frame.
 *
 * The actual push is dispatched through gfx_backend.flip below; the
 * default backend is a memcpy into the Limine framebuffer (the universal
 * fallback that works on every machine that gave Limine a framebuffer).
 * A driver such as src/virtio_gpu.c can install its own backend at boot
 * to route the same back-buffer contents through TRANSFER_TO_HOST_2D +
 * RESOURCE_FLUSH instead. */
void gfx_flip(void);

/* ---- backend indirection (M27B) -----------------------------------
 *
 * The display stack is split into two layers:
 *
 *   1. The 2D primitive layer (this file): clipping, fill_rect, blit,
 *      text, cursor, plus the global back buffer. None of this layer
 *      ever touches hardware -- it only writes 32-bit pixels into a
 *      kmalloc'd surface.
 *
 *   2. A pluggable "presentation" backend: takes the back buffer and
 *      moves it into the actual scanout. Default is the Limine
 *      framebuffer (memcpy-into-HHDM); a driver such as virtio-gpu
 *      (src/virtio_gpu.c) or future GOP-direct can install its own
 *      via gfx_set_backend() AFTER gfx_init() has succeeded.
 *
 * Backend op semantics:
 *
 *   name           short string for logs and userland introspection
 *                  (e.g. "limine-fb", "virtio-gpu"). Required.
 *
 *   flip()         present the ENTIRE back buffer to scanout. Required.
 *                  Called from gfx_flip(); must always be safe (it
 *                  silently no-ops if hardware isn't ready). The back
 *                  buffer geometry is reachable via gfx_width/height.
 *
 *   present_rect() OPTIONAL. Push a single (x,y,w,h) sub-rect of the
 *                  back buffer to scanout. Used by the dirty-rectangle
 *                  path (M27E). Backends that can only flush the whole
 *                  surface should leave this NULL -- the gfx layer
 *                  falls back to flip().
 *
 *   describe()     OPTIONAL. Stamp a backend-specific description into
 *                  `extra` (e.g. virtio-gpu PCI address, bytes/frame).
 *                  Used by displayinfo --json. Backends that only want
 *                  the default name string can leave this NULL.
 *
 *   bytes_per_pixel  OPTIONAL. Pixel size in bytes for the FRONT buffer
 *                  (the back buffer is always 4 bytes/pixel). Set to 4
 *                  for any 32-bpp framebuffer; the gfx layer uses this
 *                  only for size reporting. 0 = unknown / use default 4.
 *
 * Backends must remain pointer-stable for the lifetime of the kernel
 * (typically a `static const struct gfx_backend` in the implementer's
 * .c file). Passing NULL to gfx_set_backend reinstates the default
 * Limine-memcpy path. */
struct gfx_backend {
    void        (*flip)(void);
    void        (*present_rect)(int x, int y, int w, int h);
    int         (*describe)(char *extra, int cap);
    const char   *name;
    uint32_t      bytes_per_pixel;
};

void                      gfx_set_backend(const struct gfx_backend *backend);
const struct gfx_backend *gfx_get_backend(void);

/* ---- region invalidation (groundwork for M27E) -------------------
 *
 * Drawing primitives mark the union of every rect they touch since the
 * last gfx_flip(). gfx_flip() then either:
 *
 *   - calls backend->present_rect() over the union (if available), or
 *   - falls back to flip() (which presents everything).
 *
 * This API is exposed so the compositor can also push manual hints when
 * it knows a region is dirty without going through a primitive (e.g.
 * after copying a full window backbuf with gfx_blit). */
void gfx_mark_dirty_rect(int x, int y, int w, int h);
void gfx_mark_dirty_full(void);

/* Test/diagnostic accessors -- the compositor doesn't need these but
 * the M27 test harness does. Returns false if no rect is currently
 * dirty (i.e. flip() would be a no-op present-wise). */
bool gfx_dirty_get(int *x, int *y, int *w, int *h);

/* M27E: clear the dirty accumulator without flipping. The compositor
 * uses this to REPLACE the per-primitive dirty union with a smaller
 * compositor-level hint just before gfx_flip(). After a clear the
 * surface is treated as "nothing pending"; an immediate gfx_flip()
 * would no-op present-wise. */
void gfx_dirty_clear(void);

/* M27E: per-flip stats so the compositor + tests can prove dirty
 * rectangles are actually shrinking presents. Counters are monotonic;
 * partial_pixels accumulates with saturation. */
struct gfx_present_stats {
    uint64_t total_flips;        /* every gfx_flip call               */
    uint64_t full_flips;         /* used backend->flip()              */
    uint64_t partial_flips;      /* used backend->present_rect()      */
    uint64_t empty_flips;        /* nothing dirty -> still flushed    */
    uint64_t partial_pixels;     /* sum of present_rect rect areas    */
    uint64_t full_pixels;        /* sum of full-frame rect areas      */
};
void gfx_present_stats(struct gfx_present_stats *out);

#endif /* TOBYOS_GFX_H */
