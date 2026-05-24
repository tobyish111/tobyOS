/* wm.h -- Window manager (Phase 7).
 *
 * Manages window decorations, focus, move/resize, minimize/maximize,
 * and z-ordering. Works alongside the existing compositor.
 */

#ifndef TOBYOS_WM_H
#define TOBYOS_WM_H

#include <tobyos/types.h>

/* Decoration dimensions */
#define WM_TITLE_BAR_HEIGHT  30
#define WM_BORDER_WIDTH      1
#define WM_RESIZE_HANDLE     5
#define WM_BUTTON_SIZE       20
#define WM_BUTTON_MARGIN     5

/* Window states */
#define WM_STATE_NORMAL      0
#define WM_STATE_MAXIMIZED   1
#define WM_STATE_MINIMIZED   2
#define WM_STATE_FULLSCREEN  3

/* Hit-test results */
#define WM_HIT_NONE          0
#define WM_HIT_TITLEBAR      1
#define WM_HIT_CLOSE         2
#define WM_HIT_MAXIMIZE      3
#define WM_HIT_MINIMIZE      4
#define WM_HIT_CLIENT        5
#define WM_HIT_RESIZE_N      6
#define WM_HIT_RESIZE_S      7
#define WM_HIT_RESIZE_E      8
#define WM_HIT_RESIZE_W      9
#define WM_HIT_RESIZE_NE    10
#define WM_HIT_RESIZE_NW    11
#define WM_HIT_RESIZE_SE    12
#define WM_HIT_RESIZE_SW    13

/* Maximum managed windows */
#define WM_MAX_WINDOWS       32

struct wm_window {
    int  id;
    int  client_fd;
    char title[64];
    int  x, y, w, h;          /* outer frame bounds (including decorations) */
    int  client_x, client_y;  /* content area offset relative to x, y */
    int  client_w, client_h;  /* content area dimensions */
    int  state;
    int  focused;
    int  z_order;
    int  resizing;
    int  moving;
    int  minimized;
    int  maximized;
    int  restore_x, restore_y, restore_w, restore_h;
    int  dirty;                /* needs redraw */
    int  active;               /* slot in use */
    int  pid;                  /* owning process PID */
};

/* Initialise the window manager. Call once during GUI subsystem startup. */
void wm_init(void);

/* Process a mouse event. Coordinates are absolute screen position. */
void wm_handle_mouse(int x, int y, int buttons, int dx, int dy);

/* Process a key event from the keyboard driver. */
void wm_handle_key(int scancode, int pressed);

/* Draw window decorations (title bar, borders, buttons).
 * Call from the compositor's per-frame draw path. */
void wm_draw_decorations(struct wm_window *win);

/* Set input focus to a window. */
void wm_focus_window(int win_id);

/* Minimize/maximize/restore/close operations. */
void wm_minimize(int win_id);
void wm_maximize(int win_id);
void wm_restore(int win_id);
void wm_close(int win_id);

/* Begin interactive move/resize. */
void wm_begin_move(int win_id);
void wm_begin_resize(int win_id, int edge);

/* Hit-test: given a point in screen coordinates, determine which
 * part of the window decoration (or client area) was hit.
 * Returns one of WM_HIT_* constants. */
int wm_hit_test(int win_id, int x, int y);

/* Create/destroy managed windows. Returns the window ID or -1. */
int wm_create_window(int pid, const char *title, int x, int y, int w, int h);
void wm_destroy_window(int win_id);

/* Set window title. */
void wm_set_title(int win_id, const char *title);

/* Get the focused window (NULL if none). */
struct wm_window *wm_get_focused(void);

/* Get a window by ID (NULL if invalid). */
struct wm_window *wm_get_window(int win_id);

/* Iterate all active windows in z-order (bottom to top).
 * Calls `cb` for each window; stops if cb returns non-zero. */
void wm_foreach_z(int (*cb)(struct wm_window *win, void *ctx), void *ctx);

/* ---- Snap layouts (Desktop UX Polish) ---- */

#define SNAP_NONE    0
#define SNAP_LEFT    1
#define SNAP_RIGHT   2
#define SNAP_TOP     3
#define SNAP_TL      4
#define SNAP_TR      5
#define SNAP_BL      6
#define SNAP_BR      7

#define SNAP_EDGE_THRESHOLD 10

int  wm_snap_detect(int x, int y, int screen_w, int screen_h);
void wm_snap_apply(int wid, int snap_zone);
void wm_snap_preview(int snap_zone);
void wm_snap_cancel_preview(void);
void wm_handle_snap_key(int wid, int key);
int  wm_get_screen_w(void);
int  wm_get_screen_h(void);

#endif /* TOBYOS_WM_H */
