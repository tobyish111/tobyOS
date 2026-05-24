/* user_gui_edit/main.c -- /bin/gui_edit (TobyEdit), a simple GUI text editor.
 *
 * Opens a file from argv[1] (or starts empty). Supports insert/delete,
 * arrow navigation, Home/End, line number display, scrolling, and
 * Ctrl+S save / Ctrl+Q quit / Ctrl+N new file.
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
typedef unsigned long long uint64_t;
typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;

#define SYS_EXIT            0
#define SYS_WRITE           1
#define SYS_READ            2
#define SYS_CLOSE           4
#define SYS_YIELD           5
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_OPEN           35
#define SYS_CLOCK_MS       48

#define O_RDONLY  0x0
#define O_WRONLY  0x1
#define O_CREAT   0x040
#define O_TRUNC   0x200

struct gui_event {
    int     type;
    int     x;
    int     y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};
#define GUI_EV_KEY   4
#define GUI_EV_CLOSE 5

#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83
#define KEY_HOME  0x84
#define KEY_END   0x85

/* ---- syscall stubs --------------------------------------------- */

static inline void sys_exit(int code) {
    __asm__ volatile("syscall"
        : : "a"((long)SYS_EXIT), "D"((long)code)
        : "rcx", "r11", "memory");
    __builtin_unreachable();
}
static inline ssize_t sys_write_fd(int fd, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
static inline ssize_t sys_read(int fd, void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_READ), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_open(const char *path, int flags, int mode) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_OPEN), "D"(path), "S"((long)flags), "d"((long)mode)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_close(int fd) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_CLOSE), "D"((long)fd)
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

/* ---- tiny helpers ---------------------------------------------- */

static size_t my_strlen(const char *s) {
    const char *p = s; while (*p) p++; return (size_t)(p - s);
}
static void my_memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
}
static void my_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}
static void my_strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
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

static int str_append(char *buf, int pos, const char *s) {
    while (*s) buf[pos++] = *s++;
    return pos;
}

static int str_append_uint(char *buf, int pos, unsigned v) {
    char tmp[16];
    fmt_uint(tmp, v);
    return str_append(buf, pos, tmp);
}

/* ---- layout ---------------------------------------------------- */

#define WIN_W       640
#define WIN_H       480

#define TITLE_H      20
#define EDITOR_Y     TITLE_H
#define STATUS_H     20
#define EDITOR_H     (WIN_H - TITLE_H - STATUS_H)
#define STATUS_Y     (WIN_H - STATUS_H)

#define CELL_W       8
#define CELL_H       12
#define LINE_H       14

#define GUTTER_W     40
#define TEXT_X       (GUTTER_W + 4)
#define TEXT_W       (WIN_W - TEXT_X - 4)
#define VISIBLE_ROWS (EDITOR_H / LINE_H)
#define VISIBLE_COLS ((WIN_W - TEXT_X - 4) / CELL_W)

#define COL_BG       0x001E1E2Eu
#define COL_GUTTER   0x00181825u
#define COL_LINENUM  0x006C7086u
#define COL_TEXT     0x00CDD6F4u
#define COL_CURSOR   0x00313244u
#define COL_STATUS   0x00181825u
#define COL_ST_TEXT  0x00A6ADC8u
#define COL_MODIFIED 0x00F38BA8u
#define COL_TITLE_BG 0x00181825u
#define COL_TITLE_FG 0x00CDD6F4u
#define COL_ACCENT   0x0089B4FAu
#define COL_MSG_OK   0x00A6E3A1u
#define COL_MSG_ERR  0x00F38BA8u

/* ---- text buffer ----------------------------------------------- */

#define MAX_LINES    512
#define MAX_COLS     256

static char g_lines[MAX_LINES][MAX_COLS];
static int  g_line_count = 1;
static int  g_cursor_row;
static int  g_cursor_col;
static int  g_scroll_row;
static int  g_modified;

static char g_filename[256];
static int  g_has_file;

static char g_message[64];
static uint32_t g_msg_color;
static long g_msg_expire;

static int line_len(int row) {
    return (int)my_strlen(g_lines[row]);
}

static void set_message(const char *msg, uint32_t color) {
    my_strncpy(g_message, msg, sizeof(g_message));
    g_msg_color = color;
    g_msg_expire = sys_clock_ms() + 3000;
}

/* ---- file I/O -------------------------------------------------- */

static void load_file(const char *path) {
    long ffd = sys_open(path, O_RDONLY, 0);
    if (ffd < 0) {
        set_message("Open failed", COL_MSG_ERR);
        return;
    }

    static char filebuf[MAX_LINES * 80];
    ssize_t total = 0;
    for (;;) {
        ssize_t n = sys_read((int)ffd, filebuf + total,
                             sizeof(filebuf) - (size_t)total - 1);
        if (n <= 0) break;
        total += n;
        if ((size_t)total >= sizeof(filebuf) - 1) break;
    }
    sys_close((int)ffd);
    filebuf[total] = '\0';

    my_memset(g_lines, 0, sizeof(g_lines));
    g_line_count = 0;
    g_cursor_row = 0;
    g_cursor_col = 0;
    g_scroll_row = 0;

    int col = 0;
    for (ssize_t i = 0; i < total && g_line_count < MAX_LINES; i++) {
        if (filebuf[i] == '\n') {
            g_lines[g_line_count][col] = '\0';
            g_line_count++;
            col = 0;
        } else if (filebuf[i] == '\r') {
            continue;
        } else {
            if (col < MAX_COLS - 1)
                g_lines[g_line_count][col++] = filebuf[i];
        }
    }
    if (col > 0 || g_line_count == 0) {
        g_lines[g_line_count][col] = '\0';
        g_line_count++;
    }
    if (g_line_count == 0)
        g_line_count = 1;

    g_modified = 0;
    my_strcpy(g_filename, path);
    g_has_file = 1;
    set_message("Loaded", COL_MSG_OK);
}

static void save_file(void) {
    if (!g_has_file) {
        set_message("No filename", COL_MSG_ERR);
        return;
    }

    long ffd = sys_open(g_filename, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (ffd < 0) {
        set_message("Save failed", COL_MSG_ERR);
        return;
    }

    for (int i = 0; i < g_line_count; i++) {
        int len = line_len(i);
        if (len > 0)
            sys_write_fd((int)ffd, g_lines[i], (size_t)len);
        if (i < g_line_count - 1)
            sys_write_fd((int)ffd, "\n", 1);
    }

    sys_close((int)ffd);
    g_modified = 0;
    set_message("Saved", COL_MSG_OK);
}

static void new_file(void) {
    my_memset(g_lines, 0, sizeof(g_lines));
    g_line_count = 1;
    g_cursor_row = 0;
    g_cursor_col = 0;
    g_scroll_row = 0;
    g_modified = 0;
    g_has_file = 0;
    g_filename[0] = '\0';
    set_message("New file", COL_MSG_OK);
}

/* ---- editing operations ---------------------------------------- */

static void ensure_cursor_visible(void) {
    if (g_cursor_row < g_scroll_row)
        g_scroll_row = g_cursor_row;
    if (g_cursor_row >= g_scroll_row + VISIBLE_ROWS)
        g_scroll_row = g_cursor_row - VISIBLE_ROWS + 1;
    if (g_scroll_row < 0)
        g_scroll_row = 0;
}

static void insert_char(char ch) {
    int len = line_len(g_cursor_row);
    if (len >= MAX_COLS - 2) return;
    for (int i = len; i >= g_cursor_col; i--)
        g_lines[g_cursor_row][i + 1] = g_lines[g_cursor_row][i];
    g_lines[g_cursor_row][g_cursor_col] = ch;
    g_cursor_col++;
    g_modified = 1;
}

static void delete_back(void) {
    if (g_cursor_col > 0) {
        int len = line_len(g_cursor_row);
        for (int i = g_cursor_col - 1; i < len; i++)
            g_lines[g_cursor_row][i] = g_lines[g_cursor_row][i + 1];
        g_cursor_col--;
        g_modified = 1;
    } else if (g_cursor_row > 0) {
        int prev_len = line_len(g_cursor_row - 1);
        int cur_len  = line_len(g_cursor_row);
        if (prev_len + cur_len < MAX_COLS - 1) {
            my_memcpy(g_lines[g_cursor_row - 1] + prev_len,
                      g_lines[g_cursor_row], (size_t)(cur_len + 1));
        }
        for (int i = g_cursor_row; i < g_line_count - 1; i++)
            my_memcpy(g_lines[i], g_lines[i + 1], MAX_COLS);
        my_memset(g_lines[g_line_count - 1], 0, MAX_COLS);
        g_line_count--;
        g_cursor_row--;
        g_cursor_col = prev_len;
        g_modified = 1;
        ensure_cursor_visible();
    }
}

static void split_line(void) {
    if (g_line_count >= MAX_LINES) return;

    for (int i = g_line_count; i > g_cursor_row + 1; i--)
        my_memcpy(g_lines[i], g_lines[i - 1], MAX_COLS);
    g_line_count++;

    int len = line_len(g_cursor_row);
    int tail = len - g_cursor_col;
    if (tail > 0) {
        my_memcpy(g_lines[g_cursor_row + 1], g_lines[g_cursor_row] + g_cursor_col,
                  (size_t)(tail + 1));
    } else {
        g_lines[g_cursor_row + 1][0] = '\0';
    }
    g_lines[g_cursor_row][g_cursor_col] = '\0';

    g_cursor_row++;
    g_cursor_col = 0;
    g_modified = 1;
    ensure_cursor_visible();
}

/* ---- drawing --------------------------------------------------- */

static void redraw(int gfd) {
    sys_gui_fill(gfd, 0, 0, WIN_W, WIN_H, COL_BG);

    /* Title bar */
    sys_gui_fill(gfd, 0, 0, WIN_W, TITLE_H, COL_TITLE_BG);
    sys_gui_fill(gfd, 0, TITLE_H - 1, WIN_W, 1, COL_ACCENT);

    char title[80];
    int tp = 0;
    tp = str_append(title, tp, "TobyEdit");
    if (g_has_file) {
        tp = str_append(title, tp, " - ");
        tp = str_append(title, tp, g_filename);
    } else {
        tp = str_append(title, tp, " - [untitled]");
    }
    if (g_modified)
        tp = str_append(title, tp, " *");
    title[tp] = '\0';
    sys_gui_text(gfd, 8, 5, title,
                 g_modified ? COL_MODIFIED : COL_TITLE_FG, COL_TITLE_BG);

    /* Gutter background */
    sys_gui_fill(gfd, 0, EDITOR_Y, GUTTER_W, EDITOR_H, COL_GUTTER);
    sys_gui_fill(gfd, GUTTER_W, EDITOR_Y, 1, EDITOR_H, 0x00313244u);

    /* Editor lines */
    for (int vi = 0; vi < VISIBLE_ROWS; vi++) {
        int row = g_scroll_row + vi;
        if (row >= g_line_count) break;

        int y = EDITOR_Y + vi * LINE_H;
        int is_cursor_line = (row == g_cursor_row);

        if (is_cursor_line)
            sys_gui_fill(gfd, GUTTER_W + 1, y, WIN_W - GUTTER_W - 1, LINE_H, COL_CURSOR);

        uint32_t gutter_bg = is_cursor_line ? COL_CURSOR : COL_GUTTER;
        if (is_cursor_line)
            sys_gui_fill(gfd, 0, y, GUTTER_W, LINE_H, gutter_bg);

        char lnum[8];
        fmt_uint(lnum, (unsigned)(row + 1));
        int lnum_len = (int)my_strlen(lnum);
        int lnum_x = GUTTER_W - (lnum_len + 1) * CELL_W;
        if (lnum_x < 2) lnum_x = 2;
        sys_gui_text(gfd, lnum_x, y + 1, lnum,
                     is_cursor_line ? COL_ACCENT : COL_LINENUM, gutter_bg);

        int len = line_len(row);
        if (len > 0) {
            char disp[VISIBLE_COLS + 2];
            int start = 0;
            int dlen = len - start;
            if (dlen > VISIBLE_COLS) dlen = VISIBLE_COLS;
            if (dlen > 0) {
                my_memcpy(disp, g_lines[row] + start, (size_t)dlen);
                disp[dlen] = '\0';
                uint32_t lbg = is_cursor_line ? COL_CURSOR : COL_BG;
                sys_gui_text(gfd, TEXT_X, y + 1, disp, COL_TEXT, lbg);
            }
        }

        if (is_cursor_line) {
            int cx = TEXT_X + g_cursor_col * CELL_W;
            if (cx < WIN_W - 4)
                sys_gui_fill(gfd, cx, y + 1, 2, CELL_H, COL_ACCENT);
        }
    }

    /* Status bar */
    sys_gui_fill(gfd, 0, STATUS_Y, WIN_W, STATUS_H, COL_STATUS);
    sys_gui_fill(gfd, 0, STATUS_Y, WIN_W, 1, 0x00313244u);

    char stbuf[128];
    int sp = 0;
    sp = str_append(stbuf, sp, " Ln ");
    sp = str_append_uint(stbuf, sp, (unsigned)(g_cursor_row + 1));
    sp = str_append(stbuf, sp, ", Col ");
    sp = str_append_uint(stbuf, sp, (unsigned)(g_cursor_col + 1));
    sp = str_append(stbuf, sp, "  |  ");
    if (g_has_file)
        sp = str_append(stbuf, sp, g_filename);
    else
        sp = str_append(stbuf, sp, "[untitled]");
    if (g_modified)
        sp = str_append(stbuf, sp, "  [Modified]");
    sp = str_append(stbuf, sp, "  |  ");
    sp = str_append_uint(stbuf, sp, (unsigned)g_line_count);
    sp = str_append(stbuf, sp, " lines");
    stbuf[sp] = '\0';
    sys_gui_text(gfd, 4, STATUS_Y + 5, stbuf, COL_ST_TEXT, COL_STATUS);

    if (g_message[0] && sys_clock_ms() < g_msg_expire) {
        int mx = WIN_W - ((int)my_strlen(g_message) + 1) * CELL_W;
        sys_gui_text(gfd, mx, STATUS_Y + 5, g_message, g_msg_color, COL_STATUS);
    }

    /* Scrollbar */
    if (g_line_count > VISIBLE_ROWS) {
        int sb_x = WIN_W - 6;
        sys_gui_fill(gfd, sb_x, EDITOR_Y, 4, EDITOR_H, 0x00252536u);
        int thumb_h = (VISIBLE_ROWS * EDITOR_H) / g_line_count;
        if (thumb_h < 8) thumb_h = 8;
        int max_scroll = g_line_count - VISIBLE_ROWS;
        int thumb_y = EDITOR_Y;
        if (max_scroll > 0)
            thumb_y += (g_scroll_row * (EDITOR_H - thumb_h)) / max_scroll;
        sys_gui_fill(gfd, sb_x, thumb_y, 4, thumb_h, COL_ACCENT);
    }

    sys_gui_flip(gfd);
}

/* ---- main ------------------------------------------------------ */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    my_memset(g_lines, 0, sizeof(g_lines));
    g_line_count = 1;
    g_cursor_row = 0;
    g_cursor_col = 0;
    g_scroll_row = 0;
    g_modified = 0;
    g_has_file = 0;
    g_filename[0] = '\0';
    g_message[0] = '\0';

    if (argc > 1 && argv[1] && argv[1][0]) {
        my_strcpy(g_filename, argv[1]);
        g_has_file = 1;
        load_file(argv[1]);
    }

    int gfd = sys_gui_create(WIN_W, WIN_H, "TobyEdit");
    if (gfd < 0) {
        sys_write_fd(1, "gui_edit: gui_create failed\n", 28);
        return 1;
    }

    redraw(gfd);

    int needs_redraw = 0;

    for (;;) {
        struct gui_event ev;
        int got = sys_gui_poll_event(gfd, &ev);

        if (got > 0) {
            if (ev.type == GUI_EV_CLOSE)
                sys_exit(0);

            if (ev.type == GUI_EV_KEY) {
                uint8_t k = ev.key;
                needs_redraw = 1;

                if (k == 0x13) {
                    save_file();
                } else if (k == 0x11) {
                    sys_exit(0);
                } else if (k == 0x0E) {
                    new_file();
                } else if (k == KEY_UP) {
                    if (g_cursor_row > 0) {
                        g_cursor_row--;
                        int len = line_len(g_cursor_row);
                        if (g_cursor_col > len) g_cursor_col = len;
                        ensure_cursor_visible();
                    }
                } else if (k == KEY_DOWN) {
                    if (g_cursor_row < g_line_count - 1) {
                        g_cursor_row++;
                        int len = line_len(g_cursor_row);
                        if (g_cursor_col > len) g_cursor_col = len;
                        ensure_cursor_visible();
                    }
                } else if (k == KEY_LEFT) {
                    if (g_cursor_col > 0) {
                        g_cursor_col--;
                    } else if (g_cursor_row > 0) {
                        g_cursor_row--;
                        g_cursor_col = line_len(g_cursor_row);
                        ensure_cursor_visible();
                    }
                } else if (k == KEY_RIGHT) {
                    int len = line_len(g_cursor_row);
                    if (g_cursor_col < len) {
                        g_cursor_col++;
                    } else if (g_cursor_row < g_line_count - 1) {
                        g_cursor_row++;
                        g_cursor_col = 0;
                        ensure_cursor_visible();
                    }
                } else if (k == KEY_HOME) {
                    g_cursor_col = 0;
                } else if (k == KEY_END) {
                    g_cursor_col = line_len(g_cursor_row);
                } else if (k == '\b' || k == 127) {
                    delete_back();
                } else if (k == '\n' || k == '\r') {
                    split_line();
                } else if (k == '\t') {
                    for (int ti = 0; ti < 4; ti++)
                        insert_char(' ');
                } else if (k >= 0x20 && k <= 0x7E) {
                    insert_char((char)k);
                } else {
                    needs_redraw = 0;
                }
            }
        }

        if (g_message[0] && sys_clock_ms() >= g_msg_expire) {
            g_message[0] = '\0';
            needs_redraw = 1;
        }

        if (needs_redraw) {
            redraw(gfd);
            needs_redraw = 0;
        }

        sys_yield();
    }
}
