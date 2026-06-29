/* programs/updater/main.c -- System Update client for tobyOS.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The update state machine and its
 * time-driven progress (ST_CHECKING/DOWNLOADING/INSTALLING) are unchanged from
 * the original raw-gui_* version; only the rendering moved to TobyTK. Because
 * progress animates over time, the app drives its own loop (tk_pump + paced
 * usleep) and rebuilds the content panel whenever the state changes, updating
 * the live progress widget in place between rebuilds.
 */

#include <toby/tk.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SYS_CLOCK_MS 48
static inline long sys_clock_ms(void) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "0"((long)SYS_CLOCK_MS)
        : "rcx", "r11", "memory"); return r;
}

/* ── semantic colours (kept from the original identity) ───────────────── */
#define COL_ACCENT 0x0089B4FAu
#define COL_GREEN  0x00A6E3A1u
#define COL_RED    0x00F38BA8u

/* ── update state machine (unchanged) ─────────────────────────────────── */
enum update_state {
    ST_IDLE, ST_CHECKING, ST_AVAILABLE, ST_DOWNLOADING,
    ST_READY, ST_INSTALLING, ST_UP_TO_DATE,
};

static enum update_state g_state = ST_IDLE;
static int  g_progress = 0;
static long g_check_start = 0;
static long g_dl_start = 0;

static const char *g_cur_version = "1.0.0";
static const char *g_new_version = "1.1.0";

static const char *g_changelog[] = {
    "- Improved WiFi management",
    "- Added WireGuard VPN support",
    "- New firewall configuration",
    "- Device Manager with hot-plug",
    "- System Search / File Indexing",
    "- System Restore Points",
    NULL
};

static void tick(void) {
    long now = sys_clock_ms();
    if (g_state == ST_CHECKING) {
        g_progress = (int)((now - g_check_start) / 20);
        if (g_progress >= 100) { g_progress = 100; g_state = ST_AVAILABLE; }
    }
    if (g_state == ST_DOWNLOADING) {
        g_progress = (int)((now - g_dl_start) / 50);
        if (g_progress >= 100) { g_progress = 100; g_state = ST_READY; }
    }
    if (g_state == ST_INSTALLING) {
        g_progress = (int)((now - g_dl_start) / 30);
        if (g_progress >= 100) g_progress = 100;
    }
}

/* ── TobyTK UI ─────────────────────────────────────────────────────────── */
static struct tk_window win;
static struct tk_widget *content;
static int   content_base;
static struct tk_widget *w_prog;   /* live progress (NULL unless animating) */
static struct tk_widget *w_size;   /* live "N MB / 24 MB" (download only)   */

static void rebuild(void);

static void on_check(struct tk_window *w, struct tk_widget *b) {
    (void)w; (void)b;
    g_state = ST_CHECKING; g_progress = 0; g_check_start = sys_clock_ms(); rebuild();
}
static void on_install(struct tk_window *w, struct tk_widget *b) {
    (void)w; (void)b;
    g_state = ST_DOWNLOADING; g_progress = 0; g_dl_start = sys_clock_ms(); rebuild();
}
static void on_later(struct tk_window *w, struct tk_widget *b) {
    (void)w; (void)b; g_state = ST_IDLE; rebuild();
}
static void on_restart(struct tk_window *w, struct tk_widget *b) {
    (void)w; (void)b;
    g_state = ST_INSTALLING; g_progress = 0; g_dl_start = sys_clock_ms(); rebuild();
}

static void rebuild(void) {
    tk_clear_children(&win, content);
    tk_rewind(&win, content_base);
    w_prog = w_size = 0;

    switch (g_state) {
    case ST_IDLE:
        tk_align(tk_size(tk_colors(tk_button(&win, content, "Check for Updates", on_check),
                                   COL_ACCENT, 0x00FFFFFF), 180, 30), TK_ALIGN_LEFT);
        tk_colors(tk_label(&win, content, "No updates have been checked."),
                  0, win.theme.text_dim);
        break;
    case ST_CHECKING:
        tk_label(&win, content, "Checking for updates...");
        w_prog = tk_progress(&win, content, g_progress, 100);
        break;
    case ST_UP_TO_DATE:
        tk_colors(tk_label(&win, content, "Your system is up to date!"), 0, COL_GREEN);
        tk_size(tk_button(&win, content, "Check Again", on_check), 160, 30);
        break;
    case ST_AVAILABLE: {
        char ver[48];
        snprintf(ver, sizeof(ver), "Update Available: v%s", g_new_version);
        tk_bold(tk_colors(tk_label(&win, content, ver), 0, COL_GREEN));
        tk_colors(tk_label(&win, content, "Changelog:"), 0, COL_ACCENT);
        for (int i = 0; g_changelog[i]; i++)
            tk_label(&win, content, g_changelog[i]);
        struct tk_widget *row = tk_hbox(&win, content, 8);
        tk_colors(tk_button(&win, row, "Install Update", on_install), COL_GREEN, 0x00181825);
        tk_button(&win, row, "Later", on_later);
        break;
    }
    case ST_DOWNLOADING: {
        char ver[48];
        snprintf(ver, sizeof(ver), "Downloading v%s ...", g_new_version);
        tk_label(&win, content, ver);
        w_prog = tk_progress(&win, content, g_progress, 100);
        w_size = tk_colors(tk_label(&win, content, "0 MB / 24 MB"), 0, win.theme.text_dim);
        break;
    }
    case ST_READY:
        tk_colors(tk_label(&win, content, "Download complete!"), 0, COL_GREEN);
        tk_label(&win, content, "A system restart is required to apply the update.");
        {
            struct tk_widget *row = tk_hbox(&win, content, 8);
            tk_colors(tk_button(&win, row, "Restart Now", on_restart), COL_RED, 0x00FFFFFF);
            tk_button(&win, row, "Restart Later", on_later);
        }
        break;
    case ST_INSTALLING:
        tk_colors(tk_label(&win, content, "Installing update... Do not power off."), 0, COL_RED);
        w_prog = tk_progress(&win, content, g_progress, 100);
        break;
    }
    tk_grow(tk_label(&win, content, ""), 1);   /* push content up */
    tk_redraw(&win);
}

static void update_live(void) {
    if (w_prog) tk_set_value(&win, w_prog, g_progress);
    if (w_size) {
        char sz[32];
        snprintf(sz, sizeof(sz), "%d MB / 24 MB", g_progress * 24 / 100);
        tk_set_text(&win, w_size, sz);
    }
}

static void on_key(struct tk_window *w, struct tk_event *ev) {
    if (ev->key == 'q' || ev->key == 'Q' || ev->key == 27) tk_quit(w);
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (tk_window_open(&win, 500, 350, "System Update") != 0)
        return 1;
    tk_on_key(&win, on_key);

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 0);
    root->gap = 0;

    /* header band */
    {
        struct tk_widget *hdr = tk_vbox(&win, root, 0);
        tk_pad(hdr, 12);
        tk_colors(hdr, win.theme.panel_bg, 0);
        tk_bold(tk_colors(tk_label(&win, hdr, "tobyOS System Update"), 0, COL_ACCENT));
    }

    /* body */
    struct tk_widget *body = tk_vbox(&win, root, 8);
    tk_pad(body, 16);
    tk_grow(body, 1);
    {
        char ver[48];
        snprintf(ver, sizeof(ver), "Current Version: %s", g_cur_version);
        tk_label(&win, body, ver);
    }
    content = tk_vbox(&win, body, 6);
    tk_grow(content, 1);
    content_base = tk_checkpoint(&win);

    /* footer band */
    {
        struct tk_widget *ft = tk_vbox(&win, root, 0);
        tk_pad(ft, 4);
        tk_colors(ft, win.theme.panel_bg, 0);
        tk_colors(tk_label(&win, ft, "tobyOS Update Service"), 0, win.theme.text_dim);
    }

    rebuild();

    /* self-paced loop: events + time-driven progress animation */
    enum update_state prev = g_state;
    for (;;) {
        if (tk_pump(&win)) break;
        tick();
        if (g_state != prev) { rebuild(); prev = g_state; }
        else update_live();
        tk_redraw(&win);
        usleep(30000);
    }
    return 0;
}
