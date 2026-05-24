/* user_gui_about/main.c -- /bin/gui_about
 *
 * System Information panel for the TobyOS desktop.  Uses raw syscalls
 * (no toolkit, no libc).  Auto-refreshes live metrics every 2 seconds.
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
#define SYS_CLOCK_MS        48
#define SYS_SYSTEM_METRICS  74

#define GUI_EV_KEY   4
#define GUI_EV_CLOSE 5

#define WIN_W  520
#define WIN_H  400

#define COL_BG       0x001A1A2A
#define COL_PANEL    0x00252536
#define COL_TEXT     0x00E0E0E0
#define COL_DIM      0x00808890
#define COL_ACCENT   0x004FC3F7
#define COL_BORDER   0x0045475A

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

/* ------------------------------------------------------------------ */
/*  Inline syscall wrappers                                           */
/* ------------------------------------------------------------------ */

static inline ssize_t sys_write(int fd, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
static inline void sys_yield(void) {
    long _d;
    __asm__ volatile ("syscall" : "=a"(_d) : "0"((long)SYS_YIELD)
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
    uint32_t wh = ((uint32_t)(uint16_t)w) | (((uint32_t)(uint16_t)h) << 16);
    register long r10 __asm__("r10") = (long)wh;
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
    uint32_t xy = ((uint32_t)(uint16_t)x) | (((uint32_t)(uint16_t)y) << 16);
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

/* ------------------------------------------------------------------ */
/*  Tiny helpers (no libc)                                            */
/* ------------------------------------------------------------------ */

static size_t slen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static void putstr(const char *s) { sys_write(1, s, slen(s)); }

static char *u64_to_dec(char *end, uint64_t v) {
    *--end = '\0';
    if (v == 0) { *--end = '0'; return end; }
    while (v) { *--end = (char)('0' + (v % 10)); v /= 10; }
    return end;
}

static char *scpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    return dst;
}

static void fmt_u64(char *buf, size_t cap, uint64_t v) {
    char tmp[24];
    const char *s = u64_to_dec(tmp + sizeof(tmp), v);
    char *d = buf;
    char *lim = buf + cap - 1;
    while (*s && d < lim) *d++ = *s++;
    *d = '\0';
}

static void fmt_ip(char *buf, size_t cap, uint32_t ip_be) {
    char *d = buf;
    char *lim = buf + cap - 1;
    for (int i = 0; i < 4; i++) {
        uint32_t octet = (ip_be >> (i * 8)) & 0xFF;
        char tmp[8];
        char *s = u64_to_dec(tmp + sizeof(tmp), octet);
        while (*s && d < lim) *d++ = *s++;
        if (i < 3 && d < lim) *d++ = '.';
    }
    *d = '\0';
}

/* ------------------------------------------------------------------ */
/*  Drawing helpers                                                   */
/* ------------------------------------------------------------------ */

static int g_win;

static void fill(int x, int y, int w, int h, uint32_t c) {
    sys_gui_fill(g_win, x, y, w, h, c);
}
static void text(int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    sys_gui_text(g_win, x, y, s, fg, bg);
}

static void draw_hline(int x, int y, int w, uint32_t c) {
    fill(x, y, w, 1, c);
}

static void draw_info_row(int y, const char *label, const char *value) {
    fill(16, y, 488, 20, COL_PANEL);
    text(24, y + 3, label, COL_DIM, COL_PANEL);
    text(180, y + 3, value, COL_TEXT, COL_PANEL);
    draw_hline(16, y + 20, 488, COL_BORDER);
}

/* ------------------------------------------------------------------ */
/*  Full-frame repaint                                                */
/* ------------------------------------------------------------------ */

static void draw_frame(const struct abi_system_metrics *m) {
    char buf[64];
    char *p;

    /* Background */
    fill(0, 0, WIN_W, WIN_H, COL_BG);

    /* ---- Header bar (0..39) ---- */
    fill(0, 0, WIN_W, 40, COL_PANEL);
    text(16, 12, "About TobyOS", COL_TEXT, COL_PANEL);

    if (m->net_up)
        text(WIN_W - 80, 12, "Online", 0x0066BB6A, COL_PANEL);
    else
        text(WIN_W - 80, 12, "Offline", 0x00EF5350, COL_PANEL);

    draw_hline(0, 40, WIN_W, COL_BORDER);

    /* ---- Logo section (40..119) ---- */
    text(152, 52, "T O B Y O S", COL_ACCENT, COL_BG);
    text(128, 72, "Custom x86-64 Operating System", COL_DIM, COL_BG);
    text(140, 90, "Version 1.0 -- Nixie Desktop", COL_DIM, COL_BG);

    /* ---- Info panel border ---- */
    int py = 124;
    int ph = 210;
    draw_hline(14, py, 492, COL_BORDER);
    draw_hline(14, py + ph, 492, COL_BORDER);
    fill(14, py, 1, ph, COL_BORDER);
    fill(505, py, 1, ph, COL_BORDER);

    /* Panel interior background */
    fill(15, py + 1, 490, ph - 1, COL_PANEL);

    /* ---- Rows ---- */
    int ry = py + 1;

    draw_info_row(ry, "Architecture", "x86-64 (amd64)");
    ry += 21;

    draw_info_row(ry, "Kernel", "TobyOS monolithic + ring-3 userland");
    ry += 21;

    /* CPU */
    p = scpy(buf, "");
    fmt_u64(buf, sizeof(buf), m->tsc_mhz);
    p = buf + slen(buf);
    p = scpy(p, " MHz");
    *p = '\0';
    draw_info_row(ry, "CPU", buf);
    ry += 21;

    /* Memory: used / total MiB  (pages * 4 / 1024) */
    {
        uint64_t used_mib  = (m->used_pages  * 4) / 1024;
        uint64_t total_mib = (m->total_pages * 4) / 1024;
        p = buf;
        { char t[24]; p = scpy(p, u64_to_dec(t + sizeof(t), used_mib)); }
        p = scpy(p, " / ");
        { char t[24]; p = scpy(p, u64_to_dec(t + sizeof(t), total_mib)); }
        p = scpy(p, " MiB");
        *p = '\0';
    }
    draw_info_row(ry, "Memory", buf);
    ry += 21;

    draw_info_row(ry, "Display", "1024x768 32-bit XRGB");
    ry += 21;

    /* Network */
    if (m->net_up) {
        p = scpy(buf, "Online  ");
        fmt_ip(p, sizeof(buf) - (size_t)(p - buf), m->net_ip_be);
    } else {
        scpy(buf, "Offline");
        buf[7] = '\0';
    }
    draw_info_row(ry, "Network", buf);
    ry += 21;

    /* Uptime */
    {
        uint64_t sec = m->uptime_ms / 1000;
        uint64_t h = sec / 3600;
        uint64_t mn = (sec / 60) % 60;
        uint64_t s = sec % 60;
        p = buf;
        { char t[24]; p = scpy(p, u64_to_dec(t + sizeof(t), h)); }
        p = scpy(p, "h ");
        { char t[24]; p = scpy(p, u64_to_dec(t + sizeof(t), mn)); }
        p = scpy(p, "m ");
        { char t[24]; p = scpy(p, u64_to_dec(t + sizeof(t), s)); }
        p = scpy(p, "s");
        *p = '\0';
    }
    draw_info_row(ry, "Uptime", buf);
    ry += 21;

    /* Processes */
    p = buf;
    { char t[24]; p = scpy(p, u64_to_dec(t + sizeof(t), m->process_count)); }
    p = scpy(p, " active");
    *p = '\0';
    draw_info_row(ry, "Processes", buf);
    ry += 21;

    /* Syscalls */
    fmt_u64(buf, sizeof(buf), m->total_syscalls);
    draw_info_row(ry, "Syscalls", buf);
    ry += 21;

    /* GUI FPS */
    fmt_u64(buf, sizeof(buf), m->gui_fps);
    draw_info_row(ry, "GUI FPS", buf);

    /* ---- Footer (bottom 60px) ---- */
    int fy = WIN_H - 52;
    text(72, fy, "Built with love. No warranty expressed or implied.", COL_DIM, COL_BG);
    text(120, fy + 18, "Press Q or close button to exit.", COL_DIM, COL_BG);

    sys_gui_flip(g_win);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                       */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    g_win = sys_gui_create(WIN_W, WIN_H, "About TobyOS");
    if (g_win < 0) {
        putstr("gui_about: cannot create window\n");
        return 1;
    }

    struct abi_system_metrics met;
    long last_draw = 0;

    for (;;) {
        struct gui_event ev;
        while (sys_gui_poll_event(g_win, &ev) > 0) {
            if (ev.type == GUI_EV_CLOSE)
                return 0;
            if (ev.type == GUI_EV_KEY && (ev.key == 'q' || ev.key == 'Q'))
                return 0;
        }

        long now = sys_clock_ms();
        if (now - last_draw >= 2000 || last_draw == 0) {
            sys_system_metrics(&met);
            draw_frame(&met);
            last_draw = now;
        }

        sys_yield();
    }
}
