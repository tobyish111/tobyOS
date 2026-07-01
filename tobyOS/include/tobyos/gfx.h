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

/* Re-point the hardware framebuffer pointer after Limine/GOP address
 * normalisation or a post-vmm remap (UEFI GOP often reports phys
 * 0x400000; console/gfx must use HHDM+phys). No-op if gfx_init has
 * not run yet. */
void gfx_sync_framebuffer(void *fb, uint64_t pitch, uint32_t width, uint32_t height);

/* Direct back-buffer access -- exposed so the compositor can blit
 * window contents row-by-row without going through fill_rect. Returns 0
 * if the surface isn't initialised. */
uint32_t *gfx_backbuf(void);

/* Primitives. All clip silently to [0..width) x [0..height). */
void gfx_clear    (uint32_t color);
void gfx_set_pixel(int x, int y, uint32_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);

/* Filled rounded rectangle. Corners use midpoint circle algorithm.
 * radius is clamped to min(w,h)/2. */
void gfx_fill_rounded_rect(int x, int y, int w, int h, int radius, uint32_t color);

/* Alpha-blended filled rounded rectangle. */
void gfx_fill_rounded_rect_blend(int x, int y, int w, int h, int radius, uint32_t argb);

/* Bresenham line from (x0,y0) to (x1,y1). */
void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color);

/* Horizontal and vertical lines (optimized, no Bresenham overhead). */
void gfx_hline(int x, int y, int w, uint32_t color);
void gfx_vline(int x, int y, int h, uint32_t color);

/* 8x8 bitmap text using the existing font8x8_basic table. Each character
 * is drawn at (x + i*8, y) in foreground colour fg over background bg.
 * Use bg = 0xFF000000 (alpha-only sentinel) to skip background fill --
 * useful for drawing labels over arbitrary surfaces. */
#define GFX_TRANSPARENT 0xFF000000u
void gfx_draw_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);

/* 8x16 VGA bitmap text. Same interface as gfx_draw_text but uses the
 * larger font for better readability in UI chrome (title bars, menus,
 * dialog text). Each character is 8px wide, 16px tall. */
void gfx_draw_text16(int x, int y, const char *s, uint32_t fg, uint32_t bg);
void gfx_text16_bounds(const char *s, int *out_w, int *out_h);

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

/* Hardware cursor plane query/move (Stage-4 Intel page-flip backend). When
 * available, the compositor moves the pointer with gfx_hw_cursor_move()
 * instead of invalidating + flipping -- keeping the mouse smooth under
 * page-flipping. gfx_hw_cursor_available() is false for every other backend
 * (they use the software overlay below). */
bool gfx_hw_cursor_available(void);
void gfx_hw_cursor_move(int x, int y);

/* Fast software cursor overlay for the direct Limine framebuffer path.
 * These draw directly to scanout after saving/restoring the pixels under
 * the cursor, so pointer movement does not require a full compositor pass.
 * Backends that do not expose direct scanout simply no-op. */
void gfx_cursor_overlay_hide(void);
void gfx_cursor_overlay_show(int x, int y);
void gfx_cursor_overlay_move(int x, int y);

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

/* Stage 3 (i915-lite): install the Intel GT blitter present backend (see
 * src/gpu_intel_modeset.c). Call ONLY after intel_gt_present_setup() has
 * returned 1. The backend blits dirty rects to the live scanout via the
 * GPU and falls back to the Limine CPU memcpy on any per-flip failure. */
void                      gfx_use_intel_backend(void);

/* Stage 4 (i915-lite): install the Intel GT tear-free page-flip backend.
 * Call ONLY after intel_gt_flip_setup() has returned 1. Presents via a
 * full-frame blit into an off-screen buffer + vblank-latched PLANE_A_SURF
 * flip; composites the cursor into the frame; falls back to the Limine CPU
 * present (after restoring the Limine FB) on any per-flip failure. */
void                      gfx_use_intel_flip_backend(void);

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

/* Global clip rectangle: scope subsequent primitive drawing to a region
 * (intersected with the screen). The compositor uses this to repaint only
 * a damage rect. gfx_reset_clip() restores whole-screen drawing;
 * gfx_get_clip() reports the active clip (false when none). */
void gfx_set_clip(int x, int y, int w, int h);
void gfx_reset_clip(void);
bool gfx_get_clip(int *x, int *y, int *w, int *h);

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

/* ---- Box blur (Phase 2: acrylic/glass effect) ---------------------
 *
 * In-place 3x3 box blur on a rectangular region of the global back
 * buffer. Used by the compositor to apply a frosted-glass effect
 * behind translucent panels (taskbar, launcher, notifications)
 * before painting them. The region is clipped to back-buffer bounds.
 *
 * `passes` controls quality: 1 = single 3x3 (cheap, blocky),
 * 2 = two passes (approximates 5x5 gaussian), 3 = three passes
 * (smooth, ~7x7 gaussian approximation). Values > 3 are clamped. */
void gfx_box_blur_region(int x, int y, int w, int h, int passes);

/* VSync: returns a monotonically increasing frame counter that ticks
 * on every gfx_flip(). Userland can snapshot this, yield, and poll
 * until it changes to implement a simple vsync-wait. */
uint64_t gfx_frame_count(void);

/* ---- Surface abstraction for off-screen / per-window rendering ----
 *
 * A gfx_surface is a simple pixel buffer that drawing primitives can
 * target instead of the global compositor back buffer. Window backbufs,
 * off-screen scratch buffers, and image decode targets all become
 * surfaces. The global back buffer is itself a surface internally. */
struct gfx_surface {
    uint32_t *pixels;
    int       width;
    int       height;
};

/* Get the global back buffer as a surface (convenience). */
void gfx_surface_from_backbuf(struct gfx_surface *out);

/* Surface-targeted primitives. These do NOT touch the global backbuf
 * or dirty-rect tracking -- they write only to the provided surface.
 * All clip to [0..surface.width) x [0..surface.height). */
void gfx_surface_fill_rect(struct gfx_surface *s, int x, int y, int w, int h, uint32_t color);
void gfx_surface_draw_rect(struct gfx_surface *s, int x, int y, int w, int h, uint32_t color);
void gfx_surface_draw_line(struct gfx_surface *s, int x0, int y0, int x1, int y1, uint32_t color);
void gfx_surface_fill_rounded_rect(struct gfx_surface *s, int x, int y, int w, int h, int radius, uint32_t color);
void gfx_surface_fill_rounded_rect_blend(struct gfx_surface *s, int x, int y, int w, int h, int radius, uint32_t argb);
void gfx_surface_fill_rect_blend(struct gfx_surface *s, int x, int y, int w, int h, uint32_t argb);
void gfx_surface_draw_text(struct gfx_surface *s, int x, int y, const char *str, uint32_t fg, uint32_t bg);
void gfx_surface_draw_text_scaled(struct gfx_surface *s, int x, int y, const char *str, uint32_t fg, uint32_t bg, int scale);
void gfx_surface_blit(struct gfx_surface *s, int dst_x, int dst_y, int w, int h, const uint32_t *src, int src_pitch);
void gfx_surface_blit_blend(struct gfx_surface *s, int dst_x, int dst_y, int w, int h, const uint32_t *src, int src_pitch);

/* Circle primitives (work on surfaces). */
void gfx_surface_fill_circle(struct gfx_surface *s, int cx, int cy, int r, uint32_t color);
void gfx_surface_draw_circle(struct gfx_surface *s, int cx, int cy, int r, uint32_t color);

/* Vertical gradient fill. */
void gfx_surface_gradient_v(struct gfx_surface *s, int x, int y, int w, int h, uint32_t color_top, uint32_t color_bot);

/* Alpha-blended line. */
void gfx_surface_draw_line_blend(struct gfx_surface *s, int x0, int y0, int x1, int y1, uint32_t argb);

/* Read pixels from a surface into a user buffer. Returns number of pixels copied. */
int gfx_surface_get_pixels(const struct gfx_surface *s, int x, int y, int w, int h, uint32_t *dst);

/* Surface-targeted box blur (does not touch global state). */
void gfx_surface_box_blur(struct gfx_surface *s, int x, int y, int w, int h, int passes);

#endif /* TOBYOS_GFX_H */
