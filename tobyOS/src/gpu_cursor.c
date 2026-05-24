/* gpu_cursor.c -- Hardware cursor support (Milestone 2).
 *
 * Manages the mouse cursor image and position.  Uses the existing
 * gfx_cursor_overlay_* software path (there is no hardware cursor
 * plane on the Limine/VESA/virtio-gpu path today).  If a future
 * driver exposes a hardware cursor plane, the wrappers here can be
 * switched without touching callers.
 */

#include <tobyos/gpu.h>
#include <tobyos/gfx.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

#define CURSOR_MAX_W  64
#define CURSOR_MAX_H  64

static uint32_t  g_cursor_img[CURSOR_MAX_W * CURSOR_MAX_H];
static int       g_cursor_w;
static int       g_cursor_h;
static int       g_cursor_x;
static int       g_cursor_y;
static int       g_cursor_visible;

void cursor_set_image(const uint32_t *argb, int w, int h)
{
    if (!argb || w <= 0 || h <= 0)
        return;
    if (w > CURSOR_MAX_W) w = CURSOR_MAX_W;
    if (h > CURSOR_MAX_H) h = CURSOR_MAX_H;

    memcpy(g_cursor_img, argb, (size_t)w * (size_t)h * sizeof(uint32_t));
    g_cursor_w = w;
    g_cursor_h = h;
}

void cursor_move(int x, int y)
{
    if (g_cursor_visible)
        gfx_cursor_overlay_move(x, y);

    g_cursor_x = x;
    g_cursor_y = y;
}

void cursor_show(void)
{
    if (g_cursor_visible)
        return;
    g_cursor_visible = 1;
    gfx_cursor_overlay_show(g_cursor_x, g_cursor_y);
}

void cursor_hide(void)
{
    if (!g_cursor_visible)
        return;
    g_cursor_visible = 0;
    gfx_cursor_overlay_hide();
}
