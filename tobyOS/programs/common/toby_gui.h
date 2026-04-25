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
 *   tagged union (TG_WIDGET_LABEL/BUTTON/TEXTINPUT). Each widget has
 *   a fixed bounding box, a text buffer, and (for buttons) a callback.
 *   Drawing + event handling switch on `kind` -- no vtables, easy to
 *   read and easy to extend.
 *
 * Limitations (deliberate -- this is an educational toolkit):
 *   - no layout engine; positions are absolute
 *   - no styling/theming; one set of colours, baked in
 *   - no clipboard, no selection, no scrolling
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

#define TG_KEY_BACKSPACE    0x08
#define TG_KEY_TAB          0x09
#define TG_KEY_ENTER        0x0A

#define TG_MAX_WIDGETS  16
#define TG_TEXT_MAX     64
#define TG_TITLE_MAX    32

enum tg_widget_kind {
    TG_WIDGET_LABEL,
    TG_WIDGET_BUTTON,
    TG_WIDGET_TEXTINPUT,
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
    int           focusable;        /* 1 for button/textinput, 0 for label */

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

/* Manipulate a widget's text. tg_set_text marks the app for redraw. */
void        tg_set_text(struct tg_app *app, struct tg_widget *w, const char *text);
const char *tg_get_text(struct tg_widget *w);

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

#endif /* TOBY_GUI_H */
