/* shell/main.c -- Desktop Shell entry point (Milestone 3).
 *
 * Spawns the full desktop environment:
 *   - Fullscreen window for desktop background
 *   - Taskbar at the bottom (40px)
 *   - Virtual desktop management (4 desktops)
 *   - Alt+Tab window switcher overlay
 *   - Right-click context menu
 *   - Win+D show/hide desktop
 *   - Window snapping on screen edges
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>

extern pid_t toby_spawn(const char *path, char *const argv[],
                        char *const envp[], int fd0, int fd1, int fd2);

/* ---- forward declarations from other TUs ------------------------- */
extern void taskbar_init(int win_fd, int scr_w, int scr_h);
extern void taskbar_draw(int win_fd, int scr_w, int scr_h);
extern void taskbar_handle_click(int win_fd, int x, int y,
                                 int scr_w, int scr_h);
extern int  taskbar_height(void);

extern void startmenu_init(void);
extern void startmenu_toggle(int win_fd, int scr_w, int scr_h);
extern void startmenu_draw(int win_fd, int scr_w, int scr_h);
extern int  startmenu_handle_click(int win_fd, int x, int y,
                                   int scr_w, int scr_h);
extern int  startmenu_visible(void);
extern void startmenu_handle_key(int win_fd, int key,
                                 int scr_w, int scr_h);

/* ---- syscall numbers --------------------------------------------- */

#define SYS_WRITE           1
#define SYS_YIELD           5
#define SYS_NANOSLEEP       6
#define SYS_EXIT            7
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_GUI_LINE       18
#define SYS_SPAWN          20
#define SYS_DISPLAY_INFO   44

/* ---- types ------------------------------------------------------- */

typedef unsigned int  uint32_t_local;

struct gui_event {
    int     type;
    int     x;
    int     y;
    unsigned char button;
    unsigned char key;
    unsigned char _pad[2];
};

#define GUI_EV_NONE        0
#define GUI_EV_MOUSE_MOVE  1
#define GUI_EV_MOUSE_DOWN  2
#define GUI_EV_MOUSE_UP    3
#define GUI_EV_KEY         4
#define GUI_EV_CLOSE       5
#define GUI_EV_RESIZE      6

/* ---- syscall wrappers -------------------------------------------- */

static inline long _sys1(long nr, long a)
{
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a)
        : "rcx", "r11", "memory");
    return r;
}

static inline long _sys2(long nr, long a, long b)
{
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a), "S"(b)
        : "rcx", "r11", "memory");
    return r;
}

static inline long _sys3(long nr, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return r;
}

static inline long _sys5(long nr, long a, long b, long c, long d, long e)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return r;
}

static int gui_create(int w, int h, const char *title)
{
    return (int)_sys3(SYS_GUI_CREATE, w, h, (long)title);
}

static void gui_fill(int fd, int x, int y, int w, int h, unsigned int color)
{
    unsigned int wh = ((unsigned int)(unsigned short)w) |
                      (((unsigned int)(unsigned short)h) << 16);
    _sys5(SYS_GUI_FILL, fd, x, y, (long)wh, (long)color);
}

static void gui_text(int fd, int x, int y, const char *s,
                     unsigned int fg, unsigned int bg)
{
    unsigned int xy = ((unsigned int)(unsigned short)x) |
                      (((unsigned int)(unsigned short)y) << 16);
    _sys5(SYS_GUI_TEXT, fd, (long)xy, (long)s, (long)fg, (long)bg);
}

static void gui_flip(int fd)
{
    _sys1(SYS_GUI_FLIP, fd);
}

static int gui_poll(int fd, struct gui_event *ev)
{
    return (int)_sys2(SYS_GUI_POLL_EVENT, fd, (long)ev);
}

static void sys_nanosleep(long ns)
{
    _sys1(SYS_NANOSLEEP, ns);
}

/* ---- constants --------------------------------------------------- */

#define SCREEN_W     1024
#define SCREEN_H      768
#define TASKBAR_H      40
#define DESKTOP_COUNT   4

#define COL_DESKTOP    0x001A1B26u
#define COL_DESKTOP2   0x001E2030u
#define COL_DESKTOP3   0x00242640u
#define COL_DESKTOP4   0x00202238u
#define COL_TEXT       0x00CDD6F4u
#define COL_TEXT_DIM   0x00585B70u
#define COL_SNAP_HINT  0x00505080u

#define CTX_MENU_W     180
#define CTX_MENU_H     120
#define CTX_ITEM_H      30

#define SNAP_THRESHOLD  16

/* Alt+Tab overlay */
#define SWITCHER_W     400
#define SWITCHER_H     200

/* ---- desktop state ----------------------------------------------- */

static int g_current_desktop;

static const unsigned int g_desktop_colors[DESKTOP_COUNT] = {
    COL_DESKTOP, COL_DESKTOP2, COL_DESKTOP3, COL_DESKTOP4,
};

/* ---- context menu state ------------------------------------------ */

static int g_ctx_visible;
static int g_ctx_x, g_ctx_y;

#define CTX_ITEMS  4
static const char *g_ctx_labels[CTX_ITEMS] = {
    "Terminal",
    "Files",
    "Settings",
    "About",
};
static const char *g_ctx_commands[CTX_ITEMS] = {
    "/bin/gui_term",
    "/bin/gui_files",
    "/bin/gui_settings",
    "/bin/gui_about",
};

/* ---- Alt+Tab state ----------------------------------------------- */

static int g_switcher_active;

/* ---- snap state -------------------------------------------------- */

static int g_snap_hint;
static int g_snap_x, g_snap_y, g_snap_w, g_snap_h;

/* ---- helpers ----------------------------------------------------- */

static void draw_desktop_bg(int fd, int scr_w, int scr_h)
{
    unsigned int bg = g_desktop_colors[g_current_desktop];
    gui_fill(fd, 0, 0, scr_w, scr_h - TASKBAR_H, bg);

    /* Desktop label */
    char label[32];
    int len = 0;
    label[len++] = 'D';
    label[len++] = 'e';
    label[len++] = 's';
    label[len++] = 'k';
    label[len++] = 't';
    label[len++] = 'o';
    label[len++] = 'p';
    label[len++] = ' ';
    label[len++] = '1' + (char)g_current_desktop;
    label[len]   = '\0';

    gui_text(fd, scr_w / 2 - 36, scr_h / 2 - 8, label,
             COL_TEXT_DIM, bg);
}

static void draw_context_menu(int fd)
{
    if (!g_ctx_visible)
        return;

    gui_fill(fd, g_ctx_x, g_ctx_y, CTX_MENU_W, CTX_MENU_H, 0x00282A36u);

    for (int i = 0; i < CTX_ITEMS; i++) {
        int iy = g_ctx_y + i * CTX_ITEM_H;
        gui_text(fd, g_ctx_x + 12, iy + 8, g_ctx_labels[i],
                 COL_TEXT, 0x00282A36u);
    }
}

static void handle_context_click(int x, int y)
{
    if (!g_ctx_visible)
        return;

    if (x < g_ctx_x || x >= g_ctx_x + CTX_MENU_W ||
        y < g_ctx_y || y >= g_ctx_y + CTX_MENU_H) {
        g_ctx_visible = 0;
        return;
    }

    int idx = (y - g_ctx_y) / CTX_ITEM_H;
    if (idx >= 0 && idx < CTX_ITEMS) {
        /* Launch the selected application */
        const char *path = g_ctx_commands[idx];
        char *argv[2];
        argv[0] = (char *)path;
        argv[1] = NULL;
        toby_spawn(path, argv, NULL, 0, 1, 2);
    }

    g_ctx_visible = 0;
}

static void draw_switcher_overlay(int fd, int scr_w, int scr_h)
{
    if (!g_switcher_active)
        return;

    int ox = (scr_w - SWITCHER_W) / 2;
    int oy = (scr_h - SWITCHER_H) / 2;

    gui_fill(fd, ox, oy, SWITCHER_W, SWITCHER_H, 0x00303050u);
    gui_text(fd, ox + 20, oy + 20, "Alt+Tab: Window Switcher",
             COL_TEXT, 0x00303050u);
    gui_text(fd, ox + 20, oy + 50, "(release Alt to select)",
             COL_TEXT_DIM, 0x00303050u);

    /* Show desktop indicators */
    for (int i = 0; i < DESKTOP_COUNT; i++) {
        int bx = ox + 20 + i * 90;
        int by = oy + 90;
        unsigned int c = (i == g_current_desktop) ? 0x00505080u : 0x00404060u;
        gui_fill(fd, bx, by, 80, 60, c);

        char num[2] = { '1' + (char)i, '\0' };
        gui_text(fd, bx + 34, by + 22, num, COL_TEXT, c);
    }
}

static void draw_snap_hint(int fd)
{
    if (!g_snap_hint)
        return;
    gui_fill(fd, g_snap_x, g_snap_y, g_snap_w, g_snap_h, COL_SNAP_HINT);
}

static void detect_snap(int mx, int my, int scr_w, int scr_h)
{
    int usable_h = scr_h - TASKBAR_H;

    if (mx <= SNAP_THRESHOLD && my <= SNAP_THRESHOLD) {
        /* top-left quarter */
        g_snap_hint = 1;
        g_snap_x = 0; g_snap_y = 0;
        g_snap_w = scr_w / 2; g_snap_h = usable_h / 2;
    } else if (mx >= scr_w - SNAP_THRESHOLD && my <= SNAP_THRESHOLD) {
        /* top-right quarter */
        g_snap_hint = 1;
        g_snap_x = scr_w / 2; g_snap_y = 0;
        g_snap_w = scr_w / 2; g_snap_h = usable_h / 2;
    } else if (mx <= SNAP_THRESHOLD && my >= usable_h - SNAP_THRESHOLD) {
        /* bottom-left quarter */
        g_snap_hint = 1;
        g_snap_x = 0; g_snap_y = usable_h / 2;
        g_snap_w = scr_w / 2; g_snap_h = usable_h / 2;
    } else if (mx >= scr_w - SNAP_THRESHOLD &&
               my >= usable_h - SNAP_THRESHOLD) {
        /* bottom-right quarter */
        g_snap_hint = 1;
        g_snap_x = scr_w / 2; g_snap_y = usable_h / 2;
        g_snap_w = scr_w / 2; g_snap_h = usable_h / 2;
    } else if (mx <= SNAP_THRESHOLD) {
        /* left half */
        g_snap_hint = 1;
        g_snap_x = 0; g_snap_y = 0;
        g_snap_w = scr_w / 2; g_snap_h = usable_h;
    } else if (mx >= scr_w - SNAP_THRESHOLD) {
        /* right half */
        g_snap_hint = 1;
        g_snap_x = scr_w / 2; g_snap_y = 0;
        g_snap_w = scr_w / 2; g_snap_h = usable_h;
    } else if (my <= SNAP_THRESHOLD) {
        /* top half (maximize) */
        g_snap_hint = 1;
        g_snap_x = 0; g_snap_y = 0;
        g_snap_w = scr_w; g_snap_h = usable_h;
    } else {
        g_snap_hint = 0;
    }
}

/* ---- main event loop --------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int scr_w = SCREEN_W;
    int scr_h = SCREEN_H;

    int fd = gui_create(scr_w, scr_h, "Desktop");
    if (fd < 0) {
        const char *msg = "shell: failed to create desktop window\n";
        write(STDOUT_FILENO, msg, strlen(msg));
        return 1;
    }

    taskbar_init(fd, scr_w, scr_h);
    startmenu_init();

    /* Initial draw */
    draw_desktop_bg(fd, scr_w, scr_h);
    taskbar_draw(fd, scr_w, scr_h);
    gui_flip(fd);

    int running = 1;
    int dragging = 0;
    int needs_redraw = 0;

    while (running) {
        struct gui_event ev;
        while (gui_poll(fd, &ev) > 0) {
            switch (ev.type) {
            case GUI_EV_CLOSE:
                running = 0;
                break;

            case GUI_EV_MOUSE_DOWN:
                if (ev.button & 0x02) {
                    /* Right click -> context menu */
                    if (startmenu_visible()) {
                        startmenu_toggle(fd, scr_w, scr_h);
                    }
                    g_ctx_visible = 1;
                    g_ctx_x = ev.x;
                    g_ctx_y = ev.y;
                    if (g_ctx_x + CTX_MENU_W > scr_w)
                        g_ctx_x = scr_w - CTX_MENU_W;
                    if (g_ctx_y + CTX_MENU_H > scr_h - TASKBAR_H)
                        g_ctx_y = scr_h - TASKBAR_H - CTX_MENU_H;
                    needs_redraw = 1;
                } else {
                    /* Left click */
                    if (g_ctx_visible) {
                        handle_context_click(ev.x, ev.y);
                        needs_redraw = 1;
                    } else if (startmenu_visible()) {
                        if (!startmenu_handle_click(fd, ev.x, ev.y,
                                                    scr_w, scr_h)) {
                            startmenu_toggle(fd, scr_w, scr_h);
                        }
                        needs_redraw = 1;
                    } else if (ev.y >= scr_h - TASKBAR_H) {
                        taskbar_handle_click(fd, ev.x, ev.y,
                                             scr_w, scr_h);
                        needs_redraw = 1;
                    } else {
                        dragging = 1;
                    }
                }
                break;

            case GUI_EV_MOUSE_UP:
                if (dragging) {
                    dragging = 0;
                    g_snap_hint = 0;
                    needs_redraw = 1;
                }
                break;

            case GUI_EV_MOUSE_MOVE:
                if (dragging) {
                    detect_snap(ev.x, ev.y, scr_w, scr_h);
                    needs_redraw = 1;
                }
                break;

            case GUI_EV_KEY: {
                unsigned char key = ev.key;

                /* Alt+Tab: toggle window switcher */
                if (key == '\t') {
                    g_switcher_active = !g_switcher_active;
                    needs_redraw = 1;
                    break;
                }

                /* Win+D or Ctrl+D: toggle show desktop */
                if (key == 'd' || key == 'D') {
                    g_switcher_active = 0;
                    g_ctx_visible = 0;
                    needs_redraw = 1;
                    break;
                }

                /* 1-4: switch desktops */
                if (key >= '1' && key <= '4') {
                    g_current_desktop = key - '1';
                    needs_redraw = 1;
                    break;
                }

                /* Forward keys to start menu if visible */
                if (startmenu_visible()) {
                    startmenu_handle_key(fd, key, scr_w, scr_h);
                    needs_redraw = 1;
                }
                break;
            }

            default:
                break;
            }
        }

        if (needs_redraw) {
            draw_desktop_bg(fd, scr_w, scr_h);
            draw_snap_hint(fd);
            taskbar_draw(fd, scr_w, scr_h);
            draw_context_menu(fd);
            startmenu_draw(fd, scr_w, scr_h);
            draw_switcher_overlay(fd, scr_w, scr_h);
            gui_flip(fd);
            needs_redraw = 0;
        }

        sys_nanosleep(16000000L);
    }

    return 0;
}
