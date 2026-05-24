/* src/dnd.c -- Drag-and-drop between windows.
 *
 * Tracks a single active drag operation globally. During a drag the
 * cursor changes appearance, and the window under the cursor receives
 * DND_OFFER events so it can highlight a drop zone. On release,
 * DND_DROP delivers the payload to the target window.
 */

#include <tobyos/dnd.h>
#include <tobyos/wm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* ---- Drag state (one active drag at a time) ---- */

static struct {
    int  active;
    int  source_wid;
    int  type;
    char data[DND_DATA_MAX];
    int  data_len;
    int  cur_x, cur_y;
    int  target_wid;
    int  accepted;
} g_drag;

/* ---- Init ---- */

void dnd_init(void) {
    memset(&g_drag, 0, sizeof(g_drag));
    g_drag.target_wid = -1;
    kprintf("[dnd] drag-and-drop subsystem init\n");
}

/* ---- Begin a drag ---- */

int dnd_begin(int source_wid, int type, const char *data, int len) {
    if (g_drag.active)
        return -1;
    if (!data || len <= 0 || len > DND_DATA_MAX)
        return -1;

    g_drag.active = 1;
    g_drag.source_wid = source_wid;
    g_drag.type = type;
    memcpy(g_drag.data, data, (size_t)len);
    g_drag.data_len = len;
    g_drag.target_wid = -1;
    g_drag.accepted = 0;
    g_drag.cur_x = 0;
    g_drag.cur_y = 0;

    kprintf("[dnd] begin drag from wid %d type=%d len=%d\n",
            source_wid, type, len);
    return 0;
}

/* ---- Determine which window is under (x, y) ---- */

struct hit_ctx {
    int x, y;
    int best_z;
    int best_wid;
};

static int find_target_cb(struct wm_window *win, void *ctx) {
    struct hit_ctx *hc = (struct hit_ctx *)ctx;
    if (win->id == g_drag.source_wid)
        return 0;

    int rx = hc->x - win->x;
    int ry = hc->y - win->y;
    if (rx >= 0 && ry >= 0 && rx < win->w && ry < win->h) {
        if (win->z_order > hc->best_z) {
            hc->best_z = win->z_order;
            hc->best_wid = win->id;
        }
    }
    return 0;
}

static int find_window_at(int x, int y) {
    struct hit_ctx hc = { x, y, -1, -1 };
    wm_foreach_z(find_target_cb, &hc);
    return hc.best_wid;
}

/* ---- Update cursor position during drag ---- */

int dnd_update(int x, int y) {
    if (!g_drag.active)
        return -1;

    g_drag.cur_x = x;
    g_drag.cur_y = y;

    int new_target = find_window_at(x, y);

    if (new_target != g_drag.target_wid) {
        g_drag.target_wid = new_target;
        g_drag.accepted = 0;
    }

    return 0;
}

/* ---- Drop at current position ---- */

int dnd_drop(int x, int y) {
    if (!g_drag.active)
        return -1;

    g_drag.cur_x = x;
    g_drag.cur_y = y;

    int target = find_window_at(x, y);
    g_drag.target_wid = target;

    if (target < 0) {
        kprintf("[dnd] drop cancelled -- no target window\n");
        dnd_cancel();
        return -1;
    }

    kprintf("[dnd] drop on wid %d at (%d,%d)\n", target, x, y);

    /* The drag remains "active" briefly so the target can poll via
     * dnd_get_offer() and call dnd_accept(). The compositor or event
     * loop should call dnd_cancel() after the target has processed. */
    return 0;
}

/* ---- Cancel / finish drag ---- */

void dnd_cancel(void) {
    if (g_drag.active) {
        kprintf("[dnd] drag cancelled/finished\n");
    }
    memset(&g_drag, 0, sizeof(g_drag));
    g_drag.target_wid = -1;
}

/* ---- Target polls for an offer ---- */

int dnd_get_offer(int target_wid, struct dnd_offer *out) {
    if (!g_drag.active || !out)
        return -1;
    if (g_drag.target_wid != target_wid)
        return -1;

    out->type = g_drag.type;
    out->source_wid = g_drag.source_wid;
    memcpy(out->data, g_drag.data, (size_t)g_drag.data_len);
    out->data_len = g_drag.data_len;
    out->x = g_drag.cur_x;
    out->y = g_drag.cur_y;
    out->accepted = g_drag.accepted;
    return 0;
}

/* ---- Target accepts the drop ---- */

int dnd_accept(int target_wid) {
    if (!g_drag.active)
        return -1;
    if (g_drag.target_wid != target_wid)
        return -1;

    g_drag.accepted = 1;
    kprintf("[dnd] wid %d accepted drop\n", target_wid);

    /* Finalize: clear drag state now that payload is delivered */
    int type = g_drag.type;
    (void)type;
    memset(&g_drag, 0, sizeof(g_drag));
    g_drag.target_wid = -1;
    return 0;
}

/* ---- Query whether a drag is in progress ---- */

int dnd_is_active(void) {
    return g_drag.active;
}
