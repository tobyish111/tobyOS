/* toby_gui.c -- minimal user-space GUI toolkit implementation.
 *
 * Layout of responsibilities:
 *
 *   tg_app_init()          opens the window, paints the bg.
 *   tg_label/button/...()  appends to app->widgets[], marks redraw.
 *   tg_run()               loop:
 *                            1. poll one event (sys_gui_poll_event)
 *                            2. if event: dispatch_event()
 *                            3. if want_redraw: redraw_all()
 *                            4. sys_yield to let the compositor run
 *   dispatch_event()       routes mouse vs key, manages focus + capture
 *                            via per-widget on_event_*() helpers.
 *
 * Drawing is fully synchronous and uses the kernel-side back buffer:
 * widgets paint themselves into the window via sys_gui_fill +
 * sys_gui_text, then the toolkit calls sys_gui_flip once per frame.
 *
 * We keep all syscall stubs local so this file is fully self-contained
 * and links cleanly into any user program that includes toby_gui.h.
 */

#include "toby_gui.h"

/* ---- syscall numbers ---------------------------------------------- */

#define SYS_WRITE           1
#define SYS_YIELD           5
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14

/* ---- syscall stubs (same ABI as user_gui_demo) -------------------- */

static inline tg_ssize_t sys_write(int fd, const void *buf, tg_size_t len) {
    tg_ssize_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
static inline void sys_yield(void) {
    long _dummy;
    __asm__ volatile ("syscall"
        : "=a"(_dummy) : "0"((long)SYS_YIELD)
        : "rcx", "r11", "memory");
}
static inline int sys_gui_create(tg_uint32_t w, tg_uint32_t h, const char *title) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_CREATE), "D"((long)w), "S"((long)h), "d"(title)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_fill(int fd, int x, int y, int w, int h,
                               tg_uint32_t color) {
    long r;
    tg_uint32_t whlen = ((tg_uint32_t)(tg_uint16_t)w) |
                        (((tg_uint32_t)(tg_uint16_t)h) << 16);
    register long r10 __asm__("r10") = (long)whlen;
    register long r8  __asm__("r8")  = (long)color;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FILL), "D"((long)fd),
          "S"((long)x), "d"((long)y),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_text(int fd, int x, int y, const char *s,
                               tg_uint32_t fg, tg_uint32_t bg) {
    long r;
    tg_uint32_t xy = ((tg_uint32_t)(tg_uint16_t)x) |
                     (((tg_uint32_t)(tg_uint16_t)y) << 16);
    register long r10 __asm__("r10") = (long)fg;
    register long r8  __asm__("r8")  = (long)bg;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_TEXT), "D"((long)fd),
          "S"((long)xy), "d"(s),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_flip(int fd) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FLIP), "D"((long)fd)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_poll_event(int fd, struct tg_event *ev) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_POLL_EVENT), "D"((long)fd), "S"(ev)
        : "rcx", "r11", "memory");
    return (int)r;
}

/* ---- tiny libc helpers -------------------------------------------- */

static tg_size_t tg_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (tg_size_t)(p - s);
}
static void tg_strcpy_capped(char *dst, const char *src, tg_size_t cap) {
    tg_size_t i = 0;
    if (cap == 0) return;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* ---- colours (single global "theme") ------------------------------ */

#define TG_COL_BG           0x000E1622u
#define TG_COL_PANEL        0x00141F30u
#define TG_COL_PANEL_HI     0x0020334Eu
#define TG_COL_LABEL_FG     0x00DDEEFFu
#define TG_COL_LABEL_DIM    0x007B93ADu
#define TG_COL_BTN_FACE     0x00142134u
#define TG_COL_BTN_FACE_H   0x001F365Cu
#define TG_COL_BTN_BORDER   0x00324762u
#define TG_COL_BTN_TEXT     0x00EAF8FFu
#define TG_COL_INPUT_BG     0x00081018u
#define TG_COL_INPUT_FG     0x00E8F7FFu
#define TG_COL_INPUT_BORDER 0x00324762u
#define TG_COL_FOCUS_RING   0x0000E6FFu
#define TG_COL_ACCENT_2     0x00FF36C8u
#define TG_COL_CHECK_MARK   0x0000E6FFu
#define TG_COL_PROGRESS_FG  0x0000B8CCu
#define TG_COL_PROGRESS_BG  0x00081018u
#define TG_COL_SEP          0x00324762u
#define TG_COL_LIST_BG      0x00081018u
#define TG_COL_LIST_SEL     0x001A3A56u
#define TG_COL_LIST_FG      0x00DDEEFFu
#define TG_COL_SCROLLBAR    0x00506878u

#define TG_LIST_ITEM_H      16

/* ---- forward decls ------------------------------------------------ */

static int  point_in(struct tg_widget *w, int x, int y);
static void redraw_all(struct tg_app *app);
static void draw_widget(struct tg_app *app, int idx);
static void dispatch_event(struct tg_app *app, const struct tg_event *ev);

/* ---- widget creation --------------------------------------------- */

static struct tg_widget *alloc_widget(struct tg_app *app) {
    if (app->n_widgets >= TG_MAX_WIDGETS) return 0;
    struct tg_widget *w = &app->widgets[app->n_widgets++];
    char *p = (char *)w;
    for (tg_size_t i = 0; i < sizeof(*w); i++) p[i] = 0;
    return w;
}

struct tg_widget *tg_label(struct tg_app *app, int x, int y, int w, int h,
                           const char *text) {
    struct tg_widget *wd = alloc_widget(app);
    if (!wd) return 0;
    wd->kind = TG_WIDGET_LABEL;
    wd->x = x; wd->y = y; wd->w = w; wd->h = h;
    wd->focusable = 0;
    tg_strcpy_capped(wd->text, text, TG_TEXT_MAX);
    app->want_redraw = 1;
    return wd;
}

struct tg_widget *tg_button(struct tg_app *app, int x, int y, int w, int h,
                            const char *text, tg_button_cb cb) {
    struct tg_widget *wd = alloc_widget(app);
    if (!wd) return 0;
    wd->kind = TG_WIDGET_BUTTON;
    wd->x = x; wd->y = y; wd->w = w; wd->h = h;
    wd->focusable = 1;
    wd->on_click = cb;
    tg_strcpy_capped(wd->text, text, TG_TEXT_MAX);
    app->want_redraw = 1;
    return wd;
}

struct tg_widget *tg_textinput(struct tg_app *app, int x, int y, int w, int h) {
    struct tg_widget *wd = alloc_widget(app);
    if (!wd) return 0;
    wd->kind = TG_WIDGET_TEXTINPUT;
    wd->x = x; wd->y = y; wd->w = w; wd->h = h;
    wd->focusable = 1;
    wd->text[0] = '\0';
    app->want_redraw = 1;
    return wd;
}

int tg_checkbox(struct tg_app *app, int x, int y, int w, int h,
                const char *label, int initial) {
    struct tg_widget *wd = alloc_widget(app);
    if (!wd) return -1;
    int idx = app->n_widgets - 1;
    wd->kind = TG_WIDGET_CHECKBOX;
    wd->x = x; wd->y = y; wd->w = w; wd->h = h;
    wd->focusable = 1;
    wd->checked = initial ? 1 : 0;
    tg_strcpy_capped(wd->text, label, TG_TEXT_MAX);
    app->want_redraw = 1;
    return idx;
}

int tg_progress(struct tg_app *app, int x, int y, int w, int h,
                tg_uint8_t value) {
    struct tg_widget *wd = alloc_widget(app);
    if (!wd) return -1;
    int idx = app->n_widgets - 1;
    wd->kind = TG_WIDGET_PROGRESS;
    wd->x = x; wd->y = y; wd->w = w; wd->h = h;
    wd->focusable = 0;
    wd->progress = value > 100 ? 100 : value;
    app->want_redraw = 1;
    return idx;
}

int tg_separator(struct tg_app *app, int x, int y, int w) {
    struct tg_widget *wd = alloc_widget(app);
    if (!wd) return -1;
    int idx = app->n_widgets - 1;
    wd->kind = TG_WIDGET_SEPARATOR;
    wd->x = x; wd->y = y; wd->w = w; wd->h = 1;
    wd->focusable = 0;
    app->want_redraw = 1;
    return idx;
}

int tg_listview(struct tg_app *app, int x, int y, int w, int h,
                const char **items, int count) {
    struct tg_widget *wd = alloc_widget(app);
    if (!wd) return -1;
    int idx = app->n_widgets - 1;
    wd->kind = TG_WIDGET_LISTVIEW;
    wd->x = x; wd->y = y; wd->w = w; wd->h = h;
    wd->focusable = 1;
    wd->list_items = items;
    wd->list_count = count;
    wd->list_selected = 0;
    wd->list_scroll = 0;
    app->want_redraw = 1;
    return idx;
}

void tg_set_text(struct tg_app *app, struct tg_widget *w, const char *text) {
    if (!w) return;
    tg_strcpy_capped(w->text, text, TG_TEXT_MAX);
    if (app) app->want_redraw = 1;
}
const char *tg_get_text(struct tg_widget *w) {
    return w ? w->text : "";
}

void tg_request_redraw(struct tg_app *app) { app->want_redraw = 1; }
void tg_app_quit(struct tg_app *app)       { app->want_quit   = 1; }

/* ---- accessor functions ------------------------------------------ */

void tg_checkbox_set(struct tg_app *app, int idx, int checked) {
    if (idx < 0 || idx >= app->n_widgets) return;
    struct tg_widget *w = &app->widgets[idx];
    if (w->kind != TG_WIDGET_CHECKBOX) return;
    w->checked = checked ? 1 : 0;
    app->want_redraw = 1;
}

int tg_checkbox_get(struct tg_app *app, int idx) {
    if (idx < 0 || idx >= app->n_widgets) return 0;
    struct tg_widget *w = &app->widgets[idx];
    if (w->kind != TG_WIDGET_CHECKBOX) return 0;
    return w->checked;
}

void tg_progress_set(struct tg_app *app, int idx, tg_uint8_t value) {
    if (idx < 0 || idx >= app->n_widgets) return;
    struct tg_widget *w = &app->widgets[idx];
    if (w->kind != TG_WIDGET_PROGRESS) return;
    w->progress = value > 100 ? 100 : value;
    app->want_redraw = 1;
}

void tg_listview_set_selected(struct tg_app *app, int idx, int sel) {
    if (idx < 0 || idx >= app->n_widgets) return;
    struct tg_widget *w = &app->widgets[idx];
    if (w->kind != TG_WIDGET_LISTVIEW) return;
    if (sel < 0) sel = 0;
    if (sel >= w->list_count) sel = w->list_count - 1;
    if (sel < 0) sel = 0;
    w->list_selected = sel;
    app->want_redraw = 1;
}

int tg_listview_get_selected(struct tg_app *app, int idx) {
    if (idx < 0 || idx >= app->n_widgets) return -1;
    struct tg_widget *w = &app->widgets[idx];
    if (w->kind != TG_WIDGET_LISTVIEW) return -1;
    return w->list_selected;
}

/* ---- hit-testing ------------------------------------------------- */

static int point_in(struct tg_widget *w, int x, int y) {
    return x >= w->x && x < w->x + w->w &&
           y >= w->y && y < w->y + w->h;
}

static int hit_test(struct tg_app *app, int x, int y) {
    for (int i = app->n_widgets - 1; i >= 0; i--) {
        if (point_in(&app->widgets[i], x, y)) return i;
    }
    return -1;
}

/* ---- drawing ----------------------------------------------------- */

static void draw_rect_border(struct tg_app *app, int x, int y, int w, int h,
                             tg_uint32_t color) {
    sys_gui_fill(app->fd, x,         y,         w, 1, color);
    sys_gui_fill(app->fd, x,         y + h - 1, w, 1, color);
    sys_gui_fill(app->fd, x,         y,         1, h, color);
    sys_gui_fill(app->fd, x + w - 1, y,         1, h, color);
}

static void draw_surface(struct tg_app *app, int x, int y, int w, int h,
                         tg_uint32_t fill, tg_uint32_t border,
                         tg_uint32_t accent) {
    sys_gui_fill(app->fd, x, y, w, h, fill);
    if (h > 2 && w > 2) {
        sys_gui_fill(app->fd, x + 1, y + 1, w - 2, 1, TG_COL_PANEL_HI);
    }
    draw_rect_border(app, x, y, w, h, border);
    if (w > 2) sys_gui_fill(app->fd, x + 1, y, w - 2, 1, accent);
}

static int text_baseline_y(int widget_y, int widget_h) {
    int by = widget_y + (widget_h - 8) / 2;
    if (by < widget_y) by = widget_y;
    return by;
}

static int text_centred_x(int widget_x, int widget_w, int n_chars) {
    int tw = n_chars * 8;
    int bx = widget_x + (widget_w - tw) / 2;
    if (bx < widget_x) bx = widget_x;
    return bx;
}

static void draw_label(struct tg_app *app, struct tg_widget *w) {
    sys_gui_fill(app->fd, w->x, w->y, w->w, w->h, TG_COL_BG);
    int by = text_baseline_y(w->y, w->h);
    sys_gui_text(app->fd, w->x, by, w->text, TG_COL_LABEL_FG, TG_COL_BG);
}

static void draw_button(struct tg_app *app, struct tg_widget *w, int focused) {
    tg_uint32_t face = w->pressed ? TG_COL_BTN_FACE_H : TG_COL_BTN_FACE;
    draw_surface(app, w->x, w->y, w->w, w->h, face, TG_COL_BTN_BORDER,
                 focused ? TG_COL_FOCUS_RING : TG_COL_ACCENT_2);
    if (focused) {
        draw_rect_border(app, w->x + 2, w->y + 2, w->w - 4, w->h - 4,
                         TG_COL_FOCUS_RING);
    }
    int n  = (int)tg_strlen(w->text);
    int tx = text_centred_x(w->x, w->w, n);
    int ty = text_baseline_y(w->y, w->h);
    sys_gui_text(app->fd, tx, ty, w->text, TG_COL_BTN_TEXT, face);
}

static void draw_textinput(struct tg_app *app, struct tg_widget *w, int focused) {
    draw_surface(app, w->x, w->y, w->w, w->h, TG_COL_INPUT_BG,
                 focused ? TG_COL_FOCUS_RING : TG_COL_INPUT_BORDER,
                 focused ? TG_COL_ACCENT_2 : TG_COL_INPUT_BORDER);
    int tx = w->x + 4;
    int ty = text_baseline_y(w->y, w->h);
    int max_chars = (w->w - 8) / 8;
    if (max_chars < 0) max_chars = 0;
    int n = (int)tg_strlen(w->text);
    int start = (n > max_chars) ? (n - max_chars) : 0;
    sys_gui_text(app->fd, tx, ty, w->text + start,
                 TG_COL_INPUT_FG, TG_COL_INPUT_BG);
    if (focused) {
        int caret_x = tx + (n - start) * 8;
        if (caret_x > w->x + w->w - 2) caret_x = w->x + w->w - 2;
        sys_gui_fill(app->fd, caret_x, w->y + 3, 1, w->h - 6,
                     TG_COL_FOCUS_RING);
    }
}

static void draw_checkbox(struct tg_app *app, struct tg_widget *w, int focused) {
    int box_size = 12;
    int bx = w->x + 2;
    int by = w->y + (w->h - box_size) / 2;
    if (by < w->y) by = w->y;

    sys_gui_fill(app->fd, w->x, w->y, w->w, w->h, TG_COL_BG);

    /* Draw the checkbox square */
    sys_gui_fill(app->fd, bx, by, box_size, box_size, TG_COL_INPUT_BG);
    draw_rect_border(app, bx, by, box_size, box_size,
                     focused ? TG_COL_FOCUS_RING : TG_COL_INPUT_BORDER);

    /* Draw check mark (filled inner square) if checked */
    if (w->checked) {
        sys_gui_fill(app->fd, bx + 3, by + 3, box_size - 6, box_size - 6,
                     TG_COL_CHECK_MARK);
    }

    /* Label text to the right of the box */
    int tx = bx + box_size + 6;
    int ty = text_baseline_y(w->y, w->h);
    if (w->text[0]) {
        sys_gui_text(app->fd, tx, ty, w->text, TG_COL_LABEL_FG, TG_COL_BG);
    }
}

static void draw_progress(struct tg_app *app, struct tg_widget *w) {
    /* Border */
    sys_gui_fill(app->fd, w->x, w->y, w->w, w->h, TG_COL_PROGRESS_BG);
    draw_rect_border(app, w->x, w->y, w->w, w->h, TG_COL_INPUT_BORDER);

    /* Filled portion */
    int inner_w = w->w - 2;
    int inner_h = w->h - 2;
    if (inner_w > 0 && inner_h > 0) {
        int fill_w = (inner_w * (int)w->progress) / 100;
        if (fill_w > 0) {
            sys_gui_fill(app->fd, w->x + 1, w->y + 1, fill_w, inner_h,
                         TG_COL_PROGRESS_FG);
        }
    }
}

static void draw_separator(struct tg_app *app, struct tg_widget *w) {
    sys_gui_fill(app->fd, w->x, w->y, w->w, 1, TG_COL_SEP);
}

static void draw_listview(struct tg_app *app, struct tg_widget *w, int focused) {
    /* Background */
    sys_gui_fill(app->fd, w->x, w->y, w->w, w->h, TG_COL_LIST_BG);
    draw_rect_border(app, w->x, w->y, w->w, w->h,
                     focused ? TG_COL_FOCUS_RING : TG_COL_INPUT_BORDER);

    if (!w->list_items || w->list_count <= 0) return;

    int visible_count = (w->h - 2) / TG_LIST_ITEM_H;
    if (visible_count <= 0) visible_count = 1;

    /* Clamp scroll so selected item is visible */
    if (w->list_scroll > w->list_selected)
        w->list_scroll = w->list_selected;
    if (w->list_scroll + visible_count <= w->list_selected)
        w->list_scroll = w->list_selected - visible_count + 1;
    if (w->list_scroll < 0) w->list_scroll = 0;

    /* Draw visible items */
    for (int i = 0; i < visible_count; i++) {
        int item_idx = w->list_scroll + i;
        if (item_idx >= w->list_count) break;

        int iy = w->y + 1 + i * TG_LIST_ITEM_H;
        int iw = w->w - 2;

        /* Highlight selected item */
        if (item_idx == w->list_selected) {
            sys_gui_fill(app->fd, w->x + 1, iy, iw, TG_LIST_ITEM_H,
                         TG_COL_LIST_SEL);
        }

        /* Draw item text */
        const char *item_text = w->list_items[item_idx];
        if (item_text) {
            int ty = iy + (TG_LIST_ITEM_H - 8) / 2;
            tg_uint32_t bg = (item_idx == w->list_selected)
                             ? TG_COL_LIST_SEL : TG_COL_LIST_BG;
            sys_gui_text(app->fd, w->x + 4, ty, item_text,
                         TG_COL_LIST_FG, bg);
        }
    }

    /* Scrollbar indicator (right side) if items overflow */
    if (w->list_count > visible_count) {
        int sb_x = w->x + w->w - 4;
        int track_h = w->h - 2;
        int thumb_h = track_h * visible_count / w->list_count;
        if (thumb_h < 4) thumb_h = 4;
        int thumb_y = w->y + 1 +
                      (track_h - thumb_h) * w->list_scroll /
                      (w->list_count - visible_count);
        sys_gui_fill(app->fd, sb_x, thumb_y, 3, thumb_h, TG_COL_SCROLLBAR);
    }
}

static void draw_widget(struct tg_app *app, int idx) {
    struct tg_widget *w = &app->widgets[idx];
    int focused = (idx == app->focused);
    switch (w->kind) {
    case TG_WIDGET_LABEL:     draw_label(app, w);              break;
    case TG_WIDGET_BUTTON:    draw_button(app, w, focused);    break;
    case TG_WIDGET_TEXTINPUT: draw_textinput(app, w, focused); break;
    case TG_WIDGET_CHECKBOX:  draw_checkbox(app, w, focused);  break;
    case TG_WIDGET_PROGRESS:  draw_progress(app, w);           break;
    case TG_WIDGET_SEPARATOR: draw_separator(app, w);          break;
    case TG_WIDGET_LISTVIEW:  draw_listview(app, w, focused);  break;
    default: break;
    }
}

static void redraw_all(struct tg_app *app) {
    sys_gui_fill(app->fd, 0, 0, app->win_w, app->win_h, app->bg_color);
    sys_gui_fill(app->fd, 0, 0, app->win_w, 1, TG_COL_FOCUS_RING);
    sys_gui_fill(app->fd, 0, 1, app->win_w, 1, TG_COL_PANEL_HI);
    for (int i = 0; i < app->n_widgets; i++) draw_widget(app, i);
    sys_gui_flip(app->fd);
    app->want_redraw = 0;
}

/* ---- per-widget event handling ----------------------------------- */

static int textinput_consume_key(struct tg_widget *w, tg_uint8_t k) {
    if (k == TG_KEY_BACKSPACE) {
        tg_size_t n = tg_strlen(w->text);
        if (n == 0) return 0;
        w->text[n - 1] = '\0';
        return 1;
    }
    if (k < 0x20 || k > 0x7E) return 0;
    tg_size_t n = tg_strlen(w->text);
    if (n + 1 >= TG_TEXT_MAX) return 0;
    w->text[n]     = (char)k;
    w->text[n + 1] = '\0';
    return 1;
}

static void listview_ensure_visible(struct tg_widget *w) {
    int visible_count = (w->h - 2) / TG_LIST_ITEM_H;
    if (visible_count <= 0) visible_count = 1;
    if (w->list_selected < w->list_scroll)
        w->list_scroll = w->list_selected;
    if (w->list_selected >= w->list_scroll + visible_count)
        w->list_scroll = w->list_selected - visible_count + 1;
}

static void dispatch_event(struct tg_app *app, const struct tg_event *ev) {
    if (ev->type == TG_EV_CLOSE) {
        app->want_quit = 1;
        return;
    }
    if (ev->type == TG_EV_MOUSE_DOWN) {
        int hit = hit_test(app, ev->x, ev->y);
        if (hit >= 0 && app->widgets[hit].focusable) {
            if (app->focused != hit) app->want_redraw = 1;
            app->focused = hit;
        } else {
            if (app->focused != -1) app->want_redraw = 1;
            app->focused = -1;
        }
        if (hit >= 0 && app->widgets[hit].kind == TG_WIDGET_BUTTON) {
            app->widgets[hit].pressed = 1;
            app->captured = hit;
            app->want_redraw = 1;
        }
        /* Checkbox: toggle on click */
        if (hit >= 0 && app->widgets[hit].kind == TG_WIDGET_CHECKBOX) {
            app->widgets[hit].checked = !app->widgets[hit].checked;
            app->want_redraw = 1;
        }
        /* List view: select item based on click y position */
        if (hit >= 0 && app->widgets[hit].kind == TG_WIDGET_LISTVIEW) {
            struct tg_widget *w = &app->widgets[hit];
            int rel_y = ev->y - (w->y + 1);
            if (rel_y >= 0) {
                int clicked_item = w->list_scroll + rel_y / TG_LIST_ITEM_H;
                if (clicked_item < w->list_count) {
                    w->list_selected = clicked_item;
                    app->want_redraw = 1;
                }
            }
        }
        return;
    }

    if (ev->type == TG_EV_MOUSE_UP) {
        int cap = app->captured;
        app->captured = -1;
        if (cap >= 0 && app->widgets[cap].kind == TG_WIDGET_BUTTON) {
            int was_pressed = app->widgets[cap].pressed;
            app->widgets[cap].pressed = 0;
            app->want_redraw = 1;
            if (was_pressed && point_in(&app->widgets[cap], ev->x, ev->y)) {
                tg_button_cb cb = app->widgets[cap].on_click;
                if (cb) cb(&app->widgets[cap], app);
            }
        }
        return;
    }

    if (ev->type == TG_EV_MOUSE_MOVE) {
        int cap = app->captured;
        if (cap >= 0 && app->widgets[cap].kind == TG_WIDGET_BUTTON) {
            int now = point_in(&app->widgets[cap], ev->x, ev->y);
            if (now != app->widgets[cap].pressed) {
                app->widgets[cap].pressed = now;
                app->want_redraw = 1;
            }
        }
        return;
    }

    if (ev->type == TG_EV_KEY) {
        int f = app->focused;
        if (f < 0) return;
        struct tg_widget *w = &app->widgets[f];
        if (w->kind == TG_WIDGET_TEXTINPUT) {
            if (ev->key == TG_KEY_ENTER) {
                for (int i = 0; i < app->n_widgets; i++) {
                    if (app->widgets[i].kind == TG_WIDGET_BUTTON &&
                        app->widgets[i].on_click) {
                        app->widgets[i].on_click(&app->widgets[i], app);
                        app->want_redraw = 1;
                        break;
                    }
                }
                return;
            }
            if (textinput_consume_key(w, ev->key)) app->want_redraw = 1;
            return;
        }
        if (w->kind == TG_WIDGET_BUTTON) {
            if (ev->key == TG_KEY_ENTER || ev->key == ' ') {
                if (w->on_click) w->on_click(w, app);
                app->want_redraw = 1;
            }
            return;
        }
        if (w->kind == TG_WIDGET_CHECKBOX) {
            if (ev->key == TG_KEY_ENTER || ev->key == ' ') {
                w->checked = !w->checked;
                app->want_redraw = 1;
            }
            return;
        }
        if (w->kind == TG_WIDGET_LISTVIEW) {
            if (ev->key == TG_KEY_DOWN) {
                if (w->list_selected < w->list_count - 1) {
                    w->list_selected++;
                    listview_ensure_visible(w);
                    app->want_redraw = 1;
                }
            } else if (ev->key == TG_KEY_UP) {
                if (w->list_selected > 0) {
                    w->list_selected--;
                    listview_ensure_visible(w);
                    app->want_redraw = 1;
                }
            }
            return;
        }
    }
}

/* ---- public lifecycle -------------------------------------------- */

int tg_app_init(struct tg_app *app, int win_w, int win_h, const char *title) {
    char *p = (char *)app;
    for (tg_size_t i = 0; i < sizeof(*app); i++) p[i] = 0;

    app->win_w    = win_w;
    app->win_h    = win_h;
    app->bg_color = TG_COL_BG;
    app->focused  = -1;
    app->captured = -1;

    int fd = sys_gui_create((tg_uint32_t)win_w, (tg_uint32_t)win_h, title);
    if (fd < 0) return -1;
    app->fd = fd;
    app->want_redraw = 1;
    return 0;
}

int tg_run(struct tg_app *app) {
    redraw_all(app);

    for (;;) {
        if (app->want_quit) return 0;

        struct tg_event ev;
        int got = sys_gui_poll_event(app->fd, &ev);
        if (got < 0) return -1;
        if (got == 0) {
            if (app->want_redraw) redraw_all(app);
            sys_yield();
            continue;
        }
        dispatch_event(app, &ev);
        if (app->want_redraw) redraw_all(app);
    }
}

void tg_puts(const char *s) {
    sys_write(1, s, tg_strlen(s));
}
