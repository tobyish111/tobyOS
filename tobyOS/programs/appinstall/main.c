/* appinstall/main.c -- Graphical installer wizard for .tpkg packages.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The wizard model (4 steps,
 * package parsing, time-driven install progress, and the final SYS_EXEC
 * launch) is unchanged from the original raw-gui_* version; only the rendering
 * moved to TobyTK. The body is rebuilt (tk_checkpoint / tk_rewind) on each step
 * change and the installing-step widgets are updated live from a self-paced
 * loop (tk_pump + paced usleep).
 */

#include <toby/tk.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SYS_EXIT     0
#define SYS_EXEC     20
#define SYS_CLOCK_MS 48
static inline long sc0(long n){long r;__asm__ volatile("syscall":"=a"(r):"0"(n):"rcx","r11","memory");return r;}
static inline long sc2(long n,long a,long b){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b):"rcx","r11","memory");return r;}
static long sys_clock_ms(void){ return sc0(SYS_CLOCK_MS); }

/* ── semantic colours ─────────────────────────────────────────────────── */
#define C_ACCENT 0x005588FFu
#define C_GREEN  0x0044CC44u
#define C_WARN   0x00FFAA33u

/* ── installer state (unchanged model) ────────────────────────────────── */
static int  g_step;          /* 0=info, 1=location, 2=installing, 3=done */
static int  g_progress;
static long g_install_start;

static char g_pkg_name[32]    = "HelloApp";
static char g_pkg_version[16] = "1.0.0";
static char g_pkg_desc[128]   = "A simple hello world application for tobyOS.";
static char g_pkg_path[256];
static char g_install_dir[128] = "/apps/HelloApp/";
static char g_pkg_binary[128]  = "/apps/HelloApp/helloapp";

#define MAX_PERMS 8
static const char *g_permissions[MAX_PERMS];
static int g_perm_count;

static void parse_package(const char *path) {
    if (path && path[0]) {
        strncpy(g_pkg_path, path, sizeof(g_pkg_path) - 1);
        const char *slash = strrchr(path, '/');
        const char *name = slash ? slash + 1 : path;
        const char *dot = strstr(name, ".tpkg");
        size_t nlen = dot ? (size_t)(dot - name) : strlen(name);
        if (nlen >= sizeof(g_pkg_name)) nlen = sizeof(g_pkg_name) - 1;
        memcpy(g_pkg_name, name, nlen);
        g_pkg_name[nlen] = '\0';
        snprintf(g_install_dir, sizeof(g_install_dir), "/apps/%s/", g_pkg_name);
        snprintf(g_pkg_binary, sizeof(g_pkg_binary), "/apps/%s/%s", g_pkg_name, g_pkg_name);
    }
    g_perm_count = 3;
    g_permissions[0] = "GUI (create windows)";
    g_permissions[1] = "FILE_READ (read files)";
    g_permissions[2] = "NETWORK (internet access)";
}

/* ── TobyTK UI ─────────────────────────────────────────────────────────── */
static struct tk_window win;
static struct tk_widget *body;
static int   body_base;
static struct tk_widget *w_prog, *w_status, *w_pct;

static void rebuild(void);

static void on_next  (struct tk_window *w, struct tk_widget *b){ (void)w; (void)b; g_step = 1; rebuild(); }
static void on_back  (struct tk_window *w, struct tk_widget *b){ (void)w; (void)b; g_step = 0; rebuild(); }
static void on_cancel(struct tk_window *w, struct tk_widget *b){ (void)w; (void)b; tk_quit(&win); }
static void on_install(struct tk_window *w, struct tk_widget *b){
    (void)w; (void)b;
    g_step = 2; g_progress = 0; g_install_start = sys_clock_ms(); rebuild();
}
static void on_launch(struct tk_window *w, struct tk_widget *b){
    (void)w; (void)b;
    sc2(SYS_EXEC, (long)g_pkg_binary, 0);
    tk_quit(&win);
}

static void kv_row(struct tk_widget *parent, const char *k, const char *v) {
    struct tk_widget *row = tk_hbox(&win, parent, 8);
    tk_size(tk_colors(tk_label(&win, row, k), 0, win.theme.text_dim), 90, 0);
    tk_label(&win, row, v);
}

static const char *install_status(void) {
    if (g_progress < 30) return "Verifying package integrity...";
    if (g_progress < 60) return "Extracting files...";
    if (g_progress < 90) return "Registering application...";
    return "Finalizing...";
}

static void step_dots(struct tk_widget *parent) {
    struct tk_widget *row = tk_hbox(&win, parent, 6);
    for (int s = 0; s < 4; s++) {
        struct tk_widget *d = tk_panel(&win, row);
        tk_size(d, 12, 12);
        tk_colors(d, s <= g_step ? C_ACCENT : 0x00444455, 0);
    }
}

static void rebuild(void) {
    tk_clear_children(&win, body);
    tk_rewind(&win, body_base);
    w_prog = w_status = w_pct = 0;

    step_dots(body);

    switch (g_step) {
    case 0: {
        tk_bold(tk_font(tk_label(&win, body, "Install Package"), 18));
        tk_colors(tk_label(&win, body, "Step 1 of 4: Review Package"), 0, win.theme.text_dim);
        struct tk_widget *panel = tk_vbox(&win, body, 4);
        tk_pad(panel, 12); tk_colors(panel, win.theme.panel_bg, 0); tk_grow(panel, 1);
        kv_row(panel, "Name:", g_pkg_name);
        kv_row(panel, "Version:", g_pkg_version);
        tk_colors(tk_label(&win, panel, "Description:"), 0, win.theme.text_dim);
        tk_label(&win, panel, g_pkg_desc);
        tk_separator(&win, panel);
        tk_colors(tk_label(&win, panel, "Permissions Required:"), 0, C_WARN);
        for (int i = 0; i < g_perm_count; i++)
            tk_colors(tk_label(&win, panel, g_permissions[i]), 0, win.theme.text_dim);
        struct tk_widget *row = tk_hbox(&win, body, 8);
        tk_button(&win, row, "Cancel", on_cancel);
        tk_grow(tk_label(&win, row, ""), 1);
        tk_colors(tk_button(&win, row, "Next >", on_next), C_ACCENT, 0x00FFFFFF);
        break;
    }
    case 1: {
        tk_bold(tk_font(tk_label(&win, body, "Install Location"), 18));
        tk_colors(tk_label(&win, body, "Step 2 of 4: Choose Location"), 0, win.theme.text_dim);
        tk_colors(tk_label(&win, body, "Install directory:"), 0, win.theme.text_dim);
        tk_field(&win, body, g_install_dir);
        kv_row(body, "Required:", "128 KB");
        kv_row(body, "Available:", "3.8 MB");
        tk_checkbox(&win, body, "Create start menu entry", 1);
        tk_checkbox(&win, body, "Create desktop shortcut", 1);
        tk_checkbox(&win, body, "Register file associations", 0);
        tk_grow(tk_label(&win, body, ""), 1);
        struct tk_widget *row = tk_hbox(&win, body, 8);
        tk_button(&win, row, "Cancel", on_cancel);
        tk_button(&win, row, "< Back", on_back);
        tk_grow(tk_label(&win, row, ""), 1);
        tk_colors(tk_button(&win, row, "Install", on_install), C_GREEN, 0x00181825);
        break;
    }
    case 2:
        tk_bold(tk_font(tk_label(&win, body, "Installing..."), 18));
        tk_colors(tk_label(&win, body, "Step 3 of 4: Installation"), 0, win.theme.text_dim);
        w_status = tk_label(&win, body, install_status());
        w_prog = tk_progress(&win, body, g_progress, 100);
        w_pct = tk_align(tk_label(&win, body, "0%"), TK_ALIGN_CENTER);
        tk_colors(tk_label(&win, body, "Installing to:"), 0, win.theme.text_dim);
        tk_label(&win, body, g_install_dir);
        tk_grow(tk_label(&win, body, ""), 1);
        break;
    case 3: {
        tk_bold(tk_font(tk_colors(tk_label(&win, body, "Installation Complete!"), 0, C_GREEN), 18));
        tk_colors(tk_label(&win, body, "Step 4 of 4: Finished"), 0, win.theme.text_dim);
        struct tk_widget *panel = tk_vbox(&win, body, 4);
        tk_pad(panel, 12); tk_colors(panel, win.theme.panel_bg, 0); tk_grow(panel, 1);
        char line[160];
        snprintf(line, sizeof(line), "%s has been successfully installed.", g_pkg_name);
        tk_label(&win, panel, line);
        snprintf(line, sizeof(line), "Location: %s", g_install_dir);
        tk_colors(tk_label(&win, panel, line), 0, win.theme.text_dim);
        tk_colors(tk_label(&win, panel, "Menu entry and shortcut created."), 0, win.theme.text_dim);
        struct tk_widget *row = tk_hbox(&win, body, 8);
        tk_button(&win, row, "Close", on_cancel);
        tk_grow(tk_label(&win, row, ""), 1);
        tk_colors(tk_button(&win, row, "Launch", on_launch), C_GREEN, 0x00181825);
        break;
    }
    }
    tk_redraw(&win);
}

static void update_live(void) {
    if (w_prog)   tk_set_value(&win, w_prog, g_progress);
    if (w_status) tk_set_text(&win, w_status, install_status());
    if (w_pct) { char p[8]; snprintf(p, sizeof(p), "%d%%", g_progress); tk_set_text(&win, w_pct, p); }
}

static void on_key(struct tk_window *w, struct tk_event *ev) {
    if (ev->key == 27) tk_quit(w);
}

int main(int argc, char **argv) {
    g_step = 0; g_progress = 0; g_install_start = 0;
    parse_package(argc > 1 ? argv[1] : "/repo/helloapp.tpkg");

    if (tk_window_open(&win, 500, 420, "App Installer") != 0)
        return 1;
    tk_on_key(&win, on_key);

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 16);
    root->gap = 8;
    body = tk_vbox(&win, root, 8);
    tk_grow(body, 1);
    body_base = tk_checkpoint(&win);
    rebuild();

    int prev = g_step;
    for (;;) {
        if (tk_pump(&win)) break;
        if (g_step == 2) {
            g_progress = (int)((sys_clock_ms() - g_install_start) / 20);
            if (g_progress > 100) { g_progress = 100; g_step = 3; }
        }
        if (g_step != prev) { rebuild(); prev = g_step; }
        else { update_live(); tk_redraw(&win); }
        usleep(30000);
    }
    return 0;
}
