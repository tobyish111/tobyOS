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
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>

extern const uint8_t font8x8_basic[128][8];
extern const uint8_t font8x16_data[128][16];

/* Forward decls: default backend lives at the bottom of this file. */
static void limine_flip(void);
static void limine_present_rect(int x, int y, int w, int h);
static int  limine_describe(char *extra, int cap);

/* The Stage-3 Intel GT blitter backend (defined near gfx_set_backend
 * below). Forward-declared here so the cursor-overlay support check can
 * recognise it as a direct-scanout backend. */
static const struct gfx_backend g_backend_intel;
/* The Stage-4 Intel GT page-flip backend (also below). It scans its OWN
 * buffers, not the Limine FB, so the software cursor overlay can't reach the
 * scanout -- the backend composites the cursor into the flipped frame
 * instead. Forward-declared so cursor_overlay_supported() excludes it. */
static const struct gfx_backend g_backend_intel_flip;

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
    uint32_t *back;        /* our back buffer (kmalloc'd) -- currently active */
    uint32_t *back2;       /* second back buffer for double-buffering */
    int       active_buf;  /* 0 = back is active, 1 = back2 is active */
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

    /* Global clip rectangle. When active, clip_rect() intersects every
     * primitive's target rect with this, so the compositor can scope a
     * whole paint pass to a damage region (damage-tracked compositing)
     * without touching each draw call. Default = inactive (whole screen).
     * Over-drawing outside the clip would only reproduce identical pixels
     * on the persisted back buffer, so this is a perf lever, not a
     * correctness dependency -- with the exception of read-modify-write
     * blends that read the back buffer, which the caller must keep out of
     * the clipped path (see gui.c mid-fade guard). */
    bool clip_active;
    int  clip_x, clip_y, clip_w, clip_h;

    /* M27E: per-flip statistics. Bumped from gfx_flip and exposed via
     * gfx_present_stats() so the compositor self-test can prove that
     * partial-present is actually shrinking the work the backend does
     * each frame. Wraps at 2^64 frames -- about 5 billion years at
     * 100 Hz. */
    struct gfx_present_stats stats;

    /* VSync frame counter: incremented on every successful gfx_flip(). */
    volatile uint64_t frame_count;
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

static uint32_t *gfx_hw_fb_ptr(void) {
    if (!g.fb) return 0;
    uint64_t addr = (uint64_t)(uintptr_t)g.fb;
    uint64_t hhdm = pmm_hhdm_offset();
    if (hhdm && addr < hhdm)
        addr += hhdm;
    return (uint32_t *)(uintptr_t)addr;
}

static bool gfx_hw_fb_writable(void) {
    uint32_t *fb = gfx_hw_fb_ptr();
    if (!fb) return false;
    uint64_t hhdm = pmm_hhdm_offset();
    if (hhdm == 0) return true;
    /* vmm_init has not built a PML4 yet -- still on Limine tables. */
    if (vmm_kernel_pml4_phys() == 0) return true;
    return vmm_translate((uint64_t)(uintptr_t)fb) != 0;
}

bool gfx_init(void *fb, uint64_t pitch, uint32_t width, uint32_t height) {
    if (!fb || width == 0 || height == 0 || pitch < (uint64_t)width * 4) {
        return false;
    }

    uint64_t addr = (uint64_t)(uintptr_t)fb;
    uint64_t hhdm = pmm_hhdm_offset();
    if (hhdm && addr < hhdm)
        addr += hhdm;
    size_t bytes = (size_t)width * height * 4u;
    uint32_t *back = (uint32_t *)kmalloc(bytes);
    if (!back) {
        kprintf("[gfx] OOM allocating %lu-byte back buffer\n",
                (unsigned long)bytes);
        return false;
    }
    memset(back, 0, bytes);

    uint32_t *back2 = (uint32_t *)kmalloc(bytes);
    if (!back2) {
        kprintf("[gfx] OOM allocating second back buffer -- single-buffer mode\n");
    } else {
        memset(back2, 0, bytes);
    }

    g.fb          = (uint32_t *)(uintptr_t)addr;
    g.back        = back;
    g.back2       = back2;
    g.active_buf  = 0;
    g.width       = width;
    g.height      = height;
    g.fb_pitch_px = (uint32_t)(pitch / 4);
    g.ready       = true;
    if (!g.backend) g.backend = &g_backend_limine;
    kprintf("[gfx] back buffer %ux%u (%lu KiB) ready%s, fb_pitch=%u px, "
            "backend=%s\n",
            width, height, (unsigned long)(bytes / 1024),
            back2 ? " [double-buffered]" : "",
            g.fb_pitch_px,
            g.backend->name);
    return true;
}

void gfx_sync_framebuffer(void *fb, uint64_t pitch, uint32_t width, uint32_t height) {
    if (!g.ready || !fb || width == 0 || height == 0 || pitch < (uint64_t)width * 4)
        return;
    uint64_t addr = (uint64_t)(uintptr_t)fb;
    uint64_t hhdm = pmm_hhdm_offset();
    if (hhdm && addr < hhdm)
        addr += hhdm;
    g.fb          = (uint32_t *)(uintptr_t)addr;
    g.fb_pitch_px = (uint32_t)(pitch / 4);
    g.width       = width;
    g.height      = height;
}

/* ---- helpers --------------------------------------------------- */

static inline void put_back(int x, int y, uint32_t color) {
    g.back[(uint32_t)y * g.width + (uint32_t)x] = color;
}

/* Clamp a (x, y, w, h) rect to the back buffer AND the active clip rect
 * (if any). Returns false if the intersection is empty. Updates the rect
 * in place. */
static bool clip_rect(int *x, int *y, int *w, int *h) {
    if (*w <= 0 || *h <= 0) return false;
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g.width)  x1 = (int)g.width;
    if (y1 > (int)g.height) y1 = (int)g.height;
    if (g.clip_active) {
        if (x0 < g.clip_x)             x0 = g.clip_x;
        if (y0 < g.clip_y)             y0 = g.clip_y;
        if (x1 > g.clip_x + g.clip_w)  x1 = g.clip_x + g.clip_w;
        if (y1 > g.clip_y + g.clip_h)  y1 = g.clip_y + g.clip_h;
    }
    if (x1 <= x0 || y1 <= y0) return false;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}

/* Scope subsequent primitive drawing to a rectangle (intersected with
 * the screen). Used by the compositor to repaint only a damage region.
 * gfx_reset_clip() restores whole-screen drawing. */
void gfx_set_clip(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) { g.clip_active = false; return; }
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g.width)  x1 = (int)g.width;
    if (y1 > (int)g.height) y1 = (int)g.height;
    if (x1 <= x0 || y1 <= y0) { g.clip_active = false; return; }
    g.clip_x = x0; g.clip_y = y0;
    g.clip_w = x1 - x0; g.clip_h = y1 - y0;
    g.clip_active = true;
}

void gfx_reset_clip(void) { g.clip_active = false; }

/* Report the active clip (false when none). Lets a direct back-buffer
 * writer (e.g. paint_wallpaper's cache blit) scope itself to match. */
bool gfx_get_clip(int *x, int *y, int *w, int *h) {
    if (!g.clip_active) return false;
    if (x) *x = g.clip_x;
    if (y) *y = g.clip_y;
    if (w) *w = g.clip_w;
    if (h) *h = g.clip_h;
    return true;
}

/* ---- primitives ------------------------------------------------ */

void gfx_clear(uint32_t color) {
    if (!g.ready) return;
    size_t n = (size_t)g.width * g.height;
    uint64_t pair = ((uint64_t)color << 32) | (uint64_t)color;
    size_t qwords = n / 2;
    uint64_t *dst = (uint64_t *)g.back;
    __asm__ volatile("rep stosq"
        : "+D"(dst), "+c"(qwords) : "a"(pair) : "memory");
    if (n & 1) g.back[n - 1] = color;
    g.dirty_full = true;
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
    if (x < 0 || y < 0 || x + w > (int)g.width || y + h > (int)g.height) {
        kprintf("[gfx] fill_rect bounds violation %d,%d %dx%d in %ux%u "
                "-- skipping (kernel bug?)\n",
                x, y, w, h, g.width, g.height);
        return;
    }
    /* Fast path: use 64-bit stores for wide fills. Pack two pixels
     * into one qword so the inner loop does half the store ops. */
    uint64_t pair = ((uint64_t)color << 32) | (uint64_t)color;
    for (int dy = 0; dy < h; dy++) {
        uint32_t *row = &g.back[(uint32_t)(y + dy) * g.width + (uint32_t)x];
        int dx = 0;
        int w8 = w & ~1;  /* even count for qword pairs */
        uint64_t *row64 = (uint64_t *)row;
        for (; dx < w8; dx += 2) {
            row64[dx >> 1] = pair;
        }
        if (dx < w) row[dx] = color;
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

/* ---- lines & rounded rectangles -------------------------------- */

void gfx_hline(int x, int y, int w, uint32_t color) {
    if (!g.ready || w <= 0) return;
    int h = 1;
    if (!clip_rect(&x, &y, &w, &h)) return;
    uint32_t *row = &g.back[(uint32_t)y * g.width + (uint32_t)x];
    for (int i = 0; i < w; i++) row[i] = color;
    dirty_union(x, y, w, 1);
}

void gfx_vline(int x, int y, int h, uint32_t color) {
    if (!g.ready || h <= 0) return;
    int w = 1;
    if (!clip_rect(&x, &y, &w, &h)) return;
    uint32_t *col = &g.back[(uint32_t)y * g.width + (uint32_t)x];
    for (int i = 0; i < h; i++) { *col = color; col += g.width; }
    dirty_union(x, y, 1, h);
}

void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    if (!g.ready) return;
    int dx = x1 - x0, dy = y1 - y0;
    int sx = (dx > 0) ? 1 : -1;
    int sy = (dy > 0) ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    int minx = x0, maxx = x0, miny = y0, maxy = y0;
    for (;;) {
        gfx_set_pixel(x0, y0, color);
        if (x0 < minx) minx = x0; if (x0 > maxx) maxx = x0;
        if (y0 < miny) miny = y0; if (y0 > maxy) maxy = y0;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
    gfx_mark_dirty_rect(minx, miny, maxx - minx + 1, maxy - miny + 1);
}

void gfx_fill_rounded_rect(int x, int y, int w, int h, int radius, uint32_t color) {
    if (!g.ready || w <= 0 || h <= 0) return;
    if (radius < 0) radius = 0;
    int max_r = (w < h ? w : h) / 2;
    if (radius > max_r) radius = max_r;
    if (radius == 0) { gfx_fill_rect(x, y, w, h, color); return; }

    for (int row = 0; row < h; row++) {
        int inset = 0;
        if (row < radius) {
            int dy = radius - row - 1;
            int r2 = radius * radius, dy2 = dy * dy;
            int sx;
            for (sx = 0; sx < radius; sx++) {
                if ((radius - sx) * (radius - sx) + dy2 <= r2) break;
            }
            inset = sx;
        } else if (row >= h - radius) {
            int dy = row - (h - radius);
            int r2 = radius * radius, dy2 = dy * dy;
            int sx;
            for (sx = 0; sx < radius; sx++) {
                if ((radius - sx) * (radius - sx) + dy2 <= r2) break;
            }
            inset = sx;
        }
        int rw = w - 2 * inset;
        if (rw > 0) gfx_fill_rect(x + inset, y + row, rw, 1, color);
    }
}

void gfx_fill_rounded_rect_blend(int x, int y, int w, int h, int radius, uint32_t argb) {
    if (!g.ready || w <= 0 || h <= 0) return;
    if (radius < 0) radius = 0;
    int max_r = (w < h ? w : h) / 2;
    if (radius > max_r) radius = max_r;
    if (radius == 0) { gfx_fill_rect_blend(x, y, w, h, argb); return; }

    for (int row = 0; row < h; row++) {
        int inset = 0;
        if (row < radius) {
            int dy = radius - row - 1;
            int r2 = radius * radius, dy2 = dy * dy;
            int sx;
            for (sx = 0; sx < radius; sx++) {
                if ((radius - sx) * (radius - sx) + dy2 <= r2) break;
            }
            inset = sx;
        } else if (row >= h - radius) {
            int dy = row - (h - radius);
            int r2 = radius * radius, dy2 = dy * dy;
            int sx;
            for (sx = 0; sx < radius; sx++) {
                if ((radius - sx) * (radius - sx) + dy2 <= r2) break;
            }
            inset = sx;
        }
        int rw = w - 2 * inset;
        if (rw > 0) gfx_fill_rect_blend(x + inset, y + row, rw, 1, argb);
    }
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

/* ---- 8x16 VGA bitmap text ------------------------------------- */

void gfx_draw_text16(int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    if (!g.ready || !s) return;
    bool transparent_bg = (bg == GFX_TRANSPARENT);
    int ox = x;
    int min_x = x, min_y = y, max_x = x, max_y = y + 16;
    bool any = false;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 128) c = '?';
        const uint8_t *glyph = font8x16_data[c];
        for (int row = 0; row < 16; row++) {
            int py = y + row;
            if (py < 0 || py >= (int)g.height) continue;
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                int px = ox + col;
                if (px < 0 || px >= (int)g.width) continue;
                bool on = (bits >> (7 - col)) & 1;
                if (on)
                    put_back(px, py, fg);
                else if (!transparent_bg)
                    put_back(px, py, bg);
            }
        }
        any = true;
        if (ox        < min_x) min_x = ox;
        if (ox + 8    > max_x) max_x = ox + 8;
        if (y + 16    > max_y) max_y = y + 16;
        ox += 8;
    }
    if (any) gfx_mark_dirty_rect(min_x, min_y, max_x - min_x, max_y - min_y);
}

void gfx_text16_bounds(const char *s, int *out_w, int *out_h) {
    int len = 0;
    if (s) { for (const char *p = s; *p; p++) len++; }
    if (out_w) *out_w = len * 8;
    if (out_h) *out_h = 16;
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

/* Source-over blend using the SIMD-friendly "parallel channel" trick:
 * Process R+B in one 32-bit word, G in another, avoiding per-channel
 * extract/insert. Uses (x * a + 128) >> 8 ≈ x * a / 255. */
static inline uint32_t blend_src_over(uint32_t dst_xrgb, uint32_t src_argb) {
    uint32_t a = src_argb >> 24;
    if (__builtin_expect(a == 0, 0))   return dst_xrgb;
    if (__builtin_expect(a == 255, 0)) return src_argb & 0x00FFFFFFu;
    uint32_t inv = 255u - a;
    /* RB channels packed: mask out G, multiply both R and B in parallel */
    uint32_t src_rb = src_argb & 0x00FF00FFu;
    uint32_t dst_rb = dst_xrgb & 0x00FF00FFu;
    uint32_t out_rb = (src_rb * a + dst_rb * inv + 0x00800080u) >> 8;
    out_rb &= 0x00FF00FFu;
    /* G channel */
    uint32_t src_g = (src_argb >> 8) & 0xFFu;
    uint32_t dst_g = (dst_xrgb >> 8) & 0xFFu;
    uint32_t out_g = ((src_g * a + dst_g * inv + 128u) >> 8) & 0xFFu;
    return out_rb | (out_g << 8);
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

static struct {
    bool visible;
    int x;
    int y;
    uint32_t saved[CUR_W * CUR_H];
    uint8_t valid[CUR_W * CUR_H];
} g_cursor_overlay;

void gfx_draw_cursor(int x, int y) {
    if (!g.ready) return;
    /* Semi-transparent shadow offset by (2,2) for depth. */
    for (int dy = 0; dy < CUR_H; dy++) {
        int py = y + dy + 2;
        if (py < 0 || py >= (int)g.height) continue;
        const char *row = g_cursor[dy];
        for (int dx = 0; dx < CUR_W && row[dx]; dx++) {
            int px = x + dx + 2;
            if (px < 0 || px >= (int)g.width) continue;
            char c = row[dx];
            if (c == '#' || c == '.') {
                uint32_t dst = g.back[(uint32_t)py * g.width + (uint32_t)px];
                uint32_t blended = blend_src_over(dst, 0x40000000u);
                put_back(px, py, blended);
            }
        }
    }
    /* Main cursor sprite */
    for (int dy = 0; dy < CUR_H; dy++) {
        int py = y + dy;
        if (py < 0 || py >= (int)g.height) continue;
        const char *row = g_cursor[dy];
        for (int dx = 0; dx < CUR_W && row[dx]; dx++) {
            int px = x + dx;
            if (px < 0 || px >= (int)g.width) continue;
            char c = row[dx];
            if      (c == '#') put_back(px, py, 0x00000000u);
            else if (c == '.') put_back(px, py, 0x00FFFFFFu);
        }
    }
    gfx_mark_dirty_rect(x, y, CUR_W + 2, CUR_H + 2);
}

/* Stage-4 hardware cursor plane (src/gpu_intel_modeset.c). */
int  intel_gt_cursor_active(void);
void intel_gt_cursor_move(int x, int y);

/* True when the active backend provides a hardware cursor plane (the Intel
 * page-flip backend with its cursor plane up). The compositor uses this to
 * move the pointer via a register poke instead of invalidating + flipping. */
bool gfx_hw_cursor_available(void) {
    return (g.backend == &g_backend_intel_flip) && intel_gt_cursor_active();
}

void gfx_hw_cursor_move(int x, int y) {
    if (gfx_hw_cursor_available()) intel_gt_cursor_move(x, y);
}

static bool cursor_overlay_supported(void) {
    const struct gfx_backend *b = g.backend ? g.backend : &g_backend_limine;
    /* The overlay draws the cursor directly into the Limine scanout FB via
     * the CPU. That's valid for the default limine memcpy backend AND for the
     * Stage-3 Intel GT backend -- both keep g.fb pointing at real scanout
     * memory, and the GT backend blits each frame into that SAME scanout, so
     * the cursor (drawn on top after every present) behaves identically. It
     * is NOT valid for backends like virtio-gpu whose present path does not
     * go through the linear FB pointer. */
    return g.ready && g.fb && (b == &g_backend_limine || b == &g_backend_intel);
}

static inline uint32_t *fb_pixel_ptr(int x, int y) {
    uint32_t *fb = gfx_hw_fb_ptr();
    if (!fb) return g.fb;
    return &fb[(uint32_t)y * g.fb_pitch_px + (uint32_t)x];
}

void gfx_cursor_overlay_hide(void) {
    if (!cursor_overlay_supported() || !g_cursor_overlay.visible) {
        g_cursor_overlay.visible = false;
        return;
    }

    for (int dy = 0; dy < CUR_H; dy++) {
        int py = g_cursor_overlay.y + dy;
        if (py < 0 || py >= (int)g.height) continue;
        for (int dx = 0; dx < CUR_W; dx++) {
            int px = g_cursor_overlay.x + dx;
            int idx = dy * CUR_W + dx;
            if (px < 0 || px >= (int)g.width || !g_cursor_overlay.valid[idx]) continue;
            *fb_pixel_ptr(px, py) = g_cursor_overlay.saved[idx];
        }
    }

    g_cursor_overlay.visible = false;
}

void gfx_cursor_overlay_show(int x, int y) {
    /* Always record the position: the Stage-4 page-flip backend disables the
     * direct-scanout overlay but reads this to composite the cursor into the
     * flipped frame. */
    g_cursor_overlay.x = x;
    g_cursor_overlay.y = y;
    /* Hardware cursor plane: just reposition it (keeps it synced after each
     * flip and handles the initial position). */
    if (gfx_hw_cursor_available()) { gfx_hw_cursor_move(x, y); return; }
    if (!cursor_overlay_supported()) return;
    for (int i = 0; i < CUR_W * CUR_H; i++) {
        g_cursor_overlay.saved[i] = 0;
        g_cursor_overlay.valid[i] = 0;
    }

    for (int dy = 0; dy < CUR_H; dy++) {
        int py = y + dy;
        if (py < 0 || py >= (int)g.height) continue;
        const char *row = g_cursor[dy];
        for (int dx = 0; dx < CUR_W && row[dx]; dx++) {
            int px = x + dx;
            int idx = dy * CUR_W + dx;
            if (px < 0 || px >= (int)g.width) continue;

            uint32_t *p = fb_pixel_ptr(px, py);
            g_cursor_overlay.saved[idx] = *p;
            g_cursor_overlay.valid[idx] = 1;

            char c = row[dx];
            if      (c == '#') *p = 0x00000000u;
            else if (c == '.') *p = 0x00FFFFFFu;
        }
    }

    g_cursor_overlay.visible = true;
}

void gfx_cursor_overlay_move(int x, int y) {
    g_cursor_overlay.x = x;
    g_cursor_overlay.y = y;
    if (gfx_hw_cursor_available()) { gfx_hw_cursor_move(x, y); return; }
    if (!cursor_overlay_supported()) return;   /* SW-in-frame: tracked above */
    gfx_cursor_overlay_hide();
    gfx_cursor_overlay_show(x, y);
}

/* ---- push to display ----------------------------------------- */

/* Default backend: memcpy the back buffer into the Limine framebuffer.
 * Used everywhere unless a higher-fidelity backend (virtio-gpu, etc.)
 * has called gfx_set_backend(). */
static void limine_flip(void) {
    uint32_t *fb = gfx_hw_fb_ptr();
    if (!fb || !gfx_hw_fb_writable()) return;
    /* When the hardware pitch matches our width (the common QEMU case)
     * one big memcpy beats H separate ones. Otherwise walk row by row. */
    if (g.fb_pitch_px == g.width) {
        memcpy(fb, g.back, (size_t)g.width * g.height * 4u);
    } else {
        for (uint32_t row = 0; row < g.height; row++) {
            memcpy(&fb[row * g.fb_pitch_px],
                   &g.back[row * g.width],
                   (size_t)g.width * 4u);
        }
    }
    /* The framebuffer is mapped Write-Combining (PAT slot 4) on real
     * hardware -- flush the WC store buffers so the pixels actually reach
     * the scanout instead of lingering in the CPU's write-combine buffers.
     * No-op-cost on QEMU; required for a visible display on a real IGD. */
    __asm__ volatile ("sfence" ::: "memory");
}

/* M27B: Limine partial present. Same row-walk as limine_flip() but
 * scoped to a single rect. Used by the dirty-rectangle path; falls
 * back gracefully if anything looks wrong (caller invokes flip()
 * after present_rect either way is fine -- worst case we double-push
 * a few pixels, never crash). */
static void limine_present_rect(int x, int y, int w, int h) {
    uint32_t *fb = gfx_hw_fb_ptr();
    if (!fb || !gfx_hw_fb_writable() || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0) return;
    if (x + w > (int)g.width)  w = (int)g.width  - x;
    if (y + h > (int)g.height) h = (int)g.height - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        memcpy(&fb[(uint32_t)(y + row) * g.fb_pitch_px + (uint32_t)x],
               &g.back[(uint32_t)(y + row) * g.width      + (uint32_t)x],
               (size_t)w * 4u);
    }
    /* Flush WC store buffers (see limine_flip) -- small dirty-rect copies
     * are especially prone to lingering in the write-combine buffers. */
    __asm__ volatile ("sfence" ::: "memory");
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
    gfx_cursor_overlay_hide();

    const struct gfx_backend *b = g.backend ? g.backend : &g_backend_limine;
    int dx, dy, dw, dh;
    bool have_dirty = gfx_dirty_get(&dx, &dy, &dw, &dh);
    bool covers_all = !have_dirty
                   || g.dirty_full
                   || (dx == 0 && dy == 0
                       && dw == (int)g.width && dh == (int)g.height);
    g.stats.total_flips++;

    if (!have_dirty) {
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
    g.frame_count++;

    /* Double-buffer swap: after presenting, switch the active drawing
     * buffer so the compositor draws into the other one while scanout
     * is still referencing the just-presented content. Copy the current
     * frame into the new buffer so incremental drawing works correctly. */
    if (g.back2) {
        uint32_t *presented = g.back;
        uint32_t *next = g.back2;
        if (g.active_buf == 0) {
            next = g.back2;
            g.active_buf = 1;
        } else {
            next = g.back;
            g.active_buf = 0;
        }
        /* Keep the two buffers in sync so incremental drawing is correct.
         * `next` held the previous presented frame, and (frame N) = (frame
         * N-1) + (damage), so on a PARTIAL present we only need to copy the
         * damage rect. A full present (covers_all) re-syncs everything and
         * is the safety net if damage tracking ever misses a region -- so a
         * bug degrades to a stale region for one frame, never corruption. */
        if (!covers_all && have_dirty && dw > 0 && dh > 0) {
            for (int row = dy; row < dy + dh; row++) {
                memcpy(&next[(size_t)row * g.width + dx],
                       &presented[(size_t)row * g.width + dx],
                       (size_t)dw * 4u);
            }
        } else {
            memcpy(next, presented, (size_t)g.width * g.height * 4u);
        }
        g.back = next;
    }
}

/* ---- Stage 3: Intel GT blitter present backend --------------------
 *
 * On the real EliteDesk (gen7 Haswell) the i915-lite GT bring-up + blit
 * self-test may arm a GPU present path: each flip/present_rect blits the
 * dirty region from THIS back buffer straight into the live scanout via
 * XY_SRC_COPY_BLT. If the blit fails for any reason (unmapped page, ring
 * hang, out-of-range param) intel_gt_present_rect() returns negative and we
 * fall through to the universal Limine CPU memcpy -- so the display is never
 * lost. Implemented as a gfx_backend so it plugs in via gfx_set_backend()
 * exactly like virtio-gpu, and keeps the memcpy fallback in-file. */
int intel_gt_present_rect(const uint32_t *back, uint32_t back_w,
                          uint32_t back_h, int x, int y, int w, int h);

static void intel_flip(void) {
    if (intel_gt_present_rect(g.back, g.width, g.height,
                              0, 0, (int)g.width, (int)g.height) == 0)
        return;
    limine_flip();
}

static void intel_present_rect(int x, int y, int w, int h) {
    if (intel_gt_present_rect(g.back, g.width, g.height, x, y, w, h) == 0)
        return;
    limine_present_rect(x, y, w, h);
}

static int intel_describe(char *extra, int cap) {
    if (!extra || cap <= 0) return 0;
    return ksnprintf(extra, (size_t)cap,
                     "intel-gt-blit (fallback=limine-fb) fb_pitch_px=%u",
                     g.fb_pitch_px);
}

static const struct gfx_backend g_backend_intel = {
    .flip            = intel_flip,
    .present_rect    = intel_present_rect,
    .describe        = intel_describe,
    .name            = "intel-gt",
    .bytes_per_pixel = 4,
};

void gfx_use_intel_backend(void) {
    gfx_set_backend(&g_backend_intel);
}

/* ---- Stage 4: Intel GT tear-free page-flip backend ----------------
 *
 * Presents by blitting the whole back buffer into an off-screen scanout
 * buffer and retargeting the display plane at vblank (see
 * src/gpu_intel_modeset.c). Because the scanout is no longer the Limine FB,
 * the cursor is composited into the flipped frame from the tracked overlay
 * position. Any per-flip failure restores the Limine FB and returns negative,
 * so we fall through to the universal CPU present. The page-flip always
 * presents the entire frame, so present_rect is handled as a full flip. */
int intel_gt_flip_present(const uint32_t *back, uint32_t back_w,
                          uint32_t back_h, int cur_x, int cur_y,
                          int cur_visible, int dx, int dy, int dw, int dh);

static void intel_flip_flip(void) {
    if (intel_gt_flip_present(g.back, g.width, g.height,
                              g_cursor_overlay.x, g_cursor_overlay.y, 1,
                              0, 0, (int)g.width, (int)g.height) == 0)
        return;
    limine_flip();   /* watchdog restored the Limine FB -- CPU present to it */
}

static void intel_flip_present_rect(int x, int y, int w, int h) {
    if (intel_gt_flip_present(g.back, g.width, g.height,
                              g_cursor_overlay.x, g_cursor_overlay.y, 1,
                              x, y, w, h) == 0)
        return;
    /* On fallback repaint the WHOLE Limine FB: a partial push would leave the
     * (stale, unused-while-flipping) FB corrupt outside the dirty rect. */
    limine_flip();
}

static int intel_flip_describe(char *extra, int cap) {
    if (!extra || cap <= 0) return 0;
    return ksnprintf(extra, (size_t)cap,
                     "intel-gt-flip (fallback=limine-fb) fb_pitch_px=%u",
                     g.fb_pitch_px);
}

static const struct gfx_backend g_backend_intel_flip = {
    .flip            = intel_flip_flip,
    .present_rect    = intel_flip_present_rect,
    .describe        = intel_flip_describe,
    .name            = "intel-gt-flip",
    .bytes_per_pixel = 4,
};

void gfx_use_intel_flip_backend(void) {
    gfx_set_backend(&g_backend_intel_flip);
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

uint64_t gfx_frame_count(void) {
    return g.frame_count;
}

/* ==== Surface-targeted primitives (Phase 1: Advanced GPU/GUI Stack) ==== */

static inline void surface_put(struct gfx_surface *s, int x, int y, uint32_t c) {
    s->pixels[y * s->width + x] = c;
}

static inline uint32_t surface_get(const struct gfx_surface *s, int x, int y) {
    return s->pixels[y * s->width + x];
}

static bool surface_clip(const struct gfx_surface *s, int *x, int *y, int *w, int *h) {
    if (*w <= 0 || *h <= 0) return false;
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->width) x1 = s->width;
    if (y1 > s->height) y1 = s->height;
    if (x1 <= x0 || y1 <= y0) return false;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}

/* ---- Box blur (Phase 2) ----------------------------------------- */

static void blur_pass_surface(uint32_t *pixels, int stride, int x, int y, int w, int h) {
    if (w < 3 || h < 3) return;
    /* Horizontal pass */
    for (int row = y; row < y + h; row++) {
        uint32_t *r = &pixels[row * stride];
        uint32_t prev = r[x];
        for (int col = x + 1; col < x + w - 1; col++) {
            uint32_t l = prev;
            uint32_t c = r[col];
            uint32_t n = r[col + 1];
            prev = c;
            uint32_t rr = (((l >> 16) & 0xFF) + ((c >> 16) & 0xFF) + ((n >> 16) & 0xFF)) / 3;
            uint32_t gg = (((l >>  8) & 0xFF) + ((c >>  8) & 0xFF) + ((n >>  8) & 0xFF)) / 3;
            uint32_t bb = ((l & 0xFF) + (c & 0xFF) + (n & 0xFF)) / 3;
            r[col] = (rr << 16) | (gg << 8) | bb;
        }
    }
    /* Vertical pass */
    for (int col = x; col < x + w; col++) {
        uint32_t prev = pixels[y * stride + col];
        for (int row = y + 1; row < y + h - 1; row++) {
            uint32_t t = prev;
            uint32_t c = pixels[row * stride + col];
            uint32_t b = pixels[(row + 1) * stride + col];
            prev = c;
            uint32_t rr = (((t >> 16) & 0xFF) + ((c >> 16) & 0xFF) + ((b >> 16) & 0xFF)) / 3;
            uint32_t gg = (((t >>  8) & 0xFF) + ((c >>  8) & 0xFF) + ((b >>  8) & 0xFF)) / 3;
            uint32_t bb = ((t & 0xFF) + (c & 0xFF) + (b & 0xFF)) / 3;
            pixels[row * stride + col] = (rr << 16) | (gg << 8) | bb;
        }
    }
}

void gfx_box_blur_region(int x, int y, int w, int h, int passes) {
    if (!g.ready) return;
    if (!clip_rect(&x, &y, &w, &h)) return;
    if (passes < 1) passes = 1;
    if (passes > 3) passes = 3;
    for (int p = 0; p < passes; p++) {
        blur_pass_surface(g.back, (int)g.width, x, y, w, h);
    }
    dirty_union(x, y, w, h);
}

void gfx_surface_box_blur(struct gfx_surface *s, int x, int y, int w, int h, int passes) {
    if (!s || !s->pixels) return;
    if (!surface_clip(s, &x, &y, &w, &h)) return;
    if (passes < 1) passes = 1;
    if (passes > 3) passes = 3;
    for (int p = 0; p < passes; p++) {
        blur_pass_surface(s->pixels, s->width, x, y, w, h);
    }
}

void gfx_surface_from_backbuf(struct gfx_surface *out) {
    if (!out) return;
    out->pixels = g.ready ? g.back : 0;
    out->width  = (int)g.width;
    out->height = (int)g.height;
}

void gfx_surface_fill_rect(struct gfx_surface *s, int x, int y, int w, int h, uint32_t color) {
    if (!s || !s->pixels) return;
    if (!surface_clip(s, &x, &y, &w, &h)) return;
    for (int dy = 0; dy < h; dy++) {
        uint32_t *row = &s->pixels[(y + dy) * s->width + x];
        for (int dx = 0; dx < w; dx++) row[dx] = color;
    }
}

void gfx_surface_draw_rect(struct gfx_surface *s, int x, int y, int w, int h, uint32_t color) {
    if (!s || !s->pixels) return;
    if (w <= 0 || h <= 0) return;
    /* Top edge */
    for (int i = 0; i < w; i++) {
        int px = x + i, py = y;
        if (px >= 0 && px < s->width && py >= 0 && py < s->height)
            surface_put(s, px, py, color);
    }
    /* Bottom edge */
    for (int i = 0; i < w; i++) {
        int px = x + i, py = y + h - 1;
        if (px >= 0 && px < s->width && py >= 0 && py < s->height)
            surface_put(s, px, py, color);
    }
    /* Left edge */
    for (int i = 1; i < h - 1; i++) {
        int px = x, py = y + i;
        if (px >= 0 && px < s->width && py >= 0 && py < s->height)
            surface_put(s, px, py, color);
    }
    /* Right edge */
    for (int i = 1; i < h - 1; i++) {
        int px = x + w - 1, py = y + i;
        if (px >= 0 && px < s->width && py >= 0 && py < s->height)
            surface_put(s, px, py, color);
    }
}

void gfx_surface_draw_line(struct gfx_surface *s, int x0, int y0, int x1, int y1, uint32_t color) {
    if (!s || !s->pixels) return;
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1;
    int sy = dy > 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        if (x0 >= 0 && x0 < s->width && y0 >= 0 && y0 < s->height)
            surface_put(s, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void gfx_surface_fill_rounded_rect(struct gfx_surface *s, int x, int y, int w, int h, int radius, uint32_t color) {
    if (!s || !s->pixels) return;
    if (w <= 0 || h <= 0) return;
    int max_r = (w < h ? w : h) / 2;
    if (radius > max_r) radius = max_r;
    if (radius < 0) radius = 0;

    for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if (py < 0 || py >= s->height) continue;
        int x_start = x, x_end = x + w;
        /* Top-left / top-right corners */
        if (dy < radius) {
            int ry = radius - dy - 1;
            int r2 = radius * radius;
            int rx;
            for (rx = radius; rx >= 0; rx--) {
                if (rx * rx + ry * ry <= r2) break;
            }
            int inset = radius - rx - 1;
            x_start += inset;
            x_end   -= inset;
        }
        /* Bottom-left / bottom-right corners */
        else if (dy >= h - radius) {
            int ry = dy - (h - radius);
            int r2 = radius * radius;
            int rx;
            for (rx = radius; rx >= 0; rx--) {
                if (rx * rx + ry * ry <= r2) break;
            }
            int inset = radius - rx - 1;
            x_start += inset;
            x_end   -= inset;
        }
        if (x_start < 0) x_start = 0;
        if (x_end > s->width) x_end = s->width;
        for (int px = x_start; px < x_end; px++) {
            if (px >= 0) surface_put(s, px, py, color);
        }
    }
}

void gfx_surface_fill_rounded_rect_blend(struct gfx_surface *s, int x, int y, int w, int h, int radius, uint32_t argb) {
    if (!s || !s->pixels) return;
    uint32_t a = (argb >> 24) & 0xFF;
    if (a == 0) return;
    if (a == 255) { gfx_surface_fill_rounded_rect(s, x, y, w, h, radius, argb & 0x00FFFFFFu); return; }
    if (w <= 0 || h <= 0) return;
    int max_r = (w < h ? w : h) / 2;
    if (radius > max_r) radius = max_r;
    if (radius < 0) radius = 0;

    for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if (py < 0 || py >= s->height) continue;
        int x_start = x, x_end = x + w;
        if (dy < radius) {
            int ry = radius - dy - 1;
            int r2 = radius * radius;
            int rx;
            for (rx = radius; rx >= 0; rx--) {
                if (rx * rx + ry * ry <= r2) break;
            }
            int inset = radius - rx - 1;
            x_start += inset;
            x_end   -= inset;
        } else if (dy >= h - radius) {
            int ry = dy - (h - radius);
            int r2 = radius * radius;
            int rx;
            for (rx = radius; rx >= 0; rx--) {
                if (rx * rx + ry * ry <= r2) break;
            }
            int inset = radius - rx - 1;
            x_start += inset;
            x_end   -= inset;
        }
        if (x_start < 0) x_start = 0;
        if (x_end > s->width) x_end = s->width;
        for (int px = x_start; px < x_end; px++) {
            if (px >= 0) {
                uint32_t dst = surface_get(s, px, py);
                surface_put(s, px, py, blend_src_over(dst, argb));
            }
        }
    }
}

void gfx_surface_fill_rect_blend(struct gfx_surface *s, int x, int y, int w, int h, uint32_t argb) {
    if (!s || !s->pixels) return;
    uint32_t a = (argb >> 24) & 0xFF;
    if (a == 0) return;
    if (a == 255) { gfx_surface_fill_rect(s, x, y, w, h, argb & 0x00FFFFFFu); return; }
    if (!surface_clip(s, &x, &y, &w, &h)) return;
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx, py = y + dy;
            uint32_t dst = surface_get(s, px, py);
            surface_put(s, px, py, blend_src_over(dst, argb));
        }
    }
}

void gfx_surface_draw_text(struct gfx_surface *s, int x, int y, const char *str, uint32_t fg, uint32_t bg) {
    if (!s || !s->pixels || !str) return;
    bool transparent_bg = (bg == GFX_TRANSPARENT);
    int ox = x;
    for (; *str; str++) {
        unsigned char c = (unsigned char)*str;
        if (c >= 128) c = '?';
        const uint8_t *glyph = font8x8_basic[c];
        for (int row = 0; row < 8; row++) {
            int py = y + row;
            if (py < 0 || py >= s->height) continue;
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                int px = ox + col;
                if (px < 0 || px >= s->width) continue;
                if ((bits >> col) & 1)
                    surface_put(s, px, py, fg);
                else if (!transparent_bg)
                    surface_put(s, px, py, bg);
            }
        }
        ox += 8;
    }
}

void gfx_surface_draw_text_scaled(struct gfx_surface *s, int x, int y, const char *str, uint32_t fg, uint32_t bg, int scale) {
    if (!s || !s->pixels || !str) return;
    if (scale <= 0) scale = 1;
    bool transparent_bg = (bg == GFX_TRANSPARENT);
    int ox = x;
    for (; *str; str++) {
        unsigned char c = (unsigned char)*str;
        if (c >= 128) c = '?';
        const uint8_t *glyph = font8x8_basic[c];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                uint32_t clr = ((bits >> col) & 1) ? fg : (transparent_bg ? 0 : bg);
                bool draw = ((bits >> col) & 1) || !transparent_bg;
                if (!draw) continue;
                for (int sy = 0; sy < scale; sy++) {
                    int py = y + row * scale + sy;
                    if (py < 0 || py >= s->height) continue;
                    for (int sx = 0; sx < scale; sx++) {
                        int px = ox + col * scale + sx;
                        if (px < 0 || px >= s->width) continue;
                        surface_put(s, px, py, clr);
                    }
                }
            }
        }
        ox += 8 * scale;
    }
}

void gfx_surface_blit(struct gfx_surface *s, int dst_x, int dst_y, int w, int h, const uint32_t *src, int src_pitch) {
    if (!s || !s->pixels || !src) return;
    int sx = 0, sy = 0;
    if (dst_x < 0) { sx = -dst_x; w += dst_x; dst_x = 0; }
    if (dst_y < 0) { sy = -dst_y; h += dst_y; dst_y = 0; }
    if (dst_x + w > s->width) w = s->width - dst_x;
    if (dst_y + h > s->height) h = s->height - dst_y;
    if (w <= 0 || h <= 0) return;
    for (int dy = 0; dy < h; dy++) {
        const uint32_t *src_row = &src[(sy + dy) * src_pitch + sx];
        uint32_t *dst_row = &s->pixels[(dst_y + dy) * s->width + dst_x];
        memcpy(dst_row, src_row, (size_t)w * 4u);
    }
}

void gfx_surface_blit_blend(struct gfx_surface *s, int dst_x, int dst_y, int w, int h, const uint32_t *src, int src_pitch) {
    if (!s || !s->pixels || !src) return;
    int sx = 0, sy = 0;
    if (dst_x < 0) { sx = -dst_x; w += dst_x; dst_x = 0; }
    if (dst_y < 0) { sy = -dst_y; h += dst_y; dst_y = 0; }
    if (dst_x + w > s->width) w = s->width - dst_x;
    if (dst_y + h > s->height) h = s->height - dst_y;
    if (w <= 0 || h <= 0) return;
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            uint32_t sp = src[(sy + dy) * src_pitch + sx + dx];
            if ((sp >> 24) == 0) continue;
            int px = dst_x + dx, py = dst_y + dy;
            uint32_t dst = surface_get(s, px, py);
            surface_put(s, px, py, blend_src_over(dst, sp));
        }
    }
}

void gfx_surface_fill_circle(struct gfx_surface *s, int cx, int cy, int r, uint32_t color) {
    if (!s || !s->pixels || r <= 0) return;
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= s->height) continue;
        int r2 = r * r, dy2 = dy * dy;
        int half_w = 0;
        for (half_w = r; half_w >= 0; half_w--) {
            if (half_w * half_w + dy2 <= r2) break;
        }
        int x0 = cx - half_w, x1 = cx + half_w;
        if (x0 < 0) x0 = 0;
        if (x1 >= s->width) x1 = s->width - 1;
        for (int px = x0; px <= x1; px++) {
            surface_put(s, px, py, color);
        }
    }
}

void gfx_surface_draw_circle(struct gfx_surface *s, int cx, int cy, int r, uint32_t color) {
    if (!s || !s->pixels || r <= 0) return;
    int x = 0, y = r, d = 1 - r;
    while (x <= y) {
        int pts[][2] = {
            {cx + x, cy + y}, {cx - x, cy + y},
            {cx + x, cy - y}, {cx - x, cy - y},
            {cx + y, cy + x}, {cx - y, cy + x},
            {cx + y, cy - x}, {cx - y, cy - x},
        };
        for (int i = 0; i < 8; i++) {
            int px = pts[i][0], py = pts[i][1];
            if (px >= 0 && px < s->width && py >= 0 && py < s->height)
                surface_put(s, px, py, color);
        }
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

void gfx_surface_gradient_v(struct gfx_surface *s, int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    if (!s || !s->pixels) return;
    if (!surface_clip(s, &x, &y, &w, &h)) return;
    if (h == 1) {
        uint32_t *row = &s->pixels[y * s->width + x];
        for (int dx = 0; dx < w; dx++) row[dx] = top;
        return;
    }
    for (int dy = 0; dy < h; dy++) {
        uint32_t r = ((top >> 16) & 0xFF) * (h - 1 - dy) / (h - 1) + ((bot >> 16) & 0xFF) * dy / (h - 1);
        uint32_t gr = ((top >> 8) & 0xFF) * (h - 1 - dy) / (h - 1) + ((bot >> 8) & 0xFF) * dy / (h - 1);
        uint32_t b = (top & 0xFF) * (h - 1 - dy) / (h - 1) + (bot & 0xFF) * dy / (h - 1);
        uint32_t c = (r << 16) | (gr << 8) | b;
        uint32_t *row = &s->pixels[(y + dy) * s->width + x];
        for (int dx = 0; dx < w; dx++) row[dx] = c;
    }
}

void gfx_surface_draw_line_blend(struct gfx_surface *s, int x0, int y0, int x1, int y1, uint32_t argb) {
    if (!s || !s->pixels) return;
    uint32_t a = (argb >> 24) & 0xFF;
    if (a == 0) return;
    if (a == 255) { gfx_surface_draw_line(s, x0, y0, x1, y1, argb & 0x00FFFFFFu); return; }
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1;
    int sy = dy > 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        if (x0 >= 0 && x0 < s->width && y0 >= 0 && y0 < s->height) {
            uint32_t dst = surface_get(s, x0, y0);
            surface_put(s, x0, y0, blend_src_over(dst, argb));
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

int gfx_surface_get_pixels(const struct gfx_surface *s, int x, int y, int w, int h, uint32_t *dst) {
    if (!s || !s->pixels || !dst) return 0;
    int cx = x, cy = y, cw = w, ch = h;
    if (cw <= 0 || ch <= 0) return 0;
    int x0 = cx, y0 = cy, x1 = cx + cw, y1 = cy + ch;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->width) x1 = s->width;
    if (y1 > s->height) y1 = s->height;
    if (x1 <= x0 || y1 <= y0) return 0;
    int copied = 0;
    for (int row = y0; row < y1; row++) {
        const uint32_t *src_row = &s->pixels[row * s->width + x0];
        int count = x1 - x0;
        memcpy(dst + copied, src_row, (size_t)count * 4u);
        copied += count;
    }
    return copied;
}
