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

/* ---- limits --------------------------------------------------------- *
 * The whole widget pool is embedded by value in struct tk_window, so these
 * set its size (~256*~600B ≈ 150 KB). Declare tk_window `static` or heap-
 * allocate it -- it is too large for a stack frame. Bumped from 64/16 so
 * heavier apps (file manager, task manager, settings pages) don't exhaust
 * the pool; list/table widgets still source their rows from app-owned
 * arrays, so big data sets do NOT consume one slot per row. */
#define TK_MAX_WIDGETS   256
#define TK_MAX_CHILDREN  32
#define TK_TEXT_MAX      96
/* Per-widget dirty tracking: max widgets that can be partial-repainted in
 * one pass before falling back to a whole-window repaint. */
#define TK_MAX_DIRTY     8

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
    TK_CANVAS,       /* app-driven custom drawing surface (escape hatch) */
    TK_TABLE,        /* columns + scrollable, single-select rows (app data) */
    TK_TEXTAREA,     /* read-only scrollable multiline text (app-owned) */
};

enum tk_layout { TK_LAYOUT_NONE = 0, TK_LAYOUT_VBOX, TK_LAYOUT_HBOX };
enum tk_align  { TK_ALIGN_LEFT = 0, TK_ALIGN_CENTER, TK_ALIGN_RIGHT };

struct tk_window;
struct tk_widget;
typedef void (*tk_cb)(struct tk_window *win, struct tk_widget *w);
/* Raw-event callback, used by TK_CANVAS to receive mouse/key events the
 * toolkit does not interpret. `ev` coords are absolute window-client
 * coords (subtract the widget origin from tk_geom for canvas-local). */
typedef void (*tk_event_cb)(struct tk_window *win, struct tk_widget *w,
                            struct tk_event *ev);
/* Window-level key hook (accelerators / app-driven keyboards). When set
 * (tk_on_key), it receives EVERY key and REPLACES the default focus-based
 * key routing -- the app owns the keyboard. Leave unset for normal
 * field/checkbox/list keyboard behaviour. */
typedef void (*tk_key_cb)(struct tk_window *win, struct tk_event *ev);

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

    /* table (TK_TABLE): headers + per-cell accessor are app-owned */
    const char *const *th;   /* column headers, ncols entries (may be NULL) */
    const int *col_w;        /* column pixel widths, ncols entries */
    int  ncols, nrows;
    const char *(*cell)(void *user, int row, int col);
    void *cell_user;

    /* textarea (TK_TEXTAREA): app-owned text, rendered line-by-line */
    const char *ta_text;

    /* callbacks */
    tk_cb on_click;          /* button click / checkbox toggle / list select /
                              * field Enter */
    tk_cb on_change;         /* slider drag / list selection change */
    tk_cb on_paint;          /* TK_CANVAS: custom repaint hook */
    tk_event_cb on_event;    /* TK_CANVAS: raw mouse/key events */
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
    /* Per-widget dirty tracking: widgets changed since the last repaint via
     * the live-update setters. The next repaint touches only their rects
     * (flipping each sub-rect). Any event-driven change, structural rebuild,
     * or overflow sets dirty_full => a whole-window repaint (safe default). */
    struct tk_widget *dirty[TK_MAX_DIRTY];
    int  n_dirty;
    int  dirty_full;
    tk_key_cb on_key;            /* optional global key hook (see tk_on_key) */
    void *user;                  /* app-owned */
};

/* ---- lifecycle ------------------------------------------------------ */
/* Open a window of client size w x h. Returns 0 on success, -1 on failure
 * (no GUI / out of windows). Initialises the default theme + a vbox root. */
int  tk_window_open(struct tk_window *win, int w, int h, const char *title);
void tk_theme_default(struct tk_theme *t);
struct tk_widget *tk_root(struct tk_window *win);
void tk_maximize(struct tk_window *win);   /* fill the screen */
void tk_on_key(struct tk_window *win, tk_key_cb on_key);  /* global key hook */

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

/* ---- canvas: app-driven custom drawing (the escape hatch) ----------- *
 * A TK_CANVAS occupies a layout cell like any widget; size it with
 * tk_size()/tk_grow(). On every repaint the toolkit fills its background
 * (w->bg, or the window bg) and then calls on_paint(win, canvas). Inside
 * on_paint, draw with the tk_draw_* primitives below using the widget's
 * geometry (tk_geom) as the origin/bounds. Drawing is NOT clipped to the
 * widget rect -- keep your output within the reported bounds. Attach an
 * on_event callback (tk_on_event) to receive mouse/keys: the canvas takes
 * focus + mouse capture on click, so drags and keystrokes flow to it. */
struct tk_widget *tk_canvas(struct tk_window*, struct tk_widget *parent, tk_cb on_paint);
struct tk_widget *tk_on_event(struct tk_widget*, tk_event_cb on_event);

/* ---- table: columns + scrollable selectable rows -------------------- *
 * Headers/widths are app-owned arrays of `ncols` entries (headers may be
 * NULL for an unheaded table). Row content is pulled lazily through the
 * cell accessor, so large/changing data sets cost no widget slots and no
 * copies -- ideal for a process list or file listing. Set/refresh the row
 * count + accessor with tk_table_rows(); on_click/on_change fire on row
 * selection (read it with tk_table_selected). */
struct tk_widget *tk_table(struct tk_window*, struct tk_widget *parent,
                           const char *const *headers, const int *col_w, int ncols);
void tk_table_rows(struct tk_window*, struct tk_widget*, int nrows,
                   const char *(*cell)(void *user, int row, int col), void *user);
int  tk_table_selected(struct tk_widget*);

/* ---- textarea: read-only scrollable multiline text ------------------ *
 * `text` is app-owned (NUL-terminated, '\n'-separated lines); update it
 * live with tk_textarea_set. Scrolls with Up/Down/PageUp/PageDown when
 * focused. For an EDITABLE text editor, use a TK_CANVAS and own the cursor
 * logic in the app -- this widget is intentionally view-only. */
struct tk_widget *tk_textarea(struct tk_window*, struct tk_widget *parent, const char *text);
void tk_textarea_set(struct tk_window*, struct tk_widget*, const char *text);

/* Geometry of a laid-out widget (absolute window-client coords). Valid
 * after the first repaint -- e.g. inside on_paint / on_event. */
void tk_geom(struct tk_widget*, int *x, int *y, int *w, int *h);

/* ---- low-level draw primitives (window-client coords) --------------- *
 * Call these only while a repaint is in flight (typically from a canvas
 * on_paint). They emit the native gui_* draw ops to the window backbuffer;
 * the toolkit flips after on_paint returns. `bold` selects the bold TTF
 * face; `px` is the pixel height. tk_text_width measures with the same. */
void tk_draw_fill   (struct tk_window*, int x, int y, int w, int h, uint32_t color);
void tk_draw_rect   (struct tk_window*, int x, int y, int w, int h, uint32_t color); /* outline */
void tk_draw_rrect  (struct tk_window*, int x, int y, int w, int h, int radius, uint32_t color);
void tk_draw_line   (struct tk_window*, int x0, int y0, int x1, int y1, uint32_t color);
void tk_draw_circle (struct tk_window*, int cx, int cy, int r, uint32_t color);
void tk_draw_circle_outline(struct tk_window*, int cx, int cy, int r, uint32_t color);
void tk_draw_gradient(struct tk_window*, int x, int y, int w, int h, uint32_t top, uint32_t bot);
void tk_draw_text   (struct tk_window*, int x, int y, const char *s, uint32_t fg, int px, int bold);
int  tk_text_width  (const char *s, int px, int bold);
/* Monospace bitmap text: an 8x16 fixed-cell VGA font with an opaque bg, one
 * call per run. For char grids (terminals) where the proportional TTF
 * tk_draw_text would not column-align. Advance is 8px/char; position at col*8. */
void tk_draw_text_mono(struct tk_window*, int x, int y, const char *s, uint32_t fg, uint32_t bg);
/* Blit an ARGB8888 source. `src_pitch` is the source row stride in pixels
 * (pass w for a tightly packed buffer). _blend alpha-composites. */
void tk_draw_blit      (struct tk_window*, int x, int y, int w, int h, const uint32_t *argb, int src_pitch);
void tk_draw_blit_blend(struct tk_window*, int x, int y, int w, int h, const uint32_t *argb, int src_pitch);

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
void  tk_redraw(struct tk_window*);   /* request a whole-window repaint next iter */
/* Mark ONE widget dirty for a partial (per-widget) repaint next iteration --
 * the setters (tk_set_text/tk_table_rows/...) do this automatically; call it
 * directly for a TK_CANVAS whose app-drawn content changed. */
void  tk_dirty (struct tk_window*, struct tk_widget*);
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
