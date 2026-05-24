/* paint.c -- Render tree painting and image handling.
 *
 * Walks the layout tree, culls off-screen boxes, and produces
 * draw commands that the compositor executes onto the window surface.
 */

#include <toby/paint.h>
#include <toby/net.h>
#include <stdlib.h>
#include <string.h>

/* Forward declare layout_box from layout.h to avoid circular dep */
struct computed_style;
struct dom_node;

struct layout_box {
    int type;
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

/* css_value from css.h */
struct css_value {
    float number;
    int unit;
    uint32_t color;
    char string[64];
    int is_keyword;
};

/* Properties we care about for painting */
#define CSS_PROP_COLOR            13
#define CSS_PROP_BACKGROUND_COLOR 14
#define CSS_PROP_FONT_SIZE        16
#define CSS_PROP_BORDER_WIDTH     12
#define CSS_PROP_BORDER_RADIUS    30
#define CSS_PROP_OPACITY          29
#define CSS_PROP_COUNT            37

struct computed_style {
    struct css_value props[CSS_PROP_COUNT];
};

/* ---- Paint command emission ---- */

static void emit(struct paint_list *list, struct paint_cmd *cmd) {
    if (list->count >= PAINT_MAX_CMDS) return;
    list->cmds[list->count++] = *cmd;
}

static int is_visible(float bx, float by, float bw, float bh,
                      float sx, float sy, float vw, float vh) {
    float screen_x = bx - sx;
    float screen_y = by - sy;
    if (screen_x + bw < 0 || screen_x > vw) return 0;
    if (screen_y + bh < 0 || screen_y > vh) return 0;
    return 1;
}

static void paint_box(struct layout_box *box, struct paint_list *out,
                      float sx, float sy, float vw, float vh) {
    if (!box) return;

    float abs_x = box->x;
    float abs_y = box->y;
    float total_w = box->width;
    float total_h = box->height;

    /* Cull off-screen boxes */
    if (!is_visible(abs_x, abs_y, total_w, total_h, sx, sy, vw, vh))
        goto paint_children;

    /* Background */
    if (box->style) {
        uint32_t bg = box->style->props[CSS_PROP_BACKGROUND_COLOR].color;
        if (bg != 0 && (bg & 0xFF000000) != 0) {
            struct paint_cmd cmd = {0};
            cmd.type = PAINT_FILL_RECT;
            cmd.x = abs_x - sx;
            cmd.y = abs_y - sy;
            cmd.w = total_w;
            cmd.h = total_h;
            cmd.color = bg;

            float radius = box->style->props[CSS_PROP_BORDER_RADIUS].number;
            if (radius > 0) {
                cmd.type = PAINT_ROUNDED_RECT;
                cmd.border_radius = radius;
            }
            emit(out, &cmd);
        }

        /* Border */
        float bw = box->style->props[CSS_PROP_BORDER_WIDTH].number;
        if (bw > 0) {
            struct paint_cmd cmd = {0};
            cmd.type = PAINT_BORDER;
            cmd.x = abs_x - sx;
            cmd.y = abs_y - sy;
            cmd.w = total_w;
            cmd.h = total_h;
            cmd.border_width = bw;
            cmd.border_color = box->style->props[CSS_PROP_COLOR].color;
            emit(out, &cmd);
        }
    }

    /* Text content */
    if (box->text && box->text_len > 0) {
        struct paint_cmd cmd = {0};
        cmd.type = PAINT_TEXT;
        cmd.x = abs_x - sx + box->padding[3]; /* left padding */
        cmd.y = abs_y - sy + box->padding[0]; /* top padding */
        cmd.text = box->text;
        cmd.text_len = box->text_len;
        cmd.color = 0xFF000000; /* default black */
        cmd.font_size = 16.0f;  /* default */
        if (box->style) {
            cmd.color = box->style->props[CSS_PROP_COLOR].color;
            float fs = box->style->props[CSS_PROP_FONT_SIZE].number;
            if (fs > 0) cmd.font_size = fs;
        }
        emit(out, &cmd);
    }

paint_children:
    /* Paint children */
    for (struct layout_box *child = box->first_child; child; child = child->next_sibling) {
        paint_box(child, out, sx, sy, vw, vh);
    }
}

void paint_layout(struct layout_box *root, struct paint_list *out,
                  float scroll_x, float scroll_y,
                  float viewport_w, float viewport_h) {
    if (!out) return;
    out->count = 0;
    if (!root) return;
    paint_box(root, out, scroll_x, scroll_y, viewport_w, viewport_h);
}

void paint_execute(struct paint_list *list, int window_fd) {
    if (!list || window_fd < 0) return;

    /* Execute paint commands by issuing GUI syscalls.
     * Each command maps to one or more compositor operations. */
    for (int i = 0; i < list->count; i++) {
        struct paint_cmd *cmd = &list->cmds[i];
        (void)cmd;
        /* GUI syscalls would be called here:
         * SYS_GUI_FILL for PAINT_FILL_RECT
         * SYS_GUI_TEXT for PAINT_TEXT
         * SYS_GUI_BLIT for PAINT_IMAGE
         * etc. */
    }
}

/* ---- Scrolling ---- */

void scroll_init(struct scroll_state *s, float content_w, float content_h,
                 float viewport_w, float viewport_h) {
    if (!s) return;
    s->offset_x = 0;
    s->offset_y = 0;
    s->content_width = content_w;
    s->content_height = content_h;
    s->viewport_width = viewport_w;
    s->viewport_height = viewport_h;
    s->max_scroll_x = content_w > viewport_w ? content_w - viewport_w : 0;
    s->max_scroll_y = content_h > viewport_h ? content_h - viewport_h : 0;
}

void scroll_by(struct scroll_state *s, float dx, float dy) {
    if (!s) return;
    s->offset_x += dx;
    s->offset_y += dy;
    if (s->offset_x < 0) s->offset_x = 0;
    if (s->offset_y < 0) s->offset_y = 0;
    if (s->offset_x > s->max_scroll_x) s->offset_x = s->max_scroll_x;
    if (s->offset_y > s->max_scroll_y) s->offset_y = s->max_scroll_y;
}

void scroll_to(struct scroll_state *s, float x, float y) {
    if (!s) return;
    s->offset_x = x;
    s->offset_y = y;
    if (s->offset_x < 0) s->offset_x = 0;
    if (s->offset_y < 0) s->offset_y = 0;
    if (s->offset_x > s->max_scroll_x) s->offset_x = s->max_scroll_x;
    if (s->offset_y > s->max_scroll_y) s->offset_y = s->max_scroll_y;
}

/* ---- Image cache ---- */

struct cached_image *image_cache_get(struct image_cache *cache, const char *url) {
    if (!cache || !url) return NULL;

    /* Check existing entries */
    for (int i = 0; i < cache->count; i++) {
        if (strcmp(cache->entries[i].url, url) == 0)
            return &cache->entries[i];
    }

    /* Allocate new entry */
    if (cache->count >= IMG_CACHE_MAX) return NULL;
    struct cached_image *img = &cache->entries[cache->count++];
    memset(img, 0, sizeof(*img));
    size_t ulen = strlen(url);
    if (ulen > 255) ulen = 255;
    memcpy(img->url, url, ulen);
    img->url[ulen] = '\0';

    /* Fetch image data */
    struct http_response resp;
    if (http_get(url, &resp) == 0 && resp.status_code == 200 && resp.body) {
        /* Decode with stb_image (available in the project) */
        /* For now just mark as loaded -- actual decode would use stbi_load_from_memory */
        img->loaded = 0; /* Will be decoded by caller */
        /* Store raw data for later decode */
        img->pixels = (uint32_t *)resp.body;
        img->width = (int)resp.body_len;  /* Temporarily store raw data size in width */
        img->height = 0; /* Signals raw, not decoded */
        resp.body = NULL; /* Transfer ownership */
    }
    http_response_free(&resp);

    return img;
}

void image_cache_free(struct image_cache *cache) {
    if (!cache) return;
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].pixels) {
            free(cache->entries[i].pixels);
            cache->entries[i].pixels = NULL;
        }
    }
    cache->count = 0;
}
