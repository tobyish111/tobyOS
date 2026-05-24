/* gpu.h -- Display/Rendering Manager + hardware cursor for tobyOS.
 *
 * Milestone 2: GPU-DRM abstraction layer.
 *
 * The DRM layer sits above gfx.c and provides:
 *   - Multi-display enumeration and mode-setting
 *   - Off-screen render surfaces with dirty-rect tracking
 *   - Page-flip / vsync presentation
 *   - Hardware cursor support (software fallback)
 *
 * The layer is intentionally thin: it wraps the existing gfx back
 * buffer into a formal surface abstraction and exposes display
 * geometry that the desktop shell can query at runtime.
 */

#ifndef TOBYOS_GPU_H
#define TOBYOS_GPU_H

#include <tobyos/types.h>

/* ---- Display / output -------------------------------------------- */

#define DRM_MAX_DISPLAYS  4

struct drm_display {
    int      id;
    int      width;
    int      height;
    int      bpp;
    int      pitch;
    void    *framebuffer;
    int      connected;
    int      refresh_hz;
};

/* ---- Off-screen render surface ----------------------------------- */

struct drm_surface {
    uint32_t *pixels;
    int       width;
    int       height;
    int       dirty;
    int       dirty_x;
    int       dirty_y;
    int       dirty_w;
    int       dirty_h;
};

/* ---- DRM lifecycle ----------------------------------------------- */

void drm_init(void);

/* Display queries. */
int                     drm_get_display_count(void);
const struct drm_display *drm_get_display(int id);

/* Surface management. */
struct drm_surface *drm_create_surface(int w, int h);
void                drm_destroy_surface(struct drm_surface *s);
void                drm_surface_mark_dirty(struct drm_surface *s,
                                           int x, int y, int w, int h);

/* Presentation. */
void drm_blit_surface(struct drm_surface *surface,
                      int display_id, int x, int y);
void drm_present(int display_id);
int  drm_set_mode(int display_id, int w, int h, int hz);

/* ---- Hardware cursor --------------------------------------------- */

void cursor_set_image(const uint32_t *argb, int w, int h);
void cursor_move(int x, int y);
void cursor_show(void);
void cursor_hide(void);

#endif /* TOBYOS_GPU_H */
