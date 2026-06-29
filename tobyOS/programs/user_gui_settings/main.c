/* user_gui_settings/main.c -- /bin/gui_settings.
 *
 * The TobyOS Settings app, rebuilt on the TobyTK widget toolkit (toby/tk.h)
 * instead of hand-drawing every pixel. It is the first native app refactored
 * onto the canonical libtoby toolkit (Milestone 1 of the GUI-framework work).
 *
 * It keeps the real behaviour of the old syscall-only version: a sidebar of
 * pages, live system metrics, and persistent settings written through
 * SYS_SETTING_SET (so changes here update the compositor-owned desktop chrome
 * immediately). The drawing/layout/event plumbing is now the toolkit's job.
 *
 * Built as a LIBTOBY_PROGRAM so it links libtoby.a (libc + the toolkit). The
 * tobyOS-specific syscalls (settings/metrics/session/logout) have no POSIX
 * wrapper, so we issue them raw below.
 */

#include <toby/tk.h>

typedef unsigned long      size_t;
typedef long               ssize_t;

/* tobyOS-specific syscalls not covered by the libtoby POSIX API. */
#define SYS_WRITE            1
#define SYS_SETTING_GET     21
#define SYS_SETTING_SET     22
#define SYS_LOGOUT          24
#define SYS_SESSION_INFO    25
#define SYS_SYSTEM_METRICS  74

struct abi_system_metrics {
    uint64_t uptime_ms;
    uint64_t total_pages, used_pages, free_pages;
    uint64_t cpu_user_ns, cpu_total_ns;
    uint64_t total_syscalls, context_switches, gui_frames;
    uint64_t proc_spawns, proc_exits, vfs_ops, vfs_ns, gui_ns, net_packets, net_ns;
    uint32_t process_count, ready_count, blocked_count;
    uint32_t cpu_pct, ram_pct, gui_pct, disk_pct, net_pct;
    uint32_t syscalls_per_s, gui_fps, vfs_ops_per_s, net_packets_per_s;
    uint32_t net_up, net_ip_be, tsc_mhz;
    uint32_t _reserved[9];
};

/* ---- raw syscall stubs -------------------------------------------- */

static inline long sc1(long n, long a1) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a1)
        : "rcx", "r11", "memory"); return r;
}
static inline long sc2(long n, long a1, long a2) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory"); return r;
}
static inline long sc3(long n, long a1, long a2, long a3) {
    long r; register long r10 __asm__("r10") = 0;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "rcx", "r11", "memory"); return r;
}

static long setting_get(const char *key, char *out, size_t cap) {
    return sc3(SYS_SETTING_GET, (long)key, (long)out, (long)cap);
}
static void setting_set(const char *key, const char *val) {
    sc2(SYS_SETTING_SET, (long)key, (long)val);
}
static long session_info(char *out, size_t cap) {
    return sc2(SYS_SESSION_INFO, (long)out, (long)cap);
}
static void sys_metrics(struct abi_system_metrics *m) { sc1(SYS_SYSTEM_METRICS, (long)m); }
static void do_logout(void) { sc1(SYS_LOGOUT, 0); }

/* ---- tiny helpers ------------------------------------------------- */

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; } return *a == 0 && *b == 0;
}
static void copy(char *d, const char *s, size_t cap) {
    size_t i = 0; if (!cap) return;
    if (s) for (; s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}
static void u_dec(char *out, size_t cap, uint64_t v) {
    char tmp[24]; unsigned n = 0; if (!cap) return;
    if (!v) { if (cap > 1) { out[0] = '0'; out[1] = 0; } else out[0] = 0; return; }
    while (v && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    unsigned i = 0; while (n && i + 1 < cap) out[i++] = tmp[--n]; out[i] = 0;
}
static void cat(char *out, size_t cap, const char *s) {
    size_t n = 0; while (out[n]) n++;
    while (*s && n + 1 < cap) out[n++] = *s++;
    out[n] = 0;
}
static void cat_u(char *out, size_t cap, uint64_t v) { char b[24]; u_dec(b, sizeof b, v); cat(out, cap, b); }
static int parse_int(const char *s, int def) {
    int v = 0, any = 0; if (!s) return def;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    return any ? v : def;
}
static void set_int(const char *key, int v) { char b[16]; u_dec(b, sizeof b, (uint64_t)(v < 0 ? 0 : v)); setting_set(key, b); }

/* ---- pages -------------------------------------------------------- */

enum { PG_HOME, PG_PERSONAL, PG_SOUND, PG_POWER, PG_ACCOUNT, PG_ABOUT, PG_COUNT };
static const char *g_nav[PG_COUNT] = { "Home", "Personalize", "Sound", "Power", "Account", "About" };

struct accent { uint32_t color; const char *bg_value; const char *name; };
static const struct accent g_accents[] = {
    { 0x0000A8FFu, "0x00060912", "cyan" },
    { 0x003D7CFFu, "0x000A1020", "blue" },
    { 0x006E5CFFu, "0x00101030", "indigo" },
    { 0x00A947FFu, "0x00140824", "violet" },
    { 0x00E447B8u, "0x0010081A", "magenta" },
    { 0x00FF6E4Au, "0x00140808", "coral" },
    { 0x00FF394Cu, "0x00160812", "red" },
    { 0x00FF8E2Eu, "0x00060912", "orange" },
    { 0x00FFC04Au, "0x00120D05", "amber" },
    { 0x0000D6B6u, "0x00061218", "teal" },
    { 0x0030C060u, "0x00081410", "green" },
    { 0x00404860u, "0x000A101D", "slate" },
};
#define N_ACCENTS ((int)(sizeof(g_accents)/sizeof(g_accents[0])))
static const char *g_accent_names[N_ACCENTS];

/* ---- state -------------------------------------------------------- */

static int  g_page = PG_PERSONAL;
static int  g_accent = 7;
static int  g_mode = 0;          /* 0 = cyber (dark), 1 = basic */
static int  g_nixie = 75;
static int  g_night = 0;
static int  g_widgets = 1;
static int  g_animations = 1;
static int  g_transparency = 1;
static int  g_volume = 80;
static int  g_power_plan = 0;    /* 0 = balanced, 1 = performance */
static int  g_sleep_idle = 1;
static int  g_sys_sounds = 1;
static int  g_notify_sounds = 1;
static char g_user[32] = "toby";
static char g_status[96] = "Ready.";

static struct abi_system_metrics g_metrics;

/* ---- toolkit handles ---------------------------------------------- */

static struct tk_window  W;
static struct tk_widget *g_content;
static struct tk_widget *g_status_lbl;
static struct tk_widget *g_navbtn[PG_COUNT];
static struct tk_widget *g_user_field;
static struct tk_widget *g_vol_label;
static int g_base_cp;

static void set_status(const char *s) {
    copy(g_status, s, sizeof g_status);
    if (g_status_lbl) tk_set_text(&W, g_status_lbl, g_status);
}

static void rebuild_page(void);

/* ---- settings load/save ------------------------------------------- */

static int read_bool(const char *key, int def) {
    char b[16]; long n = setting_get(key, b, sizeof b);
    if (n <= 0) return def;
    return (streq(b, "on") || streq(b, "1") || streq(b, "true")) ? 1
         : (streq(b, "off") || streq(b, "0") || streq(b, "false")) ? 0 : def;
}

static void load_settings(void) {
    char b[64];
    if (session_info(b, sizeof b) >= 0 && b[0]) {
        int k = 0;
        while (b[k] && b[k] != '\n' && k + 1 < (int)sizeof g_user) { g_user[k] = b[k]; k++; }
        g_user[k] = 0;
    }
    if (setting_get("user.last", b, sizeof b) > 0 && b[0]) copy(g_user, b, sizeof g_user);
    if (setting_get("ui.theme", b, sizeof b) > 0 && streq(b, "basic")) g_mode = 1;
    if (setting_get("desktop.accent", b, sizeof b) > 0)
        for (int i = 0; i < N_ACCENTS; i++) if (streq(b, g_accents[i].name)) g_accent = i;
    if (setting_get("ui.nixie_glow", b, sizeof b) > 0) g_nixie = parse_int(b, g_nixie);
    if (setting_get("audio.volume", b, sizeof b) > 0) g_volume = parse_int(b, g_volume);
    if (setting_get("power.plan", b, sizeof b) > 0) g_power_plan = streq(b, "performance");
    g_animations = read_bool("ui.animations", g_animations);
    g_widgets    = read_bool("ui.widgets", g_widgets);
    g_night      = read_bool("ui.night_light", g_night);
    g_transparency = read_bool("ui.transparency", g_transparency);
}

/* ---- callbacks ---------------------------------------------------- */

static void on_nav(struct tk_window *win, struct tk_widget *w) {
    g_page = w->value;
    rebuild_page();
    (void)win;
}

static void on_theme(struct tk_window *win, struct tk_widget *w) {
    g_mode = w->checked;
    setting_set("ui.theme", g_mode ? "basic" : "cyber");
    set_status(g_mode ? "Theme: Basic (light)" : "Theme: Cyber (dark)");
    (void)win;
}
static void on_anim(struct tk_window *win, struct tk_widget *w) {
    g_animations = w->checked; setting_set("ui.animations", g_animations ? "on" : "off");
    set_status(g_animations ? "Animations on" : "Animations off"); (void)win;
}
static void on_widgets(struct tk_window *win, struct tk_widget *w) {
    g_widgets = w->checked; setting_set("ui.widgets", g_widgets ? "on" : "off");
    set_status(g_widgets ? "Desktop widgets on" : "Desktop widgets off"); (void)win;
}
static void on_night(struct tk_window *win, struct tk_widget *w) {
    g_night = w->checked; setting_set("ui.night_light", g_night ? "on" : "off");
    set_status(g_night ? "Night light on" : "Night light off"); (void)win;
}
static void on_transp(struct tk_window *win, struct tk_widget *w) {
    g_transparency = w->checked; setting_set("ui.transparency", g_transparency ? "on" : "off");
    set_status(g_transparency ? "Transparency on" : "Transparency off"); (void)win;
}
static void on_accent(struct tk_window *win, struct tk_widget *w) {
    if (w->sel < 0 || w->sel >= N_ACCENTS) return;
    g_accent = w->sel;
    setting_set("desktop.accent", g_accents[g_accent].name);
    setting_set("desktop.bg", g_accents[g_accent].bg_value);
    char s[64]; s[0] = 0; cat(s, sizeof s, "Accent: "); cat(s, sizeof s, g_accents[g_accent].name);
    set_status(s); (void)win;
}
static void on_glow(struct tk_window *win, struct tk_widget *w) {
    g_nixie = w->value; set_int("ui.nixie_glow", g_nixie);
    char s[48]; s[0] = 0; cat(s, sizeof s, "Glow: "); cat_u(s, sizeof s, (uint64_t)g_nixie); cat(s, sizeof s, "%");
    set_status(s); (void)win;
}
static void on_volume(struct tk_window *win, struct tk_widget *w) {
    g_volume = w->value; set_int("audio.volume", g_volume);
    if (g_vol_label) { char s[24]; s[0] = 0; cat_u(s, sizeof s, (uint64_t)g_volume); cat(s, sizeof s, "%"); tk_set_text(win, g_vol_label, s); }
}
static void on_sys_sounds(struct tk_window *win, struct tk_widget *w) {
    g_sys_sounds = w->checked; set_status(g_sys_sounds ? "System sounds on" : "System sounds off"); (void)win;
}
static void on_notify_sounds(struct tk_window *win, struct tk_widget *w) {
    g_notify_sounds = w->checked; set_status(g_notify_sounds ? "Notification sounds on" : "Notification sounds off"); (void)win;
}
static void on_power(struct tk_window *win, struct tk_widget *w) {
    g_power_plan = w->checked; setting_set("power.plan", g_power_plan ? "performance" : "balanced");
    set_status(g_power_plan ? "Power plan: Performance" : "Power plan: Balanced"); (void)win;
}
static void on_sleep(struct tk_window *win, struct tk_widget *w) {
    g_sleep_idle = w->checked; set_status(g_sleep_idle ? "Sleep on idle: on" : "Sleep on idle: off"); (void)win;
}
static void on_save_user(struct tk_window *win, struct tk_widget *w) {
    if (g_user_field) copy(g_user, tk_get_text(g_user_field), sizeof g_user);
    setting_set("user.last", g_user);
    char s[64]; s[0] = 0; cat(s, sizeof s, "Saved user: "); cat(s, sizeof s, g_user);
    set_status(s); (void)win; (void)w;
}
static void on_logout(struct tk_window *win, struct tk_widget *w) { (void)win; (void)w; do_logout(); }
static void on_refresh(struct tk_window *win, struct tk_widget *w) {
    sys_metrics(&g_metrics); set_status("Metrics refreshed."); rebuild_page(); (void)win; (void)w;
}

/* ---- page builders ------------------------------------------------ */

/* a "Label : control" row inside a vbox */
static struct tk_widget *row(struct tk_widget *parent) {
    struct tk_widget *r = tk_hbox(&W, parent, 12);
    tk_size(r, 0, 30);
    return r;
}
static void row_label(struct tk_widget *r, const char *text) {
    struct tk_widget *l = tk_label(&W, r, text);
    tk_size(l, 150, 0);
}
static void heading(struct tk_widget *parent, const char *text) {
    struct tk_widget *h = tk_label(&W, parent, text);
    tk_font(h, 22); tk_bold(h); tk_size(h, 0, 36);
}
static void info_line(struct tk_widget *parent, const char *label, const char *value) {
    struct tk_widget *r = row(parent);
    struct tk_widget *l = tk_label(&W, r, label); tk_size(l, 170, 0); tk_colors(l, 0, W.theme.text_dim);
    tk_label(&W, r, value);
}

static void build_home(void) {
    heading(g_content, "Home");
    char s[96];
    s[0] = 0; cat_u(s, sizeof s, (g_metrics.used_pages * 4) / 1024); cat(s, sizeof s, " / ");
    cat_u(s, sizeof s, (g_metrics.total_pages * 4) / 1024); cat(s, sizeof s, " MiB");
    info_line(g_content, "Memory used", s);
    s[0] = 0; cat_u(s, sizeof s, g_metrics.uptime_ms / 1000); cat(s, sizeof s, " s");
    info_line(g_content, "Uptime", s);
    s[0] = 0; cat_u(s, sizeof s, g_metrics.process_count);
    info_line(g_content, "Processes", s);
    s[0] = 0; cat(s, sizeof s, g_user);
    info_line(g_content, "Signed in as", s);
    tk_separator(&W, g_content);
    tk_label(&W, g_content, "Welcome to TobyOS Settings. Pick a category on the left.");
    struct tk_widget *r = tk_hbox(&W, g_content, 10); tk_size(r, 0, 40);
    tk_button(&W, r, "Refresh metrics", on_refresh);
}

static void build_personal(void) {
    heading(g_content, "Personalization");
    struct tk_widget *cb;
    cb = tk_checkbox(&W, g_content, "Use Basic (light) theme", g_mode); cb->on_click = on_theme;
    {
        struct tk_widget *r = row(g_content); tk_size(r, 0, 120);
        row_label(r, "Accent color");
        struct tk_widget *lb = tk_listbox(&W, r, g_accent_names, N_ACCENTS);
        tk_grow(lb, 1); lb->sel = g_accent; lb->on_click = on_accent; lb->on_change = on_accent;
    }
    {
        struct tk_widget *r = row(g_content);
        row_label(r, "Glow intensity");
        struct tk_widget *sl = tk_slider(&W, r, 0, 100, g_nixie); tk_grow(sl, 1); sl->on_change = on_glow;
    }
    tk_separator(&W, g_content);
    cb = tk_checkbox(&W, g_content, "Animations", g_animations); cb->on_click = on_anim;
    cb = tk_checkbox(&W, g_content, "Desktop widgets", g_widgets); cb->on_click = on_widgets;
    cb = tk_checkbox(&W, g_content, "Window transparency", g_transparency); cb->on_click = on_transp;
    cb = tk_checkbox(&W, g_content, "Night light", g_night); cb->on_click = on_night;
}

static void build_sound(void) {
    heading(g_content, "Sound");
    {
        struct tk_widget *r = row(g_content);
        row_label(r, "Volume");
        struct tk_widget *sl = tk_slider(&W, r, 0, 100, g_volume); tk_grow(sl, 1); sl->on_change = on_volume;
        g_vol_label = tk_label(&W, r, ""); tk_size(g_vol_label, 56, 0); tk_align(g_vol_label, TK_ALIGN_RIGHT);
        char s[24]; s[0] = 0; cat_u(s, sizeof s, (uint64_t)g_volume); cat(s, sizeof s, "%");
        tk_set_text(&W, g_vol_label, s);
    }
    tk_separator(&W, g_content);
    struct tk_widget *cb;
    cb = tk_checkbox(&W, g_content, "System sounds", g_sys_sounds); cb->on_click = on_sys_sounds;
    cb = tk_checkbox(&W, g_content, "Notification sounds", g_notify_sounds); cb->on_click = on_notify_sounds;
}

static void build_power(void) {
    heading(g_content, "Power");
    struct tk_widget *cb;
    cb = tk_checkbox(&W, g_content, "Performance power plan", g_power_plan); cb->on_click = on_power;
    cb = tk_checkbox(&W, g_content, "Sleep on idle", g_sleep_idle); cb->on_click = on_sleep;
    tk_separator(&W, g_content);
    char s[64]; s[0] = 0; cat_u(s, sizeof s, g_metrics.uptime_ms / 1000); cat(s, sizeof s, " s");
    info_line(g_content, "Uptime", s);
}

static void build_account(void) {
    heading(g_content, "Account");
    {
        struct tk_widget *r = row(g_content);
        row_label(r, "Username");
        g_user_field = tk_field(&W, r, g_user); tk_grow(g_user_field, 1); g_user_field->on_click = on_save_user;
    }
    struct tk_widget *r = tk_hbox(&W, g_content, 10); tk_size(r, 0, 42);
    tk_button(&W, r, "Save", on_save_user);
    struct tk_widget *lo = tk_button(&W, r, "Sign out", on_logout);
    tk_colors(lo, 0x00772A33u, 0);
}

static void build_about(void) {
    heading(g_content, "About TobyOS");
    info_line(g_content, "Edition", "TobyOS desktop");
    char s[64];
    s[0] = 0; cat_u(s, sizeof s, g_metrics.total_syscalls);
    info_line(g_content, "Total syscalls", s);
    s[0] = 0; cat_u(s, sizeof s, g_metrics.gui_frames);
    info_line(g_content, "GUI frames", s);
    s[0] = 0; cat_u(s, sizeof s, g_metrics.context_switches);
    info_line(g_content, "Context switches", s);
    s[0] = 0; cat_u(s, sizeof s, g_metrics.tsc_mhz); cat(s, sizeof s, " MHz");
    info_line(g_content, "CPU", s);
    tk_separator(&W, g_content);
    struct tk_widget *r = tk_hbox(&W, g_content, 10); tk_size(r, 0, 40);
    tk_button(&W, r, "Refresh", on_refresh);
}

static void rebuild_page(void) {
    g_vol_label = 0; g_user_field = 0;
    tk_clear_children(&W, g_content);
    tk_rewind(&W, g_base_cp);
    switch (g_page) {
    case PG_HOME:     build_home();     break;
    case PG_PERSONAL: build_personal(); break;
    case PG_SOUND:    build_sound();    break;
    case PG_POWER:    build_power();    break;
    case PG_ACCOUNT:  build_account();  break;
    case PG_ABOUT:    build_about();    break;
    default: break;
    }
    /* highlight the active nav button */
    for (int i = 0; i < PG_COUNT; i++)
        if (g_navbtn[i]) tk_colors(g_navbtn[i], i == g_page ? W.theme.list_sel : 0, 0);
    tk_redraw(&W);
}

/* ---- main --------------------------------------------------------- */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < N_ACCENTS; i++) g_accent_names[i] = g_accents[i].name;

    load_settings();
    sys_metrics(&g_metrics);

    if (tk_window_open(&W, 820, 560, "Settings") != 0) {
        const char *m = "settings: cannot create window\n";
        sc3(SYS_WRITE, 1, (long)m, 31);
        return 1;
    }

    /* ---- static chrome: sidebar + content + status bar ---- */
    struct tk_widget *body = tk_hbox(&W, 0, 0); tk_grow(body, 1);

    struct tk_widget *side = tk_vbox(&W, body, 4);
    tk_size(side, 196, 0); tk_pad(side, 14); tk_colors(side, W.theme.panel_bg, 0);
    struct tk_widget *brand = tk_label(&W, side, "Settings");
    tk_font(brand, 24); tk_bold(brand); tk_size(brand, 0, 44);
    for (int i = 0; i < PG_COUNT; i++) {
        struct tk_widget *b = tk_button(&W, side, g_nav[i], on_nav);
        b->value = i; tk_align(b, TK_ALIGN_LEFT); tk_size(b, 0, 34);
        g_navbtn[i] = b;
    }

    g_content = tk_vbox(&W, body, 10);
    tk_grow(g_content, 1); tk_pad(g_content, 22);

    g_status_lbl = tk_label(&W, tk_root(&W), g_status);
    tk_size(g_status_lbl, 0, 26); tk_font(g_status_lbl, 13);
    tk_colors(g_status_lbl, W.theme.panel_bg, W.theme.text_dim);

    g_base_cp = tk_checkpoint(&W);   /* page content allocates from here */
    rebuild_page();

    return tk_run(&W);
}
