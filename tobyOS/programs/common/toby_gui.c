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
    __asm__ volatile ("syscall"
        : : "a"((long)SYS_YIELD)
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

#define TG_COL_BG          0x00202830u  /* window background */
#define TG_COL_LABEL_FG    0x00E0E0E0u
#define TG_COL_BTN_FACE    0x00404858u
#define TG_COL_BTN_FACE_H  0x00505868u  /* hovered/pressed lighter */
#define TG_COL_BTN_BORDER  0x00808890u
#define TG_COL_BTN_TEXT    0x00FFFFFFu
#define TG_COL_INPUT_BG    0x00101418u
#define TG_COL_INPUT_FG    0x00E8F0F8u
#define TG_COL_INPUT_BORDER 0x00606870u
#define TG_COL_FOCUS_RING  0x00FFD060u  /* yellow-ish focus highlight */

/* ---- forward decls ------------------------------------------------ */

static int  point_in(struct tg_widget *w, int x, int y);
static void redraw_all(struct tg_app *app);
static void draw_widget(struct tg_app *app, int idx);
static void dispatch_event(struct tg_app *app, const struct tg_event *ev);

/* ---- widget creation --------------------------------------------- */

static struct tg_widget *alloc_widget(struct tg_app *app) {
    if (app->n_widgets >= TG_MAX_WIDGETS) return 0;
    struct tg_widget *w = &app->widgets[app->n_widgets++];
    /* zero-init */
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

/* ---- hit-testing ------------------------------------------------- */

static int point_in(struct tg_widget *w, int x, int y) {
    return x >= w->x && x < w->x + w->w &&
           y >= w->y && y < w->y + w->h;
}

static int hit_test(struct tg_app *app, int x, int y) {
    /* Topmost = last-added wins -- mirrors how DOM-ish trees usually
     * work. (No widget overlap in our demo, so this barely matters.) */
    for (int i = app->n_widgets - 1; i >= 0; i--) {
        if (point_in(&app->widgets[i], x, y)) return i;
    }
    return -1;
}

/* ---- drawing ----------------------------------------------------- */

static void draw_rect_border(struct tg_app *app, int x, int y, int w, int h,
                             tg_uint32_t color) {
    /* 1-px border drawn as 4 thin fills. */
    sys_gui_fill(app->fd, x,         y,         w, 1, color);
    sys_gui_fill(app->fd, x,         y + h - 1, w, 1, color);
    sys_gui_fill(app->fd, x,         y,         1, h, color);
    sys_gui_fill(app->fd, x + w - 1, y,         1, h, color);
}

/* Vertical-centre baseline for an 8-px font inside an h-pixel widget. */
static int text_baseline_y(int widget_y, int widget_h) {
    int by = widget_y + (widget_h - 8) / 2;
    if (by < widget_y) by = widget_y;
    return by;
}

/* Horizontally centre an `n`-char string (each char = 8 px wide). */
static int text_centred_x(int widget_x, int widget_w, int n_chars) {
    int tw = n_chars * 8;
    int bx = widget_x + (widget_w - tw) / 2;
    if (bx < widget_x) bx = widget_x;
    return bx;
}

static void draw_label(struct tg_app *app, struct tg_widget *w) {
    /* Labels paint their text with the window bg behind them so they
     * don't leave stale pixels on text changes (sys_gui_text uses a
     * solid bg per-glyph when `bg != GFX_TRANSPARENT`). */
    sys_gui_fill(app->fd, w->x, w->y, w->w, w->h, TG_COL_BG);
    int by = text_baseline_y(w->y, w->h);
    sys_gui_text(app->fd, w->x, by, w->text, TG_COL_LABEL_FG, TG_COL_BG);
}

static void draw_button(struct tg_app *app, struct tg_widget *w, int focused) {
    tg_uint32_t face = w->pressed ? TG_COL_BTN_FACE_H : TG_COL_BTN_FACE;
    sys_gui_fill(app->fd, w->x, w->y, w->w, w->h, face);
    draw_rect_border(app, w->x, w->y, w->w, w->h, TG_COL_BTN_BORDER);
    if (focused) {
        /* Inset 2 px so the focus ring is visible against the border. */
        draw_rect_border(app, w->x + 2, w->y + 2, w->w - 4, w->h - 4,
                         TG_COL_FOCUS_RING);
    }
    int n  = (int)tg_strlen(w->text);
    int tx = text_centred_x(w->x, w->w, n);
    int ty = text_baseline_y(w->y, w->h);
    sys_gui_text(app->fd, tx, ty, w->text, TG_COL_BTN_TEXT, face);
}

static void draw_textinput(struct tg_app *app, struct tg_widget *w, int focused) {
    sys_gui_fill(app->fd, w->x, w->y, w->w, w->h, TG_COL_INPUT_BG);
    draw_rect_border(app, w->x, w->y, w->w, w->h,
                     focused ? TG_COL_FOCUS_RING : TG_COL_INPUT_BORDER);
    /* 4-px text padding from the left edge. */
    int tx = w->x + 4;
    int ty = text_baseline_y(w->y, w->h);
    /* Crude horizontal "scroll": if the text is wider than the box,
     * show the rightmost slice that fits so the caret stays visible. */
    int max_chars = (w->w - 8) / 8;
    if (max_chars < 0) max_chars = 0;
    int n = (int)tg_strlen(w->text);
    int start = (n > max_chars) ? (n - max_chars) : 0;
    sys_gui_text(app->fd, tx, ty, w->text + start,
                 TG_COL_INPUT_FG, TG_COL_INPUT_BG);
    if (focused) {
        /* Caret = thin vertical bar after the last visible char. */
        int caret_x = tx + (n - start) * 8;
        if (caret_x > w->x + w->w - 2) caret_x = w->x + w->w - 2;
        sys_gui_fill(app->fd, caret_x, w->y + 3, 1, w->h - 6,
                     TG_COL_FOCUS_RING);
    }
}

static void draw_widget(struct tg_app *app, int idx) {
    struct tg_widget *w = &app->widgets[idx];
    int focused = (idx == app->focused);
    switch (w->kind) {
    case TG_WIDGET_LABEL:     draw_label(app, w);              break;
    case TG_WIDGET_BUTTON:    draw_button(app, w, focused);    break;
    case TG_WIDGET_TEXTINPUT: draw_textinput(app, w, focused); break;
    default: break;
    }
}

static void redraw_all(struct tg_app *app) {
    sys_gui_fill(app->fd, 0, 0, app->win_w, app->win_h, app->bg_color);
    for (int i = 0; i < app->n_widgets; i++) draw_widget(app, i);
    sys_gui_flip(app->fd);
    app->want_redraw = 0;
}

/* ---- per-widget event handling ----------------------------------- */

/* Append a printable ASCII char to a textinput's buffer, or apply
 * backspace. Returns 1 if the buffer changed (caller should redraw). */
static int textinput_consume_key(struct tg_widget *w, tg_uint8_t k) {
    if (k == TG_KEY_BACKSPACE) {
        tg_size_t n = tg_strlen(w->text);
        if (n == 0) return 0;
        w->text[n - 1] = '\0';
        return 1;
    }
    /* Printable ASCII only -- ignore everything else (Enter, Tab, ...). */
    if (k < 0x20 || k > 0x7E) return 0;
    tg_size_t n = tg_strlen(w->text);
    if (n + 1 >= TG_TEXT_MAX) return 0;
    w->text[n]     = (char)k;
    w->text[n + 1] = '\0';
    return 1;
}

static void dispatch_event(struct tg_app *app, const struct tg_event *ev) {
    if (ev->type == TG_EV_MOUSE_DOWN) {
        int hit = hit_test(app, ev->x, ev->y);
        /* Update focus on every click -- focusable widgets gain it,
         * a click on a label or empty area drops it. */
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
        return;
    }

    if (ev->type == TG_EV_MOUSE_UP) {
        int cap = app->captured;
        app->captured = -1;
        if (cap >= 0 && app->widgets[cap].kind == TG_WIDGET_BUTTON) {
            int was_pressed = app->widgets[cap].pressed;
            app->widgets[cap].pressed = 0;
            app->want_redraw = 1;
            /* Only fire the callback if the release happened inside
             * the button (classic UX -- drag-out cancels the click). */
            if (was_pressed && point_in(&app->widgets[cap], ev->x, ev->y)) {
                tg_button_cb cb = app->widgets[cap].on_click;
                if (cb) cb(&app->widgets[cap], app);
            }
        }
        return;
    }

    if (ev->type == TG_EV_MOUSE_MOVE) {
        /* Re-evaluate "still pressed" while dragging the mouse. If the
         * user drags off the button, it visually depresses. */
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
            if (textinput_consume_key(w, ev->key)) app->want_redraw = 1;
            return;
        }
        if (w->kind == TG_WIDGET_BUTTON) {
            /* Enter / Space activates a focused button, mirroring
             * standard desktop UX. */
            if (ev->key == TG_KEY_ENTER || ev->key == ' ') {
                if (w->on_click) w->on_click(w, app);
                app->want_redraw = 1;
            }
            return;
        }
    }
}

/* ---- public lifecycle -------------------------------------------- */

int tg_app_init(struct tg_app *app, int win_w, int win_h, const char *title) {
    /* Zero-init the whole struct. */
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
    /* Initial paint so the user sees the window even if no event ever
     * arrives. */
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

/* Convenience: write a NUL-terminated string to stdout. Useful for
 * apps that want to log diagnostics from inside callbacks. Exposed via
 * a non-namespaced symbol because user programs already have their own
 * "puts"/"putstr" conventions. */
void tg_puts(const char *s) {
    sys_write(1, s, tg_strlen(s));
}
