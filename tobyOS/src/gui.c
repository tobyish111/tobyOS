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
#include <tobyos/net.h>
#include <tobyos/audio_hda.h>
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

#define TASKBAR_BRAND        "tobyOS // M31"

#define START_BTN_W       72
#define START_BTN_LABEL   "Apps"
#define TAB_W             132
#define TAB_PAD           4
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
#define TRAY_PILL_H       (GUI_TASKBAR_H - 8)
#define TRAY_GAP          4

#define TRAY_W_CLOCK      88
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
#define LAUNCHER_SYS_MAX  7
#define LAUNCHER_MAX      (LAUNCHER_SYS_MAX + GUI_LAUNCHER_USER_MAX + 1)
#define LAUNCHER_W        180
#define LAUNCHER_ITEM_H   22
#define LAUNCHER_PAD      4

#define LAUNCH_QUEUE_MAX   4
#define TRACKED_PIDS_MAX   8
#define LAUNCH_PATH_MAX    96
#define LAUNCH_ARG_MAX     128

struct launcher_item {
    const char *label;
    const char *path;        /* /bin/<...>; NULL = special quit-desktop */
};

/* System entries at the top of the menu. NEVER mutated after boot;
 * declared const so the linker keeps them in .rodata. */
static const struct launcher_item g_launcher_sys[LAUNCHER_SYS_MAX] = {
    { "Settings",      "/bin/gui_settings" },
    { "Terminal",      "/bin/gui_term"     },
    { "Files",         "/bin/gui_files"    },
    { "About system",  "/bin/gui_about"    },
    { "Widgets demo",  "/bin/gui_widgets"  },
    { "Hello clock",   "/bin/gui_clock"    },
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
    int x0 = START_BTN_W + 4;
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

/* Launcher menu rect: rises from just above the start button. Width
 * is fixed; height grows with the item count. */
static void launcher_rect(int *mx, int *my, int *mw, int *mh) {
    int items = launcher_count();
    *mw = LAUNCHER_W;
    *mh = items * LAUNCHER_ITEM_H + LAUNCHER_PAD * 2;
    *mx = 2;
    *my = taskbar_top() - *mh;
    if (*my < 4) *my = 4;
}
/* Returns item index 0..N-1 if the cursor is over a launcher entry,
 * -1 otherwise. */
static int launcher_item_at(int px, int py) {
    if (!g.menu_open) return -1;
    int mx, my, mw, mh; launcher_rect(&mx, &my, &mw, &mh);
    if (px < mx || px >= mx + mw) return -1;
    if (py < my + LAUNCHER_PAD || py >= my + mh - LAUNCHER_PAD) return -1;
    int local = py - (my + LAUNCHER_PAD);
    int idx = local / LAUNCHER_ITEM_H;
    if (idx < 0 || idx >= launcher_count()) return -1;
    return idx;
}
static bool point_in_menu(int px, int py) {
    if (!g.menu_open) return false;
    int mx, my, mw, mh; launcher_rect(&mx, &my, &mw, &mh);
    return px >= mx && py >= my && px < mx + mw && py < my + mh;
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

static void launch_enqueue_with_profile(const char *path, const char *profile) {
    (void)gui_launch_enqueue_arg_profile_caps(path, 0, profile, 0);
}

/* M34D variant: enqueue with both profile and declared caps. */
static void launch_enqueue_with_profile_caps(const char *path,
                                             const char *profile,
                                             const char *caps) {
    (void)gui_launch_enqueue_arg_profile_caps(path, 0, profile, caps);
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
    int nx = g.cur_x + dx;
    int ny = g.cur_y + dy;
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

    /* Cursor moved; we always want a redraw if the GUI is on screen. */
    if (g.active && moved) g.dirty = true;

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
    gfx_draw_rect(bx, by, bw, bh, t->win_border);
    /* Two diagonals. */
    int pad = 3;
    for (int i = 0; i < bw - 2 * pad; i++) {
        gfx_set_pixel(bx + pad + i, by + pad + i,             t->close_fg);
        gfx_set_pixel(bx + bw - pad - 1 - i, by + pad + i,    t->close_fg);
    }
}

static void compositor_paint_one(struct window *w, bool focused) {
    const struct theme_palette *t = theme_active();
    int ow = outer_w(w);
    int oh = outer_h(w);

    /* M31: focused windows get a 1-px neon accent line just under
     * the title bar; reads as a "glass panel" highlight at framebuffer
     * resolution and identifies the focused window at a glance. */
    gfx_fill_rect(w->x, w->y, ow, GUI_TITLE_BAR_H,
                  focused ? t->title_focus : t->title_unfocus);
    if (focused) {
        gfx_fill_rect(w->x, w->y + GUI_TITLE_BAR_H - 1, ow, 1,
                      t->win_glow);
    }
    if (w->title[0]) {
        gfx_draw_text(w->x + 8, w->y + 5, w->title,
                      t->title_text, GFX_TRANSPARENT);
    }
    gfx_draw_rect(w->x, w->y, ow, oh, t->win_border);

    gfx_blit(w->x + GUI_BORDER, w->y + GUI_TITLE_BAR_H,
             w->client_w, w->client_h,
             w->backbuf, w->client_w);

    paint_close_button(w);
}

/* Wallpaper: theme-driven bg + optional grid + darker top band +
 * brand text in the middle. Cheap to paint, gives the empty desktop
 * a sense of place. M31 adds the cyber palette's optional grid
 * lines and a thin scanline band. */
static void paint_wallpaper(void) {
    const struct theme_palette *t = theme_active();
    int W = (int)gfx_width(), H = (int)gfx_height();

    /* Milestone 14: per-key overrides via /data/settings.conf so the
     * Settings GUI app can tweak the desktop colour live (and across
     * reboots). The theme palette is the FALLBACK -- explicit
     * settings still win. */
    uint32_t bg   = settings_get_u32("desktop.bg",      t->bg);
    uint32_t band = settings_get_u32("desktop.bg_band", t->bg_band);
    gfx_clear(bg);

    /* M31 cyber: faint grid overlay. Step==0 means "no grid"
     * (basic theme). We draw 1-px lines spaced t->bg_grid_step
     * apart -- cheap to fill_rect on a framebuffer of any size. */
    if (t->bg_grid_step > 0) {
        for (int x = 0; x < W; x += t->bg_grid_step) {
            gfx_fill_rect(x, 0, 1, H - GUI_TASKBAR_H, t->bg_grid);
        }
        for (int y = 0; y < H - GUI_TASKBAR_H; y += t->bg_grid_step) {
            gfx_fill_rect(0, y, W, 1, t->bg_grid);
        }
    }

    /* Top band -- looks like a lower-third HUD strip in the cyber
     * theme, sits just at the top in basic. */
    gfx_fill_rect(0, 0, W, 28, band);
    /* M31: 1-px accent line along the top band's bottom edge. */
    gfx_fill_rect(0, 28, W, 1, t->accent_cyan);

    gfx_draw_text(10, 8,
                  "tobyOS desktop  //  click [Apps] to open programs",
                  t->title_text, GFX_TRANSPARENT);

    /* Centred big brand. Crude centring using 8-px font width. */
    const char *brand = "tobyOS";
    int bx = (W - 6 * 8) / 2;
    int by = (H - GUI_TASKBAR_H) / 2 - 16;
    gfx_draw_text(bx, by, brand, t->title_text, GFX_TRANSPARENT);
    gfx_draw_text(bx - 4 * 8, by + 16,
                  "//  M31 desktop",
                  t->accent_cyan, GFX_TRANSPARENT);

    /* M31 cyber: a subtle scanline band, 8 px tall, alpha-blended
     * across the wallpaper one-third of the way down. We use the
     * blend variant so the grid still shows underneath. */
    if (t->scanline) {
        int sy = (H - GUI_TASKBAR_H) / 3;
        /* 0xRRGGBB into 0xAARRGGBB; A=0x14 (~8%) reads as a
         * just-perceptible HUD band. */
        gfx_fill_rect_blend(0, sy, W, 8, 0x14000000u | (t->accent_cyan & 0x00FFFFFFu));
    }

    /* Always-visible emergency-hotkey hint above the taskbar.
     * Critical because the user can't see the serial-only hotkey
     * reminder once the screen has switched away from text mode. */
    const char *hint =
        "F1=dump  F2=force-exit desktop  (F11/F12 also work; "
        "Pause = panic exit)";
    int hlen = 0; while (hint[hlen]) hlen++;
    int hx = W - hlen * 8 - 8;
    if (hx < 8) hx = 8;
    int hy = H - GUI_TASKBAR_H - 12;
    gfx_draw_text(hx, hy, hint, t->title_text, GFX_TRANSPARENT);
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
    gfx_fill_rect(r->x, r->y, r->w, r->h, hot ? t->tray_bg_hot : t->tray_bg);
    gfx_draw_rect(r->x, r->y, r->w, r->h, t->tray_border);
    /* Accent stripe: 1 px on top edge inside the border. */
    gfx_fill_rect(r->x + 1, r->y, r->w - 2, 1, accent);
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

/* Network pill: "NET 10.0.2.15" if the stack is up + has an IP, else
 * "NET NO LINK". Driven by g_my_ip / net_is_up(). */
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
        pill_text(r, "NET NO LINK", t->tray_text_dim);
    }
}

/* Disk pill: simple "DISK OK" -- we don't have a free-space API yet,
 * but the pill being present at all signals that tobyfs mounted. */
static void paint_tray_disk(const struct tray_rect *r, bool hot) {
    const struct theme_palette *t = theme_active();
    paint_pill(r, hot, t->status_ok);
    pill_text(r, "DISK OK", t->tray_text);
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
    pill_text(r, clk, t->tray_text);
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

    /* Bar fill + 1-px neon accent line at the very top edge. */
    gfx_fill_rect(0, yt, W, GUI_TASKBAR_H, t->taskbar);
    gfx_fill_rect(0, yt, W, 1, t->taskbar_top);

    /* Start button. */
    bool start_hot = (g.cur_x >= 0 && g.cur_x < START_BTN_W &&
                      g.cur_y >= yt && g.cur_y < yt + GUI_TASKBAR_H);
    gfx_fill_rect(2, yt + 2, START_BTN_W - 4, GUI_TASKBAR_H - 4,
                  (start_hot || g.menu_open) ? t->start_bg_hot : t->start_bg);
    gfx_draw_rect(2, yt + 2, START_BTN_W - 4, GUI_TASKBAR_H - 4, t->win_border);
    /* M31: 1-px magenta marker on the start button top edge to make
     * it visually anchor the row of tray pills on the right. */
    gfx_fill_rect(3, yt + 2, START_BTN_W - 6, 1, t->accent_magenta);
    gfx_draw_text(12, yt + 8, START_BTN_LABEL,
                  t->start_fg, GFX_TRANSPARENT);

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
    int x = START_BTN_W + 4;
    for (int i = n - 1; i >= 0; i--) {
        struct window *w = stack[i];
        bool focused = (w == g.z_top);
        int tx = x, tw = TAB_W - TAB_PAD;
        if (tx + tw > tabs_x_max) break;
        gfx_fill_rect(tx, yt + 2, tw, GUI_TASKBAR_H - 4,
                      focused ? t->tab_bg_focus : t->tab_bg);
        gfx_draw_rect(tx, yt + 2, tw, GUI_TASKBAR_H - 4, t->tab_border);
        if (focused) {
            /* Focused-tab highlight bar, sits flush at the top edge. */
            gfx_fill_rect(tx + 1, yt + 2, tw - 2, 1, t->accent_cyan);
        }
        char clip[TAB_TEXT_MAX + 1];
        copy_clip(clip, sizeof(clip), w->title[0] ? w->title : "(no title)");
        gfx_draw_text(tx + 6, yt + 8, clip, t->tab_fg, GFX_TRANSPARENT);
        x += TAB_W;
    }

    /* Right-most slice: system tray (clock / bell / win / aud / disk
     * / net). The tray is drawn AFTER the brand text would have been
     * so the brand was retired. The brand now lives on the
     * wallpaper. */
    paint_tray();

    /* Subtle dim brand text just to the left of the start button --
     * far enough to never overlap a tab. */
    int blen = 0; while (TASKBAR_BRAND[blen]) blen++;
    int bx = START_BTN_W + 6;
    /* Only show the brand if no tabs would overlap it. */
    if (n == 0) {
        gfx_draw_text(bx, yt + 8, TASKBAR_BRAND,
                      t->taskbar_text, GFX_TRANSPARENT);
    }
}

static void paint_launcher(void) {
    if (!g.menu_open) return;
    const struct theme_palette *t = theme_active();
    int mx, my, mw, mh; launcher_rect(&mx, &my, &mw, &mh);
    gfx_fill_rect(mx, my, mw, mh, t->menu_bg);
    gfx_draw_rect(mx, my, mw, mh, t->menu_border);
    /* M31: 1-px neon accent line on the menu's top edge -- mirrors
     * the taskbar accent so the menu looks like an extension of the
     * panel rather than a floating popup. */
    gfx_fill_rect(mx, my, mw, 1, t->accent_cyan);

    int n = launcher_count();
    for (int i = 0; i < n; i++) {
        int iy = my + LAUNCHER_PAD + i * LAUNCHER_ITEM_H;
        bool hot = (g.cur_x >= mx && g.cur_x < mx + mw &&
                    g.cur_y >= iy && g.cur_y < iy + LAUNCHER_ITEM_H);
        if (hot) {
            gfx_fill_rect(mx + 2, iy, mw - 4, LAUNCHER_ITEM_H, t->menu_hot);
        }
        struct launcher_item li;
        if (launcher_resolve(i, &li) && li.label) {
            gfx_draw_text(mx + 10, iy + (LAUNCHER_ITEM_H - 8) / 2,
                          li.label, t->menu_text, GFX_TRANSPARENT);
        }
    }
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

    /* Body: dark fill, 1-px outer border, 4-px accent stripe on the
     * left edge that signals urgency at a glance. */
    gfx_fill_rect(x, y, w, h, t->toast_bg);
    gfx_draw_rect(x, y, w, h, t->toast_border);
    gfx_fill_rect(x, y, 4, h, accent);
    /* Top accent line for the cyber HUD vibe. */
    gfx_fill_rect(x + 4, y, w - 4, 1, accent);

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
    gfx_fill_rect(x, y, w, h, t->center_bg);
    gfx_draw_rect(x, y, w, h, t->center_border);
    /* Top accent line. */
    gfx_fill_rect(x, y, w, 1, t->accent_cyan);

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
        gfx_fill_rect(x + 4, iy + 2, w - 8, CENTER_ITEM_H - 4,
                      hot ? t->center_item_hot : t->center_item_bg);
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
    gfx_fill_rect(bx, by, bw, bh, clr_hot ? t->center_item_hot : t->center_item_bg);
    gfx_draw_rect(bx, by, bw, bh, t->center_border);
    gfx_draw_text(bx + 10, by + (bh - 8) / 2, "Clear all",
                  t->center_header, GFX_TRANSPARENT);

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

    gfx_draw_cursor(g.cur_x, g.cur_y);
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

/* ---- keyboard delivery (called from kbd_irq) ---------------------- *
 *
 * Milestone 11: when the GUI is active, the keyboard IRQ no longer
 * pushes characters into the shell's text-mode ring (kbd buf_push).
 * Instead, every typed byte is enqueued as a GUI_EV_KEY event into
 * the topmost window -- that's our "keyboard focus" target at the
 * window-manager layer. The user-space toolkit then routes the event
 * to the focused widget inside that window.
 *
 * No spinlock needed: the window list is only mutated from syscall
 * context, and the IRQ that this is called from already runs with
 * IF=0 (kbd_irq -> isr stub clears IF). */
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
    g.dirty = true;   /* allow the compositor to repaint if anything visual changed */
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
