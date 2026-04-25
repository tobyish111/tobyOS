/* gui.h -- minimal window manager + compositor for tobyOS.
 *
 * One global compositor manages a doubly-linked z-order list of windows
 * (head = topmost). Each window owns:
 *   - its position + client size (width/height of the drawable region)
 *   - a kmalloc'd back buffer for its client pixels
 *   - a per-window event ring fed by mouse activity
 *   - the pid of its owner (for diagnostics; ownership tracking is
 *     actually done via the FILE_KIND_WINDOW fd entry, so when a
 *     process exits its windows are freed automatically through
 *     file_close -> gui_window_close)
 *
 * The compositor only redraws when something changes (mouse motion,
 * button transition, window flip / create / destroy / focus). The idle
 * loop pumps gui_tick() right after net_poll(); when nothing is dirty
 * gui_tick is a couple of compares.
 *
 * Frame layout per window (drawn by the compositor, not by the user):
 *
 *   +---------------------------------------+  <- (x, y) outer top-left
 *   |          18-px title bar              |
 *   +---------------------------------------+
 *   |                                       |
 *   |          client area  (w x h)         |  <- pixels at (x+1, y+19)..
 *   |                                       |
 *   +---------------------------------------+
 *
 * Outer width  = w + 2*BORDER   (BORDER = 1)
 * Outer height = h + TITLE_BAR_H + BORDER
 *
 * GUI apps work entirely in CLIENT-area coordinates -- they never see
 * the title bar or border. Mouse events delivered to the window are
 * also translated into client coordinates.
 */

#ifndef TOBYOS_GUI_H
#define TOBYOS_GUI_H

#include <tobyos/types.h>

/* Hard limits sized for an 8-process / 1024x768 demo. */
#define GUI_WINDOW_MAX       8
#define GUI_TITLE_MAX        32
#define GUI_EVENT_RING       16

/* Visual constants -- exposed so GUI apps can position widgets relative
 * to their own client area without surprises. */
#define GUI_TITLE_BAR_H      18
#define GUI_BORDER           1

/* Taskbar height reserved at the bottom of the screen in desktop mode.
 * Compositor draws the taskbar last, so it always paints over windows;
 * the WM also clamps window y so the title bar can never disappear
 * underneath the taskbar. */
#define GUI_TASKBAR_H        24

/* Title-bar close button (top-right "X"). 14x14 px gives the user a
 * comfortable click target on the 18-px title bar. */
#define GUI_CLOSE_BTN_SIZE   14
#define GUI_CLOSE_BTN_PAD    2

/* ---- event types --------------------------------------------------- */

#define GUI_EV_NONE          0
#define GUI_EV_MOUSE_MOVE    1
#define GUI_EV_MOUSE_DOWN    2
#define GUI_EV_MOUSE_UP      3
#define GUI_EV_KEY           4
#define GUI_EV_CLOSE         5

/* Special "key" codes carried in gui_event.key. Regular printable
 * characters use their ASCII value; everything below 0x20 carries one
 * of these meanings. The toolkit (programs/common/toby_gui.c) maps
 * them onto widget actions (backspace edit, button activate, etc.). */
#define GUI_KEY_BACKSPACE    0x08
#define GUI_KEY_TAB          0x09
#define GUI_KEY_ENTER        0x0A

/* Mirrored verbatim into userspace by SYS_GUI_POLL_EVENT, so layout +
 * size MUST stay stable across kernel <-> user. */
struct gui_event {
    int     type;        /* GUI_EV_* */
    int     x;           /* mouse: client-area x; key: 0 */
    int     y;           /* mouse: client-area y; key: 0 */
    uint8_t button;      /* mouse buttons bitmask (LEFT/RIGHT/MIDDLE) */
    uint8_t key;         /* key event: ASCII code, 0 otherwise */
    uint8_t _pad[2];
};

/* ---- forward declarations ----------------------------------------- */

struct window;

/* ---- subsystem lifecycle ------------------------------------------ */

/* Bring up the compositor: install mouse callback, paint initial
 * desktop, but stay "inactive" until the first window is created. Must
 * be called after gfx_init() + mouse_init(). */
void gui_init(void);

/* Called from the idle loop. Recomposites + flips if anything dirty,
 * otherwise returns immediately. Cheap to call at ~100 Hz. */
void gui_tick(void);

/* True once at least one window is alive. The compositor only owns the
 * framebuffer while active -- when inactive, console_putc and friends
 * keep working as before. */
bool gui_active(void);

/* ---- desktop mode (milestone 12) ---------------------------------- */

/* Enter or leave desktop mode. In desktop mode the compositor stays
 * active even with zero windows -- it draws the wallpaper, the
 * taskbar at the bottom, and (if open) the launcher menu. Entering
 * desktop mode is the standard way to bring up the GUI before any
 * application has opened a window. Leaving it returns the framebuffer
 * to console_tick + the text shell. */
void gui_set_desktop_mode(bool on);
bool gui_in_desktop_mode(void);

/* ---- deferred app launch (milestone 12 + 13) ---------------------- *
 *
 * Push a program into the pid-0 launch queue. The actual spawn happens
 * from gui_tick() running on the idle thread -- so this is safe to
 * call from IRQ context, from a syscall dispatcher, or from the text
 * shell. The optional `arg` becomes argv[1] (argv[0] is always the
 * basename of `path`). Pass NULL for no argument.
 *
 * Returns 0 on success, -1 if the queue is full or args are bad.
 * Strings are copied immediately -- caller may free them right after. */
int gui_launch_enqueue_arg(const char *path, const char *arg);

/* Milestone 18: enqueue a launch with a specific sandbox profile name.
 * `sandbox` may be NULL/"" meaning "inherit parent's caps untouched".
 * Any other string is looked up in the profile table at spawn time
 * via cap_profile_apply; an unknown name is logged but not fatal. */
int gui_launch_enqueue_arg_profile(const char *path, const char *arg,
                                   const char *sandbox);

/* ---- dynamic launcher entries (milestone 16) ---------------------- *
 *
 * The desktop "Apps" menu is built from:
 *   - a compiled-in "system" slice (Settings / Terminal / Files /
 *     About / Widgets / Clock) that never changes, and
 *   - a package-manager-managed "user" slice that pkg_init() rebuilds
 *     from the .app descriptors under /data/apps on every boot, and
 *     after each `pkg install` or `pkg remove`.
 *
 * gui_launcher_register(label, path)
 *     Append a user entry. `label` is the menu text (truncated to fit
 *     GUI_LAUNCHER_LABEL_MAX); `path` is the ELF to spawn when the
 *     user clicks. Both strings are copied immediately. Returns 0 on
 *     success, -1 if the user-slice is full or args are bad.
 *
 * gui_launcher_reset_user()
 *     Drop every user-registered entry. Called by pkg_refresh_launcher
 *     before it re-scans /data/apps/.
 *
 * The bottom of the menu ("Logout") stays pinned regardless. */

#define GUI_LAUNCHER_LABEL_MAX   24
#define GUI_LAUNCHER_USER_MAX    8

int  gui_launcher_register(const char *label, const char *path);

/* Milestone 18: register a launcher entry with an explicit sandbox
 * profile. The profile name is remembered per-entry; clicking the
 * item in the menu enqueues the launch with that profile. NULL/""
 * is treated as "no profile" == inherit the launcher's caps. */
int  gui_launcher_register_with_profile(const char *label, const char *path,
                                        const char *sandbox);

/* M34D: same as ..._with_profile but additionally remembers a declared
 * capability list (comma-separated cap names from the .tpkg manifest).
 * The launcher passes that list through proc_spec.declared_caps so
 * spawning narrows caps to (parent & profile & declared). NULL/""
 * caps is equivalent to ..._with_profile. */
int  gui_launcher_register_with_profile_caps(const char *label,
                                             const char *path,
                                             const char *sandbox,
                                             const char *caps);

/* Look up the sandbox profile associated with a previously-registered
 * user-slice launcher entry by exact path match. Returns the stored
 * profile name (valid until gui_launcher_reset_user) or NULL if no
 * such entry exists or it was registered without a profile. */
const char *gui_launcher_sandbox_for_path(const char *path);

/* M34D: same as gui_launcher_sandbox_for_path but returns the declared
 * capability list (or NULL if none). */
const char *gui_launcher_caps_for_path(const char *path);

void gui_launcher_reset_user(void);

/* ---- window operations (used by syscall.c) ------------------------ */

/* Create a new top-level window with `client_w` x `client_h` pixel
 * client area and an optional title (NUL-terminated, copied). Returns
 * a window pointer (the syscall layer wraps it in a FILE_KIND_WINDOW
 * file struct + fd) or NULL on failure (out of slots / OOM). The
 * window is placed at a tiled offset so successive creates stagger. */
struct window *gui_window_create(int client_w, int client_h, const char *title);

/* Close a window: free its buffers, splice out of the z-order list,
 * release the slot. NULL-safe; multiple calls are tolerated. */
void gui_window_close(struct window *w);

/* Drawing operations -- all (x, y, w, h) are in CLIENT coordinates and
 * clipped to the client area. Each one only writes the per-window
 * back buffer; the compositor will pick up the change on the next
 * gui_window_flip(). Return 0 on success, -1 on bad args. */
int gui_window_fill(struct window *w, int x, int y, int rw, int rh,
                    uint32_t color);
/* M27C: source-over alpha fill. The window's backbuf stays XRGB;
 * the high byte of `argb` (0xAARRGGBB) is consumed as alpha during
 * the blend. */
int gui_window_fill_argb(struct window *w, int x, int y, int rw, int rh,
                         uint32_t argb);
int gui_window_text(struct window *w, int x, int y, const char *s,
                    uint32_t fg, uint32_t bg);
/* M27D: scaled bitmap text into a window backbuf.
 * scale = 1..N -- each source pixel becomes a scale-x-scale block.
 * smooth != 0  -- additionally lay a half-alpha wedge on each
 *                 diagonal corner the bitmap font leaves unfilled
 *                 (anti-aliased look without a TTF rasteriser).
 *                 Smoothing is a no-op for scale=1; the call falls
 *                 back to gui_window_text(). */
int gui_window_text_scaled(struct window *w, int x, int y, const char *s,
                           uint32_t fg, uint32_t bg, int scale, int smooth);

/* Mark the window dirty so the next gui_tick() recomposites the
 * framebuffer. Cheap to call multiple times per frame. */
int gui_window_flip(struct window *w);

/* M27E: hint the compositor that a screen-coordinate region needs
 * presenting next frame. The compositor still does a correct full
 * repaint into the back buffer; the hint controls only how MUCH of
 * the back buffer is pushed to the front buffer (memcpy / virtio-gpu
 * TRANSFER+FLUSH). Multiple calls in one frame union together.
 *
 * gui_invalidate_full() forces a full present (used after window
 * create/destroy, z-order change, mode switch). gui_invalidate_rect
 * with a fully-covering rect is equivalent.
 *
 * gui_invalidate_stats(full, partial) reads back the cumulative
 * frame counts the compositor has produced -- used by the M27E
 * test harness to prove dirty-rect optimisation actually happened.
 * NULL pointers are tolerated. */
void gui_invalidate_rect (int x, int y, int w, int h);
void gui_invalidate_full (void);
void gui_invalidate_stats(uint64_t *out_full, uint64_t *out_partial);

/* Pop one event off the window's event queue into *out. Returns 1 if
 * an event was consumed, 0 if the queue is empty. Non-blocking. */
int gui_window_poll_event(struct window *w, struct gui_event *out);

/* ---- keyboard plumbing (milestone 11) ----------------------------- */

/* Called from the keyboard IRQ when the GUI is active. Enqueues a
 * GUI_EV_KEY event into the topmost window (the implicit keyboard
 * focus owner). `c` is an ASCII byte (printables, plus the
 * GUI_KEY_BACKSPACE / GUI_KEY_TAB / GUI_KEY_ENTER specials).
 * No-op if no window is up. */
void gui_post_key(uint8_t c);

/* ---- desktop activity tracing ------------------------------------- *
 *
 * Lightweight kprintf-based event trace. Every event line goes to
 * serial (and the framebuffer console when text mode is up) prefixed
 * with `[trace t=<pit-ticks> pid=<n>] ` so the stream can be grep'd
 * out of serial.log later. The trace is OFF by default. The shell
 * `trace [on|off|verbose|status]` command flips the level, and the
 * `desktop` builtin auto-enables level 1 so the next launcher click
 * already shows up in the log.
 *
 * Levels:
 *   0  off.
 *   1  desktop control flow: desktop mode, mouse hit-tests on UI
 *      chrome (start button, launcher items, taskbar tabs, close X),
 *      launch queue + reaping, window create/close, signal sends,
 *      and gui_tick scheduler yields.
 *   2  level 1 + per-call GUI syscalls (gui_create / fill / text /
 *      flip / poll_event). Spammy: a busy app will emit ~100/sec.
 *
 * The logf entry point is variadic-safe (printf-style, see kprintf in
 * printk.h for supported conversions). Each call adds a trailing '\n'
 * automatically -- callers must NOT include their own. */

#define GUI_TRACE_OFF       0
#define GUI_TRACE_NORMAL    1
#define GUI_TRACE_VERBOSE   2

void gui_trace_set(int level);
int  gui_trace_level(void);

__attribute__((format(printf, 1, 2)))
void gui_trace_logf(const char *fmt, ...);

/* ---- emergency / debug hotkeys (milestone 12) --------------------- *
 *
 * Both routines are SAFE TO CALL FROM IRQ CONTEXT (the keyboard IRQ
 * is the primary caller). They only read shared state and emit kprintf
 * lines; the F12 escape additionally toggles g.desktop_mode off.
 *
 * gui_dump_status(reason)
 *     Print a one-shot snapshot of the compositor: desktop/active
 *     flags, mouse position + button state, launch-queue depth, every
 *     tracked PID + its scheduler state, every live window with its
 *     z-order index + owner pid + queue depth + position. The label
 *     `reason` is included verbatim so different call sites
 *     (heartbeat / hotkey / emergency exit) are distinguishable.
 *
 * gui_emergency_exit(reason)
 *     Force-exit desktop mode so the user can fall back to the text
 *     shell even if every visible UI element has stopped responding
 *     to mouse clicks. Also dumps status (so the log captures the
 *     state that triggered the bail-out) and SIGINTs every tracked
 *     desktop-launched app so they tear themselves down cleanly.
 *     Safe to call multiple times -- a no-op if desktop mode is
 *     already off and no apps are tracked. */
void gui_dump_status(const char *reason);
void gui_emergency_exit(const char *reason);

#endif /* TOBYOS_GUI_H */
