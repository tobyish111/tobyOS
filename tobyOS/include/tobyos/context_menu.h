/* context_menu.h -- Right-click context menus.
 *
 * Provides a single global context menu that can be shown/hidden by
 * any window. The compositor renders it on top of all windows. Only
 * one context menu can be visible at a time.
 */

#ifndef TOBYOS_CONTEXT_MENU_H
#define TOBYOS_CONTEXT_MENU_H

#include <tobyos/types.h>

#define CTX_MAX_ITEMS  16
#define CTX_ITEM_LEN   32

struct ctx_menu_item {
    char label[CTX_ITEM_LEN];
    int  id;
    int  enabled;
    int  separator;
};

struct ctx_menu {
    struct ctx_menu_item items[CTX_MAX_ITEMS];
    int  count;
    int  x, y;
    int  width, height;
    int  visible;
    int  owner_wid;
    int  hover_idx;
};

void ctx_menu_init(void);
int  ctx_menu_show(int wid, int x, int y,
                   const struct ctx_menu_item *items, int count);
void ctx_menu_hide(void);
int  ctx_menu_hit_test(int x, int y);
void ctx_menu_render(uint32_t *fb, int fb_w, int fb_h);
void ctx_menu_handle_mouse(int x, int y, int buttons);

#endif /* TOBYOS_CONTEXT_MENU_H */
