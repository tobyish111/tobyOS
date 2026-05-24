/* libtoby/src/ui.c -- Widget Toolkit "TobyUI" (Phase 2 M2.3).
 *
 * Implements a retained-mode widget toolkit with:
 *   - Flexbox-like layout engine
 *   - Standard widget rendering
 *   - Event dispatch and focus management
 */

#include <toby/ui.h>
#include <stdlib.h>
#include <string.h>

/* ---- Default theme colors ---- */
#define THEME_BG          0xFFF0F0F0
#define THEME_FG          0xFF1A1A1A
#define THEME_ACCENT      0xFF0078D4
#define THEME_BORDER      0xFFCCCCCC
#define THEME_BTN_BG      0xFFE1E1E1
#define THEME_BTN_HOVER   0xFFD0D0D0
#define THEME_BTN_ACTIVE  0xFFC0C0C0
#define THEME_INPUT_BG    0xFFFFFFFF
#define THEME_CHECK       0xFF0078D4
#define THEME_SLIDER_BG   0xFFDDDDDD
#define THEME_SLIDER_FG   0xFF0078D4
#define THEME_PROGRESS_BG 0xFFE0E0E0
#define THEME_PROGRESS_FG 0xFF0078D4

/* ---- Helpers ---- */

static void fill_rect(uint32_t *fb, int fb_w, int fb_h,
                      int x, int y, int w, int h, uint32_t color) {
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= fb_h) continue;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= fb_w) continue;
            fb[dy * fb_w + dx] = color;
        }
    }
}

static void draw_border(uint32_t *fb, int fb_w, int fb_h,
                        int x, int y, int w, int h, uint32_t color, int bw) {
    fill_rect(fb, fb_w, fb_h, x, y, w, bw, color);           /* top */
    fill_rect(fb, fb_w, fb_h, x, y + h - bw, w, bw, color);  /* bottom */
    fill_rect(fb, fb_w, fb_h, x, y, bw, h, color);           /* left */
    fill_rect(fb, fb_w, fb_h, x + w - bw, y, bw, h, color);  /* right */
}

static void draw_text_simple(uint32_t *fb, int fb_w, int fb_h,
                             int x, int y, const char *text, uint32_t color) {
    /* Simple 8-pixel text rendering using syscall interface.
     * In a full implementation this would use toby_font_draw_text(). */
    (void)fb; (void)fb_w; (void)fb_h;
    (void)x; (void)y; (void)text; (void)color;
}

/* ---- Widget creation ---- */

ui_widget_t *ui_widget_new(ui_widget_type_t type) {
    ui_widget_t *w = (ui_widget_t *)malloc(sizeof(ui_widget_t));
    if (!w) return 0;
    memset(w, 0, sizeof(*w));
    w->type = type;
    w->visible = 1;
    w->enabled = 1;
    w->value_max = 100;
    w->style.fg_color = THEME_FG;
    w->style.font_size = 14.0f;
    return w;
}

void ui_widget_free(ui_widget_t *w) {
    if (!w) return;
    for (int i = 0; i < w->n_children; i++) {
        ui_widget_free(w->children[i]);
    }
    free(w);
}

void ui_widget_add_child(ui_widget_t *parent, ui_widget_t *child) {
    if (!parent || !child) return;
    if (parent->n_children >= UI_MAX_CHILDREN) return;
    child->parent = parent;
    parent->children[parent->n_children++] = child;
}

void ui_widget_remove_child(ui_widget_t *parent, ui_widget_t *child) {
    if (!parent || !child) return;
    for (int i = 0; i < parent->n_children; i++) {
        if (parent->children[i] == child) {
            parent->n_children--;
            for (int j = i; j < parent->n_children; j++)
                parent->children[j] = parent->children[j + 1];
            child->parent = 0;
            return;
        }
    }
}

/* ---- Convenience constructors ---- */

ui_widget_t *ui_label(const char *text) {
    ui_widget_t *w = ui_widget_new(UI_WIDGET_LABEL);
    if (w && text) ui_set_text(w, text);
    return w;
}

ui_widget_t *ui_button(const char *text, ui_event_handler_t on_click) {
    ui_widget_t *w = ui_widget_new(UI_WIDGET_BUTTON);
    if (!w) return 0;
    if (text) ui_set_text(w, text);
    w->on_click = on_click;
    w->min_h = 32;
    w->style.bg_color = THEME_BTN_BG;
    w->style.border_color = THEME_BORDER;
    w->style.border_width = 1;
    w->style.padding[0] = 6;  /* top */
    w->style.padding[1] = 12; /* right */
    w->style.padding[2] = 6;  /* bottom */
    w->style.padding[3] = 12; /* left */
    w->style_hover.bg_color = THEME_BTN_HOVER;
    w->style_active.bg_color = THEME_BTN_ACTIVE;
    return w;
}

ui_widget_t *ui_textinput(const char *placeholder) {
    ui_widget_t *w = ui_widget_new(UI_WIDGET_TEXTINPUT);
    if (!w) return 0;
    if (placeholder) ui_set_text(w, placeholder);
    w->min_h = 30;
    w->style.bg_color = THEME_INPUT_BG;
    w->style.border_color = THEME_BORDER;
    w->style.border_width = 1;
    w->style.padding[0] = 4;
    w->style.padding[1] = 8;
    w->style.padding[2] = 4;
    w->style.padding[3] = 8;
    w->style_focus.border_color = THEME_ACCENT;
    return w;
}

ui_widget_t *ui_checkbox(const char *label, int checked) {
    ui_widget_t *w = ui_widget_new(UI_WIDGET_CHECKBOX);
    if (!w) return 0;
    if (label) ui_set_text(w, label);
    w->checked = checked;
    w->min_h = 24;
    return w;
}

ui_widget_t *ui_slider(int min_val, int max_val, int value) {
    ui_widget_t *w = ui_widget_new(UI_WIDGET_SLIDER);
    if (!w) return 0;
    w->value_min = min_val;
    w->value_max = max_val;
    w->value = value;
    w->min_h = 24;
    return w;
}

ui_widget_t *ui_progress(int value, int max_val) {
    ui_widget_t *w = ui_widget_new(UI_WIDGET_PROGRESS);
    if (!w) return 0;
    w->value = value;
    w->value_max = max_val;
    w->min_h = 20;
    return w;
}

ui_widget_t *ui_vbox(int gap) {
    ui_widget_t *w = ui_widget_new(UI_WIDGET_CONTAINER);
    if (!w) return 0;
    w->layout = UI_LAYOUT_VBOX;
    w->gap = gap;
    return w;
}

ui_widget_t *ui_hbox(int gap) {
    ui_widget_t *w = ui_widget_new(UI_WIDGET_CONTAINER);
    if (!w) return 0;
    w->layout = UI_LAYOUT_HBOX;
    w->gap = gap;
    return w;
}

/* ---- Layout engine ---- */

static void layout_vbox(ui_widget_t *w) {
    int y = w->style.padding[0];
    int available_w = w->width - w->style.padding[1] - w->style.padding[3];

    for (int i = 0; i < w->n_children; i++) {
        ui_widget_t *child = w->children[i];
        if (!child->visible) continue;

        child->x = w->style.padding[3];
        child->y = y;

        if (child->width == 0 || child->flex_grow > 0)
            child->width = available_w;
        if (child->height == 0)
            child->height = child->min_h > 0 ? child->min_h : 30;

        y += child->height + w->gap;
    }
}

static void layout_hbox(ui_widget_t *w) {
    int x = w->style.padding[3];
    int available_h = w->height - w->style.padding[0] - w->style.padding[2];

    int total_flex = 0;
    int fixed_width = 0;
    for (int i = 0; i < w->n_children; i++) {
        ui_widget_t *child = w->children[i];
        if (!child->visible) continue;
        if (child->flex_grow > 0) total_flex += child->flex_grow;
        else fixed_width += (child->width > 0 ? child->width : child->min_w);
        if (i > 0) fixed_width += w->gap;
    }

    int flex_space = w->width - w->style.padding[1] - w->style.padding[3] - fixed_width;
    if (flex_space < 0) flex_space = 0;

    for (int i = 0; i < w->n_children; i++) {
        ui_widget_t *child = w->children[i];
        if (!child->visible) continue;

        child->x = x;
        child->y = w->style.padding[0];

        if (child->flex_grow > 0 && total_flex > 0)
            child->width = flex_space * child->flex_grow / total_flex;
        else if (child->width == 0)
            child->width = child->min_w > 0 ? child->min_w : 80;

        child->height = available_h > 0 ? available_h : (child->min_h > 0 ? child->min_h : 30);

        x += child->width + w->gap;
    }
}

static void layout_recursive(ui_widget_t *w) {
    if (!w || !w->visible) return;

    switch (w->layout) {
    case UI_LAYOUT_VBOX: layout_vbox(w); break;
    case UI_LAYOUT_HBOX: layout_hbox(w); break;
    default: break;
    }

    for (int i = 0; i < w->n_children; i++) {
        layout_recursive(w->children[i]);
    }
}

void ui_layout(ui_context_t *ctx) {
    if (!ctx || !ctx->root) return;
    ctx->root->x = 0;
    ctx->root->y = 0;
    ctx->root->width = ctx->width;
    ctx->root->height = ctx->height;
    layout_recursive(ctx->root);
}

/* ---- Painting ---- */

static void paint_button(ui_widget_t *w, uint32_t *fb, int fb_w, int fb_h,
                         int ox, int oy) {
    int ax = ox + w->x, ay = oy + w->y;
    uint32_t bg = w->style.bg_color;
    if (w->pressed) bg = w->style_active.bg_color ? w->style_active.bg_color : THEME_BTN_ACTIVE;
    else if (w->hovered) bg = w->style_hover.bg_color ? w->style_hover.bg_color : THEME_BTN_HOVER;
    if (!bg) bg = THEME_BTN_BG;

    fill_rect(fb, fb_w, fb_h, ax, ay, w->width, w->height, bg);
    if (w->style.border_width)
        draw_border(fb, fb_w, fb_h, ax, ay, w->width, w->height,
                    w->style.border_color ? w->style.border_color : THEME_BORDER,
                    w->style.border_width);
}

static void paint_checkbox(ui_widget_t *w, uint32_t *fb, int fb_w, int fb_h,
                           int ox, int oy) {
    int ax = ox + w->x, ay = oy + w->y;
    int box_size = 16;
    int box_y = ay + (w->height - box_size) / 2;

    /* Box outline */
    fill_rect(fb, fb_w, fb_h, ax, box_y, box_size, box_size,
              w->checked ? THEME_CHECK : THEME_INPUT_BG);
    draw_border(fb, fb_w, fb_h, ax, box_y, box_size, box_size,
                THEME_BORDER, 1);

    /* Checkmark (simplified) */
    if (w->checked) {
        fill_rect(fb, fb_w, fb_h, ax + 3, box_y + 3, box_size - 6, box_size - 6,
                  0xFFFFFFFF);
    }
}

static void paint_slider(ui_widget_t *w, uint32_t *fb, int fb_w, int fb_h,
                         int ox, int oy) {
    int ax = ox + w->x, ay = oy + w->y;
    int track_h = 4;
    int track_y = ay + (w->height - track_h) / 2;

    /* Track */
    fill_rect(fb, fb_w, fb_h, ax, track_y, w->width, track_h, THEME_SLIDER_BG);

    /* Filled portion */
    int range = w->value_max - w->value_min;
    int fill_w = range > 0 ? (w->value - w->value_min) * w->width / range : 0;
    fill_rect(fb, fb_w, fb_h, ax, track_y, fill_w, track_h, THEME_SLIDER_FG);

    /* Thumb */
    int thumb_x = ax + fill_w - 6;
    int thumb_y = ay + (w->height - 14) / 2;
    fill_rect(fb, fb_w, fb_h, thumb_x, thumb_y, 12, 14, THEME_ACCENT);
}

static void paint_progress(ui_widget_t *w, uint32_t *fb, int fb_w, int fb_h,
                           int ox, int oy) {
    int ax = ox + w->x, ay = oy + w->y;

    fill_rect(fb, fb_w, fb_h, ax, ay, w->width, w->height, THEME_PROGRESS_BG);

    int fill_w = w->value_max > 0 ? w->value * w->width / w->value_max : 0;
    fill_rect(fb, fb_w, fb_h, ax, ay, fill_w, w->height, THEME_PROGRESS_FG);
}

void ui_paint_widget(ui_widget_t *w, uint32_t *fb, int fb_w, int fb_h,
                     int ox, int oy) {
    if (!w || !w->visible) return;

    /* Custom paint override */
    if (w->on_paint) {
        w->on_paint(w, fb, fb_w, fb_h);
        return;
    }

    int ax = ox + w->x, ay = oy + w->y;

    switch (w->type) {
    case UI_WIDGET_CONTAINER:
        if (w->style.bg_color)
            fill_rect(fb, fb_w, fb_h, ax, ay, w->width, w->height, w->style.bg_color);
        break;
    case UI_WIDGET_LABEL:
        draw_text_simple(fb, fb_w, fb_h, ax, ay, w->text, w->style.fg_color);
        break;
    case UI_WIDGET_BUTTON:
        paint_button(w, fb, fb_w, fb_h, ox, oy);
        break;
    case UI_WIDGET_TEXTINPUT:
        fill_rect(fb, fb_w, fb_h, ax, ay, w->width, w->height, THEME_INPUT_BG);
        draw_border(fb, fb_w, fb_h, ax, ay, w->width, w->height,
                    w->focused ? THEME_ACCENT : THEME_BORDER, 1);
        break;
    case UI_WIDGET_CHECKBOX:
        paint_checkbox(w, fb, fb_w, fb_h, ox, oy);
        break;
    case UI_WIDGET_SLIDER:
        paint_slider(w, fb, fb_w, fb_h, ox, oy);
        break;
    case UI_WIDGET_PROGRESS:
        paint_progress(w, fb, fb_w, fb_h, ox, oy);
        break;
    default:
        break;
    }

    /* Paint children */
    for (int i = 0; i < w->n_children; i++) {
        ui_paint_widget(w->children[i], fb, fb_w, fb_h, ax, ay);
    }
}

void ui_paint(ui_context_t *ctx) {
    if (!ctx || !ctx->root || !ctx->framebuffer) return;
    ui_paint_widget(ctx->root, ctx->framebuffer, ctx->width, ctx->height, 0, 0);
}

/* ---- Event dispatch ---- */

static ui_widget_t *hit_test(ui_widget_t *w, int x, int y, int ox, int oy) {
    if (!w || !w->visible) return 0;

    int ax = ox + w->x, ay = oy + w->y;
    if (x < ax || x >= ax + w->width || y < ay || y >= ay + w->height)
        return 0;

    /* Check children (front to back) */
    for (int i = w->n_children - 1; i >= 0; i--) {
        ui_widget_t *hit = hit_test(w->children[i], x, y, ax, ay);
        if (hit) return hit;
    }
    return w;
}

void ui_dispatch_event(ui_context_t *ctx, ui_event_t *event) {
    if (!ctx || !event || !ctx->root) return;

    switch (event->type) {
    case UI_EVENT_CLICK:
    case UI_EVENT_MOUSEDOWN:
    case UI_EVENT_MOUSEUP: {
        ui_widget_t *target = hit_test(ctx->root, event->x, event->y, 0, 0);
        if (target) {
            if (event->type == UI_EVENT_MOUSEDOWN) {
                target->pressed = 1;
                ui_set_focus(ctx, target);
            } else if (event->type == UI_EVENT_MOUSEUP) {
                if (target->pressed && target->on_click)
                    target->on_click(target, event);
                target->pressed = 0;

                /* Toggle checkbox */
                if (target->type == UI_WIDGET_CHECKBOX)
                    target->checked = !target->checked;
            }
        }
        break;
    }
    case UI_EVENT_MOUSEMOVE: {
        ui_widget_t *target = hit_test(ctx->root, event->x, event->y, 0, 0);
        if (ctx->hover != target) {
            if (ctx->hover) ctx->hover->hovered = 0;
            ctx->hover = target;
            if (target) target->hovered = 1;
        }
        break;
    }
    case UI_EVENT_KEYDOWN:
        if (event->key == '\t') {
            ui_focus_next(ctx);
        } else if (ctx->focus && ctx->focus->on_keydown) {
            ctx->focus->on_keydown(ctx->focus, event);
        }
        break;
    default:
        break;
    }
}

/* ---- Focus management ---- */

static ui_widget_t *find_focusable(ui_widget_t *w, ui_widget_t *after, int *found) {
    if (!w || !w->visible || !w->enabled) return 0;

    int is_focusable = (w->type == UI_WIDGET_BUTTON ||
                        w->type == UI_WIDGET_TEXTINPUT ||
                        w->type == UI_WIDGET_CHECKBOX ||
                        w->type == UI_WIDGET_SLIDER ||
                        w->type == UI_WIDGET_DROPDOWN);

    if (is_focusable && *found) return w;
    if (w == after) *found = 1;

    for (int i = 0; i < w->n_children; i++) {
        ui_widget_t *r = find_focusable(w->children[i], after, found);
        if (r) return r;
    }
    return 0;
}

void ui_focus_next(ui_context_t *ctx) {
    if (!ctx || !ctx->root) return;
    int found = (ctx->focus == 0) ? 1 : 0;
    ui_widget_t *next = find_focusable(ctx->root, ctx->focus, &found);
    if (!next) {
        found = 1;
        next = find_focusable(ctx->root, 0, &found);
    }
    ui_set_focus(ctx, next);
}

void ui_focus_prev(ui_context_t *ctx) {
    /* Simplified: just wrap to next for now */
    ui_focus_next(ctx);
}

void ui_set_focus(ui_context_t *ctx, ui_widget_t *w) {
    if (!ctx) return;
    if (ctx->focus) ctx->focus->focused = 0;
    ctx->focus = w;
    if (w) w->focused = 1;
}

/* ---- Context management ---- */

ui_context_t *ui_create(int win_fd, uint32_t *framebuffer, int w, int h) {
    ui_context_t *ctx = (ui_context_t *)malloc(sizeof(ui_context_t));
    if (!ctx) return 0;
    memset(ctx, 0, sizeof(*ctx));
    ctx->win_fd = win_fd;
    ctx->framebuffer = framebuffer;
    ctx->width = w;
    ctx->height = h;
    return ctx;
}

void ui_destroy(ui_context_t *ctx) {
    if (!ctx) return;
    if (ctx->root) ui_widget_free(ctx->root);
    free(ctx);
}

/* ---- Utility ---- */

void ui_set_text(ui_widget_t *w, const char *text) {
    if (!w || !text) return;
    size_t len = strlen(text);
    if (len >= UI_TEXT_MAX) len = UI_TEXT_MAX - 1;
    memcpy(w->text, text, len);
    w->text[len] = '\0';
}

const char *ui_get_text(ui_widget_t *w) {
    return w ? w->text : "";
}

void ui_set_style(ui_widget_t *w, ui_style_t *style) {
    if (w && style) w->style = *style;
}
