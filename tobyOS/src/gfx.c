/* gfx.c -- back-buffered 2D primitives over a swappable display backend.
 *
 * One full-screen kmalloc'd back buffer of WIDTH*HEIGHT 32-bit pixels.
 * Every primitive writes to the back buffer; gfx_flip() pushes it to
 * the screen in one pass. The push is dispatched through a
 * struct gfx_backend installed at boot:
 *
 *   - Default backend (this file): memcpy back -> Limine framebuffer.
 *     The framebuffer's "pitch in pixels" can be larger than WIDTH
 *     (Limine sometimes pads rows to a cache-friendly multiple), so
 *     the flush is row-by-row using the recorded pitch. This is the
 *     UNIVERSAL fallback -- every BIOS/UEFI/GOP/std-vga path lands
 *     here, regardless of whether real GPU silicon is present.
 *
 *   - virtio-gpu backend (src/virtio_gpu.c): on a machine with a
 *     virtio-gpu PCI device, the driver installs a backend whose
 *     flip() copies into a host-side resource via TRANSFER_TO_HOST_2D
 *     + RESOURCE_FLUSH instead. The Limine FB pointer becomes
 *     irrelevant on that path; we still keep it in g.fb so that
 *     swapping backends later (or falling back) is a one-pointer write.
 *
 * Clipping: every primitive clamps its rect to the intersection of
 * itself and the back-buffer extent before walking pixels. Negative
 * x/y is supported -- the rect is just trimmed at the left/top edge.
 */

#include <tobyos/gfx.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/display.h>

extern const uint8_t font8x8_basic[128][8];

/* Forward decls: default backend lives at the bottom of this file. */
static void limine_flip(void);
static void limine_present_rect(int x, int y, int w, int h);
static int  limine_describe(char *extra, int cap);

/* M27B: Limine framebuffer backend. The default. Implements the full
 * gfx_backend ABI (flip + present_rect + describe). present_rect lets
 * the M27E dirty-rectangle path push a partial update; on the Limine
 * path that's still a CPU memcpy so the win is bounded but real
 * (typical desktop motion only dirties ~10 % of the surface). */
static const struct gfx_backend g_backend_limine = {
    .flip            = limine_flip,
    .present_rect    = limine_present_rect,
    .describe        = limine_describe,
    .name            = "limine-fb",
    .bytes_per_pixel = 4,
};

static struct {
    uint32_t *fb;          /* hardware framebuffer, in HHDM (Limine path) */
    uint32_t *back;        /* our back buffer (kmalloc'd) */
    uint32_t  width;       /* horizontal pixel count */
    uint32_t  height;      /* vertical pixel count   */
    uint32_t  fb_pitch_px; /* hardware pitch in pixels (>= width) */
    bool      ready;
    const struct gfx_backend *backend;   /* never NULL once gfx_init runs */

    /* M27B: dirty-rectangle accumulator. The union of every rect any
     * primitive has touched since the last successful flip(). When
     * `dirty_w == 0 || dirty_h == 0` the rect is empty (flip is a
     * no-op present-wise). When `dirty_full` is true we treat the
     * accumulator as "everything" -- skips per-rect bookkeeping for
     * paths like gfx_clear() that touch every pixel. */
    int  dirty_x, dirty_y, dirty_w, dirty_h;
    bool dirty_full;

    /* M27E: per-flip statistics. Bumped from gfx_flip and exposed via
     * gfx_present_stats() so the compositor self-test can prove that
     * partial-present is actually shrinking the work the backend does
     * each frame. Wraps at 2^64 frames -- about 5 billion years at
     * 100 Hz. */
    struct gfx_present_stats stats;
} g = {
    .backend = &g_backend_limine,
};

/* Reset the dirty-rect accumulator to "nothing dirty". Called after a
 * successful present so subsequent primitives start a fresh region. */
static inline void dirty_reset(void) {
    g.dirty_x = g.dirty_y = g.dirty_w = g.dirty_h = 0;
    g.dirty_full = false;
}

/* Union the supplied rect into the accumulator. Caller is responsible
 * for clamping to the back buffer first (clip_rect does this) so we
 * don't ever store off-surface dirty coords. */
static void dirty_union(int x, int y, int w, int h) {
    if (!g.ready || w <= 0 || h <= 0) return;
    if (g.dirty_full) return;        /* already covers everything */
    if (g.dirty_w == 0 || g.dirty_h == 0) {
        g.dirty_x = x; g.dirty_y = y;
        g.dirty_w = w; g.dirty_h = h;
        return;
    }
    int x0 = g.dirty_x, y0 = g.dirty_y;
    int x1 = g.dirty_x + g.dirty_w, y1 = g.dirty_y + g.dirty_h;
    if (x       < x0) x0 = x;
    if (y       < y0) y0 = y;
    if (x + w   > x1) x1 = x + w;
    if (y + h   > y1) y1 = y + h;
    g.dirty_x = x0; g.dirty_y = y0;
    g.dirty_w = x1 - x0; g.dirty_h = y1 - y0;
    /* If the union covers >=95% of the surface, just promote to full
     * to skip the per-row present_rect overhead on partials. */
    uint64_t area_dirty = (uint64_t)g.dirty_w * (uint64_t)g.dirty_h;
    uint64_t area_full  = (uint64_t)g.width  * (uint64_t)g.height;
    if (area_full && area_dirty * 100u >= area_full * 95u) {
        g.dirty_full = true;
    }
}

void gfx_mark_dirty_rect(int x, int y, int w, int h) {
    if (!g.ready) return;
    /* Clip to the back buffer extent before storing. Off-surface
     * coordinates are silently dropped (matches every primitive's
     * clip_rect path). */
    int cx = x, cy = y, cw = w, ch = h;
    if (cw <= 0 || ch <= 0) return;
    int x0 = cx, y0 = cy, x1 = cx + cw, y1 = cy + ch;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g.width)  x1 = (int)g.width;
    if (y1 > (int)g.height) y1 = (int)g.height;
    if (x1 <= x0 || y1 <= y0) return;
    dirty_union(x0, y0, x1 - x0, y1 - y0);
}

void gfx_mark_dirty_full(void) {
    if (!g.ready) return;
    g.dirty_full = true;
}

void gfx_dirty_clear(void) {
    /* Doesn't require g.ready -- harmless before init. */
    dirty_reset();
}

void gfx_present_stats(struct gfx_present_stats *out) {
    if (!out) return;
    *out = g.stats;
}

bool gfx_dirty_get(int *x, int *y, int *w, int *h) {
    if (!g.ready) return false;
    if (g.dirty_full) {
        if (x) *x = 0; if (y) *y = 0;
        if (w) *w = (int)g.width; if (h) *h = (int)g.height;
        return true;
    }
    if (g.dirty_w <= 0 || g.dirty_h <= 0) return false;
    if (x) *x = g.dirty_x; if (y) *y = g.dirty_y;
    if (w) *w = g.dirty_w; if (h) *h = g.dirty_h;
    return true;
}

bool     gfx_ready (void) { return g.ready; }
uint32_t gfx_width (void) { return g.width;  }
uint32_t gfx_height(void) { return g.height; }
uint32_t *gfx_backbuf(void) { return g.ready ? g.back : 0; }

bool gfx_init(void *fb, uint64_t pitch, uint32_t width, uint32_t height) {
    if (!fb || width == 0 || height == 0 || pitch < (uint64_t)width * 4) {
        return false;
    }
    /* The back buffer can be large (3 MiB at 1024x768). The heap grows
     * arenas via vmm_map so this just means several extra pages get
     * stitched into the heap region -- fine. */
    size_t bytes = (size_t)width * height * 4u;
    uint32_t *back = (uint32_t *)kmalloc(bytes);
    if (!back) {
        kprintf("[gfx] OOM allocating %lu-byte back buffer\n",
                (unsigned long)bytes);
        return false;
    }
    memset(back, 0, bytes);

    g.fb          = (uint32_t *)fb;
    g.back        = back;
    g.width       = width;
    g.height      = height;
    g.fb_pitch_px = (uint32_t)(pitch / 4);
    g.ready       = true;
    /* Don't reset g.backend here: a driver might have set its preferred
     * backend before gfx_init() (in practice the order is the other way
     * round, but being defensive is cheap). */
    if (!g.backend) g.backend = &g_backend_limine;
    kprintf("[gfx] back buffer %ux%u (%lu KiB) ready, fb_pitch=%u px, "
            "backend=%s\n",
            width, height, (unsigned long)(bytes / 1024), g.fb_pitch_px,
            g.backend->name);
    return true;
}

/* ---- helpers --------------------------------------------------- */

static inline void put_back(int x, int y, uint32_t color) {
    g.back[(uint32_t)y * g.width + (uint32_t)x] = color;
}

/* Clamp a (x, y, w, h) rect to the back buffer. Returns false if the
 * intersection is empty. Updates the rect in place. */
static bool clip_rect(int *x, int *y, int *w, int *h) {
    if (*w <= 0 || *h <= 0) return false;
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g.width)  x1 = (int)g.width;
    if (y1 > (int)g.height) y1 = (int)g.height;
    if (x1 <= x0 || y1 <= y0) return false;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}

/* ---- primitives ------------------------------------------------ */

void gfx_clear(uint32_t color) {
    if (!g.ready) return;
    size_t n = (size_t)g.width * g.height;
    for (size_t i = 0; i < n; i++) g.back[i] = color;
    g.dirty_full = true;          /* gfx_clear touches every pixel */
}

void gfx_set_pixel(int x, int y, uint32_t color) {
    if (!g.ready) return;
    if (x < 0 || y < 0 || x >= (int)g.width || y >= (int)g.height) return;
    put_back(x, y, color);
    dirty_union(x, y, 1, 1);
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g.ready) return;
    if (!clip_rect(&x, &y, &w, &h)) return;
    /* M27B: explicit bounds-check assertion. clip_rect() already
     * narrows to the back buffer; if either coord falls outside the
     * surface here it means the back buffer was reallocated mid-call
     * (a serious kernel bug). Belt-and-braces: bail out instead of
     * writing past the kmalloc region. */
    if (x < 0 || y < 0 || x + w > (int)g.width || y + h > (int)g.height) {
        kprintf("[gfx] fill_rect bounds violation %d,%d %dx%d in %ux%u "
                "-- skipping (kernel bug?)\n",
                x, y, w, h, g.width, g.height);
        return;
    }
    for (int dy = 0; dy < h; dy++) {
        uint32_t *row = &g.back[(uint32_t)(y + dy) * g.width + (uint32_t)x];
        for (int dx = 0; dx < w; dx++) row[dx] = color;
    }
    dirty_union(x, y, w, h);
}

void gfx_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g.ready || w <= 0 || h <= 0) return;
    /* Top + bottom edges. */
    gfx_fill_rect(x, y,            w, 1, color);
    gfx_fill_rect(x, y + h - 1,    w, 1, color);
    /* Left + right edges (avoid double-painting corners). */
    gfx_fill_rect(x,           y + 1, 1, h - 2, color);
    gfx_fill_rect(x + w - 1,   y + 1, 1, h - 2, color);
}

static void draw_glyph(int x, int y, char c, uint32_t fg, uint32_t bg) {
    uint8_t ch = (uint8_t)c;
    if (ch >= 128) ch = '?';
    const uint8_t *rows = font8x8_basic[ch];
    bool transparent_bg = (bg == GFX_TRANSPARENT);
    for (int dy = 0; dy < 8; dy++) {
        int py = y + dy;
        if (py < 0 || py >= (int)g.height) continue;
        uint8_t bits = rows[dy];
        for (int dx = 0; dx < 8; dx++) {
            int px = x + dx;
            if (px < 0 || px >= (int)g.width) continue;
            bool on = ((bits >> dx) & 1u) != 0;
            if (on)              put_back(px, py, fg);
            else if (!transparent_bg) put_back(px, py, bg);
        }
    }
}

void gfx_draw_text(int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    if (!g.ready || !s) return;
    int cx = x, cy = y;
    int min_x = x, min_y = y, max_x = x, max_y = y + 8;
    bool any = false;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; cy += 8; continue; }
        draw_glyph(cx, cy, *s, fg, bg);
        any = true;
        if (cx        < min_x) min_x = cx;
        if (cy        < min_y) min_y = cy;
        if (cx + 8    > max_x) max_x = cx + 8;
        if (cy + 8    > max_y) max_y = cy + 8;
        cx += 8;
    }
    /* M27B: union the bounding box of the entire text run instead of
     * one rect per glyph -- many fewer accumulator updates for long
     * lines. */
    if (any) gfx_mark_dirty_rect(min_x, min_y, max_x - min_x, max_y - min_y);
}

/* ---- M27D: scaled/smoothed bitmap text ------------------------- */

/* Read one bit from font8x8_basic. Returns 0/1, no clipping (caller
 * responsibility). Out-of-range chars fold to '?'. */
static inline int font_bit(uint8_t ch, int dx, int dy) {
    if (ch >= 128) ch = '?';
    if (dx < 0 || dx >= 8 || dy < 0 || dy >= 8) return 0;
    return (font8x8_basic[ch][dy] >> dx) & 1;
}

/* Draw one glyph at integer scale `s`. With s=1 this is identical
 * to draw_glyph. We honour the same transparent-bg sentinel. */
static void draw_glyph_scaled(int gx, int gy, char c,
                              uint32_t fg, uint32_t bg, int s) {
    uint8_t ch = (uint8_t)c;
    bool transparent_bg = (bg == GFX_TRANSPARENT);
    for (int dy = 0; dy < 8; dy++) {
        for (int dx = 0; dx < 8; dx++) {
            int on = font_bit(ch, dx, dy);
            if (!on && transparent_bg) continue;
            uint32_t col = on ? fg : bg;
            int x0 = gx + dx * s;
            int y0 = gy + dy * s;
            for (int sy = 0; sy < s; sy++) {
                int py = y0 + sy;
                if (py < 0 || py >= (int)g.height) continue;
                for (int sx = 0; sx < s; sx++) {
                    int px = x0 + sx;
                    if (px < 0 || px >= (int)g.width) continue;
                    put_back(px, py, col);
                }
            }
        }
    }
}

/* Smoothed glyph: same as scaled, then a corner-detection pass
 * paints anti-aliased "wedges" wherever an ON pixel meets ON
 * neighbours diagonally without an ON pixel in between. The wedge
 * is a half-alpha blend of fg into the dest, painted in the
 * scale-cell quadrant adjacent to the missing corner.
 *
 * The routine consciously keeps to integer arithmetic and never
 * touches the surface outside the glyph cell (which itself is
 * clipped). It's a fixed +O(scale^2) per glyph extra cost over the
 * unsmoothed path. */
static void draw_glyph_smooth(int gx, int gy, char c,
                              uint32_t fg, uint32_t bg, int s) {
    /* Base layer: hard scaled glyph. */
    draw_glyph_scaled(gx, gy, c, fg, bg, s);
    if (s < 2) return;
    uint8_t ch = (uint8_t)c;
    /* "Half alpha" version of fg used for the corner softening. We
     * blend it over whatever the base layer wrote at that cell -- so
     * it correctly softens against either fg (interior corners) or
     * bg (exterior corners). 0x80 alpha keeps the wedge subtle. */
    uint32_t soft = 0x80000000u | (fg & 0x00FFFFFFu);
    int half = s / 2;
    if (half < 1) half = 1;
    for (int dy = 0; dy < 8; dy++) {
        for (int dx = 0; dx < 8; dx++) {
            int p  = font_bit(ch, dx,     dy);
            if (!p) continue;
            /* For each diagonal neighbour: if it's ON, and the two
             * orthogonals between it and `p` are both OFF, paint a
             * wedge in the quadrant that points at the diagonal. */
            int nE = font_bit(ch, dx + 1, dy);
            int nW = font_bit(ch, dx - 1, dy);
            int nN = font_bit(ch, dx,     dy - 1);
            int nS = font_bit(ch, dx,     dy + 1);
            int nNE = font_bit(ch, dx + 1, dy - 1);
            int nNW = font_bit(ch, dx - 1, dy - 1);
            int nSE = font_bit(ch, dx + 1, dy + 1);
            int nSW = font_bit(ch, dx - 1, dy + 1);
            struct { int diag, ox, oy, qx, qy; } wedges[] = {
                /* corner               quadrant offset within the cell */
                { nNE && !nN && !nE,    half, 0,    half, half },
                { nNW && !nN && !nW,    0,    0,    half, half },
                { nSE && !nS && !nE,    half, half, half, half },
                { nSW && !nS && !nW,    0,    half, half, half },
            };
            int x0 = gx + dx * s;
            int y0 = gy + dy * s;
            for (size_t i = 0; i < sizeof(wedges)/sizeof(wedges[0]); i++) {
                if (!wedges[i].diag) continue;
                gfx_fill_rect_blend(x0 + wedges[i].ox,
                                    y0 + wedges[i].oy,
                                    wedges[i].qx, wedges[i].qy, soft);
            }
        }
    }
}

void gfx_draw_text_scaled(int x, int y, const char *s,
                          uint32_t fg, uint32_t bg, int scale) {
    if (!g.ready || !s) return;
    if (scale < 1) scale = 1;
    int cx = x, cy = y;
    int cell = 8 * scale;
    int min_x = x, min_y = y, max_x = x, max_y = y + cell;
    bool any = false;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; cy += cell; continue; }
        draw_glyph_scaled(cx, cy, *s, fg, bg, scale);
        any = true;
        if (cx              < min_x) min_x = cx;
        if (cy              < min_y) min_y = cy;
        if (cx + cell       > max_x) max_x = cx + cell;
        if (cy + cell       > max_y) max_y = cy + cell;
        cx += cell;
    }
    if (any) gfx_mark_dirty_rect(min_x, min_y,
                                 max_x - min_x, max_y - min_y);
}

void gfx_draw_text_smooth(int x, int y, const char *s,
                          uint32_t fg, uint32_t bg, int scale) {
    if (!g.ready || !s) return;
    if (scale < 2) {
        gfx_draw_text(x, y, s, fg, bg);
        return;
    }
    int cx = x, cy = y;
    int cell = 8 * scale;
    int min_x = x, min_y = y, max_x = x, max_y = y + cell;
    bool any = false;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; cy += cell; continue; }
        draw_glyph_smooth(cx, cy, *s, fg, bg, scale);
        any = true;
        if (cx              < min_x) min_x = cx;
        if (cy              < min_y) min_y = cy;
        if (cx + cell       > max_x) max_x = cx + cell;
        if (cy + cell       > max_y) max_y = cy + cell;
        cx += cell;
    }
    if (any) gfx_mark_dirty_rect(min_x, min_y,
                                 max_x - min_x, max_y - min_y);
}

void gfx_text_bounds(const char *s, int scale, int *out_w, int *out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!s || !*s) return;     /* "" measures as 0x0, callers rely on this */
    if (scale < 1) scale = 1;
    int cell = 8 * scale;
    int max_w = 0, run_w = 0;
    int lines = 1;
    for (const char *p = s; *p; p++) {
        if (*p == '\n') {
            if (run_w > max_w) max_w = run_w;
            run_w = 0;
            lines++;
            continue;
        }
        run_w += cell;
    }
    if (run_w > max_w) max_w = run_w;
    if (out_w) *out_w = max_w;
    if (out_h) *out_h = lines * cell;
}

void gfx_blit(int dst_x, int dst_y, int w, int h,
              const uint32_t *src, int src_pitch) {
    if (!g.ready || !src || w <= 0 || h <= 0 || src_pitch < w) return;
    /* Compute the clipped destination rect, and the matching offset
     * into the source so we don't read out of bounds when the window
     * is partially off-screen. */
    int x = dst_x, y = dst_y, cw = w, ch = h;
    if (!clip_rect(&x, &y, &cw, &ch)) return;
    /* M27B: same bounds-check as fill_rect. */
    if (x < 0 || y < 0 || x + cw > (int)g.width || y + ch > (int)g.height) {
        kprintf("[gfx] blit bounds violation %d,%d %dx%d in %ux%u "
                "-- skipping (kernel bug?)\n",
                x, y, cw, ch, g.width, g.height);
        return;
    }
    int sx = x - dst_x;
    int sy = y - dst_y;
    for (int dy = 0; dy < ch; dy++) {
        const uint32_t *src_row = &src[(sy + dy) * src_pitch + sx];
        uint32_t       *dst_row = &g.back[(uint32_t)(y + dy) * g.width
                                          + (uint32_t)x];
        memcpy(dst_row, src_row, (size_t)cw * 4u);
    }
    dirty_union(x, y, cw, ch);
}

/* ---- M27C: alpha blending ------------------------------------- */

/* Source-over for one ARGB src over one XRGB dst:
 *   out = src.RGB * a + dst.RGB * (255 - a)
 * Result is XRGB (alpha is consumed).
 * Math is per-channel with rounded division by 255 via the
 * (x * a + 127) * 257 / 65536 trick -> equivalent to dividing by 255
 * but emits no DIV on x86_64 (compiler folds to a mul + shift). */
static inline uint32_t blend_src_over(uint32_t dst_xrgb, uint32_t src_argb) {
    uint32_t a = (src_argb >> 24) & 0xFFu;
    if (a == 0)   return dst_xrgb;
    if (a == 255) return src_argb & 0x00FFFFFFu;
    uint32_t inv  = 255u - a;
    uint32_t sR = (src_argb >> 16) & 0xFFu;
    uint32_t sG = (src_argb >> 8 ) & 0xFFu;
    uint32_t sB = (src_argb      ) & 0xFFu;
    uint32_t dR = (dst_xrgb >> 16) & 0xFFu;
    uint32_t dG = (dst_xrgb >> 8 ) & 0xFFu;
    uint32_t dB = (dst_xrgb      ) & 0xFFu;
    uint32_t oR = (sR * a + dR * inv + 127u) / 255u;
    uint32_t oG = (sG * a + dG * inv + 127u) / 255u;
    uint32_t oB = (sB * a + dB * inv + 127u) / 255u;
    return (oR << 16) | (oG << 8) | oB;
}

uint32_t gfx_blend_pixel_argb(uint32_t dst_xrgb, uint32_t src_argb) {
    return blend_src_over(dst_xrgb, src_argb);
}

void gfx_fill_rect_blend(int x, int y, int w, int h, uint32_t argb) {
    if (!g.ready) return;
    /* Trivial alpha shortcuts: 0 = no-op, 255 = opaque path. */
    uint32_t a = (argb >> 24) & 0xFFu;
    if (a == 0) return;
    if (a == 255) { gfx_fill_rect(x, y, w, h, argb & 0x00FFFFFFu); return; }
    if (!clip_rect(&x, &y, &w, &h)) return;
    if (x < 0 || y < 0 || x + w > (int)g.width || y + h > (int)g.height) {
        kprintf("[gfx] fill_rect_blend bounds violation %d,%d %dx%d "
                "in %ux%u -- skipping (kernel bug?)\n",
                x, y, w, h, g.width, g.height);
        return;
    }
    for (int dy = 0; dy < h; dy++) {
        uint32_t *row = &g.back[(uint32_t)(y + dy) * g.width + (uint32_t)x];
        for (int dx = 0; dx < w; dx++) {
            row[dx] = blend_src_over(row[dx], argb);
        }
    }
    dirty_union(x, y, w, h);
}

void gfx_blit_blend(int dst_x, int dst_y, int w, int h,
                    const uint32_t *src, int src_pitch) {
    if (!g.ready || !src || w <= 0 || h <= 0 || src_pitch < w) return;
    int x = dst_x, y = dst_y, cw = w, ch = h;
    if (!clip_rect(&x, &y, &cw, &ch)) return;
    if (x < 0 || y < 0 || x + cw > (int)g.width || y + ch > (int)g.height) {
        kprintf("[gfx] blit_blend bounds violation %d,%d %dx%d in %ux%u "
                "-- skipping (kernel bug?)\n",
                x, y, cw, ch, g.width, g.height);
        return;
    }
    int sx = x - dst_x;
    int sy = y - dst_y;
    for (int dy = 0; dy < ch; dy++) {
        const uint32_t *src_row = &src[(sy + dy) * src_pitch + sx];
        uint32_t       *dst_row = &g.back[(uint32_t)(y + dy) * g.width
                                          + (uint32_t)x];
        for (int dx = 0; dx < cw; dx++) {
            dst_row[dx] = blend_src_over(dst_row[dx], src_row[dx]);
        }
    }
    dirty_union(x, y, cw, ch);
}

/* ---- mouse cursor sprite -------------------------------------- */

/* 12x19 classic NW arrow. '#' = black outline, '.' = white fill, ' ' =
 * transparent. Hand-rolled so we don't pull in another asset file. */
#define CUR_W 12
#define CUR_H 19
static const char g_cursor[CUR_H][CUR_W + 1] = {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#.........# ",
    "#......#####",
    "#...##.#    ",
    "#..# #.#    ",
    "#.#  #.#    ",
    "##    #.#   ",
    "      #.#   ",
    "       #.#  ",
    "       ###  ",
};

void gfx_draw_cursor(int x, int y) {
    if (!g.ready) return;
    for (int dy = 0; dy < CUR_H; dy++) {
        int py = y + dy;
        if (py < 0 || py >= (int)g.height) continue;
        const char *row = g_cursor[dy];
        for (int dx = 0; dx < CUR_W && row[dx]; dx++) {
            int px = x + dx;
            if (px < 0 || px >= (int)g.width) continue;
            char c = row[dx];
            if      (c == '#') put_back(px, py, 0x00000000u);  /* outline */
            else if (c == '.') put_back(px, py, 0x00FFFFFFu);  /* fill    */
            /* anything else: transparent */
        }
    }
    /* M27B: cursor sprite bounding box (whole 12x19 block). The
     * compositor draws the cursor LAST every frame so this rect is
     * almost always already in the dirty union from window paints
     * earlier in the pass; the union with itself is cheap. */
    gfx_mark_dirty_rect(x, y, CUR_W, CUR_H);
}

/* ---- push to display ----------------------------------------- */

/* Default backend: memcpy the back buffer into the Limine framebuffer.
 * Used everywhere unless a higher-fidelity backend (virtio-gpu, etc.)
 * has called gfx_set_backend(). */
static void limine_flip(void) {
    if (!g.fb) return;
    /* When the hardware pitch matches our width (the common QEMU case)
     * one big memcpy beats H separate ones. Otherwise walk row by row. */
    if (g.fb_pitch_px == g.width) {
        memcpy(g.fb, g.back, (size_t)g.width * g.height * 4u);
        return;
    }
    for (uint32_t row = 0; row < g.height; row++) {
        memcpy(&g.fb[row * g.fb_pitch_px],
               &g.back[row * g.width],
               (size_t)g.width * 4u);
    }
}

/* M27B: Limine partial present. Same row-walk as limine_flip() but
 * scoped to a single rect. Used by the dirty-rectangle path; falls
 * back gracefully if anything looks wrong (caller invokes flip()
 * after present_rect either way is fine -- worst case we double-push
 * a few pixels, never crash). */
static void limine_present_rect(int x, int y, int w, int h) {
    if (!g.fb || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0) return;
    if (x + w > (int)g.width)  w = (int)g.width  - x;
    if (y + h > (int)g.height) h = (int)g.height - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        memcpy(&g.fb [(uint32_t)(y + row) * g.fb_pitch_px + (uint32_t)x],
               &g.back[(uint32_t)(y + row) * g.width      + (uint32_t)x],
               (size_t)w * 4u);
    }
}

/* M27B: limine "describe" -- short string for displayinfo --json. */
static int limine_describe(char *extra, int cap) {
    if (!extra || cap <= 0) return 0;
    /* "fbpitch=%u px back=%lu KiB" using only conversions libtoby's
     * printf supports. Truncated cleanly if cap is small. */
    return ksnprintf(extra, (size_t)cap, "fb_pitch_px=%u back_kib=%lu",
                     g.fb_pitch_px,
                     (unsigned long)((size_t)g.width * g.height * 4u / 1024u));
}

void gfx_flip(void) {
    if (!g.ready) return;
    const struct gfx_backend *b = g.backend ? g.backend : &g_backend_limine;
    int dx, dy, dw, dh;
    bool have_dirty = gfx_dirty_get(&dx, &dy, &dw, &dh);
    bool covers_all = !have_dirty
                   || g.dirty_full
                   || (dx == 0 && dy == 0
                       && dw == (int)g.width && dh == (int)g.height);
    g.stats.total_flips++;
    if (!have_dirty) {
        /* Empty flip: nothing was marked dirty since the last present.
         * We still call flip() defensively (caller wants a present),
         * but bucket it into `empty_flips` only -- NOT also into
         * full_flips -- so the invariant
         *     total_flips == full_flips + partial_flips + empty_flips
         * stays clean for downstream test code. */
        g.stats.empty_flips++;
        b->flip();
    } else if (!covers_all && b->present_rect) {
        g.stats.partial_flips++;
        g.stats.partial_pixels += (uint64_t)dw * (uint64_t)dh;
        b->present_rect(dx, dy, dw, dh);
    } else {
        g.stats.full_flips++;
        g.stats.full_pixels += (uint64_t)g.width * (uint64_t)g.height;
        b->flip();
    }
    dirty_reset();
    display_record_flip();
}

void gfx_set_backend(const struct gfx_backend *backend) {
    g.backend = backend ? backend : &g_backend_limine;
    /* Backend swap means the next flip MUST present everything --
     * the new backend has no state for the previously-dirty regions.
     * Mark full so we don't accidentally call present_rect() on an
     * empty dirty rect after the swap. */
    g.dirty_full = true;
    kprintf("[gfx] backend = %s\n", g.backend->name);
}

const struct gfx_backend *gfx_get_backend(void) {
    return g.backend;
}
