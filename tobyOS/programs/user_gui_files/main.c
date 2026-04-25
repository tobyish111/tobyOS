/* user_gui_files/main.c -- /bin/gui_files, the GUI file manager.
 *
 * Single-pane list of the current directory. The top bar shows the
 * path; the left column shows [..] plus one row per entry. Clicking a
 * row selects it; clicking the same row twice (or pressing Enter /
 * clicking Open) acts on it:
 *
 *    directory -> navigate into it
 *    .elf      -> launch it via SYS_EXEC(path, NULL)
 *    other     -> open /bin/gui_viewer with the path as argv[1]
 *
 * The "Up" button pops one segment off the path; at "/" it's a no-op.
 *
 * We use raw sys_gui_* + sys_fs_readdir + sys_exec stubs rather than
 * the toby_gui toolkit because the toolkit wants fixed widget rects,
 * and a dynamically-built list of N rows is easier to paint + hit-test
 * ourselves.
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
#define SYS_FS_READDIR     18
#define SYS_EXEC           20

#define SYS_FS_NAME_MAX    64
#define SYS_FS_TYPE_FILE   1
#define SYS_FS_TYPE_DIR    2

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
static inline long sys_fs_readdir(const char *path,
                                  struct vfs_dirent_user *out,
                                  int cap, int offset) {
    long r;
    register long r10 __asm__("r10") = (long)offset;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_FS_READDIR), "D"(path), "S"(out),
          "d"((long)cap), "r"(r10)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_exec(const char *path, const char *arg) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_EXEC), "D"(path), "S"(arg)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny libc ------------------------------------------------ */

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}
static void putstr_console(const char *s) {
    sys_write(1, s, my_strlen(s));
}
static void my_memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
}
static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* Find the dot of a trailing .xxx extension. Returns NULL if no dot
 * in the last name segment (we only look at the basename). */
static const char *find_ext(const char *name) {
    const char *dot = 0;
    for (const char *c = name; *c; c++) {
        if (*c == '.') dot = c;
    }
    return dot;
}

/* ---- layout constants ----------------------------------------- */

#define WIN_W            480
#define WIN_H            360

#define PATH_BAR_Y       4
#define PATH_BAR_H       20

#define BTN_Y            28
#define BTN_H            20
#define BTN_UP_X         4
#define BTN_UP_W         48
#define BTN_OPEN_X       56
#define BTN_OPEN_W       60

#define LIST_X           4
#define LIST_Y           56
#define LIST_W           (WIN_W - 8)
#define LIST_ROW_H       18
#define LIST_ROWS_MAX    16

#define COL_BG           0x00181C24u
#define COL_PATH_BG      0x00282C34u
#define COL_PATH_FG      0x00E0F0FFu
#define COL_BTN_FACE     0x00404858u
#define COL_BTN_BORDER   0x00808890u
#define COL_BTN_TEXT     0x00FFFFFFu
#define COL_ROW_FG       0x00E0E0E0u
#define COL_ROW_BG       0x00181C24u
#define COL_ROW_SEL_BG   0x00405070u
#define COL_STATUS_FG    0x00FFD060u

#define ENTRIES_MAX      64

static struct vfs_dirent_user g_entries[ENTRIES_MAX];
static int                    g_entry_count = 0;

/* Path + status line are owned by main's main loop and static so the
 * helpers can touch them without threading everything through args. */
static char g_path[256]  = "/";
static char g_status[64] = "";
static int  g_selected   = -1;   /* -1 = none */

/* ---- path helpers --------------------------------------------- */

static void path_join(char *dst, size_t cap, const char *dir,
                      const char *leaf) {
    size_t i = 0;
    for (; dir[i] && i + 1 < cap; i++) dst[i] = dir[i];
    /* Add slash if needed. */
    if (i > 0 && dst[i - 1] != '/') {
        if (i + 1 >= cap) { dst[cap - 1] = '\0'; return; }
        dst[i++] = '/';
    }
    for (size_t j = 0; leaf[j] && i + 1 < cap; j++) dst[i++] = leaf[j];
    dst[i] = '\0';
}

/* Pop the last segment of g_path. Root stays root. */
static void path_pop(void) {
    size_t n = my_strlen(g_path);
    if (n <= 1) return;
    if (g_path[n - 1] == '/') g_path[--n] = '\0';
    while (n > 0 && g_path[n - 1] != '/') g_path[--n] = '\0';
    if (n == 0) { g_path[0] = '/'; g_path[1] = '\0'; return; }
    if (n > 1 && g_path[n - 1] == '/') g_path[n - 1] = '\0';
}

/* ---- directory listing --------------------------------------- */

static void refresh_listing(void) {
    my_memset(g_entries, 0, sizeof(g_entries));
    g_entry_count = 0;
    g_selected = -1;

    long n = sys_fs_readdir(g_path, g_entries, ENTRIES_MAX, 0);
    if (n < 0) {
        /* No such directory / mount. Put a synthetic "(read error)"
         * pseudo-entry so the user sees what happened rather than an
         * empty pane. */
        g_entry_count = 0;
        /* Build status message inline (no kprintf-equivalent). */
        const char *msg = "<read error>";
        size_t i = 0;
        while (msg[i] && i + 1 < sizeof(g_status)) {
            g_status[i] = msg[i]; i++;
        }
        g_status[i] = '\0';
        return;
    }
    g_entry_count = (int)n;
    g_status[0] = '\0';
}

/* ---- drawing -------------------------------------------------- */

static void draw_border(int fd, int x, int y, int w, int h, uint32_t c) {
    sys_gui_fill(fd, x,         y,         w, 1, c);
    sys_gui_fill(fd, x,         y + h - 1, w, 1, c);
    sys_gui_fill(fd, x,         y,         1, h, c);
    sys_gui_fill(fd, x + w - 1, y,         1, h, c);
}
static void draw_button(int fd, int x, int y, int w, int h,
                        const char *label) {
    sys_gui_fill(fd, x, y, w, h, COL_BTN_FACE);
    draw_border(fd, x, y, w, h, COL_BTN_BORDER);
    int tx = x + (w - (int)my_strlen(label) * 8) / 2;
    if (tx < x + 2) tx = x + 2;
    int ty = y + (h - 8) / 2;
    sys_gui_text(fd, tx, ty, label, COL_BTN_TEXT, COL_BTN_FACE);
}

static void draw_row(int fd, int row_idx, int y, int is_sel) {
    uint32_t bg = is_sel ? COL_ROW_SEL_BG : COL_ROW_BG;
    sys_gui_fill(fd, LIST_X, y, LIST_W, LIST_ROW_H, bg);

    if (row_idx < 0) {
        sys_gui_text(fd, LIST_X + 4, y + 5, "[..]  parent directory",
                     COL_ROW_FG, bg);
        return;
    }
    if (row_idx >= g_entry_count) return;

    const struct vfs_dirent_user *e = &g_entries[row_idx];
    const char *tag = (e->type == SYS_FS_TYPE_DIR) ? "[D] " : "[F] ";
    /* Compose "[T] name" in one line. Cap the displayed name to fit
     * the row width so truncation is silent-but-visible. */
    char line[80];
    size_t off = 0;
    for (size_t i = 0; tag[i]; i++) line[off++] = tag[i];
    for (size_t i = 0; e->name[i] && off + 1 < sizeof(line) - 1; i++) {
        line[off++] = e->name[i];
    }
    line[off] = '\0';
    sys_gui_text(fd, LIST_X + 4, y + 5, line, COL_ROW_FG, bg);
}

static void redraw(int fd) {
    sys_gui_fill(fd, 0, 0, WIN_W, WIN_H, COL_BG);

    /* Path bar. */
    sys_gui_fill(fd, 4, PATH_BAR_Y, WIN_W - 8, PATH_BAR_H, COL_PATH_BG);
    draw_border(fd, 4, PATH_BAR_Y, WIN_W - 8, PATH_BAR_H, COL_BTN_BORDER);
    sys_gui_text(fd, 8, PATH_BAR_Y + 6, g_path, COL_PATH_FG, COL_PATH_BG);

    /* Buttons. */
    draw_button(fd, BTN_UP_X,   BTN_Y, BTN_UP_W,   BTN_H, "Up");
    draw_button(fd, BTN_OPEN_X, BTN_Y, BTN_OPEN_W, BTN_H, "Open");

    /* Status (right of the buttons) -- shows "<read error>", "launched
     * /bin/hello", etc. Short-lived; overwritten on next action. */
    if (g_status[0]) {
        sys_gui_text(fd, BTN_OPEN_X + BTN_OPEN_W + 8,
                     BTN_Y + (BTN_H - 8) / 2, g_status,
                     COL_STATUS_FG, COL_BG);
    }

    /* List. Row -1 is the synthetic parent pseudo-row; rows 0..n-1
     * are real directory entries. */
    int y = LIST_Y;
    draw_row(fd, -1, y, g_selected == -1 ? 0 : 0);
    /* The parent row is special -- always drawn, never selectable with
     * the up/down convention; clicking it navigates immediately. */
    y += LIST_ROW_H;
    for (int i = 0; i < g_entry_count && i < LIST_ROWS_MAX - 1; i++) {
        draw_row(fd, i, y, i == g_selected);
        y += LIST_ROW_H;
    }

    sys_gui_flip(fd);
}

/* ---- open action --------------------------------------------- */

static void set_status(const char *s) {
    size_t i = 0;
    while (s[i] && i + 1 < sizeof(g_status)) { g_status[i] = s[i]; i++; }
    g_status[i] = '\0';
}

/* Returns true if the item was opened "navigationally" (i.e. changed
 * cwd), so the caller knows to reload the listing. */
static int open_entry(int idx) {
    if (idx < 0) { path_pop(); return 1; }
    if (idx >= g_entry_count) return 0;
    const struct vfs_dirent_user *e = &g_entries[idx];

    if (e->type == SYS_FS_TYPE_DIR) {
        char next[256];
        path_join(next, sizeof(next), g_path, e->name);
        /* Copy back into g_path. */
        size_t i = 0;
        for (; next[i] && i + 1 < sizeof(g_path); i++) g_path[i] = next[i];
        g_path[i] = '\0';
        return 1;
    }

    /* File. Build an absolute path. */
    char full[256];
    path_join(full, sizeof(full), g_path, e->name);

    const char *ext = find_ext(e->name);
    if (ext && streq(ext, ".elf")) {
        long rc = sys_exec(full, 0);
        set_status(rc == 0 ? "launched" : "exec failed");
        return 0;
    }
    /* Anything else -> open in the viewer. */
    long rc = sys_exec("/bin/gui_viewer", full);
    set_status(rc == 0 ? "opened in viewer" : "exec failed");
    return 0;
}

/* ---- hit-testing ---------------------------------------------- */

static int hit_row(int mx, int my) {
    if (mx < LIST_X || mx >= LIST_X + LIST_W) return -2;
    int rel = my - LIST_Y;
    if (rel < 0) return -2;
    int row = rel / LIST_ROW_H;
    if (row == 0) return -1;              /* parent row */
    int idx = row - 1;
    if (idx >= g_entry_count || idx >= LIST_ROWS_MAX - 1) return -2;
    return idx;
}

static int hit_in(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

/* ---- main ---------------------------------------------------- */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fd = sys_gui_create(WIN_W, WIN_H, "Files");
    if (fd < 0) {
        putstr_console("gui_files: sys_gui_create failed\n");
        return 1;
    }
    refresh_listing();
    redraw(fd);

    for (;;) {
        struct gui_event ev;
        int got = sys_gui_poll_event(fd, &ev);
        if (got < 0) return 1;
        if (got == 0) { sys_yield(); continue; }

        if (ev.type == GUI_EV_MOUSE_DOWN && ev.button) {
            /* Up button: pop path + reload. */
            if (hit_in(ev.x, ev.y, BTN_UP_X, BTN_Y, BTN_UP_W, BTN_H)) {
                path_pop();
                refresh_listing();
                redraw(fd);
                continue;
            }
            /* Open button: open the current selection. */
            if (hit_in(ev.x, ev.y, BTN_OPEN_X, BTN_Y,
                       BTN_OPEN_W, BTN_H)) {
                if (g_selected == -2) { redraw(fd); continue; }
                int navigated = open_entry(g_selected);
                if (navigated) refresh_listing();
                redraw(fd);
                continue;
            }
            /* List row. */
            int row = hit_row(ev.x, ev.y);
            if (row == -2) {
                if (g_selected != -1) { g_selected = -1; redraw(fd); }
                continue;
            }
            if (row == -1) {
                /* Parent row -> navigate immediately. */
                path_pop();
                refresh_listing();
                redraw(fd);
                continue;
            }
            /* Click on a row: select first, open on second click. */
            if (g_selected == row) {
                int navigated = open_entry(row);
                if (navigated) refresh_listing();
            } else {
                g_selected = row;
            }
            redraw(fd);
            continue;
        }
        /* Enter activates the current selection too (UX parity with
         * the toolkit widgets). */
        if (ev.type == GUI_EV_KEY && ev.key == '\n') {
            if (g_selected >= 0) {
                int navigated = open_entry(g_selected);
                if (navigated) refresh_listing();
                redraw(fd);
            }
        }
    }
}
