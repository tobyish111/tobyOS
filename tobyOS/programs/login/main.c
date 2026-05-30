/* login/main.c -- Graphical login screen for tobyOS (Milestone 4).
 *
 * Full-screen window with username/password fields, "Sign In" button.
 * On success: sets uid on current process, execs the desktop shell.
 * On failure: shows "Invalid credentials" and clears password.
 * Supports Enter key to submit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tobyos/abi/abi.h>

/* ---- syscall wrappers ---- */

static inline long sc0(long nr)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(nr) : "rcx","r11","memory");
    return r;
}
static inline long sc1(long nr, long a1)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"(nr), "D"(a1) : "rcx","r11","memory");
    return r;
}
static inline long sc2(long nr, long a1, long a2)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"(nr), "D"(a1), "S"(a2) : "rcx","r11","memory");
    return r;
}
static inline long sc3(long nr, long a1, long a2, long a3)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx","r11","memory");
    return r;
}
static inline long sc5(long nr, long a1, long a2, long a3, long a4, long a5)
{
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx","r11","memory");
    return r;
}

static int gui_create(int w, int h, const char *title)
{
    return (int)sc3(ABI_SYS_GUI_CREATE, w, h, (long)title);
}
static void gui_fill(int fd, int x, int y, int w, int h, unsigned c)
{
    unsigned wh = ((unsigned)(unsigned short)w) |
                  (((unsigned)(unsigned short)h) << 16);
    sc5(ABI_SYS_GUI_FILL, fd, x, y, wh, c);
}
static void gui_text(int fd, int x, int y, const char *s, unsigned fg)
{
    long xy = (long)(unsigned)((unsigned)x | ((unsigned)y << 16));
    sc5(ABI_SYS_GUI_TEXT, fd, xy, (long)s, fg, 0);
}
static void gui_text_scaled(int fd, int x, int y, const char *s,
                            unsigned fg, unsigned bg, int scale)
{
    long xy = (long)(unsigned)((unsigned)x | ((unsigned)y << 16));
    unsigned a5 = (bg & 0x00FFFFFFu) | ((unsigned)(scale & 0x7F) << 24);
    sc5(ABI_SYS_GUI_TEXT_SCALED, fd, xy, (long)s, fg, a5);
}
static void gui_flip(int fd) { sc1(ABI_SYS_GUI_FLIP, fd); }
static void gui_set_state(int fd, int s) { sc2(ABI_SYS_GUI_SET_STATE, fd, s); }
static long gui_poll(int fd, void *ev)
{
    return sc2(ABI_SYS_GUI_POLL_EVENT, fd, (long)ev);
}
static long sys_login(const char *user, const char *pass)
{
    return sc2(ABI_SYS_LOGIN, (long)user, (long)pass);
}
static long sys_exec(const char *path)
{
    return sc1(ABI_SYS_EXEC, (long)path);
}
static long sys_clock_ms(void) { return sc0(ABI_SYS_CLOCK_MS); }

struct gui_event {
    int     type;
    int     x, y;
    unsigned char button;
    unsigned char key;
    unsigned char _pad[2];
};
/* GUI event types -- MUST match <tobyos/gui.h> GUI_EV_*. A mouse-move
 * event is type 1; older code defined EV_CLOSE=1, which made the login
 * window exit (and the login service flap between login/desktop the
 * moment the pointer moved). Keep these in lockstep with the kernel. */
#define EV_MOUSE_MOVE 1
#define EV_MOUSE_DOWN 2
#define EV_MOUSE_UP   3
#define EV_KEY        4
#define EV_CLOSE      5
#define EV_RESIZE     6

#define WIN_MAXIMIZED 2

/* ---- colours ---- */
#define COL_BG        0x1a1a2e
#define COL_PANEL     0x16213e
#define COL_FIELD_BG  0x0f3460
#define COL_FIELD_FG  0xffffff
#define COL_BUTTON    0xe94560
#define COL_BTN_TEXT  0xffffff
#define COL_TITLE     0xffffff
#define COL_SUBTITLE  0x8899aa
#define COL_ERROR     0xff4444
#define COL_PLACEHOLDER 0x6677aa

/* ---- layout constants ---- */
#define SCR_W  1024
#define SCR_H  768
#define PANEL_W 360
#define PANEL_H 340
#define FIELD_H 28
#define FIELD_W 280
#define BTN_W   280
#define BTN_H   36

/* ---- state ---- */
static char username[32];
static int  ulen;
static char password[32];
static int  plen;
static int  focus; /* 0=user, 1=pass */
static int  error_shown;
static long error_time;

static int px, py; /* panel top-left */

static void draw(int fd)
{
    /* background */
    gui_fill(fd, 0, 0, SCR_W, SCR_H, COL_BG);

    /* gradient effect: a few horizontal bands */
    for (int i = 0; i < 8; i++) {
        unsigned shade = 0x1a1a2e + i * 0x020204;
        gui_fill(fd, 0, i * (SCR_H/8), SCR_W, SCR_H/8, shade);
    }

    /* panel */
    px = (SCR_W - PANEL_W) / 2;
    py = (SCR_H - PANEL_H) / 2 - 20;
    gui_fill(fd, px, py, PANEL_W, PANEL_H, COL_PANEL);

    /* title */
    gui_text_scaled(fd, px + PANEL_W/2 - 60, py + 20,
                    "tobyOS", COL_TITLE, COL_PANEL, 3);
    gui_text(fd, px + PANEL_W/2 - 48, py + 56,
             "Sign in to continue", COL_SUBTITLE);

    /* Username label + field */
    int fy = py + 90;
    gui_text(fd, px + 38, fy, "Username", COL_SUBTITLE);
    fy += 18;
    int fx = px + (PANEL_W - FIELD_W) / 2;
    gui_fill(fd, fx, fy, FIELD_W, FIELD_H, COL_FIELD_BG);
    if (focus == 0) {
        gui_fill(fd, fx, fy + FIELD_H - 2, FIELD_W, 2, COL_BUTTON);
    }
    if (ulen > 0)
        gui_text(fd, fx + 8, fy + 7, username, COL_FIELD_FG);
    else
        gui_text(fd, fx + 8, fy + 7, "Enter username", COL_PLACEHOLDER);

    /* Password label + field */
    fy += FIELD_H + 16;
    gui_text(fd, px + 38, fy, "Password", COL_SUBTITLE);
    fy += 18;
    gui_fill(fd, fx, fy, FIELD_W, FIELD_H, COL_FIELD_BG);
    if (focus == 1) {
        gui_fill(fd, fx, fy + FIELD_H - 2, FIELD_W, 2, COL_BUTTON);
    }
    if (plen > 0) {
        char dots[32];
        int dl = plen > 30 ? 30 : plen;
        for (int i = 0; i < dl; i++) dots[i] = '*';
        dots[dl] = '\0';
        gui_text(fd, fx + 8, fy + 7, dots, COL_FIELD_FG);
    } else {
        gui_text(fd, fx + 8, fy + 7, "Enter password", COL_PLACEHOLDER);
    }

    /* Sign In button */
    int by = fy + FIELD_H + 24;
    int bx = px + (PANEL_W - BTN_W) / 2;
    gui_fill(fd, bx, by, BTN_W, BTN_H, COL_BUTTON);
    gui_text(fd, bx + BTN_W/2 - 28, by + 10, "Sign In", COL_BTN_TEXT);

    /* Error message */
    if (error_shown) {
        long now = sys_clock_ms();
        if (now - error_time < 3000) {
            gui_text(fd, px + PANEL_W/2 - 68, by + BTN_H + 14,
                     "Invalid credentials", COL_ERROR);
        } else {
            error_shown = 0;
        }
    }

    gui_flip(fd);
}

static int in_rect(int mx, int my, int rx, int ry, int rw, int rh)
{
    return mx >= rx && mx < rx + rw && my >= ry && my < ry + rh;
}

static void do_login(int fd)
{
    if (ulen == 0) return;
    username[ulen] = '\0';
    password[plen] = '\0';

    long rc = sys_login(username, password);
    if (rc >= 0) {
        /* Success -- exec desktop */
        sys_exec("/bin/sh");
        _exit(0);
    }

    /* Failure */
    error_shown = 1;
    error_time = sys_clock_ms();
    memset(password, 0, sizeof(password));
    plen = 0;
    draw(fd);
}

int main(void)
{
    memset(username, 0, sizeof(username));
    memset(password, 0, sizeof(password));
    ulen = plen = 0;
    focus = 0;
    error_shown = 0;

    int fd = gui_create(SCR_W, SCR_H, "Login");
    if (fd < 0) return 1;
    gui_set_state(fd, WIN_MAXIMIZED);

    draw(fd);

    struct gui_event ev;
    for (;;) {
        if (gui_poll(fd, &ev) > 0) {
            /* The login screen is the session gateway -- it must not be
             * dismissable. Ignore EV_CLOSE (Alt+F4, stray close events)
             * so the only way out is a successful sign-in (do_login execs
             * the shell and exits). This also stops the login service
             * from flapping between login and the bare desktop. */
            if (ev.type == EV_CLOSE) continue;

            if (ev.type == EV_MOUSE_DOWN) {
                int fx = px + (PANEL_W - FIELD_W) / 2;
                int ufy = py + 90 + 18;
                int pfy = ufy + FIELD_H + 16 + 18;
                int by  = pfy + FIELD_H + 24;
                int bx  = px + (PANEL_W - BTN_W) / 2;

                if (in_rect(ev.x, ev.y, fx, ufy, FIELD_W, FIELD_H))
                    focus = 0;
                else if (in_rect(ev.x, ev.y, fx, pfy, FIELD_W, FIELD_H))
                    focus = 1;
                else if (in_rect(ev.x, ev.y, bx, by, BTN_W, BTN_H))
                    do_login(fd);

                draw(fd);
            }

            if (ev.type == EV_KEY) {
                unsigned char k = ev.key;
                if (k == '\n' || k == '\r') {
                    do_login(fd);
                    continue;
                }
                if (k == '\t') {
                    focus = 1 - focus;
                    draw(fd);
                    continue;
                }
                if (k == 0x08) { /* backspace */
                    if (focus == 0 && ulen > 0) username[--ulen] = '\0';
                    if (focus == 1 && plen > 0) password[--plen] = '\0';
                    draw(fd);
                    continue;
                }
                if (k >= 0x20 && k < 0x7f) {
                    if (focus == 0 && ulen < 30) {
                        username[ulen++] = (char)k;
                        username[ulen] = '\0';
                    }
                    if (focus == 1 && plen < 30) {
                        password[plen++] = (char)k;
                        password[plen] = '\0';
                    }
                    draw(fd);
                }
            }
        }
        /* yield to avoid spinning */
        __asm__ volatile("syscall" : : "a"((long)ABI_SYS_YIELD)
                         : "rcx","r11","memory");
    }

    _exit(0);
    return 0;
}
