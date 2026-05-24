/* toby/layout.h -- box layout engine for the tobyOS browser.
 *
 * Builds a layout tree from a styled DOM, then computes positions and
 * sizes for block, inline, and flex layout contexts.
 */

#ifndef TOBY_LAYOUT_H
#define TOBY_LAYOUT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dom_node;
struct css_stylesheet;
struct computed_style;

enum layout_type { LAYOUT_BLOCK, LAYOUT_INLINE, LAYOUT_FLEX, LAYOUT_NONE };

struct layout_box {
    enum layout_type type;
    struct dom_node *node;
    struct computed_style *style;

    float x, y;
    float width, height;
    float margin[4];
    float padding[4];
    float border[4];

    float content_width, content_height;

    struct layout_box *parent;
    struct layout_box *first_child;
    struct layout_box *next_sibling;

    char *text;
    int text_len;
};

struct layout_box *layout_build(struct dom_node *root,
                                struct css_stylesheet *ss,
                                float viewport_width,
                                float viewport_height);
void layout_free(struct layout_box *root);
void layout_compute(struct layout_box *root, float viewport_width);
struct layout_box *layout_hit_test(struct layout_box *root, float x, float y);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_LAYOUT_H */
