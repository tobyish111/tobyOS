/* toby_gui.h -- minimal user-space GUI toolkit for tobyOS apps.
 *
 * Built on top of the milestone-10 GUI syscalls (sys_gui_create,
 * sys_gui_fill, sys_gui_text, sys_gui_flip, sys_gui_poll_event) and
 * the milestone-11 keyboard event delivery (kernel routes keystrokes
 * to the topmost window via GUI_EV_KEY).
 *
 * Programming model
 * -----------------
 *   1. tg_app_init(&app, w, h, title)        opens a window
 *   2. tg_label/tg_button/tg_textinput(...)  add widgets at fixed (x,y,w,h)
 *   3. tg_run(&app)                          main loop: poll, dispatch, redraw
 *
 * Event dispatch
 * --------------
 *   - mouse-down inside a widget       -> that widget gets focus + the event
 *   - mouse-down outside any widget    -> focus cleared
 *   - mouse-move/up while a widget is "captured" (was just pressed) ->
 *     the captured widget gets the event (used by Button to tell
 *     "release inside me" from "release outside me" -> the click)
 *   - key event                        -> focused widget only
 *
 * Widget model
 * ------------
 *   tagged union (TG_WIDGET_LABEL/BUTTON/TEXTINPUT/CHECKBOX/PROGRESS/
 *   SEPARATOR/LISTVIEW). Each widget has a fixed bounding box, a text
 *   buffer, and type-specific fields. Drawing + event handling switch
 *   on `kind` -- no vtables, easy to read and easy to extend.
 *
 * Limitations (deliberate -- this is an educational toolkit):
 *   - no layout engine; positions are absolute
 *   - no styling/theming; one set of colours, baked in
 *   - no clipboard, no selection
 *   - one window per tg_app
 *   - text inputs hold up to TG_TEXT_MAX-1 chars
 */

#ifndef TOBY_GUI_H
#define TOBY_GUI_H

typedef unsigned long      tg_size_t;
typedef long               tg_ssize_t;
typedef unsigned int       tg_uint32_t;
typedef int                tg_int32_t;
typedef unsigned short     tg_uint16_t;
typedef unsigned char      tg_uint8_t;

/* Mirrors the kernel's struct gui_event (must stay byte-compatible). */
struct tg_event {
    int        type;
    int        x;
    int        y;
    tg_uint8_t button;
    tg_uint8_t key;
    tg_uint8_t _pad[2];
};

/* Event types -- keep in sync with include/tobyos/gui.h. */
#define TG_EV_NONE          0
#define TG_EV_MOUSE_MOVE    1
#define TG_EV_MOUSE_DOWN    2
#define TG_EV_MOUSE_UP      3
#define TG_EV_KEY           4
#define TG_EV_CLOSE         5

#define TG_KEY_BACKSPACE    0x08
#define TG_KEY_TAB          0x09
#define TG_KEY_ENTER        0x0A
#define TG_KEY_UP           0x80
#define TG_KEY_DOWN         0x81

#define TG_MAX_WIDGETS  32
#define TG_TEXT_MAX     64
#define TG_TITLE_MAX    32

enum tg_widget_kind {
    TG_WIDGET_LABEL,
    TG_WIDGET_BUTTON,
    TG_WIDGET_TEXTINPUT,
    TG_WIDGET_CHECKBOX    = 4,
    TG_WIDGET_PROGRESS    = 5,
    TG_WIDGET_SEPARATOR   = 6,
    TG_WIDGET_LISTVIEW    = 7,
};

struct tg_app;
struct tg_widget;

typedef void (*tg_button_cb)(struct tg_widget *btn, struct tg_app *app);

struct tg_widget {
    int           kind;
    int           x, y, w, h;
    char          text[TG_TEXT_MAX];

    /* button-only */
    int           pressed;          /* 1 while mouse button held inside */
    tg_button_cb  on_click;

    /* textinput-only: cursor index = strlen(text) (we only edit at end) */
    int           focusable;        /* 1 for button/textinput/checkbox/listview */

    /* checkbox */
    int           checked;

    /* progress bar: 0-100 */
    tg_uint8_t    progress;

    /* list view */
    int           list_count;
    int           list_selected;
    int           list_scroll;
    const char  **list_items;       /* borrowed pointer to app's string array */

    /* Caller-owned -- toolkit never touches it. */
    void         *user;
};

struct tg_app {
    int               fd;                       /* window fd from sys_gui_create */
    int               win_w, win_h;
    tg_uint32_t       bg_color;
    struct tg_widget  widgets[TG_MAX_WIDGETS];
    int               n_widgets;
    int               focused;                  /* index, -1 if none */
    int               captured;                 /* index of widget currently
                                                 * holding the mouse, -1 if none */
    int               want_redraw;
    int               want_quit;                /* set by tg_app_quit() */
};

/* ---- API ---------------------------------------------------------- */

/* Open a window; returns 0 on success, -1 on failure (no GUI? OOM?). */
int tg_app_init(struct tg_app *app, int win_w, int win_h, const char *title);

/* Add widgets. All return a pointer into app->widgets[] (NULL if the
 * table is full). Caller may stash the pointer to update text / read
 * input later. (x,y,w,h) are in client-area pixels. */
struct tg_widget *tg_label(struct tg_app *app, int x, int y, int w, int h,
                           const char *text);
struct tg_widget *tg_button(struct tg_app *app, int x, int y, int w, int h,
                            const char *text, tg_button_cb cb);
struct tg_widget *tg_textinput(struct tg_app *app, int x, int y, int w, int h);

/* New widget creation functions. Each returns widget index or -1 on failure. */
int tg_checkbox(struct tg_app *app, int x, int y, int w, int h,
                const char *label, int initial);
int tg_progress(struct tg_app *app, int x, int y, int w, int h,
                tg_uint8_t value);
int tg_separator(struct tg_app *app, int x, int y, int w);
int tg_listview(struct tg_app *app, int x, int y, int w, int h,
                const char **items, int count);

/* Manipulate a widget's text. tg_set_text marks the app for redraw. */
void        tg_set_text(struct tg_app *app, struct tg_widget *w, const char *text);
const char *tg_get_text(struct tg_widget *w);

/* Checkbox accessors. */
void tg_checkbox_set(struct tg_app *app, int idx, int checked);
int  tg_checkbox_get(struct tg_app *app, int idx);

/* Progress bar accessor. */
void tg_progress_set(struct tg_app *app, int idx, tg_uint8_t value);

/* List view accessors. */
void tg_listview_set_selected(struct tg_app *app, int idx, int sel);
int  tg_listview_get_selected(struct tg_app *app, int idx);

/* Force a full redraw next loop iteration. */
void tg_request_redraw(struct tg_app *app);

/* Stop the main loop. The window is NOT closed by the toolkit -- when
 * main() returns, start.S calls SYS_EXIT, which the kernel handles by
 * tearing down the proc and closing all its fds (which closes the
 * window). */
void tg_app_quit(struct tg_app *app);

/* Run forever (or until tg_app_quit). Returns 0 on a quit, -1 on a
 * fatal error (poll syscall failed). */
int tg_run(struct tg_app *app);

/* Convenience: write a NUL-terminated string to stdout. Useful for
 * apps that want to log diagnostics outside the GUI. */
void tg_puts(const char *s);

/* ---- Advanced drawing API (Phase 1) --------------------------------
 *
 * Direct access to the new drawing primitives exposed by syscalls 80-92.
 * These bypass the widget system and draw directly into the window
 * backbuffer. Call tg_flip() after drawing to present. */

/* Syscall numbers for advanced drawing. */
#define TG_SYS_GUI_LINE              80
#define TG_SYS_GUI_RECT              81
#define TG_SYS_GUI_ROUNDED_RECT      82
#define TG_SYS_GUI_ROUNDED_RECT_BLEND 83
#define TG_SYS_GUI_CIRCLE            84
#define TG_SYS_GUI_CIRCLE_OUTLINE    85
#define TG_SYS_GUI_BLIT              86
#define TG_SYS_GUI_BLIT_BLEND        87
#define TG_SYS_GUI_GETPIXELS         88
#define TG_SYS_GUI_GRADIENT          89
#define TG_SYS_GUI_LINE_BLEND        90
#define TG_SYS_GUI_SET_OPACITY       91

/* Raw drawing functions (operate on the app's window fd). */
static inline long tg_syscall6(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int tg_draw_line(int fd, int x0, int y0, int x1, int y1, tg_uint32_t color) {
    long xy0 = (long)(((unsigned)y0 << 16) | ((unsigned)x0 & 0xFFFF));
    long xy1 = (long)(((unsigned)y1 << 16) | ((unsigned)x1 & 0xFFFF));
    return (int)tg_syscall6(TG_SYS_GUI_LINE, fd, xy0, xy1, (long)color, 0);
}

static inline int tg_draw_line_blend(int fd, int x0, int y0, int x1, int y1, tg_uint32_t argb) {
    long xy0 = (long)(((unsigned)y0 << 16) | ((unsigned)x0 & 0xFFFF));
    long xy1 = (long)(((unsigned)y1 << 16) | ((unsigned)x1 & 0xFFFF));
    return (int)tg_syscall6(TG_SYS_GUI_LINE_BLEND, fd, xy0, xy1, (long)argb, 0);
}

static inline int tg_draw_circle(int fd, int cx, int cy, int r, tg_uint32_t color) {
    long cxy = (long)(((unsigned)cy << 16) | ((unsigned)cx & 0xFFFF));
    return (int)tg_syscall6(TG_SYS_GUI_CIRCLE, fd, cxy, (long)r, (long)color, 0);
}

static inline int tg_draw_circle_outline(int fd, int cx, int cy, int r, tg_uint32_t color) {
    long cxy = (long)(((unsigned)cy << 16) | ((unsigned)cx & 0xFFFF));
    return (int)tg_syscall6(TG_SYS_GUI_CIRCLE_OUTLINE, fd, cxy, (long)r, (long)color, 0);
}

static inline int tg_draw_rounded_rect(int fd, int x, int y, int w, int h, int radius, tg_uint32_t color) {
    long wh = (long)(((unsigned)h << 16) | ((unsigned)w & 0xFFFF));
    long rc = (long)(((unsigned)radius << 24) | (color & 0x00FFFFFFu));
    return (int)tg_syscall6(TG_SYS_GUI_ROUNDED_RECT, fd, (long)((unsigned)x | ((unsigned)y << 16)), wh, 0, rc);
}

static inline int tg_draw_gradient(int fd, int x, int y, int w, int h, tg_uint32_t top, tg_uint32_t bot) {
    long xy = (long)(((unsigned)y << 16) | ((unsigned)x & 0xFFFF));
    long wh = (long)(((unsigned)h << 16) | ((unsigned)w & 0xFFFF));
    return (int)tg_syscall6(TG_SYS_GUI_GRADIENT, fd, xy, wh, (long)top, (long)bot);
}

static inline int tg_blit(int fd, int x, int y, int w, int h, const tg_uint32_t *pixels) {
    long dxy = (long)(((unsigned)y << 16) | ((unsigned)x & 0xFFFF));
    long wh  = (long)(((unsigned)h << 16) | ((unsigned)w & 0xFFFF));
    return (int)tg_syscall6(TG_SYS_GUI_BLIT, fd, dxy, (long)pixels, wh, 0);
}

static inline int tg_blit_blend(int fd, int x, int y, int w, int h, const tg_uint32_t *pixels) {
    long dxy = (long)(((unsigned)y << 16) | ((unsigned)x & 0xFFFF));
    long wh  = (long)(((unsigned)h << 16) | ((unsigned)w & 0xFFFF));
    return (int)tg_syscall6(TG_SYS_GUI_BLIT_BLEND, fd, dxy, (long)pixels, wh, 0);
}

static inline int tg_getpixels(int fd, int x, int y, int w, int h, tg_uint32_t *dst) {
    long xy = (long)(((unsigned)y << 16) | ((unsigned)x & 0xFFFF));
    long wh = (long)(((unsigned)h << 16) | ((unsigned)w & 0xFFFF));
    return (int)tg_syscall6(TG_SYS_GUI_GETPIXELS, fd, xy, (long)dst, wh, 0);
}

static inline int tg_set_opacity(int fd, tg_uint8_t alpha) {
    return (int)tg_syscall6(TG_SYS_GUI_SET_OPACITY, fd, (long)alpha, 0, 0, 0);
}

/* Flip the window after drawing (same as before). */
static inline int tg_flip(int fd) {
    return (int)tg_syscall6(13, (long)fd, 0, 0, 0, 0);
}

#endif /* TOBY_GUI_H */
