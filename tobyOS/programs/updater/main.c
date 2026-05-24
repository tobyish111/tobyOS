/* programs/updater/main.c -- System Update client for tobyOS.
 *
 * GUI window 500x350.
 * - Displays current version (1.0.0)
 * - "Check for Updates" button
 * - Shows changelog when update is available
 * - Download progress bar
 * - "Install Update" button with reboot prompt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── syscall numbers ─────────────────────────────────────────────── */

#define SYS_WRITE           1
#define SYS_EXIT            3
#define SYS_YIELD           5
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_CLOCK_MS       48

struct gui_event {
    int     type;
    int     x, y;
    unsigned char button;
    unsigned char key;
    unsigned char _pad[2];
};

#define GUI_EV_MOUSE_DOWN 2
#define GUI_EV_KEY        4
#define GUI_EV_CLOSE      5

/* ── syscall wrappers ────────────────────────────────────────────── */

static long sc1(long n, long a) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a):"rcx","r11","memory");
    return r;
}
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");
    return r;
}
static long sc6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall":"=a"(r)
        :"0"(n),"D"(a1),"S"(a2),"d"(a3),"r"(r10),"r"(r8),"r"(r9)
        :"rcx","r11","memory");
    return r;
}

static void sys_yield(void) { sc1(SYS_YIELD, 0); }
static int  sys_gui_create(int w, int h, const char *t) { return (int)sc3(SYS_GUI_CREATE,w,h,(long)t); }
static void sys_gui_fill(int fd,int x,int y,int w,int h,unsigned c) { sc6(SYS_GUI_FILL,fd,x,y,w,((long)h<<32)|c,0); }
static void sys_gui_text(int fd,int x,int y,const char *s,unsigned fg,unsigned bg) { sc6(SYS_GUI_TEXT,fd,x,y,(long)s,fg,bg); }
static void sys_gui_flip(int fd) { sc1(SYS_GUI_FLIP, fd); }
static int  sys_gui_poll(int fd, struct gui_event *ev) { return (int)sc3(SYS_GUI_POLL_EVENT,fd,(long)ev,0); }
static long sys_clock_ms(void) { return sc1(SYS_CLOCK_MS, 0); }

/* ── constants ───────────────────────────────────────────────────── */

#define WIN_W 500
#define WIN_H 350

#define COL_BG     0x1E1E2E
#define COL_PANEL  0x313244
#define COL_TEXT   0xCDD6F4
#define COL_DIM    0x6C7086
#define COL_ACCENT 0x89B4FA
#define COL_GREEN  0xA6E3A1
#define COL_RED    0xF38BA8
#define COL_BTN    0x585B70
#define COL_WHITE  0xFFFFFF

static int g_fd = -1;

/* Update state machine */
enum update_state {
    ST_IDLE,
    ST_CHECKING,
    ST_AVAILABLE,
    ST_DOWNLOADING,
    ST_READY,
    ST_INSTALLING,
    ST_UP_TO_DATE,
};

static enum update_state g_state = ST_IDLE;
static int g_progress = 0;
static long g_check_start = 0;
static long g_dl_start = 0;

static const char *g_cur_version = "1.0.0";
static const char *g_new_version = "1.1.0";

static const char *g_changelog[] = {
    "- Improved WiFi management",
    "- Added WireGuard VPN support",
    "- New firewall configuration",
    "- Device Manager with hot-plug",
    "- System Search / File Indexing",
    "- System Restore Points",
    NULL
};

/* ── drawing ─────────────────────────────────────────────────────── */

static void draw_btn(int x, int y, int w, int h, const char *label, unsigned bg) {
    sys_gui_fill(g_fd, x, y, w, h, bg);
    int tx = x + (w - (int)strlen(label) * 8) / 2;
    int ty = y + (h - 8) / 2;
    sys_gui_text(g_fd, tx, ty, label, COL_WHITE, bg);
}

static void draw_progress_bar(int x, int y, int w, int h, int pct) {
    sys_gui_fill(g_fd, x, y, w, h, COL_PANEL);
    int fill_w = (w * pct) / 100;
    if (fill_w > 0)
        sys_gui_fill(g_fd, x, y, fill_w, h, COL_ACCENT);

    char pstr[8];
    snprintf(pstr, sizeof(pstr), "%d%%", pct);
    sys_gui_text(g_fd, x + w / 2 - 12, y + 2, pstr, COL_WHITE,
                 (pct > 50) ? COL_ACCENT : COL_PANEL);
}

static void redraw(void) {
    sys_gui_fill(g_fd, 0, 0, WIN_W, WIN_H, COL_BG);

    /* Header */
    sys_gui_fill(g_fd, 0, 0, WIN_W, 40, COL_PANEL);
    sys_gui_text(g_fd, 15, 14, "tobyOS System Update", COL_ACCENT, COL_PANEL);

    int y = 55;

    /* Current version */
    char ver[48];
    snprintf(ver, sizeof(ver), "Current Version: %s", g_cur_version);
    sys_gui_text(g_fd, 20, y, ver, COL_TEXT, COL_BG);
    y += 25;

    switch (g_state) {
    case ST_IDLE:
        draw_btn(20, y, 160, 28, "Check for Updates", COL_ACCENT);
        y += 45;
        sys_gui_text(g_fd, 20, y, "No updates have been checked.", COL_DIM, COL_BG);
        break;

    case ST_CHECKING:
        sys_gui_text(g_fd, 20, y, "Checking for updates...", COL_TEXT, COL_BG);
        y += 20;
        draw_progress_bar(20, y, WIN_W - 40, 16, g_progress);
        break;

    case ST_UP_TO_DATE:
        sys_gui_fill(g_fd, 20, y, 12, 12, COL_GREEN);
        sys_gui_text(g_fd, 40, y + 2, "Your system is up to date!", COL_GREEN, COL_BG);
        y += 35;
        draw_btn(20, y, 160, 28, "Check Again", COL_BTN);
        break;

    case ST_AVAILABLE:
        snprintf(ver, sizeof(ver), "Update Available: v%s", g_new_version);
        sys_gui_text(g_fd, 20, y, ver, COL_GREEN, COL_BG);
        y += 25;

        sys_gui_text(g_fd, 20, y, "Changelog:", COL_ACCENT, COL_BG);
        y += 18;

        for (int i = 0; g_changelog[i]; i++) {
            sys_gui_text(g_fd, 30, y, g_changelog[i], COL_TEXT, COL_BG);
            y += 16;
        }

        y += 10;
        draw_btn(20, y, 140, 28, "Install Update", COL_GREEN);
        draw_btn(175, y, 80, 28, "Later", COL_BTN);
        break;

    case ST_DOWNLOADING:
        snprintf(ver, sizeof(ver), "Downloading v%s ...", g_new_version);
        sys_gui_text(g_fd, 20, y, ver, COL_TEXT, COL_BG);
        y += 25;
        draw_progress_bar(20, y, WIN_W - 40, 20, g_progress);
        y += 30;
        {
            char sz[32];
            int mb = g_progress * 24 / 100;
            snprintf(sz, sizeof(sz), "%d MB / 24 MB", mb);
            sys_gui_text(g_fd, 20, y, sz, COL_DIM, COL_BG);
        }
        break;

    case ST_READY:
        sys_gui_text(g_fd, 20, y, "Download complete!", COL_GREEN, COL_BG);
        y += 25;
        sys_gui_text(g_fd, 20, y, "A system restart is required to apply the update.",
                     COL_TEXT, COL_BG);
        y += 30;
        draw_btn(20, y, 130, 28, "Restart Now", COL_RED);
        draw_btn(165, y, 130, 28, "Restart Later", COL_BTN);
        break;

    case ST_INSTALLING:
        sys_gui_text(g_fd, 20, y, "Installing update... Do not power off.", COL_RED, COL_BG);
        y += 25;
        draw_progress_bar(20, y, WIN_W - 40, 20, g_progress);
        break;
    }

    /* Footer */
    sys_gui_fill(g_fd, 0, WIN_H - 22, WIN_W, 22, COL_PANEL);
    sys_gui_text(g_fd, 10, WIN_H - 17, "tobyOS Update Service", COL_DIM, COL_PANEL);

    sys_gui_flip(g_fd);
}

/* ── state transitions ───────────────────────────────────────────── */

static void tick(void) {
    long now = sys_clock_ms();

    if (g_state == ST_CHECKING) {
        long elapsed = now - g_check_start;
        g_progress = (int)(elapsed / 20);
        if (g_progress >= 100) {
            g_progress = 100;
            g_state = ST_AVAILABLE;
        }
    }

    if (g_state == ST_DOWNLOADING) {
        long elapsed = now - g_dl_start;
        g_progress = (int)(elapsed / 50);
        if (g_progress >= 100) {
            g_progress = 100;
            g_state = ST_READY;
        }
    }

    if (g_state == ST_INSTALLING) {
        long elapsed = now - g_dl_start;
        g_progress = (int)(elapsed / 30);
        if (g_progress >= 100)
            g_progress = 100;
    }
}

static void handle_click(int mx, int my) {
    int by = 80; /* approximate button y */

    switch (g_state) {
    case ST_IDLE:
        if (mx >= 20 && mx < 180 && my >= by && my < by + 28) {
            g_state = ST_CHECKING;
            g_progress = 0;
            g_check_start = sys_clock_ms();
        }
        break;

    case ST_UP_TO_DATE:
        if (mx >= 20 && mx < 180 && my >= by + 35 && my < by + 63) {
            g_state = ST_CHECKING;
            g_progress = 0;
            g_check_start = sys_clock_ms();
        }
        break;

    case ST_AVAILABLE:
        /* Install button */
        {
            int btn_y = by + 25 + 18 + 6 * 16 + 10;
            if (mx >= 20 && mx < 160 && my >= btn_y && my < btn_y + 28) {
                g_state = ST_DOWNLOADING;
                g_progress = 0;
                g_dl_start = sys_clock_ms();
            }
            if (mx >= 175 && mx < 255 && my >= btn_y && my < btn_y + 28)
                g_state = ST_IDLE;
        }
        break;

    case ST_READY:
        if (mx >= 20 && mx < 150 && my >= by + 55 && my < by + 83) {
            g_state = ST_INSTALLING;
            g_progress = 0;
            g_dl_start = sys_clock_ms();
        }
        if (mx >= 165 && mx < 295 && my >= by + 55 && my < by + 83)
            g_state = ST_IDLE;
        break;

    default:
        break;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    g_fd = sys_gui_create(WIN_W, WIN_H, "System Update");
    if (g_fd < 0) return 1;

    redraw();

    for (;;) {
        struct gui_event ev;
        while (sys_gui_poll(g_fd, &ev)) {
            if (ev.type == GUI_EV_CLOSE) return 0;
            if (ev.type == GUI_EV_MOUSE_DOWN) {
                handle_click(ev.x, ev.y);
            }
        }

        tick();
        redraw();
        sys_yield();
    }
}
