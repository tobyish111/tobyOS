/* gui.c -- window manager + desktop environment + compositor.
 *
 * Single global state machine driven from two places:
 *
 *   1. mouse_irq -> on_mouse_event() (IRQ context). Updates the cursor
 *      position, button bitmask, drag state, and enqueues mouse events
 *      into the topmost window under the cursor. Marks `g.dirty`.
 *      In milestone 12 it also handles taskbar / launcher / close-button
 *      clicks before falling through to per-window dispatch.
 *
 *   2. gui_tick() (called from the idle loop, NOT from IRQ). If
 *      g.dirty is set, runs the compositor pass and gfx_flip()s.
 *      Also drains the deferred app-launch queue (mouse IRQ enqueues
 *      "open this program", gui_tick spawns it via proc_create_from_elf
 *      while running on the pid-0 idle thread) and reaps terminated
 *      desktop-launched processes.
 *
 * That split keeps the IRQ short and avoids drawing from interrupt
 * context. Concurrency is simple: the IRQ only ever modifies the small
 * "input" parts of state (cursor pos, dirty flag, event queues, launch
 * queue); the compositor + launcher only read them.
 *
 * Z-order: doubly linked list. g.z_top = head = topmost (= keyboard
 * focused). Clicking anywhere on a non-topmost window splices it to
 * the head; clicking on a taskbar tab does the same.
 *
 * Dragging: mouse-down on a window's title bar (NOT on the close
 * button) records (drag_win, drag_dx, drag_dy). Subsequent moves
 * while a button is held update the window's position. Mouse-up
 * clears the drag and synthesises a matching mouse-up to the app.
 *
 * Desktop mode (milestone 12): a sticky flag that keeps the
 * compositor active even with zero windows. While set, we paint the
 * wallpaper + taskbar even on an empty desktop. Activated via
 * gui_set_desktop_mode(true) (the `desktop` shell command), exited
 * via the launcher's "Exit Desktop" entry.
 */

#include <tobyos/gui.h>
#include <tobyos/gfx.h>
#include <tobyos/mouse.h>
#include <tobyos/heap.h>
#include <tobyos/pmm.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/console.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/signal.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>
#include <tobyos/service.h>
#include <tobyos/session.h>
#include <tobyos/settings.h>
#include <tobyos/cap.h>
#include <tobyos/pkg.h>
#include <tobyos/perf.h>
#include <tobyos/theme.h>
#include <tobyos/notify.h>
#include <tobyos/sysmon.h>
#include <tobyos/net.h>
#include <tobyos/audio_hda.h>
#include <tobyos/bootlog.h>
#include <stdarg.h>

/* ---- internal window struct ---------------------------------------- */

struct window {
    bool       in_use;
    int        wid;                 /* 1.. (slot+1), 0 means closed */
    int        owner_pid;
    int        x, y;                /* outer top-left in screen coords */
    int        client_w, client_h;
    char       title[GUI_TITLE_MAX];
    uint32_t  *backbuf;             /* client_w * client_h */

    struct gui_event ev[GUI_EVENT_RING];
    uint8_t          ev_head;       /* producer: IRQ */
    uint8_t          ev_tail;       /* consumer: poll syscall */

    struct window *z_prev, *z_next; /* doubly linked, head = top */
};

/* ---- module state ------------------------------------------------- */

/* M31: every colour the compositor draws now comes from the theme
 * palette (theme_active()). Previously this block held a fixed set
 * of #defines (DESKTOP_BG, TITLE_BG_FG, ...); they were inlined into
 * the paint functions which made theming impossible without a
 * recompile. The palette layout in theme.h preserves the same
 * meaning -- start here when looking for "where did the menu colour
 * go?". A debug build can `theme_set(THEME_BASIC)` from a serial
 * command to A/B against the M12 colours. */

#define TASKBAR_BRAND        "TOBYOS"

#define START_BTN_W       58
#define START_BTN_LABEL   "TO"
#define TASKBAR_SEARCH_W  150
#define TASKBAR_PIN_COUNT 7
#define TASKBAR_PIN_W      42
#define TAB_W             148
#define TAB_PAD           6
#define TAB_TEXT_MAX      14

/* ---- M31 system tray geometry ----------------------------------- *
 *
 * The tray sits on the right edge of the taskbar. Each indicator
 * occupies a fixed-width "pill" (rounded look faked with a 1-px
 * accent border on the top edge). The clock is always present at
 * the very right; the bell pill sits just left of it; the rest are
 * only drawn when the underlying subsystem says it has something
 * meaningful to report.
 *
 * Pill widths are conservative (60-92 px) so the tray fits even on
 * a 1024x768 mode without trimming. On wider modes we get a bit of
 * empty space between the last tab and the first pill, which reads
 * fine.
 *
 * Tray pills, right-to-left:
 *   CLOCK  - "HH:MM:SS"   88 px
 *   BELL   - "B N"        56 px (N = unread notify count)
 *   WIN    - "WIN N"      72 px (N = open windows)
 *   AUD    - "AUD ON|--"  72 px
 *   DISK   - "DISK OK"    80 px
 *   NET    - "NET a.b.c.d / NO LINK"  176 px (widest -- IPv4 string) */

#define TRAY_PAD          8
#define TRAY_PILL_H       (GUI_TASKBAR_H - 10)
#define TRAY_GAP          4

#define TRAY_W_CLOCK      96
#define TRAY_W_BELL       56
#define TRAY_W_WIN        72
#define TRAY_W_AUD        72
#define TRAY_W_DISK       80
#define TRAY_W_NET       176

/* ---- M31 toast geometry ----------------------------------------- */

#define TOAST_W          340
#define TOAST_H_TITLE     38   /* title only */
#define TOAST_H_FULL      80   /* title + body */
#define TOAST_MARGIN      14   /* from screen-right and taskbar-top */

/* ---- M31 notification center geometry --------------------------- */

#define CENTER_W         360
#define CENTER_HEAD_H     40
#define CENTER_FOOT_H     30
#define CENTER_ITEM_H     56
#define CENTER_ITEM_PAD    8
#define CENTER_VISIBLE_MAX 6     /* items rendered without scrolling */

/* System slice (compiled-in), plus a user slice filled in at runtime
 * by the package manager, plus one pinned "Logout" entry at the
 * bottom. LAUNCHER_MAX sizes the internal cursor math; keep it big
 * enough to hold all three plus headroom. */
#define LAUNCHER_SYS_MAX  8
#define LAUNCHER_MAX      (LAUNCHER_SYS_MAX + GUI_LAUNCHER_USER_MAX + 1)
#define LAUNCHER_W        420
#define LAUNCHER_HEAD_H    84
#define LAUNCHER_ITEM_H    25
#define LAUNCHER_PAD       10
#define LAUNCHER_PROFILE_W 116
#define LAUNCHER_LIST_W   172
#define LAUNCHER_TILE_W    58
#define LAUNCHER_TILE_H    50

#define LAUNCH_QUEUE_MAX   4
#define TRACKED_PIDS_MAX   8
#define LAUNCH_PATH_MAX    96
#define LAUNCH_ARG_MAX     128
#define SYSMON_HISTORY     32

struct launcher_item {
    const char *label;
    const char *path;        /* /bin/<...>; NULL = special quit-desktop */
};

/* System entries at the top of the menu. NEVER mutated after boot;
 * declared const so the linker keeps them in .rodata. */
static const struct launcher_item g_launcher_sys[LAUNCHER_SYS_MAX] = {
    { "File Explorer", "/bin/gui_files"    },
    { "Settings",      "/bin/gui_settings" },
    { "Nex Terminal",  "/bin/gui_term"     },
    { "Toby Widgets",  "/bin/gui_widgets"  },
    { "Photos",        "/bin/gui_viewer"   },
    { "Clock",         "/bin/gui_clock"    },
    { "About TobyOS",  "/bin/gui_about"    },
    { 0, 0 },
};

/* User entries (populated by gui_launcher_register() from pkg_init
 * and the `pkg install / remove` flow). Labels + paths are copied in
 * so the caller's transient buffers can go away. */
static char g_launcher_user_label   [GUI_LAUNCHER_USER_MAX][GUI_LAUNCHER_LABEL_MAX];
static char g_launcher_user_path    [GUI_LAUNCHER_USER_MAX][LAUNCH_PATH_MAX];
static char g_launcher_user_sandbox [GUI_LAUNCHER_USER_MAX][CAP_PROFILE_NAME_MAX];
/* M34D: declared CAPS list per launcher entry. Empty when the package
 * didn't declare one (legacy app -> profile alone narrows). */
static char g_launcher_user_caps    [GUI_LAUNCHER_USER_MAX][PKG_CAPS_LIST_MAX];
static int  g_launcher_user_count;

/* Pinned "Logout" entry at the bottom, always present. */
static const struct launcher_item g_launcher_logout = { "Logout", 0 };

static int launcher_sys_count(void) {
    int n = 0;
    for (int i = 0; i < LAUNCHER_SYS_MAX && g_launcher_sys[i].label; i++) n++;
    return n;
}

/* Populate *out with the launcher item at combined index `idx`. Path
 * may be NULL for the Logout entry. Returns false if idx is out of
 * range. */
static bool launcher_resolve(int idx, struct launcher_item *out) {
    int s = launcher_sys_count();
    if (idx < 0) return false;
    if (idx < s) { *out = g_launcher_sys[idx]; return true; }
    idx -= s;
    if (idx < g_launcher_user_count) {
        out->label = g_launcher_user_label[idx];
        out->path  = g_launcher_user_path [idx];
        return true;
    }
    idx -= g_launcher_user_count;
    if (idx == 0) { *out = g_launcher_logout; return true; }
    return false;
}

/* A queued launch request. `path` is the ELF to load; `arg` is an
 * optional single argument that will appear as argv[1] in the child
 * (argv[0] is always the basename of `path`). `sandbox` is the
 * milestone-18 profile name (empty = inherit parent untouched). All
 * three strings are stored inline so the caller doesn't need to
 * keep its copy alive. */
struct launch_entry {
    char path   [LAUNCH_PATH_MAX];
    char arg    [LAUNCH_ARG_MAX];
    char sandbox[CAP_PROFILE_NAME_MAX];
    /* M34D: declared capability list ("FILE_READ,GUI") to apply after
     * the sandbox profile narrows caps. Empty = no extra narrowing. */
    char caps   [PKG_CAPS_LIST_MAX];
    bool has_arg;
};

static int launcher_count(void) {
    /* system + user + pinned Logout */
    return launcher_sys_count() + g_launcher_user_count + 1;
}

/* Defined further down after copy_bounded + the `g` state struct are
 * in scope -- the implementations call both. */
int  gui_launcher_register(const char *label, const char *path);
void gui_launcher_reset_user(void);

static struct window g_pool[GUI_WINDOW_MAX];

static struct {
    bool          ready;
    bool          active;        /* >= 1 window alive OR desktop_mode on */
    bool          desktop_mode;  /* sticky: keep compositor active when
                                  * no windows exist (paints wallpaper
                                  * + taskbar + branding). */
    volatile bool dirty;         /* compositor needs to redraw + flip */

    /* Cursor + buttons (written by IRQ, read by compositor). */
    volatile int     cur_x, cur_y;
    volatile uint8_t cur_buttons;
    volatile uint8_t prev_buttons;

    /* Drag state (set/cleared by IRQ). */
    struct window *drag_win;
    int            drag_dx, drag_dy;

    struct window *z_top;        /* head of z-order list */
    int            spawn_x, spawn_y;  /* tiled placement cursor */

    /* ---- desktop-environment bits (milestone 12) ----------------- */

    /* Launcher menu open/closed. Toggled by clicking the start button. */
    bool          menu_open;

    /* Deferred launch queue: mouse IRQ / syscalls push "open this
     * path [with this arg]", the gui_tick() drain (running on pid 0)
     * consumes them and spawns the program. Entries carry inline
     * strings so the caller's buffer can go away immediately. */
    struct launch_entry launch_q[LAUNCH_QUEUE_MAX];
    uint8_t             launch_head, launch_tail;

    /* PIDs of apps we launched from the desktop. Reaped lazily in
     * gui_tick() so terminated children don't sit around forever as
     * zombies. Slot=0 means empty. */
    int           tracked_pids[TRACKED_PIDS_MAX];

    /* M27E: compositor-level invalidation hints. The compositor still
     * does a correct full repaint each frame (so the back buffer is
     * always pixel-perfect), but if every dirty event was a hint
     * (mouse move, window flip, drag) we replace the gfx-layer dirty
     * accumulator with `inv_*` just before gfx_flip(). gfx_flip()
     * then takes the present_rect() path and the only pixels actually
     * pushed to the front buffer are the ones in this hint union.
     *
     * `inv_full`  forces a full present (window create/destroy, z-
     *             order swap, mode change, backend swap) -- gfx_flip
     *             goes back through the b->flip() path.
     * `inv_w/h==0` means "no hint this frame" -- the compositor falls
     *             back to whatever the gfx accumulator already had.
     * `inv_x/y/w/h` is the union of all hints since the last flip,
     *             clipped to the screen. */
    int           inv_x, inv_y, inv_w, inv_h;
    bool          inv_full;
    /* M27E: per-tick stats. Updated AFTER gfx_flip so the stats
     * reflect what actually went out, not what was hinted. */
    uint64_t      cmp_full_frames;     /* used b->flip()              */
    uint64_t      cmp_partial_frames;  /* used b->present_rect()      */

    volatile int  input_boost_pid;     /* focused app with fresh key input */

    /* ---- M31 desktop notifications + system tray ---------------- */

    /* Currently-displayed toast. id == 0 means "no toast active";
     * the compositor will pop the next pending one from the kernel
     * notify ring on its next pass. The strings are copied out of
     * the ring at pop time so the toast survives the ring's own
     * dismiss/eviction churn. */
    uint32_t toast_id;
    uint32_t toast_urgency;
    uint64_t toast_expire_ms;          /* boot-relative ms              */
    char     toast_app  [ABI_NOTIFY_APP_MAX];
    char     toast_title[ABI_NOTIFY_TITLE_MAX];
    char     toast_body [ABI_NOTIFY_BODY_MAX];

    /* Notification-center panel (slides in from the right when the
     * tray bell is clicked). Pure overlay -- not a real window, so
     * it never steals focus and has no Z-order. */
    bool     center_open;

    /* M36 shell panels. These are compositor-owned shell UI, not fake
     * app windows. Clicking the widgets/taskbar pin toggles them, and
     * quick-setting tiles update local shell state or post real kernel
     * status notifications. */
    bool     widgets_open;
    bool     quick_wifi;
    bool     quick_bt;
    bool     quick_night;
    bool     quick_nixie;
    bool     quick_airplane;
    bool     quick_focus;

    struct abi_system_metrics mon;
    uint64_t mon_last_sample_ms;
    uint8_t  mon_cpu_hist [SYSMON_HISTORY];
    uint8_t  mon_ram_hist [SYSMON_HISTORY];
    uint8_t  mon_gui_hist [SYSMON_HISTORY];
    uint8_t  mon_disk_hist[SYSMON_HISTORY];
    uint8_t  mon_hist_pos;
    bool     mon_hist_ready;

    /* Last drawn clock minute -- used by gui_tick to mark the
     * compositor dirty exactly once per minute even when nothing
     * else changed (otherwise the desktop would freeze the clock
     * on a static frame). */
    uint32_t last_clock_min;
} g;

/* ---- desktop activity trace --------------------------------------- *
 *
 * See gui.h for the public API. The actual emit goes through kprintf,
 * so it lands on serial (and the framebuffer console when text mode
 * is up). Keeping this as a single function means every trace site
 * emits the SAME prefix shape, which makes the log easy to grep:
 *
 *   [trace t=12345 pid=3] mouse down=(412,634) hit=launcher item=2
 *
 * The trace level is checked twice deliberately: gui_trace_logf() is
 * still safe to call when level==0 (it just returns), but call sites
 * also check gui_trace_level() inline so they can skip building any
 * expensive arguments (e.g. proc_lookup) when tracing is off. */
static int g_trace = GUI_TRACE_OFF;

void gui_trace_set(int level) {
    if (level < GUI_TRACE_OFF)     level = GUI_TRACE_OFF;
    if (level > GUI_TRACE_VERBOSE) level = GUI_TRACE_VERBOSE;
    if (level == g_trace) return;
    int old = g_trace;
    g_trace = level;
    /* Always log the transition itself, even if we just turned it
     * off -- the operator wants to see WHEN it stopped. */
    kprintf("[trace] level %d -> %d\n", old, level);
}

int gui_trace_level(void) { return g_trace; }

void gui_trace_logf(const char *fmt, ...) {
    if (g_trace == GUI_TRACE_OFF) return;
    struct proc *p = current_proc();
    int pid = p ? p->pid : -1;
    kprintf("[trace t=%lu pid=%d] ", (unsigned long)pit_ticks(), pid);
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
}

/* ---- helpers ------------------------------------------------------ */

static uint32_t color_mix(uint32_t a, uint32_t b, int num, int den) {
    if (den <= 0) return a;
    if (num < 0) num = 0;
    if (num > den) num = den;
    int ia = den - num;
    uint32_t ar = (a >> 16) & 0xFFu, ag = (a >> 8) & 0xFFu, ab = a & 0xFFu;
    uint32_t br = (b >> 16) & 0xFFu, bg = (b >> 8) & 0xFFu, bb = b & 0xFFu;
    uint32_t r = (ar * (uint32_t)ia + br * (uint32_t)num) / (uint32_t)den;
    uint32_t gch = (ag * (uint32_t)ia + bg * (uint32_t)num) / (uint32_t)den;
    uint32_t bl = (ab * (uint32_t)ia + bb * (uint32_t)num) / (uint32_t)den;
    return (r << 16) | (gch << 8) | bl;
}

static uint32_t argb(uint8_t a, uint32_t xrgb) {
    return ((uint32_t)a << 24) | (xrgb & 0x00FFFFFFu);
}

static void fill_vgradient(int x, int y, int w, int h,
                           uint32_t top, uint32_t bottom, int bands) {
    if (w <= 0 || h <= 0) return;
    if (bands < 1) bands = 1;
    if (bands > h) bands = h;
    for (int i = 0; i < bands; i++) {
        int y0 = y + (i * h) / bands;
        int y1 = y + ((i + 1) * h) / bands;
        gfx_fill_rect(x, y0, w, y1 - y0, color_mix(top, bottom, i, bands - 1));
    }
}

static void draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static int str_px(const char *s) {
    int n = 0;
    if (s) while (s[n]) n++;
    return n * 8;
}

static void draw_text_centered(int x, int y, int w, int h,
                               const char *s, uint32_t fg) {
    int tx = x + (w - str_px(s)) / 2;
    if (tx < x + 4) tx = x + 4;
    int ty = y + (h - 8) / 2;
    if (ty < y) ty = y;
    gfx_draw_text(tx, ty, s, fg, GFX_TRANSPARENT);
}

static void paint_glass_rect(int x, int y, int w, int h,
                             uint32_t fill, uint32_t glass,
                             uint32_t border, uint32_t accent) {
    if (w <= 0 || h <= 0) return;
    gfx_fill_rect(x, y, w, h, fill);
    if ((glass >> 24) != 0) {
        gfx_fill_rect_blend(x, y, w, h, glass);
    }
    gfx_draw_rect(x, y, w, h, border);
    if (w > 2) {
        gfx_fill_rect(x + 1, y, w - 2, 1, accent);
        gfx_fill_rect_blend(x + 1, y + 1, w - 2, 1, 0x28FFFFFFu);
    }
}

static int outer_w(const struct window *w) {
    return w->client_w + 2 * GUI_BORDER;
}
static int outer_h(const struct window *w) {
    return w->client_h + GUI_TITLE_BAR_H + GUI_BORDER;
}

static bool point_in_outer(const struct window *w, int px, int py) {
    return px >= w->x && py >= w->y &&
           px <  w->x + outer_w(w) &&
           py <  w->y + outer_h(w);
}
static bool point_in_title(const struct window *w, int px, int py) {
    return px >= w->x && py >= w->y &&
           px <  w->x + outer_w(w) &&
           py <  w->y + GUI_TITLE_BAR_H;
}
static bool point_in_client(const struct window *w, int px, int py) {
    int cx = w->x + GUI_BORDER;
    int cy = w->y + GUI_TITLE_BAR_H;
    return px >= cx && py >= cy &&
           px <  cx + w->client_w &&
           py <  cy + w->client_h;
}

/* Close button rect (within the title bar, top-right). Computed
 * fresh from the window's current x/y/w so dragging keeps it in
 * the right place. */
static void close_btn_rect(const struct window *w,
                           int *bx, int *by, int *bw, int *bh) {
    *bw = GUI_CLOSE_BTN_SIZE;
    *bh = GUI_CLOSE_BTN_SIZE;
    *bx = w->x + outer_w(w) - GUI_CLOSE_BTN_SIZE - GUI_CLOSE_BTN_PAD;
    *by = w->y + (GUI_TITLE_BAR_H - GUI_CLOSE_BTN_SIZE) / 2;
}
static bool point_in_close(const struct window *w, int px, int py) {
    int bx, by, bw, bh; close_btn_rect(w, &bx, &by, &bw, &bh);
    return px >= bx && py >= by && px < bx + bw && py < by + bh;
}

/* ---- desktop / taskbar geometry ----------------------------------- */

static int taskbar_top(void) { return (int)gfx_height() - GUI_TASKBAR_H; }

static int taskbar_tabs_x0(void) {
    int W = (int)gfx_width();
    int x = START_BTN_W + TASKBAR_SEARCH_W + 12 +
            TASKBAR_PIN_COUNT * TASKBAR_PIN_W + 10;
    if (x > W / 2) {
        x = START_BTN_W + TASKBAR_SEARCH_W + 12;
    }
    return x;
}

static bool point_in_taskbar(int px, int py) {
    (void)px;
    return g.desktop_mode && py >= taskbar_top();
}
static bool point_in_start_btn(int px, int py) {
    if (!g.desktop_mode) return false;
    int yt = taskbar_top();
    return px >= 0 && px < START_BTN_W && py >= yt && py < yt + GUI_TASKBAR_H;
}

/* Walk visible windows in z-order (topmost first) and return the tab
 * the cursor is over, or NULL. Tabs are laid out left -> right after
 * the start button. */
static struct window *taskbar_tab_at(int px, int py) {
    if (!g.desktop_mode) return 0;
    int yt = taskbar_top();
    if (py < yt || py >= yt + GUI_TASKBAR_H) return 0;
    int x0 = taskbar_tabs_x0();
    /* The order in the z-list is top-first; we want tabs left=oldest
     * so newer windows appear on the right. Collect into a stack
     * first to reverse. */
    struct window *stack[GUI_WINDOW_MAX]; int n = 0;
    for (struct window *w = g.z_top; w && n < GUI_WINDOW_MAX; w = w->z_next) {
        stack[n++] = w;
    }
    for (int i = n - 1; i >= 0; i--) {
        int x = x0;
        if (px >= x && px < x + TAB_W - TAB_PAD) return stack[i];
        x0 += TAB_W;
    }
    return 0;
}

static int taskbar_pin_at(int px, int py) {
    if (!g.desktop_mode) return -1;
    int yt = taskbar_top();
    if (py < yt || py >= yt + GUI_TASKBAR_H) return -1;
    int sx = START_BTN_W + 4;
    int x0 = sx + TASKBAR_SEARCH_W + 8;
    for (int i = 0; i < TASKBAR_PIN_COUNT; i++) {
        int ix = x0 + i * TASKBAR_PIN_W;
        if (px >= ix && px < ix + TASKBAR_PIN_W - 6) return i;
    }
    return -1;
}

static bool point_in_taskbar_search(int px, int py) {
    if (!g.desktop_mode) return false;
    int yt = taskbar_top();
    int sx = START_BTN_W + 4;
    return px >= sx && px < sx + TASKBAR_SEARCH_W &&
           py >= yt + 5 && py < yt + GUI_TASKBAR_H - 5;
}

static int desktop_icon_at(int px, int py) {
    if (!g.desktop_mode) return -1;
    int x = 16;
    int y = 58;
    for (int i = 0; i < 4; i++) {
        int iy = y + i * 74;
        if (px >= x && px < x + 76 &&
            py >= iy && py < iy + 68) return i;
    }
    return -1;
}

/* Launcher menu rect: rises from just above the start button. Width
 * is fixed; height grows with the item count. */
static void launcher_rect(int *mx, int *my, int *mw, int *mh) {
    int W = (int)gfx_width();
    int available_h = taskbar_top() - 12;
    *mw = LAUNCHER_W;
    if (*mw > W - 16) *mw = W - 16;
    *mh = 416;
    if (*mh > available_h) *mh = available_h;
    if (*mh < 292) *mh = 292;
    *mx = 8;
    *my = taskbar_top() - *mh - 8;
    if (*my < 6) *my = 6;
}
/* Returns item index 0..N-1 if the cursor is over a launcher entry,
 * -1 otherwise. */
static int launcher_item_at(int px, int py) {
    if (!g.menu_open) return -1;
    int mx, my, mw, mh; launcher_rect(&mx, &my, &mw, &mh);
    if (px < mx || px >= mx + mw) return -1;

    int n = launcher_count();
    int list_x = mx + LAUNCHER_PAD;
    int list_y = my + LAUNCHER_HEAD_H + 20;
    int list_w = LAUNCHER_LIST_W;
    int visible = n - 1;
    if (visible > 6) visible = 6;
    if (px >= list_x && px < list_x + list_w &&
        py >= list_y && py < list_y + visible * LAUNCHER_ITEM_H) {
        int idx = (py - list_y) / LAUNCHER_ITEM_H;
        if (idx >= 0 && idx < visible) return idx;
    }

    int grid_x = mx + 190;
    int grid_y = my + LAUNCHER_HEAD_H + 18;
    int grid_cols = 3;
    for (int i = 0; i < 9; i++) {
        int col = i % grid_cols;
        int row = i / grid_cols;
        int tx = grid_x + col * (LAUNCHER_TILE_W + 12);
        int ty = grid_y + row * (LAUNCHER_TILE_H + 12);
        if (px >= tx && px < tx + LAUNCHER_TILE_W &&
            py >= ty && py < ty + LAUNCHER_TILE_H) {
            if (i < n - 1) return i;
        }
    }

    int power_x = mx + LAUNCHER_PAD;
    int power_y = my + mh - 64;
    if (px >= power_x && px < power_x + LAUNCHER_PROFILE_W - 16 &&
        py >= power_y && py < power_y + 28) {
        return n - 1;
    }
    return -1;
}
static bool point_in_menu(int px, int py) {
    if (!g.menu_open) return false;
    int mx, my, mw, mh; launcher_rect(&mx, &my, &mw, &mh);
    return px >= mx && py >= my && px < mx + mw && py < my + mh;
}

static void gui_post_net_details(void) {
    char body[180];
    net_status_summary(body, sizeof(body));
    net_debug_dump();
    notify_post(ABI_NOTIFY_KIND_NET, g_my_ip ? NOTIFY_URG_INFO : NOTIFY_URG_ERR,
                "network", net_status_name(), body);
}

/* ---- M31 system-tray geometry ------------------------------------
 *
 * Pills are laid out RIGHT TO LEFT, starting from the right edge of
 * the taskbar minus TRAY_PAD. Each pill occupies its own width plus
 * TRAY_GAP. We expose two helpers:
 *
 *   tray_layout(rects, n)   compute up to n pill rects, returning
 *                           the actual count drawn this frame.
 *   point_in_tray(px,py,*idx)   hit-test, fills *idx with the pill
 *                               index from tray_layout.
 *
 * Pill 0 == clock, 1 == bell, 2 == win count, 3 == audio, 4 == disk,
 * 5 == net. The order in the rects array matches that. */

enum tray_pill {
    TRAY_PILL_CLOCK = 0,
    TRAY_PILL_BELL  = 1,
    TRAY_PILL_WIN   = 2,
    TRAY_PILL_AUD   = 3,
    TRAY_PILL_DISK  = 4,
    TRAY_PILL_NET   = 5,
    TRAY_PILL_COUNT = 6,
};

struct tray_rect { int x, y, w, h; bool present; };

static int tray_pill_width(enum tray_pill p) {
    switch (p) {
    case TRAY_PILL_CLOCK: return TRAY_W_CLOCK;
    case TRAY_PILL_BELL:  return TRAY_W_BELL;
    case TRAY_PILL_WIN:   return TRAY_W_WIN;
    case TRAY_PILL_AUD:   return TRAY_W_AUD;
    case TRAY_PILL_DISK:  return TRAY_W_DISK;
    case TRAY_PILL_NET:   return TRAY_W_NET;
    default:              return 0;
    }
}

static void tray_layout(struct tray_rect rects[TRAY_PILL_COUNT]) {
    int W  = (int)gfx_width();
    int yt = taskbar_top();
    int yp = yt + (GUI_TASKBAR_H - TRAY_PILL_H) / 2;
    int xr = W - TRAY_PAD;
    /* If the screen is narrow, drop pills from left to right
     * (least-important first) until what's left fits. Tabs need at
     * least START_BTN_W + 8 px of slack on the left. */
    int min_x_for_pills = START_BTN_W + 8;
    bool show[TRAY_PILL_COUNT] = { true, true, true, true, true, true };
    for (;;) {
        int total = 0;
        int n_show = 0;
        for (int i = 0; i < TRAY_PILL_COUNT; i++) {
            if (!show[i]) continue;
            total += tray_pill_width((enum tray_pill)i) + TRAY_GAP;
            n_show++;
        }
        if (n_show == 0 || (xr - total) >= min_x_for_pills) break;
        /* Drop the highest-index (least-important) visible pill. */
        for (int i = TRAY_PILL_COUNT - 1; i >= TRAY_PILL_CLOCK + 1; i--) {
            if (show[i]) { show[i] = false; break; }
        }
        if (n_show == 1) break;     /* never drop the clock */
    }

    /* Walk pills in order CLOCK..NET, placing them right-to-left. */
    for (int i = 0; i < TRAY_PILL_COUNT; i++) {
        if (!show[i]) {
            rects[i].present = false;
            rects[i].x = rects[i].y = rects[i].w = rects[i].h = 0;
            continue;
        }
        int w = tray_pill_width((enum tray_pill)i);
        xr -= w;
        rects[i].x = xr;
        rects[i].y = yp;
        rects[i].w = w;
        rects[i].h = TRAY_PILL_H;
        rects[i].present = true;
        xr -= TRAY_GAP;
    }
}

static int point_in_tray_pill(int px, int py) {
    if (!g.desktop_mode) return -1;
    if (py < taskbar_top()) return -1;
    struct tray_rect rects[TRAY_PILL_COUNT];
    tray_layout(rects);
    for (int i = 0; i < TRAY_PILL_COUNT; i++) {
        if (!rects[i].present) continue;
        if (px >= rects[i].x && px < rects[i].x + rects[i].w &&
            py >= rects[i].y && py < rects[i].y + rects[i].h) {
            return i;
        }
    }
    return -1;
}

/* ---- M31 notification-center panel geometry ---------------------- */

static void center_rect(int *x, int *y, int *w, int *h) {
    int W  = (int)gfx_width();
    int yt = taskbar_top();
    int top = 24;
    int height = (yt - top) - 8;
    if (height < CENTER_HEAD_H + CENTER_FOOT_H + CENTER_ITEM_H + 16) {
        height = CENTER_HEAD_H + CENTER_FOOT_H + CENTER_ITEM_H + 16;
    }
    *x = W - CENTER_W - 8;
    *y = top;
    *w = CENTER_W;
    *h = height;
}

static bool point_in_center(int px, int py) {
    if (!g.center_open) return false;
    int x, y, w, h; center_rect(&x, &y, &w, &h);
    return px >= x && py >= y && px < x + w && py < y + h;
}

/* The "Clear all" footer button. Returns true if the cursor is over
 * the button; updates the rect-out arguments either way. */
static bool center_clear_btn_rect(int *bx, int *by, int *bw, int *bh) {
    int x, y, w, h; center_rect(&x, &y, &w, &h);
    *bw = 96;
    *bh = CENTER_FOOT_H - 10;
    *bx = x + w - *bw - 10;
    *by = y + h - CENTER_FOOT_H + 5;
    return true;
}

/* ---- M31 toast geometry ------------------------------------------ */

static int toast_height(void) {
    /* If the body is non-empty, draw the taller variant. */
    return g.toast_body[0] ? TOAST_H_FULL : TOAST_H_TITLE;
}
static void toast_rect(int *x, int *y, int *w, int *h) {
    int W  = (int)gfx_width();
    int yt = taskbar_top();
    *w = TOAST_W;
    *h = toast_height();
    *x = W - TOAST_W - TOAST_MARGIN;
    *y = yt - *h - TOAST_MARGIN;
    if (*y < 8) *y = 8;
}

/* ---- M31 wall-clock helpers --------------------------------------- *
 *
 * We don't have a real-time clock subsystem yet; pit_ticks gives us
 * ms since boot. Showing "uptime in HH:MM:SS" is more useful as a
 * heartbeat than a fake wall-clock time would be, and it's never a
 * lie about how long the box has been alive. */
static uint64_t now_uptime_ms(void) {
    uint32_t hz = pit_hz();
    if (hz == 0) return 0;
    return (pit_ticks() * 1000ull) / (uint64_t)hz;
}
static void format_uptime(char *out, size_t cap) {
    uint64_t ms = now_uptime_ms();
    uint32_t total_s = (uint32_t)(ms / 1000ull);
    uint32_t h = (total_s / 3600u);
    uint32_t m = (total_s / 60u) % 60u;
    uint32_t s = (total_s) % 60u;
    /* "HH:MM:SS" -- ksnprintf gives us the zero-pad. */
    ksnprintf(out, cap, "%02u:%02u:%02u",
              (unsigned)h, (unsigned)m, (unsigned)s);
}

/* ---- deferred app launch queue ----------------------------------- *
 *
 * Mouse-IRQ context can't safely call proc_create_from_elf -- it would
 * touch the proc table while we're sitting on the IRQ stack with the
 * cursor's CR3 active. Instead, we enqueue the program path here and
 * let gui_tick() (which runs on pid 0 from the idle loop) drain the
 * queue and actually spawn. */
/* Copy up to cap-1 bytes of src into dst and NUL-terminate. Returns
 * the number of bytes copied (NOT counting the terminator). */
static size_t copy_bounded(char *dst, const char *src, size_t cap) {
    if (cap == 0) return 0;
    size_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
    return i;
}

int gui_launch_enqueue_arg_profile_caps(const char *path, const char *arg,
                                        const char *sandbox,
                                        const char *caps) {
    if (!path || !g.ready) return -1;
    uint8_t next = (uint8_t)((g.launch_head + 1u) % LAUNCH_QUEUE_MAX);
    if (next == g.launch_tail) {
        gui_trace_logf("launch_enqueue '%s' DROPPED (queue full)", path);
        return -1;
    }
    struct launch_entry *e = &g.launch_q[g.launch_head];
    copy_bounded(e->path, path, sizeof(e->path));
    copy_bounded(e->sandbox, sandbox ? sandbox : "", sizeof(e->sandbox));
    copy_bounded(e->caps,    caps    ? caps    : "", sizeof(e->caps));
    if (arg) {
        copy_bounded(e->arg, arg, sizeof(e->arg));
        e->has_arg = true;
    } else {
        e->arg[0] = '\0';
        e->has_arg = false;
    }
    g.launch_head = next;
    gui_trace_logf("launch_enqueue '%s'%s%s sandbox='%s' caps='%s' (head=%u tail=%u)",
                   e->path, e->has_arg ? " arg=" : "",
                   e->has_arg ? e->arg : "",
                   e->sandbox, e->caps,
                   (unsigned)g.launch_head, (unsigned)g.launch_tail);
    return 0;
}

int gui_launch_enqueue_arg_profile(const char *path, const char *arg,
                                   const char *sandbox) {
    return gui_launch_enqueue_arg_profile_caps(path, arg, sandbox, 0);
}

/* Back-compat wrapper -- inherits parent's caps untouched (no profile). */
int gui_launch_enqueue_arg(const char *path, const char *arg) {
    return gui_launch_enqueue_arg_profile_caps(path, arg, 0, 0);
}

/* M34D variant: enqueue with both profile and declared caps. */
static void launch_enqueue_with_profile_caps(const char *path,
                                             const char *profile,
                                             const char *caps) {
    (void)gui_launch_enqueue_arg_profile_caps(path, 0, profile, caps);
}

static void shell_launch_path(const char *path) {
    if (!path || !path[0]) return;
    launch_enqueue_with_profile_caps(path,
                                     gui_launcher_sandbox_for_path(path),
                                     gui_launcher_caps_for_path(path));
}

static void shell_launch_desktop_icon(int icon) {
    switch (icon) {
    case 0: shell_launch_path("/bin/gui_files");    break; /* This PC */
    case 1: shell_launch_path("/bin/gui_about");    break; /* TobyOS */
    case 2:
        notify_post(ABI_NOTIFY_KIND_USER, NOTIFY_URG_INFO,
                    "shell", "Recycle Bin",
                    "Recycle Bin is now a real shell target; file deletion plumbing is next.");
        break;
    case 3: shell_launch_path("/bin/gui_settings"); break; /* Control Panel */
    default: break;
    }
}

static void shell_launch_pin(int pin) {
    switch (pin) {
    case 0: shell_launch_path("/bin/gui_files");    break;
    case 1:
        notify_post(ABI_NOTIFY_KIND_USER, NOTIFY_URG_INFO,
                    "shell", "Toby Browser",
                    "Browser shell tile is wired; TCP/HTTP UI app is a future layer.");
        break;
    case 2: shell_launch_path("/bin/gui_term");     break;
    case 3: shell_launch_path("/bin/gui_widgets");  break;
    case 4: shell_launch_path("/bin/gui_settings"); break;
    case 5:
        g.widgets_open = !g.widgets_open;
        notify_post(ABI_NOTIFY_KIND_USER, NOTIFY_URG_INFO,
                    "shell", g.widgets_open ? "Widgets open" : "Widgets hidden",
                    "Quick Settings and System Monitor are compositor shell panels.");
        break;
    case 6:
        notify_post(ABI_NOTIFY_KIND_USER, NOTIFY_URG_INFO,
                    "mail", "Mail",
                    "Mail tile is reserved for the first networked user app.");
        break;
    default: break;
    }
}

static int shell_widgets_quick_at(int px, int py) {
    if (!g.widgets_open) return -1;
    int W = (int)gfx_width();
    int desk_h = (int)gfx_height() - GUI_TASKBAR_H;
    if (W < 900 || desk_h < 520) return -1;
    int rw = W >= 1120 ? 282 : 238;
    int x = W - rw - 14;
    int y = 58 + 142;
    int tw = (rw - 34) / 3;
    for (int i = 0; i < 6; i++) {
        int col = i % 3;
        int row = i / 3;
        int tx = x + 10 + col * (tw + 8);
        int ty = y + 36 + row * 48;
        if (px >= tx && px < tx + tw && py >= ty && py < ty + 42) return i;
    }
    return -1;
}

static bool point_in_shell_widgets(int px, int py) {
    if (!g.widgets_open) return false;
    int W = (int)gfx_width();
    int desk_h = (int)gfx_height() - GUI_TASKBAR_H;
    if (W < 900 || desk_h < 520) return false;
    int rw = W >= 1120 ? 282 : 238;
    int x = W - rw - 14;
    int y = 58;
    int h = 128 + 14 + 154 + 16 + 200;
    return px >= x && px < x + rw && py >= y && py < y + h;
}

static void shell_toggle_quick(int tile) {
    const char *title = "Quick Settings";
    const char *body = "Shell toggle updated.";
    switch (tile) {
    case 0:
        gui_post_net_details();
        return;
    case 1: g.quick_bt = !g.quick_bt; title = "Bluetooth"; break;
    case 2: g.quick_night = !g.quick_night; title = "Night Light"; break;
    case 3: g.quick_nixie = !g.quick_nixie; title = "Nixie Glow"; break;
    case 4: g.quick_airplane = !g.quick_airplane; title = "Airplane mode"; break;
    case 5: g.quick_focus = !g.quick_focus; title = "Focus assist"; break;
    default: break;
    }
    notify_post(ABI_NOTIFY_KIND_USER, NOTIFY_URG_INFO,
                "settings", title, body);
}

/* ---- dynamic launcher registry (milestone 16) --------------------- */

int gui_launcher_register_with_profile_caps(const char *label,
                                            const char *path,
                                            const char *sandbox,
                                            const char *caps) {
    if (!label || !path) return -1;
    if (g_launcher_user_count >= GUI_LAUNCHER_USER_MAX) {
        gui_trace_logf("launcher_register '%s' DROPPED (user slice full)", label);
        return -1;
    }
    int s = g_launcher_user_count;
    copy_bounded(g_launcher_user_label  [s], label,
                 sizeof(g_launcher_user_label[s]));
    copy_bounded(g_launcher_user_path   [s], path,
                 sizeof(g_launcher_user_path [s]));
    copy_bounded(g_launcher_user_sandbox[s], sandbox ? sandbox : "",
                 sizeof(g_launcher_user_sandbox[s]));
    copy_bounded(g_launcher_user_caps   [s], caps ? caps : "",
                 sizeof(g_launcher_user_caps[s]));
    g_launcher_user_count++;
    g.dirty = true;
    gui_trace_logf("launcher_register '%s' -> %s sandbox='%s' caps='%s' (slot=%d)",
                   label, path, sandbox ? sandbox : "",
                   caps ? caps : "", s);
    return 0;
}

int gui_launcher_register_with_profile(const char *label, const char *path,
                                       const char *sandbox) {
    return gui_launcher_register_with_profile_caps(label, path, sandbox, 0);
}

int gui_launcher_register(const char *label, const char *path) {
    return gui_launcher_register_with_profile_caps(label, path, 0, 0);
}

const char *gui_launcher_sandbox_for_path(const char *path) {
    if (!path) return 0;
    for (int i = 0; i < g_launcher_user_count; i++) {
        if (strcmp(g_launcher_user_path[i], path) == 0) {
            return g_launcher_user_sandbox[i][0]
                       ? g_launcher_user_sandbox[i] : 0;
        }
    }
    return 0;
}

const char *gui_launcher_caps_for_path(const char *path) {
    if (!path) return 0;
    for (int i = 0; i < g_launcher_user_count; i++) {
        if (strcmp(g_launcher_user_path[i], path) == 0) {
            return g_launcher_user_caps[i][0]
                       ? g_launcher_user_caps[i] : 0;
        }
    }
    return 0;
}

void gui_launcher_reset_user(void) {
    if (g_launcher_user_count == 0) return;
    g_launcher_user_count = 0;
    /* Wipe the backing memory so stale bytes don't leak into the next
     * registration round if a short label replaces a longer one. */
    memset(g_launcher_user_label,   0, sizeof(g_launcher_user_label));
    memset(g_launcher_user_path,    0, sizeof(g_launcher_user_path));
    memset(g_launcher_user_sandbox, 0, sizeof(g_launcher_user_sandbox));
    memset(g_launcher_user_caps,    0, sizeof(g_launcher_user_caps));
    g.dirty = true;
}

static void z_unlink(struct window *w) {
    if (w->z_prev) w->z_prev->z_next = w->z_next;
    if (w->z_next) w->z_next->z_prev = w->z_prev;
    if (g.z_top == w) g.z_top = w->z_next;
    w->z_prev = w->z_next = 0;
}

static void z_push_front(struct window *w) {
    w->z_prev = 0;
    w->z_next = g.z_top;
    if (g.z_top) g.z_top->z_prev = w;
    g.z_top = w;
}

static void z_raise(struct window *w) {
    if (g.z_top == w) return;
    z_unlink(w);
    z_push_front(w);
    g.dirty = true;
    /* M27E: a z-order change can re-expose any pixel on screen --
     * we have to present everything. */
    gui_invalidate_full();
}

/* Find the topmost window that contains the screen point. NULL if no
 * window does. */
static struct window *window_at(int px, int py) {
    for (struct window *w = g.z_top; w; w = w->z_next) {
        if (point_in_outer(w, px, py)) return w;
    }
    return 0;
}

/* ---- event queue (per window) ------------------------------------- */

/* Push from IRQ context. If the ring is full we drop the OLDEST event
 * to keep input fresh -- a stuck window shouldn't be able to back-
 * pressure cursor updates. */
static void enqueue_event(struct window *w, int type, int x, int y,
                          uint8_t button, uint8_t key) {
    uint8_t next = (uint8_t)((w->ev_head + 1u) % GUI_EVENT_RING);
    if (next == w->ev_tail) {
        /* full -- drop oldest */
        w->ev_tail = (uint8_t)((w->ev_tail + 1u) % GUI_EVENT_RING);
    }
    struct gui_event *e = &w->ev[w->ev_head];
    e->type   = type;
    e->x      = x;
    e->y      = y;
    e->button = button;
    e->key    = key;
    e->_pad[0] = e->_pad[1] = 0;
    w->ev_head = next;
}

/* Forward decls -- needed because on_mouse_event() (below) calls
 * recompute_active() (the launcher's "Exit Desktop" entry tears the
 * compositor down). */
static void recompute_active(void);

/* ---- mouse callback (IRQ) ----------------------------------------- */

static void on_mouse_event(int dx, int dy, uint8_t buttons) {
    if (!g.ready) return;

    int W = (int)gfx_width(), H = (int)gfx_height();
    int old_x = g.cur_x, old_y = g.cur_y;

    /*
     * Pointer speed / acceleration.
     *
     * Raw USB HID mouse deltas feel slow compared to Windows because
     * Windows applies pointer speed and acceleration. This does not fix
     * USB report burstiness, but it makes each received report move the
     * cursor farther.
     */
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    int maxa = ax > ay ? ax : ay;

    int mult = 2;

    if (maxa >= 4)
        mult = 3;
    if (maxa >= 10)
        mult = 4;
    if (maxa >= 24)
        mult = 5;

    int sdx = dx * mult;
    int sdy = dy * mult;

    int nx = g.cur_x + sdx;
    int ny = g.cur_y + sdy;

    if (nx < 0)      nx = 0;
    if (ny < 0)      ny = 0;
    if (nx >= W)     nx = W - 1;
    if (ny >= H)     ny = H - 1;

    bool moved = (nx != old_x) || (ny != old_y);
    g.cur_x = nx;
    g.cur_y = ny;
    /* M27E: hint just the cursor sprite bbox (old + new). 12x19 is the
     * cursor size from gfx.c. The hint lives until the next compositor
     * pass consumes it. */
    if (moved) {
        gui_invalidate_rect(old_x, old_y, 12, 19);
        gui_invalidate_rect(nx,    ny,    12, 19);
        if (g.active) {
            gfx_cursor_overlay_move(nx, ny);
        }
    }

    /* Detect button transitions: compare THIS packet's buttons against
     * the immediately-previous packet's buttons (g.cur_buttons). The
     * earlier "shuffle into prev_buttons" version was off-by-one and
     * re-fired went_down on every mouse-move that occurred while a
     * button was held -- which made the desktop feel like it was
     * eating clicks (each move re-z_raised, re-enqueued MOUSE_DOWN,
     * re-armed drag, etc.). */
    uint8_t prev   = g.cur_buttons;
    g.prev_buttons = prev;
    g.cur_buttons  = buttons;
    bool went_down = (buttons & ~prev) != 0;
    bool went_up   = (~buttons & prev) != 0;
    (void)went_up;
    if (went_down || went_up) {
        gui_trace_logf("mouse edge: prev=0x%02x -> cur=0x%02x (down=%d up=%d) "
                       "at (%d,%d)",
                       (unsigned)prev, (unsigned)buttons,
                       (int)went_down, (int)went_up, nx, ny);
    }

    /* If the GUI isn't even displayed (no windows AND no desktop), do
     * nothing -- input goes to console_tick / shell instead. */
    if (!g.active) return;

    /* Drag in progress -- just slide the window. */
    if (g.drag_win) {
        if (moved) {
            int prev_x = g.drag_win->x;
            int prev_y = g.drag_win->y;
            g.drag_win->x = nx - g.drag_dx;
            g.drag_win->y = ny - g.drag_dy;
            /* Clamp so the title bar can never disappear under the
             * taskbar (would make the window undraggable). */
            int max_y = (int)gfx_height() - GUI_TASKBAR_H - GUI_TITLE_BAR_H;
            if (g.drag_win->y < 0)     g.drag_win->y = 0;
            if (g.drag_win->y > max_y) g.drag_win->y = max_y;
            g.dirty = true;
            /* M27E: hint both the OLD and NEW window positions so the
             * region the window USED to occupy gets the wallpaper +
             * overlapped windows behind it presented correctly. */
            int ow = outer_w(g.drag_win), oh = outer_h(g.drag_win);
            gui_invalidate_rect(prev_x,        prev_y,        ow, oh);
            gui_invalidate_rect(g.drag_win->x, g.drag_win->y, ow, oh);
        }
        if (!buttons) {
            struct window *w = g.drag_win;
            int cx = nx - (w->x + GUI_BORDER);
            int cy = ny - (w->y + GUI_TITLE_BAR_H);
            enqueue_event(w, GUI_EV_MOUSE_UP, cx, cy, prev, 0);
            g.drag_win = 0;
        }
        return;
    }

    /* ---- desktop-chrome hit testing (taskbar / menu / close-X) ----
     *
     * Order matters: launcher menu first (it floats above everything
     * else), then taskbar, then close-buttons on title bars, then the
     * normal per-window dispatch below. Mouse-down + mouse-up are the
     * only events these zones consume; movement still falls through so
     * the cursor keeps repainting smoothly. */
    if (g.desktop_mode && went_down) {
        /* Click inside the launcher menu? */
        if (g.menu_open) {
            int item = launcher_item_at(nx, ny);
            if (item >= 0) {
                struct launcher_item li;
                bool ok = launcher_resolve(item, &li);
                gui_trace_logf("mouse down=(%d,%d) hit=launcher item=%d label='%s'",
                               nx, ny, item,
                               ok && li.label ? li.label : "(null)");
                if (!ok) {
                    /* Shouldn't happen -- belt-and-suspenders */
                } else if (li.path) {
                    /* Milestone 18: user-slice entries may carry a
                     * sandbox profile read from the .app descriptor.
                     * System entries (li belongs to g_launcher_sys)
                     * launch under the default inherited caps -- they
                     * need settings/term/GUI access to be useful.
                     * M34D: user-slice entries may ALSO carry a
                     * declared CAPS list; the launch queue carries it
                     * through to proc_spec.declared_caps so the spawn
                     * narrows past the profile. */
                    const char *prof = gui_launcher_sandbox_for_path(li.path);
                    const char *caps = gui_launcher_caps_for_path   (li.path);
                    launch_enqueue_with_profile_caps(li.path, prof, caps);
                } else {
                    /* "Logout" -- terminate the session. The session
                     * manager SIGTERMs every process tagged with the
                     * current session id and then re-spawns
                     * /bin/login on the next service_tick(). The
                     * desktop itself stays up (compositor + taskbar)
                     * because it's owned by pid 0, which has no
                     * session tag. */
                    gui_trace_logf("launcher: LOGOUT selected");
                    session_logout();
                }
                g.menu_open = false;
                g.dirty = true;
                return;
            }
            if (!point_in_menu(nx, ny) && !point_in_start_btn(nx, ny)) {
                /* Click outside menu dismisses it (and falls through
                 * to the normal handlers so the click can also raise/
                 * focus a window). */
                gui_trace_logf("mouse down=(%d,%d) hit=outside-menu (dismiss)", nx, ny);
                g.menu_open = false;
                g.dirty = true;
            }
        }
        /* Start button toggles the menu. */
        if (point_in_start_btn(nx, ny)) {
            g.menu_open = !g.menu_open;
            gui_trace_logf("mouse down=(%d,%d) hit=start-button menu_open=%d",
                           nx, ny, (int)g.menu_open);
            g.dirty = true;
            return;
        }

        /* M31: tray pill click. The bell pill toggles the
         * notification center; the other pills are status-only and
         * just dismiss any open menu. */
        int pill = point_in_tray_pill(nx, ny);
        if (pill >= 0) {
            if (pill == TRAY_PILL_BELL) {
                g.center_open = !g.center_open;
                gui_trace_logf("mouse down=(%d,%d) hit=tray-bell center_open=%d",
                               nx, ny, (int)g.center_open);
            } else if (pill == TRAY_PILL_NET) {
                gui_post_net_details();
                gui_trace_logf("mouse down=(%d,%d) hit=tray-net status=%s",
                               nx, ny, net_status_name());
            } else {
                gui_trace_logf("mouse down=(%d,%d) hit=tray-pill idx=%d",
                               nx, ny, pill);
            }
            g.menu_open = false;
            g.dirty = true;
            return;
        }

        /* M31: notification-center clicks. Clicking the "Clear all"
         * footer dismisses every entry in the ring; clicking
         * anywhere else inside the panel just keeps it open and
         * gets swallowed (so the click doesn't fall through to
         * windows behind it). Clicking outside closes the panel. */
        if (g.center_open) {
            int cbx, cby, cbw, cbh;
            center_clear_btn_rect(&cbx, &cby, &cbw, &cbh);
            if (nx >= cbx && nx < cbx + cbw &&
                ny >= cby && ny < cby + cbh) {
                gui_trace_logf("mouse down=(%d,%d) hit=center-clear-all", nx, ny);
                notify_dismiss_all();
                g.dirty = true;
                return;
            }
            if (point_in_center(nx, ny)) {
                /* Eat the click -- center is "modal-ish". */
                g.dirty = true;
                return;
            }
            /* Outside the panel and not on the bell -> close it. */
            if (!point_in_taskbar(nx, ny)) {
                gui_trace_logf("mouse down=(%d,%d) hit=outside-center -> close",
                               nx, ny);
                g.center_open = false;
                g.dirty = true;
                /* Fall through so the click still raises/focuses
                 * whatever was below the panel. */
            }
        }

        /* M36: right-side shell widgets. Quick Settings tiles update
         * shell state or post real kernel status. Other clicks inside
         * the stack are swallowed because the widgets are compositor
         * chrome layered above app windows. */
        if (g.widgets_open) {
            int quick = shell_widgets_quick_at(nx, ny);
            if (quick >= 0) {
                shell_toggle_quick(quick);
                gui_trace_logf("mouse down=(%d,%d) hit=quick-settings tile=%d",
                               nx, ny, quick);
                g.menu_open = false;
                g.dirty = true;
                return;
            }
            if (point_in_shell_widgets(nx, ny)) {
                gui_trace_logf("mouse down=(%d,%d) hit=shell-widgets", nx, ny);
                g.menu_open = false;
                g.dirty = true;
                return;
            }
        }

        /* M31: clicking a toast dismisses it (and the underlying
         * notification). Toast lives entirely in compositor space,
         * so we hit-test by recomputing its rect. */
        if (g.toast_id != 0) {
            int tx, ty, tw, th; toast_rect(&tx, &ty, &tw, &th);
            if (nx >= tx && nx < tx + tw &&
                ny >= ty && ny < ty + th) {
                gui_trace_logf("mouse down=(%d,%d) hit=toast id=%u dismiss",
                               nx, ny, (unsigned)g.toast_id);
                notify_dismiss(g.toast_id);
                g.toast_id = 0;
                g.dirty = true;
                return;
            }
        }

        /* Taskbar tab raises the corresponding window. */
        if (point_in_taskbar(nx, ny)) {
            if (point_in_taskbar_search(nx, ny)) {
                notify_post(ABI_NOTIFY_KIND_USER, NOTIFY_URG_INFO,
                            "search", "Search",
                            "Search UI is wired as a shell target; indexing comes next.");
                g.menu_open = false;
                g.dirty = true;
                return;
            }
            int pin = taskbar_pin_at(nx, ny);
            if (pin >= 0) {
                shell_launch_pin(pin);
                gui_trace_logf("mouse down=(%d,%d) hit=taskbar-pin idx=%d",
                               nx, ny, pin);
                g.menu_open = false;
                g.dirty = true;
                return;
            }
            struct window *t = taskbar_tab_at(nx, ny);
            gui_trace_logf("mouse down=(%d,%d) hit=taskbar tab_wid=%d tab_pid=%d",
                           nx, ny, t ? t->wid : 0, t ? t->owner_pid : -1);
            if (t) z_raise(t);
            g.menu_open = false;
            g.dirty = true;
            return;
        }
    }

    /* Window-level click handling. */
    struct window *under = window_at(nx, ny);

    if (went_down) {
        if (under) {
            z_raise(under);
            /* Close button "X" -- ask the kernel to terminate the
             * owning process; the proc's fd cleanup will then close
             * this window. We don't tear the window down inline because
             * the proc may still be holding the window fd. */
            if (point_in_close(under, nx, ny)) {
                gui_trace_logf("mouse down=(%d,%d) hit=close-X wid=%d "
                               "owner_pid=%d -> SIGINT",
                               nx, ny, under->wid, under->owner_pid);
                if (under->owner_pid > 0) {
                    signal_send_to_pid(under->owner_pid, SIGINT);
                }
                g.dirty = true;
                return;
            }
            if (point_in_title(under, nx, ny)) {
                gui_trace_logf("mouse down=(%d,%d) hit=title wid=%d "
                               "owner_pid=%d (drag start)",
                               nx, ny, under->wid, under->owner_pid);
                g.drag_win = under;
                g.drag_dx  = nx - under->x;
                g.drag_dy  = ny - under->y;
            } else if (point_in_client(under, nx, ny)) {
                int cx = nx - (under->x + GUI_BORDER);
                int cy = ny - (under->y + GUI_TITLE_BAR_H);
                gui_trace_logf("mouse down=(%d,%d) hit=client wid=%d "
                               "owner_pid=%d cx=%d cy=%d",
                               nx, ny, under->wid, under->owner_pid, cx, cy);
                enqueue_event(under, GUI_EV_MOUSE_DOWN, cx, cy, buttons, 0);
            }
            g.dirty = true;
        } else if (g.desktop_mode) {
            int icon = desktop_icon_at(nx, ny);
            if (icon >= 0) {
                shell_launch_desktop_icon(icon);
                gui_trace_logf("mouse down=(%d,%d) hit=desktop-icon idx=%d",
                               nx, ny, icon);
                g.menu_open = false;
                g.dirty = true;
                return;
            }
            gui_trace_logf("mouse down=(%d,%d) hit=bare-desktop", nx, ny);
        }
        /* Click on bare desktop just dismisses any open menu. */
    } else if (went_up && under && point_in_client(under, nx, ny)) {
        int cx = nx - (under->x + GUI_BORDER);
        int cy = ny - (under->y + GUI_TITLE_BAR_H);
        enqueue_event(under, GUI_EV_MOUSE_UP, cx, cy, buttons, 0);
    } else if (moved && under && point_in_client(under, nx, ny)) {
        int cx = nx - (under->x + GUI_BORDER);
        int cy = ny - (under->y + GUI_TITLE_BAR_H);
        enqueue_event(under, GUI_EV_MOUSE_MOVE, cx, cy, buttons, 0);
    }
}

/* ---- compositor (idle context) ------------------------------------ */

/* Draw the title-bar close button. The "X" is two diagonal strokes
 * built out of single-pixel rects -- avoids depending on a glyph and
 * looks crisp at 14 px. */
static void paint_close_button(const struct window *w) {
    const struct theme_palette *t = theme_active();
    int bx, by, bw, bh; close_btn_rect(w, &bx, &by, &bw, &bh);
    /* Hot if the cursor is over it. */
    bool hot = (g.cur_x >= bx && g.cur_x < bx + bw &&
                g.cur_y >= by && g.cur_y < by + bh);
    gfx_fill_rect(bx, by, bw, bh, hot ? t->close_bg_hot : t->close_bg);
    gfx_draw_rect(bx, by, bw, bh, hot ? t->close_fg : t->win_border);
    if (!hot && bw > 4) {
        gfx_fill_rect(bx + 2, by, bw - 4, 1, t->accent_magenta);
    }
    /* Two diagonals. */
    int pad = 3;
    for (int i = 0; i < bw - 2 * pad; i++) {
        gfx_set_pixel(bx + pad + i, by + pad + i,             t->close_fg);
        gfx_set_pixel(bx + bw - pad - 1 - i, by + pad + i,    t->close_fg);
    }
}

static void paint_window_shadow(const struct window *w, bool focused) {
    const struct theme_palette *t = theme_active();
    int ow = outer_w(w);
    int oh = outer_h(w);
    if ((t->win_shadow_deep >> 24) != 0) {
        gfx_fill_rect_blend(w->x + 8, w->y + 10, ow, oh, t->win_shadow_deep);
    }
    if ((t->win_shadow >> 24) != 0) {
        gfx_fill_rect_blend(w->x + 3, w->y + 4, ow + 6, oh + 6, t->win_shadow);
    }
    if (focused) {
        gfx_fill_rect_blend(w->x - 1, w->y - 1, ow + 2, 1,
                            argb(0x80, t->accent_cyan));
        gfx_fill_rect_blend(w->x - 1, w->y, 1, oh,
                            argb(0x30, t->accent_cyan));
        gfx_fill_rect_blend(w->x + ow, w->y, 1, oh,
                            argb(0x30, t->accent_magenta));
    }
}

static void paint_window_glyph(int x, int y, uint32_t a, uint32_t b) {
    gfx_fill_rect(x,     y,     4, 4, a);
    gfx_fill_rect(x + 6, y,     4, 4, b);
    gfx_fill_rect(x,     y + 6, 4, 4, b);
    gfx_fill_rect(x + 6, y + 6, 4, 4, a);
}

/* ---- M36 shell prototype primitives -------------------------------
 *
 * These helpers deliberately stay compositor-local for the first pass:
 * they draw the "Windows 10 inspired, TobyOS-branded cyberpunk shell"
 * as cheap framebuffer primitives without creating extra processes or
 * changing the real window-manager contract. Future work can graduate
 * each mock panel into real shell surfaces once the compositor has a
 * retained scene graph and richer hit-testing. */

static void paint_soft_panel(int x, int y, int w, int h,
                             uint32_t fill, uint32_t glass,
                             uint32_t border, uint32_t accent) {
    if (w <= 0 || h <= 0) return;
    gfx_fill_rect_blend(x + 7, y + 9, w, h, 0x56000000u);
    paint_glass_rect(x, y, w, h, fill, glass, border, accent);
    if (w > 6 && h > 6) {
        gfx_fill_rect_blend(x + 2, y + 2, w - 4, 1, 0x20FFFFFFu);
        gfx_fill_rect_blend(x + 2, y + h - 3, w - 4, 1, argb(0x20, accent));
    }
}

static void paint_toby_hex_logo(int x, int y, int s,
                                uint32_t hot, uint32_t dim) {
    if (s < 12) s = 12;
    int cx = x + s / 2;
    int p0x = cx,          p0y = y;
    int p1x = x + s - 1,   p1y = y + s / 4;
    int p2x = x + s - 1,   p2y = y + (s * 3) / 4;
    int p3x = cx,          p3y = y + s - 1;
    int p4x = x,           p4y = y + (s * 3) / 4;
    int p5x = x,           p5y = y + s / 4;

    draw_line(p0x, p0y, p1x, p1y, hot);
    draw_line(p1x, p1y, p2x, p2y, hot);
    draw_line(p2x, p2y, p3x, p3y, hot);
    draw_line(p3x, p3y, p4x, p4y, hot);
    draw_line(p4x, p4y, p5x, p5y, hot);
    draw_line(p5x, p5y, p0x, p0y, hot);

    int my = y + s / 2;
    draw_line(cx, my, p0x, p0y, dim);
    draw_line(cx, my, p2x, p2y, dim);
    draw_line(cx, my, p4x, p4y, dim);
    draw_line(cx, my, cx, p3y, dim);
    gfx_fill_rect(cx - 1, my - 1, 3, 3, hot);
}

static void paint_icon_symbol(int x, int y, int kind,
                              uint32_t fg, uint32_t dim) {
    switch (kind) {
    case 0: /* monitor */
        gfx_draw_rect(x + 2, y + 3, 24, 16, fg);
        gfx_fill_rect(x + 5, y + 6, 18, 1, dim);
        gfx_fill_rect(x + 12, y + 20, 4, 4, fg);
        gfx_fill_rect(x + 7, y + 24, 14, 2, fg);
        break;
    case 1: /* folder */
        gfx_draw_rect(x + 1, y + 8, 26, 17, fg);
        gfx_fill_rect(x + 3, y + 5, 10, 4, fg);
        gfx_fill_rect(x + 4, y + 12, 20, 1, dim);
        break;
    case 2: /* bin */
        gfx_draw_rect(x + 6, y + 8, 16, 19, fg);
        gfx_fill_rect(x + 4, y + 5, 20, 2, fg);
        gfx_fill_rect(x + 10, y + 12, 1, 10, dim);
        gfx_fill_rect(x + 17, y + 12, 1, 10, dim);
        break;
    default: /* gear-ish control panel */
        gfx_draw_rect(x + 7, y + 7, 14, 14, fg);
        gfx_draw_rect(x + 11, y + 11, 6, 6, dim);
        gfx_fill_rect(x + 13, y + 2, 2, 6, fg);
        gfx_fill_rect(x + 13, y + 20, 2, 6, fg);
        gfx_fill_rect(x + 2, y + 13, 6, 2, fg);
        gfx_fill_rect(x + 20, y + 13, 6, 2, fg);
        break;
    }
}

static void paint_desktop_icon(int x, int y, const char *label, int kind) {
    const struct theme_palette *t = theme_active();
    bool hot = (g.cur_x >= x && g.cur_x < x + 64 &&
                g.cur_y >= y && g.cur_y < y + 66);
    paint_soft_panel(x + 5, y, 48, 42,
                     hot ? t->center_item_hot : t->panel,
                     argb(hot ? 0x64 : 0x30, t->panel),
                     hot ? t->border_cyan : t->tray_border,
                     kind == 2 ? t->glow_orange : t->glow_cyan);
    paint_icon_symbol(x + 15, y + 7, kind, t->glow_orange, t->glow_cyan);
    int tx = x + (58 - str_px(label)) / 2;
    if (tx < x) tx = x;
    gfx_draw_text(tx, y + 48, label, t->text_primary, GFX_TRANSPARENT);
}

static void paint_desktop_icons(int top_y) {
    int x = 16;
    int y = top_y;
    paint_desktop_icon(x, y,       "This PC",       0);
    paint_desktop_icon(x, y + 74,  "TobyOS",        1);
    paint_desktop_icon(x, y + 148, "Recycle Bin",   2);
    paint_desktop_icon(x, y + 222, "Control Panel", 3);
}

static void paint_city_wallpaper(int W, int desk_h,
                                 const struct theme_palette *t) {
    if (desk_h < 180) return;
    int horizon = desk_h * 60 / 100;
    int base = desk_h - 54;
    if (base < horizon + 30) base = horizon + 30;

    static const int widths[18] =
        { 42, 26, 58, 34, 48, 70, 30, 54, 40, 64, 28, 52, 36, 68, 44, 32, 56, 38 };
    for (int i = 0; i < 18; i++) {
        int bw = widths[i];
        int x = (i * 83 + 24) % (W + 80) - 40;
        int bh = 78 + ((i * 47) % (desk_h / 3 + 60));
        int y = base - bh;
        if (y < 46) y = 46;
        uint32_t fill = color_mix(t->bg, 0x00050A16u, i & 1, 3);
        gfx_fill_rect_blend(x, y, bw, base - y, argb(0xD0, fill));
        gfx_draw_rect(x, y, bw, base - y, (i & 1) ? t->border_cyan : t->win_border);
        for (int wy = y + 12; wy < base - 8; wy += 14) {
            for (int wx = x + 6; wx < x + bw - 8; wx += 12) {
                if (((wx + wy + i) & 3) == 0) continue;
                uint32_t c = ((wx + i) & 1) ? t->glow_cyan : t->accent_magenta;
                gfx_fill_rect_blend(wx, wy, 4, 2, argb(0x80, c));
            }
        }
        if (i == 5 && bw > 56) {
            gfx_draw_text(x + 7, y + 34, "TOBYOS", t->glow_cyan, GFX_TRANSPARENT);
        }
    }

    gfx_fill_rect_blend(0, horizon - 2, W, 2, argb(0x90, t->accent_magenta));
    gfx_fill_rect_blend(0, horizon + 5, W, 1, argb(0x80, t->glow_cyan));
    for (int r = 0; r < 8; r++) {
        int yy = base + r * 7;
        if (yy >= desk_h) break;
        gfx_fill_rect_blend(0, yy, W, 1,
                            argb((uint8_t)(0x54 - r * 4),
                                 (r & 1) ? t->glow_orange : t->glow_cyan));
    }
    draw_line(W / 2 - 70, horizon + 10, W / 2 - 210, desk_h - 1, t->bg_grid);
    draw_line(W / 2 + 70, horizon + 10, W / 2 + 210, desk_h - 1, t->bg_grid);
}

static void paint_top_hud(int W) {
    const struct theme_palette *t = theme_active();
    int h = 42;
    paint_glass_rect(0, 0, W, h, t->panel, t->panel_glass,
                     t->win_border, t->border_cyan);
    paint_toby_hex_logo(18, 8, 26, t->glow_orange, t->border_orange);
    gfx_draw_text_smooth(54, 9, "TOBYOS", t->glow_orange,
                         GFX_TRANSPARENT, 2);
    gfx_draw_text(56, 29, "NIXIE DESKTOP", t->text_secondary,
                  GFX_TRANSPARENT);

    int mid = W / 3;
    gfx_fill_rect_blend(mid - 26, 8, 1, 26, argb(0x50, t->win_border));
    gfx_draw_text(mid, 10, "MAY 2026", t->text_primary, GFX_TRANSPARENT);
    gfx_draw_text(mid, 25, "BUILD PREVIEW", t->text_secondary,
                  GFX_TRANSPARENT);

    int eqx = W / 2 - 60;
    for (int i = 0; i < 30; i++) {
        int bh = 3 + ((i * 7 + i / 3) % 18);
        uint32_t c = (i & 3) ? t->glow_cyan : t->glow_orange;
        gfx_fill_rect_blend(eqx + i * 5, 31 - bh, 2, bh, argb(0xC0, c));
    }

    int hx = W * 62 / 100;
    if (hx + 160 < W) {
        gfx_fill_rect_blend(hx - 22, 8, 1, 26, argb(0x50, t->win_border));
        gfx_draw_text(hx, 10, "SYS HEALTH", t->text_secondary,
                      GFX_TRANSPARENT);
        gfx_draw_text(hx, 25, net_is_up() ? "Good" : "Booting",
                      net_is_up() ? t->success : t->glow_orange,
                      GFX_TRANSPARENT);
    }

    const char *icons[] = { "Q", "GRID", "BELL", "VOL", "NET", "USR", "PWR" };
    int ix = W - 320;
    if (ix < hx + 118) ix = hx + 118;
    for (unsigned i = 0; i < sizeof(icons) / sizeof(icons[0]); i++) {
        int cw = (i == 1 || i == 2) ? 42 : 34;
        if (ix + cw >= W - 8) break;
        gfx_fill_rect_blend(ix, 8, 1, 26, argb(0x38, t->win_border));
        gfx_draw_text(ix + 9, 17, icons[i], t->text_primary,
                      GFX_TRANSPARENT);
        ix += cw;
    }
}

/* Retired M36 prototype artwork. The shell now launches real GUI apps
 * for File Explorer and Settings; keep this disabled reference nearby
 * only while the real apps catch up visually. */
#if 0
static void paint_mock_window_frame(int x, int y, int w, int h,
                                    const char *title, uint32_t accent) {
    const struct theme_palette *t = theme_active();
    paint_soft_panel(x, y, w, h, t->panel, t->panel_glass,
                     t->win_border, accent);
    fill_vgradient(x + 1, y + 1, w - 2, 28,
                   t->title_focus_hi, t->title_focus, 4);
    gfx_fill_rect(x + 1, y + 28, w - 2, 1, accent);
    paint_window_glyph(x + 12, y + 9, accent, t->glow_orange);
    gfx_draw_text(x + 30, y + 10, title, t->text_primary, GFX_TRANSPARENT);

    int cx = x + w - 78;
    gfx_draw_text(cx,      y + 10, "-", t->text_secondary, GFX_TRANSPARENT);
    gfx_draw_text(cx + 28, y + 10, "[]", t->text_secondary, GFX_TRANSPARENT);
    gfx_draw_text(cx + 60, y + 10, "X", t->danger, GFX_TRANSPARENT);
}

static void paint_mock_folder(int x, int y, const char *label,
                              uint32_t accent) {
    const struct theme_palette *t = theme_active();
    paint_soft_panel(x, y, 74, 54, 0x000B1424u, argb(0x24, t->panel),
                     t->tray_border, accent);
    paint_icon_symbol(x + 21, y + 7, 1, accent, t->glow_cyan);
    gfx_draw_text(x + 7, y + 39, label, t->text_primary, GFX_TRANSPARENT);
}

static void paint_mock_drive(int x, int y, const char *name,
                             const char *detail, int pct,
                             uint32_t accent) {
    const struct theme_palette *t = theme_active();
    gfx_draw_text(x, y, name, t->text_primary, GFX_TRANSPARENT);
    gfx_draw_rect(x, y + 14, 126, 8, t->tray_border);
    int fill = (126 - 2) * pct / 100;
    if (fill < 0) fill = 0;
    if (fill > 124) fill = 124;
    gfx_fill_rect(x + 1, y + 15, fill, 6, accent);
    gfx_draw_text(x, y + 27, detail, t->text_secondary, GFX_TRANSPARENT);
}

static void paint_file_explorer_mock(int W, int desk_h) {
    const struct theme_palette *t = theme_active();
    int rail = W >= 1120 ? 300 : 248;
    int x = W >= 1120 ? W * 32 / 100 : 112;
    int y = 64;
    int w = W - rail - x - 26;
    if (w > 590) w = 590;
    if (w < 390) return;
    int h = desk_h * 44 / 100;
    if (h > 280) h = 280;
    if (h < 220) h = 220;

    paint_mock_window_frame(x, y, w, h, "This PC", t->border_cyan);

    int toolbar_y = y + 38;
    gfx_draw_text(x + 14, toolbar_y + 8, "<  >  ^", t->text_primary,
                  GFX_TRANSPARENT);
    paint_soft_panel(x + 92, toolbar_y, w - 226, 24, 0x00070C16u,
                     argb(0x28, t->panel), t->tray_border, t->border_cyan);
    gfx_draw_text(x + 106, toolbar_y + 8, "This PC", t->text_primary,
                  GFX_TRANSPARENT);
    paint_soft_panel(x + w - 122, toolbar_y, 108, 24, 0x00070C16u,
                     argb(0x20, t->panel), t->tray_border, t->border_orange);
    gfx_draw_text(x + w - 108, toolbar_y + 8, "Search", t->text_secondary,
                  GFX_TRANSPARENT);

    int side_w = 128;
    int body_y = y + 72;
    gfx_fill_rect_blend(x + 1, body_y, side_w, h - 74, argb(0x70, 0x00081218u));
    const char *nav[] = { "Quick access", "Desktop", "Downloads", "Documents",
                          "Pictures", "Music", "This PC", "Network" };
    for (unsigned i = 0; i < sizeof(nav) / sizeof(nav[0]); i++) {
        int iy = body_y + 10 + (int)i * 18;
        bool sel = (i == 6);
        if (sel) {
            gfx_fill_rect_blend(x + 5, iy - 3, side_w - 10, 16,
                                argb(0xC0, t->center_item_hot));
            gfx_fill_rect(x + 5, iy - 3, 3, 16, t->border_orange);
        }
        gfx_draw_text(x + 14, iy, nav[i], sel ? t->text_primary : t->text_secondary,
                      GFX_TRANSPARENT);
    }
    gfx_fill_rect(x + side_w + 1, body_y, 1, h - 74, t->win_border);

    int main_x = x + side_w + 18;
    gfx_draw_text(main_x, body_y + 10, "Folders (6)", t->text_primary,
                  GFX_TRANSPARENT);
    const char *folders[] = { "Desktop", "Documents", "Downloads",
                              "Pictures", "Music", "Videos" };
    for (int i = 0; i < 6; i++) {
        int col = i % 3;
        int row = i / 3;
        int fx = main_x + col * 94;
        int fy = body_y + 30 + row * 66;
        if (fx + 74 < x + w - 10) {
            paint_mock_folder(fx, fy, folders[i],
                              (i & 1) ? t->glow_orange : t->glow_cyan);
        }
    }

    int drives_y = y + h - 68;
    gfx_draw_text(main_x, drives_y - 18, "Devices and drives (3)",
                  t->text_primary, GFX_TRANSPARENT);
    paint_mock_drive(main_x, drives_y, "System (C:)", "78.6 GB free", 46,
                     t->glow_cyan);
    if (main_x + 154 + 126 < x + w - 8) {
        paint_mock_drive(main_x + 154, drives_y, "Data (D:)", "512 GB free", 56,
                         t->accent_magenta);
    }
    if (main_x + 308 + 126 < x + w - 8) {
        paint_mock_drive(main_x + 308, drives_y, "Backup (E:)", "1.12 TB free", 63,
                         t->glow_orange);
    }
}

static void paint_color_dot(int x, int y, uint32_t c, bool selected) {
    const struct theme_palette *t = theme_active();
    gfx_fill_rect(x, y, 14, 14, c);
    gfx_draw_rect(x - 2, y - 2, 18, 18, selected ? t->text_primary : t->tray_border);
    if (selected) {
        gfx_draw_text(x + 3, y + 3, "*", 0x00000000u, GFX_TRANSPARENT);
    }
}

static void paint_settings_mock(int W, int desk_h) {
    const struct theme_palette *t = theme_active();
    int rail = W >= 1120 ? 300 : 248;
    int x = W >= 1120 ? W * 32 / 100 : 112;
    int y = 64 + (desk_h * 44 / 100);
    if (y < 350) y = 350;
    int w = W - rail - x - 26;
    if (w > 590) w = 590;
    if (w < 390) return;
    int h = desk_h - y - 18;
    if (h > 250) h = 250;
    if (h < 172) return;

    paint_mock_window_frame(x, y, w, h, "Settings", t->border_orange);
    int body_y = y + 36;
    int side_w = 136;
    gfx_fill_rect_blend(x + 1, body_y, side_w, h - 38, argb(0x72, 0x00081218u));
    const char *nav[] = { "Home", "System", "Devices", "Network",
                          "Personalization", "Apps", "Accounts", "About" };
    for (unsigned i = 0; i < sizeof(nav) / sizeof(nav[0]); i++) {
        int iy = body_y + 12 + (int)i * 18;
        bool sel = (i == 4);
        if (sel) {
            gfx_fill_rect_blend(x + 6, iy - 3, side_w - 12, 16,
                                argb(0xC0, t->center_item_hot));
            gfx_fill_rect(x + 6, iy - 3, 3, 16, t->border_orange);
        }
        gfx_draw_text(x + 18, iy, nav[i], sel ? t->text_primary : t->text_secondary,
                      GFX_TRANSPARENT);
    }
    gfx_fill_rect(x + side_w + 1, body_y, 1, h - 38, t->win_border);

    int cx = x + side_w + 22;
    gfx_draw_text(cx, body_y + 12, "Personalization  >  Colors",
                  t->text_primary, GFX_TRANSPARENT);
    paint_soft_panel(cx, body_y + 34, 164, 74, 0x00070C16u,
                     argb(0x22, t->panel), t->tray_border, t->border_cyan);
    fill_vgradient(cx + 2, body_y + 36, 160, 70, t->bg, t->bg_vignette, 8);
    for (int i = 0; i < 7; i++) {
        int bx = cx + 8 + i * 22;
        int bh = 22 + ((i * 13) % 34);
        int by = body_y + 104 - bh;
        gfx_fill_rect_blend(bx, by, 16, bh, argb(0xC8, 0x00071118u));
        gfx_fill_rect_blend(bx + 4, by + 8, 8, 1,
                            argb(0x9A, (i & 1) ? t->glow_cyan : t->accent_magenta));
    }
    gfx_fill_rect_blend(cx + 2, body_y + 92, 160, 1, argb(0x80, t->glow_cyan));
    paint_soft_panel(cx + 178, body_y + 34, w - side_w - 216, 74,
                     0x00070C16u, argb(0x20, t->panel),
                     t->tray_border, t->border_orange);
    gfx_draw_text(cx + 190, body_y + 44, "Choose your accent color",
                  t->text_primary, GFX_TRANSPARENT);
    uint32_t dots[] = {
        0x0000A8FFu, 0x003D7CFFu, 0x006E5CFFu, 0x00A947FFu,
        0x00E447B8u, 0x00FF6E4Au, 0x00FF394Cu, 0x00FF8E2Eu,
        0x00FFC04Au, 0x0000D6B6u, 0x0030C060u, 0x00404860u
    };
    for (unsigned i = 0; i < sizeof(dots) / sizeof(dots[0]); i++) {
        paint_color_dot(cx + 190 + (int)(i % 6) * 24,
                        body_y + 62 + (int)(i / 6) * 24,
                        dots[i], i == 7 || i == 9);
    }

    int row_y = body_y + 122;
    paint_soft_panel(cx, row_y, w - side_w - 36, 28, 0x00070C16u,
                     argb(0x24, t->panel), t->tray_border, t->border_cyan);
    gfx_draw_text(cx + 12, row_y + 10, "Transparency effects",
                  t->text_primary, GFX_TRANSPARENT);
    gfx_draw_text(x + w - 84, row_y + 10, "On", t->text_primary,
                  GFX_TRANSPARENT);
    gfx_draw_rect(x + w - 42, row_y + 7, 28, 14, t->border_orange);
    gfx_fill_rect(x + w - 25, row_y + 9, 11, 10, t->glow_orange);

    row_y += 36;
    paint_soft_panel(cx, row_y, w - side_w - 36, 28, 0x00070C16u,
                     argb(0x24, t->panel), t->tray_border, t->border_orange);
    gfx_draw_text(cx + 12, row_y + 10, "Nixie Glow", t->text_primary,
                  GFX_TRANSPARENT);
    gfx_fill_rect(x + w - 158, row_y + 14, 96, 2, t->tray_border);
    gfx_fill_rect(x + w - 158, row_y + 14, 68, 2, t->glow_orange);
    gfx_fill_rect(x + w - 92, row_y + 10, 8, 10, t->glow_orange);
    gfx_draw_text(x + w - 54, row_y + 10, "75%", t->text_primary,
                  GFX_TRANSPARENT);
}
#endif

static void paint_quick_tile(int x, int y, int w, const char *a,
                             const char *b, bool on) {
    const struct theme_palette *t = theme_active();
    paint_soft_panel(x, y, w, 42,
                     on ? t->center_item_hot : 0x00070C16u,
                     argb(on ? 0x44 : 0x22, t->panel),
                     t->tray_border, on ? t->border_cyan : t->border_orange);
    gfx_draw_text(x + 10, y + 9, a, on ? t->glow_cyan : t->text_primary,
                  GFX_TRANSPARENT);
    gfx_draw_text(x + 10, y + 24, b, on ? t->glow_cyan : t->text_secondary,
                  GFX_TRANSPARENT);
}

static uint8_t mon_pct(uint32_t pct) {
    if (pct > 100u) pct = 100u;
    return (uint8_t)pct;
}

static void monitor_update_if_due(void) {
    uint64_t now = now_uptime_ms();
    if (g.mon_last_sample_ms != 0 &&
        now - g.mon_last_sample_ms < 750ull) {
        return;
    }

    sysmon_sample(&g.mon);
    g.mon_last_sample_ms = now;

    uint8_t pos = g.mon_hist_pos;
    g.mon_cpu_hist [pos] = mon_pct(g.mon.cpu_pct);
    g.mon_ram_hist [pos] = mon_pct(g.mon.ram_pct);
    g.mon_gui_hist [pos] = mon_pct(g.mon.gui_pct);
    g.mon_disk_hist[pos] = mon_pct(g.mon.disk_pct);
    g.mon_hist_pos = (uint8_t)((pos + 1u) % SYSMON_HISTORY);
    if (g.mon_hist_pos == 0) g.mon_hist_ready = true;
}

static void format_pages_mib(char *out, size_t cap,
                             uint64_t used_pages, uint64_t total_pages) {
    uint64_t used_mib  = (used_pages  * (uint64_t)PAGE_SIZE) >> 20;
    uint64_t total_mib = (total_pages * (uint64_t)PAGE_SIZE) >> 20;
    ksnprintf(out, cap, "%lu / %lu MiB",
              (unsigned long)used_mib, (unsigned long)total_mib);
}

static void paint_mini_graph(int x, int y, int w, int h,
                             const uint8_t hist[SYSMON_HISTORY],
                             uint32_t color) {
    const struct theme_palette *t = theme_active();
    gfx_draw_rect(x, y, w, h, t->tray_border);
    int bars = (w - 4) / 3;
    if (bars > SYSMON_HISTORY) bars = SYSMON_HISTORY;
    int first = (int)g.mon_hist_pos - bars;
    if (first < 0) first += SYSMON_HISTORY;
    for (int i = 0; i < bars; i++) {
        int idx = (first + i) % SYSMON_HISTORY;
        uint32_t v = hist[idx];
        int bh = (int)((v * (uint32_t)(h - 5)) / 100u);
        if (v && bh < 2) bh = 2;
        gfx_fill_rect_blend(x + 2 + i * 3, y + h - 2 - bh, 2, bh,
                            argb(0xB0, color));
    }
}

static void paint_monitor_row(int x, int y, const char *name,
                              const char *detail, int pct,
                              uint32_t accent,
                              const uint8_t hist[SYSMON_HISTORY]) {
    const struct theme_palette *t = theme_active();
    char pctbuf[8];
    ksnprintf(pctbuf, sizeof(pctbuf), "%d%%", pct);
    gfx_draw_rect(x, y, 38, 38, accent);
    gfx_draw_text(x + 7, y + 15, pctbuf, t->text_primary, GFX_TRANSPARENT);
    gfx_draw_text(x + 52, y + 4, name, t->text_primary, GFX_TRANSPARENT);
    gfx_draw_text(x + 52, y + 20, detail, t->text_secondary, GFX_TRANSPARENT);
    paint_mini_graph(x + 132, y + 4, 88, 30, hist, accent);
}

static void paint_right_widgets(int W, int desk_h) {
    if (!g.widgets_open || W < 900 || desk_h < 520) return;
    monitor_update_if_due();
    const struct theme_palette *t = theme_active();
    int rw = W >= 1120 ? 282 : 238;
    int x = W - rw - 14;
    int y = 58;

    paint_soft_panel(x, y, rw, 128, t->panel, t->panel_glass,
                     t->border_cyan, t->border_cyan);
    gfx_draw_text(x + 12, y + 12, "NOTIFICATIONS", t->text_primary,
                  GFX_TRANSPARENT);
    struct abi_notification recs[3];
    uint32_t rn = notify_get_records(recs, 3);
    if (rn == 0) {
        gfx_draw_text(x + 44, y + 58, "No notifications",
                      t->text_secondary, GFX_TRANSPARENT);
    }
    for (uint32_t i = 0; i < rn; i++) {
        int iy = y + 36 + i * 28;
        uint32_t accent = recs[i].urgency == ABI_NOTIFY_URG_ERR
                        ? t->danger
                        : (recs[i].urgency == ABI_NOTIFY_URG_WARN
                           ? t->border_orange : t->border_cyan);
        gfx_draw_rect(x + 12, iy, 20, 18, accent);
        gfx_draw_text(x + 44, iy, recs[i].title[0] ? recs[i].title : "Notification",
                      t->text_primary, GFX_TRANSPARENT);
        gfx_draw_text(x + 44, iy + 12, recs[i].body[0] ? recs[i].body : recs[i].app,
                      t->text_secondary,
                      GFX_TRANSPARENT);
    }
    gfx_draw_text(x + rw - 72, y + 108, "Clear all", t->text_secondary,
                  GFX_TRANSPARENT);

    y += 142;
    paint_soft_panel(x, y, rw, 154, t->panel, t->panel_glass,
                     t->border_cyan, t->border_orange);
    gfx_draw_text(x + 12, y + 12, "QUICK SETTINGS", t->text_primary,
                  GFX_TRANSPARENT);
    int tw = (rw - 34) / 3;
    bool wifi_up = net_is_up();
    paint_quick_tile(x + 10, y + 36, tw, "Wi-Fi",
                     wifi_up ? "On" : "No link", wifi_up);
    paint_quick_tile(x + 18 + tw, y + 36, tw, "BT",
                     g.quick_bt ? "On" : "Off", g.quick_bt);
    paint_quick_tile(x + 26 + tw * 2, y + 36, tw, "Night",
                     g.quick_night ? "On" : "Off", g.quick_night);
    paint_quick_tile(x + 10, y + 84, tw, "Nixie",
                     g.quick_nixie ? "On" : "Off", g.quick_nixie);
    paint_quick_tile(x + 18 + tw, y + 84, tw, "Air",
                     g.quick_airplane ? "On" : "Off", g.quick_airplane);
    paint_quick_tile(x + 26 + tw * 2, y + 84, tw, "Focus",
                     g.quick_focus ? "On" : "Off", g.quick_focus);
    gfx_fill_rect(x + 22, y + 136, rw - 52, 2, t->tray_border);
    gfx_fill_rect(x + 22, y + 136, (rw - 52) * 72 / 100, 2, t->glow_cyan);
    gfx_fill_rect(x + 22 + (rw - 52) * 72 / 100, y + 132, 8, 10, t->glow_cyan);

    y += 170;
    paint_soft_panel(x, y, rw, 200, t->panel, t->panel_glass,
                     t->border_cyan, t->border_cyan);
    gfx_draw_text(x + 12, y + 12, "SYSTEM MONITOR", t->text_primary,
                  GFX_TRANSPARENT);
    char cpu_detail[32], ram_detail[32], gui_detail[32], disk_detail[32];
    ksnprintf(cpu_detail, sizeof(cpu_detail), "%u MHz  %u sys/s",
              (unsigned)g.mon.tsc_mhz, (unsigned)g.mon.syscalls_per_s);
    format_pages_mib(ram_detail, sizeof(ram_detail),
                     g.mon.used_pages, g.mon.total_pages);
    ksnprintf(gui_detail, sizeof(gui_detail), "%u fps  %lu frames",
              (unsigned)g.mon.gui_fps, (unsigned long)g.mon.gui_frames);
    ksnprintf(disk_detail, sizeof(disk_detail), "VFS %u ops/s",
              (unsigned)g.mon.vfs_ops_per_s);
    paint_monitor_row(x + 12, y + 36,  "CPU",  cpu_detail,
                      (int)g.mon.cpu_pct, t->glow_cyan, g.mon_cpu_hist);
    paint_monitor_row(x + 12, y + 74,  "RAM",  ram_detail,
                      (int)g.mon.ram_pct, t->accent_magenta, g.mon_ram_hist);
    paint_monitor_row(x + 12, y + 112, "GUI",  gui_detail,
                      (int)g.mon.gui_pct, t->success, g.mon_gui_hist);
    paint_monitor_row(x + 12, y + 150, "Disk", disk_detail,
                      (int)g.mon.disk_pct, t->glow_orange, g.mon_disk_hist);
}

static void paint_shell_widgets(int W, int desk_h) {
    /* M36: the old static File Explorer / Settings poster windows were
     * removed. Those surfaces now launch as real user-space GUI apps.
     * The remaining right-hand stack is compositor-owned shell chrome:
     * notification center, quick settings, and system monitor. */
    paint_right_widgets(W, desk_h);
}

static void compositor_paint_one(struct window *w, bool focused) {
    const struct theme_palette *t = theme_active();
    int ow = outer_w(w);
    int oh = outer_h(w);

    paint_window_shadow(w, focused);

    fill_vgradient(w->x, w->y, ow, GUI_TITLE_BAR_H,
                   focused ? t->title_focus_hi : t->title_unfocus_hi,
                   focused ? t->title_focus    : t->title_unfocus,
                   5);
    gfx_fill_rect_blend(w->x + 1, w->y + 1, ow - 2, 1, 0x30FFFFFFu);
    gfx_fill_rect(w->x, w->y + GUI_TITLE_BAR_H - 1, ow, 1,
                  focused ? t->win_glow : t->win_border);
    paint_window_glyph(w->x + 8, w->y + (GUI_TITLE_BAR_H - 10) / 2,
                       focused ? t->accent_cyan : t->title_text_dim,
                       focused ? t->accent_magenta : t->win_border);
    if (w->title[0]) {
        gfx_draw_text(w->x + 24, w->y + (GUI_TITLE_BAR_H - 8) / 2, w->title,
                      focused ? t->title_text : t->title_text_dim,
                      GFX_TRANSPARENT);
    }

    gfx_blit(w->x + GUI_BORDER, w->y + GUI_TITLE_BAR_H,
             w->client_w, w->client_h,
             w->backbuf, w->client_w);

    gfx_draw_rect(w->x, w->y, ow, oh, t->win_border);
    paint_close_button(w);
}

/* Wallpaper: theme-driven bg + optional grid + darker top band +
 * brand text in the middle. Cheap to paint, gives the empty desktop
 * a sense of place. M31 adds the cyber palette's optional grid
 * lines and a thin scanline band. */
static void paint_wallpaper(void) {
    const struct theme_palette *t = theme_active();
    int W = (int)gfx_width(), H = (int)gfx_height();
    int desk_h = H - GUI_TASKBAR_H;
    if (desk_h < 1) desk_h = H;

    /* Milestone 14: per-key overrides via /data/settings.conf so the
     * Settings GUI app can tweak the desktop colour live (and across
     * reboots). The theme palette is the FALLBACK -- explicit
     * settings still win. */
    uint32_t bg   = settings_get_u32("desktop.bg",      t->bg);
    uint32_t band = settings_get_u32("desktop.bg_band", t->bg_band);
    if (t->id == THEME_CYBER && bg == 0x00204060u) {
        bg = t->bg;   /* migrate the old baked-in blue to the theme look */
    }
    fill_vgradient(0, 0, W, desk_h, bg, t->bg_vignette, 24);
    if (desk_h < H) {
        gfx_fill_rect(0, desk_h, W, H - desk_h, t->taskbar);
    }

    /* M31 cyber: faint grid overlay. Step==0 means "no grid"
     * (basic theme). We draw 1-px lines spaced t->bg_grid_step
     * apart -- cheap to fill_rect on a framebuffer of any size. */
    if (t->bg_grid_step > 0) {
        int step = t->bg_grid_step;
        for (int x = 0; x < W; x += step) {
            gfx_fill_rect_blend(x, 0, 1, desk_h, argb(0x55, t->bg_grid));
        }
        for (int y = 0; y < desk_h; y += step) {
            gfx_fill_rect_blend(0, y, W, 1, argb(0x55, t->bg_grid));
        }
    }

    /* City skyline placeholder: cheap geometric accents that make the
     * empty desktop feel intentional without needing bitmap assets. */
    if (t->id == THEME_CYBER) {
        paint_city_wallpaper(W, desk_h, t);
    }

    /* M31 cyber: a subtle scanline band, 8 px tall, alpha-blended
     * across the wallpaper one-third of the way down. We use the
     * blend variant so the grid still shows underneath. */
    if (t->scanline) {
        int sy = desk_h / 3;
        /* 0xRRGGBB into 0xAARRGGBB; A=0x14 (~8%) reads as a
         * just-perceptible HUD band. */
        gfx_fill_rect_blend(0, sy, W, 8, argb(0x14, t->accent_cyan));
    }

    (void)band;
    paint_top_hud(W);
    paint_desktop_icons(58);
    paint_shell_widgets(W, desk_h);

    /* Always-visible emergency-hotkey hint above the taskbar.
     * Critical because the user can't see the serial-only hotkey
     * reminder once the screen has switched away from text mode. */
    const char *hint =
        "F1=dump  F2=force-exit desktop  (F11/F12 also work; "
        "Pause = panic exit)";
    int hlen = 0; while (hint[hlen]) hlen++;
    int hx = W - hlen * 8 - 8;
    if (hx < 8) hx = 8;
    int hy = desk_h - 14;
    gfx_draw_text(hx, hy, hint, t->title_text_dim, GFX_TRANSPARENT);
}

/* Truncating copy: copies up to dst_max-1 chars from src to dst,
 * appending "" if it had to truncate. */
static void copy_clip(char *dst, int dst_max, const char *src) {
    int i = 0;
    while (src[i] && i < dst_max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ---- M31 system tray --------------------------------------------- *
 *
 * The tray is rendered as a row of small pills on the right side of
 * the taskbar. Pills draw themselves indistinguishably from real
 * widgets but they are NOT actual windows -- they live entirely in
 * the compositor pass. This keeps the desktop responsive (no extra
 * proc spawn for a clock) and avoids the focus / Z-order / input
 * routing complexity that real tray plugins would bring. */

/* Paint a single pill: dark fill + 1-px accent border on top edge.
 * The "accent" colour is applied to the top-edge stripe ONLY, not
 * the whole border, so the row of pills reads as a HUD strip rather
 * than as outlined buttons. The hover state brightens the fill. */
static void paint_pill(const struct tray_rect *r, bool hot, uint32_t accent) {
    if (!r->present) return;
    const struct theme_palette *t = theme_active();
    paint_glass_rect(r->x, r->y, r->w, r->h,
                     hot ? t->tray_bg_hot : t->tray_bg,
                     argb(hot ? 0x60 : 0x38, t->tray_bg),
                     t->tray_border, accent);
}

/* Centred text inside a pill. Text is monospace 8x16, so column
 * count is len * 8 -- we pad on the left to centre. */
static void pill_text(const struct tray_rect *r, const char *s, uint32_t fg) {
    int len = 0; while (s[len]) len++;
    int tx = r->x + (r->w - len * 8) / 2;
    if (tx < r->x + 4) tx = r->x + 4;
    int ty = r->y + (r->h - 8) / 2;
    gfx_draw_text(tx, ty, s, fg, GFX_TRANSPARENT);
}

/* Network pill: show the IP once configured; otherwise make the failure
 * class visible so bare-metal boots are debuggable without serial. */
static void paint_tray_net(const struct tray_rect *r, bool hot) {
    const struct theme_palette *t = theme_active();
    char text[24];
    bool up = net_is_up() && g_my_ip != 0;
    uint32_t accent = up ? t->status_ok : t->status_err;
    paint_pill(r, hot, accent);
    if (up) {
        char ip[16];
        net_format_ip(ip, g_my_ip);
        ksnprintf(text, sizeof(text), "NET %s", ip);
        pill_text(r, text, t->tray_text);
    } else {
        const char *label = "NET NO LINK";
        switch (net_status()) {
        case NET_STATUS_NO_NIC:     label = "NET NO NIC"; break;
        case NET_STATUS_DHCP_WAIT:  label = "NET DHCP..."; break;
        case NET_STATUS_DHCP_EMPTY: label = "NET EMPTY IP"; break;
        default:                    break;
        }
        pill_text(r, label, t->tray_text_dim);
    }
}

/* Power pill: early shell mock shows AC/power status here. Battery
 * telemetry can replace this once ACPI battery reporting is promoted
 * into the tray API. */
static void paint_tray_disk(const struct tray_rect *r, bool hot) {
    const struct theme_palette *t = theme_active();
    paint_pill(r, hot, t->status_ok);
    pill_text(r, "PWR AC", t->tray_text);
}

/* Audio pill: "AUD ON" if HDA controller probed, "AUD --" otherwise. */
static void paint_tray_aud(const struct tray_rect *r, bool hot) {
    const struct theme_palette *t = theme_active();
    bool present = audio_hda_present();
    uint32_t accent = present ? t->status_ok : t->status_warn;
    paint_pill(r, hot, accent);
    if (present) {
        pill_text(r, "AUD ON", t->tray_text);
    } else {
        pill_text(r, "AUD --", t->tray_text_dim);
    }
}

/* Window-count pill: "WIN N" where N is the number of open windows.
 * Helps the operator see at a glance how many apps are running. */
static void paint_tray_win(const struct tray_rect *r, bool hot) {
    const struct theme_palette *t = theme_active();
    int n = 0;
    for (struct window *w = g.z_top; w; w = w->z_next) n++;
    paint_pill(r, hot, t->accent_magenta);
    char text[16];
    ksnprintf(text, sizeof(text), "WIN %d", n);
    pill_text(r, text, t->tray_text);
}

/* Bell pill: shows the unread notification count. Click toggles the
 * notification center panel. The bell uses a "B" letter glyph since
 * the framebuffer font has no proper bell symbol. */
static void paint_tray_bell(const struct tray_rect *r, bool hot) {
    const struct theme_palette *t = theme_active();
    uint32_t unread = notify_unread_count();
    uint32_t accent = unread ? t->accent_amber : t->accent_cyan;
    paint_pill(r, hot || g.center_open, accent);
    char text[8];
    ksnprintf(text, sizeof(text), "B %u", (unsigned)unread);
    pill_text(r, text, unread ? t->tray_text : t->tray_text_dim);
}

/* Clock pill: HH:MM:SS uptime (no RTC subsystem yet). Always
 * present; never dropped by tray_layout. */
static void paint_tray_clock(const struct tray_rect *r) {
    const struct theme_palette *t = theme_active();
    paint_pill(r, false, t->accent_cyan);
    char clk[16];
    format_uptime(clk, sizeof(clk));
    gfx_draw_text(r->x + 10, r->y + 4, clk, t->tray_text, GFX_TRANSPARENT);
    gfx_draw_text(r->x + 10, r->y + 15, "MAY 2026", t->tray_text_dim,
                  GFX_TRANSPARENT);
}

static void paint_tray(void) {
    struct tray_rect rects[TRAY_PILL_COUNT];
    tray_layout(rects);
    int hover_idx = point_in_tray_pill(g.cur_x, g.cur_y);

    if (rects[TRAY_PILL_NET].present)
        paint_tray_net(&rects[TRAY_PILL_NET], hover_idx == TRAY_PILL_NET);
    if (rects[TRAY_PILL_DISK].present)
        paint_tray_disk(&rects[TRAY_PILL_DISK], hover_idx == TRAY_PILL_DISK);
    if (rects[TRAY_PILL_AUD].present)
        paint_tray_aud(&rects[TRAY_PILL_AUD], hover_idx == TRAY_PILL_AUD);
    if (rects[TRAY_PILL_WIN].present)
        paint_tray_win(&rects[TRAY_PILL_WIN], hover_idx == TRAY_PILL_WIN);
    if (rects[TRAY_PILL_BELL].present)
        paint_tray_bell(&rects[TRAY_PILL_BELL], hover_idx == TRAY_PILL_BELL);
    if (rects[TRAY_PILL_CLOCK].present)
        paint_tray_clock(&rects[TRAY_PILL_CLOCK]);
}

static void paint_taskbar(void) {
    const struct theme_palette *t = theme_active();
    int W  = (int)gfx_width();
    int yt = taskbar_top();

    /* Glass taskbar layered over windows. */
    gfx_fill_rect(0, yt, W, GUI_TASKBAR_H, t->taskbar);
    if ((t->taskbar_glass >> 24) != 0) {
        gfx_fill_rect_blend(0, yt, W, GUI_TASKBAR_H, t->taskbar_glass);
    }
    gfx_fill_rect(0, yt, W, 1, t->taskbar_top);
    gfx_fill_rect_blend(0, yt + 1, W, 1, 0x26FFFFFFu);
    gfx_fill_rect_blend(0, yt - 6, W, 6, 0x30000000u);

    /* TobyOS launcher button. */
    bool start_hot = (g.cur_x >= 0 && g.cur_x < START_BTN_W &&
                      g.cur_y >= yt && g.cur_y < yt + GUI_TASKBAR_H);
    paint_glass_rect(4, yt + 3, START_BTN_W - 8, GUI_TASKBAR_H - 6,
                     (start_hot || g.menu_open) ? t->start_bg_hot : t->start_bg,
                     argb(0x54, t->start_bg),
                     t->win_border, t->glow_orange);
    paint_toby_hex_logo(17, yt + 7, 22, t->glow_orange, t->border_orange);

    /* Search area, intentionally visual-only for now. */
    int sx = START_BTN_W + 4;
    int sy = yt + 5;
    int sh = GUI_TASKBAR_H - 10;
    paint_glass_rect(sx, sy, TASKBAR_SEARCH_W, sh, 0x00070C16u,
                     argb(0x34, t->panel), t->tray_border, t->border_cyan);
    gfx_draw_rect(sx + 10, sy + 7, 10, 10, t->text_primary);
    draw_line(sx + 18, sy + 15, sx + 24, sy + 21, t->text_primary);
    gfx_draw_text(sx + 34, sy + (sh - 8) / 2, "Search TobyOS",
                  t->text_secondary, GFX_TRANSPARENT);

    const char *pins[TASKBAR_PIN_COUNT] = { "F", "G", ">_", "</>", "S", "W", "M" };
    int px = sx + TASKBAR_SEARCH_W + 8;
    for (int i = 0; i < TASKBAR_PIN_COUNT; i++) {
        int ix = px + i * TASKBAR_PIN_W;
        bool hot = (g.cur_x >= ix && g.cur_x < ix + TASKBAR_PIN_W - 4 &&
                    g.cur_y >= yt && g.cur_y < yt + GUI_TASKBAR_H);
        paint_glass_rect(ix, yt + 5, TASKBAR_PIN_W - 6, sh,
                         hot ? t->center_item_hot : 0x00070C16u,
                         argb(hot ? 0x58 : 0x28, t->panel),
                         hot ? t->border_orange : t->tray_border,
                         t->glow_orange);
        draw_text_centered(ix, yt + 5, TASKBAR_PIN_W - 6, sh,
                           pins[i], t->glow_orange);
        gfx_fill_rect(ix + 10, yt + GUI_TASKBAR_H - 4,
                      TASKBAR_PIN_W - 26, 2, t->glow_orange);
    }

    /* Window tabs (left -> right, oldest first). The right-edge limit
     * accounts for the tray width so tabs never overdraw the clock. */
    struct window *stack[GUI_WINDOW_MAX]; int n = 0;
    for (struct window *w = g.z_top; w && n < GUI_WINDOW_MAX; w = w->z_next) {
        stack[n++] = w;
    }
    int tabs_x_max = W;
    {
        struct tray_rect rects[TRAY_PILL_COUNT];
        tray_layout(rects);
        for (int i = 0; i < TRAY_PILL_COUNT; i++) {
            if (rects[i].present && rects[i].x < tabs_x_max) {
                tabs_x_max = rects[i].x;
            }
        }
        tabs_x_max -= TRAY_PAD;
    }
    int x = taskbar_tabs_x0();
    for (int i = n - 1; i >= 0; i--) {
        struct window *w = stack[i];
        bool focused = (w == g.z_top);
        int tx = x, tw = TAB_W - TAB_PAD;
        if (tx + tw > tabs_x_max) break;
        paint_glass_rect(tx, yt + 4, tw, GUI_TASKBAR_H - 8,
                         focused ? t->tab_bg_focus : t->tab_bg,
                         argb(focused ? 0x58 : 0x30, t->tab_bg),
                         t->tab_border,
                         focused ? t->accent_cyan : t->win_border);
        char clip[TAB_TEXT_MAX + 1];
        copy_clip(clip, sizeof(clip), w->title[0] ? w->title : "(no title)");
        gfx_draw_text(tx + 10, yt + (GUI_TASKBAR_H - 8) / 2,
                      clip, t->tab_fg, GFX_TRANSPARENT);
        x += TAB_W;
    }

    /* Right-most slice: system tray (clock / bell / win / aud / disk
     * / net). The tray is drawn AFTER the brand text would have been
     * so the brand was retired. The brand now lives on the
     * wallpaper. */
    paint_tray();

    /* Subtle dim brand text just to the left of the start button --
     * far enough to never overlap a tab. */
    int bx = taskbar_tabs_x0();
    /* Only show the brand if no tabs would overlap it. */
    if (n == 0) {
        gfx_draw_text(bx, yt + (GUI_TASKBAR_H - 8) / 2, TASKBAR_BRAND,
                      t->taskbar_text, GFX_TRANSPARENT);
    }
}

static void paint_launcher(void) {
    if (!g.menu_open) return;
    const struct theme_palette *t = theme_active();
    int mx, my, mw, mh; launcher_rect(&mx, &my, &mw, &mh);
    paint_soft_panel(mx, my, mw, mh, t->menu_bg, argb(0x86, t->menu_bg),
                     t->menu_border, t->border_orange);

    paint_toby_hex_logo(mx + 16, my + 14, 32, t->glow_orange,
                        t->border_orange);
    gfx_draw_text(mx + 58, my + 14, "Toby", t->text_primary,
                  GFX_TRANSPARENT);
    gfx_draw_text(mx + 58, my + 28, "Administrator", t->text_secondary,
                  GFX_TRANSPARENT);
    gfx_draw_text(mx + 14, my + 56, "Most used", t->text_primary,
                  GFX_TRANSPARENT);
    gfx_draw_text(mx + 190, my + 24, "Pinned", t->text_primary,
                  GFX_TRANSPARENT);
    gfx_draw_text(mx + mw - 24, my + 24, "X", t->text_secondary,
                  GFX_TRANSPARENT);

    int n = launcher_count();
    int visible = n - 1;
    if (visible > 6) visible = 6;
    int list_x = mx + LAUNCHER_PAD;
    int list_y = my + LAUNCHER_HEAD_H + 20;
    for (int i = 0; i < visible; i++) {
        int iy = list_y + i * LAUNCHER_ITEM_H;
        bool hot = (g.cur_x >= list_x && g.cur_x < list_x + LAUNCHER_LIST_W &&
                    g.cur_y >= iy && g.cur_y < iy + LAUNCHER_ITEM_H);
        struct launcher_item li;
        bool ok = launcher_resolve(i, &li) && li.label;
        if (!ok) continue;
        if (hot) {
            gfx_fill_rect_blend(list_x, iy - 2, LAUNCHER_LIST_W,
                                LAUNCHER_ITEM_H - 1, argb(0xD8, t->menu_hot));
            gfx_fill_rect(list_x, iy - 2, 3, LAUNCHER_ITEM_H - 1,
                          t->border_orange);
        }
        char letter[2] = { li.label[0], 0 };
        gfx_draw_text(list_x + 12, iy + 6, letter, t->glow_orange,
                      GFX_TRANSPARENT);
        gfx_draw_text(list_x + 32, iy + 6, li.label, t->menu_text,
                      GFX_TRANSPARENT);
    }

    gfx_draw_text(list_x, my + mh - 92, "All apps  >", t->text_primary,
                  GFX_TRANSPARENT);
    int power_y = my + mh - 64;
    bool power_hot = (g.cur_x >= list_x && g.cur_x < list_x + LAUNCHER_PROFILE_W - 16 &&
                      g.cur_y >= power_y && g.cur_y < power_y + 28);
    paint_glass_rect(list_x, power_y, LAUNCHER_PROFILE_W - 16, 28,
                     power_hot ? t->center_item_hot : 0x00070C16u,
                     argb(0x28, t->panel), t->tray_border, t->border_orange);
    gfx_draw_text(list_x + 12, power_y + 10, "Power", t->glow_orange,
                  GFX_TRANSPARENT);

    int grid_x = mx + 190;
    int grid_y = my + LAUNCHER_HEAD_H + 18;
    for (int i = 0; i < 9; i++) {
        int col = i % 3;
        int row = i / 3;
        int tx = grid_x + col * (LAUNCHER_TILE_W + 12);
        int ty = grid_y + row * (LAUNCHER_TILE_H + 12);
        struct launcher_item li;
        bool ok = launcher_resolve(i, &li) && li.label && li.path;
        const char *label = ok ? li.label : "App";
        bool hot = (g.cur_x >= tx && g.cur_x < tx + LAUNCHER_TILE_W &&
                    g.cur_y >= ty && g.cur_y < ty + LAUNCHER_TILE_H);
        paint_soft_panel(tx, ty, LAUNCHER_TILE_W, LAUNCHER_TILE_H,
                         hot ? t->center_item_hot : 0x00070C16u,
                         argb(hot ? 0x58 : 0x30, t->panel),
                         hot ? t->border_cyan : t->tray_border,
                         (i & 1) ? t->glow_cyan : t->glow_orange);
        char letter[2] = { label[0], 0 };
        draw_text_centered(tx, ty + 8, LAUNCHER_TILE_W, 12,
                           letter, (i & 1) ? t->glow_cyan : t->glow_orange);
        char clipped[10];
        copy_clip(clipped, sizeof(clipped), label);
        draw_text_centered(tx, ty + 31, LAUNCHER_TILE_W, 12,
                           clipped, t->text_primary);
    }

    int recent_y = grid_y + 3 * (LAUNCHER_TILE_H + 12) + 8;
    if (recent_y + 64 < my + mh - 42) {
        gfx_draw_text(grid_x, recent_y, "Recently opened", t->text_primary,
                      GFX_TRANSPARENT);
        gfx_draw_text(grid_x, recent_y + 20, "project_alpha.sk",
                      t->text_secondary, GFX_TRANSPARENT);
        gfx_draw_text(grid_x + 132, recent_y + 20, "system_diagram.png",
                      t->text_secondary, GFX_TRANSPARENT);
        gfx_draw_text(grid_x, recent_y + 38, "tobyos_update.log",
                      t->text_secondary, GFX_TRANSPARENT);
        gfx_draw_text(grid_x + 132, recent_y + 38, "notes.txt",
                      t->text_secondary, GFX_TRANSPARENT);
    }

    paint_glass_rect(mx + 12, my + mh - 34, mw - 24, 24, 0x00070C16u,
                     argb(0x28, t->panel), t->tray_border, t->border_cyan);
    gfx_draw_text(mx + 28, my + mh - 26, "Search programs and files...",
                  t->text_secondary, GFX_TRANSPARENT);
}

/* ---- M31 desktop notifications: toast + center ------------------- *
 *
 * Toasts and the notification center are pure compositor overlays.
 * They are NOT real gui_window instances:
 *   - They never steal keyboard focus or mouse-event routing.
 *   - They are never enumerated in ALT-TAB / taskbar tab listings.
 *   - They live and die entirely inside gui_tick / mouse_event paths.
 *
 * The kernel notification ring is the source of truth. The compositor
 * pulls one toast at a time via notify_pop_pending_toast() and
 * snapshots the live records via notify_get_records() for the center
 * panel. */

static const char *urg_label(uint32_t urg) {
    switch (urg) {
    case ABI_NOTIFY_URG_WARN: return "WARN";
    case ABI_NOTIFY_URG_ERR:  return "ERR";
    default:                  return "INFO";
    }
}

static uint32_t urg_accent(const struct theme_palette *t, uint32_t urg) {
    switch (urg) {
    case ABI_NOTIFY_URG_WARN: return t->status_warn;
    case ABI_NOTIFY_URG_ERR:  return t->status_err;
    default:                  return t->accent_cyan;
    }
}

/* Trim a string in-place at `cap` characters minus 1, NUL-terminating
 * what's left. Used so very long titles/bodies still fit inside the
 * toast / notification-center cells without wrapping. */
static void trim_to_fit(char *s, int cap_chars) {
    if (cap_chars <= 0) { s[0] = 0; return; }
    int n = 0; while (s[n]) n++;
    if (n <= cap_chars) return;
    /* Keep cap_chars - 1 chars + a "" indicator (single dot, since
     * the framebuffer font is ASCII-only). */
    if (cap_chars >= 2) {
        s[cap_chars - 1] = '.';
        s[cap_chars]     = 0;
    } else {
        s[cap_chars] = 0;
    }
}

static void paint_toast(void) {
    if (g.toast_id == 0) return;
    const struct theme_palette *t = theme_active();
    int x, y, w, h; toast_rect(&x, &y, &w, &h);
    uint32_t accent = urg_accent(t, g.toast_urgency);

    /* Body: dark glass fill, 1-px outer border, 4-px accent stripe on
     * the left edge that signals urgency at a glance. */
    gfx_fill_rect_blend(x + 6, y + 8, w, h, 0x65000000u);
    paint_glass_rect(x, y, w, h, t->toast_bg, argb(0x70, t->toast_bg),
                     t->toast_border, accent);
    gfx_fill_rect(x, y, 4, h, accent);

    /* Header: APP — URG */
    char header[ABI_NOTIFY_APP_MAX + 8];
    ksnprintf(header, sizeof(header), "%s -- %s",
              g.toast_app[0] ? g.toast_app : "?",
              urg_label(g.toast_urgency));
    gfx_draw_text(x + 12, y + 6, header,
                  t->tray_text_dim, GFX_TRANSPARENT);

    /* Title: bold-ish via the brighter colour. */
    char title[ABI_NOTIFY_TITLE_MAX];
    copy_clip(title, sizeof(title), g.toast_title);
    /* Toast width 340, left pad 12, char width 8 -> ~40 visible. */
    trim_to_fit(title, (TOAST_W - 16) / 8);
    gfx_draw_text(x + 12, y + 18, title,
                  t->toast_title, GFX_TRANSPARENT);

    /* Body, only if the taller variant is in play. */
    if (h == TOAST_H_FULL && g.toast_body[0]) {
        char body[ABI_NOTIFY_BODY_MAX];
        copy_clip(body, sizeof(body), g.toast_body);
        trim_to_fit(body, (TOAST_W - 16) / 8);
        gfx_draw_text(x + 12, y + 36, body,
                      t->toast_body, GFX_TRANSPARENT);
        /* Hint at the dismiss-on-click affordance. */
        gfx_draw_text(x + 12, y + 56, "click to dismiss",
                      t->tray_text_dim, GFX_TRANSPARENT);
    } else {
        gfx_draw_text(x + 12, y + h - 12, "click to dismiss",
                      t->tray_text_dim, GFX_TRANSPARENT);
    }
}

static void paint_center(void) {
    if (!g.center_open) return;
    const struct theme_palette *t = theme_active();
    int x, y, w, h; center_rect(&x, &y, &w, &h);

    /* Optional dimmer behind the panel so the wallpaper recedes a
     * touch -- keeps the cyberpunk "modal HUD" feel. */
    gfx_fill_rect_blend(0, 0,
                        (int)gfx_width(), (int)gfx_height() - GUI_TASKBAR_H,
                        0x40000000u);

    /* Panel body. */
    gfx_fill_rect_blend(x + 8, y + 10, w, h, 0x70000000u);
    paint_glass_rect(x, y, w, h, t->center_bg, argb(0x76, t->center_bg),
                     t->center_border, t->accent_cyan);

    /* Header: title + unread count */
    char header[48];
    ksnprintf(header, sizeof(header),
              "Notifications  (%u unread)",
              (unsigned)notify_unread_count());
    gfx_draw_text(x + 12, y + 14, header, t->center_header, GFX_TRANSPARENT);

    /* List items, newest first. */
    struct abi_notification recs[CENTER_VISIBLE_MAX];
    uint32_t n = notify_get_records(recs, CENTER_VISIBLE_MAX);
    int iy = y + CENTER_HEAD_H;
    for (uint32_t i = 0; i < n; i++) {
        struct abi_notification *r = &recs[i];
        bool hot = (g.cur_x >= x + 4 && g.cur_x < x + w - 4 &&
                    g.cur_y >= iy && g.cur_y < iy + CENTER_ITEM_H);
        gfx_fill_rect_blend(x + 4, iy + 2, w - 8, CENTER_ITEM_H - 4,
                            argb(hot ? 0xE8 : 0xD0,
                                 hot ? t->center_item_hot : t->center_item_bg));
        gfx_draw_rect(x + 4, iy + 2, w - 8, CENTER_ITEM_H - 4,
                      hot ? t->accent_cyan : t->win_border);
        /* Urgency stripe on the left edge of the item. */
        gfx_fill_rect(x + 4, iy + 2, 3, CENTER_ITEM_H - 4,
                      urg_accent(t, r->urgency));

        /* Header: APP -- URG  (greyed out so the title pops) */
        char head_buf[ABI_NOTIFY_APP_MAX + 8];
        ksnprintf(head_buf, sizeof(head_buf), "%s -- %s",
                  r->app[0] ? r->app : "?", urg_label(r->urgency));
        gfx_draw_text(x + 14, iy + 6, head_buf,
                      t->tray_text_dim, GFX_TRANSPARENT);

        char title[ABI_NOTIFY_TITLE_MAX];
        copy_clip(title, sizeof(title), r->title);
        trim_to_fit(title, (CENTER_W - 28) / 8);
        gfx_draw_text(x + 14, iy + 18, title,
                      t->toast_title, GFX_TRANSPARENT);

        if (r->body[0]) {
            char body[ABI_NOTIFY_BODY_MAX];
            copy_clip(body, sizeof(body), r->body);
            trim_to_fit(body, (CENTER_W - 28) / 8);
            gfx_draw_text(x + 14, iy + 36, body,
                          t->toast_body, GFX_TRANSPARENT);
        }
        iy += CENTER_ITEM_H;
        if (iy + CENTER_ITEM_H > y + h - CENTER_FOOT_H) break;
    }

    if (n == 0) {
        gfx_draw_text(x + 16, y + CENTER_HEAD_H + 12,
                      "No notifications.", t->tray_text_dim, GFX_TRANSPARENT);
    }

    /* Footer: clear-all button */
    int bx, by, bw, bh;
    center_clear_btn_rect(&bx, &by, &bw, &bh);
    bool clr_hot = (g.cur_x >= bx && g.cur_x < bx + bw &&
                    g.cur_y >= by && g.cur_y < by + bh);
    paint_glass_rect(bx, by, bw, bh,
                     clr_hot ? t->center_item_hot : t->center_item_bg,
                     argb(0x40, t->center_item_bg),
                     t->center_border, t->accent_magenta);
    draw_text_centered(bx, by, bw, bh, "Clear all", t->center_header);

    /* Close hint. */
    gfx_draw_text(x + 12, y + h - 18, "(click bell to close)",
                  t->tray_text_dim, GFX_TRANSPARENT);
}

static void compositor_pass(void) {
    /* Milestone 19: split the compositor cost into the paint phase
     * (everything in user-facing pixel time) and the flip phase
     * (memcpy to VRAM). Seeing them separately tells you whether a
     * "laggy" frame is the GPU-ish step or the copy step. */
    uint64_t t_comp = perf_rdtsc();

    const struct theme_palette *theme = theme_active();
    if (g.desktop_mode) {
        paint_wallpaper();
    } else {
        gfx_clear(theme->bg);
        if (!g.z_top) {
            gfx_draw_text(8, 8, "tobyOS GUI -- waiting for a window...",
                          theme->title_text, GFX_TRANSPARENT);
        }
    }

    /* Walk back-to-front so the topmost ends up on top. */
    struct window *stack[GUI_WINDOW_MAX];
    int n = 0;
    for (struct window *w = g.z_top; w && n < GUI_WINDOW_MAX; w = w->z_next) {
        stack[n++] = w;
    }
    for (int i = n - 1; i >= 0; i--) {
        compositor_paint_one(stack[i], i == 0);
    }

    if (g.desktop_mode) {
        paint_taskbar();
        paint_launcher();
        /* M31: notification center first (acts as a modal-ish panel
         * over the wallpaper + windows), THEN the toast above it so
         * a freshly-arrived toast is always on top of the panel. */
        paint_center();
        paint_toast();
    }

    perf_zone_end(PERF_Z_GUI_COMPOSITE, t_comp);

    /* M27E: The full compositor pass above always marks the entire
     * surface dirty (gfx_clear / paint_wallpaper / per-window blits).
     * That defeats partial-present even when the only thing that
     * actually changed on screen was, say, a 12x19-pixel cursor sprite.
     *
     * Strategy: the compositor maintains its OWN invalidation hints
     * (g.inv_*), populated from the user-input layer (cursor moves,
     * drag, window flip). If we have non-full hints, we now REPLACE
     * the gfx-layer dirty union with that hint -- the back buffer is
     * still pixel-perfect everywhere, so presenting only the hint
     * region is correct.
     *
     * Falls back to a full present whenever:
     *   - inv_full was set (window create/destroy, z-swap, mode flip)
     *   - hints were never registered this frame (defensive: present
     *     everything so we don't accidentally skip a valid update)
     *   - the hint covers >=95% of the screen anyway. */
    bool used_partial = false;
    if (!g.inv_full && g.inv_w > 0 && g.inv_h > 0) {
        uint64_t hint_area = (uint64_t)g.inv_w * (uint64_t)g.inv_h;
        uint64_t full_area = (uint64_t)gfx_width() * (uint64_t)gfx_height();
        if (full_area && hint_area * 100u < full_area * 95u) {
            gfx_dirty_clear();
            gfx_mark_dirty_rect(g.inv_x, g.inv_y, g.inv_w, g.inv_h);
            used_partial = true;
        }
    }
    /* Reset hints for the next frame regardless of which path we took
     * -- the compositor consumes them per-pass. */
    g.inv_x = g.inv_y = g.inv_w = g.inv_h = 0;
    g.inv_full = false;

    uint64_t t_flip = perf_rdtsc();
    gfx_flip();
    gfx_cursor_overlay_show(g.cur_x, g.cur_y);
    perf_zone_end(PERF_Z_GUI_FLIP, t_flip);

    if (used_partial) g.cmp_partial_frames++;
    else              g.cmp_full_frames++;

    /* Every completed compositor pass is one "frame" for the monitor. */
    perf_count_gui_frame();
}

/* M27E: invalidation hint API. All three functions are safe to call
 * from any context (IRQ, syscall, compositor itself) -- the hint
 * union is tiny and lives entirely in the global gui state. */
void gui_invalidate_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int sw = (int)gfx_width(), sh = (int)gfx_height();
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0)  x0 = 0;
    if (y0 < 0)  y0 = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    if (x1 <= x0 || y1 <= y0) return;
    if (g.inv_w == 0 || g.inv_h == 0) {
        g.inv_x = x0; g.inv_y = y0;
        g.inv_w = x1 - x0; g.inv_h = y1 - y0;
        return;
    }
    int u0 = g.inv_x, v0 = g.inv_y;
    int u1 = g.inv_x + g.inv_w, v1 = g.inv_y + g.inv_h;
    if (x0 < u0) u0 = x0;
    if (y0 < v0) v0 = y0;
    if (x1 > u1) u1 = x1;
    if (y1 > v1) v1 = y1;
    g.inv_x = u0; g.inv_y = v0;
    g.inv_w = u1 - u0; g.inv_h = v1 - v0;
}

void gui_invalidate_full(void) {
    g.inv_full = true;
    g.inv_x = 0; g.inv_y = 0;
    g.inv_w = (int)gfx_width();
    g.inv_h = (int)gfx_height();
}

void gui_invalidate_stats(uint64_t *out_full, uint64_t *out_partial) {
    if (out_full)    *out_full    = g.cmp_full_frames;
    if (out_partial) *out_partial = g.cmp_partial_frames;
}

/* ---- launch-queue + child reaper (drained from gui_tick on pid 0) -- *
 *
 * proc_create_from_elf walks the proc table and the VFS; it's safe to
 * call from the idle thread (pid 0) but not from arbitrary syscall
 * contexts. We therefore guard both the launch and the reap with an
 * "are we pid 0?" check -- gui_tick is called from the syscall-return
 * fast path too, where this would be unsafe. */
static void track_pid(int pid) {
    for (int i = 0; i < TRACKED_PIDS_MAX; i++) {
        if (g.tracked_pids[i] == 0) {
            g.tracked_pids[i] = pid;
            gui_trace_logf("track_pid pid=%d slot=%d", pid, i);
            return;
        }
    }
    /* Table full -- the process still runs, we just won't auto-reap. */
    gui_trace_logf("track_pid pid=%d FAILED (table full)", pid);
}
static void drain_launch_queue(void) {
    while (g.launch_tail != g.launch_head) {
        struct launch_entry e = g.launch_q[g.launch_tail];   /* copy out */
        g.launch_q[g.launch_tail].path   [0] = '\0';
        g.launch_q[g.launch_tail].arg    [0] = '\0';
        g.launch_q[g.launch_tail].sandbox[0] = '\0';
        g.launch_q[g.launch_tail].caps   [0] = '\0';
        g.launch_q[g.launch_tail].has_arg = false;
        g.launch_tail = (uint8_t)((g.launch_tail + 1u) % LAUNCH_QUEUE_MAX);
        if (e.path[0] == '\0') continue;

        /* Derive basename for argv[0] so the child's ps name matches
         * how users invoke it ("gui_viewer", not "/bin/gui_viewer"). */
        const char *base = e.path;
        for (const char *c = e.path; *c; c++) if (*c == '/') base = c + 1;

        /* Tag desktop-launched apps with the active session so
         * session_logout() can later SIGTERM them. We piggy-back on
         * the "spawn inherits parent's session_id" rule from
         * proc_spawn: temporarily flip pid 0's session_id to the
         * active session's id around the spawn, then restore it.
         *
         * If no one is logged in yet (pre-/bin/login) the session id
         * is 0, which matches pid 0's default and means "no
         * session". The login program itself is started by
         * session_init() through the service manager, NOT through
         * this queue, so it correctly stays untagged. */
        struct proc *self = current_proc();
        int saved_sid = self ? self->session_id : 0;
        int saved_uid = self ? self->uid        : 0;
        int saved_gid = self ? self->gid        : 0;
        int sid = session_active() ? session_current_id() : 0;
        int uid = session_active() ? session_current_uid() : 0;
        int gid = session_active() ? session_current_gid() : 0;
        if (self) {
            self->session_id = sid;
            self->uid        = uid;
            self->gid        = gid;
        }

        /* Milestone 18: pick the sandbox profile for this spawn. If
         * the enqueued entry carried an explicit profile (pkg-app
         * descriptors may set one), use that. Otherwise fall back
         * to "default" so user apps don't accidentally inherit pid
         * 0's ADMIN caps -- a signed-in user dragging an app off the
         * desktop should never get blanket kernel privilege. */
        const char *profile = e.sandbox[0] ? e.sandbox : "default";

        int pid;
        if (e.has_arg) {
            /* argv = { basename, arg }. Strings are kernel-owned (the
             * launch_entry copy above is on our stack), and proc_spawn
             * copies them into the child's user stack -- so we can let
             * `e` fall out of scope safely after the call returns. */
            char       *argv[2];
            argv[0] = (char *)base;
            argv[1] = e.arg;
            struct proc_spec ps = {
                .path = e.path,
                .name = base,
                .fd0  = 0, .fd1 = 0, .fd2 = 0,
                .argc = 2, .argv = argv,
                .sandbox_profile = profile,
                .declared_caps   = e.caps[0] ? e.caps : 0,
            };
            gui_trace_logf("drain_launch_queue: spawning '%s' arg='%s' "
                           "sandbox='%s' caps='%s'",
                           e.path, e.arg, profile, e.caps);
            pid = proc_spawn(&ps);
        } else {
            struct proc_spec ps = {
                .path = e.path,
                .name = base,
                .fd0  = 0, .fd1 = 0, .fd2 = 0,
                .argc = 0, .argv = 0,
                .sandbox_profile = profile,
                .declared_caps   = e.caps[0] ? e.caps : 0,
            };
            gui_trace_logf("drain_launch_queue: spawning '%s' sandbox='%s' caps='%s'",
                           e.path, profile, e.caps);
            pid = proc_spawn(&ps);
        }
        if (self) {
            self->session_id = saved_sid;
            self->uid        = saved_uid;
            self->gid        = saved_gid;
        }

        if (pid < 0) {
            kprintf("[gui] launch '%s' failed\n", e.path);
            gui_trace_logf("drain_launch_queue: '%s' FAILED (rv=%d)",
                           e.path, pid);
            continue;
        }
        track_pid(pid);
        kprintf("[gui] launched %s as pid %d (session=%d)\n",
                e.path, pid, sid);
        gui_trace_logf("drain_launch_queue: spawned '%s' as pid=%d session=%d",
                       e.path, pid, sid);
    }
}
static void reap_tracked(void) {
    for (int i = 0; i < TRACKED_PIDS_MAX; i++) {
        int pid = g.tracked_pids[i];
        if (pid == 0) continue;
        struct proc *p = proc_lookup(pid);
        if (!p) {
            /* Already gone (e.g. reaped by someone else). */
            gui_trace_logf("reap: pid=%d gone (slot=%d cleared)", pid, i);
            g.tracked_pids[i] = 0;
            continue;
        }
        if (p->state == PROC_TERMINATED) {
            int code = proc_wait(pid);
            gui_trace_logf("reap: pid=%d terminated exit=%d (slot=%d cleared)",
                           pid, code, i);
            g.tracked_pids[i] = 0;
        }
    }
}

/* True if any tracked desktop-launched app is still alive. The idle
 * loop never calls sched_yield itself (the kernel is cooperative);
 * if we have live apps, gui_tick yields once per call so they get
 * CPU time without us having to run them in the foreground. */
static bool any_tracked_alive(void) {
    for (int i = 0; i < TRACKED_PIDS_MAX; i++) {
        int pid = g.tracked_pids[i];
        if (pid == 0) continue;
        struct proc *p = proc_lookup(pid);
        if (p && p->state != PROC_TERMINATED) return true;
    }
    return false;
}

void gui_tick(void) {
    if (!g.ready) return;

    /* Process-spawn / reap operations need pid 0's address space. */
    struct proc *cur = current_proc();
    bool on_pid0 = (cur && cur->pid == 0);

    if (g.input_boost_pid > 0) {
        int pid = g.input_boost_pid;
        g.input_boost_pid = 0;
        if (cur && cur->pid != pid) {
            sched_boost_pid(pid);
            sched_yield();
            cur = current_proc();
            on_pid0 = (cur && cur->pid == 0);
        }
    }

    /* Deferred BOOTLOG.TXT + UDP boot log: MSC on the live USB may
     * enumerate only after the idle loop has polled USB for a few
     * frames; the flush at end-of-kmain can miss. Wait ~30 ticks
     * (~300 ms at 100 Hz) after desktop_mode so xHCI/MSC settles and
     * ARP to the log collector (192.168.68.74) is reliable. */
    if (on_pid0 && g.desktop_mode) {
        static uint8_t s_bootlog_usb_retry_wait;
        static bool s_bootlog_usb_retry_done;
        if (!s_bootlog_usb_retry_done) {
            if (s_bootlog_usb_retry_wait < 30) {
                s_bootlog_usb_retry_wait++;
            } else {
                s_bootlog_usb_retry_done = true;
                bootlog_net_upload();
                bootlog_flush_usb_retry();
            }
        }
    }

    if (on_pid0) {
        if (g.launch_tail != g.launch_head) drain_launch_queue();
        reap_tracked();
        /* Service manager pump: monitor PROGRAM services, restart
         * any that exited if their policy says so. Cheap when there
         * are no live services to watch. We piggy-back on the GUI
         * tick because it already runs ~100 Hz from the idle loop
         * and is guaranteed to be on pid 0. */
        service_tick();
    }

    /* Heartbeat: at NORMAL trace level, emit a one-line liveness summary
     * every ~1 second of wall-clock from the pid-0 idle thread. If the
     * desktop ever locks up "for real", the heartbeat stops -- which is
     * itself the diagnostic signal. We rate-limit by pit_ticks (100 Hz). */
    if (on_pid0 && g_trace >= GUI_TRACE_NORMAL && g.active) {
        static unsigned long s_last_hb_tick = 0;
        unsigned long now = (unsigned long)pit_ticks();
        if (now - s_last_hb_tick >= 100) {
            s_last_hb_tick = now;
            int wcount = 0;
            for (struct window *w = g.z_top; w; w = w->z_next) wcount++;
            int alive = 0;
            for (int i = 0; i < TRACKED_PIDS_MAX; i++) {
                int pid = g.tracked_pids[i];
                if (pid == 0) continue;
                struct proc *p = proc_lookup(pid);
                if (p && p->state != PROC_TERMINATED) alive++;
            }
            gui_trace_logf("heartbeat windows=%d apps_alive=%d cursor=(%d,%d) "
                           "btn=0x%02x dirty=%d menu=%d desktop=%d",
                           wcount, alive, g.cur_x, g.cur_y,
                           (unsigned)g.cur_buttons, (int)g.dirty,
                           (int)g.menu_open, (int)g.desktop_mode);
        }
    }

    /* M31: notification toast lifecycle. Run on every tick from
     * pid 0 so the compositor pulls a fresh toast as soon as one
     * lands in the kernel ring. We mark dirty whenever:
     *   - a new toast was popped (so it appears within one tick)
     *   - the active toast expired (so it disappears within one tick)
     *
     * Pop is gated on the slot being free -- one toast at a time so
     * the user can read each one. Entries that never get popped
     * still appear in the notification center. */
    if (on_pid0 && g.active && g.desktop_mode) {
        uint64_t now = now_uptime_ms();
        if (g.toast_id != 0 && now >= g.toast_expire_ms) {
            gui_trace_logf("toast id=%u expired", (unsigned)g.toast_id);
            g.toast_id = 0;
            g.dirty = true;
        }
        if (g.toast_id == 0) {
            struct abi_notification rec;
            if (notify_pop_pending_toast(&rec)) {
                g.toast_id      = rec.id;
                g.toast_urgency = rec.urgency;
                g.toast_expire_ms = now + (uint64_t)NOTIFY_TOAST_LIFETIME_MS;
                copy_clip(g.toast_app,   sizeof(g.toast_app),   rec.app);
                copy_clip(g.toast_title, sizeof(g.toast_title), rec.title);
                copy_clip(g.toast_body,  sizeof(g.toast_body),  rec.body);
                gui_trace_logf("toast new id=%u urg=%u app='%s' title='%s'",
                               (unsigned)rec.id, (unsigned)rec.urgency,
                               g.toast_app, g.toast_title);
                g.dirty = true;
            }
        }

        /* Mark dirty once per second so the clock pill ticks. We use
         * `last_clock_min` to remember the second we last drew (the
         * field is reused as "second-of-uptime" -- sufficient for
         * uptime display, no wall-clock yet). */
        uint32_t cur_s = (uint32_t)(now / 1000ull);
        if (cur_s != g.last_clock_min) {
            g.last_clock_min = cur_s;
            g.dirty = true;
        }
    }

    /* Compositor passes ONLY run on pid 0.
     *
     * Why: compositor_pass() copies the full framebuffer (1280*800*4 ~=
     * 4 MB) plus walks every window's backbuf. Doing that after every
     * syscall an app makes is catastrophic for perceived latency --
     * gui_about issues ~25 draw syscalls in its first redraw_all(), so
     * pid 1 was monopolising the CPU for ~100 MB of memcpy before pid 0
     * ever got to update the cursor. Symptom: the desktop "froze" for
     * ~1-2 seconds right after launching an app.
     *
     * Instead: app syscalls just mark g.dirty=true and return fast; we
     * force-yield to pid 0, which is the only thread that actually
     * paints. Pid 0 then yields back so the app keeps making progress.
     *
     * This keeps the screen refresh rate bounded by how often pid 0 is
     * scheduled (basically every PIT tick = 100 Hz), instead of by how
     * fast the foreground app can dirty the back buffer. */
    if (on_pid0 && g.active && g.dirty) {
        if (g_trace >= GUI_TRACE_VERBOSE) {
            gui_trace_logf("compositor_pass: dirty -> repaint+flip");
        }
        g.dirty = false;
        compositor_pass();
    }

    /* Cooperative scheduling.
     *
     * pid 0 path: hand the CPU to any tracked desktop app that still
     *             wants to run. Done AFTER compositor_pass so the app
     *             sees a freshly-painted screen and a clean event queue.
     * app path:   if the syscall we just serviced dirtied the
     *             compositor, immediately yield to pid 0 so the user
     *             sees the new pixels promptly (and so pid 0's
     *             heartbeat keeps firing). */
    if (on_pid0) {
        if (any_tracked_alive()) {
            if (g_trace >= GUI_TRACE_VERBOSE) {
                gui_trace_logf("gui_tick: pid0 yielding to tracked app");
            }
            sched_yield();
        }
    } else if (g.dirty) {
        if (g_trace >= GUI_TRACE_VERBOSE) {
            gui_trace_logf("gui_tick: app yielding to pid0 for repaint");
        }
        sched_yield();
    }
}

bool gui_active(void) { return g.active; }

void gui_settings_changed(const char *key, const char *val) {
    if (!key) return;
    bool on = !(val && strcmp(val, "off") == 0);
    if (strcmp(key, "ui.widgets") == 0) {
        g.widgets_open = on;
    } else if (strcmp(key, "ui.night_light") == 0) {
        g.quick_night = on;
    } else if (strcmp(key, "device.bluetooth") == 0) {
        g.quick_bt = on;
    } else {
        return;
    }
    g.dirty = true;
}

void gui_set_desktop_mode(bool on) {
    if (!g.ready) return;
    if (g.desktop_mode == on) return;
    /* Auto-enable NORMAL trace when entering desktop mode so the next
     * launch / mouse-edge / heartbeat already shows up in serial.log
     * without the user having to remember to run `trace on` first.
     * Leaving desktop mode does NOT auto-disable -- the user may want
     * the trace to keep flowing across mode transitions during a
     * debugging session. */
    if (on && g_trace == GUI_TRACE_OFF) {
        gui_trace_set(GUI_TRACE_NORMAL);
        kprintf("[gui] auto-enabled trace (level=%d) for desktop session\n",
                g_trace);
    }
    gui_trace_logf("gui_set_desktop_mode: %s -> %s",
                   g.desktop_mode ? "on" : "off",
                   on ? "on" : "off");
    g.desktop_mode = on;
    if (!on) g.menu_open = false;
    recompute_active();
    g.dirty = true;
}
bool gui_in_desktop_mode(void) { return g.desktop_mode; }

/* ---- diagnostic dump + emergency exit (milestone 12) -------------- *
 *
 * Both routines may be invoked from IRQ context (the keyboard IRQ
 * fires gui_emergency_exit on F12). They therefore use ONLY kprintf
 * + plain reads of the global state and never call into kmalloc /
 * sched / signal-deliver.
 *
 * We deliberately bypass the gui_trace_logf prefix here so the dump
 * is always visible, regardless of whether tracing is enabled. The
 * dump is also intentionally verbose so it functions as a single
 * "what's the GUI doing right now?" snapshot. */
static const char *proc_state_str_local(int s) {
    switch (s) {
    case PROC_UNUSED:     return "UNUSED";
    case PROC_READY:      return "READY";
    case PROC_RUNNING:    return "RUN";
    case PROC_BLOCKED:    return "BLOCK";
    case PROC_TERMINATED: return "TERM";
    default:              return "?";
    }
}

void gui_dump_status(const char *reason) {
    unsigned long t = (unsigned long)pit_ticks();
    kprintf("[gui-status t=%lu] %s\n", t, reason ? reason : "");
    kprintf("  ready=%d active=%d desktop_mode=%d dirty=%d menu_open=%d\n",
            (int)g.ready, (int)g.active, (int)g.desktop_mode,
            (int)g.dirty, (int)g.menu_open);
    kprintf("  cursor=(%d,%d) cur_btn=0x%02x prev_btn=0x%02x drag_win=%d\n",
            g.cur_x, g.cur_y,
            (unsigned)g.cur_buttons, (unsigned)g.prev_buttons,
            g.drag_win ? g.drag_win->wid : 0);

    int qd = (int)((g.launch_head - g.launch_tail) % LAUNCH_QUEUE_MAX);
    if (qd < 0) qd += LAUNCH_QUEUE_MAX;
    kprintf("  launch_q: head=%u tail=%u depth=%d\n",
            (unsigned)g.launch_head, (unsigned)g.launch_tail, qd);
    for (int i = 0; i < LAUNCH_QUEUE_MAX; i++) {
        if (g.launch_q[i].path[0]) {
            kprintf("    slot[%d] = '%s'%s%s\n", i, g.launch_q[i].path,
                    g.launch_q[i].has_arg ? " arg=" : "",
                    g.launch_q[i].has_arg ? g.launch_q[i].arg : "");
        }
    }

    kprintf("  tracked PIDs:\n");
    int nlive = 0;
    for (int i = 0; i < TRACKED_PIDS_MAX; i++) {
        int pid = g.tracked_pids[i];
        if (pid == 0) continue;
        struct proc *p = proc_lookup(pid);
        if (!p) {
            kprintf("    slot[%d] pid=%d (gone)\n", i, pid);
        } else {
            kprintf("    slot[%d] pid=%d state=%s name='%s'\n",
                    i, pid, proc_state_str_local((int)p->state), p->name);
            if (p->state != PROC_TERMINATED) nlive++;
        }
    }
    if (nlive == 0) kprintf("    (none alive)\n");

    int wcount = 0;
    for (struct window *w = g.z_top; w; w = w->z_next) wcount++;
    kprintf("  windows: %d in z-order (top first)\n", wcount);
    int z = 0;
    for (struct window *w = g.z_top; w; w = w->z_next, z++) {
        int qhead = w->ev_head, qtail = w->ev_tail;
        int qdep = qhead - qtail;
        if (qdep < 0) qdep += GUI_EVENT_RING;
        kprintf("    z=%d wid=%d owner_pid=%d pos=(%d,%d) size=%dx%d "
                "ev_q[h=%d t=%d depth=%d] title='%s'\n",
                z, w->wid, w->owner_pid, w->x, w->y,
                w->client_w, w->client_h,
                qhead, qtail, qdep, w->title);
    }
    kprintf("[gui-status end]\n");
}

void gui_emergency_exit(const char *reason) {
    /* Always dump first so the snapshot reflects the freeze, not the
     * post-cleanup state. */
    gui_dump_status(reason ? reason : "emergency exit");
    if (g.desktop_mode) {
        kprintf("[gui] emergency: forcing desktop mode OFF\n");
        g.desktop_mode = false;
        g.menu_open    = false;
        g.drag_win     = 0;
        recompute_active();
        g.dirty = true;
    }
    /* SIGINT every desktop-launched app so they tear themselves down
     * cleanly. signal_send_to_pid is IRQ-safe (sets a pending bit).
     * The signal is delivered next time the target process traps into
     * the kernel (any syscall), which is essentially immediately for
     * a tg_run-style poll loop. */
    for (int i = 0; i < TRACKED_PIDS_MAX; i++) {
        int pid = g.tracked_pids[i];
        if (pid == 0) continue;
        struct proc *p = proc_lookup(pid);
        if (!p || p->state == PROC_TERMINATED) continue;
        kprintf("[gui] emergency: SIGINT -> pid %d ('%s')\n", pid, p->name);
        signal_send_to_pid(pid, SIGINT);
    }
}

/* ---- subsystem lifecycle ------------------------------------------ */

void gui_init(void) {
    if (!gfx_ready()) {
        kprintf("[gui] gfx not ready, skipping\n");
        return;
    }
    memset(&g, 0, sizeof(g));
    memset(g_pool, 0, sizeof(g_pool));
    g.ready = true;
    g.cur_x = (int)gfx_width()  / 2;
    g.cur_y = (int)gfx_height() / 2;
    g.spawn_x = 60;
    g.spawn_y = 40;
    char sbuf[16];
    settings_get_str("ui.widgets", sbuf, sizeof(sbuf), "on");
    g.widgets_open = strcmp(sbuf, "off") != 0;
    g.quick_wifi = true;
    g.quick_bt = true;
    g.quick_night = true;
    g.quick_nixie = true;
    g.quick_focus = true;
    mouse_set_callback(on_mouse_event);
    kprintf("[gui] window manager ready (max %d windows, %d-px title bar)\n",
            GUI_WINDOW_MAX, GUI_TITLE_BAR_H);
}

/* ---- window pool -------------------------------------------------- */

static struct window *alloc_slot(void) {
    for (int i = 0; i < GUI_WINDOW_MAX; i++) {
        if (!g_pool[i].in_use) {
            struct window *w = &g_pool[i];
            memset(w, 0, sizeof(*w));
            w->in_use = true;
            w->wid    = i + 1;
            return w;
        }
    }
    return 0;
}

static void free_slot(struct window *w) {
    if (!w) return;
    if (w->backbuf) { kfree(w->backbuf); w->backbuf = 0; }
    w->in_use = false;
    w->wid    = 0;
}

/* Activate / deactivate the GUI. Active when EITHER at least one
 * window exists OR desktop mode is on (the user typed `desktop`).
 * When deactivating, console_clear() so the text shell can resume
 * drawing into the framebuffer cleanly. */
static void recompute_active(void) {
    bool any = (g.z_top != 0) || g.desktop_mode;
    if (any && !g.active) {
        g.active = true;
        g.dirty  = true;
        kprintf("[gui] entering graphical mode\n");
    } else if (!any && g.active) {
        g.active = false;
        kprintf("[gui] returning to text mode\n");
        console_clear();
    }
}

/* ---- create / close ----------------------------------------------- */

struct window *gui_window_create(int client_w, int client_h, const char *title) {
    if (!g.ready) return 0;
    if (client_w < 40 || client_h < 20) return 0;
    if (client_w > (int)gfx_width()  - 4) client_w = (int)gfx_width()  - 4;
    if (client_h > (int)gfx_height() - GUI_TITLE_BAR_H - 4) {
        client_h = (int)gfx_height() - GUI_TITLE_BAR_H - 4;
    }

    struct window *w = alloc_slot();
    if (!w) return 0;

    size_t bytes = (size_t)client_w * client_h * 4u;
    w->backbuf = (uint32_t *)kmalloc(bytes);
    if (!w->backbuf) { free_slot(w); return 0; }
    /* Default fill so brand-new windows aren't transparent garbage.
     * Pull from the active theme so a basic-theme boot doesn't get
     * cyber-coloured backgrounds. */
    {
        const struct theme_palette *t = theme_active();
        uint32_t fill = t->win_bg;
        for (size_t i = 0; i < (size_t)client_w * client_h; i++) {
            w->backbuf[i] = fill;
        }
    }

    w->client_w = client_w;
    w->client_h = client_h;
    w->x = g.spawn_x;
    w->y = g.spawn_y;

    /* Tile next spawn so successive windows aren't hidden behind one. */
    g.spawn_x += 28;
    g.spawn_y += 28;
    if (g.spawn_x + outer_w(w) > (int)gfx_width())  g.spawn_x = 40;
    if (g.spawn_y + outer_h(w) > (int)gfx_height()) g.spawn_y = 30;

    if (title) {
        size_t i = 0;
        for (; i < GUI_TITLE_MAX - 1 && title[i]; i++) w->title[i] = title[i];
        w->title[i] = '\0';
    }

    struct proc *p = current_proc();
    w->owner_pid = p ? p->pid : -1;

    z_push_front(w);
    recompute_active();
    g.dirty = true;
    /* M27E: new window can land anywhere -- safest is a full present. */
    gui_invalidate_full();
    gui_trace_logf("window_create wid=%d owner_pid=%d size=%dx%d "
                   "pos=(%d,%d) title='%s'",
                   w->wid, w->owner_pid, w->client_w, w->client_h,
                   w->x, w->y, w->title);
    return w;
}

void gui_window_close(struct window *w) {
    if (!w || !w->in_use) return;
    gui_trace_logf("window_close wid=%d owner_pid=%d title='%s'",
                   w->wid, w->owner_pid, w->title);
    /* If we're tearing down a window that had focus AND was being
     * dragged (a Ctrl+C during a drag is the obvious case), make sure
     * the IRQ isn't left holding a dangling pointer. */
    if (g.drag_win == w) g.drag_win = 0;
    z_unlink(w);
    free_slot(w);
    recompute_active();
    g.dirty = true;
    /* M27E: closing a window re-exposes whatever was beneath it --
     * full present is the only safe path. */
    gui_invalidate_full();
}

/* ---- drawing operations (kernel-side, drive backbuf) -------------- */

/* Clip a (x, y, w, h) rect to a window's client area. Returns false if
 * the intersection is empty. */
static bool clip_to_client(const struct window *w, int *x, int *y,
                           int *rw, int *rh) {
    if (*rw <= 0 || *rh <= 0) return false;
    int x0 = *x, y0 = *y, x1 = *x + *rw, y1 = *y + *rh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w->client_w) x1 = w->client_w;
    if (y1 > w->client_h) y1 = w->client_h;
    if (x1 <= x0 || y1 <= y0) return false;
    *x = x0; *y = y0; *rw = x1 - x0; *rh = y1 - y0;
    return true;
}

int gui_window_fill(struct window *w, int x, int y, int rw, int rh,
                    uint32_t color) {
    if (!w || !w->in_use || !w->backbuf) return -1;
    if (!clip_to_client(w, &x, &y, &rw, &rh)) return 0;
    for (int dy = 0; dy < rh; dy++) {
        uint32_t *row = &w->backbuf[(y + dy) * w->client_w + x];
        for (int dx = 0; dx < rw; dx++) row[dx] = color;
    }
    return 0;
}

/* M27C: blend an ARGB colour into the window's XRGB backbuf using the
 * shared source-over math from gfx.c. The window content remains
 * XRGB after the call -- alpha is consumed during the blend.
 *
 * Trivial-alpha shortcuts mirror gfx_fill_rect_blend so a fully
 * transparent fill is a true no-op (returns 0 with no writes) and a
 * fully opaque fill doesn't pay the per-pixel multiply. */
int gui_window_fill_argb(struct window *w, int x, int y, int rw, int rh,
                         uint32_t argb) {
    if (!w || !w->in_use || !w->backbuf) return -1;
    uint32_t a = (argb >> 24) & 0xFFu;
    if (a == 0)   return 0;
    if (a == 255) return gui_window_fill(w, x, y, rw, rh,
                                         argb & 0x00FFFFFFu);
    if (!clip_to_client(w, &x, &y, &rw, &rh)) return 0;
    for (int dy = 0; dy < rh; dy++) {
        uint32_t *row = &w->backbuf[(y + dy) * w->client_w + x];
        for (int dx = 0; dx < rw; dx++) {
            row[dx] = gfx_blend_pixel_argb(row[dx], argb);
        }
    }
    return 0;
}

/* Draw 8x8 text into a window's back buffer. Uses font8x8_basic
 * directly so we don't go through gfx (which writes to the global
 * back buffer). */
extern const uint8_t font8x8_basic[128][8];

static void window_draw_glyph(struct window *w, int x, int y, char c,
                              uint32_t fg, uint32_t bg) {
    uint8_t ch = (uint8_t)c;
    if (ch >= 128) ch = '?';
    const uint8_t *rows = font8x8_basic[ch];
    bool transparent = (bg == GFX_TRANSPARENT);
    for (int dy = 0; dy < 8; dy++) {
        int py = y + dy;
        if (py < 0 || py >= w->client_h) continue;
        uint8_t bits = rows[dy];
        for (int dx = 0; dx < 8; dx++) {
            int px = x + dx;
            if (px < 0 || px >= w->client_w) continue;
            bool on = ((bits >> dx) & 1u) != 0;
            if      (on)              w->backbuf[py * w->client_w + px] = fg;
            else if (!transparent)    w->backbuf[py * w->client_w + px] = bg;
        }
    }
}

int gui_window_text(struct window *w, int x, int y, const char *s,
                    uint32_t fg, uint32_t bg) {
    if (!w || !w->in_use || !w->backbuf || !s) return -1;
    int cx = x;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; y += 8; continue; }
        window_draw_glyph(w, cx, y, *s, fg, bg);
        cx += 8;
    }
    return 0;
}

/* M27D: scaled glyph into a window's backbuf. Each source pixel
 * becomes a `s x s` block. With s=1 this is identical to
 * window_draw_glyph. The smooth=true path additionally lays a
 * half-alpha wedge on each diagonal corner that the bitmap font
 * leaves unfilled -- the same algorithm as gfx.c::draw_glyph_smooth
 * but operating on the window backbuf. */
static void window_draw_glyph_scaled(struct window *w, int x, int y,
                                     char c, uint32_t fg, uint32_t bg,
                                     int s, bool smooth) {
    uint8_t ch = (uint8_t)c;
    if (ch >= 128) ch = '?';
    const uint8_t *rows = font8x8_basic[ch];
    bool transparent = (bg == GFX_TRANSPARENT);
    /* Hard pass. */
    for (int dy = 0; dy < 8; dy++) {
        for (int dx = 0; dx < 8; dx++) {
            bool on = ((rows[dy] >> dx) & 1u) != 0;
            uint32_t col = on ? fg : bg;
            if (!on && transparent) continue;
            for (int sy = 0; sy < s; sy++) {
                int py = y + dy * s + sy;
                if (py < 0 || py >= w->client_h) continue;
                for (int sx = 0; sx < s; sx++) {
                    int px = x + dx * s + sx;
                    if (px < 0 || px >= w->client_w) continue;
                    w->backbuf[py * w->client_w + px] = col;
                }
            }
        }
    }
    if (!smooth || s < 2) return;
    /* Smoothing pass: alpha-blend a half-cell wedge into each
     * exterior diagonal corner. We bypass gfx_blend_pixel_argb's
     * shortcuts and do the math inline so we don't pay the call. */
    int half = s / 2; if (half < 1) half = 1;
    uint32_t fgR = (fg >> 16) & 0xFFu;
    uint32_t fgG = (fg >> 8 ) & 0xFFu;
    uint32_t fgB = (fg      ) & 0xFFu;
    for (int dy = 0; dy < 8; dy++) {
        for (int dx = 0; dx < 8; dx++) {
            int p  = (rows[dy] >> dx) & 1;
            if (!p) continue;
            int nE = (dx+1<8) ? ((rows[dy]   >> (dx+1)) & 1) : 0;
            int nW = (dx-1>=0)? ((rows[dy]   >> (dx-1)) & 1) : 0;
            int nN = (dy-1>=0)? ((rows[dy-1] >> dx)     & 1) : 0;
            int nS = (dy+1<8) ? ((rows[dy+1] >> dx)     & 1) : 0;
            int nNE = (dx+1<8 && dy-1>=0) ? ((rows[dy-1] >> (dx+1)) & 1) : 0;
            int nNW = (dx-1>=0&& dy-1>=0) ? ((rows[dy-1] >> (dx-1)) & 1) : 0;
            int nSE = (dx+1<8 && dy+1<8 ) ? ((rows[dy+1] >> (dx+1)) & 1) : 0;
            int nSW = (dx-1>=0&& dy+1<8 ) ? ((rows[dy+1] >> (dx-1)) & 1) : 0;
            struct { int diag, ox, oy; } wedges[] = {
                { nNE && !nN && !nE,    half, 0    },
                { nNW && !nN && !nW,    0,    0    },
                { nSE && !nS && !nE,    half, half },
                { nSW && !nS && !nW,    0,    half },
            };
            int x0 = x + dx * s;
            int y0 = y + dy * s;
            for (size_t i = 0; i < sizeof(wedges)/sizeof(wedges[0]); i++) {
                if (!wedges[i].diag) continue;
                /* 50% alpha = (fg + dst) / 2 -- no rounded-by-255
                 * needed at this fixed alpha, faster too. */
                for (int sy = 0; sy < half; sy++) {
                    int py = y0 + wedges[i].oy + sy;
                    if (py < 0 || py >= w->client_h) continue;
                    for (int sx = 0; sx < half; sx++) {
                        int px = x0 + wedges[i].ox + sx;
                        if (px < 0 || px >= w->client_w) continue;
                        uint32_t d = w->backbuf[py * w->client_w + px];
                        uint32_t dR = (d >> 16) & 0xFFu;
                        uint32_t dG = (d >> 8 ) & 0xFFu;
                        uint32_t dB = (d      ) & 0xFFu;
                        uint32_t oR = (fgR + dR) >> 1;
                        uint32_t oG = (fgG + dG) >> 1;
                        uint32_t oB = (fgB + dB) >> 1;
                        w->backbuf[py * w->client_w + px] =
                            (oR << 16) | (oG << 8) | oB;
                    }
                }
            }
        }
    }
}

int gui_window_text_scaled(struct window *w, int x, int y, const char *s,
                           uint32_t fg, uint32_t bg, int scale, int smooth) {
    if (!w || !w->in_use || !w->backbuf || !s) return -1;
    if (scale < 1) scale = 1;
    bool sm = (smooth != 0) && (scale >= 2);
    int cx = x;
    int cell = 8 * scale;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; y += cell; continue; }
        window_draw_glyph_scaled(w, cx, y, *s, fg, bg, scale, sm);
        cx += cell;
    }
    return 0;
}

int gui_window_flip(struct window *w) {
    if (!w || !w->in_use) return -1;
    g.dirty = true;
    /* M27E: hint just the window's outer rect. The compositor still
     * does a correct full repaint into the back buffer, but only this
     * region needs to actually go out to the front buffer. */
    gui_invalidate_rect(w->x, w->y, outer_w(w), outer_h(w));
    return 0;
}

/* ---- keyboard delivery (called from keyboard IRQ / USB HID poll) --- *
 *
 * Milestone 11: when the GUI is active, the keyboard IRQ no longer
 * pushes characters into the shell's text-mode ring (kbd buf_push).
 * Instead, every typed byte is enqueued as a GUI_EV_KEY event into
 * the topmost window -- that's our "keyboard focus" target at the
 * window-manager layer. The user-space toolkit then routes the event
 * to the focused widget inside that window.
 *
 * Linux's input shape is producer -> event queue -> consumer wakeup.
 * Match that here: posting a key prioritises the owning process, but
 * does not mark the compositor dirty. The app will dirty/flip its
 * window after it consumes the event, avoiding a stale repaint before
 * the keystroke is visible. */
void gui_post_key(uint8_t c) {
    if (!g.ready || !g.active) return;
    struct window *w = g.z_top;
    if (!w) return;
    if (g_trace >= GUI_TRACE_VERBOSE) {
        gui_trace_logf("post_key key=0x%02x ('%c') -> wid=%d owner_pid=%d",
                       (unsigned)c,
                       (c >= 0x20 && c < 0x7f) ? (char)c : '.',
                       w->wid, w->owner_pid);
    }
    enqueue_event(w, GUI_EV_KEY, 0, 0, 0, c);
    g.input_boost_pid = w->owner_pid;
}

int gui_window_poll_event(struct window *w, struct gui_event *out) {
    if (!w || !w->in_use || !out) return -1;
    /* Peek IRQ-side state with interrupts off so we don't see a
     * half-written event from the producer. The window list itself is
     * never mutated from the IRQ, so disabling for a few cycles is
     * enough -- no spinlocks required. */
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    int got = 0;
    if (w->ev_head != w->ev_tail) {
        *out = w->ev[w->ev_tail];
        w->ev_tail = (uint8_t)((w->ev_tail + 1u) % GUI_EVENT_RING);
        got = 1;
    }
    if (flags & (1ULL << 9)) sti();
    return got;
}
