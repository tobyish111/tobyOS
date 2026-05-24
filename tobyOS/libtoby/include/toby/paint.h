/* toby/paint.h -- Render tree painting for the browser.
 *
 * Walks the layout tree and produces draw commands for the compositor.
 */

#ifndef TOBY_PAINT_H
#define TOBY_PAINT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Draw command types */
enum paint_cmd_type {
    PAINT_FILL_RECT,
    PAINT_BORDER,
    PAINT_TEXT,
    PAINT_IMAGE,
    PAINT_ROUNDED_RECT,
    PAINT_GRADIENT,
    PAINT_CLIP_PUSH,
    PAINT_CLIP_POP
};

struct paint_cmd {
    enum paint_cmd_type type;
    float x, y, w, h;
    uint32_t color;
    uint32_t border_color;
    float border_width;
    float border_radius;
    const char *text;
    int text_len;
    float font_size;
    uint32_t *image_data;
    int image_w, image_h;
};

#define PAINT_MAX_CMDS 4096

struct paint_list {
    struct paint_cmd cmds[PAINT_MAX_CMDS];
    int count;
};

struct layout_box;

/* Paint a layout tree into a command list */
void paint_layout(struct layout_box *root, struct paint_list *out,
                  float scroll_x, float scroll_y,
                  float viewport_w, float viewport_h);

/* Execute paint commands onto a window framebuffer */
void paint_execute(struct paint_list *list, int window_fd);

/* ---- Scrolling state ---- */

struct scroll_state {
    float offset_x, offset_y;
    float content_width, content_height;
    float viewport_width, viewport_height;
    float max_scroll_x, max_scroll_y;
};

void scroll_init(struct scroll_state *s, float content_w, float content_h,
                 float viewport_w, float viewport_h);
void scroll_by(struct scroll_state *s, float dx, float dy);
void scroll_to(struct scroll_state *s, float x, float y);

/* ---- Image cache ---- */

#define IMG_CACHE_MAX 64

struct cached_image {
    char url[256];
    uint32_t *pixels;
    int width, height;
    int loaded;
};

struct image_cache {
    struct cached_image entries[IMG_CACHE_MAX];
    int count;
};

/* Load image from URL (async-ish: returns placeholder if not yet loaded) */
struct cached_image *image_cache_get(struct image_cache *cache, const char *url);
void image_cache_free(struct image_cache *cache);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_PAINT_H */
