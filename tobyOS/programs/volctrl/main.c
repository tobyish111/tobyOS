/* volctrl/main.c -- Volume control popup for tobyOS.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The volume model (master level,
 * mute, per-application sliders) is unchanged from the original raw-gui_*
 * version -- only the rendering/input layer moved to TobyTK so the popup
 * shares the system theme and slider/button behaviour.
 */

#include <toby/tk.h>
#include <stdio.h>
#include <string.h>

/* ── volume state (unchanged model) ───────────────────────────────────── */
static int g_master_vol = 75;
static int g_muted;

#define MAX_APPS 8
struct app_vol {
    char name[24];
    int  volume;
    int  pid;
};
static struct app_vol g_apps[MAX_APPS];
static int g_app_count;

static void init_demo_apps(void) {
    g_app_count = 3;
    strncpy(g_apps[0].name, "Media Player", 23); g_apps[0].volume = 80; g_apps[0].pid = 10;
    strncpy(g_apps[1].name, "Browser", 23);      g_apps[1].volume = 60; g_apps[1].pid = 15;
    strncpy(g_apps[2].name, "Terminal", 23);     g_apps[2].volume = 40; g_apps[2].pid = 20;
}

/* ── TobyTK UI ─────────────────────────────────────────────────────────── */
#define C_MUTE   0x00CC4444u   /* semantic: muted (red)   */
#define C_UNMUTE 0x0044AA44u   /* semantic: active (green) */

static struct tk_window win;
static struct tk_widget *w_mute_btn;
static struct tk_widget *w_master_pct;
static struct tk_widget *w_master_slider;
static struct tk_widget *w_app_pct[MAX_APPS];

static void set_pct(struct tk_widget *lbl, int v) {
    char b[8];
    snprintf(b, sizeof(b), "%d%%", v);
    tk_set_text(&win, lbl, b);
}

static void refresh_master(void) {
    set_pct(w_master_pct, g_muted ? 0 : g_master_vol);
    tk_set_text(&win, w_mute_btn, g_muted ? "Muted" : "Unmute");
    tk_colors(w_mute_btn, g_muted ? C_MUTE : C_UNMUTE, 0x00FFFFFF);
    tk_redraw(&win);
}

static void on_mute(struct tk_window *w, struct tk_widget *b) {
    (void)w; (void)b;
    g_muted = !g_muted;
    refresh_master();
}

static void on_master(struct tk_window *w, struct tk_widget *s) {
    (void)w;
    g_master_vol = s->value;
    if (g_muted) { g_muted = 0; tk_set_text(&win, w_mute_btn, "Unmute");
                   tk_colors(w_mute_btn, C_UNMUTE, 0x00FFFFFF); }
    set_pct(w_master_pct, g_master_vol);
}

static void on_app(struct tk_window *w, struct tk_widget *s) {
    (void)w;
    int i = (int)(long)s->user;
    if (i < 0 || i >= g_app_count) return;
    g_apps[i].volume = s->value;
    set_pct(w_app_pct[i], g_apps[i].volume);
}

static void on_key(struct tk_window *w, struct tk_event *ev) {
    if (ev->key == 'q' || ev->key == 'Q' || ev->key == 27) tk_quit(w);
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    g_muted = 0;
    init_demo_apps();

    if (tk_window_open(&win, 300, 430, "Volume") != 0)
        return 1;
    tk_on_key(&win, on_key);

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 14);
    root->gap = 8;

    /* header */
    tk_bold(tk_font(tk_label(&win, root, "Volume"), 20));
    tk_separator(&win, root);

    /* mute toggle + master percentage */
    {
        struct tk_widget *row = tk_hbox(&win, root, 8);
        w_mute_btn = tk_button(&win, row, g_muted ? "Muted" : "Unmute", on_mute);
        tk_colors(w_mute_btn, g_muted ? C_MUTE : C_UNMUTE, 0x00FFFFFF);
        tk_grow(tk_label(&win, row, ""), 1);
        w_master_pct = tk_align(tk_font(tk_label(&win, row, ""), 16), TK_ALIGN_RIGHT);
    }

    /* master slider */
    tk_colors(tk_label(&win, root, "Master Volume"), 0, win.theme.text_dim);
    w_master_slider = tk_slider(&win, root, 0, 100, g_master_vol);
    w_master_slider->on_change = on_master;
    tk_separator(&win, root);

    /* per-application sliders */
    tk_colors(tk_label(&win, root, "Application Volume"), 0, win.theme.text_dim);
    for (int i = 0; i < g_app_count; i++) {
        struct tk_widget *panel = tk_vbox(&win, root, 2);
        tk_pad(panel, 8);
        tk_colors(panel, win.theme.panel_bg, 0);
        struct tk_widget *hdr = tk_hbox(&win, panel, 8);
        tk_grow(tk_label(&win, hdr, g_apps[i].name), 1);
        w_app_pct[i] = tk_align(tk_label(&win, hdr, ""), TK_ALIGN_RIGHT);
        struct tk_widget *s = tk_slider(&win, panel, 0, 100, g_apps[i].volume);
        s->user = (void *)(long)i;
        s->on_change = on_app;
        set_pct(w_app_pct[i], g_apps[i].volume);
    }

    /* output device */
    tk_separator(&win, root);
    tk_colors(tk_label(&win, root, "Output Device"), 0, win.theme.text_dim);
    {
        struct tk_widget *dev = tk_panel(&win, root);
        tk_pad(dev, 8);
        tk_colors(dev, win.theme.panel_bg, 0);
        tk_label(&win, dev, "Speakers (Intel HDA)");
    }

    refresh_master();
    return tk_run(&win);
}
