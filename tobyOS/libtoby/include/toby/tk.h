/* toby/tk.h -- TobyTK: the canonical native widget toolkit for tobyOS.
 *
 * A small retained-mode toolkit for native tobyOS GUI apps. It supersedes the
 * two earlier half-finished attempts: programs/common/toby_gui.c (worked, but
 * absolute-positioned, 8x8-bitmap text, and lived outside libtoby) and
 * libtoby/src/ui.c "TobyUI" (had a layout engine but its text rendering was a
 * no-op stub and it drew into a disconnected local buffer).
 *
 * Model
 * -----
 *   - One window (tk_window) wraps a compositor window fd + a retained widget
 *     tree + a self-pacing event loop.
 *   - Widgets live in a fixed pool inside tk_window (no malloc -> no OOM edge),
 *     but are tree-structured so the layout engine can recurse.
 *   - Layout: vbox / hbox containers with padding, gap, flex-grow and fixed /
 *     shrink-to-fit sizing. The root is a vbox filling the window.
 *   - Drawing goes through the existing gui_* syscall ABI (the proven low-level
 *     layer), and text uses the kernel TrueType rasterizer (kfont.c) via the
 *     GUI_TEXT_TTF syscalls -- real antialiased glyphs, not the 8x8 bitmap.
 *   - Theming via a userspace palette (default: a dark Plasma-like look).
 *
 * Because tk_window embeds the whole widget pool, declare it `static` (or heap
 * allocate it) -- it is a few tens of KB, too large for a typical stack frame.
 *
 * Usage sketch
 * ------------
 *   static struct tk_window win;
 *   tk_window_open(&win, 720, 480, "My App");
 *   struct tk_widget *col = tk_vbox(&win, NULL, 8);   // NULL parent = root
 *   tk_pad(col, 12);
 *   tk_label(&win, col, "Hello");
 *   tk_button(&win, col, "Click me", on_click);
 *   tk_run(&win);
 */

#ifndef TOBY_TK_H
#define TOBY_TK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- event types ---------------------------------------------------- *
 * MUST equal the kernel GUI_EV_* (include/tobyos/gui.h). History footgun:
 * a stale local enum once defined CLOSE=1, colliding with MOUSE_MOVE=1, so
 * every hover read as a window-close. Do NOT re-derive these numbers. */
#define TK_EV_NONE        0
#define TK_EV_MOUSE_MOVE  1
#define TK_EV_MOUSE_DOWN  2
#define TK_EV_MOUSE_UP    3
#define TK_EV_KEY         4
#define TK_EV_CLOSE       5
#define TK_EV_RESIZE      6

#define TK_KEY_BACKSPACE  0x08
#define TK_KEY_TAB        0x09
#define TK_KEY_ENTER      0x0A
#define TK_KEY_UP         0x80
#define TK_KEY_DOWN       0x81

/* Byte-compatible with the kernel struct gui_event. */
struct tk_event {
    int     type;
    int     x, y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};

/* Window state for tk_maximize (matches GUI_WIN_*). */
#define TK_WIN_NORMAL     0
#define TK_WIN_MINIMIZED  1
#define TK_WIN_MAXIMIZED  2

/* ---- limits --------------------------------------------------------- */
#define TK_MAX_WIDGETS   64
#define TK_MAX_CHILDREN  16
#define TK_TEXT_MAX      96

/* ---- widget kinds --------------------------------------------------- */
enum tk_kind {
    TK_PANEL = 0,    /* generic container (vbox/hbox/none) */
    TK_LABEL,
    TK_BUTTON,
    TK_FIELD,        /* single-line text field */
    TK_CHECKBOX,
    TK_LISTBOX,      /* scrollable, single-select */
    TK_SLIDER,
    TK_PROGRESS,
    TK_SEPARATOR,
};

enum tk_layout { TK_LAYOUT_NONE = 0, TK_LAYOUT_VBOX, TK_LAYOUT_HBOX };
enum tk_align  { TK_ALIGN_LEFT = 0, TK_ALIGN_CENTER, TK_ALIGN_RIGHT };

struct tk_window;
struct tk_widget;
typedef void (*tk_cb)(struct tk_window *win, struct tk_widget *w);

struct tk_widget {
    int  kind;
    struct tk_widget *parent;
    struct tk_widget *child[TK_MAX_CHILDREN];
    int  n_child;

    /* layout inputs */
    int  layout;             /* container only: TK_LAYOUT_* */
    int  gap;                /* container only: space between children */
    int  pad;                /* container inner padding (all sides) */
    int  fixed_w, fixed_h;   /* 0 = auto (shrink-to-fit or flex) */
    int  flex;               /* flex-grow weight along parent's main axis */
    int  align;              /* text alignment (TK_ALIGN_*) */
    int  font_px;            /* 0 = inherit window default */
    int  bold;               /* 1 = use the bold TTF face */

    /* computed geometry (absolute window-client coords; set by layout) */
    int  x, y, w, h;

    /* visuals (0 = use the active theme's default for this kind) */
    uint32_t bg, fg;

    char text[TK_TEXT_MAX];

    /* state */
    int  visible, enabled, focusable;
    int  pressed, hovered, focused;
    int  checked;
    int  value, value_min, value_max;

    /* listbox */
    const char **items;
    int  n_items, sel, scroll;

    /* callbacks */
    tk_cb on_click;          /* button click / checkbox toggle / list select /
                              * field Enter */
    tk_cb on_change;         /* slider drag / list selection change */
    void *user;              /* app-owned; the toolkit never touches it */
};

/* Userspace theme palette (0x00RRGGBB). Defaults to a dark Plasma-like look. */
struct tk_theme {
    uint32_t window_bg;
    uint32_t panel_bg;
    uint32_t text;
    uint32_t text_dim;
    uint32_t accent;
    uint32_t border;
    uint32_t btn_bg, btn_bg_hover, btn_bg_active, btn_text;
    uint32_t field_bg, field_border, field_text;
    uint32_t focus_ring;
    uint32_t list_bg, list_sel, list_text;
    uint32_t check_mark;
    uint32_t track, track_fill;
};

struct tk_window {
    int  fd;
    int  w, h;
    int  default_font_px;
    struct tk_theme theme;

    struct tk_widget pool[TK_MAX_WIDGETS];
    int  n;
    struct tk_widget *root;
    struct tk_widget *focus;
    struct tk_widget *capture;   /* widget holding the mouse during a drag */

    int  want_redraw;
    int  want_quit;
    void *user;                  /* app-owned */
};

/* ---- lifecycle ------------------------------------------------------ */
/* Open a window of client size w x h. Returns 0 on success, -1 on failure
 * (no GUI / out of windows). Initialises the default theme + a vbox root. */
int  tk_window_open(struct tk_window *win, int w, int h, const char *title);
void tk_theme_default(struct tk_theme *t);
struct tk_widget *tk_root(struct tk_window *win);
void tk_maximize(struct tk_window *win);   /* fill the screen */

/* ---- widget construction ------------------------------------------- *
 * `parent` NULL means "attach to root". All return a pointer into the
 * window's pool (NULL if the pool is full). */
struct tk_widget *tk_panel   (struct tk_window*, struct tk_widget *parent);
struct tk_widget *tk_vbox    (struct tk_window*, struct tk_widget *parent, int gap);
struct tk_widget *tk_hbox    (struct tk_window*, struct tk_widget *parent, int gap);
struct tk_widget *tk_label   (struct tk_window*, struct tk_widget *parent, const char *text);
struct tk_widget *tk_button  (struct tk_window*, struct tk_widget *parent, const char *text, tk_cb on_click);
struct tk_widget *tk_field   (struct tk_window*, struct tk_widget *parent, const char *initial);
struct tk_widget *tk_checkbox(struct tk_window*, struct tk_widget *parent, const char *label, int checked);
struct tk_widget *tk_listbox (struct tk_window*, struct tk_widget *parent, const char **items, int n);
struct tk_widget *tk_slider  (struct tk_window*, struct tk_widget *parent, int mn, int mx, int v);
struct tk_widget *tk_progress(struct tk_window*, struct tk_widget *parent, int v, int mx);
struct tk_widget *tk_separator(struct tk_window*, struct tk_widget *parent);

/* ---- widget configuration (return the widget for convenience) ------- */
struct tk_widget *tk_size  (struct tk_widget*, int w, int h);   /* fixed size (0=auto) */
struct tk_widget *tk_grow  (struct tk_widget*, int flex);       /* flex-grow weight */
struct tk_widget *tk_pad   (struct tk_widget*, int pad);
struct tk_widget *tk_align (struct tk_widget*, int align);
struct tk_widget *tk_colors(struct tk_widget*, uint32_t bg, uint32_t fg);
struct tk_widget *tk_font  (struct tk_widget*, int px);
struct tk_widget *tk_bold  (struct tk_widget*);

/* ---- runtime accessors --------------------------------------------- */
void  tk_set_text  (struct tk_window*, struct tk_widget*, const char *text);
const char *tk_get_text(struct tk_widget*);
int   tk_checked   (struct tk_widget*);
void  tk_set_checked(struct tk_window*, struct tk_widget*, int on);
int   tk_selected  (struct tk_widget*);                 /* listbox sel index */
void  tk_set_value (struct tk_window*, struct tk_widget*, int v); /* slider/progress */

/* ---- dynamic rebuild (e.g. swapping a content panel between pages) -- *
 * Pattern:
 *   int base = tk_checkpoint(win);    // after building the static chrome
 *   ...
 *   tk_clear_children(win, content);  // detach the old page's widgets
 *   tk_rewind(win, base);             // free their pool slots
 *   ... build the new page into `content` ...  // re-allocates from `base`
 *   tk_redraw(win);
 * Only widgets allocated AFTER the checkpoint may be rewound; the container
 * being refilled must itself pre-date the checkpoint. */
int  tk_checkpoint(struct tk_window *win);
void tk_rewind(struct tk_window *win, int checkpoint);
void tk_clear_children(struct tk_window *win, struct tk_widget *w);

/* ---- loop ----------------------------------------------------------- */
void  tk_redraw(struct tk_window*);   /* request a repaint next iteration */
void  tk_quit  (struct tk_window*);
int   tk_run   (struct tk_window*);   /* blocking loop until quit; returns 0 */
/* Non-blocking single pump: drain pending events + repaint if dirty.
 * Returns 1 if a quit was requested, else 0. For apps (or the demo harness)
 * that drive their own outer loop. */
int   tk_pump  (struct tk_window*);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_TK_H */
