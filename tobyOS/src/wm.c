/* src/wm.c -- Window manager implementation (Phase 7).
 *
 * Handles window decorations, focus, z-order, move/resize,
 * and minimize/maximize. Draws directly to the compositor's
 * framebuffer through gfx_* helpers.
 */

#include <tobyos/wm.h>
#include <tobyos/gfx.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/heap.h>

/* ---- Internal state ---- */

static struct wm_window g_windows[WM_MAX_WINDOWS];
static int g_next_id = 1;
static int g_focused_id = -1;
static int g_screen_w = 1024;
static int g_screen_h = 768;
static int g_taskbar_h = 40;

/* Move/resize tracking */
static int g_drag_win    = -1;
static int g_drag_mode   = WM_HIT_NONE;  /* titlebar=move, resize-edge */
static int g_drag_start_x, g_drag_start_y;
static int g_drag_orig_x, g_drag_orig_y, g_drag_orig_w, g_drag_orig_h;

/* Decoration colors */
#define WM_COLOR_TITLE_FOCUSED    0xFF2D5A8C
#define WM_COLOR_TITLE_UNFOCUSED  0xFF6B6B6B
#define WM_COLOR_TITLE_TEXT       0xFFFFFFFF
#define WM_COLOR_BORDER           0xFF404040
#define WM_COLOR_BTN_CLOSE        0xFFE04040
#define WM_COLOR_BTN_MAX          0xFF40A040
#define WM_COLOR_BTN_MIN          0xFFE0A020

/* ---- Helpers ---- */

static void wm_strcpy(char *dst, const char *src, int maxlen) {
    int i = 0;
    while (src[i] && i < maxlen - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static struct wm_window *find_window(int id) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (g_windows[i].active && g_windows[i].id == id)
            return &g_windows[i];
    }
    return 0;
}

static struct wm_window *alloc_window(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g_windows[i].active) return &g_windows[i];
    }
    return 0;
}

static int max_z_order(void) {
    int max = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (g_windows[i].active && g_windows[i].z_order > max)
            max = g_windows[i].z_order;
    }
    return max;
}

static void raise_window(struct wm_window *win) {
    int new_z = max_z_order() + 1;
    win->z_order = new_z;
}

static void update_client_rect(struct wm_window *win) {
    win->client_x = WM_BORDER_WIDTH;
    win->client_y = WM_TITLE_BAR_HEIGHT;
    win->client_w = win->w - 2 * WM_BORDER_WIDTH;
    win->client_h = win->h - WM_TITLE_BAR_HEIGHT - WM_BORDER_WIDTH;
    if (win->client_w < 1) win->client_w = 1;
    if (win->client_h < 1) win->client_h = 1;
}

/* ---- Public API ---- */

void wm_init(void) {
    memset(g_windows, 0, sizeof(g_windows));
    g_next_id = 1;
    g_focused_id = -1;

    /* Query actual screen dimensions from gfx if possible */
    int sw = (int)gfx_width();
    int sh = (int)gfx_height();
    if (sw > 0) g_screen_w = sw;
    if (sh > 0) g_screen_h = sh;

    kprintf("[wm] window manager init (%dx%d, taskbar %dpx)\n",
            g_screen_w, g_screen_h, g_taskbar_h);
}

int wm_create_window(int pid, const char *title, int x, int y, int w, int h) {
    struct wm_window *win = alloc_window();
    if (!win) return -1;

    memset(win, 0, sizeof(*win));
    win->active = 1;
    win->id = g_next_id++;
    win->pid = pid;
    win->x = x;
    win->y = y;
    win->w = w + 2 * WM_BORDER_WIDTH;
    win->h = h + WM_TITLE_BAR_HEIGHT + WM_BORDER_WIDTH;
    win->state = WM_STATE_NORMAL;
    win->z_order = max_z_order() + 1;
    win->dirty = 1;

    if (title) {
        wm_strcpy(win->title, title, sizeof(win->title));
    } else {
        wm_strcpy(win->title, "Window", sizeof(win->title));
    }

    update_client_rect(win);
    wm_focus_window(win->id);

    kprintf("[wm] created window %d '%s' at (%d,%d) %dx%d\n",
            win->id, win->title, x, y, w, h);
    return win->id;
}

void wm_destroy_window(int win_id) {
    struct wm_window *win = find_window(win_id);
    if (!win) return;

    if (g_focused_id == win_id) g_focused_id = -1;
    win->active = 0;
    kprintf("[wm] destroyed window %d\n", win_id);
}

void wm_set_title(int win_id, const char *title) {
    struct wm_window *win = find_window(win_id);
    if (!win || !title) return;
    wm_strcpy(win->title, title, sizeof(win->title));
    win->dirty = 1;
}

void wm_focus_window(int win_id) {
    struct wm_window *prev = find_window(g_focused_id);
    if (prev) {
        prev->focused = 0;
        prev->dirty = 1;
    }

    struct wm_window *win = find_window(win_id);
    if (!win) return;
    if (win->minimized) return;

    win->focused = 1;
    win->dirty = 1;
    g_focused_id = win_id;
    raise_window(win);
}

void wm_minimize(int win_id) {
    struct wm_window *win = find_window(win_id);
    if (!win || win->minimized) return;

    win->minimized = 1;
    win->state = WM_STATE_MINIMIZED;
    win->dirty = 1;

    if (g_focused_id == win_id) {
        g_focused_id = -1;
        /* Focus next visible window */
        int best_z = -1;
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            if (g_windows[i].active && !g_windows[i].minimized &&
                g_windows[i].z_order > best_z) {
                best_z = g_windows[i].z_order;
                g_focused_id = g_windows[i].id;
            }
        }
        if (g_focused_id >= 0) wm_focus_window(g_focused_id);
    }
}

void wm_maximize(int win_id) {
    struct wm_window *win = find_window(win_id);
    if (!win) return;

    if (win->maximized) {
        wm_restore(win_id);
        return;
    }

    /* Save current geometry for restore */
    win->restore_x = win->x;
    win->restore_y = win->y;
    win->restore_w = win->w;
    win->restore_h = win->h;

    /* Expand to full screen minus taskbar */
    win->x = 0;
    win->y = 0;
    win->w = g_screen_w;
    win->h = g_screen_h - g_taskbar_h;
    win->maximized = 1;
    win->state = WM_STATE_MAXIMIZED;
    update_client_rect(win);
    win->dirty = 1;
}

void wm_restore(int win_id) {
    struct wm_window *win = find_window(win_id);
    if (!win) return;

    if (win->minimized) {
        win->minimized = 0;
        win->state = WM_STATE_NORMAL;
        win->dirty = 1;
        wm_focus_window(win_id);
        return;
    }

    if (win->maximized) {
        win->x = win->restore_x;
        win->y = win->restore_y;
        win->w = win->restore_w;
        win->h = win->restore_h;
        win->maximized = 0;
        win->state = WM_STATE_NORMAL;
        update_client_rect(win);
        win->dirty = 1;
    }
}

void wm_close(int win_id) {
    struct wm_window *win = find_window(win_id);
    if (!win) return;

    /* Send close event to the owning process. For now we just
     * destroy the window. A real implementation would send a
     * WM_CLOSE message via the GUI event pipe so the app can
     * save unsaved work. */
    wm_destroy_window(win_id);
}

int wm_hit_test(int win_id, int x, int y) {
    struct wm_window *win = find_window(win_id);
    if (!win) return WM_HIT_NONE;

    int rx = x - win->x;
    int ry = y - win->y;

    /* Outside window */
    if (rx < 0 || ry < 0 || rx >= win->w || ry >= win->h)
        return WM_HIT_NONE;

    /* Resize edges */
    if (ry < WM_RESIZE_HANDLE && rx < WM_RESIZE_HANDLE) return WM_HIT_RESIZE_NW;
    if (ry < WM_RESIZE_HANDLE && rx >= win->w - WM_RESIZE_HANDLE) return WM_HIT_RESIZE_NE;
    if (ry >= win->h - WM_RESIZE_HANDLE && rx < WM_RESIZE_HANDLE) return WM_HIT_RESIZE_SW;
    if (ry >= win->h - WM_RESIZE_HANDLE && rx >= win->w - WM_RESIZE_HANDLE) return WM_HIT_RESIZE_SE;
    if (ry < WM_RESIZE_HANDLE) return WM_HIT_RESIZE_N;
    if (ry >= win->h - WM_RESIZE_HANDLE) return WM_HIT_RESIZE_S;
    if (rx < WM_RESIZE_HANDLE) return WM_HIT_RESIZE_W;
    if (rx >= win->w - WM_RESIZE_HANDLE) return WM_HIT_RESIZE_E;

    /* Title bar buttons (right side) */
    if (ry < WM_TITLE_BAR_HEIGHT) {
        int btn_y = (WM_TITLE_BAR_HEIGHT - WM_BUTTON_SIZE) / 2;
        int btn_right = win->w - WM_BUTTON_MARGIN;

        /* Close button (rightmost) */
        if (rx >= btn_right - WM_BUTTON_SIZE && rx < btn_right &&
            ry >= btn_y && ry < btn_y + WM_BUTTON_SIZE)
            return WM_HIT_CLOSE;

        /* Maximize button */
        btn_right -= WM_BUTTON_SIZE + WM_BUTTON_MARGIN;
        if (rx >= btn_right - WM_BUTTON_SIZE && rx < btn_right &&
            ry >= btn_y && ry < btn_y + WM_BUTTON_SIZE)
            return WM_HIT_MAXIMIZE;

        /* Minimize button */
        btn_right -= WM_BUTTON_SIZE + WM_BUTTON_MARGIN;
        if (rx >= btn_right - WM_BUTTON_SIZE && rx < btn_right &&
            ry >= btn_y && ry < btn_y + WM_BUTTON_SIZE)
            return WM_HIT_MINIMIZE;

        return WM_HIT_TITLEBAR;
    }

    return WM_HIT_CLIENT;
}

void wm_begin_move(int win_id) {
    struct wm_window *win = find_window(win_id);
    if (!win || win->maximized) return;
    win->moving = 1;
}

void wm_begin_resize(int win_id, int edge) {
    struct wm_window *win = find_window(win_id);
    if (!win || win->maximized) return;
    win->resizing = edge;
}

void wm_handle_mouse(int x, int y, int buttons, int dx, int dy) {
    (void)dx; (void)dy;

    /* If dragging */
    if (g_drag_win >= 0 && (buttons & 1)) {
        struct wm_window *win = find_window(g_drag_win);
        if (!win) { g_drag_win = -1; return; }

        int ddx = x - g_drag_start_x;
        int ddy = y - g_drag_start_y;

        if (g_drag_mode == WM_HIT_TITLEBAR) {
            /* Moving */
            win->x = g_drag_orig_x + ddx;
            win->y = g_drag_orig_y + ddy;
            win->dirty = 1;
        } else {
            /* Resizing */
            int new_x = g_drag_orig_x;
            int new_y = g_drag_orig_y;
            int new_w = g_drag_orig_w;
            int new_h = g_drag_orig_h;

            if (g_drag_mode == WM_HIT_RESIZE_E ||
                g_drag_mode == WM_HIT_RESIZE_NE ||
                g_drag_mode == WM_HIT_RESIZE_SE) {
                new_w += ddx;
            }
            if (g_drag_mode == WM_HIT_RESIZE_W ||
                g_drag_mode == WM_HIT_RESIZE_NW ||
                g_drag_mode == WM_HIT_RESIZE_SW) {
                new_x += ddx;
                new_w -= ddx;
            }
            if (g_drag_mode == WM_HIT_RESIZE_S ||
                g_drag_mode == WM_HIT_RESIZE_SE ||
                g_drag_mode == WM_HIT_RESIZE_SW) {
                new_h += ddy;
            }
            if (g_drag_mode == WM_HIT_RESIZE_N ||
                g_drag_mode == WM_HIT_RESIZE_NE ||
                g_drag_mode == WM_HIT_RESIZE_NW) {
                new_y += ddy;
                new_h -= ddy;
            }

            /* Minimum size constraints */
            if (new_w < 100) new_w = 100;
            if (new_h < WM_TITLE_BAR_HEIGHT + 50) new_h = WM_TITLE_BAR_HEIGHT + 50;

            win->x = new_x;
            win->y = new_y;
            win->w = new_w;
            win->h = new_h;
            update_client_rect(win);
            win->dirty = 1;
        }
        return;
    }

    /* Button released - end drag */
    if (g_drag_win >= 0 && !(buttons & 1)) {
        struct wm_window *win = find_window(g_drag_win);
        if (win) {
            win->moving = 0;
            win->resizing = 0;
        }
        g_drag_win = -1;
        g_drag_mode = WM_HIT_NONE;
        return;
    }

    /* Button pressed - start interaction */
    if (buttons & 1) {
        /* Find topmost window under cursor */
        struct wm_window *hit_win = 0;
        int best_z = -1;
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            if (!g_windows[i].active || g_windows[i].minimized) continue;
            int rx = x - g_windows[i].x;
            int ry = y - g_windows[i].y;
            if (rx >= 0 && ry >= 0 && rx < g_windows[i].w && ry < g_windows[i].h) {
                if (g_windows[i].z_order > best_z) {
                    best_z = g_windows[i].z_order;
                    hit_win = &g_windows[i];
                }
            }
        }

        if (!hit_win) return;

        wm_focus_window(hit_win->id);

        int hit = wm_hit_test(hit_win->id, x, y);
        switch (hit) {
        case WM_HIT_CLOSE:
            wm_close(hit_win->id);
            break;
        case WM_HIT_MAXIMIZE:
            wm_maximize(hit_win->id);
            break;
        case WM_HIT_MINIMIZE:
            wm_minimize(hit_win->id);
            break;
        case WM_HIT_TITLEBAR:
            g_drag_win = hit_win->id;
            g_drag_mode = WM_HIT_TITLEBAR;
            g_drag_start_x = x;
            g_drag_start_y = y;
            g_drag_orig_x = hit_win->x;
            g_drag_orig_y = hit_win->y;
            break;
        case WM_HIT_RESIZE_N: case WM_HIT_RESIZE_S:
        case WM_HIT_RESIZE_E: case WM_HIT_RESIZE_W:
        case WM_HIT_RESIZE_NE: case WM_HIT_RESIZE_NW:
        case WM_HIT_RESIZE_SE: case WM_HIT_RESIZE_SW:
            g_drag_win = hit_win->id;
            g_drag_mode = hit;
            g_drag_start_x = x;
            g_drag_start_y = y;
            g_drag_orig_x = hit_win->x;
            g_drag_orig_y = hit_win->y;
            g_drag_orig_w = hit_win->w;
            g_drag_orig_h = hit_win->h;
            break;
        default:
            break;
        }
    }
}

void wm_handle_key(int scancode, int pressed) {
    (void)scancode;
    (void)pressed;
    /* Alt+F4 close, Alt+Tab switch, etc. -- placeholder for now */
}

void wm_draw_decorations(struct wm_window *win) {
    if (!win || win->minimized) return;

    uint32_t *fb = gfx_backbuf();
    int fb_w = (int)gfx_width();
    int fb_h = (int)gfx_height();
    if (!fb || fb_w <= 0 || fb_h <= 0) return;

    uint32_t title_color = win->focused ? WM_COLOR_TITLE_FOCUSED
                                        : WM_COLOR_TITLE_UNFOCUSED;

    /* Draw title bar */
    for (int py = win->y; py < win->y + WM_TITLE_BAR_HEIGHT && py < fb_h; py++) {
        if (py < 0) continue;
        for (int px = win->x; px < win->x + win->w && px < fb_w; px++) {
            if (px < 0) continue;
            fb[py * fb_w + px] = title_color;
        }
    }

    /* Draw border (1px) */
    for (int py = win->y; py < win->y + win->h && py < fb_h; py++) {
        if (py < 0) continue;
        if (win->x >= 0 && win->x < fb_w)
            fb[py * fb_w + win->x] = WM_COLOR_BORDER;
        int rx = win->x + win->w - 1;
        if (rx >= 0 && rx < fb_w)
            fb[py * fb_w + rx] = WM_COLOR_BORDER;
    }
    for (int px = win->x; px < win->x + win->w && px < fb_w; px++) {
        if (px < 0) continue;
        int by = win->y + win->h - 1;
        if (by >= 0 && by < fb_h)
            fb[by * fb_w + px] = WM_COLOR_BORDER;
    }

    /* Draw close button (red square with X) */
    int btn_y = win->y + (WM_TITLE_BAR_HEIGHT - WM_BUTTON_SIZE) / 2;
    int btn_x = win->x + win->w - WM_BUTTON_MARGIN - WM_BUTTON_SIZE;
    for (int py = btn_y; py < btn_y + WM_BUTTON_SIZE && py < fb_h; py++) {
        if (py < 0) continue;
        for (int px = btn_x; px < btn_x + WM_BUTTON_SIZE && px < fb_w; px++) {
            if (px < 0) continue;
            fb[py * fb_w + px] = WM_COLOR_BTN_CLOSE;
        }
    }

    /* Draw maximize button (green square) */
    btn_x -= WM_BUTTON_SIZE + WM_BUTTON_MARGIN;
    for (int py = btn_y; py < btn_y + WM_BUTTON_SIZE && py < fb_h; py++) {
        if (py < 0) continue;
        for (int px = btn_x; px < btn_x + WM_BUTTON_SIZE && px < fb_w; px++) {
            if (px < 0) continue;
            fb[py * fb_w + px] = WM_COLOR_BTN_MAX;
        }
    }

    /* Draw minimize button (yellow square) */
    btn_x -= WM_BUTTON_SIZE + WM_BUTTON_MARGIN;
    for (int py = btn_y; py < btn_y + WM_BUTTON_SIZE && py < fb_h; py++) {
        if (py < 0) continue;
        for (int px = btn_x; px < btn_x + WM_BUTTON_SIZE && px < fb_w; px++) {
            if (px < 0) continue;
            fb[py * fb_w + px] = WM_COLOR_BTN_MIN;
        }
    }

    /* Draw title text using the 8x16 kernel font */
    int tx = win->x + WM_BUTTON_MARGIN + 4;
    int ty = win->y + (WM_TITLE_BAR_HEIGHT - 16) / 2;
    gfx_draw_text16(tx, ty, win->title, WM_COLOR_TITLE_TEXT, title_color);

    win->dirty = 0;
}

struct wm_window *wm_get_focused(void) {
    return find_window(g_focused_id);
}

struct wm_window *wm_get_window(int win_id) {
    return find_window(win_id);
}

void wm_foreach_z(int (*cb)(struct wm_window *win, void *ctx), void *ctx) {
    /* Sort by z_order and call in order */
    for (int z = 0; z <= max_z_order(); z++) {
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            if (g_windows[i].active && !g_windows[i].minimized &&
                g_windows[i].z_order == z) {
                if (cb(&g_windows[i], ctx)) return;
            }
        }
    }
}

/* ---- Snap layouts ---- */

static int g_snap_preview_zone = SNAP_NONE;

int wm_get_screen_w(void) { return g_screen_w; }
int wm_get_screen_h(void) { return g_screen_h; }

int wm_snap_detect(int x, int y, int screen_w, int screen_h) {
    int usable_h = screen_h - g_taskbar_h;
    int at_left   = (x <= SNAP_EDGE_THRESHOLD);
    int at_right  = (x >= screen_w - SNAP_EDGE_THRESHOLD);
    int at_top    = (y <= SNAP_EDGE_THRESHOLD);
    int at_bottom = (y >= usable_h - SNAP_EDGE_THRESHOLD);

    if (at_top && at_left)   return SNAP_TL;
    if (at_top && at_right)  return SNAP_TR;
    if (at_bottom && at_left)  return SNAP_BL;
    if (at_bottom && at_right) return SNAP_BR;
    if (at_left)   return SNAP_LEFT;
    if (at_right)  return SNAP_RIGHT;
    if (at_top)    return SNAP_TOP;

    return SNAP_NONE;
}

void wm_snap_apply(int wid, int snap_zone) {
    struct wm_window *win = find_window(wid);
    if (!win) return;

    if (snap_zone == SNAP_NONE) return;

    /* Save pre-snap geometry for restore (only if not already snapped) */
    if (!win->maximized) {
        win->restore_x = win->x;
        win->restore_y = win->y;
        win->restore_w = win->w;
        win->restore_h = win->h;
    }

    int usable_h = g_screen_h - g_taskbar_h;
    int half_w = g_screen_w / 2;
    int half_h = usable_h / 2;

    switch (snap_zone) {
    case SNAP_LEFT:
        win->x = 0; win->y = 0;
        win->w = half_w; win->h = usable_h;
        break;
    case SNAP_RIGHT:
        win->x = half_w; win->y = 0;
        win->w = g_screen_w - half_w; win->h = usable_h;
        break;
    case SNAP_TOP:
        win->x = 0; win->y = 0;
        win->w = g_screen_w; win->h = usable_h;
        win->maximized = 1;
        win->state = WM_STATE_MAXIMIZED;
        break;
    case SNAP_TL:
        win->x = 0; win->y = 0;
        win->w = half_w; win->h = half_h;
        break;
    case SNAP_TR:
        win->x = half_w; win->y = 0;
        win->w = g_screen_w - half_w; win->h = half_h;
        break;
    case SNAP_BL:
        win->x = 0; win->y = half_h;
        win->w = half_w; win->h = usable_h - half_h;
        break;
    case SNAP_BR:
        win->x = half_w; win->y = half_h;
        win->w = g_screen_w - half_w; win->h = usable_h - half_h;
        break;
    }

    update_client_rect(win);
    win->dirty = 1;
}

void wm_snap_preview(int snap_zone) {
    g_snap_preview_zone = snap_zone;
}

void wm_snap_cancel_preview(void) {
    g_snap_preview_zone = SNAP_NONE;
}

void wm_handle_snap_key(int wid, int key) {
    struct wm_window *win = find_window(wid);
    if (!win) return;

    /* Key codes: 0=Left, 1=Right, 2=Up, 3=Down */
    switch (key) {
    case 0: /* Win+Left */
        if (win->maximized) {
            wm_restore(wid);
            wm_snap_apply(wid, SNAP_LEFT);
        } else {
            wm_snap_apply(wid, SNAP_LEFT);
        }
        break;
    case 1: /* Win+Right */
        if (win->maximized) {
            wm_restore(wid);
            wm_snap_apply(wid, SNAP_RIGHT);
        } else {
            wm_snap_apply(wid, SNAP_RIGHT);
        }
        break;
    case 2: /* Win+Up -- maximize */
        wm_maximize(wid);
        break;
    case 3: /* Win+Down -- restore or minimize */
        if (win->maximized)
            wm_restore(wid);
        else
            wm_minimize(wid);
        break;
    }
}
