/* user_gui_viewer/main.c -- /bin/gui_viewer, the minimal text viewer.
 *
 * Opened by the file manager (or the terminal `run` builtin) with the
 * file path in argv[1]. Reads up to VIEWER_CAP bytes via SYS_FS_READFILE
 * and paints the contents into a scrollable text window.
 *
 * Controls:
 *    Arrow keys / j / k -> line up / down
 *    PgUp / PgDn / u / d -> page up / down
 *    Home / g           -> top
 *    End  / G           -> bottom
 *    q                  -> quit
 *
 * We only have a small set of keys delivered as ASCII today (no arrow
 * key codes), so the viewer honors the lowercase letter shortcuts
 * above -- they're the same ones vim / less use and feel natural.
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
#define SYS_FS_READFILE    19

struct gui_event {
    int     type;
    int     x;
    int     y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};
#define GUI_EV_KEY 4

static inline ssize_t sys_write(int fd, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
static inline void sys_yield(void) {
    __asm__ volatile ("syscall"
        : : "a"((long)SYS_YIELD)
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

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}
static void putstr_console(const char *s) {
    sys_write(1, s, my_strlen(s));
}

/* ---- window + text layout ------------------------------------ */

#define WIN_W        560
#define WIN_H        400
#define HDR_H        20
#define PAD          6
#define CELL_W       8
#define CELL_H       12
#define COLS         ((WIN_W - 2 * PAD) / CELL_W)
#define ROWS         ((WIN_H - HDR_H - 2 * PAD) / CELL_H)

#define COL_BG       0x00141820u
#define COL_HDR_BG   0x00303848u
#define COL_HDR_FG   0x00E0F0FFu
#define COL_TEXT_FG  0x00E0E0E0u

/* Viewer file cap. 32 KiB is plenty for readme / motd-sized text
 * files; if larger we just show the first 32 KiB with a trailing
 * marker. */
#define VIEWER_CAP   (32 * 1024)
static char g_buf[VIEWER_CAP + 1];
static long g_size = 0;

/* Line index: byte offset of each line's start in g_buf. Rebuilt once
 * on load. */
#define LINES_MAX    4096
static long g_line_off[LINES_MAX];
static int  g_line_count = 0;
static int  g_top_line   = 0;

static void index_lines(void) {
    g_line_count = 0;
    g_line_off[g_line_count++] = 0;
    for (long i = 0; i < g_size && g_line_count < LINES_MAX; i++) {
        if (g_buf[i] == '\n' && i + 1 < g_size) {
            g_line_off[g_line_count++] = i + 1;
        }
    }
}

static void scroll_up(int n) {
    g_top_line -= n;
    if (g_top_line < 0) g_top_line = 0;
}
static void scroll_down(int n) {
    g_top_line += n;
    int max_top = g_line_count - ROWS;
    if (max_top < 0) max_top = 0;
    if (g_top_line > max_top) g_top_line = max_top;
}

static void redraw(int fd, const char *path) {
    sys_gui_fill(fd, 0, 0, WIN_W, WIN_H, COL_BG);

    /* Header bar with path + line counter. */
    sys_gui_fill(fd, 0, 0, WIN_W, HDR_H, COL_HDR_BG);
    sys_gui_text(fd, 6, 6, path, COL_HDR_FG, COL_HDR_BG);

    /* Body: draw up to ROWS lines starting at g_top_line. Each line
     * is drawn as one sys_gui_text call, truncated to COLS chars. */
    char line[COLS + 1];
    int y = HDR_H + PAD;
    for (int r = 0; r < ROWS; r++) {
        int li = g_top_line + r;
        if (li >= g_line_count) break;
        long start = g_line_off[li];
        long end = (li + 1 < g_line_count) ? g_line_off[li + 1] : g_size;
        if (end > start && g_buf[end - 1] == '\n') end--;
        long len = end - start;
        if (len > COLS) len = COLS;
        for (long k = 0; k < len; k++) {
            char c = g_buf[start + k];
            if (c < 0x20 || c > 0x7E) c = '.';
            line[k] = c;
        }
        line[len] = '\0';
        sys_gui_text(fd, PAD, y, line, COL_TEXT_FG, COL_BG);
        y += CELL_H;
    }

    sys_gui_flip(fd);
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    const char *path = (argc >= 2) ? argv[1] : "/readme.txt";

    long n = sys_fs_readfile(path, g_buf, VIEWER_CAP);
    if (n < 0) {
        /* Failed to read -- present an error line so the user sees
         * something rather than an empty window. */
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
        if (ev.type != GUI_EV_KEY) continue;

        switch (ev.key) {
        case 'j': case '\n':  scroll_down(1);    break;
        case 'k': case '\b':  scroll_up(1);      break;
        case 'd': case ' ':   scroll_down(ROWS); break;
        case 'u':             scroll_up(ROWS);   break;
        case 'g':             g_top_line = 0;    break;
        case 'G': {
            int max_top = g_line_count - ROWS;
            if (max_top < 0) max_top = 0;
            g_top_line = max_top;
            break;
        }
        case 'q':
            sys_exit(0);
        default:
            continue;   /* don't redraw on keys we ignore */
        }
        redraw(fd, path);
    }
}
