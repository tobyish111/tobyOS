/* toby/ui.h -- Widget Toolkit "TobyUI" (Phase 2 M2.3).
 *
 * A retained-mode GUI toolkit providing standard controls for tobyOS
 * applications. Renders to a window backbuffer via the existing GUI
 * syscall interface.
 *
 * Architecture:
 *   - Client-side library (libtobyui.a) linked into each app
 *   - Widgets are organized in a tree (parent -> children)
 *   - Flexbox-like layout engine for positioning
 *   - Event bubbling from leaf widgets up to root
 *   - Focus management and keyboard navigation
 */

#ifndef TOBY_UI_H
#define TOBY_UI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Widget types ---- */

typedef enum {
    UI_WIDGET_NONE = 0,
    UI_WIDGET_CONTAINER,  /* generic container (div-like) */
    UI_WIDGET_LABEL,
    UI_WIDGET_BUTTON,
    UI_WIDGET_TEXTINPUT,
    UI_WIDGET_CHECKBOX,
    UI_WIDGET_RADIO,
    UI_WIDGET_SLIDER,
    UI_WIDGET_SCROLLBAR,
    UI_WIDGET_LISTVIEW,
    UI_WIDGET_TREEVIEW,
    UI_WIDGET_TABCONTROL,
    UI_WIDGET_MENUBAR,
    UI_WIDGET_TOOLBAR,
    UI_WIDGET_STATUSBAR,
    UI_WIDGET_PROGRESS,
    UI_WIDGET_DROPDOWN,
    UI_WIDGET_IMAGE,
} ui_widget_type_t;

/* ---- Layout ---- */

typedef enum {
    UI_LAYOUT_NONE = 0,
    UI_LAYOUT_VBOX,       /* vertical stack */
    UI_LAYOUT_HBOX,       /* horizontal stack */
    UI_LAYOUT_FLEX,       /* flexbox */
    UI_LAYOUT_GRID,       /* grid */
    UI_LAYOUT_ABSOLUTE,   /* manual positioning */
} ui_layout_t;

typedef enum {
    UI_ALIGN_START = 0,
    UI_ALIGN_CENTER,
    UI_ALIGN_END,
    UI_ALIGN_STRETCH,
} ui_align_t;

/* ---- Styling ---- */

typedef struct {
    uint32_t  bg_color;      /* background (0 = transparent) */
    uint32_t  fg_color;      /* foreground/text color */
    uint32_t  border_color;
    int       border_width;
    int       border_radius;
    int       padding[4];    /* top, right, bottom, left */
    int       margin[4];
    float     font_size;
    int       font_bold;
} ui_style_t;

/* ---- Events ---- */

typedef enum {
    UI_EVENT_NONE = 0,
    UI_EVENT_CLICK,
    UI_EVENT_DBLCLICK,
    UI_EVENT_MOUSEDOWN,
    UI_EVENT_MOUSEUP,
    UI_EVENT_MOUSEMOVE,
    UI_EVENT_MOUSEENTER,
    UI_EVENT_MOUSELEAVE,
    UI_EVENT_KEYDOWN,
    UI_EVENT_KEYUP,
    UI_EVENT_CHAR,
    UI_EVENT_FOCUS,
    UI_EVENT_BLUR,
    UI_EVENT_SCROLL,
    UI_EVENT_RESIZE,
    UI_EVENT_CHANGE,       /* value changed (checkbox, slider, etc.) */
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    int             x, y;       /* mouse position */
    int             button;     /* mouse button (1=left, 2=right, 3=mid) */
    int             key;        /* key scancode */
    int             ch;         /* character (for UI_EVENT_CHAR) */
    int             modifiers;  /* shift/ctrl/alt flags */
    int             scroll_dx;
    int             scroll_dy;
} ui_event_t;

/* ---- Widget structure ---- */

#define UI_MAX_CHILDREN  32
#define UI_TEXT_MAX      256

struct ui_widget;
typedef void (*ui_event_handler_t)(struct ui_widget *widget, ui_event_t *event);
typedef void (*ui_paint_handler_t)(struct ui_widget *widget, uint32_t *fb,
                                   int fb_w, int fb_h);

typedef struct ui_widget {
    ui_widget_type_t type;
    int              id;
    char             text[UI_TEXT_MAX];

    /* Geometry (set by layout engine) */
    int              x, y;          /* position relative to parent */
    int              width, height; /* actual size */
    int              min_w, min_h;  /* minimum size hints */
    int              max_w, max_h;  /* maximum size hints (0 = no limit) */
    int              flex_grow;     /* flex grow factor */

    /* Layout */
    ui_layout_t      layout;
    ui_align_t       align_items;   /* cross-axis alignment */
    ui_align_t       justify;       /* main-axis alignment */
    int              gap;           /* spacing between children */

    /* Style */
    ui_style_t       style;
    ui_style_t       style_hover;
    ui_style_t       style_active;
    ui_style_t       style_focus;

    /* State */
    int              visible;
    int              enabled;
    int              focused;
    int              hovered;
    int              pressed;
    int              checked;     /* for checkboxes/radio */
    int              value;       /* for sliders/progress */
    int              value_min;
    int              value_max;
    int              scroll_y;

    /* Tree structure */
    struct ui_widget *parent;
    struct ui_widget *children[UI_MAX_CHILDREN];
    int               n_children;

    /* Event handlers */
    ui_event_handler_t on_click;
    ui_event_handler_t on_change;
    ui_event_handler_t on_keydown;
    ui_paint_handler_t on_paint;   /* custom paint override */

    void             *userdata;
} ui_widget_t;

/* ---- Application window context ---- */

typedef struct {
    int           win_fd;        /* window file descriptor */
    uint32_t     *framebuffer;   /* backbuffer pointer */
    int           width;
    int           height;
    ui_widget_t  *root;          /* root widget */
    ui_widget_t  *focus;         /* currently focused widget */
    ui_widget_t  *hover;         /* widget under mouse */
} ui_context_t;

/* ---- API ---- */

/* Create a new UI context bound to an existing window fd */
ui_context_t *ui_create(int win_fd, uint32_t *framebuffer, int w, int h);
void          ui_destroy(ui_context_t *ctx);

/* Widget creation */
ui_widget_t *ui_widget_new(ui_widget_type_t type);
void         ui_widget_free(ui_widget_t *w);
void         ui_widget_add_child(ui_widget_t *parent, ui_widget_t *child);
void         ui_widget_remove_child(ui_widget_t *parent, ui_widget_t *child);

/* Convenience constructors */
ui_widget_t *ui_label(const char *text);
ui_widget_t *ui_button(const char *text, ui_event_handler_t on_click);
ui_widget_t *ui_textinput(const char *placeholder);
ui_widget_t *ui_checkbox(const char *label, int checked);
ui_widget_t *ui_slider(int min_val, int max_val, int value);
ui_widget_t *ui_progress(int value, int max_val);
ui_widget_t *ui_vbox(int gap);
ui_widget_t *ui_hbox(int gap);

/* Layout */
void ui_layout(ui_context_t *ctx);

/* Rendering */
void ui_paint(ui_context_t *ctx);
void ui_paint_widget(ui_widget_t *w, uint32_t *fb, int fb_w, int fb_h,
                     int ox, int oy);

/* Event dispatch */
void ui_dispatch_event(ui_context_t *ctx, ui_event_t *event);

/* Focus management */
void ui_focus_next(ui_context_t *ctx);
void ui_focus_prev(ui_context_t *ctx);
void ui_set_focus(ui_context_t *ctx, ui_widget_t *w);

/* Utility */
void ui_set_text(ui_widget_t *w, const char *text);
const char *ui_get_text(ui_widget_t *w);
void ui_set_style(ui_widget_t *w, ui_style_t *style);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_UI_H */
