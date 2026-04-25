/* toby/gui.h -- tobyOS SDK 1.0 GUI toolkit (frozen contract).
 *
 * The same toolkit shipped in-tree as programs/common/toby_gui.h, but
 * re-homed under <toby/gui.h> so SDK consumers have a stable include
 * path that doesn't leak the in-tree program directory layout. The
 * `tg_*` symbol prefix is preserved so source compatibility with
 * existing in-tree apps is exact -- you can copy main.c from
 * programs/user_gui_demo/ into an out-of-tree project and only
 * change the #include line.
 *
 * Built on top of the milestone-10 GUI syscalls (sys_gui_create,
 * sys_gui_fill, sys_gui_text, sys_gui_flip, sys_gui_poll_event) plus
 * milestone-11 keyboard delivery (kernel routes keystrokes to the
 * topmost window via GUI_EV_KEY). Implementation lives in
 * libtoby_gui.a (shipped under sdk/lib/), so an SDK-built app links
 * with: crt0.o + main.o + libtoby_gui.a + libtoby.a.
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
 *   - mouse-move/up while a widget is "captured" -> captured widget gets it
 *     (used by Button to tell "release inside me" from "release outside me")
 *   - key event                        -> focused widget only
 *
 * Widget model
 * ------------
 *   tagged union (TG_WIDGET_LABEL/BUTTON/TEXTINPUT). Each widget has a
 *   fixed bounding box, a text buffer, and (for buttons) a callback.
 *
 * Limitations (deliberate -- this is an educational toolkit):
 *   - no layout engine; positions are absolute
 *   - one set of colours (theme-aware via the kernel compositor)
 *   - no clipboard, no selection, no scrolling
 *   - one window per tg_app
 *   - text inputs hold up to TG_TEXT_MAX-1 chars
 */

#ifndef TOBY_GUI_H
#define TOBY_GUI_H

#ifdef __cplusplus
extern "C" {
#endif

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

    int           pressed;
    tg_button_cb  on_click;

    int           focusable;
    void         *user;
};

struct tg_app {
    int               fd;
    int               win_w, win_h;
    tg_uint32_t       bg_color;
    struct tg_widget  widgets[TG_MAX_WIDGETS];
    int               n_widgets;
    int               focused;
    int               captured;
    int               want_redraw;
    int               want_quit;
};

/* ---- API ---------------------------------------------------------- */

int tg_app_init(struct tg_app *app, int win_w, int win_h, const char *title);

struct tg_widget *tg_label(struct tg_app *app, int x, int y, int w, int h,
                           const char *text);
struct tg_widget *tg_button(struct tg_app *app, int x, int y, int w, int h,
                            const char *text, tg_button_cb cb);
struct tg_widget *tg_textinput(struct tg_app *app, int x, int y, int w, int h);

void        tg_set_text(struct tg_app *app, struct tg_widget *w, const char *text);
const char *tg_get_text(struct tg_widget *w);

void tg_request_redraw(struct tg_app *app);
void tg_app_quit(struct tg_app *app);
int  tg_run(struct tg_app *app);

void tg_puts(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_GUI_H */
