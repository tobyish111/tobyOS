/* user_gui_viewer/main.c -- /bin/gui_viewer
 *
 * File viewer with text and hex view modes, search, line numbers,
 * toolbar, and status bar.
 *
 * Controls:
 *    j / k        -> line up / down
 *    u / d        -> page up / down
 *    g / G        -> top / bottom
 *    f / /        -> open search bar
 *    n            -> find next
 *    Esc          -> dismiss search bar
 *    q            -> quit
 *    Mouse click  -> toolbar buttons
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
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
#define SYS_FS_READFILE    19

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
    __asm__ volatile ("syscall"
        : "=a"(_dummy) : "0"((long)SYS_YIELD)
        : "rcx", "r11", "memory");
}
__attribute__((noreturn))
static inline void sys_exit(int code) {
    __asm__ volatile ("syscall"
        : : "a"((long)SYS_EXIT), "D"((long)code)
        : "rcx", "r11", "memory");
    for (;;) { }
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
static inline long sys_fs_readfile(const char *path, void *out, size_t cap) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_FS_READFILE), "D"(path), "S"(out), "d"(cap)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- utility -------------------------------------------------- */

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static void putstr_console(const char *s) {
    sys_write(1, s, my_strlen(s));
}

static int my_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

static void byte_to_hex(char *out, uint8_t b) {
    const char *hex = "0123456789ABCDEF";
    out[0] = hex[b >> 4];
    out[1] = hex[b & 0xF];
}

static void uint_to_dec(char *buf, int val) {
    if (val < 0) val = 0;
    char tmp[12];
    int i = 0;
    if (val == 0) { tmp[i++] = '0'; }
    else {
        while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

static void offset_to_hex8(char *out, long off) {
    for (int i = 7; i >= 0; i--) {
        const char *hex = "0123456789ABCDEF";
        out[i] = hex[off & 0xF];
        off >>= 4;
    }
    out[8] = '\0';
}

/* ---- layout ---------------------------------------------------- */

#define WIN_W        720
#define WIN_H        500
#define TOOLBAR_H    30
#define STATUS_H     22
#define GUTTER_W     40
#define PAD          6
#define CELL_W       8
#define CELL_H       14

#define CONTENT_Y    (TOOLBAR_H)
#define CONTENT_H    (WIN_H - TOOLBAR_H - STATUS_H)
#define ROWS         (CONTENT_H / CELL_H)

#define TEXT_COLS    ((WIN_W - GUTTER_W - PAD) / CELL_W)
#define HEX_COLS     76

#define COL_BG       0x001E1E2Eu
#define COL_TOOLBAR  0x002D2D3Du
#define COL_GUTTER   0x00252536u
#define COL_TEXT     0x00E0E0E0u
#define COL_DIM      0x00808890u
#define COL_ACCENT   0x004FC3F7u
#define COL_FOUND    0x00264F78u
#define COL_STATUS   0x002D2D3Du

/* ---- state ----------------------------------------------------- */

#define VIEWER_CAP   (32 * 1024)
static char g_buf[VIEWER_CAP + 1];
static long g_size = 0;

#define LINES_MAX    4096
static long g_line_off[LINES_MAX];
static int  g_line_count = 0;
static int  g_top_line   = 0;

#define MODE_TEXT 0
#define MODE_HEX  1
static int g_mode = MODE_TEXT;

#define SEARCH_MAX 64
static char g_search[SEARCH_MAX];
static int  g_search_len = 0;
static int  g_search_active = 0;
static int  g_found_line = -1;

static int g_hex_total_lines = 0;

static void index_lines(void) {
    g_line_count = 0;
    g_line_off[g_line_count++] = 0;
    for (long i = 0; i < g_size && g_line_count < LINES_MAX; i++) {
        if (g_buf[i] == '\n' && i + 1 < g_size) {
            g_line_off[g_line_count++] = i + 1;
        }
    }
    g_hex_total_lines = (int)((g_size + 15) / 16);
}

static int max_top(void) {
    int total = (g_mode == MODE_HEX) ? g_hex_total_lines : g_line_count;
    int mt = total - ROWS;
    return (mt < 0) ? 0 : mt;
}

static void scroll_up(int n) {
    g_top_line -= n;
    if (g_top_line < 0) g_top_line = 0;
}

static void scroll_down(int n) {
    g_top_line += n;
    int mt = max_top();
    if (g_top_line > mt) g_top_line = mt;
}

/* ---- search ---------------------------------------------------- */

static int search_forward(int start_line) {
    if (g_search_len == 0) return -1;
    for (int li = start_line; li < g_line_count; li++) {
        long s = g_line_off[li];
        long e = (li + 1 < g_line_count) ? g_line_off[li + 1] : g_size;
        long line_len = e - s;
        for (long k = 0; k + g_search_len <= line_len; k++) {
            if (my_memcmp(&g_buf[s + k], g_search, (size_t)g_search_len) == 0)
                return li;
        }
    }
    return -1;
}

/* ---- drawing --------------------------------------------------- */

static void draw_toolbar(int fd, const char *path) {
    sys_gui_fill(fd, 0, 0, WIN_W, TOOLBAR_H, COL_TOOLBAR);

    uint32_t text_fg = (g_mode == MODE_TEXT) ? COL_ACCENT : COL_DIM;
    uint32_t hex_fg  = (g_mode == MODE_HEX)  ? COL_ACCENT : COL_DIM;

    sys_gui_text(fd, 8, 9, "Text", text_fg, COL_TOOLBAR);
    sys_gui_text(fd, 44, 9, "|", COL_DIM, COL_TOOLBAR);
    sys_gui_text(fd, 56, 9, "Hex", hex_fg, COL_TOOLBAR);
    sys_gui_text(fd, 96, 9, "Search", COL_TEXT, COL_TOOLBAR);

    int plen = (int)my_strlen(path);
    int max_path = (WIN_W - 160) / CELL_W;
    const char *display = path;
    if (plen > max_path) display = path + (plen - max_path);
    sys_gui_text(fd, WIN_W - (int)my_strlen(display) * CELL_W - 8, 9,
                 display, COL_DIM, COL_TOOLBAR);
}

static void draw_status(int fd, const char *path) {
    int sy = WIN_H - STATUS_H;
    sys_gui_fill(fd, 0, sy, WIN_W, STATUS_H, COL_STATUS);

    char status[128];
    int pos = 0;

    const char *fname = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') fname = p + 1;
    }
    for (const char *p = fname; *p && pos < 40; ) status[pos++] = *p++;
    status[pos++] = ' '; status[pos++] = ' ';

    if (g_mode == MODE_TEXT) {
        char num[12];
        uint_to_dec(num, g_top_line + 1);
        for (int i = 0; num[i]; i++) status[pos++] = num[i];
        status[pos++] = '/';
        uint_to_dec(num, g_line_count);
        for (int i = 0; num[i]; i++) status[pos++] = num[i];
    } else {
        char num[12];
        uint_to_dec(num, g_top_line + 1);
        for (int i = 0; num[i]; i++) status[pos++] = num[i];
        status[pos++] = '/';
        uint_to_dec(num, g_hex_total_lines);
        for (int i = 0; num[i]; i++) status[pos++] = num[i];
    }

    status[pos++] = ' '; status[pos++] = ' ';
    char szbuf[12];
    uint_to_dec(szbuf, (int)g_size);
    for (int i = 0; szbuf[i]; i++) status[pos++] = szbuf[i];
    status[pos++] = 'B';
    status[pos++] = ' '; status[pos++] = ' ';

    if (g_mode == MODE_TEXT) {
        status[pos++] = 'T'; status[pos++] = 'E'; status[pos++] = 'X'; status[pos++] = 'T';
    } else {
        status[pos++] = 'H'; status[pos++] = 'E'; status[pos++] = 'X';
    }
    status[pos] = '\0';

    sys_gui_text(fd, 8, sy + 5, status, COL_TEXT, COL_STATUS);
}

static void draw_text_mode(int fd) {
    int y = CONTENT_Y;
    char lnum[8];

    for (int r = 0; r < ROWS; r++) {
        int li = g_top_line + r;
        if (li >= g_line_count) break;

        uint32_t line_bg = (li == g_found_line) ? COL_FOUND : COL_BG;

        sys_gui_fill(fd, 0, y, GUTTER_W, CELL_H, COL_GUTTER);
        uint_to_dec(lnum, li + 1);
        int nlen = (int)my_strlen(lnum);
        int gx = GUTTER_W - (nlen + 1) * CELL_W;
        if (gx < 2) gx = 2;
        sys_gui_text(fd, gx, y + 1, lnum, COL_DIM, COL_GUTTER);

        if (li == g_found_line) {
            sys_gui_fill(fd, GUTTER_W, y, WIN_W - GUTTER_W, CELL_H, COL_FOUND);
        }

        long start = g_line_off[li];
        long end = (li + 1 < g_line_count) ? g_line_off[li + 1] : g_size;
        if (end > start && g_buf[end - 1] == '\n') end--;
        long len = end - start;
        if (len > TEXT_COLS) len = TEXT_COLS;

        char line[TEXT_COLS + 1];
        for (long k = 0; k < len; k++) {
            char c = g_buf[start + k];
            if (c < 0x20 || c > 0x7E) c = '.';
            line[k] = c;
        }
        line[len] = '\0';
        sys_gui_text(fd, GUTTER_W + PAD, y + 1, line, COL_TEXT, line_bg);
        y += CELL_H;
    }
}

static void draw_hex_mode(int fd) {
    int y = CONTENT_Y;
    char line[80];

    for (int r = 0; r < ROWS; r++) {
        int row_idx = g_top_line + r;
        long off = (long)row_idx * 16;
        if (off >= g_size) break;

        offset_to_hex8(line, off);
        line[8] = ' '; line[9] = ' '; line[10] = '|'; line[11] = ' ';
        int pos = 12;

        int count = 16;
        if (off + count > g_size) count = (int)(g_size - off);

        for (int i = 0; i < 16; i++) {
            if (i == 8) { line[pos++] = ' '; }
            if (i < count) {
                byte_to_hex(&line[pos], (uint8_t)g_buf[off + i]);
                pos += 2;
            } else {
                line[pos++] = ' '; line[pos++] = ' ';
            }
            line[pos++] = ' ';
        }

        line[pos++] = '|'; line[pos++] = ' ';

        for (int i = 0; i < 16; i++) {
            if (i < count) {
                uint8_t c = (uint8_t)g_buf[off + i];
                line[pos++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
            } else {
                line[pos++] = ' ';
            }
        }
        line[pos] = '\0';

        sys_gui_text(fd, PAD, y + 1, line, COL_TEXT, COL_BG);
        y += CELL_H;
    }
}

static void draw_search_bar(int fd) {
    if (!g_search_active) return;
    int by = WIN_H - STATUS_H - 24;
    sys_gui_fill(fd, 0, by, WIN_W, 24, COL_TOOLBAR);

    char bar[SEARCH_MAX + 12];
    bar[0] = '/'; bar[1] = ' ';
    int p = 2;
    for (int i = 0; i < g_search_len && p < (int)sizeof(bar) - 2; i++)
        bar[p++] = g_search[i];
    bar[p++] = '_';
    bar[p] = '\0';

    sys_gui_text(fd, 8, by + 6, bar, COL_ACCENT, COL_TOOLBAR);
}

static void redraw(int fd, const char *path) {
    sys_gui_fill(fd, 0, 0, WIN_W, WIN_H, COL_BG);
    draw_toolbar(fd, path);

    if (g_mode == MODE_TEXT)
        draw_text_mode(fd);
    else
        draw_hex_mode(fd);

    draw_search_bar(fd);
    draw_status(fd, path);
    sys_gui_flip(fd);
}

/* ---- toolbar hit testing --------------------------------------- */

static int toolbar_hit(int x, int y) {
    if (y < 0 || y >= TOOLBAR_H) return 0;
    if (x >= 4 && x < 40) return 1;   /* Text button */
    if (x >= 52 && x < 88) return 2;  /* Hex button */
    if (x >= 92 && x < 148) return 3; /* Search button */
    return 0;
}

/* ---- main ------------------------------------------------------ */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    const char *path = (argc >= 2) ? argv[1] : "/readme.txt";

    long n = sys_fs_readfile(path, g_buf, VIEWER_CAP);
    if (n < 0) {
        const char *msg = "gui_viewer: cannot read file";
        g_size = (long)my_strlen(msg);
        for (long i = 0; i < g_size; i++) g_buf[i] = msg[i];
        g_buf[g_size] = '\0';
    } else {
        g_size = n;
        g_buf[g_size] = '\0';
    }
    index_lines();

    int fd = sys_gui_create(WIN_W, WIN_H, "Viewer");
    if (fd < 0) {
        putstr_console("gui_viewer: sys_gui_create failed\n");
        return 1;
    }
    redraw(fd, path);

    for (;;) {
        struct gui_event ev;
        int got = sys_gui_poll_event(fd, &ev);
        if (got <= 0) { sys_yield(); continue; }
        if (ev.type == GUI_EV_CLOSE) return 0;

        if (ev.type == GUI_EV_MOUSE_DOWN) {
            int hit = toolbar_hit(ev.x, ev.y);
            if (hit == 1 && g_mode != MODE_TEXT) {
                g_mode = MODE_TEXT;
                g_top_line = 0;
                redraw(fd, path);
            } else if (hit == 2 && g_mode != MODE_HEX) {
                g_mode = MODE_HEX;
                g_top_line = 0;
                redraw(fd, path);
            } else if (hit == 3) {
                g_search_active = 1;
                g_search_len = 0;
                redraw(fd, path);
            }
            continue;
        }

        if (ev.type != GUI_EV_KEY) continue;

        if (g_search_active) {
            if (ev.key == 27) {
                g_search_active = 0;
            } else if (ev.key == '\n') {
                g_search[g_search_len] = '\0';
                int found = search_forward(g_top_line);
                if (found < 0) found = search_forward(0);
                if (found >= 0) {
                    g_found_line = found;
                    g_top_line = found;
                    int mt = max_top();
                    if (g_top_line > mt) g_top_line = mt;
                }
                g_search_active = 0;
            } else if (ev.key == '\b' || ev.key == 127) {
                if (g_search_len > 0) g_search_len--;
            } else if (ev.key >= 0x20 && ev.key <= 0x7E) {
                if (g_search_len < SEARCH_MAX - 1)
                    g_search[g_search_len++] = (char)ev.key;
            }
            redraw(fd, path);
            continue;
        }

        switch (ev.key) {
        case 'j': case '\n':  scroll_down(1);    break;
        case 'k': case '\b':  scroll_up(1);      break;
        case 'd': case ' ':   scroll_down(ROWS); break;
        case 'u':             scroll_up(ROWS);   break;
        case 'g':             g_top_line = 0;    break;
        case 'G': {
            g_top_line = max_top();
            break;
        }
        case 'f': case '/':
            g_search_active = 1;
            g_search_len = 0;
            break;
        case 'n': {
            if (g_search_len > 0) {
                g_search[g_search_len] = '\0';
                int start = (g_found_line >= 0) ? g_found_line + 1 : g_top_line;
                int found = search_forward(start);
                if (found < 0) found = search_forward(0);
                if (found >= 0) {
                    g_found_line = found;
                    g_top_line = found;
                    int mt = max_top();
                    if (g_top_line > mt) g_top_line = mt;
                }
            }
            break;
        }
        case 'q':
            sys_exit(0);
        default:
            continue;
        }
        redraw(fd, path);
    }
}
