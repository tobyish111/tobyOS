/* src/context_menu.c -- Right-click context menus.
 *
 * A single global context menu rendered by the compositor on top of
 * all windows. Supports up to CTX_MAX_ITEMS items with hover highlight,
 * separators, and disabled entries.
 */

#include <tobyos/context_menu.h>
#include <tobyos/gfx.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* ---- Styling ---- */

#define CTX_ITEM_HEIGHT    24
#define CTX_SEP_HEIGHT      8
#define CTX_PADDING_X      12
#define CTX_PADDING_Y       4
#define CTX_MIN_WIDTH     120
#define CTX_CHAR_WIDTH      8

#define CTX_COLOR_BG       0xFFF0F0F0
#define CTX_COLOR_BORDER   0xFF808080
#define CTX_COLOR_HOVER    0xFF3078D0
#define CTX_COLOR_TEXT     0xFF202020
#define CTX_COLOR_TEXT_HI  0xFFFFFFFF
#define CTX_COLOR_DISABLED 0xFF909090
#define CTX_COLOR_SEP      0xFFC0C0C0

/* ---- Global state ---- */

static struct ctx_menu g_menu;

/* ---- Init ---- */

void ctx_menu_init(void) {
    memset(&g_menu, 0, sizeof(g_menu));
    g_menu.hover_idx = -1;
    kprintf("[ctx_menu] context menu subsystem init\n");
}

/* ---- Show menu ---- */

int ctx_menu_show(int wid, int x, int y,
                  const struct ctx_menu_item *items, int count) {
    if (!items || count <= 0) return -1;
    if (count > CTX_MAX_ITEMS) count = CTX_MAX_ITEMS;

    g_menu.owner_wid = wid;
    g_menu.x = x;
    g_menu.y = y;
    g_menu.count = count;
    g_menu.hover_idx = -1;
    g_menu.visible = 1;

    /* Copy items and compute dimensions */
    int max_label_w = 0;
    int total_h = CTX_PADDING_Y * 2;

    for (int i = 0; i < count; i++) {
        memcpy(&g_menu.items[i], &items[i], sizeof(struct ctx_menu_item));
        if (items[i].separator) {
            total_h += CTX_SEP_HEIGHT;
        } else {
            total_h += CTX_ITEM_HEIGHT;
            int lw = (int)strlen(items[i].label) * CTX_CHAR_WIDTH;
            if (lw > max_label_w) max_label_w = lw;
        }
    }

    g_menu.width = max_label_w + CTX_PADDING_X * 2;
    if (g_menu.width < CTX_MIN_WIDTH) g_menu.width = CTX_MIN_WIDTH;
    g_menu.height = total_h;

    return 0;
}

/* ---- Hide menu ---- */

void ctx_menu_hide(void) {
    g_menu.visible = 0;
    g_menu.hover_idx = -1;
}

/* ---- Hit test: returns item id at (x,y) or -1 ---- */

int ctx_menu_hit_test(int x, int y) {
    if (!g_menu.visible) return -1;

    int rx = x - g_menu.x;
    int ry = y - g_menu.y;

    if (rx < 0 || ry < 0 || rx >= g_menu.width || ry >= g_menu.height)
        return -1;

    int cur_y = CTX_PADDING_Y;
    for (int i = 0; i < g_menu.count; i++) {
        int item_h = g_menu.items[i].separator ? CTX_SEP_HEIGHT : CTX_ITEM_HEIGHT;
        if (ry >= cur_y && ry < cur_y + item_h) {
            if (g_menu.items[i].separator) return -1;
            if (!g_menu.items[i].enabled) return -1;
            return g_menu.items[i].id;
        }
        cur_y += item_h;
    }
    return -1;
}

/* ---- Handle mouse for hover tracking ---- */

void ctx_menu_handle_mouse(int x, int y, int buttons) {
    (void)buttons;
    if (!g_menu.visible) return;

    int rx = x - g_menu.x;
    int ry = y - g_menu.y;

    if (rx < 0 || ry < 0 || rx >= g_menu.width || ry >= g_menu.height) {
        g_menu.hover_idx = -1;
        return;
    }

    int cur_y = CTX_PADDING_Y;
    g_menu.hover_idx = -1;
    for (int i = 0; i < g_menu.count; i++) {
        int item_h = g_menu.items[i].separator ? CTX_SEP_HEIGHT : CTX_ITEM_HEIGHT;
        if (ry >= cur_y && ry < cur_y + item_h) {
            if (!g_menu.items[i].separator && g_menu.items[i].enabled)
                g_menu.hover_idx = i;
            break;
        }
        cur_y += item_h;
    }
}

/* ---- Render the menu onto a framebuffer ---- */

void ctx_menu_render(uint32_t *fb, int fb_w, int fb_h) {
    if (!g_menu.visible || !fb) return;

    int mx = g_menu.x;
    int my = g_menu.y;
    int mw = g_menu.width;
    int mh = g_menu.height;

    /* Clamp to screen */
    if (mx + mw > fb_w) mx = fb_w - mw;
    if (my + mh > fb_h) my = fb_h - mh;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;

    /* Background fill */
    for (int py = my; py < my + mh && py < fb_h; py++) {
        if (py < 0) continue;
        for (int px = mx; px < mx + mw && px < fb_w; px++) {
            if (px < 0) continue;
            fb[py * fb_w + px] = CTX_COLOR_BG;
        }
    }

    /* Border */
    for (int py = my; py < my + mh && py < fb_h; py++) {
        if (py < 0) continue;
        if (mx >= 0 && mx < fb_w)
            fb[py * fb_w + mx] = CTX_COLOR_BORDER;
        int rx = mx + mw - 1;
        if (rx >= 0 && rx < fb_w)
            fb[py * fb_w + rx] = CTX_COLOR_BORDER;
    }
    for (int px = mx; px < mx + mw && px < fb_w; px++) {
        if (px < 0) continue;
        if (my >= 0 && my < fb_h)
            fb[my * fb_w + px] = CTX_COLOR_BORDER;
        int by = my + mh - 1;
        if (by >= 0 && by < fb_h)
            fb[by * fb_w + px] = CTX_COLOR_BORDER;
    }

    /* Items */
    int cur_y = my + CTX_PADDING_Y;
    for (int i = 0; i < g_menu.count; i++) {
        struct ctx_menu_item *item = &g_menu.items[i];

        if (item->separator) {
            int sep_y = cur_y + CTX_SEP_HEIGHT / 2;
            if (sep_y >= 0 && sep_y < fb_h) {
                for (int px = mx + 4; px < mx + mw - 4 && px < fb_w; px++) {
                    if (px >= 0)
                        fb[sep_y * fb_w + px] = CTX_COLOR_SEP;
                }
            }
            cur_y += CTX_SEP_HEIGHT;
            continue;
        }

        /* Hover highlight */
        int is_hover = (i == g_menu.hover_idx);
        if (is_hover) {
            for (int py = cur_y; py < cur_y + CTX_ITEM_HEIGHT && py < fb_h; py++) {
                if (py < 0) continue;
                for (int px = mx + 1; px < mx + mw - 1 && px < fb_w; px++) {
                    if (px < 0) continue;
                    fb[py * fb_w + px] = CTX_COLOR_HOVER;
                }
            }
        }

        /* Label text */
        uint32_t text_color;
        if (!item->enabled)
            text_color = CTX_COLOR_DISABLED;
        else if (is_hover)
            text_color = CTX_COLOR_TEXT_HI;
        else
            text_color = CTX_COLOR_TEXT;

        int tx = mx + CTX_PADDING_X;
        int ty = cur_y + (CTX_ITEM_HEIGHT - 16) / 2;
        gfx_draw_text16(tx, ty, item->label, text_color, GFX_TRANSPARENT);

        cur_y += CTX_ITEM_HEIGHT;
    }
}
