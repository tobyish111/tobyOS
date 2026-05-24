/* user_gui_clock/main.c -- /bin/gui_clock
 *
 * Three-tab clock application: analog/digital clock, stopwatch, timer.
 * Uses system uptime (SYS_CLOCK_MS) as the time source.
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
typedef unsigned long long uint64_t;
typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;

#define SYS_WRITE           1
#define SYS_YIELD           5
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_CLOCK_MS       48
#define SYS_GUI_LINE       80

#define GUI_EV_MOUSE_DOWN   2
#define GUI_EV_KEY          4
#define GUI_EV_CLOSE        5

struct gui_event {
    int     type;
    int     x;
    int     y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};

/* ---- syscall stubs ------------------------------------------------ */

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
    __asm__ volatile ("syscall"
        : "=a"(_d) : "0"((long)SYS_YIELD)
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
        : "0"((long)SYS_GUI_FILL), "D"((long)fd),
          "S"((long)x), "d"((long)y),
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
        : "0"((long)SYS_GUI_TEXT), "D"((long)fd),
          "S"((long)xy), "d"(s),
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

static inline void sys_gui_line(int fd, int x0, int y0, int x1, int y1,
                                uint32_t color) {
    long r;
    uint32_t p0 = ((uint32_t)(uint16_t)x0) | (((uint32_t)(uint16_t)y0) << 16);
    uint32_t p1 = ((uint32_t)(uint16_t)x1) | (((uint32_t)(uint16_t)y1) << 16);
    register long r10 __asm__("r10") = (long)color;
    register long r8  __asm__("r8")  = 0;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_LINE), "D"((long)fd), "S"((long)p0),
          "d"((long)p1), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
}

/* ---- fixed-point trig for 60 positions around the clock face ------ */

static const int sin60[60] = {
       0,  105,  208,  309,  407,  500,  588,  669,  743,  809,
     866,  914,  951,  978,  995, 1000,  995,  978,  951,  914,
     866,  809,  743,  669,  588,  500,  407,  309,  208,  105,
       0, -105, -208, -309, -407, -500, -588, -669, -743, -809,
    -866, -914, -951, -978, -995,-1000, -995, -978, -951, -914,
    -866, -809, -743, -669, -588, -500, -407, -309, -208, -105
};

static const int cos60[60] = {
     1000,  995,  978,  951,  914,  866,  809,  743,  669,  588,
      500,  407,  309,  208,  105,    0, -105, -208, -309, -407,
     -500, -588, -669, -743, -809, -866, -914, -951, -978, -995,
    -1000, -995, -978, -951, -914, -866, -809, -743, -669, -588,
     -500, -407, -309, -208, -105,    0,  105,  208,  309,  407,
      500,  588,  669,  743,  809,  866,  914,  951,  978,  995
};

/* ---- layout ------------------------------------------------------- */

#define WIN_W        320
#define WIN_H        400

#define TAB_H         30
#define TAB_COUNT      3

#define COL_BG       0x001A1A2Au
#define COL_PANEL    0x00252538u
#define COL_TAB_BG   0x00202035u
#define COL_ACCENT   0x004FC3F7u
#define COL_TEXT     0x00E0E0E0u
#define COL_DIM      0x00808890u
#define COL_WHITE    0x00FFFFFFu
#define COL_GREEN    0x0081C784u
#define COL_RED      0x00FF5252u
#define COL_ORANGE   0x00FFB74Du
#define COL_BTN      0x00303050u
#define COL_FLASH_BG 0x00402020u

#define CLOCK_CX     160
#define CLOCK_CY     175
#define CLOCK_R      100

#define TAB_CLOCK      0
#define TAB_STOPWATCH  1
#define TAB_TIMER      2

#define MAX_LAPS       5

/* ---- helpers ------------------------------------------------------ */

static size_t my_strlen(const char *s) {
    const char *p = s; while (*p) p++;
    return (size_t)(p - s);
}

static void putstr(const char *s) { sys_write(1, s, my_strlen(s)); }

static void fmt_2d(char *out, unsigned v) {
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
}

static void fmt_uint(char *out, unsigned v) {
    char tmp[12]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    else while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int i = 0;
    while (n--) out[i++] = tmp[n];
    out[i] = '\0';
}

static int in_rect(int mx, int my, int rx, int ry, int rw, int rh) {
    return mx >= rx && mx < rx + rw && my >= ry && my < ry + rh;
}

/* ---- drawing primitives ------------------------------------------- */

static void draw_btn(int fd, int x, int y, int w, int h,
                     const char *label, uint32_t bg, uint32_t fg) {
    sys_gui_fill(fd, x, y, w, h, bg);
    sys_gui_fill(fd, x, y, w, 1, COL_ACCENT);
    int tx = x + (w - (int)my_strlen(label) * 8) / 2;
    int ty = y + (h - 16) / 2;
    sys_gui_text(fd, tx, ty, label, fg, bg);
}

static void draw_thick_line(int fd, int x0, int y0, int x1, int y1,
                            uint32_t color, int thickness) {
    for (int dx = -thickness; dx <= thickness; dx++)
        for (int dy = -thickness; dy <= thickness; dy++)
            sys_gui_line(fd, x0 + dx, y0 + dy, x1 + dx, y1 + dy, color);
}

/* ---- tab bar ------------------------------------------------------ */

static void draw_tab_bar(int fd, int active) {
    static const char *names[3] = { "Clock", "Stopwatch", "Timer" };
    static const int widths[3]  = { 107, 106, 107 };
    int x = 0;
    for (int i = 0; i < TAB_COUNT; i++) {
        uint32_t bg = (i == active) ? COL_BG : COL_TAB_BG;
        uint32_t fg = (i == active) ? COL_ACCENT : COL_DIM;
        sys_gui_fill(fd, x, 0, widths[i], TAB_H, bg);
        int tx = x + (widths[i] - (int)my_strlen(names[i]) * 8) / 2;
        sys_gui_text(fd, tx, 7, names[i], fg, bg);
        if (i == active)
            sys_gui_fill(fd, x, TAB_H - 2, widths[i], 2, COL_ACCENT);
        x += widths[i];
    }
}

static int tab_from_click(int mx) {
    if (mx < 107) return 0;
    if (mx < 213) return 1;
    return 2;
}

/* ---- clock tab ---------------------------------------------------- */

static void draw_clock_tab(int fd, long now_ms) {
    unsigned total_s = (unsigned)(now_ms / 1000);
    unsigned hrs  = total_s / 3600;
    unsigned mins = (total_s / 60) % 60;
    unsigned secs = total_s % 60;

    /* Circle outline: 60 line segments */
    for (int i = 0; i < 60; i++) {
        int j = (i + 1) % 60;
        int x0 = CLOCK_CX + sin60[i] * CLOCK_R / 1000;
        int y0 = CLOCK_CY - cos60[i] * CLOCK_R / 1000;
        int x1 = CLOCK_CX + sin60[j] * CLOCK_R / 1000;
        int y1 = CLOCK_CY - cos60[j] * CLOCK_R / 1000;
        sys_gui_line(fd, x0, y0, x1, y1, COL_DIM);
    }

    /* 12 hour tick marks */
    for (int i = 0; i < 12; i++) {
        int idx = i * 5;
        int r_out = CLOCK_R;
        int r_in  = (i % 3 == 0) ? CLOCK_R - 15 : CLOCK_R - 10;
        uint32_t tc = (i % 3 == 0) ? COL_WHITE : COL_TEXT;
        int ox = CLOCK_CX + sin60[idx] * r_out / 1000;
        int oy = CLOCK_CY - cos60[idx] * r_out / 1000;
        int ix = CLOCK_CX + sin60[idx] * r_in  / 1000;
        int iy = CLOCK_CY - cos60[idx] * r_in  / 1000;
        draw_thick_line(fd, ox, oy, ix, iy, tc, 0);
    }

    /* Minute dots */
    for (int i = 0; i < 60; i++) {
        if (i % 5 == 0) continue;
        int px = CLOCK_CX + sin60[i] * (CLOCK_R - 5) / 1000;
        int py = CLOCK_CY - cos60[i] * (CLOCK_R - 5) / 1000;
        sys_gui_fill(fd, px, py, 2, 2, COL_DIM);
    }

    /* Hour hand: thick, short */
    int h_pos = ((hrs % 12) * 5 + mins / 12) % 60;
    int hx = CLOCK_CX + sin60[h_pos] * 55 / 1000;
    int hy = CLOCK_CY - cos60[h_pos] * 55 / 1000;
    draw_thick_line(fd, CLOCK_CX, CLOCK_CY, hx, hy, COL_WHITE, 2);

    /* Minute hand: medium */
    int mmx = CLOCK_CX + sin60[mins] * 78 / 1000;
    int mmy = CLOCK_CY - cos60[mins] * 78 / 1000;
    draw_thick_line(fd, CLOCK_CX, CLOCK_CY, mmx, mmy, COL_TEXT, 1);

    /* Second hand: thin, long */
    int sx = CLOCK_CX + sin60[secs] * 90 / 1000;
    int sy = CLOCK_CY - cos60[secs] * 90 / 1000;
    sys_gui_line(fd, CLOCK_CX, CLOCK_CY, sx, sy, COL_ACCENT);

    /* Center dot */
    sys_gui_fill(fd, CLOCK_CX - 3, CLOCK_CY - 3, 6, 6, COL_ACCENT);

    /* Digital readout panel */
    char ts[9];
    fmt_2d(ts, hrs); ts[2] = ':';
    fmt_2d(ts + 3, mins); ts[5] = ':';
    fmt_2d(ts + 6, secs); ts[8] = '\0';

    int panel_x = (WIN_W - 112) / 2;
    sys_gui_fill(fd, panel_x, 300, 112, 28, COL_PANEL);
    sys_gui_fill(fd, panel_x, 300, 112, 1, COL_ACCENT);
    int text_x = (WIN_W - 8 * 8) / 2;
    sys_gui_text(fd, text_x, 306, ts, COL_ACCENT, COL_PANEL);

    sys_gui_text(fd, (WIN_W - 11 * 8) / 2, 340, "System Time", COL_DIM, COL_BG);

    /* Heartbeat dot */
    uint32_t dot = (secs & 1) ? COL_GREEN : COL_DIM;
    sys_gui_fill(fd, CLOCK_CX - 2, 360, 5, 5, dot);
}

/* ---- stopwatch tab ------------------------------------------------ */

#define SW_BTN_Y     150
#define SW_BTN_H      32
#define SW_BTN1_X     30
#define SW_BTN1_W    120
#define SW_BTN2_X    170
#define SW_BTN2_W    120

#define SW_STOPPED    0
#define SW_RUNNING    1
#define SW_PAUSED     2

static void draw_stopwatch_tab(int fd, int state, long elapsed_ms,
                               long *laps, int lap_count) {
    unsigned ms  = (unsigned)elapsed_ms;
    unsigned tot = ms / 1000;
    unsigned mm  = tot / 60;
    unsigned ss  = tot % 60;
    unsigned t   = (ms / 100) % 10;

    /* Title */
    sys_gui_text(fd, (WIN_W - 9 * 8) / 2, 40, "Stopwatch", COL_ACCENT, COL_BG);

    /* Large time display */
    char buf[9];
    fmt_2d(buf, mm % 100); buf[2] = ':';
    fmt_2d(buf + 3, ss);   buf[5] = '.';
    buf[6] = (char)('0' + t); buf[7] = '\0';

    sys_gui_fill(fd, 30, 62, WIN_W - 60, 44, COL_PANEL);
    sys_gui_fill(fd, 30, 62, WIN_W - 60, 1, COL_ACCENT);
    sys_gui_text(fd, (WIN_W - 7 * 8) / 2, 75, buf, COL_WHITE, COL_PANEL);

    /* Status label */
    const char *status = "Ready";
    uint32_t sc = COL_DIM;
    if (state == SW_RUNNING) { status = "Running"; sc = COL_GREEN; }
    else if (state == SW_PAUSED) { status = "Paused"; sc = COL_ORANGE; }
    sys_gui_text(fd, (WIN_W - (int)my_strlen(status) * 8) / 2, 118, status, sc, COL_BG);

    /* Buttons */
    if (state == SW_STOPPED) {
        draw_btn(fd, SW_BTN1_X, SW_BTN_Y, SW_BTN1_W, SW_BTN_H,
                 "Start", COL_GREEN, COL_BG);
        draw_btn(fd, SW_BTN2_X, SW_BTN_Y, SW_BTN2_W, SW_BTN_H,
                 "Reset", COL_BTN, COL_DIM);
    } else if (state == SW_RUNNING) {
        draw_btn(fd, SW_BTN1_X, SW_BTN_Y, SW_BTN1_W, SW_BTN_H,
                 "Stop", COL_RED, COL_WHITE);
        draw_btn(fd, SW_BTN2_X, SW_BTN_Y, SW_BTN2_W, SW_BTN_H,
                 "Lap", COL_BTN, COL_TEXT);
    } else {
        draw_btn(fd, SW_BTN1_X, SW_BTN_Y, SW_BTN1_W, SW_BTN_H,
                 "Resume", COL_GREEN, COL_BG);
        draw_btn(fd, SW_BTN2_X, SW_BTN_Y, SW_BTN2_W, SW_BTN_H,
                 "Reset", COL_BTN, COL_TEXT);
    }

    /* Lap list */
    int lap_y = 200;
    sys_gui_fill(fd, 30, lap_y - 4, WIN_W - 60, 1, 0x00303048u);
    sys_gui_text(fd, 30, lap_y, "Laps", COL_ACCENT, COL_BG);

    if (lap_count == 0) {
        sys_gui_text(fd, 50, lap_y + 22, "No laps yet", COL_DIM, COL_BG);
    }
    for (int i = 0; i < lap_count && i < MAX_LAPS; i++) {
        unsigned lm = (unsigned)laps[i];
        unsigned ls = lm / 1000;
        char lb[14];
        lb[0] = '#'; lb[1] = (char)('1' + i); lb[2] = ' '; lb[3] = ' ';
        fmt_2d(lb + 4, (ls / 60) % 100);
        lb[6] = ':';
        fmt_2d(lb + 7, ls % 60);
        lb[9] = '.';
        lb[10] = (char)('0' + (lm / 100) % 10);
        lb[11] = '\0';
        sys_gui_text(fd, 50, lap_y + 22 + i * 20, lb, COL_TEXT, COL_BG);
    }
}

/* ---- timer tab ---------------------------------------------------- */

#define TM_PRESET_Y   145
#define TM_PRESET_H    28
#define TM_BTN_Y      190
#define TM_BTN_H       32

#define TM_STOPPED     0
#define TM_RUNNING     1
#define TM_PAUSED      2
#define TM_DONE        3

static void draw_timer_tab(int fd, int state, long remain_ms,
                           long duration_ms, long now_ms) {
    long disp = (remain_ms > 0) ? remain_ms : 0;
    unsigned tot = (unsigned)(disp / 1000);
    unsigned mm  = tot / 60;
    unsigned ss  = tot % 60;

    char buf[6];
    fmt_2d(buf, mm % 100); buf[2] = ':';
    fmt_2d(buf + 3, ss);   buf[5] = '\0';

    /* Flash effect when done */
    int flash = (state == TM_DONE) ? (int)((now_ms / 500) & 1) : 0;
    uint32_t fg       = COL_WHITE;
    uint32_t panel_bg = COL_PANEL;
    if (state == TM_DONE) {
        fg       = flash ? COL_RED : COL_ORANGE;
        panel_bg = flash ? COL_PANEL : COL_FLASH_BG;
    }

    /* Title */
    sys_gui_text(fd, (WIN_W - 5 * 8) / 2, 40, "Timer", COL_ACCENT, COL_BG);

    /* Large countdown display */
    sys_gui_fill(fd, 30, 62, WIN_W - 60, 44, panel_bg);
    sys_gui_fill(fd, 30, 62, WIN_W - 60, 1, COL_ACCENT);
    sys_gui_text(fd, (WIN_W - 5 * 8) / 2, 75, buf, fg, panel_bg);

    /* "TIME'S UP!" label */
    if (state == TM_DONE) {
        uint32_t upc = flash ? COL_ORANGE : COL_RED;
        sys_gui_text(fd, (WIN_W - 10 * 8) / 2, 118, "TIME'S UP!", upc, COL_BG);
    }

    /* Duration display (when settable) */
    if (state == TM_STOPPED) {
        unsigned dur_m = (unsigned)(duration_ms / 60000);
        char db[12];
        db[0] = 'S'; db[1] = 'e'; db[2] = 't'; db[3] = ':'; db[4] = ' ';
        fmt_uint(db + 5, dur_m);
        int p = 5; while (db[p]) p++;
        db[p++] = 'm'; db[p] = '\0';
        sys_gui_text(fd, (WIN_W - p * 8) / 2, 118, db, COL_DIM, COL_BG);
    }

    /* Status when running/paused */
    if (state == TM_RUNNING)
        sys_gui_text(fd, (WIN_W - 7 * 8) / 2, 118, "Running", COL_GREEN, COL_BG);
    else if (state == TM_PAUSED)
        sys_gui_text(fd, (WIN_W - 6 * 8) / 2, 118, "Paused", COL_ORANGE, COL_BG);

    /* Preset buttons (only when stopped/done) */
    if (state == TM_STOPPED || state == TM_DONE) {
        draw_btn(fd,  20, TM_PRESET_Y, 80, TM_PRESET_H, "+1m",  COL_BTN, COL_TEXT);
        draw_btn(fd, 120, TM_PRESET_Y, 80, TM_PRESET_H, "+5m",  COL_BTN, COL_TEXT);
        draw_btn(fd, 220, TM_PRESET_Y, 80, TM_PRESET_H, "+10m", COL_BTN, COL_TEXT);
    }

    /* Control buttons */
    if (state == TM_STOPPED || state == TM_DONE) {
        uint32_t sbg = (duration_ms > 0) ? COL_GREEN : COL_BTN;
        uint32_t sfg = (duration_ms > 0) ? COL_BG    : COL_DIM;
        draw_btn(fd,  30, TM_BTN_Y, 120, TM_BTN_H, "Start", sbg, sfg);
        draw_btn(fd, 170, TM_BTN_Y, 120, TM_BTN_H, "Reset", COL_BTN, COL_DIM);
    } else if (state == TM_RUNNING) {
        draw_btn(fd,  30, TM_BTN_Y, 120, TM_BTN_H, "Stop",  COL_RED, COL_WHITE);
        draw_btn(fd, 170, TM_BTN_Y, 120, TM_BTN_H, "Reset", COL_BTN, COL_TEXT);
    } else {
        draw_btn(fd,  30, TM_BTN_Y, 120, TM_BTN_H, "Resume", COL_GREEN, COL_BG);
        draw_btn(fd, 170, TM_BTN_Y, 120, TM_BTN_H, "Reset",  COL_BTN, COL_TEXT);
    }

    /* Progress bar */
    if (duration_ms > 0 && state != TM_STOPPED) {
        int bar_y = 240;
        int bar_w = WIN_W - 60;
        sys_gui_fill(fd, 30, bar_y, bar_w, 8, COL_PANEL);
        long fill = disp * bar_w / duration_ms;
        if (fill > bar_w) fill = bar_w;
        if (fill > 0) {
            uint32_t bc = (state == TM_DONE) ? COL_RED : COL_ACCENT;
            sys_gui_fill(fd, 30, bar_y, (int)fill, 8, bc);
        }
    }
}

/* ---- main --------------------------------------------------------- */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fd = sys_gui_create(WIN_W, WIN_H, "Clock");
    if (fd < 0) {
        putstr("gui_clock: cannot create window\n");
        return 1;
    }

    int  active_tab   = TAB_CLOCK;
    long last_draw    = 0;

    /* Stopwatch state */
    int  sw_state     = SW_STOPPED;
    long sw_start_ms  = 0;
    long sw_accum_ms  = 0;
    long sw_laps[MAX_LAPS];
    int  sw_lap_count = 0;
    for (int i = 0; i < MAX_LAPS; i++) sw_laps[i] = 0;

    /* Timer state */
    int  tm_state     = TM_STOPPED;
    long tm_duration  = 0;
    long tm_remain    = 0;
    long tm_start_ms  = 0;
    long tm_start_val = 0;

    for (;;) {
        struct gui_event ev;
        int got = sys_gui_poll_event(fd, &ev);

        if (got > 0 && ev.type == GUI_EV_CLOSE) return 0;
        if (got > 0 && ev.type == GUI_EV_KEY && ev.key == 'q') return 0;

        if (got > 0 && ev.type == GUI_EV_MOUSE_DOWN) {
            int mx = ev.x, my = ev.y;

            /* Tab bar */
            if (my < TAB_H) {
                int t = tab_from_click(mx);
                if (t >= 0 && t < TAB_COUNT) active_tab = t;
            }

            /* Stopwatch buttons */
            if (active_tab == TAB_STOPWATCH) {
                if (in_rect(mx, my, SW_BTN1_X, SW_BTN_Y, SW_BTN1_W, SW_BTN_H)) {
                    if (sw_state == SW_STOPPED || sw_state == SW_PAUSED) {
                        sw_start_ms = sys_clock_ms();
                        sw_state = SW_RUNNING;
                    } else if (sw_state == SW_RUNNING) {
                        sw_accum_ms += sys_clock_ms() - sw_start_ms;
                        sw_state = SW_PAUSED;
                    }
                }
                if (in_rect(mx, my, SW_BTN2_X, SW_BTN_Y, SW_BTN2_W, SW_BTN_H)) {
                    if (sw_state == SW_RUNNING && sw_lap_count < MAX_LAPS) {
                        sw_laps[sw_lap_count++] =
                            sw_accum_ms + (sys_clock_ms() - sw_start_ms);
                    } else if (sw_state != SW_RUNNING) {
                        sw_state     = SW_STOPPED;
                        sw_accum_ms  = 0;
                        sw_start_ms  = 0;
                        sw_lap_count = 0;
                        for (int i = 0; i < MAX_LAPS; i++) sw_laps[i] = 0;
                    }
                }
            }

            /* Timer buttons */
            if (active_tab == TAB_TIMER) {
                /* Preset buttons */
                if (tm_state == TM_STOPPED || tm_state == TM_DONE) {
                    if (in_rect(mx, my, 20, TM_PRESET_Y, 80, TM_PRESET_H)) {
                        tm_duration += 60000;
                        tm_remain = tm_duration;
                    }
                    if (in_rect(mx, my, 120, TM_PRESET_Y, 80, TM_PRESET_H)) {
                        tm_duration += 300000;
                        tm_remain = tm_duration;
                    }
                    if (in_rect(mx, my, 220, TM_PRESET_Y, 80, TM_PRESET_H)) {
                        tm_duration += 600000;
                        tm_remain = tm_duration;
                    }
                }

                /* Start / Stop / Resume */
                if (in_rect(mx, my, 30, TM_BTN_Y, 120, TM_BTN_H)) {
                    if ((tm_state == TM_STOPPED || tm_state == TM_DONE) &&
                        tm_duration > 0) {
                        if (tm_state == TM_DONE) tm_remain = tm_duration;
                        tm_start_ms  = sys_clock_ms();
                        tm_start_val = tm_remain;
                        tm_state     = TM_RUNNING;
                    } else if (tm_state == TM_PAUSED) {
                        tm_start_ms  = sys_clock_ms();
                        tm_start_val = tm_remain;
                        tm_state     = TM_RUNNING;
                    } else if (tm_state == TM_RUNNING) {
                        tm_remain = tm_start_val - (sys_clock_ms() - tm_start_ms);
                        if (tm_remain < 0) tm_remain = 0;
                        tm_state = TM_PAUSED;
                    }
                }

                /* Reset */
                if (in_rect(mx, my, 170, TM_BTN_Y, 120, TM_BTN_H)) {
                    tm_state    = TM_STOPPED;
                    tm_duration = 0;
                    tm_remain   = 0;
                }
            }

            last_draw = 0;
        }

        long now = sys_clock_ms();

        /* Update running timer */
        if (tm_state == TM_RUNNING) {
            tm_remain = tm_start_val - (now - tm_start_ms);
            if (tm_remain <= 0) {
                tm_remain = 0;
                tm_state  = TM_DONE;
            }
        }

        /* Redraw throttle: 100ms for stopwatch/timer, 500ms for clock */
        int interval = (active_tab == TAB_CLOCK) ? 500 : 100;
        if (tm_state == TM_DONE) interval = 250;
        if (now - last_draw < interval) {
            sys_yield();
            continue;
        }
        last_draw = now;

        /* ---- full redraw ---- */
        sys_gui_fill(fd, 0, 0, WIN_W, WIN_H, COL_BG);
        draw_tab_bar(fd, active_tab);

        if (active_tab == TAB_CLOCK) {
            draw_clock_tab(fd, now);
        } else if (active_tab == TAB_STOPWATCH) {
            long sw_el = sw_accum_ms;
            if (sw_state == SW_RUNNING)
                sw_el += now - sw_start_ms;
            draw_stopwatch_tab(fd, sw_state, sw_el, sw_laps, sw_lap_count);
        } else {
            draw_timer_tab(fd, tm_state, tm_remain, tm_duration, now);
        }

        sys_gui_flip(fd);
        sys_yield();
    }
}
