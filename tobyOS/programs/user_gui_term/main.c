/* user_gui_term/main.c -- /bin/gui_term, the GUI terminal emulator.
 *
 * Flow:
 *   1. sys_gui_create() opens a terminal-shaped window.
 *   2. sys_term_open()  allocates a kernel-side session.
 *   3. Forever:
 *        - poll one GUI event; a GUI_EV_KEY byte goes to sys_term_write
 *        - drain the session's output ring (sys_term_read) into a tiny
 *          VT processor that updates an in-memory text grid
 *        - if the grid changed, redraw the dirty rows
 *        - sys_yield
 *
 * The "VT processor" is deliberately trivial:
 *     printable (0x20..0x7E) -> write cell, advance cursor
 *     '\r'                   -> cursor col = 0
 *     '\n'                   -> next row, scroll if needed
 *     '\b'                   -> back one column (cell erase already
 *                               issued by the kernel as BS SP BS)
 *     0x0C (form-feed)       -> clear screen
 *
 * A scrollback buffer is left for a future milestone -- we just scroll
 * the visible grid in-place when the cursor walks off the last row.
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
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
#define SYS_TERM_OPEN      15
#define SYS_TERM_WRITE     16
#define SYS_TERM_READ      17

/* Must stay byte-compatible with struct gui_event in the kernel. */
struct gui_event {
    int     type;
    int     x;
    int     y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};
#define GUI_EV_KEY 4

/* ---- syscall stubs --------------------------------------------- */

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

/* ---- helpers --------------------------------------------------- */

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}
static void putstr_console(const char *s) {
    sys_write(1, s, my_strlen(s));
}

/* ---- terminal grid -------------------------------------------- */

/* 8x8 glyphs painted into 8-wide x CELL_H-tall cells. CELL_H=12 leaves
 * 4 px of vertical padding between lines so text doesn't touch. */
#define COLS        64
#define ROWS        20
#define CELL_W      8
#define CELL_H      12
#define PAD         4       /* per-edge padding from the window's edge */
#define WIN_W       (COLS * CELL_W + 2 * PAD)
#define WIN_H       (ROWS * CELL_H + 2 * PAD)

#define COL_BG      0x00101820u   /* dark slate */
#define COL_FG      0x00E0F0E0u   /* off-white greenish */
#define COL_CURSOR  0x00FFD060u   /* same yellow as the toolkit focus ring */

/* Per-cell char. Empty cells hold ' '. The grid is redrawn row-by-row
 * so we don't need per-cell dirty tracking (25 sys_gui_text calls per
 * redraw is cheap). */
static char g_grid[ROWS][COLS];
static int  g_cx = 0;
static int  g_cy = 0;
static int  g_need_redraw = 1;

static void grid_clear(void) {
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            g_grid[y][x] = ' ';
    g_cx = 0;
    g_cy = 0;
    g_need_redraw = 1;
}

/* Scroll the grid up by one row: discard row 0, shift everything up,
 * blank the new last row. Called when the cursor advances past the
 * bottom. Matches the Unix "scroll on \n past last line" convention. */
static void grid_scroll_up(void) {
    for (int y = 0; y + 1 < ROWS; y++) {
        for (int x = 0; x < COLS; x++) g_grid[y][x] = g_grid[y + 1][x];
    }
    for (int x = 0; x < COLS; x++) g_grid[ROWS - 1][x] = ' ';
}

static void grid_newline(void) {
    g_cx = 0;
    if (g_cy + 1 >= ROWS) {
        grid_scroll_up();
    } else {
        g_cy++;
    }
}

/* Feed a single byte into the VT. Only the tiny subset we actually
 * produce is handled -- anything else is silently dropped, which is
 * the right behaviour for our controlled kernel-side producer. */
static void vt_putc(char c) {
    unsigned char u = (unsigned char)c;
    if (u == '\r') {
        g_cx = 0;
    } else if (u == '\n') {
        grid_newline();
    } else if (u == 0x08) {
        if (g_cx > 0) g_cx--;
    } else if (u == 0x0C) {
        grid_clear();
    } else if (u >= 0x20 && u < 0x7F) {
        if (g_cx >= COLS) grid_newline();
        g_grid[g_cy][g_cx++] = (char)u;
    }
    /* Tab / bell / etc: ignored. */
    g_need_redraw = 1;
}

/* ---- redraw ---------------------------------------------------- */

/* Paint the whole grid. We blank one row at a time and then draw all
 * COLS chars as a single sys_gui_text string -- keeps the number of
 * syscalls proportional to ROWS, not ROWS*COLS. */
static void redraw(int fd) {
    sys_gui_fill(fd, 0, 0, WIN_W, WIN_H, COL_BG);

    char line[COLS + 1];
    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) line[x] = g_grid[y][x];
        line[COLS] = '\0';
        sys_gui_text(fd, PAD, PAD + y * CELL_H, line, COL_FG, COL_BG);
    }
    /* Cursor: 8x2 bar under the current column. */
    int cy = PAD + g_cy * CELL_H + (CELL_H - 2);
    int cx = PAD + g_cx * CELL_W;
    sys_gui_fill(fd, cx, cy, CELL_W, 2, COL_CURSOR);

    sys_gui_flip(fd);
    g_need_redraw = 0;
}

/* ---- main ----------------------------------------------------- */

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
        putstr_console("gui_term: sys_term_open failed (pool full?)\n");
        return 1;
    }

    grid_clear();
    redraw(win);

    char inbuf[1];
    char outbuf[128];

    for (;;) {
        /* Drain all pending GUI events -- we only care about keys. */
        struct gui_event ev;
        int got = 1;
        while (got > 0) {
            got = sys_gui_poll_event(win, &ev);
            if (got <= 0) break;
            if (ev.type == GUI_EV_KEY && ev.key != 0) {
                inbuf[0] = (char)ev.key;
                sys_term_write(sess, inbuf, 1);
            }
        }

        /* Drain as much of the output ring as we can this tick. */
        long n = sys_term_read(sess, outbuf, sizeof(outbuf));
        while (n > 0) {
            for (long i = 0; i < n; i++) vt_putc(outbuf[i]);
            n = sys_term_read(sess, outbuf, sizeof(outbuf));
        }

        if (g_need_redraw) redraw(win);
        sys_yield();
    }
}
