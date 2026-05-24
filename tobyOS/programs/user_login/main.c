/* user_login/main.c -- Full-screen GUI login screen for tobyOS.
 *
 * Displays a centered login form over a dark gradient background.
 * After successful authentication, fades out with an animation
 * before exiting, revealing the desktop beneath.
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
#define SYS_SETTING_GET    21
#define SYS_LOGIN          23
#define SYS_CLOCK_MS       48
#define SYS_GUI_TEXT_SCALED 56
#define SYS_GUI_SET_STATE  75
#define SYS_GUI_ROUNDED_RECT 82
#define SYS_GUI_LINE       80
#define SYS_GUI_SET_OPACITY 91

struct gui_event {
    int     type;
    int     x;
    int     y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};
#define GUI_EV_CLOSE      1
#define GUI_EV_MOUSE_DOWN 2
#define GUI_EV_KEY        4
#define GUI_EV_RESIZE     5
#define GUI_WIN_MAXIMIZED 2

/* ---- inline syscall helpers -------------------------------------- */

static inline long syscall0(long nr) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r)
        : "0"(nr) : "rcx","r11","memory");
    return r;
}
static inline long syscall1(long nr, long a1) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r)
        : "0"(nr), "D"(a1) : "rcx","r11","memory");
    return r;
}
static inline long syscall2(long nr, long a1, long a2) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r)
        : "0"(nr), "D"(a1), "S"(a2) : "rcx","r11","memory");
    return r;
}
static inline long syscall3(long nr, long a1, long a2, long a3) {
    long r;
    register long r10 __asm__("r10") = 0;
    __asm__ volatile ("syscall" : "=a"(r)
        : "0"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "rcx","r11","memory");
    return r;
}
static inline long syscall5(long nr, long a1, long a2, long a3,
                            long a4, long a5) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile ("syscall" : "=a"(r)
        : "0"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx","r11","memory");
    return r;
}

static void sys_exit(int code) __attribute__((unused));
static void sys_exit(int code) { syscall1(SYS_EXIT, code); for(;;); }
static void sys_yield(void)    { syscall0(SYS_YIELD); }
static long sys_clock_ms(void) { return syscall0(SYS_CLOCK_MS); }

static int sys_gui_create(int w, int h, const char *title) {
    return (int)syscall3(SYS_GUI_CREATE, w, h, (long)title);
}
static void sys_gui_fill(int fd, int x, int y, int w, int h, uint32_t c) {
    uint32_t wh = ((uint32_t)(uint16_t)w) | ((uint32_t)(uint16_t)h << 16);
    syscall5(SYS_GUI_FILL, fd, x, y, wh, c);
}
static void sys_gui_text(int fd, int x, int y, const char *s,
                         uint32_t fg, uint32_t bg) __attribute__((unused));
static void sys_gui_text(int fd, int x, int y, const char *s,
                         uint32_t fg, uint32_t bg) {
    uint32_t xy = ((uint32_t)(uint16_t)x) | ((uint32_t)(uint16_t)y << 16);
    syscall5(SYS_GUI_TEXT, fd, xy, (long)s, fg, bg);
}
static void sys_gui_text_scaled(int fd, int x, int y, const char *s,
                                uint32_t fg, int scale) {
    uint32_t xy = ((uint32_t)(uint16_t)x) | ((uint32_t)(uint16_t)y << 16);
    uint32_t bs = (uint32_t)scale << 24;
    syscall5(SYS_GUI_TEXT_SCALED, fd, xy, (long)s, fg, bs);
}
static void sys_gui_flip(int fd) { syscall1(SYS_GUI_FLIP, fd); }
static int sys_gui_poll_event(int fd, struct gui_event *e) {
    return (int)syscall2(SYS_GUI_POLL_EVENT, fd, (long)e);
}
static void sys_gui_set_state(int fd, int state) {
    syscall2(SYS_GUI_SET_STATE, fd, state);
}
static void sys_gui_set_opacity(int fd, int alpha) {
    syscall2(SYS_GUI_SET_OPACITY, fd, alpha);
}
static long sys_setting_get(const char *key, char *out, size_t cap) {
    return syscall3(SYS_SETTING_GET, (long)key, (long)out, (long)cap);
}
static long sys_login(const char *username) {
    return syscall2(SYS_LOGIN, (long)username, (long)NULL);
}
static void sys_gui_rounded_rect(int fd, int x, int y, int w, int h,
                                 int radius, uint32_t color) __attribute__((unused));
static void sys_gui_rounded_rect(int fd, int x, int y, int w, int h,
                                 int radius, uint32_t color) {
    uint32_t xy = ((uint32_t)(uint16_t)x) | ((uint32_t)(uint16_t)y << 16);
    uint32_t wh = ((uint32_t)(uint16_t)w) | ((uint32_t)(uint16_t)h << 16);
    uint32_t rc = ((uint32_t)radius << 24) | (color & 0x00FFFFFFu);
    syscall5(SYS_GUI_ROUNDED_RECT, fd, (long)xy, (long)wh, (long)rc, 0);
}

static void putstr(const char *s) {
    int len = 0; while (s[len]) len++;
    syscall3(SYS_WRITE, 1, (long)s, len);
}

/* ---- string helpers ---------------------------------------------- */

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++;
    return n;
}
static void my_strcpy(char *d, const char *s) {
    while (*s) *d++ = *s++;
    *d = '\0';
}

/* ---- app state --------------------------------------------------- */

#define WIN_W  1024
#define WIN_H   724

#define FIELD_X     362
#define FIELD_Y     380
#define FIELD_W     300
#define FIELD_H      32

#define BTN_X       412
#define BTN_Y       430
#define BTN_W       200
#define BTN_H        36

#define USERNAME_MAX 32

static char g_username[USERNAME_MAX];
static int  g_cursor;
static int  g_state;      /* 0=idle, 1=error, 2=success, 3=animating */
static char g_status[64];
static int  g_win;
static int  g_actual_w;
static int  g_actual_h;

/* Colors */
#define COL_BG          0x000A0E14u
#define COL_BG_GRAD     0x00141E2Au
#define COL_PANEL       0x00162030u
#define COL_PANEL_BORDER 0x00304050u
#define COL_FIELD_BG    0x000C1218u
#define COL_FIELD_BORDER 0x00446688u
#define COL_ACCENT      0x0066BBDDu
#define COL_ACCENT_DIM  0x00336688u
#define COL_TEXT        0x00E0E8F0u
#define COL_TEXT_DIM    0x00708090u
#define COL_ERROR       0x00EF5350u
#define COL_SUCCESS     0x0066BB6Au
#define COL_CURSOR      0x0066BBDDu
#define COL_BTN         0x00225588u
#define COL_BTN_HOT     0x003377AAu

/* ---- drawing ----------------------------------------------------- */

static void fill_gradient(int fd, int x, int y, int w, int h,
                          uint32_t top, uint32_t bot) {
    for (int row = 0; row < h; row++) {
        uint32_t tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
        uint32_t br = (bot >> 16) & 0xFF, bg_ = (bot >> 8) & 0xFF, bb = bot & 0xFF;
        uint32_t r = tr + (br - tr) * (uint32_t)row / (uint32_t)h;
        uint32_t g = tg + (bg_ - tg) * (uint32_t)row / (uint32_t)h;
        uint32_t b = tb + (bb - tb) * (uint32_t)row / (uint32_t)h;
        sys_gui_fill(fd, x, y + row, w, 1, (r << 16) | (g << 8) | b);
    }
}

static void draw_centered_text(int fd, int y, const char *s,
                               uint32_t fg, int scale) {
    int len = my_strlen(s);
    int tw = len * 8 * scale;
    int x = (g_actual_w - tw) / 2;
    if (x < 0) x = 0;
    sys_gui_text_scaled(fd, x, y, s, fg, scale);
}

static void redraw(int fd) {
    int W = g_actual_w;
    int H = g_actual_h;

    /* Background gradient */
    fill_gradient(fd, 0, 0, W, H / 3, COL_BG, COL_BG_GRAD);
    sys_gui_fill(fd, 0, H / 3, W, H - H / 3, COL_BG_GRAD);

    /* Decorative lines */
    sys_gui_fill(fd, 0, H / 3, W, 1, COL_PANEL_BORDER);

    /* Center panel */
    int pw = 380, ph = 280;
    int px = (W - pw) / 2;
    int py = (H - ph) / 2 - 20;
    sys_gui_fill(fd, px, py, pw, ph, COL_PANEL);
    /* Panel border */
    sys_gui_fill(fd, px, py, pw, 1, COL_PANEL_BORDER);
    sys_gui_fill(fd, px, py + ph - 1, pw, 1, COL_PANEL_BORDER);
    sys_gui_fill(fd, px, py, 1, ph, COL_PANEL_BORDER);
    sys_gui_fill(fd, px + pw - 1, py, 1, ph, COL_PANEL_BORDER);

    /* Accent bar at top of panel */
    sys_gui_fill(fd, px + 1, py + 1, pw - 2, 3, COL_ACCENT);

    /* TobyOS branding */
    draw_centered_text(fd, py + 30, "TobyOS", COL_ACCENT, 3);
    draw_centered_text(fd, py + 64, "Sign in to continue", COL_TEXT_DIM, 1);

    /* Username label */
    int lx = px + 40;
    int ly = py + 100;
    sys_gui_text_scaled(fd, lx, ly, "Username", COL_TEXT_DIM, 1);

    /* Username field */
    int fx = px + 40;
    int fy = ly + 16;
    int fw = pw - 80;
    int fh = 28;
    sys_gui_fill(fd, fx, fy, fw, fh, COL_FIELD_BG);
    sys_gui_fill(fd, fx, fy, fw, 1, COL_FIELD_BORDER);
    sys_gui_fill(fd, fx, fy + fh - 1, fw, 1, COL_FIELD_BORDER);
    sys_gui_fill(fd, fx, fy, 1, fh, COL_FIELD_BORDER);
    sys_gui_fill(fd, fx + fw - 1, fy, 1, fh, COL_FIELD_BORDER);

    /* Username text */
    if (g_username[0]) {
        sys_gui_text_scaled(fd, fx + 8, fy + 6, g_username, COL_TEXT, 2);
    }

    /* Cursor */
    int cx = fx + 8 + g_cursor * 16;
    sys_gui_fill(fd, cx, fy + 4, 2, fh - 8, COL_CURSOR);

    /* Login button */
    int bx = px + 40;
    int by = fy + fh + 20;
    int bw = pw - 80;
    int bh = 32;
    sys_gui_fill(fd, bx, by, bw, bh, COL_BTN);
    sys_gui_fill(fd, bx, by, bw, 1, COL_ACCENT_DIM);
    {
        const char *bt = "Sign In";
        int btlen = my_strlen(bt);
        int btx = bx + (bw - btlen * 16) / 2;
        sys_gui_text_scaled(fd, btx, by + 8, bt, COL_TEXT, 2);
    }

    /* Status message */
    if (g_status[0]) {
        uint32_t sc = COL_TEXT_DIM;
        if (g_state == 1) sc = COL_ERROR;
        if (g_state == 2) sc = COL_SUCCESS;
        draw_centered_text(fd, by + bh + 20, g_status, sc, 1);
    }

    /* Footer */
    draw_centered_text(fd, H - 30, "Type username and press Enter or click Sign In",
                       COL_TEXT_DIM, 1);

    sys_gui_flip(fd);
}

static int hit_button(int mx, int my) {
    int W = g_actual_w;
    int H = g_actual_h;
    int pw = 380, ph = 280;
    int px = (W - pw) / 2;
    int py = (H - ph) / 2 - 20;
    int bx = px + 40;
    int by = py + 100 + 16 + 28 + 20;
    int bw = pw - 80;
    int bh = 32;
    return (mx >= bx && mx < bx + bw && my >= by && my < by + bh);
}

static void do_login(void) {
    if (!g_username[0]) {
        my_strcpy(g_status, "Please enter a username");
        g_state = 1;
        return;
    }
    long rc = sys_login(g_username);
    if (rc != 0) {
        my_strcpy(g_status, "Login failed - invalid username");
        g_state = 1;
        return;
    }
    my_strcpy(g_status, "Welcome! Loading desktop...");
    g_state = 2;
}

/* ---- entry point ------------------------------------------------- */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    g_win = sys_gui_create(WIN_W, WIN_H, "");
    if (g_win < 0) {
        putstr("login: cannot create window\n");
        return 1;
    }

    g_actual_w = WIN_W;
    g_actual_h = WIN_H;

    /* Maximize to fill screen */
    sys_gui_set_state(g_win, GUI_WIN_MAXIMIZED);

    /* Pre-fill username from settings */
    char last[64];
    long n = sys_setting_get("user.last", last, sizeof(last));
    if (n > 0 && last[0]) {
        int i = 0;
        while (last[i] && i < USERNAME_MAX - 1) {
            g_username[i] = last[i]; i++;
        }
        g_username[i] = '\0';
        g_cursor = i;
    } else {
        my_strcpy(g_username, "toby");
        g_cursor = 4;
    }

    my_strcpy(g_status, "");
    g_state = 0;
    redraw(g_win);

    for (;;) {
        struct gui_event ev;
        int got = sys_gui_poll_event(g_win, &ev);

        if (got > 0) {
            if (ev.type == GUI_EV_CLOSE)
                return 0;

            if (ev.type == GUI_EV_RESIZE) {
                g_actual_w = ev.x;
                g_actual_h = ev.y;
                redraw(g_win);
                continue;
            }

            if (g_state == 2) {
                /* Already logged in, ignore further input */
                continue;
            }

            if (ev.type == GUI_EV_MOUSE_DOWN) {
                if (hit_button(ev.x, ev.y)) {
                    do_login();
                    redraw(g_win);
                    if (g_state == 2) break;
                }
            }

            if (ev.type == GUI_EV_KEY) {
                uint8_t k = ev.key;
                if (k == '\n' || k == '\r') {
                    do_login();
                    redraw(g_win);
                    if (g_state == 2) break;
                } else if (k == 0x08 || k == 127) {
                    /* Backspace */
                    if (g_cursor > 0) {
                        g_cursor--;
                        g_username[g_cursor] = '\0';
                        g_state = 0;
                        g_status[0] = '\0';
                        redraw(g_win);
                    }
                } else if (k >= 0x20 && k < 127) {
                    if (g_cursor < USERNAME_MAX - 1) {
                        g_username[g_cursor] = (char)k;
                        g_cursor++;
                        g_username[g_cursor] = '\0';
                        g_state = 0;
                        g_status[0] = '\0';
                        redraw(g_win);
                    }
                }
            }
        } else {
            sys_yield();
        }
    }

    /* Login succeeded -- animate fade-out */
    redraw(g_win);

    /* Smooth fade: 30 steps over ~600ms */
    for (int step = 0; step < 30; step++) {
        int alpha = 255 - (step * 255 / 30);
        if (alpha < 0) alpha = 0;
        sys_gui_set_opacity(g_win, alpha);
        sys_gui_flip(g_win);
        /* ~20ms per frame for 50fps animation */
        long target = sys_clock_ms() + 20;
        while (sys_clock_ms() < target)
            sys_yield();
    }
    sys_gui_set_opacity(g_win, 0);
    sys_gui_flip(g_win);

    return 0;
}
