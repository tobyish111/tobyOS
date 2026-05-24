/* settings/main.c -- Milestone 6 tabbed settings application.
 *
 * A 900x600 GUI window with a left sidebar (200px page list) and
 * a right content area (700px) showing the selected settings page.
 * Settings are persisted to /system/settings.conf (key=value).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ================================================================
 * Syscall interface
 * ================================================================ */

static long sc0(long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n) : "rcx","r11","memory");
    return r;
}
static long sc1(long n, long a) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a) : "rcx","r11","memory");
    return r;
}
static long sc2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b) : "rcx","r11","memory");
    return r;
}
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c) : "rcx","r11","memory");
    return r;
}
static long sc4(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx","r11","memory");
    return r;
}
static long sc5(long n, long a, long b, long c, long d, long e) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8) : "rcx","r11","memory");
    return r;
}

#define SYS_EXIT            0
#define SYS_WRITE           1
#define SYS_READ            2
#define SYS_YIELD           5
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_OPEN           35
#define SYS_LSEEK          36
#define SYS_CLOSE           4
#define SYS_GUI_RECT       81
#define SYS_GUI_ROUNDED_RECT 82
#define SYS_GUI_LINE       80
#define SYS_SETTING_GET    21
#define SYS_SETTING_SET    22

struct gui_event {
    int     type;
    int     x, y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};

#define EV_MOUSE_MOVE  1
#define EV_MOUSE_DOWN  2
#define EV_MOUSE_UP    3
#define EV_KEY         4
#define EV_CLOSE       5

/* ================================================================
 * GUI helpers
 * ================================================================ */

static int g_win = -1;
#define WIN_W 900
#define WIN_H 600
#define SIDEBAR_W 200
#define CONTENT_X SIDEBAR_W
#define CONTENT_W (WIN_W - SIDEBAR_W)

static void gui_fill(int x, int y, int w, int h, uint32_t color) {
    uint64_t pos = (uint64_t)(uint16_t)x | ((uint64_t)(uint16_t)y << 16);
    uint64_t sz  = (uint64_t)(uint16_t)w | ((uint64_t)(uint16_t)h << 16);
    sc4(SYS_GUI_FILL, g_win, (long)pos, (long)sz, (long)color);
}

static void gui_text(int x, int y, const char *s, uint32_t color) {
    uint64_t pos = (uint64_t)(uint16_t)x | ((uint64_t)(uint16_t)y << 16);
    sc4(SYS_GUI_TEXT, g_win, (long)pos, (long)(uintptr_t)s, (long)color);
}

static void gui_flip(void) { sc1(SYS_GUI_FLIP, g_win); }

static int gui_poll(struct gui_event *ev) {
    return (int)sc2(SYS_GUI_POLL_EVENT, g_win, (long)(uintptr_t)ev);
}

/* ================================================================
 * Settings storage
 * ================================================================ */

#define MAX_SETTINGS 64
#define KEY_MAX 64
#define VAL_MAX 128
#define CONF_PATH "/system/settings.conf"

struct setting_entry {
    char key[KEY_MAX];
    char val[VAL_MAX];
};

static struct setting_entry g_settings[MAX_SETTINGS];
static int g_num_settings = 0;

static void settings_load(void) {
    g_num_settings = 0;
    char buf[4096];
    memset(buf, 0, sizeof(buf));

    long fd = sc3(SYS_OPEN, (long)(uintptr_t)CONF_PATH, 0, 0);
    if (fd < 0) return;

    long n = sc3(SYS_READ, fd, (long)(uintptr_t)buf, (long)(sizeof(buf) - 1));
    sc1(SYS_CLOSE, fd);
    if (n <= 0) return;

    buf[n] = '\0';
    char *p = buf;
    while (*p && g_num_settings < MAX_SETTINGS) {
        char *line_end = p;
        while (*line_end && *line_end != '\n') line_end++;

        char *eq = p;
        while (eq < line_end && *eq != '=') eq++;
        if (eq < line_end) {
            int klen = (int)(eq - p);
            int vlen = (int)(line_end - eq - 1);
            if (klen > 0 && klen < KEY_MAX && vlen < VAL_MAX) {
                memcpy(g_settings[g_num_settings].key, p, klen);
                g_settings[g_num_settings].key[klen] = '\0';
                memcpy(g_settings[g_num_settings].val, eq + 1, vlen);
                g_settings[g_num_settings].val[vlen] = '\0';
                g_num_settings++;
            }
        }
        p = (*line_end) ? line_end + 1 : line_end;
    }
}

static void settings_save(void) {
    char buf[4096];
    int off = 0;
    for (int i = 0; i < g_num_settings && off < 4000; i++) {
        int kl = strlen(g_settings[i].key);
        int vl = strlen(g_settings[i].val);
        memcpy(buf + off, g_settings[i].key, kl); off += kl;
        buf[off++] = '=';
        memcpy(buf + off, g_settings[i].val, vl); off += vl;
        buf[off++] = '\n';
    }

    long fd = sc3(SYS_OPEN, (long)(uintptr_t)CONF_PATH,
                  0x040 | 0x200 | 0x1, 0);  /* O_CREAT|O_TRUNC|O_WRONLY */
    if (fd < 0) return;
    sc3(SYS_WRITE, fd, (long)(uintptr_t)buf, (long)off);
    sc1(SYS_CLOSE, fd);
}

static const char *setting_get(const char *key) {
    for (int i = 0; i < g_num_settings; i++) {
        if (strcmp(g_settings[i].key, key) == 0)
            return g_settings[i].val;
    }
    return NULL;
}

static void setting_set(const char *key, const char *val) {
    for (int i = 0; i < g_num_settings; i++) {
        if (strcmp(g_settings[i].key, key) == 0) {
            strncpy(g_settings[i].val, val, VAL_MAX - 1);
            g_settings[i].val[VAL_MAX - 1] = '\0';
            settings_save();
            return;
        }
    }
    if (g_num_settings < MAX_SETTINGS) {
        strncpy(g_settings[g_num_settings].key, key, KEY_MAX - 1);
        g_settings[g_num_settings].key[KEY_MAX - 1] = '\0';
        strncpy(g_settings[g_num_settings].val, val, VAL_MAX - 1);
        g_settings[g_num_settings].val[VAL_MAX - 1] = '\0';
        g_num_settings++;
        settings_save();
    }
}

static int setting_get_int(const char *key, int def) {
    const char *v = setting_get(key);
    if (!v) return def;
    return atoi(v);
}

static void setting_set_int(const char *key, int val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    setting_set(key, buf);
}

/* ================================================================
 * Color scheme
 * ================================================================ */

#define COL_BG         0x001E1E2E
#define COL_SIDEBAR    0x00181825
#define COL_ACTIVE     0x00313244
#define COL_HOVER      0x0045475A
#define COL_TEXT       0x00CDD6F4
#define COL_TEXT_DIM   0x00A6ADC8
#define COL_ACCENT     0x0089B4FA
#define COL_BUTTON     0x00585B70
#define COL_BUTTON_HL  0x0089B4FA
#define COL_SLIDER_BG  0x00313244
#define COL_SLIDER_FG  0x0089B4FA
#define COL_TOGGLE_ON  0x0089B4FA
#define COL_TOGGLE_OFF 0x00585B70
#define COL_INPUT_BG   0x00313244
#define COL_DIVIDER    0x0045475A
#define COL_RED        0x00F38BA8
#define COL_GREEN      0x00A6E3A1

/* ================================================================
 * Page definitions
 * ================================================================ */

enum page_id {
    PAGE_DISPLAY,
    PAGE_THEMES,
    PAGE_SOUND,
    PAGE_NETWORK,
    PAGE_ACCOUNTS,
    PAGE_STORAGE,
    PAGE_APPS,
    PAGE_POWER,
    PAGE_ACCESSIBILITY,
    PAGE_DATETIME,
    PAGE_KEYBOARD,
    PAGE_UPDATES,
    PAGE_COUNT
};

static const char *page_names[PAGE_COUNT] = {
    "Display", "Themes", "Sound", "Network",
    "Accounts", "Storage", "Apps", "Power",
    "Accessibility", "Date/Time", "Keyboard", "Updates"
};

static int g_current_page = PAGE_DISPLAY;
static int g_hover_page = -1;
static int g_mouse_x = 0, g_mouse_y = 0;

/* ================================================================
 * UI drawing primitives
 * ================================================================ */

static void draw_hline(int x, int y, int w, uint32_t color) {
    gui_fill(x, y, w, 1, color);
}

static void draw_slider(int x, int y, int w, int val, int max_val) {
    gui_fill(x, y + 4, w, 8, COL_SLIDER_BG);
    int fill_w = (val * w) / max_val;
    if (fill_w > w) fill_w = w;
    gui_fill(x, y + 4, fill_w, 8, COL_SLIDER_FG);
    int knob_x = x + fill_w - 6;
    if (knob_x < x) knob_x = x;
    gui_fill(knob_x, y, 12, 16, COL_ACCENT);
}

static void draw_toggle(int x, int y, int on) {
    uint32_t bg = on ? COL_TOGGLE_ON : COL_TOGGLE_OFF;
    gui_fill(x, y, 40, 20, bg);
    int knob_x = on ? x + 22 : x + 2;
    gui_fill(knob_x, y + 2, 16, 16, COL_TEXT);
}

static void draw_button(int x, int y, int w, const char *label, int highlight) {
    uint32_t bg = highlight ? COL_BUTTON_HL : COL_BUTTON;
    gui_fill(x, y, w, 28, bg);
    gui_text(x + 8, y + 6, label, COL_TEXT);
}

static void draw_dropdown(int x, int y, int w, const char *current) {
    gui_fill(x, y, w, 26, COL_INPUT_BG);
    gui_text(x + 8, y + 5, current, COL_TEXT);
    gui_text(x + w - 16, y + 5, "v", COL_TEXT_DIM);
}

static void draw_section_title(int x, int y, const char *title) {
    gui_text(x, y, title, COL_ACCENT);
    draw_hline(x, y + 18, CONTENT_W - 40, COL_DIVIDER);
}

static void draw_label_value(int x, int y, const char *label, const char *value) {
    gui_text(x, y, label, COL_TEXT_DIM);
    gui_text(x + 180, y, value, COL_TEXT);
}

static void draw_radio(int x, int y, const char *label, int selected) {
    uint32_t ring = selected ? COL_ACCENT : COL_BUTTON;
    gui_fill(x, y + 2, 14, 14, ring);
    if (selected)
        gui_fill(x + 3, y + 5, 8, 8, COL_TEXT);
    else
        gui_fill(x + 2, y + 4, 10, 10, COL_BG);
    gui_text(x + 20, y, label, COL_TEXT);
}

static void draw_checkbox(int x, int y, const char *label, int checked) {
    gui_fill(x, y + 1, 16, 16, checked ? COL_ACCENT : COL_INPUT_BG);
    if (checked)
        gui_text(x + 3, y + 1, "X", COL_TEXT);
    gui_text(x + 22, y, label, COL_TEXT);
}

static int point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

/* ================================================================
 * Page: Display
 * ================================================================ */

static void page_display(void) {
    int x = CONTENT_X + 20, y = 60;

    draw_section_title(x, y, "Resolution");
    y += 30;
    const char *res = setting_get("display.resolution");
    if (!res) res = "1920x1080";
    draw_dropdown(x, y, 200, res);
    y += 40;

    draw_section_title(x, y, "Scaling");
    y += 30;
    int scale = setting_get_int("display.scale", 100);
    char sbuf[32];
    snprintf(sbuf, sizeof(sbuf), "%d%%", scale);
    gui_text(x, y, sbuf, COL_TEXT);
    draw_slider(x + 60, y, 200, scale, 200);
    y += 40;

    draw_section_title(x, y, "Refresh Rate");
    y += 30;
    const char *rate = setting_get("display.refresh");
    if (!rate) rate = "60 Hz";
    draw_dropdown(x, y, 150, rate);
    y += 40;

    draw_section_title(x, y, "Available Modes");
    y += 30;
    gui_text(x, y, "1920x1080 @ 60Hz", COL_TEXT); y += 18;
    gui_text(x, y, "1680x1050 @ 60Hz", COL_TEXT); y += 18;
    gui_text(x, y, "1280x720  @ 60Hz", COL_TEXT); y += 18;
    gui_text(x, y, "1024x768  @ 60Hz", COL_TEXT); y += 18;
    gui_text(x, y, "800x600   @ 60Hz", COL_TEXT); y += 18;
}

/* ================================================================
 * Page: Themes
 * ================================================================ */

static const uint32_t accent_colors[8] = {
    0x0089B4FA, 0x00F38BA8, 0x00A6E3A1, 0x00FAB387,
    0x00CBA6F7, 0x0094E2D5, 0x00F9E2AF, 0x00EBA0AC
};

static void page_themes(void) {
    int x = CONTENT_X + 20, y = 60;

    draw_section_title(x, y, "Appearance");
    y += 30;
    int dark = setting_get_int("theme.dark", 1);
    gui_text(x, y, "Dark Mode", COL_TEXT);
    draw_toggle(x + 150, y, dark);
    y += 40;

    draw_section_title(x, y, "Accent Color");
    y += 30;
    int cur_accent = setting_get_int("theme.accent", 0);
    for (int i = 0; i < 8; i++) {
        int cx = x + i * 40;
        gui_fill(cx, y, 30, 30, accent_colors[i]);
        if (i == cur_accent)
            gui_fill(cx + 10, y + 10, 10, 10, COL_TEXT);
    }
    y += 50;

    draw_section_title(x, y, "Wallpaper");
    y += 30;
    const char *wp = setting_get("theme.wallpaper");
    if (!wp) wp = "(none)";
    draw_label_value(x, y, "Current:", wp);
    y += 25;
    draw_button(x, y, 120, "Browse...", 0);
}

/* ================================================================
 * Page: Sound
 * ================================================================ */

static void page_sound(void) {
    int x = CONTENT_X + 20, y = 60;

    draw_section_title(x, y, "Output Device");
    y += 30;
    const char *dev = setting_get("sound.device");
    if (!dev) dev = "Intel HD Audio";
    draw_dropdown(x, y, 250, dev);
    y += 40;

    draw_section_title(x, y, "Master Volume");
    y += 30;
    int vol = setting_get_int("sound.volume", 75);
    char vbuf[16];
    snprintf(vbuf, sizeof(vbuf), "%d%%", vol);
    gui_text(x, y, vbuf, COL_TEXT);
    draw_slider(x + 50, y, 250, vol, 100);
    y += 40;

    draw_section_title(x, y, "Per-App Volume");
    y += 30;
    gui_text(x, y, "Shell", COL_TEXT);
    draw_slider(x + 100, y, 200, 80, 100);
    y += 25;
    gui_text(x, y, "Browser", COL_TEXT);
    draw_slider(x + 100, y, 200, 60, 100);
    y += 25;
    gui_text(x, y, "Music", COL_TEXT);
    draw_slider(x + 100, y, 200, 90, 100);
    y += 40;

    draw_button(x, y, 120, "Test Sound", 0);
}

/* ================================================================
 * Page: Network
 * ================================================================ */

static void draw_signal_bars(int x, int y, int strength) {
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 3;
        uint32_t c = (i < strength) ? COL_GREEN : COL_BUTTON;
        gui_fill(x + i * 6, y + (16 - h), 4, h, c);
    }
}

static void page_network(void) {
    int x = CONTENT_X + 20, y = 60;

    draw_section_title(x, y, "WiFi Networks");
    y += 30;

    const char *networks[] = {"HomeNetwork", "Office_5G", "CoffeeShop", "Guest"};
    int signals[] = {4, 3, 2, 1};
    for (int i = 0; i < 4; i++) {
        gui_text(x, y, networks[i], COL_TEXT);
        draw_signal_bars(x + 200, y, signals[i]);
        if (i == 0) {
            gui_text(x + 240, y, "(Connected)", COL_GREEN);
            draw_button(x + 380, y - 4, 100, "Disconnect", 0);
        } else {
            draw_button(x + 380, y - 4, 80, "Connect", 0);
        }
        y += 30;
    }
    y += 10;

    draw_section_title(x, y, "Ethernet");
    y += 30;
    draw_label_value(x, y, "Status:", "Connected (1 Gbps)");
    y += 20;
    draw_label_value(x, y, "IP Address:", "10.0.2.15");
    y += 20;
    draw_label_value(x, y, "Gateway:", "10.0.2.2");
    y += 30;

    draw_section_title(x, y, "DNS");
    y += 30;
    const char *dns1 = setting_get("net.dns1");
    if (!dns1) dns1 = "8.8.8.8";
    const char *dns2 = setting_get("net.dns2");
    if (!dns2) dns2 = "8.8.4.4";
    gui_text(x, y, "Primary DNS:", COL_TEXT_DIM);
    gui_fill(x + 120, y - 2, 150, 20, COL_INPUT_BG);
    gui_text(x + 125, y, dns1, COL_TEXT);
    y += 25;
    gui_text(x, y, "Secondary DNS:", COL_TEXT_DIM);
    gui_fill(x + 120, y - 2, 150, 20, COL_INPUT_BG);
    gui_text(x + 125, y, dns2, COL_TEXT);
}

/* ================================================================
 * Page: Accounts
 * ================================================================ */

static void page_accounts(void) {
    int x = CONTENT_X + 20, y = 60;

    draw_section_title(x, y, "Current User");
    y += 30;
    const char *user = setting_get("user.name");
    if (!user) user = "admin";
    draw_label_value(x, y, "Username:", user);
    y += 20;
    draw_label_value(x, y, "Role:", "Administrator");
    y += 20;
    draw_label_value(x, y, "Login time:", "2026-05-23 15:00");
    y += 35;

    draw_button(x, y, 160, "Change Password", 0);
    draw_button(x + 180, y, 120, "Add User", 0);
    y += 50;

    draw_section_title(x, y, "All Users");
    y += 30;
    gui_text(x, y, "admin", COL_TEXT);
    gui_text(x + 120, y, "Administrator", COL_TEXT_DIM);
    y += 22;
    gui_text(x, y, "guest", COL_TEXT);
    gui_text(x + 120, y, "Standard", COL_TEXT_DIM);
    y += 22;
    gui_text(x, y, "service", COL_TEXT);
    gui_text(x + 120, y, "Service Account", COL_TEXT_DIM);
}

/* ================================================================
 * Page: Storage
 * ================================================================ */

static void page_storage(void) {
    int x = CONTENT_X + 20, y = 60;

    draw_section_title(x, y, "Disk Usage");
    y += 30;
    int used_pct = setting_get_int("storage.used_pct", 42);
    int total_mb = setting_get_int("storage.total_mb", 4096);
    int used_mb = (total_mb * used_pct) / 100;

    gui_fill(x, y, 400, 24, COL_SLIDER_BG);
    int fill_w = (used_pct * 400) / 100;
    uint32_t bar_col = (used_pct > 80) ? COL_RED : COL_ACCENT;
    gui_fill(x, y, fill_w, 24, bar_col);

    char ubuf[64];
    snprintf(ubuf, sizeof(ubuf), "%d MB / %d MB (%d%%)", used_mb, total_mb, used_pct);
    gui_text(x + 10, y + 4, ubuf, COL_TEXT);
    y += 40;

    draw_section_title(x, y, "Large Files");
    y += 30;
    gui_text(x, y, "/bin/browser         812 KB", COL_TEXT); y += 18;
    gui_text(x, y, "/bin/gui_settings    256 KB", COL_TEXT); y += 18;
    gui_text(x, y, "/data/logs/          128 KB", COL_TEXT); y += 18;
    gui_text(x, y, "/repo/               384 KB", COL_TEXT); y += 18;
    gui_text(x, y, "/lib/libtoby.so       96 KB", COL_TEXT); y += 30;

    draw_button(x, y, 120, "Clean up", 0);
}

/* ================================================================
 * Page: Apps
 * ================================================================ */

static void page_apps(void) {
    int x = CONTENT_X + 20, y = 60;

    draw_section_title(x, y, "Installed Applications");
    y += 30;

    const char *apps[] = {
        "browser", "gui_calc", "gui_edit", "gui_files",
        "gui_settings", "gui_taskmgr", "gui_term", "gui_viewer"
    };
    int n_apps = 8;
    for (int i = 0; i < n_apps && y < WIN_H - 60; i++) {
        gui_text(x, y, apps[i], COL_TEXT);
        draw_button(x + 200, y - 4, 100, "Set Default", 0);
        draw_button(x + 310, y - 4, 90, "Uninstall", 0);
        y += 30;
    }
    y += 10;

    draw_section_title(x, y, "System Utilities (/bin)");
    y += 30;
    gui_text(x, y, "sh, pkg, ps, top, cp, mv, rm, find ...", COL_TEXT_DIM);
}

/* ================================================================
 * Page: Power
 * ================================================================ */

static void page_power(void) {
    int x = CONTENT_X + 20, y = 60;

    draw_section_title(x, y, "Sleep Timeout");
    y += 30;
    const char *sleep_opts[] = {"Never", "5 min", "10 min", "15 min", "30 min"};
    int cur_sleep = setting_get_int("power.sleep_timeout", 2);
    draw_dropdown(x, y, 150, sleep_opts[cur_sleep < 5 ? cur_sleep : 0]);
    y += 40;

    draw_section_title(x, y, "Lid Close Action");
    y += 30;
    int lid_action = setting_get_int("power.lid_action", 0);
    draw_radio(x, y, "Sleep", lid_action == 0); y += 22;
    draw_radio(x, y, "Shutdown", lid_action == 1); y += 22;
    draw_radio(x, y, "Do Nothing", lid_action == 2); y += 35;

    draw_section_title(x, y, "Power Plan");
    y += 30;
    int plan = setting_get_int("power.plan", 0);
    draw_radio(x, y, "Balanced", plan == 0); y += 22;
    draw_radio(x, y, "Performance", plan == 1); y += 22;
    draw_radio(x, y, "Power Saver", plan == 2); y += 35;

    draw_section_title(x, y, "Battery");
    y += 30;
    draw_label_value(x, y, "Status:", "Not present (AC power)");
}

/* ================================================================
 * Page: Accessibility
 * ================================================================ */

static void page_accessibility(void) {
    int x = CONTENT_X + 20, y = 60;

    draw_section_title(x, y, "Visual");
    y += 30;
    int hc = setting_get_int("a11y.high_contrast", 0);
    gui_text(x, y, "High Contrast", COL_TEXT);
    draw_toggle(x + 200, y, hc);
    y += 35;

    draw_section_title(x, y, "Cursor Size");
    y += 30;
    int csz = setting_get_int("a11y.cursor_size", 0);
    draw_radio(x, y, "Small", csz == 0); y += 22;
    draw_radio(x, y, "Medium", csz == 1); y += 22;
    draw_radio(x, y, "Large", csz == 2); y += 35;

    draw_section_title(x, y, "Audio Assist");
    y += 30;
    int sr = setting_get_int("a11y.screen_reader", 0);
    gui_text(x, y, "Screen Reader", COL_TEXT);
    draw_toggle(x + 200, y, sr);
    y += 35;

    draw_section_title(x, y, "Input");
    y += 30;
    int sticky = setting_get_int("a11y.sticky_keys", 0);
    gui_text(x, y, "Sticky Keys", COL_TEXT);
    draw_toggle(x + 200, y, sticky);
}

/* ================================================================
 * Page: Date/Time
 * ================================================================ */

static void page_datetime(void) {
    int x = CONTENT_X + 20, y = 60;

    draw_section_title(x, y, "Current Time");
    y += 30;
    draw_label_value(x, y, "Date:", "2026-05-23");
    y += 20;
    draw_label_value(x, y, "Time:", "15:55:00");
    y += 35;

    draw_section_title(x, y, "Timezone");
    y += 30;
    const char *tz = setting_get("datetime.timezone");
    if (!tz) tz = "America/Chicago (UTC-5)";
    draw_dropdown(x, y, 280, tz);
    y += 40;

    draw_button(x, y, 160, "Sync with NTP", 0);
    y += 45;

    draw_section_title(x, y, "Date Format");
    y += 30;
    int fmt = setting_get_int("datetime.format", 0);
    draw_radio(x, y, "YYYY-MM-DD", fmt == 0); y += 22;
    draw_radio(x, y, "MM/DD/YYYY", fmt == 1); y += 22;
    draw_radio(x, y, "DD/MM/YYYY", fmt == 2);
}

/* ================================================================
 * Page: Keyboard
 * ================================================================ */

static void page_keyboard(void) {
    int x = CONTENT_X + 20, y = 60;

    draw_section_title(x, y, "Repeat Delay");
    y += 30;
    int delay = setting_get_int("kb.repeat_delay", 500);
    char dbuf[32];
    snprintf(dbuf, sizeof(dbuf), "%d ms", delay);
    gui_text(x, y, dbuf, COL_TEXT);
    draw_slider(x + 80, y, 250, delay, 1000);
    y += 40;

    draw_section_title(x, y, "Repeat Rate");
    y += 30;
    int rate = setting_get_int("kb.repeat_rate", 30);
    char rbuf[32];
    snprintf(rbuf, sizeof(rbuf), "%d/sec", rate);
    gui_text(x, y, rbuf, COL_TEXT);
    draw_slider(x + 80, y, 250, rate, 60);
    y += 40;

    draw_section_title(x, y, "Key Remapping");
    y += 30;
    gui_text(x, y, "From", COL_TEXT_DIM);
    gui_text(x + 100, y, "To", COL_TEXT_DIM);
    y += 20;
    gui_text(x, y, "Caps Lock", COL_TEXT);
    gui_text(x + 100, y, "Ctrl", COL_TEXT);
    y += 18;
    gui_text(x, y, "Right Alt", COL_TEXT);
    gui_text(x + 100, y, "Super", COL_TEXT);
    y += 30;
    draw_button(x, y, 120, "Add Remap", 0);
}

/* ================================================================
 * Page: Updates
 * ================================================================ */

static void page_updates(void) {
    int x = CONTENT_X + 20, y = 60;

    draw_section_title(x, y, "System Version");
    y += 30;
    draw_label_value(x, y, "tobyOS:", "1.0.0-milestone8");
    y += 20;
    draw_label_value(x, y, "Kernel ABI:", "v1");
    y += 20;
    draw_label_value(x, y, "Build date:", "2026-05-23");
    y += 35;

    draw_button(x, y, 180, "Check for Updates", 0);
    y += 45;

    draw_section_title(x, y, "Auto-Update");
    y += 30;
    int autoup = setting_get_int("updates.auto", 1);
    gui_text(x, y, "Automatic Updates", COL_TEXT);
    draw_toggle(x + 200, y, autoup);
    y += 40;

    draw_section_title(x, y, "Update History");
    y += 30;
    gui_text(x, y, "2026-05-20  v0.9.0 -> v1.0.0", COL_TEXT); y += 18;
    gui_text(x, y, "2026-05-15  v0.8.0 -> v0.9.0", COL_TEXT); y += 18;
    gui_text(x, y, "2026-05-10  v0.7.0 -> v0.8.0", COL_TEXT);
}

/* ================================================================
 * Page dispatcher
 * ================================================================ */

typedef void (*page_fn)(void);

static const page_fn pages[PAGE_COUNT] = {
    page_display, page_themes, page_sound, page_network,
    page_accounts, page_storage, page_apps, page_power,
    page_accessibility, page_datetime, page_keyboard, page_updates
};

/* ================================================================
 * Sidebar drawing
 * ================================================================ */

static void draw_sidebar(void) {
    gui_fill(0, 0, SIDEBAR_W, WIN_H, COL_SIDEBAR);

    gui_text(15, 15, "Settings", COL_ACCENT);
    draw_hline(10, 38, SIDEBAR_W - 20, COL_DIVIDER);

    for (int i = 0; i < PAGE_COUNT; i++) {
        int item_y = 50 + i * 36;
        uint32_t bg = COL_SIDEBAR;
        if (i == g_current_page) bg = COL_ACTIVE;
        else if (i == g_hover_page) bg = COL_HOVER;
        gui_fill(4, item_y, SIDEBAR_W - 8, 32, bg);
        gui_text(16, item_y + 8, page_names[i], COL_TEXT);
    }
}

/* ================================================================
 * Event handling
 * ================================================================ */

static int handle_sidebar_click(int x, int y) {
    if (x >= 4 && x < SIDEBAR_W - 4) {
        int idx = (y - 50) / 36;
        if (idx >= 0 && idx < PAGE_COUNT && y >= 50) {
            g_current_page = idx;
            return 1;
        }
    }
    return 0;
}

static void handle_page_click(int x, int y) {
    int rx = x - CONTENT_X - 20;
    int ry = y;
    (void)rx; (void)ry;

    switch (g_current_page) {
    case PAGE_THEMES: {
        /* Check accent color clicks */
        int ay = 120; /* approximate y of accent grid */
        if (y >= ay && y < ay + 30) {
            for (int i = 0; i < 8; i++) {
                int cx = CONTENT_X + 20 + i * 40;
                if (x >= cx && x < cx + 30) {
                    setting_set_int("theme.accent", i);
                    return;
                }
            }
        }
        /* Dark mode toggle */
        if (y >= 85 && y < 110 && x >= CONTENT_X + 170 && x < CONTENT_X + 210) {
            int d = setting_get_int("theme.dark", 1);
            setting_set_int("theme.dark", d ? 0 : 1);
        }
        break;
    }
    case PAGE_POWER: {
        /* Power plan radio buttons */
        int plan_y_base = 265;
        if (y >= plan_y_base && y < plan_y_base + 66) {
            int idx = (y - plan_y_base) / 22;
            if (idx >= 0 && idx <= 2)
                setting_set_int("power.plan", idx);
        }
        break;
    }
    case PAGE_ACCESSIBILITY: {
        /* High contrast toggle */
        if (y >= 85 && y < 110 && x >= CONTENT_X + 220) {
            int v = setting_get_int("a11y.high_contrast", 0);
            setting_set_int("a11y.high_contrast", v ? 0 : 1);
        }
        /* Screen reader toggle */
        if (y >= 230 && y < 260 && x >= CONTENT_X + 220) {
            int v = setting_get_int("a11y.screen_reader", 0);
            setting_set_int("a11y.screen_reader", v ? 0 : 1);
        }
        /* Sticky keys toggle */
        if (y >= 300 && y < 330 && x >= CONTENT_X + 220) {
            int v = setting_get_int("a11y.sticky_keys", 0);
            setting_set_int("a11y.sticky_keys", v ? 0 : 1);
        }
        break;
    }
    case PAGE_UPDATES: {
        /* Auto-update toggle */
        if (y >= 230 && y < 260 && x >= CONTENT_X + 220) {
            int v = setting_get_int("updates.auto", 1);
            setting_set_int("updates.auto", v ? 0 : 1);
        }
        break;
    }
    default:
        break;
    }
}

/* ================================================================
 * Main render loop
 * ================================================================ */

static void render(void) {
    /* Background */
    gui_fill(0, 0, WIN_W, WIN_H, COL_BG);

    /* Sidebar */
    draw_sidebar();

    /* Content area title */
    gui_fill(CONTENT_X, 0, CONTENT_W, 45, COL_SIDEBAR);
    gui_text(CONTENT_X + 20, 14, page_names[g_current_page], COL_TEXT);
    draw_hline(CONTENT_X, 44, CONTENT_W, COL_DIVIDER);

    /* Content */
    if (g_current_page >= 0 && g_current_page < PAGE_COUNT)
        pages[g_current_page]();

    gui_flip();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    g_win = (int)sc5(SYS_GUI_CREATE, WIN_W, WIN_H,
                     (long)(uintptr_t)"Settings", 0, 0);
    if (g_win < 0) {
        sc1(SYS_EXIT, 1);
        return 1;
    }

    settings_load();
    render();

    for (;;) {
        struct gui_event ev;
        int got = gui_poll(&ev);
        if (got > 0) {
            switch (ev.type) {
            case EV_CLOSE:
                sc1(SYS_EXIT, 0);
                return 0;
            case EV_MOUSE_MOVE:
                g_mouse_x = ev.x;
                g_mouse_y = ev.y;
                if (ev.x < SIDEBAR_W) {
                    int idx = (ev.y - 50) / 36;
                    g_hover_page = (idx >= 0 && idx < PAGE_COUNT && ev.y >= 50) ? idx : -1;
                } else {
                    g_hover_page = -1;
                }
                render();
                break;
            case EV_MOUSE_DOWN:
                if (ev.x < SIDEBAR_W) {
                    if (handle_sidebar_click(ev.x, ev.y))
                        render();
                } else {
                    handle_page_click(ev.x, ev.y);
                    render();
                }
                break;
            case EV_KEY:
                if (ev.key == 27) { /* ESC */
                    sc1(SYS_EXIT, 0);
                    return 0;
                }
                break;
            default:
                break;
            }
        } else {
            sc0(SYS_YIELD);
        }
    }
}
