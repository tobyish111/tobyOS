/* compositor_accel.c -- Hardware-accelerated compositor enhancements.
 *
 * Provides double buffering, dirty rectangle tracking, hardware cursor
 * support, and FPS counting. When Intel GPU hardware is available, uses
 * page flipping for tear-free display; otherwise falls back to memcpy.
 */

#include <tobyos/compositor_accel.h>
#include <tobyos/gpu_intel.h>
#include <tobyos/gfx.h>
#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/pit.h>

#define printk kprintf

/* ======================================================================
 * State
 * ====================================================================== */

static struct {
    int initialized;
    int double_buffer_enabled;
    int hw_accel;

    uint32_t *front_buf;
    uint32_t *back_buf;
    uint64_t front_phys;
    uint64_t back_phys;
    int width, height, stride;

    struct compositor_dirty_rect dirty[COMPOSITOR_MAX_DIRTY];
    int dirty_count;
    int full_repaint;

    /* FPS tracking */
    uint64_t frame_count;
    uint64_t last_fps_tick;
    int current_fps;
    int frames_this_second;

    /* Hardware cursor state */
    int cursor_visible;
    int cursor_x, cursor_y;
} g_comp;

/* ======================================================================
 * Initialization
 * ====================================================================== */

void compositor_accel_init(void) {
    memset(&g_comp, 0, sizeof(g_comp));

    if (intel_gpu_detected()) {
        g_comp.hw_accel = 1;
        g_comp.width = intel_gpu_get_width();
        g_comp.height = intel_gpu_get_height();
        g_comp.stride = g_comp.width * 4;
        g_comp.front_buf = intel_gpu_get_framebuffer();

        printk("[compositor] hardware acceleration enabled (%dx%d)\n",
               g_comp.width, g_comp.height);
    } else {
        g_comp.hw_accel = 0;
        if (gfx_ready()) {
            g_comp.width = (int)gfx_width();
            g_comp.height = (int)gfx_height();
            g_comp.stride = g_comp.width * 4;
            g_comp.front_buf = gfx_backbuf();
        }
        printk("[compositor] software mode (%dx%d)\n",
               g_comp.width, g_comp.height);
    }

    g_comp.back_buf = g_comp.front_buf;
    g_comp.last_fps_tick = pit_ticks();
    g_comp.initialized = 1;
}

/* ======================================================================
 * Double buffering
 * ====================================================================== */

void compositor_set_double_buffer(int enabled) {
    if (!g_comp.initialized) return;

    if (enabled && !g_comp.double_buffer_enabled) {
        if (g_comp.hw_accel) {
            /* Use the GPU's pre-allocated back buffer */
            g_comp.double_buffer_enabled = 1;
            printk("[compositor] double buffering enabled (page-flip)\n");
        } else {
            /* Software double buffer: use gfx backbuf as front,
             * allocate a second buffer area statically. */
            static uint32_t s_sw_back[3840 * 2160];
            int pixels = g_comp.width * g_comp.height;
            if (pixels <= 3840 * 2160) {
                g_comp.back_buf = s_sw_back;
                g_comp.double_buffer_enabled = 1;
                memset(g_comp.back_buf, 0, (size_t)pixels * 4);
                printk("[compositor] double buffering enabled (memcpy)\n");
            }
        }
    } else if (!enabled && g_comp.double_buffer_enabled) {
        g_comp.double_buffer_enabled = 0;
        g_comp.back_buf = g_comp.front_buf;
        printk("[compositor] double buffering disabled\n");
    }
}

void compositor_flip(void) {
    if (!g_comp.initialized) return;

    if (g_comp.hw_accel && g_comp.double_buffer_enabled) {
        /* Page flip: tell GPU to display the back buffer */
        intel_gpu_wait_vblank();
        intel_gpu_page_flip(g_comp.back_phys);

        /* Swap pointers */
        uint32_t *tmp = g_comp.front_buf;
        g_comp.front_buf = g_comp.back_buf;
        g_comp.back_buf = tmp;

        uint64_t tmp_phys = g_comp.front_phys;
        g_comp.front_phys = g_comp.back_phys;
        g_comp.back_phys = tmp_phys;
    } else if (g_comp.double_buffer_enabled) {
        /* Software flip: copy back to front */
        if (g_comp.dirty_count > 0 && !g_comp.full_repaint) {
            /* Only copy dirty regions */
            for (int i = 0; i < g_comp.dirty_count; i++) {
                struct compositor_dirty_rect *r = &g_comp.dirty[i];
                int x0 = r->x < 0 ? 0 : r->x;
                int y0 = r->y < 0 ? 0 : r->y;
                int x1 = r->x + r->w;
                int y1 = r->y + r->h;
                if (x1 > g_comp.width) x1 = g_comp.width;
                if (y1 > g_comp.height) y1 = g_comp.height;

                for (int y = y0; y < y1; y++) {
                    int offset = y * g_comp.width + x0;
                    int count = x1 - x0;
                    memcpy(&g_comp.front_buf[offset],
                           &g_comp.back_buf[offset],
                           (size_t)count * 4);
                }
            }
        } else {
            memcpy(g_comp.front_buf, g_comp.back_buf,
                   (size_t)g_comp.width * g_comp.height * 4);
        }
    }

    /* Update FPS counter */
    g_comp.frame_count++;
    g_comp.frames_this_second++;

    uint64_t now = pit_ticks();
    uint64_t elapsed = now - g_comp.last_fps_tick;
    if (elapsed >= 1000) {
        g_comp.current_fps = (int)((uint64_t)g_comp.frames_this_second * 1000 / elapsed);
        g_comp.frames_this_second = 0;
        g_comp.last_fps_tick = now;
    }

    /* Reset dirty tracking for next frame */
    g_comp.dirty_count = 0;
    g_comp.full_repaint = 0;
}

/* ======================================================================
 * Dirty rectangle tracking
 * ====================================================================== */

void compositor_mark_dirty(int x, int y, int w, int h) {
    if (!g_comp.initialized) return;

    if (g_comp.dirty_count >= COMPOSITOR_MAX_DIRTY) {
        g_comp.full_repaint = 1;
        return;
    }

    /* Clamp to screen bounds */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_comp.width) w = g_comp.width - x;
    if (y + h > g_comp.height) h = g_comp.height - y;
    if (w <= 0 || h <= 0) return;

    /* Try to merge with existing rects */
    for (int i = 0; i < g_comp.dirty_count; i++) {
        struct compositor_dirty_rect *r = &g_comp.dirty[i];
        /* Check if new rect overlaps or is adjacent */
        if (x <= r->x + r->w + 16 && x + w >= r->x - 16 &&
            y <= r->y + r->h + 16 && y + h >= r->y - 16) {
            /* Merge: expand existing rect */
            int nx = x < r->x ? x : r->x;
            int ny = y < r->y ? y : r->y;
            int nx2 = (x + w) > (r->x + r->w) ? (x + w) : (r->x + r->w);
            int ny2 = (y + h) > (r->y + r->h) ? (y + h) : (r->y + r->h);
            r->x = nx;
            r->y = ny;
            r->w = nx2 - nx;
            r->h = ny2 - ny;
            return;
        }
    }

    struct compositor_dirty_rect *r = &g_comp.dirty[g_comp.dirty_count++];
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
}

void compositor_damage_flush(void) {
    if (!g_comp.initialized) return;

    if (g_comp.dirty_count == 0 && !g_comp.full_repaint) return;
    compositor_flip();
}

/* ======================================================================
 * FPS counter
 * ====================================================================== */

int compositor_get_fps(void) {
    return g_comp.current_fps;
}

/* ======================================================================
 * Hardware cursor
 * ====================================================================== */

void compositor_set_hw_cursor(const uint32_t *image, int w, int h) {
    if (!g_comp.initialized) return;

    if (g_comp.hw_accel) {
        intel_gpu_set_cursor(image, w, h);
        g_comp.cursor_visible = 1;
    }
}

void compositor_move_hw_cursor(int x, int y) {
    if (!g_comp.initialized) return;

    g_comp.cursor_x = x;
    g_comp.cursor_y = y;

    if (g_comp.hw_accel && g_comp.cursor_visible) {
        intel_gpu_move_cursor(x, y);
    }
}

void compositor_hide_hw_cursor(void) {
    if (!g_comp.initialized) return;

    if (g_comp.hw_accel) {
        intel_gpu_hide_cursor();
    }
    g_comp.cursor_visible = 0;
}

/* ======================================================================
 * Query state
 * ====================================================================== */

int compositor_accel_active(void) {
    return g_comp.hw_accel;
}

int compositor_double_buffer_active(void) {
    return g_comp.double_buffer_enabled;
}
