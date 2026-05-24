/* user_gui_settings/main.c -- /bin/gui_settings.
 *
 * A real, interactive Settings app for the TobyOS desktop shell. It
 * stays deliberately small and syscall-only, but the pages are no
 * longer wallpaper: they read live metrics, write persistent settings,
 * and update compositor-owned shell chrome through SYS_SETTING_SET.
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
typedef unsigned long long uint64_t;
typedef unsigned int       uint32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;

#define SYS_WRITE            1
#define SYS_YIELD            5
#define SYS_GUI_CREATE      10
#define SYS_GUI_FILL        11
#define SYS_GUI_TEXT        12
#define SYS_GUI_FLIP        13
#define SYS_GUI_POLL_EVENT  14
#define SYS_SETTING_GET     21
#define SYS_SETTING_SET     22
#define SYS_LOGOUT          24
#define SYS_SESSION_INFO    25
#define SYS_CLOCK_MS        48
#define SYS_SYSTEM_METRICS  74

#define GUI_EV_MOUSE_DOWN    2
#define GUI_EV_CLOSE         5

struct gui_event {
    int     type;
    int     x;
    int     y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};

struct abi_system_metrics {
    uint64_t uptime_ms;
    uint64_t total_pages;
    uint64_t used_pages;
    uint64_t free_pages;
    uint64_t cpu_user_ns;
    uint64_t cpu_total_ns;
    uint64_t total_syscalls;
    uint64_t context_switches;
    uint64_t gui_frames;
    uint64_t proc_spawns;
    uint64_t proc_exits;
    uint64_t vfs_ops;
    uint64_t vfs_ns;
    uint64_t gui_ns;
    uint64_t net_packets;
    uint64_t net_ns;
    uint32_t process_count;
    uint32_t ready_count;
    uint32_t blocked_count;
    uint32_t cpu_pct;
    uint32_t ram_pct;
    uint32_t gui_pct;
    uint32_t disk_pct;
    uint32_t net_pct;
    uint32_t syscalls_per_s;
    uint32_t gui_fps;
    uint32_t vfs_ops_per_s;
    uint32_t net_packets_per_s;
    uint32_t net_up;
    uint32_t net_ip_be;
    uint32_t tsc_mhz;
    uint32_t _reserved[9];
};

static inline ssize_t sys_write(int fd, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
static inline void sys_yield(void) {
    long _dummy;
    __asm__ volatile ("syscall" : "=a"(_dummy) : "0"((long)SYS_YIELD)
                      : "rcx", "r11", "memory");
}
static inline int sys_gui_create(uint32_t w, uint32_t h, const char *title) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_CREATE), "D"((long)w), "S"((long)h), "d"(title)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_fill(int fd, int x, int y, int w, int h,
                               uint32_t color) {
    long r;
    uint32_t whlen = ((uint32_t)(uint16_t)w) |
                     (((uint32_t)(uint16_t)h) << 16);
    register long r10 __asm__("r10") = (long)whlen;
    register long r8  __asm__("r8")  = (long)color;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FILL), "D"((long)fd), "S"((long)x), "d"((long)y),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_text(int fd, int x, int y, const char *s,
                               uint32_t fg, uint32_t bg) {
    long r;
    uint32_t xy = ((uint32_t)(uint16_t)x) |
                  (((uint32_t)(uint16_t)y) << 16);
    register long r10 __asm__("r10") = (long)fg;
    register long r8  __asm__("r8")  = (long)bg;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_TEXT), "D"((long)fd), "S"((long)xy), "d"(s),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_flip(int fd) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FLIP), "D"((long)fd)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_poll_event(int fd, struct gui_event *ev) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_POLL_EVENT), "D"((long)fd), "S"(ev)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline long sys_setting_get(const char *key, char *out, size_t cap) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_SETTING_GET), "D"(key), "S"(out), "d"(cap)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_setting_set(const char *key, const char *val) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_SETTING_SET), "D"(key), "S"(val)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_logout(void) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_LOGOUT)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_session_info(char *out, size_t cap) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_SESSION_INFO), "D"(out), "S"(cap)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_clock_ms(void) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_CLOCK_MS)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_system_metrics(struct abi_system_metrics *out) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_SYSTEM_METRICS), "D"(out)
        : "rcx", "r11", "memory");
    return r;
}

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}
static void putstr(const char *s) {
    sys_write(1, s, my_strlen(s));
}
static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
static int hit(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}
static void copy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    if (cap == 0) return;
    if (src) {
        for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    }
    dst[i] = 0;
}
static void u64_dec(char *out, size_t cap, uint64_t v) {
    char tmp[24];
    unsigned n = 0;
    if (cap == 0) return;
    if (v == 0) {
        if (cap > 1) { out[0] = '0'; out[1] = 0; }
        else out[0] = 0;
        return;
    }
    while (v && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10ull));
        v /= 10ull;
    }
    unsigned i = 0;
    while (n && i + 1 < cap) out[i++] = tmp[--n];
    out[i] = 0;
}
static void append(char *out, size_t cap, const char *s) {
    size_t n = my_strlen(out);
    size_t i = 0;
    while (s[i] && n + 1 < cap) out[n++] = s[i++];
    out[n] = 0;
}
static void append_u64(char *out, size_t cap, uint64_t v) {
    char b[24];
    u64_dec(b, sizeof(b), v);
    append(out, cap, b);
}
static void fmt_percent(char *out, size_t cap, uint32_t pct) {
    if (pct > 100u) pct = 100u;
    out[0] = 0;
    append_u64(out, cap, pct);
    append(out, cap, "%");
}
static struct abi_system_metrics g_metrics;

static void fmt_memory(char *out, size_t cap) {
    uint64_t used = (g_metrics.used_pages * 4ull) / 1024ull;
    uint64_t total = (g_metrics.total_pages * 4ull) / 1024ull;
    out[0] = 0;
    append_u64(out, cap, used);
    append(out, cap, " / ");
    append_u64(out, cap, total);
    append(out, cap, " MiB");
}
static void fmt_ip(char *out, size_t cap, uint32_t ip) {
    out[0] = 0;
    append_u64(out, cap, ip & 0xFFu);
    append(out, cap, ".");
    append_u64(out, cap, (ip >> 8) & 0xFFu);
    append(out, cap, ".");
    append_u64(out, cap, (ip >> 16) & 0xFFu);
    append(out, cap, ".");
    append_u64(out, cap, (ip >> 24) & 0xFFu);
}

#define WIN_W        680
#define WIN_H        480
#define SIDE_W       166

#define COL_BG       0x000A101Du
#define COL_PANEL    0x00070C16u
#define COL_PANEL2   0x000D1523u
#define COL_HOT      0x001F365Cu
#define COL_LINE     0x00324762u
#define COL_CYAN     0x0000E6FFu
#define COL_ORANGE   0x00FFB347u
#define COL_MAGENTA  0x00FF36C8u
#define COL_TEXT     0x00EAF8FFu
#define COL_DIM      0x0088A2BAu
#define COL_OK       0x0040D060u
#define COL_RED      0x00FF4060u

enum page_id {
    PAGE_HOME = 0,
    PAGE_SYSTEM,
    PAGE_DISPLAY,
    PAGE_SOUND,
    PAGE_STORAGE,
    PAGE_POWER,
    PAGE_DEVICES,
    PAGE_NETWORK,
    PAGE_PERSONAL,
    PAGE_APPS,
    PAGE_ACCOUNTS,
    PAGE_ABOUT,
    PAGE_COUNT
};

static const char *g_nav[PAGE_COUNT] = {
    "Home", "System", "Display", "Sound", "Storage", "Power",
    "Devices", "Network", "Personalization", "Apps", "Accounts", "About TobyOS"
};

struct accent {
    uint32_t color;
    const char *bg_value;
    const char *name;
};

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

static int  g_page = PAGE_PERSONAL;
static int  g_accent = 7;
static int  g_transparency = 1;
static int  g_nixie = 75;
static int  g_mode = 0;
static int  g_night = 0;
static int  g_widgets = 1;
static int  g_animations = 1;
static int  g_bluetooth = 0;
static int  g_hw_cursor = 0;
static int  g_vsync = 1;
static int  g_sys_sounds = 1;
static int  g_notify_sounds = 1;
static int  g_volume = 80;
static int  g_power_plan = 0;
static int  g_sleep_idle = 1;
static char g_user[32] = "toby";
static char g_status[96] = "status: ready.";

static void draw_border(int fd, int x, int y, int w, int h, uint32_t c) {
    sys_gui_fill(fd, x, y, w, 1, c);
    sys_gui_fill(fd, x, y + h - 1, w, 1, c);
    sys_gui_fill(fd, x, y, 1, h, c);
    sys_gui_fill(fd, x + w - 1, y, 1, h, c);
}
static void draw_panel(int fd, int x, int y, int w, int h,
                       uint32_t fill, uint32_t accent) {
    sys_gui_fill(fd, x, y, w, h, fill);
    draw_border(fd, x, y, w, h, COL_LINE);
    if (w > 2) sys_gui_fill(fd, x + 1, y, w - 2, 1, accent);
}
static void draw_button(int fd, int x, int y, int w, int h,
                        const char *label, int hot) {
    uint32_t bg = hot ? COL_HOT : COL_PANEL2;
    draw_panel(fd, x, y, w, h, bg, hot ? COL_CYAN : COL_ORANGE);
    int tx = x + (w - (int)my_strlen(label) * 8) / 2;
    if (tx < x + 4) tx = x + 4;
    sys_gui_text(fd, tx, y + (h - 8) / 2, label, COL_TEXT, bg);
}
static void draw_toggle(int fd, int x, int y, int on) {
    draw_border(fd, x, y, 38, 18, on ? COL_ORANGE : COL_LINE);
    sys_gui_fill(fd, on ? x + 21 : x + 3, y + 3, 14, 12,
                 on ? COL_ORANGE : COL_DIM);
}
static void draw_bar(int fd, int x, int y, int w, uint32_t pct,
                     uint32_t color) {
    if (pct > 100u) pct = 100u;
    sys_gui_fill(fd, x, y, w, 5, COL_LINE);
    sys_gui_fill(fd, x, y, (int)((uint32_t)w * pct / 100u), 5, color);
}
static void draw_metric(int fd, int x, int y, const char *name,
                        const char *value, uint32_t pct, uint32_t color) {
    draw_panel(fd, x, y, 224, 48, COL_PANEL2, color);
    sys_gui_text(fd, x + 12, y + 10, name, COL_TEXT, COL_PANEL2);
    sys_gui_text(fd, x + 112, y + 10, value, COL_DIM, COL_PANEL2);
    draw_bar(fd, x + 12, y + 32, 198, pct, color);
}
static void draw_setting_row(int fd, int x, int y, const char *label,
                             const char *sub, int on) {
    draw_panel(fd, x, y, WIN_W - x - 18, 42, COL_PANEL2,
               on ? COL_CYAN : COL_ORANGE);
    sys_gui_text(fd, x + 12, y + 9, label, COL_TEXT, COL_PANEL2);
    sys_gui_text(fd, x + 12, y + 25, sub, COL_DIM, COL_PANEL2);
    draw_toggle(fd, WIN_W - 64, y + 12, on);
}
static void refresh_metrics(void) {
    if (sys_system_metrics(&g_metrics) != 0) {
        g_metrics.uptime_ms = 0;
    }
}

static int read_bool(const char *key, int def) {
    char b[32];
    long n = sys_setting_get(key, b, sizeof(b));
    if (n <= 0) return def;
    if (streq(b, "off") || streq(b, "0") || streq(b, "false")) return 0;
    if (streq(b, "on") || streq(b, "1") || streq(b, "true")) return 1;
    return def;
}
static void write_bool(const char *key, int on) {
    sys_setting_set(key, on ? "on" : "off");
}
static int parse_int(const char *s, int def) {
    int v = 0;
    int any = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10 + (s[i] - '0');
        any = 1;
    }
    return any ? v : def;
}
static void load_user_from_session(void) {
    char info[96];
    if (sys_session_info(info, sizeof(info)) <= 0) return;
    for (int i = 0; info[i]; i++) {
        if (info[i] == 'u' && info[i + 1] == 's' && info[i + 2] == 'e' &&
            info[i + 3] == 'r' && info[i + 4] == '=') {
            int j = i + 5;
            int k = 0;
            while (info[j] && info[j] != '\n' && k + 1 < (int)sizeof(g_user)) {
                g_user[k++] = info[j++];
            }
            g_user[k] = 0;
            return;
        }
    }
}
static void load_settings(void) {
    char b[64];
    g_transparency = read_bool("ui.transparency", 1);
    g_night = read_bool("ui.night_light", 0);
    g_widgets = read_bool("ui.widgets", 1);
    g_animations = read_bool("ui.animations", 1);
    g_bluetooth = read_bool("device.bluetooth", 0);
    g_hw_cursor = read_bool("display.hw_cursor", 0);
    g_vsync = read_bool("display.vsync", 1);
    g_sys_sounds = read_bool("audio.system_sounds", 1);
    g_notify_sounds = read_bool("audio.notify_sounds", 1);
    g_sleep_idle = read_bool("power.sleep_idle", 1);
    if (sys_setting_get("audio.volume", b, sizeof(b)) > 0) {
        int v = parse_int(b, g_volume);
        if (v >= 0 && v <= 100) g_volume = v;
    }
    if (sys_setting_get("power.plan", b, sizeof(b)) > 0) {
        if (streq(b, "performance")) g_power_plan = 1;
        else g_power_plan = 0;
    }
    if (sys_setting_get("ui.theme", b, sizeof(b)) > 0 && streq(b, "basic")) {
        g_mode = 1;
    }
    if (sys_setting_get("ui.nixie_glow", b, sizeof(b)) > 0) {
        int v = parse_int(b, g_nixie);
        if (v >= 0 && v <= 100) g_nixie = v;
    }
    if (sys_setting_get("desktop.accent", b, sizeof(b)) > 0) {
        for (unsigned i = 0; i < sizeof(g_accents) / sizeof(g_accents[0]); i++) {
            if (streq(b, g_accents[i].name)) g_accent = (int)i;
        }
    }
    if (sys_setting_get("user.last", b, sizeof(b)) > 0) copy(g_user, b, sizeof(g_user));
    load_user_from_session();
}
static void apply_all(void) {
    sys_setting_set("ui.theme", g_mode ? "basic" : "cyber");
    sys_setting_set("desktop.bg", g_accents[g_accent].bg_value);
    sys_setting_set("desktop.accent", g_accents[g_accent].name);
    write_bool("ui.transparency", g_transparency);
    write_bool("ui.night_light", g_night);
    write_bool("ui.widgets", g_widgets);
    write_bool("ui.animations", g_animations);
    write_bool("device.bluetooth", g_bluetooth);
    write_bool("display.hw_cursor", g_hw_cursor);
    write_bool("display.vsync", g_vsync);
    write_bool("audio.system_sounds", g_sys_sounds);
    write_bool("audio.notify_sounds", g_notify_sounds);
    write_bool("power.sleep_idle", g_sleep_idle);
    {
        char vb[8];
        u64_dec(vb, sizeof(vb), (uint64_t)g_volume);
        sys_setting_set("audio.volume", vb);
    }
    sys_setting_set("power.plan", g_power_plan ? "performance" : "balanced");
    char nb[8];
    u64_dec(nb, sizeof(nb), (uint64_t)g_nixie);
    sys_setting_set("ui.nixie_glow", nb);
    sys_setting_set("user.last", g_user);
    copy(g_status, "status: applied + saved.", sizeof(g_status));
}

static void draw_nav(int fd) {
    sys_gui_fill(fd, 0, 35, SIDE_W, WIN_H - 35, COL_PANEL);
    sys_gui_fill(fd, SIDE_W, 35, 1, WIN_H - 35, COL_LINE);
    for (int i = 0; i < PAGE_COUNT; i++) {
        int y = 54 + i * 30;
        uint32_t bg = (i == g_page) ? COL_HOT : COL_PANEL;
        if (i == g_page) {
            sys_gui_fill(fd, 8, y - 5, SIDE_W - 16, 19, bg);
            sys_gui_fill(fd, 8, y - 5, 3, 19, COL_ORANGE);
        }
        sys_gui_text(fd, 20, y, g_nav[i], i == g_page ? COL_TEXT : COL_DIM, bg);
    }
}

static void page_home(int fd, int x) {
    char b[40];
    sys_gui_text(fd, x, 54, "Home", COL_TEXT, COL_BG);
    fmt_percent(b, sizeof(b), g_metrics.cpu_pct);
    draw_metric(fd, x, 82, "CPU", b, g_metrics.cpu_pct, COL_CYAN);
    fmt_memory(b, sizeof(b));
    draw_metric(fd, x + 238, 82, "RAM", b, g_metrics.ram_pct, COL_MAGENTA);
    draw_panel(fd, x, 146, WIN_W - x - 18, 74, COL_PANEL2, COL_ORANGE);
    sys_gui_text(fd, x + 14, 160, "System health", COL_TEXT, COL_PANEL2);
    sys_gui_text(fd, x + 14, 180,
                 g_metrics.net_up ? "Network online and compositor active."
                                  : "Network not linked; compositor still active.",
                 g_metrics.net_up ? COL_OK : COL_ORANGE, COL_PANEL2);
    sys_gui_text(fd, x + 14, 198, "Use the sidebar to edit real saved settings.",
                 COL_DIM, COL_PANEL2);
    draw_button(fd, x, 244, 132, 28, "System", 0);
    draw_button(fd, x + 146, 244, 132, 28, "Network", 0);
    draw_button(fd, x + 292, 244, 132, 28, "Colors", 0);
}

static void page_system(int fd, int x) {
    char b[48];
    sys_gui_text(fd, x, 54, "System", COL_TEXT, COL_BG);
    fmt_percent(b, sizeof(b), g_metrics.cpu_pct);
    draw_metric(fd, x, 82, "CPU", b, g_metrics.cpu_pct, COL_CYAN);
    fmt_memory(b, sizeof(b));
    draw_metric(fd, x + 238, 82, "RAM", b, g_metrics.ram_pct, COL_MAGENTA);
    b[0] = 0; append_u64(b, sizeof(b), g_metrics.gui_fps); append(b, sizeof(b), " fps");
    draw_metric(fd, x, 144, "GUI", b, g_metrics.gui_pct, COL_OK);
    b[0] = 0; append_u64(b, sizeof(b), g_metrics.vfs_ops_per_s); append(b, sizeof(b), " ops/s");
    draw_metric(fd, x + 238, 144, "VFS", b, g_metrics.disk_pct, COL_ORANGE);
    draw_setting_row(fd, x, 220, "Desktop widgets", "Show right-side monitor panels",
                     g_widgets);
    draw_setting_row(fd, x, 272, "Animations", "Use shell transition effects when available",
                     g_animations);
}

static void page_display(int fd, int x) {
    char b[48];
    sys_gui_text(fd, x, 54, "Display", COL_TEXT, COL_BG);
    draw_panel(fd, x, 82, WIN_W - x - 18, 92, COL_PANEL2, COL_CYAN);
    sys_gui_text(fd, x + 14, 98, "Resolution", COL_DIM, COL_PANEL2);
    sys_gui_text(fd, x + 150, 98, "1024 x 768", COL_TEXT, COL_PANEL2);
    sys_gui_text(fd, x + 14, 118, "Color depth", COL_DIM, COL_PANEL2);
    sys_gui_text(fd, x + 150, 118, "32-bit XRGB", COL_TEXT, COL_PANEL2);
    sys_gui_text(fd, x + 14, 138, "Refresh rate", COL_DIM, COL_PANEL2);
    b[0] = 0; append(b, sizeof(b), "~");
    append_u64(b, sizeof(b), g_metrics.gui_fps);
    append(b, sizeof(b), " Hz");
    sys_gui_text(fd, x + 150, 138, b, COL_TEXT, COL_PANEL2);
    draw_setting_row(fd, x, 190, "Hardware cursor",
                     "Use GPU-accelerated mouse pointer", g_hw_cursor);
    draw_setting_row(fd, x, 242, "VSync",
                     "Synchronize frame updates with display", g_vsync);
}

static void page_sound(int fd, int x) {
    char b[48];
    sys_gui_text(fd, x, 54, "Sound", COL_TEXT, COL_BG);
    draw_panel(fd, x, 82, WIN_W - x - 18, 42, COL_PANEL2, COL_ORANGE);
    sys_gui_text(fd, x + 12, 94, "Volume", COL_TEXT, COL_PANEL2);
    sys_gui_fill(fd, WIN_W - 204, 102, 128, 3, COL_LINE);
    sys_gui_fill(fd, WIN_W - 204, 102, 128 * g_volume / 100, 3, COL_ORANGE);
    sys_gui_fill(fd, WIN_W - 204 + 128 * g_volume / 100 - 3, 97, 8, 12,
                 COL_ORANGE);
    fmt_percent(b, sizeof(b), (uint32_t)g_volume);
    sys_gui_text(fd, WIN_W - 64, 94, b, COL_TEXT, COL_PANEL2);
    draw_setting_row(fd, x, 140, "System sounds",
                     "Play sounds for system events", g_sys_sounds);
    draw_setting_row(fd, x, 192, "Notification sounds",
                     "Play audio for app notifications", g_notify_sounds);
    draw_panel(fd, x, 260, WIN_W - x - 18, 46, COL_PANEL2, COL_CYAN);
    sys_gui_text(fd, x + 14, 274, "Audio: PCM 48kHz stereo (virtio-sound)",
                 COL_DIM, COL_PANEL2);
}

static void page_storage(int fd, int x) {
    char b[64];
    sys_gui_text(fd, x, 54, "Storage", COL_TEXT, COL_BG);
    draw_panel(fd, x, 82, WIN_W - x - 18, 80, COL_PANEL2, COL_MAGENTA);
    sys_gui_text(fd, x + 14, 98, "Disk usage", COL_TEXT, COL_PANEL2);
    fmt_percent(b, sizeof(b), g_metrics.disk_pct);
    sys_gui_text(fd, x + 150, 98, b, COL_TEXT, COL_PANEL2);
    draw_bar(fd, x + 14, 122, WIN_W - x - 46, g_metrics.disk_pct, COL_MAGENTA);
    sys_gui_text(fd, x + 14, 136, "TobyFS persistent volume",
                 COL_DIM, COL_PANEL2);
    draw_panel(fd, x, 178, WIN_W - x - 18, 68, COL_PANEL2, COL_ORANGE);
    b[0] = 0; append_u64(b, sizeof(b), g_metrics.vfs_ops_per_s);
    append(b, sizeof(b), " ops/s");
    sys_gui_text(fd, x + 14, 194, "VFS operations", COL_DIM, COL_PANEL2);
    sys_gui_text(fd, x + 150, 194, b, COL_TEXT, COL_PANEL2);
    sys_gui_text(fd, x + 14, 218, "Mount: /data (read-write)",
                 COL_DIM, COL_PANEL2);
}

static void page_power(int fd, int x) {
    char b[64];
    sys_gui_text(fd, x, 54, "Power", COL_TEXT, COL_BG);
    draw_panel(fd, x, 82, WIN_W - x - 18, 52, COL_PANEL2, COL_CYAN);
    sys_gui_text(fd, x + 14, 96, "Power plan", COL_TEXT, COL_PANEL2);
    draw_button(fd, x + 150, 92, 100, 26, "Balanced", g_power_plan == 0);
    draw_button(fd, x + 260, 92, 100, 26, "Performance", g_power_plan == 1);
    draw_setting_row(fd, x, 150, "Sleep on idle",
                     "Suspend after inactivity timeout", g_sleep_idle);
    draw_panel(fd, x, 218, WIN_W - x - 18, 80, COL_PANEL2, COL_ORANGE);
    b[0] = 0; append_u64(b, sizeof(b), g_metrics.uptime_ms / 1000ull);
    append(b, sizeof(b), " seconds");
    sys_gui_text(fd, x + 14, 234, "Uptime", COL_DIM, COL_PANEL2);
    sys_gui_text(fd, x + 150, 234, b, COL_TEXT, COL_PANEL2);
    b[0] = 0; append_u64(b, sizeof(b), g_metrics.tsc_mhz);
    append(b, sizeof(b), " MHz");
    sys_gui_text(fd, x + 14, 258, "TSC frequency", COL_DIM, COL_PANEL2);
    sys_gui_text(fd, x + 150, 258, b, COL_TEXT, COL_PANEL2);
}

static void page_devices(int fd, int x) {
    sys_gui_text(fd, x, 54, "Bluetooth & devices", COL_TEXT, COL_BG);
    draw_setting_row(fd, x, 82, "Bluetooth preference",
                     "Saved for the future Bluetooth service", g_bluetooth);
    draw_setting_row(fd, x, 134, "Night light",
                     "Warm display preference used by shell chrome", g_night);
    draw_panel(fd, x, 202, WIN_W - x - 18, 64, COL_PANEL2, COL_CYAN);
    sys_gui_text(fd, x + 12, 216, "Input", COL_TEXT, COL_PANEL2);
    sys_gui_text(fd, x + 12, 236,
                 "Keyboard and mouse are handled by kernel HID/PS2 paths.",
                 COL_DIM, COL_PANEL2);
}

static void page_network(int fd, int x) {
    char b[64];
    sys_gui_text(fd, x, 54, "Network", COL_TEXT, COL_BG);
    draw_panel(fd, x, 82, WIN_W - x - 18, 92, COL_PANEL2,
               g_metrics.net_up ? COL_CYAN : COL_ORANGE);
    sys_gui_text(fd, x + 14, 98, "Current link", COL_TEXT, COL_PANEL2);
    sys_gui_text(fd, x + 124, 98, g_metrics.net_up ? "Online" : "No link",
                 g_metrics.net_up ? COL_OK : COL_ORANGE, COL_PANEL2);
    fmt_ip(b, sizeof(b), g_metrics.net_ip_be);
    sys_gui_text(fd, x + 14, 122, "IPv4", COL_DIM, COL_PANEL2);
    sys_gui_text(fd, x + 124, 122, g_metrics.net_up ? b : "0.0.0.0",
                 COL_TEXT, COL_PANEL2);
    b[0] = 0; append_u64(b, sizeof(b), g_metrics.net_packets_per_s);
    append(b, sizeof(b), " packets/s");
    sys_gui_text(fd, x + 14, 146, "Traffic", COL_DIM, COL_PANEL2);
    sys_gui_text(fd, x + 124, 146, b, COL_TEXT, COL_PANEL2);
    draw_button(fd, x, 198, 132, 28, "Refresh", 0);
}

static void page_personal(int fd, int x) {
    sys_gui_text(fd, x, 54, "Personalization  >  Colors", COL_TEXT, COL_BG);
    draw_panel(fd, x, 80, 232, 90, COL_PANEL2, COL_CYAN);
    sys_gui_fill(fd, x + 12, 94, 88, 58, 0x00060912u);
    for (int i = 0; i < 7; i++) {
        int bx = x + 18 + i * 12;
        int bh = 12 + ((i * 9) % 36);
        sys_gui_fill(fd, bx, 152 - bh, 8, bh, 0x00071118u);
        sys_gui_fill(fd, bx + 2, 152 - bh + 7, 4, 1,
                     (i & 1) ? COL_CYAN : COL_MAGENTA);
    }
    sys_gui_text(fd, x + 114, 98, "TobyOS preview", COL_TEXT, COL_PANEL2);
    sys_gui_text(fd, x + 114, 118, "Cyber glass shell", COL_DIM, COL_PANEL2);
    sys_gui_fill(fd, x + 114, 144, 88, 2, g_accents[g_accent].color);

    sys_gui_text(fd, x + 260, 86, "Accent color", COL_TEXT, COL_BG);
    for (unsigned i = 0; i < sizeof(g_accents) / sizeof(g_accents[0]); i++) {
        int dx = x + 264 + (int)(i % 6) * 28;
        int dy = 106 + (int)(i / 6) * 28;
        sys_gui_fill(fd, dx, dy, 16, 16, g_accents[i].color);
        draw_border(fd, dx - 2, dy - 2, 20, 20,
                    (int)i == g_accent ? COL_TEXT : COL_LINE);
    }

    draw_setting_row(fd, x, 196, "Transparency effects",
                     "Make shell panels use glass styling", g_transparency);
    draw_panel(fd, x, 248, WIN_W - x - 18, 42, COL_PANEL2, COL_ORANGE);
    sys_gui_text(fd, x + 12, 260, "Nixie Glow", COL_TEXT, COL_PANEL2);
    sys_gui_fill(fd, WIN_W - 204, 268, 128, 3, COL_LINE);
    sys_gui_fill(fd, WIN_W - 204, 268, 128 * g_nixie / 100, 3, COL_ORANGE);
    sys_gui_fill(fd, WIN_W - 204 + 128 * g_nixie / 100 - 3, 263, 8, 12,
                 COL_ORANGE);
    char pct[12];
    fmt_percent(pct, sizeof(pct), (uint32_t)g_nixie);
    sys_gui_text(fd, WIN_W - 64, 260, pct, COL_TEXT, COL_PANEL2);

    draw_panel(fd, x, 300, WIN_W - x - 18, 42, COL_PANEL2, COL_CYAN);
    sys_gui_text(fd, x + 12, 312, "Choose your mode", COL_TEXT, COL_PANEL2);
    draw_button(fd, WIN_W - 138, 308, 112, 26, g_mode ? "Basic" : "Cyber", 0);
}

static void page_apps(int fd, int x) {
    sys_gui_text(fd, x, 54, "Apps", COL_TEXT, COL_BG);
    draw_setting_row(fd, x, 82, "Shell widgets",
                     "Enable notifications, quick settings, monitor stack",
                     g_widgets);
    draw_setting_row(fd, x, 134, "Animations",
                     "Save animation preference for shell apps", g_animations);
    draw_panel(fd, x, 202, WIN_W - x - 18, 74, COL_PANEL2, COL_ORANGE);
    sys_gui_text(fd, x + 12, 216, "Pinned apps", COL_TEXT, COL_PANEL2);
    sys_gui_text(fd, x + 12, 238,
                 "Start menu and taskbar launch real /bin GUI apps.",
                 COL_DIM, COL_PANEL2);
}

static void page_accounts(int fd, int x) {
    sys_gui_text(fd, x, 54, "Accounts", COL_TEXT, COL_BG);
    draw_panel(fd, x, 82, WIN_W - x - 18, 86, COL_PANEL2, COL_CYAN);
    sys_gui_text(fd, x + 14, 100, "Signed in user", COL_TEXT, COL_PANEL2);
    sys_gui_text(fd, x + 150, 100, g_user, COL_ORANGE, COL_PANEL2);
    sys_gui_text(fd, x + 14, 124, "Saved user.last controls login defaults.",
                 COL_DIM, COL_PANEL2);
    draw_button(fd, x + 14, 138, 116, 24, "Cycle user", 0);
    draw_button(fd, x, 196, 116, 28, "Logout", 0);
}

static void page_about(int fd, int x) {
    char b[64];
    sys_gui_text(fd, x, 54, "About TobyOS", COL_TEXT, COL_BG);
    draw_panel(fd, x, 82, WIN_W - x - 18, 158, COL_PANEL2, COL_ORANGE);
    sys_gui_text(fd, x + 14, 100, "TOBYOS NIXIE DESKTOP", COL_ORANGE, COL_PANEL2);
    b[0] = 0; append_u64(b, sizeof(b), g_metrics.uptime_ms / 1000ull);
    append(b, sizeof(b), " seconds");
    sys_gui_text(fd, x + 14, 128, "Uptime", COL_DIM, COL_PANEL2);
    sys_gui_text(fd, x + 150, 128, b, COL_TEXT, COL_PANEL2);
    b[0] = 0; append_u64(b, sizeof(b), g_metrics.process_count);
    append(b, sizeof(b), " processes");
    sys_gui_text(fd, x + 14, 152, "Scheduler", COL_DIM, COL_PANEL2);
    sys_gui_text(fd, x + 150, 152, b, COL_TEXT, COL_PANEL2);
    b[0] = 0; append_u64(b, sizeof(b), g_metrics.total_syscalls);
    sys_gui_text(fd, x + 14, 176, "Syscalls", COL_DIM, COL_PANEL2);
    sys_gui_text(fd, x + 150, 176, b, COL_TEXT, COL_PANEL2);
    sys_gui_text(fd, x + 14, 206, "Settings are persisted in /data/settings.conf.",
                 COL_DIM, COL_PANEL2);
}

static void redraw(int fd) {
    refresh_metrics();
    sys_gui_fill(fd, 0, 0, WIN_W, WIN_H, COL_BG);
    sys_gui_fill(fd, 0, 0, WIN_W, 34, COL_PANEL2);
    sys_gui_fill(fd, 0, 34, WIN_W, 1, COL_CYAN);
    sys_gui_text(fd, 14, 13, "Settings", COL_TEXT, COL_PANEL2);
    sys_gui_text(fd, WIN_W - 154, 13, g_metrics.net_up ? "NET ONLINE" : "NET NO LINK",
                 g_metrics.net_up ? COL_OK : COL_ORANGE, COL_PANEL2);
    draw_nav(fd);

    int x = SIDE_W + 20;
    switch (g_page) {
    case PAGE_HOME:     page_home(fd, x); break;
    case PAGE_SYSTEM:   page_system(fd, x); break;
    case PAGE_DISPLAY:  page_display(fd, x); break;
    case PAGE_SOUND:    page_sound(fd, x); break;
    case PAGE_STORAGE:  page_storage(fd, x); break;
    case PAGE_POWER:    page_power(fd, x); break;
    case PAGE_DEVICES:  page_devices(fd, x); break;
    case PAGE_NETWORK:  page_network(fd, x); break;
    case PAGE_PERSONAL: page_personal(fd, x); break;
    case PAGE_APPS:     page_apps(fd, x); break;
    case PAGE_ACCOUNTS: page_accounts(fd, x); break;
    default:            page_about(fd, x); break;
    }

    draw_button(fd, x, WIN_H - 42, 76, 26, "Apply", 0);
    draw_button(fd, x + 86, WIN_H - 42, 76, 26, "Reset", 0);
    draw_button(fd, x + 172, WIN_H - 42, 76, 26, "Logout", 0);
    sys_gui_text(fd, x + 264, WIN_H - 33, g_status, COL_ORANGE, COL_BG);
    sys_gui_flip(fd);
}

static void reset_defaults(void) {
    g_accent = 7;
    g_transparency = 1;
    g_nixie = 75;
    g_mode = 0;
    g_night = 0;
    g_widgets = 1;
    g_animations = 1;
    g_bluetooth = 0;
    g_hw_cursor = 0;
    g_vsync = 1;
    g_sys_sounds = 1;
    g_notify_sounds = 1;
    g_volume = 80;
    g_power_plan = 0;
    g_sleep_idle = 1;
    copy(g_user, "toby", sizeof(g_user));
    apply_all();
    copy(g_status, "status: defaults restored.", sizeof(g_status));
}

static void cycle_user(void) {
    if (streq(g_user, "toby")) copy(g_user, "neon", sizeof(g_user));
    else if (streq(g_user, "neon")) copy(g_user, "admin", sizeof(g_user));
    else copy(g_user, "toby", sizeof(g_user));
    sys_setting_set("user.last", g_user);
    copy(g_status, "status: account preference saved.", sizeof(g_status));
}

static int handle_nav_click(int x, int y) {
    if (x >= SIDE_W) return 0;
    for (int i = 0; i < PAGE_COUNT; i++) {
        int ry = 54 + i * 30 - 5;
        if (hit(x, y, 8, ry, SIDE_W - 16, 20)) {
            g_page = i;
            return 1;
        }
    }
    return 0;
}

static void handle_click(int fd, int px, int py) {
    int x = SIDE_W + 20;
    if (handle_nav_click(px, py)) { redraw(fd); return; }

    if (hit(px, py, x, WIN_H - 42, 76, 26)) {
        apply_all(); redraw(fd); return;
    }
    if (hit(px, py, x + 86, WIN_H - 42, 76, 26)) {
        reset_defaults(); redraw(fd); return;
    }
    if (hit(px, py, x + 172, WIN_H - 42, 76, 26)) {
        sys_logout(); return;
    }

    if (g_page == PAGE_HOME) {
        if (hit(px, py, x, 244, 132, 28)) g_page = PAGE_SYSTEM;
        else if (hit(px, py, x + 146, 244, 132, 28)) g_page = PAGE_NETWORK;
        else if (hit(px, py, x + 292, 244, 132, 28)) g_page = PAGE_PERSONAL;
        redraw(fd); return;
    }

    if (g_page == PAGE_DISPLAY) {
        if (hit(px, py, WIN_W - 70, 202, 52, 24)) {
            g_hw_cursor = !g_hw_cursor; write_bool("display.hw_cursor", g_hw_cursor);
            copy(g_status, "status: hw cursor saved.", sizeof(g_status));
        } else if (hit(px, py, WIN_W - 70, 254, 52, 24)) {
            g_vsync = !g_vsync; write_bool("display.vsync", g_vsync);
            copy(g_status, "status: vsync saved.", sizeof(g_status));
        }
        redraw(fd); return;
    }

    if (g_page == PAGE_SOUND) {
        if (hit(px, py, WIN_W - 210, 88, 148, 30)) {
            int v = (px - (WIN_W - 204)) * 100 / 128;
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            g_volume = v;
            char vb[8]; u64_dec(vb, sizeof(vb), (uint64_t)g_volume);
            sys_setting_set("audio.volume", vb);
            copy(g_status, "status: volume saved.", sizeof(g_status));
        } else if (hit(px, py, WIN_W - 70, 152, 52, 24)) {
            g_sys_sounds = !g_sys_sounds; write_bool("audio.system_sounds", g_sys_sounds);
            copy(g_status, "status: system sounds saved.", sizeof(g_status));
        } else if (hit(px, py, WIN_W - 70, 204, 52, 24)) {
            g_notify_sounds = !g_notify_sounds; write_bool("audio.notify_sounds", g_notify_sounds);
            copy(g_status, "status: notify sounds saved.", sizeof(g_status));
        }
        redraw(fd); return;
    }

    if (g_page == PAGE_POWER) {
        if (hit(px, py, x + 150, 92, 100, 26)) {
            g_power_plan = 0;
            sys_setting_set("power.plan", "balanced");
            copy(g_status, "status: power plan saved.", sizeof(g_status));
        } else if (hit(px, py, x + 260, 92, 100, 26)) {
            g_power_plan = 1;
            sys_setting_set("power.plan", "performance");
            copy(g_status, "status: power plan saved.", sizeof(g_status));
        } else if (hit(px, py, WIN_W - 70, 162, 52, 24)) {
            g_sleep_idle = !g_sleep_idle; write_bool("power.sleep_idle", g_sleep_idle);
            copy(g_status, "status: sleep setting saved.", sizeof(g_status));
        }
        redraw(fd); return;
    }

    if (g_page == PAGE_SYSTEM) {
        if (hit(px, py, WIN_W - 70, 232, 52, 24)) {
            g_widgets = !g_widgets; write_bool("ui.widgets", g_widgets);
            copy(g_status, "status: widgets setting saved.", sizeof(g_status));
        } else if (hit(px, py, WIN_W - 70, 284, 52, 24)) {
            g_animations = !g_animations; write_bool("ui.animations", g_animations);
            copy(g_status, "status: animation setting saved.", sizeof(g_status));
        }
        redraw(fd); return;
    }

    if (g_page == PAGE_DEVICES) {
        if (hit(px, py, WIN_W - 70, 94, 52, 24)) {
            g_bluetooth = !g_bluetooth; write_bool("device.bluetooth", g_bluetooth);
            copy(g_status, "status: bluetooth preference saved.", sizeof(g_status));
        } else if (hit(px, py, WIN_W - 70, 146, 52, 24)) {
            g_night = !g_night; write_bool("ui.night_light", g_night);
            copy(g_status, "status: night light saved.", sizeof(g_status));
        }
        redraw(fd); return;
    }

    if (g_page == PAGE_NETWORK) {
        if (hit(px, py, x, 198, 132, 28)) {
            copy(g_status, "status: network metrics refreshed.", sizeof(g_status));
            redraw(fd); return;
        }
    }

    if (g_page == PAGE_PERSONAL) {
        for (unsigned i = 0; i < sizeof(g_accents) / sizeof(g_accents[0]); i++) {
            int dx = x + 264 + (int)(i % 6) * 28;
            int dy = 106 + (int)(i / 6) * 28;
            if (hit(px, py, dx - 4, dy - 4, 24, 24)) {
                g_accent = (int)i;
                sys_setting_set("desktop.bg", g_accents[g_accent].bg_value);
                sys_setting_set("desktop.accent", g_accents[g_accent].name);
                copy(g_status, "status: accent saved.", sizeof(g_status));
                redraw(fd); return;
            }
        }
        if (hit(px, py, WIN_W - 70, 208, 52, 24)) {
            g_transparency = !g_transparency;
            write_bool("ui.transparency", g_transparency);
            copy(g_status, "status: transparency saved.", sizeof(g_status));
        } else if (hit(px, py, WIN_W - 210, 254, 148, 30)) {
            int v = (px - (WIN_W - 204)) * 100 / 128;
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            g_nixie = v;
            char nb[8]; u64_dec(nb, sizeof(nb), (uint64_t)g_nixie);
            sys_setting_set("ui.nixie_glow", nb);
            copy(g_status, "status: glow saved.", sizeof(g_status));
        } else if (hit(px, py, WIN_W - 138, 308, 112, 26)) {
            g_mode = !g_mode;
            sys_setting_set("ui.theme", g_mode ? "basic" : "cyber");
            copy(g_status, "status: theme applied live.", sizeof(g_status));
        }
        redraw(fd); return;
    }

    if (g_page == PAGE_APPS) {
        if (hit(px, py, WIN_W - 70, 94, 52, 24)) {
            g_widgets = !g_widgets; write_bool("ui.widgets", g_widgets);
            copy(g_status, "status: widgets setting saved.", sizeof(g_status));
        } else if (hit(px, py, WIN_W - 70, 146, 52, 24)) {
            g_animations = !g_animations; write_bool("ui.animations", g_animations);
            copy(g_status, "status: animation setting saved.", sizeof(g_status));
        }
        redraw(fd); return;
    }

    if (g_page == PAGE_ACCOUNTS) {
        if (hit(px, py, x + 14, 138, 116, 24)) {
            cycle_user(); redraw(fd); return;
        }
        if (hit(px, py, x, 196, 116, 28)) {
            sys_logout(); return;
        }
    }
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int fd = sys_gui_create(WIN_W, WIN_H, "Settings");
    if (fd < 0) {
        putstr("gui_settings: sys_gui_create failed\n");
        return 1;
    }

    load_settings();
    redraw(fd);
    long last_live = sys_clock_ms();

    for (;;) {
        struct gui_event ev;
        int got = sys_gui_poll_event(fd, &ev);
        if (got < 0) return 1;
        if (got == 0) {
            long now = sys_clock_ms();
            if (now - last_live >= 1000 &&
                (g_page == PAGE_HOME || g_page == PAGE_SYSTEM ||
                 g_page == PAGE_DISPLAY || g_page == PAGE_STORAGE ||
                 g_page == PAGE_POWER || g_page == PAGE_NETWORK ||
                 g_page == PAGE_ABOUT)) {
                last_live = now;
                redraw(fd);
            }
            sys_yield();
            continue;
        }
        if (ev.type == GUI_EV_CLOSE) return 0;
        if (ev.type == GUI_EV_MOUSE_DOWN && ev.button) {
            handle_click(fd, ev.x, ev.y);
        }
    }
}
