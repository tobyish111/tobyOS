/* compositor_accel.h -- Hardware-accelerated compositor support.
 *
 * Enhances the existing gfx compositor with:
 * - Double buffering with page-flip via Intel GPU (or fallback memcpy)
 * - Dirty rectangle tracking for partial repaints
 * - Hardware cursor via Intel GPU cursor plane
 * - FPS counter
 */

#ifndef TOBYOS_COMPOSITOR_ACCEL_H
#define TOBYOS_COMPOSITOR_ACCEL_H

#include <tobyos/types.h>

/* Maximum tracked dirty rectangles per frame */
#define COMPOSITOR_MAX_DIRTY 64

struct compositor_dirty_rect {
    int x, y, w, h;
};

void compositor_accel_init(void);
void compositor_set_double_buffer(int enabled);
void compositor_flip(void);
void compositor_mark_dirty(int x, int y, int w, int h);
void compositor_damage_flush(void);
int  compositor_get_fps(void);

/* Hardware cursor */
void compositor_set_hw_cursor(const uint32_t *image, int w, int h);
void compositor_move_hw_cursor(int x, int y);
void compositor_hide_hw_cursor(void);

/* Query state */
int  compositor_accel_active(void);
int  compositor_double_buffer_active(void);

#endif /* TOBYOS_COMPOSITOR_ACCEL_H */
