/* wifipicker/main.c -- WiFi picker system tray popup for tobyOS.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The network model (scan results,
 * saved list, airplane mode, connect/password flow) is unchanged from the
 * original raw-gui_* version; only the rendering/input layer moved to TobyTK.
 * The Networks / Saved tabs and the password dialog are built by rebuilding a
 * single content container (tk_checkpoint / tk_rewind) on each state change, so
 * the popup shares the system theme, table and field widgets.
 */

#include <toby/tk.h>
#include <stdio.h>
#include <string.h>

/* ── network model (unchanged) ────────────────────────────────────────── */
#define MAX_NETWORKS 12
#define MAX_SAVED    8

struct wifi_net {
    char ssid[33];
    int  signal;     /* 0-100 */
    int  security;   /* 0=open, 1=WEP, 2=WPA2, 3=WPA3 */
    int  connected;
};

static struct wifi_net g_nets[MAX_NETWORKS];
static int  g_net_count;
static char g_saved[MAX_SAVED][33];
static int  g_saved_count;
static int  g_airplane_mode;
static int  g_selected = -1;
static int  g_show_password;
static int  g_show_saved;

static const char *sec_name(int s) {
    return s == 1 ? "WEP" : s == 2 ? "WPA2" : s == 3 ? "WPA3" : "Open";
}

static void scan_networks(void) {
    g_net_count = 0;
    struct { const char *ssid; int sig; int sec; int conn; } demo[] = {
        {"HomeNetwork",     85, 2, 1},
        {"CoffeeShop_WiFi", 72, 0, 0},
        {"Office_5G",       65, 2, 0},
        {"Neighbor_Net",    45, 2, 0},
        {"FreeWiFi",        38, 0, 0},
        {"IoT_Device",      30, 1, 0},
        {"Hidden_Net",      22, 3, 0},
    };
    for (int i = 0; i < 7 && g_net_count < MAX_NETWORKS; i++) {
        struct wifi_net *n = &g_nets[g_net_count++];
        strncpy(n->ssid, demo[i].ssid, 32);
        n->ssid[32] = '\0';
        n->signal = demo[i].sig;
        n->security = demo[i].sec;
        n->connected = demo[i].conn;
    }
    g_saved_count = 0;
    strncpy(g_saved[g_saved_count++], "HomeNetwork", 32);
    strncpy(g_saved[g_saved_count++], "Office_5G", 32);
}

/* ── table row caches (cell accessors return these) ───────────────────── */
static char net_rows[MAX_NETWORKS][3][40];
static char saved_rows[MAX_SAVED][2][40];

static void build_net_rows(void) {
    for (int i = 0; i < g_net_count; i++) {
        strncpy(net_rows[i][0], g_nets[i].ssid, 39); net_rows[i][0][39] = '\0';
        if (g_nets[i].connected)
            snprintf(net_rows[i][1], 40, "%s (on)", sec_name(g_nets[i].security));
        else
            snprintf(net_rows[i][1], 40, "%s", sec_name(g_nets[i].security));
        snprintf(net_rows[i][2], 40, "%d%%", g_nets[i].signal);
    }
}

static int saved_is_connected(int i) {
    for (int j = 0; j < g_net_count; j++)
        if (g_nets[j].connected && strcmp(g_nets[j].ssid, g_saved[i]) == 0) return 1;
    return 0;
}

static void build_saved_rows(void) {
    for (int i = 0; i < g_saved_count; i++) {
        strncpy(saved_rows[i][0], g_saved[i], 39); saved_rows[i][0][39] = '\0';
        snprintf(saved_rows[i][1], 40, "%s", saved_is_connected(i) ? "Connected" : "Saved");
    }
}

static const char *net_cell(void *u, int r, int c) {
    (void)u;
    if (r < 0 || r >= g_net_count || c < 0 || c > 2) return "";
    return net_rows[r][c];
}
static const char *saved_cell(void *u, int r, int c) {
    (void)u;
    if (r < 0 || r >= g_saved_count || c < 0 || c > 1) return "";
    return saved_rows[r][c];
}

static const char *const net_headers[]   = { "Network", "Security", "Signal" };
static const int         net_widths[]    = { 150, 80, 60 };
static const char *const saved_headers[] = { "Saved Network", "Status" };
static const int         saved_widths[]  = { 190, 90 };

/* ── TobyTK UI ─────────────────────────────────────────────────────────── */
#define C_ACCENT 0x005588FFu

static struct tk_window win;
static struct tk_widget *content;
static int   content_base;
static struct tk_widget *w_pw_field;
static struct tk_widget *w_saved_tbl;
static struct tk_widget *w_net_tbl;

static void rebuild(void);

static void on_scan   (struct tk_window *w, struct tk_widget *b) { (void)w; (void)b; scan_networks(); rebuild(); }
static void on_air    (struct tk_window *w, struct tk_widget *c) { (void)w; g_airplane_mode = tk_checked(c); rebuild(); }
static void on_tab_net(struct tk_window *w, struct tk_widget *b) { (void)w; (void)b; g_show_saved = 0; rebuild(); }
static void on_tab_sav(struct tk_window *w, struct tk_widget *b) { (void)w; (void)b; g_show_saved = 1; rebuild(); }

static void on_net_select(struct tk_window *w, struct tk_widget *t) {
    (void)w;
    int i = tk_table_selected(t);
    if (i < 0 || i >= g_net_count) return;
    g_selected = i;
    if (g_nets[i].connected) return;
    if (g_nets[i].security == 0) {
        for (int j = 0; j < g_net_count; j++) g_nets[j].connected = (j == i);
        rebuild();
    } else {
        g_show_password = 1;
        rebuild();
    }
}

static void do_connect(void) {
    if (g_selected >= 0 && g_selected < g_net_count) {
        for (int j = 0; j < g_net_count; j++) g_nets[j].connected = (j == g_selected);
        /* remember it */
        int found = 0;
        for (int j = 0; j < g_saved_count; j++)
            if (strcmp(g_saved[j], g_nets[g_selected].ssid) == 0) { found = 1; break; }
        if (!found && g_saved_count < MAX_SAVED) {
            strncpy(g_saved[g_saved_count], g_nets[g_selected].ssid, 32);
            g_saved[g_saved_count][32] = '\0';
            g_saved_count++;
        }
    }
    g_show_password = 0;
    rebuild();
}
static void on_connect(struct tk_window *w, struct tk_widget *b) { (void)w; (void)b; do_connect(); }
static void on_cancel (struct tk_window *w, struct tk_widget *b) { (void)w; (void)b; g_show_password = 0; rebuild(); }

static void on_forget(struct tk_window *w, struct tk_widget *b) {
    (void)w; (void)b;
    int i = tk_table_selected(w_saved_tbl);
    if (i < 0 || i >= g_saved_count) return;
    memmove(&g_saved[i], &g_saved[i + 1], (g_saved_count - i - 1) * sizeof(g_saved[0]));
    g_saved_count--;
    rebuild();
}

static void tab_button(struct tk_widget *row, const char *text, int active, tk_cb cb) {
    struct tk_widget *b = tk_button(&win, row, text, cb);
    tk_grow(b, 1);
    if (active) tk_colors(b, C_ACCENT, 0x00FFFFFF);
}

static void rebuild(void) {
    tk_clear_children(&win, content);
    tk_rewind(&win, content_base);
    w_saved_tbl = w_net_tbl = w_pw_field = 0;

    if (g_airplane_mode) {
        tk_grow(tk_label(&win, content, ""), 1);
        tk_align(tk_bold(tk_label(&win, content, "Airplane Mode On")), TK_ALIGN_CENTER);
        tk_align(tk_colors(tk_label(&win, content, "WiFi is disabled"), 0, win.theme.text_dim),
                 TK_ALIGN_CENTER);
        tk_grow(tk_label(&win, content, ""), 1);
    } else if (g_show_password) {
        char title[64];
        snprintf(title, sizeof(title), "Connect to %s",
                 g_selected >= 0 ? g_nets[g_selected].ssid : "");
        tk_bold(tk_label(&win, content, title));
        tk_colors(tk_label(&win, content, "Password:"), 0, win.theme.text_dim);
        w_pw_field = tk_field(&win, content, "");
        w_pw_field->on_click = on_connect;   /* Enter connects */
        struct tk_widget *row = tk_hbox(&win, content, 8);
        tk_colors(tk_button(&win, row, "Connect", on_connect), C_ACCENT, 0x00FFFFFF);
        tk_button(&win, row, "Cancel", on_cancel);
        tk_grow(tk_label(&win, content, ""), 1);
    } else {
        struct tk_widget *tabs = tk_hbox(&win, content, 6);
        tab_button(tabs, "Networks", !g_show_saved, on_tab_net);
        tab_button(tabs, "Saved",     g_show_saved, on_tab_sav);

        if (g_show_saved) {
            build_saved_rows();
            w_saved_tbl = tk_table(&win, content, saved_headers, saved_widths, 2);
            tk_table_rows(&win, w_saved_tbl, g_saved_count, saved_cell, 0);
            tk_grow(w_saved_tbl, 1);
            tk_button(&win, content, "Forget selected", on_forget);
        } else {
            build_net_rows();
            w_net_tbl = tk_table(&win, content, net_headers, net_widths, 3);
            tk_table_rows(&win, w_net_tbl, g_net_count, net_cell, 0);
            w_net_tbl->on_change = on_net_select;
            tk_grow(w_net_tbl, 1);
        }
    }
    tk_redraw(&win);
}

static void on_key(struct tk_window *w, struct tk_event *ev) {
    if (ev->key == 'q' || ev->key == 'Q' || ev->key == 27) tk_quit(w);
}

int main(void) {
    if (tk_window_open(&win, 340, 480, "WiFi") != 0)
        return 1;
    tk_on_key(&win, on_key);

    scan_networks();

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 12);
    root->gap = 8;

    /* header: title + scan + airplane toggle */
    struct tk_widget *hdr = tk_hbox(&win, root, 8);
    tk_grow(tk_bold(tk_font(tk_label(&win, hdr, "WiFi"), 18)), 1);
    tk_button(&win, hdr, "Scan", on_scan);
    {
        struct tk_widget *cb = tk_checkbox(&win, hdr, "Airplane", g_airplane_mode);
        cb->on_click = on_air;
    }
    tk_separator(&win, root);

    /* dynamically rebuilt content area */
    content = tk_vbox(&win, root, 8);
    tk_grow(content, 1);
    content_base = tk_checkpoint(&win);
    rebuild();

    return tk_run(&win);
}
