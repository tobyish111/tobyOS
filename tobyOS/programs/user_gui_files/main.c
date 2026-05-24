/* user_gui_files/main.c -- /bin/gui_files, the GUI file manager.
 *
 * Windows Explorer-like file manager with:
 *   - Directory navigation (enter, back, up, sidebar shortcuts)
 *   - File operations (copy, paste, delete, new folder)
 *   - File type icons and size display
 *   - Detail/preview panel for selected file
 *   - Toolbar with action buttons
 *   - Keyboard + mouse driven
 *
 * Keyboard:
 *   j/k      - Move selection down/up
 *   Enter/l  - Open selected (dir=navigate, elf=launch, other=viewer)
 *   Backspace/h - Go up one directory
 *   g/G      - Jump to top/bottom
 *   d        - Delete selected file (with confirmation)
 *   c        - Copy selected file path to clipboard
 *   p        - Paste (copy file from clipboard to current dir)
 *   n        - New folder
 *   r        - Refresh listing
 *   q        - Quit
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;

#define SYS_WRITE           1
#define SYS_READ            2
#define SYS_EXIT            0
#define SYS_CLOSE           4
#define SYS_YIELD           5
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_FS_READDIR     18
#define SYS_FS_READFILE    19
#define SYS_EXEC           20
#define SYS_OPEN           35
#define SYS_UNLINK         41
#define SYS_MKDIR          42

#define O_RDONLY    0x0
#define O_WRONLY    0x1
#define O_CREAT     0x040
#define O_TRUNC     0x200

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
#define GUI_EV_CLOSE        5

/* ---- syscall stubs --------------------------------------------- */

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
static inline long sys_fs_readfile(const char *path, void *out, size_t cap) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_FS_READFILE), "D"(path), "S"(out), "d"(cap)
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
static inline long sys_open(const char *path, int flags, int mode) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_OPEN), "D"(path), "S"((long)flags), "d"((long)mode)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_close(int fd2) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_CLOSE), "D"((long)fd2)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_unlink(const char *path) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_UNLINK), "D"(path)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_mkdir(const char *path, int mode) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_MKDIR), "D"(path), "S"((long)mode)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_write_to(int fd2, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd2), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny libc ------------------------------------------------ */

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
static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}
static int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static void str_copy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static const char *find_ext(const char *name) {
    const char *dot = 0;
    for (const char *c = name; *c; c++) if (*c == '.') dot = c;
    return dot;
}

static void fmt_size(char *buf, size_t cap, uint32_t bytes) {
    if (bytes < 1024) {
        int i = 0;
        if (bytes == 0) { buf[0] = '0'; i = 1; }
        else {
            char tmp[12]; int n = 0;
            uint32_t v = bytes;
            while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
            for (int j = n - 1; j >= 0 && (size_t)i + 1 < cap; j--) buf[i++] = tmp[j];
        }
        if ((size_t)i + 2 < cap) { buf[i++] = ' '; buf[i++] = 'B'; }
        buf[i] = '\0';
        return;
    }
    if (bytes < 1024 * 1024) {
        uint32_t kb = bytes / 1024;
        int i = 0;
        char tmp[12]; int n = 0;
        uint32_t v = kb;
        while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
        for (int j = n - 1; j >= 0 && (size_t)i + 1 < cap; j--) buf[i++] = tmp[j];
        if ((size_t)i + 3 < cap) { buf[i++] = ' '; buf[i++] = 'K'; buf[i++] = 'B'; }
        buf[i] = '\0';
        return;
    }
    uint32_t mb = bytes / (1024 * 1024);
    int i = 0;
    char tmp[12]; int n = 0;
    uint32_t v = mb;
    if (v == 0) { buf[0] = '0'; i = 1; }
    else { while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
           for (int j = n - 1; j >= 0 && (size_t)i + 1 < cap; j--) buf[i++] = tmp[j]; }
    if ((size_t)i + 3 < cap) { buf[i++] = ' '; buf[i++] = 'M'; buf[i++] = 'B'; }
    buf[i] = '\0';
}

/* ---- layout constants ----------------------------------------- */

#define WIN_W            760
#define WIN_H            480

#define TOOLBAR_Y        0
#define TOOLBAR_H        36
#define PATHBAR_Y        36
#define PATHBAR_H        26

#define SIDEBAR_X        0
#define SIDEBAR_Y        62
#define SIDEBAR_W        150
#define SIDEBAR_H        (WIN_H - SIDEBAR_Y - 22)

#define LIST_X           152
#define LIST_Y           62
#define LIST_W           (WIN_W - LIST_X - 12)
#define LIST_ROW_H       20
#define LIST_VISIBLE     ((WIN_H - LIST_Y - 22) / LIST_ROW_H)

#define STATUSBAR_Y      (WIN_H - 22)
#define STATUSBAR_H      22

/* ---- Colors (dark Explorer theme) ----------------------------- */

#define COL_BG           0x001E1E2Eu
#define COL_TOOLBAR_BG   0x002D2D3Du
#define COL_PATHBAR_BG   0x001A1A2Au
#define COL_SIDEBAR_BG   0x00252536u
#define COL_LIST_BG      0x001E1E2Eu
#define COL_STATUS_BG    0x002D2D3Du

#define COL_TEXT         0x00E0E0E0u
#define COL_TEXT_DIM     0x00808890u
#define COL_ACCENT       0x004FC3F7u
#define COL_ACCENT2      0x00FFB74Du
#define COL_DANGER       0x00EF5350u

#define COL_BTN_BG       0x003D3D4Du
#define COL_BTN_HOVER    0x004D4D5Du
#define COL_BTN_TEXT     0x00E0E0E0u
#define COL_BTN_BORDER   0x005D5D6Du

#define COL_ROW_BG0      0x001E1E2Eu
#define COL_ROW_BG1      0x00242434u
#define COL_ROW_SEL      0x00264F78u
#define COL_ROW_SEL_BD   0x004FC3F7u
#define COL_DIR_FG       0x004FC3F7u
#define COL_FILE_FG      0x00E0E0E0u
#define COL_ELF_FG       0x0081C784u
#define COL_SIZE_FG      0x00808890u
#define COL_ICON_FG      0x00FFB74Du

#define COL_SIDEBAR_SEL  0x00353546u
#define COL_SIDEBAR_MARK 0x004FC3F7u
#define COL_SCROLLBAR_BG 0x002D2D3Du
#define COL_SCROLL_THUMB 0x004FC3F7u

#define COL_CONFIRM_BG   0x003A2020u
#define COL_CONFIRM_BD   0x00EF5350u

/* ---- State ---------------------------------------------------- */

#define ENTRIES_MAX      128
#define TOTAL_ROWS       (g_entry_count + 1)

static struct vfs_dirent_user g_entries[ENTRIES_MAX];
static int  g_entry_count = 0;

static char g_path[256]  = "/";
static char g_status[80] = "";
static int  g_selected   = 0;
static int  g_scroll     = 0;

/* Clipboard for copy/paste */
static char g_clipboard[256] = "";
static int  g_clip_is_file = 0;

/* Confirmation dialog */
static int  g_confirm_mode = 0;  /* 0=none, 1=delete, 2=new folder input */
static char g_input_buf[64] = "";
static int  g_input_len = 0;

/* Sidebar items and their paths */
static const char *g_sidebar_labels[] = {
    "Home", "Root", "Binaries", "Config", "Libraries",
    "Repository", "System"
};
static const char *g_sidebar_paths[] = {
    "/", "/", "/bin", "/etc", "/lib",
    "/repo", "/system"
};
#define SIDEBAR_COUNT 7
static int g_sidebar_sel = 1;

/* ---- path helpers --------------------------------------------- */

static void path_join(char *dst, size_t cap, const char *dir, const char *leaf) {
    size_t i = 0;
    for (; dir[i] && i + 1 < cap; i++) dst[i] = dir[i];
    if (i > 0 && dst[i - 1] != '/') {
        if (i + 1 >= cap) { dst[cap - 1] = '\0'; return; }
        dst[i++] = '/';
    }
    for (size_t j = 0; leaf[j] && i + 1 < cap; j++) dst[i++] = leaf[j];
    dst[i] = '\0';
}

static void path_pop(void) {
    size_t n = my_strlen(g_path);
    if (n <= 1) return;
    if (g_path[n - 1] == '/') g_path[--n] = '\0';
    while (n > 0 && g_path[n - 1] != '/') g_path[--n] = '\0';
    if (n == 0) { g_path[0] = '/'; g_path[1] = '\0'; return; }
    if (n > 1 && g_path[n - 1] == '/') g_path[n - 1] = '\0';
}

/* Get just the filename from a path */
static const char *path_basename(const char *path) {
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && *(p+1)) last = p + 1;
    return last;
}

/* ---- sorting -------------------------------------------------- */

static void sort_entries(void) {
    for (int i = 1; i < g_entry_count; i++) {
        struct vfs_dirent_user tmp;
        my_memcpy(&tmp, &g_entries[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0) {
            int a_dir = (g_entries[j].type == SYS_FS_TYPE_DIR);
            int b_dir = (tmp.type == SYS_FS_TYPE_DIR);
            if (a_dir > b_dir) break;
            if (a_dir == b_dir && my_strcmp(g_entries[j].name, tmp.name) <= 0) break;
            my_memcpy(&g_entries[j + 1], &g_entries[j], sizeof(tmp));
            j--;
        }
        my_memcpy(&g_entries[j + 1], &tmp, sizeof(tmp));
    }
}

/* ---- directory listing --------------------------------------- */

static void refresh_listing(void) {
    my_memset(g_entries, 0, sizeof(g_entries));
    g_entry_count = 0;
    g_selected = 0;
    g_scroll = 0;

    long n = sys_fs_readdir(g_path, g_entries, ENTRIES_MAX, 0);
    if (n < 0) {
        g_entry_count = 0;
        str_copy(g_status, "Error reading directory", sizeof(g_status));
        return;
    }
    g_entry_count = (int)n;
    sort_entries();

    /* Update sidebar selection */
    for (int i = 0; i < SIDEBAR_COUNT; i++) {
        if (streq(g_path, g_sidebar_paths[i])) { g_sidebar_sel = i; break; }
    }

    /* Build status */
    int dirs = 0, files = 0;
    for (int i = 0; i < g_entry_count; i++) {
        if (g_entries[i].type == SYS_FS_TYPE_DIR) dirs++;
        else files++;
    }
    char *p = g_status;
    int pos = 0;
    /* dirs count */
    char tmp[12]; int tn = 0;
    uint32_t v = (uint32_t)dirs;
    if (v == 0) tmp[tn++] = '0';
    else while (v > 0) { tmp[tn++] = (char)('0' + v%10); v /= 10; }
    for (int j = tn-1; j >= 0; j--) p[pos++] = tmp[j];
    const char *s1 = " folders, ";
    for (int i = 0; s1[i]; i++) p[pos++] = s1[i];
    /* files count */
    tn = 0; v = (uint32_t)files;
    if (v == 0) tmp[tn++] = '0';
    else while (v > 0) { tmp[tn++] = (char)('0' + v%10); v /= 10; }
    for (int j = tn-1; j >= 0; j--) p[pos++] = tmp[j];
    const char *s2 = " files";
    for (int i = 0; s2[i]; i++) p[pos++] = s2[i];
    p[pos] = '\0';
}

/* ---- scroll management ---------------------------------------- */

static void ensure_visible(void) {
    if (g_selected < g_scroll) g_scroll = g_selected;
    if (g_selected >= g_scroll + LIST_VISIBLE)
        g_scroll = g_selected - LIST_VISIBLE + 1;
    if (g_scroll < 0) g_scroll = 0;
}

/* ---- file type icon/label ------------------------------------- */

static const char *file_icon(const struct vfs_dirent_user *e) {
    if (e->type == SYS_FS_TYPE_DIR) return "[D]";
    const char *ext = find_ext(e->name);
    if (!ext) return "[ ]";
    if (streq(ext, ".elf")) return "[x]";
    if (streq(ext, ".c") || streq(ext, ".h")) return "[C]";
    if (streq(ext, ".txt") || streq(ext, ".md")) return "[T]";
    if (streq(ext, ".conf") || streq(ext, ".toml")) return "[=]";
    if (streq(ext, ".tpkg")) return "[P]";
    if (streq(ext, ".so") || streq(ext, ".a") || streq(ext, ".o")) return "[L]";
    if (streq(ext, ".tar") || streq(ext, ".img") || streq(ext, ".iso")) return "[#]";
    if (streq(ext, ".db")) return "[D]";
    return "[ ]";
}

static uint32_t file_color(const struct vfs_dirent_user *e) {
    if (e->type == SYS_FS_TYPE_DIR) return COL_DIR_FG;
    const char *ext = find_ext(e->name);
    if (!ext) return COL_FILE_FG;
    if (streq(ext, ".elf")) return COL_ELF_FG;
    if (streq(ext, ".c") || streq(ext, ".h")) return 0x0090CAF9u;
    if (streq(ext, ".tpkg")) return COL_ACCENT2;
    return COL_FILE_FG;
}

/* ---- drawing -------------------------------------------------- */

static void draw_button(int fd, int x, int y, int w, int h,
                        const char *label, uint32_t bg) {
    sys_gui_fill(fd, x, y, w, h, bg);
    sys_gui_fill(fd, x, y, w, 1, COL_BTN_BORDER);
    sys_gui_fill(fd, x, y+h-1, w, 1, COL_BTN_BORDER);
    sys_gui_fill(fd, x, y, 1, h, COL_BTN_BORDER);
    sys_gui_fill(fd, x+w-1, y, 1, h, COL_BTN_BORDER);
    int tx = x + (w - (int)my_strlen(label) * 8) / 2;
    sys_gui_text(fd, tx, y + (h-8)/2, label, COL_BTN_TEXT, bg);
}

static void draw_row(int fd, int row_idx, int y, int is_sel, int alt) {
    uint32_t bg = is_sel ? COL_ROW_SEL : (alt ? COL_ROW_BG1 : COL_ROW_BG0);
    sys_gui_fill(fd, LIST_X, y, LIST_W, LIST_ROW_H, bg);

    if (is_sel) {
        sys_gui_fill(fd, LIST_X, y, 3, LIST_ROW_H, COL_ROW_SEL_BD);
    }

    if (row_idx == 0) {
        sys_gui_text(fd, LIST_X + 8, y + 6, "[^]  ..", COL_TEXT_DIM, bg);
        return;
    }

    int eidx = row_idx - 1;
    if (eidx >= g_entry_count) return;
    const struct vfs_dirent_user *e = &g_entries[eidx];

    /* Icon */
    const char *icon = file_icon(e);
    sys_gui_text(fd, LIST_X + 8, y + 6, icon, COL_ICON_FG, bg);

    /* Filename */
    uint32_t fg = file_color(e);
    sys_gui_text(fd, LIST_X + 38, y + 6, e->name, fg, bg);

    /* Size (right-aligned, files only) */
    if (e->type != SYS_FS_TYPE_DIR) {
        char sizebuf[16];
        fmt_size(sizebuf, sizeof(sizebuf), e->size);
        int sx = LIST_X + LIST_W - (int)my_strlen(sizebuf) * 8 - 8;
        sys_gui_text(fd, sx, y + 6, sizebuf, COL_SIZE_FG, bg);
    } else {
        int sx = LIST_X + LIST_W - 3 * 8 - 8;
        sys_gui_text(fd, sx, y + 6, "DIR", COL_TEXT_DIM, bg);
    }
}

static void redraw(int fd) {
    /* Background */
    sys_gui_fill(fd, 0, 0, WIN_W, WIN_H, COL_BG);

    /* ---- Toolbar ---- */
    sys_gui_fill(fd, 0, TOOLBAR_Y, WIN_W, TOOLBAR_H, COL_TOOLBAR_BG);

    /* Nav buttons */
    draw_button(fd, 6, 6, 30, 24, "<", COL_BTN_BG);
    draw_button(fd, 40, 6, 30, 24, ">", COL_BTN_BG);
    draw_button(fd, 74, 6, 34, 24, "Up", COL_BTN_BG);

    /* Separator */
    sys_gui_fill(fd, 116, 8, 1, 20, COL_BTN_BORDER);

    /* Action buttons */
    draw_button(fd, 124, 6, 50, 24, "Open", COL_BTN_BG);
    draw_button(fd, 178, 6, 50, 24, "Copy", COL_BTN_BG);
    draw_button(fd, 232, 6, 54, 24, "Paste", COL_BTN_BG);
    draw_button(fd, 290, 6, 46, 24, "Del", 0x003D2020u);
    draw_button(fd, 340, 6, 60, 24, "NewDir", COL_BTN_BG);

    /* Separator */
    sys_gui_fill(fd, 408, 8, 1, 20, COL_BTN_BORDER);

    /* Refresh */
    draw_button(fd, 416, 6, 34, 24, "R", COL_BTN_BG);

    /* ---- Path bar ---- */
    sys_gui_fill(fd, 0, PATHBAR_Y, WIN_W, PATHBAR_H, COL_PATHBAR_BG);
    sys_gui_fill(fd, 0, PATHBAR_Y + PATHBAR_H - 1, WIN_W, 1, COL_BTN_BORDER);

    /* Path breadcrumb */
    sys_gui_text(fd, 8, PATHBAR_Y + 9, "Path:", COL_TEXT_DIM, COL_PATHBAR_BG);
    sys_gui_text(fd, 52, PATHBAR_Y + 9, g_path, COL_TEXT, COL_PATHBAR_BG);

    /* ---- Sidebar ---- */
    sys_gui_fill(fd, SIDEBAR_X, SIDEBAR_Y, SIDEBAR_W, SIDEBAR_H, COL_SIDEBAR_BG);
    sys_gui_fill(fd, SIDEBAR_W, SIDEBAR_Y, 1, SIDEBAR_H, COL_BTN_BORDER);

    sys_gui_text(fd, 10, SIDEBAR_Y + 6, "Places", COL_ACCENT, COL_SIDEBAR_BG);

    for (int i = 0; i < SIDEBAR_COUNT; i++) {
        int sy = SIDEBAR_Y + 24 + i * 22;
        uint32_t sbg = (i == g_sidebar_sel) ? COL_SIDEBAR_SEL : COL_SIDEBAR_BG;
        sys_gui_fill(fd, 2, sy, SIDEBAR_W - 4, 20, sbg);
        if (i == g_sidebar_sel)
            sys_gui_fill(fd, 2, sy, 3, 20, COL_SIDEBAR_MARK);
        sys_gui_text(fd, 14, sy + 6, g_sidebar_labels[i],
                     (i == g_sidebar_sel) ? COL_TEXT : COL_TEXT_DIM, sbg);
    }

    /* Clipboard indicator */
    if (g_clipboard[0]) {
        int cy = SIDEBAR_Y + SIDEBAR_H - 30;
        sys_gui_text(fd, 8, cy, "Clipboard:", COL_TEXT_DIM, COL_SIDEBAR_BG);
        const char *bn = path_basename(g_clipboard);
        char clip_disp[18];
        int cl = 0;
        while (bn[cl] && cl < 16) { clip_disp[cl] = bn[cl]; cl++; }
        clip_disp[cl] = '\0';
        sys_gui_text(fd, 8, cy + 12, clip_disp, COL_ACCENT2, COL_SIDEBAR_BG);
    }

    /* ---- Column headers ---- */
    sys_gui_fill(fd, LIST_X, LIST_Y - 1, LIST_W, 1, COL_BTN_BORDER);

    /* ---- File list ---- */
    int total = TOTAL_ROWS;
    int y = LIST_Y;
    for (int vis = 0; vis < LIST_VISIBLE; vis++) {
        int row = g_scroll + vis;
        if (row >= total) break;
        draw_row(fd, row, y, row == g_selected, vis & 1);
        y += LIST_ROW_H;
    }

    /* Scrollbar */
    if (total > LIST_VISIBLE) {
        int sb_x = WIN_W - 10;
        int sb_h = WIN_H - LIST_Y - STATUSBAR_H;
        sys_gui_fill(fd, sb_x, LIST_Y, 8, sb_h, COL_SCROLLBAR_BG);
        int thumb_h = (LIST_VISIBLE * sb_h) / total;
        if (thumb_h < 16) thumb_h = 16;
        int max_scroll = total - LIST_VISIBLE;
        int thumb_y = LIST_Y + (max_scroll > 0 ? (g_scroll * (sb_h - thumb_h)) / max_scroll : 0);
        sys_gui_fill(fd, sb_x + 1, thumb_y, 6, thumb_h, COL_SCROLL_THUMB);
    }

    /* ---- Status bar ---- */
    sys_gui_fill(fd, 0, STATUSBAR_Y, WIN_W, STATUSBAR_H, COL_STATUS_BG);
    sys_gui_fill(fd, 0, STATUSBAR_Y, WIN_W, 1, COL_BTN_BORDER);
    if (g_status[0])
        sys_gui_text(fd, 8, STATUSBAR_Y + 7, g_status, COL_TEXT_DIM, COL_STATUS_BG);

    /* Selected file info on the right side of status */
    if (g_selected > 0 && g_selected - 1 < g_entry_count) {
        const struct vfs_dirent_user *e = &g_entries[g_selected - 1];
        char info[48];
        int ip = 0;
        if (e->type == SYS_FS_TYPE_DIR) {
            const char *d = "Folder";
            while (*d) info[ip++] = *d++;
        } else {
            char sb[16];
            fmt_size(sb, sizeof(sb), e->size);
            int si = 0;
            while (sb[si]) info[ip++] = sb[si++];
        }
        info[ip] = '\0';
        int ix = WIN_W - ip * 8 - 12;
        sys_gui_text(fd, ix, STATUSBAR_Y + 7, info, COL_TEXT, COL_STATUS_BG);
    }

    /* ---- Confirmation dialog overlay ---- */
    if (g_confirm_mode == 1) {
        int dx = WIN_W / 2 - 140, dy = WIN_H / 2 - 40;
        sys_gui_fill(fd, dx, dy, 280, 80, COL_CONFIRM_BG);
        sys_gui_fill(fd, dx, dy, 280, 2, COL_CONFIRM_BD);
        sys_gui_fill(fd, dx, dy+78, 280, 2, COL_CONFIRM_BD);
        sys_gui_fill(fd, dx, dy, 2, 80, COL_CONFIRM_BD);
        sys_gui_fill(fd, dx+278, dy, 2, 80, COL_CONFIRM_BD);
        sys_gui_text(fd, dx+10, dy+10, "Delete this file?", COL_DANGER, COL_CONFIRM_BG);
        if (g_selected > 0 && g_selected-1 < g_entry_count)
            sys_gui_text(fd, dx+10, dy+30, g_entries[g_selected-1].name, COL_TEXT, COL_CONFIRM_BG);
        sys_gui_text(fd, dx+10, dy+56, "y=Yes  n/Esc=Cancel", COL_TEXT_DIM, COL_CONFIRM_BG);
    } else if (g_confirm_mode == 2) {
        int dx = WIN_W / 2 - 140, dy = WIN_H / 2 - 40;
        sys_gui_fill(fd, dx, dy, 280, 80, 0x00202040u);
        sys_gui_fill(fd, dx, dy, 280, 2, COL_ACCENT);
        sys_gui_fill(fd, dx, dy+78, 280, 2, COL_ACCENT);
        sys_gui_fill(fd, dx, dy, 2, 80, COL_ACCENT);
        sys_gui_fill(fd, dx+278, dy, 2, 80, COL_ACCENT);
        sys_gui_text(fd, dx+10, dy+10, "New folder name:", COL_ACCENT, 0x00202040u);
        char disp[48];
        int dl = 0;
        for (int i = 0; i < g_input_len && dl < 40; i++) disp[dl++] = g_input_buf[i];
        disp[dl++] = '_';
        disp[dl] = '\0';
        sys_gui_text(fd, dx+10, dy+34, disp, COL_TEXT, 0x00202040u);
        sys_gui_text(fd, dx+10, dy+56, "Enter=Create  Esc=Cancel", COL_TEXT_DIM, 0x00202040u);
    }

    sys_gui_flip(fd);
}

/* ---- file operations ------------------------------------------ */

static void set_status(const char *s) {
    str_copy(g_status, s, sizeof(g_status));
}

static void do_copy(void) {
    if (g_selected == 0) { set_status("Cannot copy '..'"); return; }
    int idx = g_selected - 1;
    if (idx >= g_entry_count) return;
    path_join(g_clipboard, sizeof(g_clipboard), g_path, g_entries[idx].name);
    g_clip_is_file = (g_entries[idx].type == SYS_FS_TYPE_FILE);
    set_status("Copied to clipboard");
}

static void do_paste(void) {
    if (!g_clipboard[0]) { set_status("Clipboard empty"); return; }
    if (!g_clip_is_file) { set_status("Can only paste files"); return; }

    /* Read source file */
    static char copy_buf[32768];
    long n = sys_fs_readfile(g_clipboard, copy_buf, sizeof(copy_buf));
    if (n < 0) { set_status("Read failed"); return; }

    /* Determine destination */
    const char *bn = path_basename(g_clipboard);
    char dest[256];
    path_join(dest, sizeof(dest), g_path, bn);

    /* Write to destination */
    long fdd = sys_open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fdd < 0) { set_status("Create failed"); return; }
    sys_write_to((int)fdd, copy_buf, (size_t)n);
    sys_close((int)fdd);
    set_status("File pasted");
}

static void do_delete(void) {
    if (g_selected == 0) return;
    int idx = g_selected - 1;
    if (idx >= g_entry_count) return;

    char full[256];
    path_join(full, sizeof(full), g_path, g_entries[idx].name);
    long rc = sys_unlink(full);
    if (rc == 0) set_status("Deleted");
    else set_status("Delete failed");
}

static void do_mkdir(void) {
    if (g_input_len == 0) return;
    g_input_buf[g_input_len] = '\0';
    char full[256];
    path_join(full, sizeof(full), g_path, g_input_buf);
    long rc = sys_mkdir(full, 0);
    if (rc == 0) set_status("Folder created");
    else set_status("mkdir failed");
}

/* ---- open action --------------------------------------------- */

static int open_entry(int row) {
    if (row == 0) { path_pop(); return 1; }
    int idx = row - 1;
    if (idx >= g_entry_count) return 0;
    const struct vfs_dirent_user *e = &g_entries[idx];

    if (e->type == SYS_FS_TYPE_DIR) {
        char next[256];
        path_join(next, sizeof(next), g_path, e->name);
        str_copy(g_path, next, sizeof(g_path));
        return 1;
    }

    char full[256];
    path_join(full, sizeof(full), g_path, e->name);

    const char *ext = find_ext(e->name);
    if (ext && streq(ext, ".elf")) {
        long rc = sys_exec(full, 0);
        set_status(rc == 0 ? "Launched" : "Exec failed");
        return 0;
    }
    long rc = sys_exec("/bin/gui_viewer", full);
    set_status(rc == 0 ? "Opened in viewer" : "Open failed");
    return 0;
}

/* ---- hit testing ---------------------------------------------- */

static int hit_in(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static int hit_row(int mx, int my) {
    if (mx < LIST_X || mx >= LIST_X + LIST_W) return -1;
    int rel = my - LIST_Y;
    if (rel < 0) return -1;
    int vis = rel / LIST_ROW_H;
    if (vis >= LIST_VISIBLE) return -1;
    int row = g_scroll + vis;
    if (row >= TOTAL_ROWS) return -1;
    return row;
}

static int hit_sidebar(int mx, int my) {
    if (mx >= SIDEBAR_W || mx < 0) return -1;
    int rel = my - (SIDEBAR_Y + 24);
    if (rel < 0) return -1;
    int idx = rel / 22;
    if (idx >= SIDEBAR_COUNT) return -1;
    return idx;
}

/* ---- main ---------------------------------------------------- */

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fd = sys_gui_create(WIN_W, WIN_H, "File Explorer");
    if (fd < 0) return 1;

    refresh_listing();
    redraw(fd);

    for (;;) {
        struct gui_event ev;
        int got = sys_gui_poll_event(fd, &ev);
        if (got < 0) return 1;
        if (got == 0) { sys_yield(); continue; }
        if (ev.type == GUI_EV_CLOSE) return 0;

        /* ---- Confirmation dialog input ---- */
        if (g_confirm_mode == 1) {
            if (ev.type == GUI_EV_KEY) {
                if (ev.key == 'y' || ev.key == 'Y') {
                    do_delete();
                    g_confirm_mode = 0;
                    refresh_listing();
                    redraw(fd);
                } else if (ev.key == 'n' || ev.key == 'N' || ev.key == 27) {
                    g_confirm_mode = 0;
                    set_status("Cancelled");
                    redraw(fd);
                }
            }
            continue;
        }
        if (g_confirm_mode == 2) {
            if (ev.type == GUI_EV_KEY) {
                if (ev.key == '\n' || ev.key == '\r') {
                    do_mkdir();
                    g_confirm_mode = 0;
                    refresh_listing();
                    redraw(fd);
                } else if (ev.key == 27) {
                    g_confirm_mode = 0;
                    set_status("Cancelled");
                    redraw(fd);
                } else if (ev.key == '\b' || ev.key == 127) {
                    if (g_input_len > 0) g_input_len--;
                    redraw(fd);
                } else if (ev.key >= 0x20 && ev.key <= 0x7E && g_input_len < 60) {
                    g_input_buf[g_input_len++] = (char)ev.key;
                    redraw(fd);
                }
            }
            continue;
        }

        /* ---- Mouse events ---- */
        if (ev.type == GUI_EV_MOUSE_DOWN && ev.button) {
            /* Toolbar buttons */
            if (hit_in(ev.x, ev.y, 6, 6, 30, 24) || hit_in(ev.x, ev.y, 74, 6, 34, 24)) {
                path_pop(); refresh_listing(); redraw(fd); continue;
            }
            if (hit_in(ev.x, ev.y, 124, 6, 50, 24)) {
                int nav = open_entry(g_selected);
                if (nav) refresh_listing();
                redraw(fd); continue;
            }
            if (hit_in(ev.x, ev.y, 178, 6, 50, 24)) {
                do_copy(); redraw(fd); continue;
            }
            if (hit_in(ev.x, ev.y, 232, 6, 54, 24)) {
                do_paste(); refresh_listing(); redraw(fd); continue;
            }
            if (hit_in(ev.x, ev.y, 290, 6, 46, 24)) {
                if (g_selected > 0) { g_confirm_mode = 1; redraw(fd); }
                continue;
            }
            if (hit_in(ev.x, ev.y, 340, 6, 60, 24)) {
                g_confirm_mode = 2; g_input_len = 0;
                redraw(fd); continue;
            }
            if (hit_in(ev.x, ev.y, 416, 6, 34, 24)) {
                refresh_listing(); redraw(fd); continue;
            }

            /* Sidebar */
            int si = hit_sidebar(ev.x, ev.y);
            if (si >= 0) {
                g_sidebar_sel = si;
                str_copy(g_path, g_sidebar_paths[si], sizeof(g_path));
                refresh_listing();
                redraw(fd);
                continue;
            }

            /* File list */
            int row = hit_row(ev.x, ev.y);
            if (row >= 0) {
                if (g_selected == row) {
                    int nav = open_entry(row);
                    if (nav) refresh_listing();
                } else {
                    g_selected = row;
                }
                redraw(fd);
                continue;
            }
            continue;
        }

        /* ---- Keyboard ---- */
        if (ev.type == GUI_EV_KEY) {
            int total = TOTAL_ROWS;
            switch (ev.key) {
            case 'j':
                if (g_selected < total - 1) { g_selected++; ensure_visible(); }
                redraw(fd); break;
            case 'k':
                if (g_selected > 0) { g_selected--; ensure_visible(); }
                redraw(fd); break;
            case '\n': case 'l': {
                int nav = open_entry(g_selected);
                if (nav) refresh_listing();
                redraw(fd); break;
            }
            case '\b': case 'h':
                path_pop(); refresh_listing(); redraw(fd); break;
            case 'g':
                g_selected = 0; g_scroll = 0; redraw(fd); break;
            case 'G':
                g_selected = total - 1; ensure_visible(); redraw(fd); break;
            case 'd':
                if (g_selected > 0) { g_confirm_mode = 1; redraw(fd); }
                break;
            case 'c':
                do_copy(); redraw(fd); break;
            case 'p':
                do_paste(); refresh_listing(); redraw(fd); break;
            case 'n':
                g_confirm_mode = 2; g_input_len = 0; redraw(fd); break;
            case 'r':
                refresh_listing(); redraw(fd); break;
            case 'q':
                sys_exit(0); break;
            default: break;
            }
        }
    }
}
