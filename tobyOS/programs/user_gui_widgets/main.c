/* user_gui_widgets/main.c -- /bin/gui_widgets, the Notes application.
 *
 * A note-taking app with sidebar file list, text editor with line numbers,
 * and toolbar for file operations. Notes stored in /data/notes/.
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
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
#define SYS_FS_READDIR     18
#define SYS_FS_READFILE    19
#define SYS_OPEN           35
#define SYS_UNLINK         41
#define SYS_MKDIR          42

#define O_RDONLY    0x0
#define O_WRONLY    0x1
#define O_CREAT     0x040
#define O_TRUNC     0x200

#define SYS_FS_NAME_MAX    64
#define SYS_FS_TYPE_FILE   1

struct vfs_dirent_user {
    char     name[SYS_FS_NAME_MAX];
    uint32_t type;
    uint32_t size;
};

struct gui_event {
    int     type;
    int     x;
    int     y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};

#define GUI_EV_MOUSE_DOWN   2
#define GUI_EV_KEY          4
#define GUI_EV_CLOSE        5

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
static inline void sys_yield(void) {
    long _d;
    __asm__ volatile("syscall"
        : "=a"(_d) : "0"((long)SYS_YIELD)
        : "rcx", "r11", "memory");
}
static inline ssize_t sys_write_fd(int fd, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
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
    uint32_t wh = ((uint32_t)(uint16_t)w) |
                  (((uint32_t)(uint16_t)h) << 16);
    register long r10 __asm__("r10") = (long)wh;
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
static inline long sys_fs_readdir(const char *path,
                                  struct vfs_dirent_user *out,
                                  int cap, int offset) {
    long r;
    register long r10 __asm__("r10") = (long)offset;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_FS_READDIR), "D"(path), "S"(out),
          "d"((long)cap), "r"(r10)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_fs_readfile(const char *path, void *out, size_t cap) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_FS_READFILE), "D"(path), "S"(out), "d"(cap)
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
static inline long sys_close(int fd2) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_CLOSE), "D"((long)fd2)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_unlink(const char *path) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_UNLINK), "D"(path)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_mkdir(const char *path, int mode) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_MKDIR), "D"(path), "S"((long)mode)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny libc ------------------------------------------------- */

static size_t my_strlen(const char *s) {
    const char *p = s; while (*p) p++; return (size_t)(p - s);
}
static void my_memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
}
static void my_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s2 = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s2[i];
}
static void my_memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s2 = (const unsigned char *)src;
    if (d < s2) {
        for (size_t i = 0; i < n; i++) d[i] = s2[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s2[i - 1];
    }
}
static void str_copy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
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
    char tmp[16]; fmt_uint(tmp, v);
    return str_append(buf, pos, tmp);
}

/* ---- layout ---------------------------------------------------- */

#define WIN_W           600
#define WIN_H           420

#define TOOLBAR_H       34
#define SIDEBAR_W       160
#define STATUSBAR_H     20

#define SIDEBAR_Y       TOOLBAR_H
#define SIDEBAR_H       (WIN_H - TOOLBAR_H - STATUSBAR_H)

#define EDITOR_X        SIDEBAR_W
#define EDITOR_Y        TOOLBAR_H
#define EDITOR_W        (WIN_W - SIDEBAR_W)
#define EDITOR_H        (WIN_H - TOOLBAR_H - STATUSBAR_H)

#define STATUS_Y        (WIN_H - STATUSBAR_H)

#define CELL_W          8
#define CELL_H          12
#define LINE_H          14

#define GUTTER_W        32
#define TEXT_X          (EDITOR_X + GUTTER_W + 2)
#define TEXT_W          (EDITOR_W - GUTTER_W - 6)
#define VISIBLE_ROWS    (EDITOR_H / LINE_H)
#define VISIBLE_COLS    (TEXT_W / CELL_W)

#define SIDEBAR_ROW_H   20
#define SIDEBAR_VISIBLE ((SIDEBAR_H - 4) / SIDEBAR_ROW_H)

/* ---- colors ---------------------------------------------------- */

#define COL_BG          0x001E1E2Eu
#define COL_SIDEBAR     0x00252536u
#define COL_EDITOR      0x001A1A2Au
#define COL_TEXT        0x00E0E0E0u
#define COL_ACCENT      0x004FC3F7u
#define COL_DIM         0x00808890u
#define COL_TOOLBAR     0x002D2D3Du
#define COL_STATUS      0x002D2D3Du
#define COL_BTN_BG      0x003D3D4Du
#define COL_BTN_BORDER  0x005D5D6Du
#define COL_GUTTER_BG   0x00202030u
#define COL_LINENUM     0x006C7086u
#define COL_CURLINE     0x00252540u
#define COL_CURSOR_BAR  0x004FC3F7u
#define COL_SEL_BG      0x00353546u
#define COL_SEL_MARK    0x004FC3F7u
#define COL_MODIFIED    0x00F38BA8u
#define COL_DANGER      0x00EF5350u

/* ---- state ----------------------------------------------------- */

#define NOTES_DIR       "/data/notes"
#define MAX_NOTES       64
#define BUF_MAX         8192
#define NAME_MAX_LEN    64

static struct vfs_dirent_user g_notes[MAX_NOTES];
static int  g_note_count     = 0;
static int  g_note_sel       = -1;
static int  g_sidebar_scroll = 0;

static char g_buf[BUF_MAX];
static int  g_buf_len     = 0;
static int  g_cursor_x    = 0;
static int  g_cursor_y    = 0;
static int  g_scroll_y    = 0;
static int  g_modified    = 0;
static int  g_total_lines = 1;

static char g_filename[NAME_MAX_LEN] = "";

static int  g_dialog_mode = 0;
static char g_input_buf[NAME_MAX_LEN] = "";
static int  g_input_len   = 0;

static int  g_untitled_n   = 1;

/* ---- text buffer helpers --------------------------------------- */

static int line_start(int line) {
    int off = 0;
    for (int l = 0; l < line && off < g_buf_len; off++)
        if (g_buf[off] == '\n') l++;
    return off;
}

static int line_len(int line) {
    int off = line_start(line);
    int len = 0;
    while (off + len < g_buf_len && g_buf[off + len] != '\n') len++;
    return len;
}

static void recount_lines(void) {
    g_total_lines = 1;
    for (int i = 0; i < g_buf_len; i++)
        if (g_buf[i] == '\n') g_total_lines++;
}

static int cursor_offset(void) {
    return line_start(g_cursor_y) + g_cursor_x;
}

static void clamp_cursor(void) {
    if (g_cursor_y >= g_total_lines) g_cursor_y = g_total_lines - 1;
    if (g_cursor_y < 0) g_cursor_y = 0;
    int len = line_len(g_cursor_y);
    if (g_cursor_x > len) g_cursor_x = len;
    if (g_cursor_x < 0) g_cursor_x = 0;
}

static void ensure_visible(void) {
    if (g_cursor_y < g_scroll_y)
        g_scroll_y = g_cursor_y;
    if (g_cursor_y >= g_scroll_y + VISIBLE_ROWS)
        g_scroll_y = g_cursor_y - VISIBLE_ROWS + 1;
    if (g_scroll_y < 0) g_scroll_y = 0;
}

static void ensure_sidebar_visible(void) {
    if (g_note_sel < 0) return;
    if (g_note_sel < g_sidebar_scroll)
        g_sidebar_scroll = g_note_sel;
    if (g_note_sel >= g_sidebar_scroll + SIDEBAR_VISIBLE)
        g_sidebar_scroll = g_note_sel - SIDEBAR_VISIBLE + 1;
    if (g_sidebar_scroll < 0) g_sidebar_scroll = 0;
}

/* ---- buffer editing -------------------------------------------- */

static void buf_insert(int off, char ch) {
    if (g_buf_len >= BUF_MAX - 1) return;
    my_memmove(g_buf + off + 1, g_buf + off, (size_t)(g_buf_len - off));
    g_buf[off] = ch;
    g_buf_len++;
    g_buf[g_buf_len] = '\0';
    g_modified = 1;
    recount_lines();
}

static void buf_delete(int off) {
    if (off < 0 || off >= g_buf_len) return;
    my_memmove(g_buf + off, g_buf + off + 1, (size_t)(g_buf_len - off - 1));
    g_buf_len--;
    g_buf[g_buf_len] = '\0';
    g_modified = 1;
    recount_lines();
}

/* ---- file operations ------------------------------------------- */

static void build_path(char *out, size_t cap, const char *name) {
    int p = 0;
    const char *dir = NOTES_DIR;
    while (*dir && (size_t)p + 1 < cap) out[p++] = *dir++;
    out[p++] = '/';
    while (*name && (size_t)p + 1 < cap) out[p++] = *name++;
    out[p] = '\0';
}

static void refresh_notes(void) {
    my_memset(g_notes, 0, sizeof(g_notes));
    g_note_count = 0;
    long n = sys_fs_readdir(NOTES_DIR, g_notes, MAX_NOTES, 0);
    if (n <= 0) { g_note_count = 0; return; }

    int wc = 0;
    for (int i = 0; i < (int)n; i++) {
        if (g_notes[i].type == SYS_FS_TYPE_FILE) {
            if (wc != i) my_memcpy(&g_notes[wc], &g_notes[i], sizeof(g_notes[0]));
            wc++;
        }
    }
    g_note_count = wc;

    for (int i = 1; i < g_note_count; i++) {
        struct vfs_dirent_user tmp;
        my_memcpy(&tmp, &g_notes[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0) {
            const char *a = g_notes[j].name, *b = tmp.name;
            while (*a && *b && *a == *b) { a++; b++; }
            if ((int)(unsigned char)*a - (int)(unsigned char)*b <= 0) break;
            my_memcpy(&g_notes[j + 1], &g_notes[j], sizeof(tmp));
            j--;
        }
        my_memcpy(&g_notes[j + 1], &tmp, sizeof(tmp));
    }
}

static void load_note(int idx) {
    if (idx < 0 || idx >= g_note_count) return;
    char path[128];
    build_path(path, sizeof(path), g_notes[idx].name);

    long n = sys_fs_readfile(path, g_buf, BUF_MAX - 1);
    if (n < 0) n = 0;
    g_buf_len = (int)n;
    g_buf[g_buf_len] = '\0';

    g_cursor_x = 0;
    g_cursor_y = 0;
    g_scroll_y = 0;
    g_modified = 0;
    str_copy(g_filename, g_notes[idx].name, NAME_MAX_LEN);
    g_note_sel = idx;
    recount_lines();
    ensure_sidebar_visible();
}

static void save_note(void) {
    if (!g_filename[0]) return;
    char path[128];
    build_path(path, sizeof(path), g_filename);

    long fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0) return;
    if (g_buf_len > 0)
        sys_write_fd((int)fd, g_buf, (size_t)g_buf_len);
    sys_close((int)fd);
    g_modified = 0;

    refresh_notes();
    for (int i = 0; i < g_note_count; i++) {
        if (streq(g_notes[i].name, g_filename)) { g_note_sel = i; break; }
    }
    ensure_sidebar_visible();
}

static void new_note(void) {
    for (;;) {
        char name[NAME_MAX_LEN];
        int p = 0;
        p = str_append(name, p, "untitled_");
        p = str_append_uint(name, p, (unsigned)g_untitled_n);
        p = str_append(name, p, ".txt");
        name[p] = '\0';

        int exists = 0;
        for (int i = 0; i < g_note_count; i++) {
            if (streq(g_notes[i].name, name)) { exists = 1; break; }
        }
        if (!exists) {
            str_copy(g_filename, name, NAME_MAX_LEN);
            g_untitled_n++;
            break;
        }
        g_untitled_n++;
    }

    g_buf[0] = '\0';
    g_buf_len = 0;
    g_cursor_x = 0;
    g_cursor_y = 0;
    g_scroll_y = 0;
    g_total_lines = 1;
    g_modified = 0;

    save_note();
}

static void delete_note(void) {
    if (!g_filename[0]) return;
    char path[128];
    build_path(path, sizeof(path), g_filename);
    sys_unlink(path);

    refresh_notes();
    if (g_note_count > 0) {
        if (g_note_sel >= g_note_count) g_note_sel = g_note_count - 1;
        load_note(g_note_sel);
    } else {
        g_note_sel = -1;
        g_filename[0] = '\0';
        g_buf_len = 0;
        g_buf[0] = '\0';
        g_cursor_x = 0;
        g_cursor_y = 0;
        g_scroll_y = 0;
        g_modified = 0;
        g_total_lines = 1;
    }
}

static void do_rename(const char *new_name) {
    if (!g_filename[0] || !new_name[0]) return;
    if (streq(g_filename, new_name)) return;

    char old_path[128], new_path[128];
    build_path(old_path, sizeof(old_path), g_filename);
    build_path(new_path, sizeof(new_path), new_name);

    long fd = sys_open(new_path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0) return;
    if (g_buf_len > 0)
        sys_write_fd((int)fd, g_buf, (size_t)g_buf_len);
    sys_close((int)fd);

    sys_unlink(old_path);
    str_copy(g_filename, new_name, NAME_MAX_LEN);
    g_modified = 0;

    refresh_notes();
    for (int i = 0; i < g_note_count; i++) {
        if (streq(g_notes[i].name, g_filename)) { g_note_sel = i; break; }
    }
    ensure_sidebar_visible();
}

/* ---- drawing --------------------------------------------------- */

static void draw_btn(int fd, int x, int y, int w, int h,
                     const char *label, uint32_t bg) {
    sys_gui_fill(fd, x, y, w, h, bg);
    sys_gui_fill(fd, x, y, w, 1, COL_BTN_BORDER);
    sys_gui_fill(fd, x, y + h - 1, w, 1, COL_BTN_BORDER);
    sys_gui_fill(fd, x, y, 1, h, COL_BTN_BORDER);
    sys_gui_fill(fd, x + w - 1, y, 1, h, COL_BTN_BORDER);
    int tx = x + (w - (int)my_strlen(label) * CELL_W) / 2;
    sys_gui_text(fd, tx, y + (h - 8) / 2, label, COL_TEXT, bg);
}

static int hit_in(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void redraw(int fd) {
    sys_gui_fill(fd, 0, 0, WIN_W, WIN_H, COL_BG);

    /* ---- Toolbar ---- */
    sys_gui_fill(fd, 0, 0, WIN_W, TOOLBAR_H, COL_TOOLBAR);
    sys_gui_fill(fd, 0, TOOLBAR_H - 1, WIN_W, 1, COL_BTN_BORDER);

    draw_btn(fd,   6, 5, 40, 24, "New",    COL_BTN_BG);
    draw_btn(fd,  50, 5, 44, 24, "Save",   COL_BTN_BG);
    draw_btn(fd,  98, 5, 52, 24, "Delete", 0x003D2020u);
    draw_btn(fd, 154, 5, 56, 24, "Rename", COL_BTN_BG);

    if (g_filename[0]) {
        int nlen = (int)my_strlen(g_filename);
        int nx = WIN_W - nlen * CELL_W - 8;
        if (nx < 220) nx = 220;
        sys_gui_text(fd, nx, 13, g_filename, COL_ACCENT, COL_TOOLBAR);
    }

    /* ---- Sidebar ---- */
    sys_gui_fill(fd, 0, SIDEBAR_Y, SIDEBAR_W, SIDEBAR_H, COL_SIDEBAR);
    sys_gui_fill(fd, SIDEBAR_W - 1, SIDEBAR_Y, 1, SIDEBAR_H, COL_BTN_BORDER);

    if (g_note_count == 0) {
        sys_gui_text(fd, 8, SIDEBAR_Y + 8, "[empty]", COL_DIM, COL_SIDEBAR);
    } else {
        for (int vi = 0; vi < SIDEBAR_VISIBLE; vi++) {
            int idx = g_sidebar_scroll + vi;
            if (idx >= g_note_count) break;
            int ry = SIDEBAR_Y + 2 + vi * SIDEBAR_ROW_H;
            int is_sel = (idx == g_note_sel);
            uint32_t rbg = is_sel ? COL_SEL_BG : COL_SIDEBAR;
            sys_gui_fill(fd, 2, ry, SIDEBAR_W - 4, SIDEBAR_ROW_H, rbg);
            if (is_sel)
                sys_gui_fill(fd, 2, ry, 3, SIDEBAR_ROW_H, COL_SEL_MARK);

            char disp[20];
            int dl = 0;
            const char *nm = g_notes[idx].name;
            while (nm[dl] && dl < 18) { disp[dl] = nm[dl]; dl++; }
            disp[dl] = '\0';
            sys_gui_text(fd, 10, ry + 6, disp,
                         is_sel ? COL_TEXT : COL_DIM, rbg);
        }

        if (g_note_count > SIDEBAR_VISIBLE) {
            int sb_h = SIDEBAR_H;
            int thumb_h = (SIDEBAR_VISIBLE * sb_h) / g_note_count;
            if (thumb_h < 10) thumb_h = 10;
            int max_sc = g_note_count - SIDEBAR_VISIBLE;
            int thumb_y = SIDEBAR_Y;
            if (max_sc > 0)
                thumb_y += (g_sidebar_scroll * (sb_h - thumb_h)) / max_sc;
            sys_gui_fill(fd, SIDEBAR_W - 6, thumb_y, 4, thumb_h, COL_ACCENT);
        }
    }

    /* ---- Editor pane ---- */
    sys_gui_fill(fd, EDITOR_X, EDITOR_Y, EDITOR_W, EDITOR_H, COL_EDITOR);
    sys_gui_fill(fd, EDITOR_X, EDITOR_Y, GUTTER_W, EDITOR_H, COL_GUTTER_BG);
    sys_gui_fill(fd, EDITOR_X + GUTTER_W, EDITOR_Y, 1, EDITOR_H, COL_BTN_BORDER);

    if (g_filename[0]) {
        for (int vi = 0; vi < VISIBLE_ROWS; vi++) {
            int row = g_scroll_y + vi;
            if (row >= g_total_lines) break;
            int y = EDITOR_Y + vi * LINE_H;
            int is_cur = (row == g_cursor_y);

            if (is_cur) {
                sys_gui_fill(fd, EDITOR_X + GUTTER_W + 1, y,
                             EDITOR_W - GUTTER_W - 1, LINE_H, COL_CURLINE);
                sys_gui_fill(fd, EDITOR_X, y, GUTTER_W, LINE_H, COL_CURLINE);
            }

            char lnum[8];
            fmt_uint(lnum, (unsigned)(row + 1));
            int lnum_len = (int)my_strlen(lnum);
            int lnum_x = EDITOR_X + GUTTER_W - (lnum_len + 1) * CELL_W;
            if (lnum_x < EDITOR_X + 2) lnum_x = EDITOR_X + 2;
            uint32_t gbg = is_cur ? COL_CURLINE : COL_GUTTER_BG;
            sys_gui_text(fd, lnum_x, y + 1, lnum,
                         is_cur ? COL_ACCENT : COL_LINENUM, gbg);

            int ls = line_start(row);
            int ll = line_len(row);
            if (ll > 0) {
                int dlen = ll;
                if (dlen > VISIBLE_COLS) dlen = VISIBLE_COLS;
                char ld[52];
                my_memcpy(ld, g_buf + ls, (size_t)dlen);
                ld[dlen] = '\0';
                uint32_t lbg = is_cur ? COL_CURLINE : COL_EDITOR;
                sys_gui_text(fd, TEXT_X, y + 1, ld, COL_TEXT, lbg);
            }

            if (is_cur) {
                int cx = TEXT_X + g_cursor_x * CELL_W;
                if (cx < WIN_W - 4)
                    sys_gui_fill(fd, cx, y + 1, 2, CELL_H, COL_CURSOR_BAR);
            }
        }

        if (g_total_lines > VISIBLE_ROWS) {
            int sb_x = WIN_W - 6;
            sys_gui_fill(fd, sb_x, EDITOR_Y, 4, EDITOR_H, 0x00252536u);
            int thumb_h = (VISIBLE_ROWS * EDITOR_H) / g_total_lines;
            if (thumb_h < 8) thumb_h = 8;
            int max_sc = g_total_lines - VISIBLE_ROWS;
            int thumb_y = EDITOR_Y;
            if (max_sc > 0)
                thumb_y += (g_scroll_y * (EDITOR_H - thumb_h)) / max_sc;
            sys_gui_fill(fd, sb_x, thumb_y, 4, thumb_h, COL_ACCENT);
        }
    } else {
        sys_gui_text(fd, EDITOR_X + 40, EDITOR_Y + 60,
                     "Select or create a note", COL_DIM, COL_EDITOR);
    }

    /* ---- Status bar ---- */
    sys_gui_fill(fd, 0, STATUS_Y, WIN_W, STATUSBAR_H, COL_STATUS);
    sys_gui_fill(fd, 0, STATUS_Y, WIN_W, 1, COL_BTN_BORDER);

    char st[128];
    int sp = 0;
    sp = str_append(st, sp, " Line ");
    sp = str_append_uint(st, sp, (unsigned)(g_cursor_y + 1));
    sp = str_append(st, sp, ", Col ");
    sp = str_append_uint(st, sp, (unsigned)(g_cursor_x + 1));
    if (g_modified)
        sp = str_append(st, sp, "  | Modified");
    sp = str_append(st, sp, "  | ");
    sp = str_append_uint(st, sp, (unsigned)g_note_count);
    sp = str_append(st, sp, " notes");
    st[sp] = '\0';
    sys_gui_text(fd, 4, STATUS_Y + 6, st,
                 g_modified ? COL_MODIFIED : COL_DIM, COL_STATUS);

    /* ---- Dialog overlays ---- */
    if (g_dialog_mode == 1) {
        int dx = WIN_W / 2 - 140, dy = WIN_H / 2 - 40;
        sys_gui_fill(fd, dx, dy, 280, 80, 0x00202040u);
        sys_gui_fill(fd, dx, dy,      280, 2, COL_ACCENT);
        sys_gui_fill(fd, dx, dy + 78, 280, 2, COL_ACCENT);
        sys_gui_fill(fd, dx, dy,        2, 80, COL_ACCENT);
        sys_gui_fill(fd, dx + 278, dy,  2, 80, COL_ACCENT);
        sys_gui_text(fd, dx + 10, dy + 10, "New name:", COL_ACCENT, 0x00202040u);
        char inp[68];
        int il = 0;
        for (int i = 0; i < g_input_len && il < 60; i++) inp[il++] = g_input_buf[i];
        inp[il++] = '_';
        inp[il] = '\0';
        sys_gui_text(fd, dx + 10, dy + 34, inp, COL_TEXT, 0x00202040u);
        sys_gui_text(fd, dx + 10, dy + 56, "Enter=OK  Esc=Cancel",
                     COL_DIM, 0x00202040u);
    } else if (g_dialog_mode == 2) {
        int dx = WIN_W / 2 - 140, dy = WIN_H / 2 - 40;
        sys_gui_fill(fd, dx, dy, 280, 80, 0x003A2020u);
        sys_gui_fill(fd, dx, dy,      280, 2, COL_DANGER);
        sys_gui_fill(fd, dx, dy + 78, 280, 2, COL_DANGER);
        sys_gui_fill(fd, dx, dy,        2, 80, COL_DANGER);
        sys_gui_fill(fd, dx + 278, dy,  2, 80, COL_DANGER);
        sys_gui_text(fd, dx + 10, dy + 10, "Delete this note?",
                     COL_DANGER, 0x003A2020u);
        sys_gui_text(fd, dx + 10, dy + 30, g_filename, COL_TEXT, 0x003A2020u);
        sys_gui_text(fd, dx + 10, dy + 56, "y=Yes  n/Esc=Cancel",
                     COL_DIM, 0x003A2020u);
    }

    sys_gui_flip(fd);
}

/* ---- main ------------------------------------------------------ */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    sys_mkdir("/data", 0);
    sys_mkdir(NOTES_DIR, 0);

    int fd = sys_gui_create(WIN_W, WIN_H, "Notes");
    if (fd < 0) return 1;

    refresh_notes();
    if (g_note_count > 0)
        load_note(0);

    redraw(fd);

    for (;;) {
        struct gui_event ev;
        int got = sys_gui_poll_event(fd, &ev);
        if (got < 0) return 1;
        if (got == 0) { sys_yield(); continue; }
        if (ev.type == GUI_EV_CLOSE) return 0;

        /* ---- Rename dialog ---- */
        if (g_dialog_mode == 1) {
            if (ev.type == GUI_EV_KEY) {
                if (ev.key == '\n' || ev.key == '\r') {
                    g_input_buf[g_input_len] = '\0';
                    if (g_input_len > 0) do_rename(g_input_buf);
                    g_dialog_mode = 0;
                } else if (ev.key == 27) {
                    g_dialog_mode = 0;
                } else if (ev.key == '\b' || ev.key == 127) {
                    if (g_input_len > 0) g_input_len--;
                } else if (ev.key >= 0x20 && ev.key <= 0x7E &&
                           g_input_len < NAME_MAX_LEN - 2) {
                    g_input_buf[g_input_len++] = (char)ev.key;
                }
            }
            redraw(fd); continue;
        }

        /* ---- Delete confirmation ---- */
        if (g_dialog_mode == 2) {
            if (ev.type == GUI_EV_KEY) {
                if (ev.key == 'y' || ev.key == 'Y') {
                    delete_note();
                    g_dialog_mode = 0;
                } else if (ev.key == 'n' || ev.key == 'N' || ev.key == 27) {
                    g_dialog_mode = 0;
                }
            }
            redraw(fd); continue;
        }

        /* ---- Mouse ---- */
        if (ev.type == GUI_EV_MOUSE_DOWN && ev.button) {
            if (hit_in(ev.x, ev.y, 6, 5, 40, 24)) {
                new_note(); redraw(fd); continue;
            }
            if (hit_in(ev.x, ev.y, 50, 5, 44, 24)) {
                save_note(); redraw(fd); continue;
            }
            if (hit_in(ev.x, ev.y, 98, 5, 52, 24) && g_filename[0]) {
                g_dialog_mode = 2; redraw(fd); continue;
            }
            if (hit_in(ev.x, ev.y, 154, 5, 56, 24) && g_filename[0]) {
                str_copy(g_input_buf, g_filename, NAME_MAX_LEN);
                g_input_len = (int)my_strlen(g_input_buf);
                g_dialog_mode = 1;
                redraw(fd); continue;
            }

            if (ev.x < SIDEBAR_W && ev.y >= SIDEBAR_Y &&
                ev.y < SIDEBAR_Y + SIDEBAR_H) {
                int rel = ev.y - SIDEBAR_Y - 2;
                if (rel >= 0) {
                    int idx = g_sidebar_scroll + rel / SIDEBAR_ROW_H;
                    if (idx >= 0 && idx < g_note_count)
                        load_note(idx);
                }
                redraw(fd); continue;
            }

            if (ev.x >= TEXT_X && ev.y >= EDITOR_Y &&
                ev.y < EDITOR_Y + EDITOR_H && g_filename[0]) {
                int vy = (ev.y - EDITOR_Y) / LINE_H;
                int row = g_scroll_y + vy;
                if (row >= g_total_lines) row = g_total_lines - 1;
                if (row < 0) row = 0;
                g_cursor_y = row;
                int col = (ev.x - TEXT_X + CELL_W / 2) / CELL_W;
                int ll = line_len(g_cursor_y);
                if (col > ll) col = ll;
                if (col < 0) col = 0;
                g_cursor_x = col;
                redraw(fd); continue;
            }
            continue;
        }

        /* ---- Keyboard (editor) ---- */
        if (ev.type == GUI_EV_KEY && g_filename[0]) {
            uint8_t k = ev.key;
            int need_redraw = 1;

            if (k == KEY_UP) {
                if (g_cursor_y > 0) {
                    g_cursor_y--;
                    clamp_cursor();
                    ensure_visible();
                }
            } else if (k == KEY_DOWN) {
                if (g_cursor_y < g_total_lines - 1) {
                    g_cursor_y++;
                    clamp_cursor();
                    ensure_visible();
                }
            } else if (k == KEY_LEFT) {
                if (g_cursor_x > 0) {
                    g_cursor_x--;
                } else if (g_cursor_y > 0) {
                    g_cursor_y--;
                    g_cursor_x = line_len(g_cursor_y);
                    ensure_visible();
                }
            } else if (k == KEY_RIGHT) {
                int ll = line_len(g_cursor_y);
                if (g_cursor_x < ll) {
                    g_cursor_x++;
                } else if (g_cursor_y < g_total_lines - 1) {
                    g_cursor_y++;
                    g_cursor_x = 0;
                    ensure_visible();
                }
            } else if (k == KEY_HOME) {
                g_cursor_x = 0;
            } else if (k == KEY_END) {
                g_cursor_x = line_len(g_cursor_y);
            } else if (k == '\b' || k == 127) {
                int off = cursor_offset();
                if (off > 0) {
                    if (g_cursor_x > 0) {
                        g_cursor_x--;
                    } else if (g_cursor_y > 0) {
                        g_cursor_y--;
                        g_cursor_x = line_len(g_cursor_y);
                    }
                    buf_delete(off - 1);
                    ensure_visible();
                }
            } else if (k == '\n' || k == '\r') {
                int off = cursor_offset();
                buf_insert(off, '\n');
                g_cursor_y++;
                g_cursor_x = 0;
                ensure_visible();
            } else if (k >= 0x20 && k <= 0x7E) {
                int off = cursor_offset();
                buf_insert(off, (char)k);
                g_cursor_x++;
            } else {
                need_redraw = 0;
            }

            if (need_redraw) redraw(fd);
        }
    }
}
