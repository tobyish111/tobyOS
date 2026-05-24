/* shell/taskbar.c -- Taskbar implementation (Milestone 3).
 *
 * 40px bar at the bottom of the screen:
 *   Left:   Start button
 *   Center: running application buttons (placeholder stubs)
 *   Right:  system tray (clock, battery, volume, network, notifications)
 *
 * Linked into the shell binary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- syscall numbers (shared with main.c) ------------------------ */

#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_YIELD           5
#define SYS_TIME           40

/* ---- types (matching main.c) ------------------------------------- */

/* ---- syscall wrappers (duplicated here for self-contained TU) ---- */

static inline long tb_sys5(long nr, long a, long b, long c, long d, long e)
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

static void tb_fill(int fd, int x, int y, int w, int h, unsigned int color)
{
    unsigned int wh = ((unsigned int)(unsigned short)w) |
                      (((unsigned int)(unsigned short)h) << 16);
    tb_sys5(SYS_GUI_FILL, fd, x, y, (long)wh, (long)color);
}

static void tb_text(int fd, int x, int y, const char *s,
                    unsigned int fg, unsigned int bg)
{
    unsigned int xy = ((unsigned int)(unsigned short)x) |
                      (((unsigned int)(unsigned short)y) << 16);
    tb_sys5(SYS_GUI_TEXT, fd, (long)xy, (long)s, (long)fg, (long)bg);
}

/* ---- forward declarations from main.c / startmenu.c -------------- */
extern void startmenu_toggle(int win_fd, int scr_w, int scr_h);

/* ---- constants --------------------------------------------------- */

#define TASKBAR_H      40
#define START_BTN_W    80
#define TRAY_W        200
#define APP_BTN_W      80
#define APP_BTN_H      28

#define COL_TASKBAR    0x00181825u
#define COL_START_BG   0x00313244u
#define COL_START_FG   0x00CDD6F4u
#define COL_APP_BG     0x00363848u
#define COL_APP_FG     0x00BAC2DEu
#define COL_TRAY_BG    0x00181825u
#define COL_TRAY_FG    0x00A6ADC8u
#define COL_BORDER     0x0045475Au
#define COL_HIGHLIGHT  0x00585B70u

/* ---- running app list (stubs) ------------------------------------ */

#define MAX_APPS  8

static struct {
    char    title[24];
    int     active;
} g_apps[MAX_APPS];

static int g_app_count;

/* ---- clock / tray state ------------------------------------------ */

static int g_clock_h = 12;
static int g_clock_m = 0;
static int g_battery_pct = 100;

/* ---- simple integer to 2-digit string ---------------------------- */

static void fmt_2d(char *buf, int v)
{
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    buf[0] = '0' + (v / 10);
    buf[1] = '0' + (v % 10);
    buf[2] = '\0';
}

/* ---- public API -------------------------------------------------- */

void taskbar_init(int win_fd, int scr_w, int scr_h)
{
    (void)win_fd;
    (void)scr_w;
    (void)scr_h;

    g_app_count = 0;
    memset(g_apps, 0, sizeof(g_apps));

    g_clock_h = 12;
    g_clock_m = 0;
    g_battery_pct = 100;
}

int taskbar_height(void)
{
    return TASKBAR_H;
}

void taskbar_draw(int win_fd, int scr_w, int scr_h)
{
    int tb_y = scr_h - TASKBAR_H;

    /* Background */
    tb_fill(win_fd, 0, tb_y, scr_w, TASKBAR_H, COL_TASKBAR);

    /* Top border line */
    tb_fill(win_fd, 0, tb_y, scr_w, 1, COL_BORDER);

    /* Start button */
    tb_fill(win_fd, 4, tb_y + 6, START_BTN_W, APP_BTN_H, COL_START_BG);
    tb_text(win_fd, 16, tb_y + 12, "Start", COL_START_FG, COL_START_BG);

    /* Application buttons (center region) */
    int app_x = START_BTN_W + 16;
    for (int i = 0; i < g_app_count && i < MAX_APPS; i++) {
        unsigned int bg = g_apps[i].active ? COL_HIGHLIGHT : COL_APP_BG;
        tb_fill(win_fd, app_x, tb_y + 6, APP_BTN_W, APP_BTN_H, bg);
        tb_text(win_fd, app_x + 6, tb_y + 12, g_apps[i].title,
                COL_APP_FG, bg);
        app_x += APP_BTN_W + 4;
    }

    /* System tray (right side) */
    int tray_x = scr_w - TRAY_W;

    /* Network icon placeholder */
    tb_text(win_fd, tray_x, tb_y + 12, "NET", COL_TRAY_FG, COL_TASKBAR);

    /* Volume icon placeholder */
    tb_text(win_fd, tray_x + 36, tb_y + 12, "VOL", COL_TRAY_FG, COL_TASKBAR);

    /* Battery */
    char batt[8];
    int bi = 0;
    int pct = g_battery_pct;
    if (pct >= 100) { batt[bi++] = '1'; pct -= 100; }
    batt[bi++] = '0' + (pct / 10);
    batt[bi++] = '0' + (pct % 10);
    batt[bi++] = '%';
    batt[bi]   = '\0';
    tb_text(win_fd, tray_x + 76, tb_y + 12, batt, COL_TRAY_FG, COL_TASKBAR);

    /* Notification bell */
    tb_text(win_fd, tray_x + 120, tb_y + 12, "BELL",
            COL_TRAY_FG, COL_TASKBAR);

    /* Clock HH:MM */
    char clock[6];
    fmt_2d(clock, g_clock_h);
    clock[2] = ':';
    fmt_2d(clock + 3, g_clock_m);
    tb_text(win_fd, tray_x + 160, tb_y + 12, clock,
            COL_TRAY_FG, COL_TASKBAR);
}

void taskbar_handle_click(int win_fd, int x, int y, int scr_w, int scr_h)
{
    int tb_y = scr_h - TASKBAR_H;

    if (y < tb_y)
        return;

    /* Start button region */
    if (x >= 4 && x < 4 + START_BTN_W) {
        startmenu_toggle(win_fd, scr_w, scr_h);
        return;
    }

    /* App buttons */
    int app_x = START_BTN_W + 16;
    for (int i = 0; i < g_app_count && i < MAX_APPS; i++) {
        if (x >= app_x && x < app_x + APP_BTN_W) {
            /* Toggle focus/minimize on the app */
            g_apps[i].active = !g_apps[i].active;
            return;
        }
        app_x += APP_BTN_W + 4;
    }

    /* Right-click anywhere on taskbar: could open Task Manager
     * (not implemented in this stub) */
}
