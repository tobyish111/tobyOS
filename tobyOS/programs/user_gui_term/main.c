/* user_gui_term/main.c -- /bin/gui_term, the GUI terminal emulator.
 *
 * 80x30 character grid with 8x16-pixel cells (640x480 window).
 * 200-line scrollback ring buffer, ANSI SGR color parsing (ESC[...m),
 * blinking block cursor, and PageUp/PageDown scrollback navigation.
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;

#define SYS_EXIT            0
#define SYS_WRITE           1
#define SYS_YIELD           5
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_TERM_OPEN      15
#define SYS_TERM_WRITE     16
#define SYS_TERM_READ      17
#define SYS_CLOCK_MS       48

#define GUI_EV_MOUSE_DOWN   2
#define GUI_EV_KEY          4
#define GUI_EV_CLOSE        5

#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_PGUP  0x86
#define KEY_PGDN  0x87

struct gui_event {
    int     type;
    int     x;
    int     y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};

/* ---- Grid / window geometry ---------------------------------------- */

#define COLS        80
#define ROWS        30
#define SCROLLBACK  200
#define CELL_W      8
#define CELL_H      16
#define WIN_W       (COLS * CELL_W)
#define WIN_H       (ROWS * CELL_H)

/* ---- Cell type ----------------------------------------------------- */

typedef struct {
    uint8_t ch;
    uint8_t fg;
    uint8_t bg;
} cell_t;

/* ---- Color palette (8 standard + 8 bright) ------------------------- */

static const uint32_t g_palette[16] = {
    0x00000000,   /*  0  black           */
    0x00CC0000,   /*  1  red             */
    0x0000CC00,   /*  2  green           */
    0x00CCCC00,   /*  3  yellow          */
    0x000000CC,   /*  4  blue            */
    0x00CC00CC,   /*  5  magenta         */
    0x0000CCCC,   /*  6  cyan            */
    0x00CCCCCC,   /*  7  light gray      */
    0x00333333,   /*  8  dark gray       */
    0x00FF3333,   /*  9  bright red      */
    0x0033FF33,   /* 10  bright green    */
    0x00FFFF33,   /* 11  bright yellow   */
    0x003333FF,   /* 12  bright blue     */
    0x00FF33FF,   /* 13  bright magenta  */
    0x0033FFFF,   /* 14  bright cyan     */
    0x00FFFFFF,   /* 15  bright white    */
};

#define COL_CURSOR 0x00FFD060u

/* ---- Terminal state ------------------------------------------------ */

static cell_t  g_ring[SCROLLBACK][COLS];
static int     g_ring_start;
static int     g_ring_count;
static int     g_scroll_offset;

static int     g_cx, g_cy;
static int     g_need_redraw;
static uint8_t g_cur_fg;
static uint8_t g_cur_bg;
static int     g_bold;

static uint32_t g_last_blink;
static int      g_cursor_visible;

/* ANSI escape parser */
#define STATE_NORMAL 0
#define STATE_ESC    1
#define STATE_CSI    2
#define MAX_CSI_PARAMS 8

static int g_esc_state;
static int g_csi_params[MAX_CSI_PARAMS];
static int g_csi_nparams;
static int g_csi_priv;

/* ---- Syscall stubs ------------------------------------------------- */

static inline ssize_t sys_write(int fd, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}

static inline void sys_exit(int code) {
    __asm__ volatile ("syscall"
        : : "a"((long)SYS_EXIT), "D"((long)code)
        : "rcx", "r11", "memory");
    __builtin_unreachable();
}

static inline void sys_yield(void) {
    long _dummy;
    __asm__ volatile ("syscall"
        : "=a"(_dummy) : "0"((long)SYS_YIELD)
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

static inline int sys_term_open(void) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_TERM_OPEN)
        : "rcx", "r11", "memory");
    return (int)r;
}

static inline long sys_term_write(int fd, const void *buf, size_t len) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_TERM_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}

static inline long sys_term_read(int fd, void *buf, size_t cap) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_TERM_READ), "D"((long)fd), "S"(buf), "d"(cap)
        : "rcx", "r11", "memory");
    return r;
}

static inline uint32_t sys_clock_ms(void) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_CLOCK_MS)
        : "rcx", "r11", "memory");
    return (uint32_t)r;
}

/* ---- Helpers ------------------------------------------------------- */

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static void putstr_console(const char *s) {
    sys_write(1, s, my_strlen(s));
}

/* ---- Ring buffer access -------------------------------------------- */

static cell_t *ring_line(int offset) {
    return g_ring[(g_ring_start + offset) % SCROLLBACK];
}

static cell_t *screen_line(int row) {
    return ring_line(g_ring_count - ROWS + row);
}

static cell_t *view_line(int row) {
    return ring_line(g_ring_count - ROWS - g_scroll_offset + row);
}

static uint8_t effective_fg(void) {
    uint8_t fg = g_cur_fg;
    if (g_bold && fg < 8) fg += 8;
    return fg;
}

/* ---- Grid operations ----------------------------------------------- */

static void clear_line_default(cell_t *line) {
    for (int x = 0; x < COLS; x++) {
        line[x].ch = ' ';
        line[x].fg = 7;
        line[x].bg = 0;
    }
}

static void init_ring(void) {
    g_ring_start = 0;
    g_ring_count = ROWS;
    for (int i = 0; i < SCROLLBACK; i++)
        clear_line_default(g_ring[i]);
    g_cx = 0;
    g_cy = 0;
    g_scroll_offset = 0;
    g_need_redraw = 1;
    g_cur_fg = 7;
    g_cur_bg = 0;
    g_bold = 0;
    g_esc_state = STATE_NORMAL;
    g_cursor_visible = 1;
}

static void grid_clear(void) {
    for (int r = 0; r < ROWS; r++)
        clear_line_default(screen_line(r));
    g_cx = 0;
    g_cy = 0;
    g_need_redraw = 1;
}

static void scroll_up(void) {
    if (g_ring_count < SCROLLBACK) {
        g_ring_count++;
    } else {
        g_ring_start = (g_ring_start + 1) % SCROLLBACK;
    }
    clear_line_default(screen_line(ROWS - 1));
}

static void grid_newline(void) {
    g_cx = 0;
    if (g_cy + 1 >= ROWS)
        scroll_up();
    else
        g_cy++;
}

/* ---- CSI command dispatch ------------------------------------------ */

static void csi_dispatch(char cmd) {
    int p0 = g_csi_nparams > 0 ? g_csi_params[0] : 0;
    int p1 = g_csi_nparams > 1 ? g_csi_params[1] : 0;

    if (g_csi_priv)
        return;

    switch (cmd) {
    case 'm': {
        if (g_csi_nparams == 0 || (g_csi_nparams == 1 && p0 == 0)) {
            g_cur_fg = 7; g_cur_bg = 0; g_bold = 0;
            break;
        }
        for (int i = 0; i < g_csi_nparams; i++) {
            int p = g_csi_params[i];
            if      (p == 0)              { g_cur_fg = 7; g_cur_bg = 0; g_bold = 0; }
            else if (p == 1)              { g_bold = 1; }
            else if (p == 22)             { g_bold = 0; }
            else if (p >= 30 && p <= 37)  { g_cur_fg = (uint8_t)(p - 30); }
            else if (p == 39)             { g_cur_fg = 7; }
            else if (p >= 40 && p <= 47)  { g_cur_bg = (uint8_t)(p - 40); }
            else if (p == 49)             { g_cur_bg = 0; }
        }
        break;
    }

    case 'A': {
        int n = p0 > 0 ? p0 : 1;
        g_cy -= n;
        if (g_cy < 0) g_cy = 0;
        break;
    }
    case 'B': {
        int n = p0 > 0 ? p0 : 1;
        g_cy += n;
        if (g_cy >= ROWS) g_cy = ROWS - 1;
        break;
    }
    case 'C': {
        int n = p0 > 0 ? p0 : 1;
        g_cx += n;
        if (g_cx >= COLS) g_cx = COLS - 1;
        break;
    }
    case 'D': {
        int n = p0 > 0 ? p0 : 1;
        g_cx -= n;
        if (g_cx < 0) g_cx = 0;
        break;
    }
    case 'G':
        g_cx = (p0 > 0 ? p0 - 1 : 0);
        if (g_cx >= COLS) g_cx = COLS - 1;
        break;
    case 'd':
        g_cy = (p0 > 0 ? p0 - 1 : 0);
        if (g_cy >= ROWS) g_cy = ROWS - 1;
        break;
    case 'H': case 'f':
        g_cy = (p0 > 0 ? p0 - 1 : 0);
        g_cx = (p1 > 0 ? p1 - 1 : 0);
        if (g_cy >= ROWS) g_cy = ROWS - 1;
        if (g_cx >= COLS) g_cx = COLS - 1;
        break;

    case 'J': {
        cell_t *line;
        if (p0 == 0) {
            line = screen_line(g_cy);
            for (int x = g_cx; x < COLS; x++) {
                line[x].ch = ' '; line[x].fg = g_cur_fg; line[x].bg = g_cur_bg;
            }
            for (int r = g_cy + 1; r < ROWS; r++) {
                line = screen_line(r);
                for (int x = 0; x < COLS; x++) {
                    line[x].ch = ' '; line[x].fg = g_cur_fg; line[x].bg = g_cur_bg;
                }
            }
        } else if (p0 == 1) {
            for (int r = 0; r < g_cy; r++) {
                line = screen_line(r);
                for (int x = 0; x < COLS; x++) {
                    line[x].ch = ' '; line[x].fg = g_cur_fg; line[x].bg = g_cur_bg;
                }
            }
            line = screen_line(g_cy);
            for (int x = 0; x <= g_cx && x < COLS; x++) {
                line[x].ch = ' '; line[x].fg = g_cur_fg; line[x].bg = g_cur_bg;
            }
        } else if (p0 == 2) {
            grid_clear();
        }
        break;
    }

    case 'K': {
        cell_t *line = screen_line(g_cy);
        int start, end;
        if      (p0 == 1) { start = 0;    end = g_cx + 1; }
        else if (p0 == 2) { start = 0;    end = COLS; }
        else              { start = g_cx; end = COLS; }
        if (end > COLS) end = COLS;
        for (int x = start; x < end; x++) {
            line[x].ch = ' ';
            line[x].fg = g_cur_fg;
            line[x].bg = g_cur_bg;
        }
        break;
    }

    case '@': {
        int n = p0 > 0 ? p0 : 1;
        cell_t *line = screen_line(g_cy);
        for (int x = COLS - 1; x >= g_cx + n; x--)
            line[x] = line[x - n];
        for (int x = g_cx; x < g_cx + n && x < COLS; x++) {
            line[x].ch = ' '; line[x].fg = g_cur_fg; line[x].bg = g_cur_bg;
        }
        break;
    }
    case 'P': {
        int n = p0 > 0 ? p0 : 1;
        cell_t *line = screen_line(g_cy);
        for (int x = g_cx; x + n < COLS; x++)
            line[x] = line[x + n];
        int fill_start = COLS - n;
        if (fill_start < g_cx) fill_start = g_cx;
        for (int x = fill_start; x < COLS; x++) {
            line[x].ch = ' '; line[x].fg = g_cur_fg; line[x].bg = g_cur_bg;
        }
        break;
    }

    case 'L': {
        int n = p0 > 0 ? p0 : 1;
        for (int y = ROWS - 1; y >= g_cy + n; y--) {
            cell_t *dst = screen_line(y);
            cell_t *src = screen_line(y - n);
            for (int x = 0; x < COLS; x++) dst[x] = src[x];
        }
        for (int y = g_cy; y < g_cy + n && y < ROWS; y++)
            clear_line_default(screen_line(y));
        break;
    }
    case 'M': {
        int n = p0 > 0 ? p0 : 1;
        for (int y = g_cy; y + n < ROWS; y++) {
            cell_t *dst = screen_line(y);
            cell_t *src = screen_line(y + n);
            for (int x = 0; x < COLS; x++) dst[x] = src[x];
        }
        int fill_start = ROWS - n;
        if (fill_start < g_cy) fill_start = g_cy;
        for (int y = fill_start; y < ROWS; y++)
            clear_line_default(screen_line(y));
        break;
    }
    }
}

/* ---- VT processor with ANSI state machine -------------------------- */

static void vt_putc(uint8_t c) {
    switch (g_esc_state) {
    case STATE_NORMAL:
        if (c == '\033') {
            g_esc_state = STATE_ESC;
        } else if (c == '\r') {
            g_cx = 0;
        } else if (c == '\n') {
            grid_newline();
        } else if (c == '\b') {
            if (g_cx > 0) g_cx--;
        } else if (c == '\t') {
            g_cx = (g_cx + 8) & ~7;
            if (g_cx >= COLS) g_cx = COLS - 1;
        } else if (c == 0x0C) {
            grid_clear();
        } else if (c >= 0x20 && c < 0x7F) {
            if (g_cx >= COLS) grid_newline();
            cell_t *line = screen_line(g_cy);
            line[g_cx].ch = c;
            line[g_cx].fg = effective_fg();
            line[g_cx].bg = g_cur_bg;
            g_cx++;
        }
        break;

    case STATE_ESC:
        if (c == '[') {
            g_esc_state = STATE_CSI;
            g_csi_nparams = 0;
            g_csi_priv = 0;
            for (int i = 0; i < MAX_CSI_PARAMS; i++)
                g_csi_params[i] = 0;
        } else {
            g_esc_state = STATE_NORMAL;
        }
        break;

    case STATE_CSI:
        if (c == '?') {
            g_csi_priv = 1;
        } else if (c >= '0' && c <= '9') {
            if (g_csi_nparams == 0) g_csi_nparams = 1;
            int idx = g_csi_nparams - 1;
            if (idx < MAX_CSI_PARAMS)
                g_csi_params[idx] = g_csi_params[idx] * 10 + (c - '0');
        } else if (c == ';') {
            if (g_csi_nparams == 0) g_csi_nparams = 1;
            if (g_csi_nparams < MAX_CSI_PARAMS) {
                g_csi_params[g_csi_nparams] = 0;
                g_csi_nparams++;
            }
        } else if (c >= 0x40 && c <= 0x7E) {
            if (g_csi_nparams == 0) g_csi_nparams = 1;
            csi_dispatch((char)c);
            g_esc_state = STATE_NORMAL;
        } else {
            g_esc_state = STATE_NORMAL;
        }
        break;
    }
    g_need_redraw = 1;
}

/* ---- Redraw -------------------------------------------------------- */

static void redraw(int win) {
    /* Only clear and repaint rows that actually changed since last draw.
     * Falls back to full repaint when scroll offset changes or on first draw. */
    static int s_last_scroll = -1;
    int full = (s_last_scroll != g_scroll_offset);
    s_last_scroll = g_scroll_offset;

    if (full)
        sys_gui_fill(win, 0, 0, WIN_W, WIN_H, g_palette[0]);

    char run[COLS + 1];

    for (int y = 0; y < ROWS; y++) {
        cell_t *line = view_line(y);
        if (!full) {
            sys_gui_fill(win, 0, y * CELL_H, WIN_W, CELL_H, g_palette[0]);
        }
        int x = 0;
        while (x < COLS) {
            uint8_t fg = line[x].fg;
            uint8_t bg = line[x].bg;
            int start = x;
            while (x < COLS && line[x].fg == fg && line[x].bg == bg) {
                uint8_t ch = line[x].ch;
                run[x - start] = ch ? (char)ch : ' ';
                x++;
            }
            run[x - start] = '\0';
            sys_gui_text(win,
                         start * CELL_W, y * CELL_H,
                         run,
                         g_palette[fg < 16 ? fg : 7],
                         g_palette[bg < 16 ? bg : 0]);
        }
    }

    if (g_cursor_visible && g_scroll_offset == 0 && g_cx < COLS) {
        int px = g_cx * CELL_W;
        int py = g_cy * CELL_H;
        sys_gui_fill(win, px, py, CELL_W, CELL_H, COL_CURSOR);
        cell_t *cl = screen_line(g_cy);
        char cbuf[2];
        cbuf[0] = cl[g_cx].ch ? (char)cl[g_cx].ch : ' ';
        cbuf[1] = '\0';
        sys_gui_text(win, px, py, cbuf, g_palette[0], COL_CURSOR);
    }

    sys_gui_flip(win);
    g_need_redraw = 0;
}

/* ---- Main ---------------------------------------------------------- */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int win = sys_gui_create(WIN_W, WIN_H, "Terminal");
    if (win < 0) {
        putstr_console("gui_term: sys_gui_create failed\n");
        return 1;
    }
    int sess = sys_term_open();
    if (sess < 0) {
        putstr_console("gui_term: sys_term_open failed\n");
        return 1;
    }

    init_ring();
    g_last_blink = sys_clock_ms();
    redraw(win);

    char outbuf[256];

    for (;;) {
        struct gui_event ev;
        int got = 1;
        while (got > 0) {
            got = sys_gui_poll_event(win, &ev);
            if (got <= 0) break;
            if (ev.type == GUI_EV_CLOSE)
                sys_exit(0);
            if (ev.type == GUI_EV_KEY && ev.key != 0) {
                if (ev.key == KEY_PGUP) {
                    g_scroll_offset += ROWS / 2;
                    int max_off = g_ring_count - ROWS;
                    if (max_off < 0) max_off = 0;
                    if (g_scroll_offset > max_off)
                        g_scroll_offset = max_off;
                    g_need_redraw = 1;
                } else if (ev.key == KEY_PGDN) {
                    g_scroll_offset -= ROWS / 2;
                    if (g_scroll_offset < 0)
                        g_scroll_offset = 0;
                    g_need_redraw = 1;
                } else {
                    char kb = (char)ev.key;
                    sys_term_write(sess, &kb, 1);
                }
            }
        }

        long n = sys_term_read(sess, outbuf, sizeof(outbuf));
        if (n > 0 && g_scroll_offset > 0)
            g_scroll_offset = 0;
        while (n > 0) {
            for (long i = 0; i < n; i++)
                vt_putc((uint8_t)outbuf[i]);
            n = sys_term_read(sess, outbuf, sizeof(outbuf));
        }

        uint32_t now = sys_clock_ms();
        if (now - g_last_blink >= 500) {
            g_cursor_visible = !g_cursor_visible;
            g_last_blink = now;
            g_need_redraw = 1;
        }

        if (g_need_redraw) redraw(win);
        sys_yield();
    }
}
