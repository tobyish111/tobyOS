/* toby/render.h -- Userland 2D rendering API (Milestone 2).
 *
 * Provides an off-screen surface abstraction for userland programs.
 * Surfaces are plain pixel buffers; render_present() pushes them to
 * the compositor via the GUI syscall interface.
 */

#ifndef TOBY_RENDER_H
#define TOBY_RENDER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct render_surface {
    uint32_t *pixels;
    int       width;
    int       height;
    int       clip_x;
    int       clip_y;
    int       clip_w;
    int       clip_h;
} render_surface_t;

render_surface_t *render_create_surface(int w, int h);
void              render_destroy_surface(render_surface_t *s);

void render_fill_rect(render_surface_t *s,
                      int x, int y, int w, int h, uint32_t color);
void render_blit(render_surface_t *dst, const render_surface_t *src,
                 int dst_x, int dst_y,
                 int src_x, int src_y, int w, int h);
void render_alpha_blend(render_surface_t *dst, const render_surface_t *src,
                        int dst_x, int dst_y, uint8_t alpha);
void render_present(render_surface_t *s, int window_fd);
void render_set_clip(render_surface_t *s, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_RENDER_H */
