/* gpu_drm.c -- Display/Rendering Manager abstraction (Milestone 2).
 *
 * Detects displays from the Limine framebuffer (GOP/VESA mode info
 * provided at boot), manages off-screen render surfaces with dirty-rect
 * tracking, and provides page-flip presentation.
 */

#include <tobyos/gpu.h>
#include <tobyos/gfx.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

static struct drm_display g_displays[DRM_MAX_DISPLAYS];
static int                g_display_count;
static int                g_inited;

/* ---- helpers ----------------------------------------------------- */

static inline int clamp_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ---- DRM init ---------------------------------------------------- */

void drm_init(void)
{
    if (g_inited)
        return;
    g_inited = 1;
    g_display_count = 0;

    if (!gfx_ready()) {
        kprintf("[gpu_drm] no framebuffer available\n");
        return;
    }

    struct drm_display *d = &g_displays[0];
    d->id          = 0;
    d->width       = (int)gfx_width();
    d->height      = (int)gfx_height();
    d->bpp         = 32;
    d->pitch       = d->width * 4;
    d->framebuffer = gfx_backbuf();
    d->connected   = 1;
    d->refresh_hz  = 60;
    g_display_count = 1;

    kprintf("[gpu_drm] display 0: %dx%d %d bpp @ %d Hz\n",
           d->width, d->height, d->bpp, d->refresh_hz);
}

/* ---- display queries --------------------------------------------- */

int drm_get_display_count(void)
{
    return g_display_count;
}

const struct drm_display *drm_get_display(int id)
{
    if (id < 0 || id >= g_display_count)
        return NULL;
    return &g_displays[id];
}

/* ---- surface management ------------------------------------------ */

struct drm_surface *drm_create_surface(int w, int h)
{
    if (w <= 0 || h <= 0)
        return NULL;

    struct drm_surface *s = kmalloc(sizeof(*s));
    if (!s)
        return NULL;

    s->pixels = kmalloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!s->pixels) {
        kfree(s);
        return NULL;
    }

    s->width   = w;
    s->height  = h;
    s->dirty   = 0;
    s->dirty_x = 0;
    s->dirty_y = 0;
    s->dirty_w = 0;
    s->dirty_h = 0;

    memset(s->pixels, 0, (size_t)w * (size_t)h * sizeof(uint32_t));
    return s;
}

void drm_destroy_surface(struct drm_surface *s)
{
    if (!s)
        return;
    if (s->pixels)
        kfree(s->pixels);
    kfree(s);
}

void drm_surface_mark_dirty(struct drm_surface *s,
                            int x, int y, int w, int h)
{
    if (!s || w <= 0 || h <= 0)
        return;

    x = clamp_i(x, 0, s->width);
    y = clamp_i(y, 0, s->height);
    if (x + w > s->width)  w = s->width  - x;
    if (y + h > s->height) h = s->height - y;
    if (w <= 0 || h <= 0)
        return;

    if (!s->dirty) {
        s->dirty_x = x;
        s->dirty_y = y;
        s->dirty_w = w;
        s->dirty_h = h;
        s->dirty   = 1;
        return;
    }

    /* union the new rect with the existing dirty rect */
    int x1 = s->dirty_x;
    int y1 = s->dirty_y;
    int x2 = x1 + s->dirty_w;
    int y2 = y1 + s->dirty_h;

    if (x < x1)     x1 = x;
    if (y < y1)     y1 = y;
    if (x + w > x2) x2 = x + w;
    if (y + h > y2) y2 = y + h;

    s->dirty_x = x1;
    s->dirty_y = y1;
    s->dirty_w = x2 - x1;
    s->dirty_h = y2 - y1;
}

/* ---- presentation ------------------------------------------------ */

void drm_blit_surface(struct drm_surface *surface,
                      int display_id, int dst_x, int dst_y)
{
    if (!surface || !surface->pixels)
        return;

    const struct drm_display *d = drm_get_display(display_id);
    if (!d || !d->connected || !d->framebuffer)
        return;

    /* Only blit the dirty region (or everything if nothing is tracked). */
    int sx = 0, sy = 0, sw = surface->width, sh = surface->height;
    if (surface->dirty) {
        sx = surface->dirty_x;
        sy = surface->dirty_y;
        sw = surface->dirty_w;
        sh = surface->dirty_h;
    }

    int dx = dst_x + sx;
    int dy = dst_y + sy;

    /* Clip to display bounds. */
    if (dx < 0) { sx -= dx; sw += dx; dx = 0; }
    if (dy < 0) { sy -= dy; sh += dy; dy = 0; }
    if (dx + sw > d->width)  sw = d->width  - dx;
    if (dy + sh > d->height) sh = d->height - dy;
    if (sw <= 0 || sh <= 0)
        return;

    /* Use the gfx blit path to push into the back buffer. */
    gfx_blit(dx, dy, sw, sh,
             surface->pixels + sy * surface->width + sx,
             surface->width);

    /* Clear dirty tracking after blit. */
    surface->dirty   = 0;
    surface->dirty_x = 0;
    surface->dirty_y = 0;
    surface->dirty_w = 0;
    surface->dirty_h = 0;
}

void drm_present(int display_id)
{
    if (display_id < 0 || display_id >= g_display_count)
        return;
    gfx_flip();
}

int drm_set_mode(int display_id, int w, int h, int hz)
{
    if (display_id < 0 || display_id >= g_display_count)
        return -1;

    /* Mode-setting is limited: we can only report what the bootloader
     * gave us.  Record the requested refresh rate but the resolution
     * is fixed at boot. */
    struct drm_display *d = &g_displays[display_id];
    if (w != d->width || h != d->height) {
        kprintf("[gpu_drm] mode-set %dx%d not supported (have %dx%d)\n",
               w, h, d->width, d->height);
        return -1;
    }

    if (hz > 0)
        d->refresh_hz = hz;
    return 0;
}
