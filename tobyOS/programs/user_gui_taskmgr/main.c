/* user_gui_taskmgr/main.c -- /bin/gui_taskmgr, a tabbed GUI task manager.
 *
 * Three tabs: Processes, Performance, Services.
 * Auto-refreshes every ~1 second. Keys: j/k scroll, Tab cycles tabs,
 * 1/2/3 selects tab, Delete/x kills selected process, q quits.
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
typedef unsigned long long uint64_t;
typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;

#define NULL ((void *)0)

#define SYS_EXIT            0
#define SYS_WRITE           1
#define SYS_YIELD           5
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_CLOCK_MS       48
#define SYS_SVC_LIST       65
#define SYS_SYSTEM_METRICS 74
#define SYS_KILL           75

struct gui_event {
    int     type;
    int     x;
    int     y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};
#define GUI_EV_MOUSE_DOWN 2
#define GUI_EV_KEY        4
#define GUI_EV_CLOSE      5

#define ABI_SVC_NAME_MAX 16
#define ABI_SVC_PATH_MAX 64

struct abi_service_info {
    char     name[ABI_SVC_NAME_MAX];
    char     path[ABI_SVC_PATH_MAX];
    uint32_t state;
    uint32_t kind;
    int32_t  pid;
    int32_t  last_exit;
    uint32_t restart_count;
    uint32_t crash_count;
    uint64_t last_start_ms;
    uint64_t last_crash_ms;
    uint64_t backoff_until_ms;
    uint8_t  autorestart;
    uint8_t  _pad[7];
    uint64_t _reserved[2];
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

/* ---- syscall stubs --------------------------------------------- */

static inline void sys_exit(int code) {
    __asm__ volatile("syscall"
        : : "a"((long)SYS_EXIT), "D"((long)code)
        : "rcx", "r11", "memory");
    __builtin_unreachable();
}
static inline ssize_t sys_write(int fd, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
static inline void sys_yield(void) {
    long _dummy;
    __asm__ volatile("syscall"
        : "=a"(_dummy) : "0"(5L)
        : "rcx", "r11", "memory");
}
static inline int sys_gui_create(uint32_t w, uint32_t h, const char *title) {
    long r;
    __asm__ volatile("syscall"
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
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FILL), "D"((long)fd),
          "S"((long)x), "d"((long)y),
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
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_TEXT), "D"((long)fd),
          "S"((long)xy), "d"(s),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_flip(int fd) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FLIP), "D"((long)fd)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_poll_event(int fd, struct gui_event *ev) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_POLL_EVENT), "D"((long)fd), "S"(ev)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline long sys_clock_ms(void) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_CLOCK_MS)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_svc_list(struct abi_service_info *out, uint32_t cap) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_SVC_LIST), "D"(out), "S"((long)cap)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_system_metrics(struct abi_system_metrics *out) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_SYSTEM_METRICS), "D"(out)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_kill(int pid) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_KILL), "D"((long)pid)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny helpers ---------------------------------------------- */

static size_t my_strlen(const char *s) {
    const char *p = s; while (*p) p++; return (size_t)(p - s);
}
static void my_memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
}
static void my_strncpy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void fmt_uint(char *out, unsigned v) {
    char tmp[16]; int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; } }
    int i = 0;
    while (n--) out[i++] = tmp[n];
    out[i] = '\0';
}

static void fmt_uint64(char *out, uint64_t v) {
    char tmp[24]; int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } }
    int i = 0;
    while (n--) out[i++] = tmp[n];
    out[i] = '\0';
}

static int str_append(char *buf, int pos, const char *s) {
    while (*s) buf[pos++] = *s++;
    return pos;
}

static int str_append_uint(char *buf, int pos, unsigned v) {
    char tmp[16];
    fmt_uint(tmp, v);
    return str_append(buf, pos, tmp);
}

static int str_append_uint64(char *buf, int pos, uint64_t v) {
    char tmp[24];
    fmt_uint64(tmp, v);
    return str_append(buf, pos, tmp);
}

/* pad/truncate a string into a fixed-width field */
static void fmt_fixed(char *out, const char *src, int width) {
    int i = 0;
    while (i < width && src[i]) { out[i] = src[i]; i++; }
    while (i < width) out[i++] = ' ';
    out[width] = '\0';
}

/* ---- layout ---------------------------------------------------- */

#define WIN_W       700
#define WIN_H       500

#define TAB_BAR_H    30
#define TAB_W       110
#define TAB_COUNT     3
#define CONTENT_Y    (TAB_BAR_H + 1)
#define CONTENT_H    (WIN_H - CONTENT_Y)

#define ROW_H        16
#define CELL_W        8
#define CELL_H       12

/* dark theme */
#define COL_BG        0x001A1B26u
#define COL_TAB_BG    0x00141520u
#define COL_TAB_ACT   0x001A1B26u
#define COL_TAB_HOV   0x00222436u
#define COL_TAB_TEXT  0x00565F89u
#define COL_TAB_ATXT  0x00C0CAF5u
#define COL_TAB_LINE  0x007AA2F7u
#define COL_HEADER    0x00292E42u
#define COL_TEXT      0x00C0CAF5u
#define COL_DIM       0x00565F89u
#define COL_ACCENT    0x007AA2F7u
#define COL_GREEN     0x009ECE6Au
#define COL_BLUE      0x007DCFFFu
#define COL_YELLOW    0x00E0AF68u
#define COL_RED       0x00F7768Eu
#define COL_ORANGE    0x00FF9E64u
#define COL_ROW_ALT   0x001F2030u
#define COL_ROW_SEL   0x00283457u
#define COL_BAR_BG    0x00303340u
#define COL_BAR_CPU   0x009ECE6Au
#define COL_BAR_MEM   0x007AA2F7u
#define COL_BTN_BG    0x00F7768Eu
#define COL_BTN_TEXT  0x00FFFFFFu
#define COL_BTN_DIS   0x00404050u
#define COL_GRAPH_BG  0x00252535u
#define COL_GRAPH_CPU 0x009ECE6Au
#define COL_GRAPH_MEM 0x007AA2F7u
#define COL_STATUS_BG 0x00141520u

/* ---- state ----------------------------------------------------- */

#define MAX_SVCS      64
#define HISTORY_LEN   16

enum { TAB_PROCESSES, TAB_PERFORMANCE, TAB_SERVICES };

static struct abi_service_info g_svcs[MAX_SVCS];
static int  g_svc_count;
static struct abi_system_metrics g_metrics;

static int  g_tab;
static int  g_scroll;
static int  g_selected;

static uint32_t g_cpu_hist[HISTORY_LEN];
static uint32_t g_ram_hist[HISTORY_LEN];
static int      g_hist_idx;
static int      g_hist_filled;

static const char *g_tab_names[TAB_COUNT] = {
    "Processes", "Performance", "Services"
};

/* ---- data refresh ---------------------------------------------- */

static void refresh_data(void) {
    my_memset(&g_metrics, 0, sizeof(g_metrics));
    sys_system_metrics(&g_metrics);

    my_memset(g_svcs, 0, sizeof(g_svcs));
    long n = sys_svc_list(g_svcs, MAX_SVCS);
    g_svc_count = (n > 0) ? (int)n : 0;

    g_cpu_hist[g_hist_idx] = g_metrics.cpu_pct;
    g_ram_hist[g_hist_idx] = g_metrics.ram_pct;
    g_hist_idx = (g_hist_idx + 1) % HISTORY_LEN;
    if (g_hist_filled < HISTORY_LEN) g_hist_filled++;
}

/* ---- helper drawing -------------------------------------------- */

static const char *state_str(uint32_t s) {
    switch (s) {
    case 0:  return "Stopped";
    case 1:  return "Running";
    case 2:  return "Crashed";
    case 3:  return "Failed";
    case 4:  return "Disabled";
    default: return "Unknown";
    }
}

static uint32_t state_color(uint32_t s) {
    switch (s) {
    case 1:  return COL_GREEN;
    case 2:  return COL_RED;
    case 3:  return COL_RED;
    default: return COL_DIM;
    }
}

static void format_uptime(char *buf, uint64_t ms) {
    unsigned total_s = (unsigned)(ms / 1000);
    unsigned d = total_s / 86400;
    unsigned h = (total_s / 3600) % 24;
    unsigned m = (total_s / 60) % 60;
    unsigned s = total_s % 60;
    int p = 0;
    if (d > 0) {
        p = str_append_uint(buf, p, d);
        buf[p++] = 'd';
        buf[p++] = ' ';
    }
    p = str_append_uint(buf, p, h);
    buf[p++] = 'h';
    buf[p++] = ' ';
    p = str_append_uint(buf, p, m);
    buf[p++] = 'm';
    buf[p++] = ' ';
    p = str_append_uint(buf, p, s);
    buf[p++] = 's';
    buf[p] = '\0';
}

static void draw_bar(int fd, int x, int y, int max_w, int h,
                     uint32_t pct, uint32_t fg, uint32_t bg) {
    sys_gui_fill(fd, x, y, max_w, h, bg);
    int fill_w = (int)((pct * (uint32_t)max_w) / 100u);
    if (fill_w > max_w) fill_w = max_w;
    if (fill_w > 0)
        sys_gui_fill(fd, x, y, fill_w, h, fg);
}

static void draw_graph(int fd, int x, int y, int w, int h,
                       const uint32_t *hist, int count, int head,
                       uint32_t bar_col, uint32_t bg_col) {
    sys_gui_fill(fd, x, y, w, h, bg_col);

    if (count == 0) return;
    int bar_w = w / HISTORY_LEN;
    if (bar_w < 2) bar_w = 2;
    int gap = 1;
    int effective_w = bar_w - gap;
    if (effective_w < 1) effective_w = 1;

    for (int i = 0; i < count && i < HISTORY_LEN; i++) {
        int idx = (head - count + i + HISTORY_LEN) % HISTORY_LEN;
        uint32_t pct = hist[idx];
        if (pct > 100) pct = 100;
        int bar_h = (int)(pct * (uint32_t)h / 100u);
        if (bar_h < 1 && pct > 0) bar_h = 1;
        int bx = x + i * bar_w;
        int by = y + h - bar_h;
        if (bar_h > 0)
            sys_gui_fill(fd, bx, by, effective_w, bar_h, bar_col);
    }
}

static int hit_rect(int mx, int my, int rx, int ry, int rw, int rh) {
    return mx >= rx && mx < rx + rw && my >= ry && my < ry + rh;
}

/* ---- tab bar drawing ------------------------------------------- */

static void draw_tab_bar(int fd) {
    sys_gui_fill(fd, 0, 0, WIN_W, TAB_BAR_H, COL_TAB_BG);

    for (int i = 0; i < TAB_COUNT; i++) {
        int tx = i * TAB_W;
        int active = (i == g_tab);
        uint32_t bg = active ? COL_TAB_ACT : COL_TAB_BG;
        uint32_t fg = active ? COL_TAB_ATXT : COL_TAB_TEXT;

        sys_gui_fill(fd, tx, 0, TAB_W, TAB_BAR_H, bg);

        int text_x = tx + (TAB_W - (int)my_strlen(g_tab_names[i]) * CELL_W) / 2;
        int text_y = (TAB_BAR_H - CELL_H) / 2;
        sys_gui_text(fd, text_x, text_y, g_tab_names[i], fg, bg);

        if (active)
            sys_gui_fill(fd, tx, TAB_BAR_H - 3, TAB_W, 3, COL_TAB_LINE);
    }

    sys_gui_fill(fd, 0, TAB_BAR_H, WIN_W, 1, COL_HEADER);
}

/* ---- Processes tab --------------------------------------------- */

#define PROC_HDR_Y     (CONTENT_Y + 2)
#define PROC_HDR_H     18
#define PROC_LIST_Y    (PROC_HDR_Y + PROC_HDR_H)
#define PROC_STATUS_H  22
#define PROC_LIST_H    (WIN_H - PROC_LIST_Y - PROC_STATUS_H)
#define PROC_ROWS      (PROC_LIST_H / ROW_H)

#define KILL_BTN_W     60
#define KILL_BTN_H     18
#define KILL_BTN_X     (WIN_W - KILL_BTN_W - 14)
#define KILL_BTN_Y     (WIN_H - PROC_STATUS_H + 2)

static void draw_processes(int fd) {
    /* column headers */
    sys_gui_fill(fd, 0, PROC_HDR_Y, WIN_W, PROC_HDR_H, COL_HEADER);
    sys_gui_text(fd, 14,  PROC_HDR_Y + 3, "PID",   COL_ACCENT, COL_HEADER);
    sys_gui_text(fd, 80,  PROC_HDR_Y + 3, "Name",  COL_ACCENT, COL_HEADER);
    sys_gui_text(fd, 320, PROC_HDR_Y + 3, "State", COL_ACCENT, COL_HEADER);
    sys_gui_text(fd, 440, PROC_HDR_Y + 3, "CPU%",  COL_ACCENT, COL_HEADER);

    /* list */
    int visible = PROC_ROWS;
    if (visible < 0) visible = 0;

    for (int vi = 0; vi < visible; vi++) {
        int idx = g_scroll + vi;
        int ry = PROC_LIST_Y + vi * ROW_H;

        if (idx >= g_svc_count) {
            sys_gui_fill(fd, 0, ry, WIN_W, ROW_H, COL_BG);
            continue;
        }

        int is_sel = (idx == g_selected);
        uint32_t rbg = is_sel ? COL_ROW_SEL
                     : ((vi & 1) ? COL_ROW_ALT : COL_BG);

        sys_gui_fill(fd, 0, ry, WIN_W, ROW_H, rbg);
        if (is_sel)
            sys_gui_fill(fd, 0, ry, 3, ROW_H, COL_ACCENT);

        const struct abi_service_info *svc = &g_svcs[idx];

        char pidbuf[12];
        if (svc->pid >= 0) fmt_uint(pidbuf, (unsigned)svc->pid);
        else { pidbuf[0] = '-'; pidbuf[1] = '\0'; }
        sys_gui_text(fd, 14, ry + 2, pidbuf, COL_TEXT, rbg);

        char namebuf[24];
        my_strncpy(namebuf, svc->name, 22);
        sys_gui_text(fd, 80, ry + 2, namebuf, COL_TEXT, rbg);

        sys_gui_text(fd, 320, ry + 2, state_str(svc->state),
                     state_color(svc->state), rbg);

        /* CPU% column: use the global cpu_pct as a per-process approximation */
        char cpubuf[8];
        if (svc->state == 1) {
            int p = 0;
            unsigned per_proc = g_metrics.cpu_pct;
            if (g_metrics.process_count > 1)
                per_proc = g_metrics.cpu_pct / g_metrics.process_count;
            p = str_append_uint(cpubuf, p, per_proc);
            cpubuf[p++] = '%';
            cpubuf[p] = '\0';
        } else {
            cpubuf[0] = '0'; cpubuf[1] = '%'; cpubuf[2] = '\0';
        }
        sys_gui_text(fd, 440, ry + 2, cpubuf, COL_DIM, rbg);
    }

    /* scrollbar */
    if (g_svc_count > visible && visible > 0) {
        int sb_x = WIN_W - 6;
        int sb_h = visible * ROW_H;
        sys_gui_fill(fd, sb_x, PROC_LIST_Y, 4, sb_h, COL_BAR_BG);
        int thumb_h = (visible * sb_h) / g_svc_count;
        if (thumb_h < 8) thumb_h = 8;
        int max_scroll = g_svc_count - visible;
        int thumb_y = PROC_LIST_Y;
        if (max_scroll > 0)
            thumb_y += (g_scroll * (sb_h - thumb_h)) / max_scroll;
        sys_gui_fill(fd, sb_x, thumb_y, 4, thumb_h, COL_ACCENT);
    }

    /* status bar */
    sys_gui_fill(fd, 0, WIN_H - PROC_STATUS_H, WIN_W, PROC_STATUS_H, COL_STATUS_BG);
    sys_gui_fill(fd, 0, WIN_H - PROC_STATUS_H, WIN_W, 1, COL_HEADER);

    char stbuf[64];
    int sp = 0;
    sp = str_append(stbuf, sp, "Total: ");
    sp = str_append_uint(stbuf, sp, (unsigned)g_svc_count);
    sp = str_append(stbuf, sp, "  Procs: ");
    sp = str_append_uint(stbuf, sp, g_metrics.process_count);
    sp = str_append(stbuf, sp, "  Ready: ");
    sp = str_append_uint(stbuf, sp, g_metrics.ready_count);
    sp = str_append(stbuf, sp, "  Blocked: ");
    sp = str_append_uint(stbuf, sp, g_metrics.blocked_count);
    stbuf[sp] = '\0';
    sys_gui_text(fd, 14, KILL_BTN_Y + 3, stbuf, COL_DIM, COL_STATUS_BG);

    /* kill button */
    int can_kill = (g_selected >= 0 && g_selected < g_svc_count &&
                    g_svcs[g_selected].pid >= 0 && g_svcs[g_selected].state == 1);
    uint32_t btn_bg = can_kill ? COL_BTN_BG : COL_BTN_DIS;
    uint32_t btn_fg = can_kill ? COL_BTN_TEXT : COL_DIM;
    sys_gui_fill(fd, KILL_BTN_X, KILL_BTN_Y, KILL_BTN_W, KILL_BTN_H, btn_bg);
    sys_gui_text(fd, KILL_BTN_X + 12, KILL_BTN_Y + 3, "Kill", btn_fg, btn_bg);
}

/* ---- Performance tab ------------------------------------------- */

static void draw_performance(int fd) {
    int y = CONTENT_Y + 10;
    int lx = 20;
    int rx = 370;

    /* ---- CPU section ---- */
    sys_gui_text(fd, lx, y, "CPU Usage", COL_ACCENT, COL_BG);
    y += 18;

    char big_cpu[16];
    int bp = 0;
    bp = str_append_uint(big_cpu, bp, g_metrics.cpu_pct);
    big_cpu[bp++] = '%';
    big_cpu[bp] = '\0';
    sys_gui_text(fd, lx, y, big_cpu, COL_GREEN, COL_BG);

    /* horizontal bar */
    draw_bar(fd, lx + 60, y, 250, 12, g_metrics.cpu_pct, COL_BAR_CPU, COL_BAR_BG);
    y += 22;

    /* graph */
    sys_gui_text(fd, lx, y, "History (16s)", COL_DIM, COL_BG);
    y += 14;
    draw_graph(fd, lx, y, HISTORY_LEN * 18, 60,
               g_cpu_hist, g_hist_filled, g_hist_idx,
               COL_GRAPH_CPU, COL_GRAPH_BG);
    y += 70;

    /* ---- RAM section ---- */
    sys_gui_text(fd, lx, y, "Memory Usage", COL_ACCENT, COL_BG);
    y += 18;

    char big_ram[16];
    int rp = 0;
    rp = str_append_uint(big_ram, rp, g_metrics.ram_pct);
    big_ram[rp++] = '%';
    big_ram[rp] = '\0';
    sys_gui_text(fd, lx, y, big_ram, COL_BLUE, COL_BG);

    draw_bar(fd, lx + 60, y, 250, 12, g_metrics.ram_pct, COL_BAR_MEM, COL_BAR_BG);
    y += 22;

    sys_gui_text(fd, lx, y, "History (16s)", COL_DIM, COL_BG);
    y += 14;
    draw_graph(fd, lx, y, HISTORY_LEN * 18, 60,
               g_ram_hist, g_hist_filled, g_hist_idx,
               COL_GRAPH_MEM, COL_GRAPH_BG);

    /* ---- Right-side stats ---- */
    int sy = CONTENT_Y + 10;

    sys_gui_text(fd, rx, sy, "System Info", COL_ACCENT, COL_BG);
    sy += 22;

    /* uptime */
    char uptbuf[48];
    int up = 0;
    up = str_append(uptbuf, up, "Uptime:  ");
    char uptmp[32];
    format_uptime(uptmp, g_metrics.uptime_ms);
    up = str_append(uptbuf, up, uptmp);
    uptbuf[up] = '\0';
    sys_gui_text(fd, rx, sy, uptbuf, COL_TEXT, COL_BG);
    sy += 20;

    /* pages */
    char pgbuf[64];
    int pg = 0;
    pg = str_append(pgbuf, pg, "Pages:   ");
    pg = str_append_uint64(pgbuf, pg, g_metrics.used_pages);
    pg = str_append(pgbuf, pg, " / ");
    pg = str_append_uint64(pgbuf, pg, g_metrics.total_pages);
    pgbuf[pg] = '\0';
    sys_gui_text(fd, rx, sy, pgbuf, COL_TEXT, COL_BG);
    sy += 20;

    /* processes */
    char pcbuf[48];
    int pc = 0;
    pc = str_append(pcbuf, pc, "Procs:   ");
    pc = str_append_uint(pcbuf, pc, g_metrics.process_count);
    pc = str_append(pcbuf, pc, " (");
    pc = str_append_uint(pcbuf, pc, g_metrics.ready_count);
    pc = str_append(pcbuf, pc, " rdy)");
    pcbuf[pc] = '\0';
    sys_gui_text(fd, rx, sy, pcbuf, COL_TEXT, COL_BG);
    sy += 28;

    /* divider */
    sys_gui_fill(fd, rx, sy - 8, 280, 1, COL_HEADER);

    sys_gui_text(fd, rx, sy, "Counters /s", COL_ACCENT, COL_BG);
    sy += 22;

    /* syscalls/s */
    char scbuf[48];
    int sc = 0;
    sc = str_append(scbuf, sc, "Syscalls:  ");
    sc = str_append_uint(scbuf, sc, g_metrics.syscalls_per_s);
    scbuf[sc] = '\0';
    sys_gui_text(fd, rx, sy, scbuf, COL_TEXT, COL_BG);
    sy += 20;

    /* ctx switches */
    char cxbuf[48];
    int cx = 0;
    cx = str_append(cxbuf, cx, "Ctx sw:    ");
    cx = str_append_uint64(cxbuf, cx, g_metrics.context_switches);
    cxbuf[cx] = '\0';
    sys_gui_text(fd, rx, sy, cxbuf, COL_TEXT, COL_BG);
    sy += 20;

    /* GUI fps */
    char fbuf[32];
    int fi = 0;
    fi = str_append(fbuf, fi, "GUI FPS:   ");
    fi = str_append_uint(fbuf, fi, g_metrics.gui_fps);
    fbuf[fi] = '\0';
    sys_gui_text(fd, rx, sy, fbuf, COL_TEXT, COL_BG);
    sy += 20;

    /* VFS ops/s */
    char vbuf[32];
    int vi = 0;
    vi = str_append(vbuf, vi, "VFS ops/s: ");
    vi = str_append_uint(vbuf, vi, g_metrics.vfs_ops_per_s);
    vbuf[vi] = '\0';
    sys_gui_text(fd, rx, sy, vbuf, COL_TEXT, COL_BG);
    sy += 20;

    /* Net packets/s */
    char nbuf[32];
    int ni = 0;
    ni = str_append(nbuf, ni, "Net pkt/s: ");
    ni = str_append_uint(nbuf, ni, g_metrics.net_packets_per_s);
    nbuf[ni] = '\0';
    sys_gui_text(fd, rx, sy, nbuf, COL_TEXT, COL_BG);
    sy += 20;

    /* TSC MHz */
    char tbuf[32];
    int ti = 0;
    ti = str_append(tbuf, ti, "TSC MHz:   ");
    ti = str_append_uint(tbuf, ti, g_metrics.tsc_mhz);
    tbuf[ti] = '\0';
    sys_gui_text(fd, rx, sy, tbuf, COL_DIM, COL_BG);
}

/* ---- Services tab ---------------------------------------------- */

#define SVC_HDR_Y     (CONTENT_Y + 2)
#define SVC_HDR_H     18
#define SVC_LIST_Y    (SVC_HDR_Y + SVC_HDR_H)
#define SVC_STATUS_H  22
#define SVC_LIST_H    (WIN_H - SVC_LIST_Y - SVC_STATUS_H)
#define SVC_ROWS      (SVC_LIST_H / ROW_H)

static int g_svc_scroll;
static int g_svc_selected;

static void draw_services(int fd) {
    /* column headers */
    sys_gui_fill(fd, 0, SVC_HDR_Y, WIN_W, SVC_HDR_H, COL_HEADER);
    sys_gui_text(fd, 14,  SVC_HDR_Y + 3, "Name",     COL_ACCENT, COL_HEADER);
    sys_gui_text(fd, 200, SVC_HDR_Y + 3, "State",    COL_ACCENT, COL_HEADER);
    sys_gui_text(fd, 340, SVC_HDR_Y + 3, "PID",      COL_ACCENT, COL_HEADER);
    sys_gui_text(fd, 430, SVC_HDR_Y + 3, "Restarts", COL_ACCENT, COL_HEADER);
    sys_gui_text(fd, 550, SVC_HDR_Y + 3, "Crashes",  COL_ACCENT, COL_HEADER);

    int visible = SVC_ROWS;
    if (visible < 0) visible = 0;

    for (int vi = 0; vi < visible; vi++) {
        int idx = g_svc_scroll + vi;
        int ry = SVC_LIST_Y + vi * ROW_H;

        if (idx >= g_svc_count) {
            sys_gui_fill(fd, 0, ry, WIN_W, ROW_H, COL_BG);
            continue;
        }

        int is_sel = (idx == g_svc_selected);
        uint32_t rbg = is_sel ? COL_ROW_SEL
                     : ((vi & 1) ? COL_ROW_ALT : COL_BG);

        sys_gui_fill(fd, 0, ry, WIN_W, ROW_H, rbg);
        if (is_sel)
            sys_gui_fill(fd, 0, ry, 3, ROW_H, COL_ACCENT);

        const struct abi_service_info *svc = &g_svcs[idx];

        /* state indicator dot */
        uint32_t sc = state_color(svc->state);
        sys_gui_fill(fd, 6, ry + 5, 5, 5, sc);

        char namebuf[24];
        my_strncpy(namebuf, svc->name, 22);
        sys_gui_text(fd, 14, ry + 2, namebuf, COL_TEXT, rbg);

        sys_gui_text(fd, 200, ry + 2, state_str(svc->state), sc, rbg);

        char pidbuf[12];
        if (svc->pid >= 0) fmt_uint(pidbuf, (unsigned)svc->pid);
        else { pidbuf[0] = '-'; pidbuf[1] = '\0'; }
        sys_gui_text(fd, 340, ry + 2, pidbuf, COL_TEXT, rbg);

        char rstbuf[12];
        fmt_uint(rstbuf, svc->restart_count);
        sys_gui_text(fd, 430, ry + 2, rstbuf, COL_DIM, rbg);

        char crbuf[12];
        fmt_uint(crbuf, svc->crash_count);
        sys_gui_text(fd, 550, ry + 2, crbuf,
                     svc->crash_count > 0 ? COL_RED : COL_DIM, rbg);
    }

    /* scrollbar */
    if (g_svc_count > visible && visible > 0) {
        int sb_x = WIN_W - 6;
        int sb_h = visible * ROW_H;
        sys_gui_fill(fd, sb_x, SVC_LIST_Y, 4, sb_h, COL_BAR_BG);
        int thumb_h = (visible * sb_h) / g_svc_count;
        if (thumb_h < 8) thumb_h = 8;
        int max_scroll = g_svc_count - visible;
        int thumb_y = SVC_LIST_Y;
        if (max_scroll > 0)
            thumb_y += (g_svc_scroll * (sb_h - thumb_h)) / max_scroll;
        sys_gui_fill(fd, sb_x, thumb_y, 4, thumb_h, COL_ACCENT);
    }

    /* status bar */
    sys_gui_fill(fd, 0, WIN_H - SVC_STATUS_H, WIN_W, SVC_STATUS_H, COL_STATUS_BG);
    sys_gui_fill(fd, 0, WIN_H - SVC_STATUS_H, WIN_W, 1, COL_HEADER);

    int running = 0, stopped = 0, crashed = 0;
    for (int i = 0; i < g_svc_count; i++) {
        if (g_svcs[i].state == 1) running++;
        else if (g_svcs[i].state == 2) crashed++;
        else stopped++;
    }

    char stbuf[80];
    int sp = 0;
    sp = str_append(stbuf, sp, "Services: ");
    sp = str_append_uint(stbuf, sp, (unsigned)g_svc_count);
    sp = str_append(stbuf, sp, "  Running: ");
    sp = str_append_uint(stbuf, sp, (unsigned)running);
    sp = str_append(stbuf, sp, "  Stopped: ");
    sp = str_append_uint(stbuf, sp, (unsigned)stopped);
    sp = str_append(stbuf, sp, "  Crashed: ");
    sp = str_append_uint(stbuf, sp, (unsigned)crashed);
    stbuf[sp] = '\0';
    sys_gui_text(fd, 14, WIN_H - SVC_STATUS_H + 5, stbuf, COL_DIM, COL_STATUS_BG);
}

/* ---- full redraw ----------------------------------------------- */

static void redraw(int fd) {
    sys_gui_fill(fd, 0, 0, WIN_W, WIN_H, COL_BG);
    draw_tab_bar(fd);

    switch (g_tab) {
    case TAB_PROCESSES:   draw_processes(fd);  break;
    case TAB_PERFORMANCE: draw_performance(fd); break;
    case TAB_SERVICES:    draw_services(fd);   break;
    }

    sys_gui_flip(fd);
}

/* ---- scroll helpers -------------------------------------------- */

static int *cur_scroll(void) {
    return (g_tab == TAB_SERVICES) ? &g_svc_scroll : &g_scroll;
}
static int *cur_selected(void) {
    return (g_tab == TAB_SERVICES) ? &g_svc_selected : &g_selected;
}
static int cur_visible(void) {
    return (g_tab == TAB_SERVICES) ? SVC_ROWS : PROC_ROWS;
}

static void clamp_selection(void) {
    int *sel = cur_selected();
    int *scr = cur_scroll();
    if (*sel >= g_svc_count)
        *sel = g_svc_count > 0 ? g_svc_count - 1 : 0;
    if (*sel < 0) *sel = 0;
    if (*scr < 0) *scr = 0;
    int vis = cur_visible();
    if (vis > 0 && *scr > g_svc_count - vis)
        *scr = g_svc_count - vis;
    if (*scr < 0) *scr = 0;
}

static void move_down(void) {
    int *sel = cur_selected();
    int *scr = cur_scroll();
    int vis = cur_visible();
    if (*sel < g_svc_count - 1) {
        (*sel)++;
        if (*sel >= *scr + vis)
            *scr = *sel - vis + 1;
    }
}

static void move_up(void) {
    int *sel = cur_selected();
    int *scr = cur_scroll();
    if (*sel > 0) {
        (*sel)--;
        if (*sel < *scr)
            *scr = *sel;
    }
}

static void do_kill(void) {
    if (g_tab != TAB_PROCESSES) return;
    if (g_selected < 0 || g_selected >= g_svc_count) return;
    const struct abi_service_info *svc = &g_svcs[g_selected];
    if (svc->pid >= 0 && svc->state == 1)
        sys_kill(svc->pid);
}

/* ---- main ------------------------------------------------------ */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fd = sys_gui_create(WIN_W, WIN_H, "Task Manager");
    if (fd < 0) {
        sys_write(1, "taskmgr: gui_create failed\n", 27);
        return 1;
    }

    g_tab = TAB_PROCESSES;
    g_scroll = 0;
    g_selected = 0;
    g_svc_scroll = 0;
    g_svc_selected = 0;
    g_hist_idx = 0;
    g_hist_filled = 0;

    refresh_data();
    redraw(fd);

    long last_refresh = sys_clock_ms();

    for (;;) {
        struct gui_event ev;
        int got = sys_gui_poll_event(fd, &ev);
        if (got > 0) {
            if (ev.type == GUI_EV_CLOSE)
                sys_exit(0);

            if (ev.type == GUI_EV_MOUSE_DOWN) {
                /* tab clicks */
                if (ev.y < TAB_BAR_H) {
                    int clicked_tab = ev.x / TAB_W;
                    if (clicked_tab >= 0 && clicked_tab < TAB_COUNT) {
                        g_tab = clicked_tab;
                        redraw(fd);
                    }
                }
                /* kill button click (Processes tab) */
                else if (g_tab == TAB_PROCESSES &&
                         hit_rect(ev.x, ev.y, KILL_BTN_X, KILL_BTN_Y,
                                  KILL_BTN_W, KILL_BTN_H)) {
                    do_kill();
                    refresh_data();
                    clamp_selection();
                    last_refresh = sys_clock_ms();
                    redraw(fd);
                }
                /* row click in Processes tab */
                else if (g_tab == TAB_PROCESSES &&
                         ev.y >= PROC_LIST_Y &&
                         ev.y < PROC_LIST_Y + PROC_ROWS * ROW_H) {
                    int row = (ev.y - PROC_LIST_Y) / ROW_H;
                    int idx = g_scroll + row;
                    if (idx < g_svc_count)
                        g_selected = idx;
                    redraw(fd);
                }
                /* row click in Services tab */
                else if (g_tab == TAB_SERVICES &&
                         ev.y >= SVC_LIST_Y &&
                         ev.y < SVC_LIST_Y + SVC_ROWS * ROW_H) {
                    int row = (ev.y - SVC_LIST_Y) / ROW_H;
                    int idx = g_svc_scroll + row;
                    if (idx < g_svc_count)
                        g_svc_selected = idx;
                    redraw(fd);
                }
            }

            if (ev.type == GUI_EV_KEY) {
                switch (ev.key) {
                case 'q':
                    sys_exit(0);

                case '1':
                    g_tab = TAB_PROCESSES;
                    redraw(fd);
                    break;
                case '2':
                    g_tab = TAB_PERFORMANCE;
                    redraw(fd);
                    break;
                case '3':
                    g_tab = TAB_SERVICES;
                    redraw(fd);
                    break;

                case '\t':
                    g_tab = (g_tab + 1) % TAB_COUNT;
                    redraw(fd);
                    break;

                case 'j':
                    if (g_tab == TAB_PROCESSES || g_tab == TAB_SERVICES) {
                        move_down();
                        redraw(fd);
                    }
                    break;
                case 'k':
                    if (g_tab == TAB_PROCESSES || g_tab == TAB_SERVICES) {
                        move_up();
                        redraw(fd);
                    }
                    break;

                case 'g':
                    if (g_tab == TAB_PROCESSES || g_tab == TAB_SERVICES) {
                        *cur_selected() = 0;
                        *cur_scroll() = 0;
                        redraw(fd);
                    }
                    break;
                case 'G':
                    if (g_tab == TAB_PROCESSES || g_tab == TAB_SERVICES) {
                        int *sel = cur_selected();
                        int *scr = cur_scroll();
                        *sel = g_svc_count > 0 ? g_svc_count - 1 : 0;
                        int vis = cur_visible();
                        if (*sel >= *scr + vis)
                            *scr = *sel - vis + 1;
                        if (*scr < 0) *scr = 0;
                        redraw(fd);
                    }
                    break;

                case 'x':
                case 127:
                    do_kill();
                    refresh_data();
                    clamp_selection();
                    last_refresh = sys_clock_ms();
                    redraw(fd);
                    break;

                case 'r':
                    refresh_data();
                    clamp_selection();
                    last_refresh = sys_clock_ms();
                    redraw(fd);
                    break;

                default:
                    break;
                }
            }
        }

        long now = sys_clock_ms();
        if (now - last_refresh >= 1000) {
            last_refresh = now;
            refresh_data();
            clamp_selection();
            redraw(fd);
        }

        sys_yield();
    }
}