/* lockscreen/main.c -- Lock screen overlay for tobyOS (Milestone 4).
 *
 * Full-screen overlay showing time and date. Click or keypress reveals
 * password field. Correct password dismisses; wrong password shows error.
 * Launched via Win+L from the desktop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tobyos/abi/abi.h>

static inline long sc0(long nr) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"0"(nr):"rcx","r11","memory");
    return r;
}
static inline long sc1(long nr, long a1) {
    long r;
    __asm__ volatile("syscall":"=a"(r)
        :"0"(nr),"D"(a1):"rcx","r11","memory");
    return r;
}
static inline long sc2(long nr, long a1, long a2) {
    long r;
    __asm__ volatile("syscall":"=a"(r)
        :"0"(nr),"D"(a1),"S"(a2):"rcx","r11","memory");
    return r;
}
static inline long sc3(long nr, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall":"=a"(r)
        :"0"(nr),"D"(a1),"S"(a2),"d"(a3):"rcx","r11","memory");
    return r;
}
static inline long sc5(long nr, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall":"=a"(r)
        :"0"(nr),"D"(a1),"S"(a2),"d"(a3),"r"(r10),"r"(r8)
        :"rcx","r11","memory");
    return r;
}

static int  gui_create(int w, int h, const char *t) { return (int)sc3(ABI_SYS_GUI_CREATE,w,h,(long)t); }
static void gui_fill(int fd, int x, int y, int w, int h, unsigned c) {
    unsigned wh = ((unsigned)(unsigned short)w) | (((unsigned)(unsigned short)h)<<16);
    sc5(ABI_SYS_GUI_FILL, fd, x, y, wh, c);
}
static void gui_text(int fd, int x, int y, const char *s, unsigned fg) {
    long xy = (long)(unsigned)((unsigned)x | ((unsigned)y<<16));
    sc5(ABI_SYS_GUI_TEXT, fd, xy, (long)s, fg, 0);
}
static void gui_text_scaled(int fd, int x, int y, const char *s,
                            unsigned fg, unsigned bg, int scale) {
    long xy = (long)(unsigned)((unsigned)x | ((unsigned)y<<16));
    unsigned a5 = (bg & 0x00FFFFFFu) | ((unsigned)(scale & 0x7F)<<24);
    sc5(ABI_SYS_GUI_TEXT_SCALED, fd, xy, (long)s, fg, a5);
}
static void gui_flip(int fd) { sc1(ABI_SYS_GUI_FLIP, fd); }
static void gui_set_state(int fd, int s) { sc2(ABI_SYS_GUI_SET_STATE, fd, s); }
static long gui_poll(int fd, void *ev) { return sc2(ABI_SYS_GUI_POLL_EVENT, fd, (long)ev); }
static long sys_login(const char *u, const char *p) { return sc2(ABI_SYS_LOGIN, (long)u, (long)p); }
static long sys_clock_ms(void) { return sc0(ABI_SYS_CLOCK_MS); }
static long sys_username(char *buf, int sz) { return sc2(ABI_SYS_USERNAME, (long)buf, sz); }

struct gui_event { int type, x, y; unsigned char button, key, _pad[2]; };
#define EV_CLOSE 5  /* match kernel GUI_EV_CLOSE; type 1 is MOUSE_MOVE */
#define EV_MOUSE_DOWN 2
#define EV_KEY 4

#define W 1024
#define H 768

#define COL_BG      0x0a0a1a
#define COL_TIME    0xffffff
#define COL_DATE    0x88aacc
#define COL_HINT    0x556688
#define COL_FIELD   0x1a2a3a
#define COL_FG      0xffffff
#define COL_BTN     0x3366cc
#define COL_ERR     0xff4444

static char password[32];
static int  plen;
static int  phase; /* 0=splash, 1=input */
static int  err_shown;
static long err_time;
static char cur_user[32];
static int  shake_offset;
static long shake_time;
static int  retry_locked;
static long retry_unlock_time;

static void format_time(long ms, char *buf)
{
    long total_s = ms / 1000;
    int h = (int)((total_s / 3600) % 24);
    int m = (int)((total_s / 60) % 60);
    buf[0] = '0' + h/10; buf[1] = '0' + h%10;
    buf[2] = ':';
    buf[3] = '0' + m/10; buf[4] = '0' + m%10;
    buf[5] = '\0';
}

static void draw_avatar(int fd, int cx, int cy) {
    /* Draw a circle-like avatar with user initials */
    int r = 36;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy <= r*r) {
                gui_fill(fd, cx + dx, cy + dy, 1, 1, 0x3366cc);
            }
        }
    }
    /* Draw initials */
    char init[4] = {0};
    if (cur_user[0]) {
        init[0] = cur_user[0];
        if (cur_user[0] >= 'a' && cur_user[0] <= 'z')
            init[0] -= 32;
    } else {
        init[0] = '?';
    }
    init[1] = '\0';
    gui_text_scaled(fd, cx - 8, cy - 10, init, COL_FG, 0x3366cc, 3);
}

static void draw(int fd)
{
    gui_fill(fd, 0, 0, W, H, COL_BG);

    long ms = sys_clock_ms();
    char timebuf[8];
    format_time(ms, timebuf);

    /* Large centered clock */
    gui_text_scaled(fd, W/2 - 60, H/2 - 150, timebuf, COL_TIME, COL_BG, 4);

    /* Date-like line (just uptime in this env) */
    gui_text(fd, W/2 - 40, H/2 - 90, "tobyOS", COL_DATE);

    /* User avatar */
    draw_avatar(fd, W/2, H/2 - 30);

    /* Username */
    if (cur_user[0]) {
        int ulen = (int)strlen(cur_user);
        gui_text(fd, W/2 - ulen * 4, H/2 + 15, cur_user, COL_DATE);
    }

    /* Compute shake offset for wrong password animation */
    int sx = 0;
    if (shake_offset && ms - shake_time < 400) {
        int phase_ms = (int)((ms - shake_time) % 100);
        sx = (phase_ms < 50) ? 6 : -6;
        if (ms - shake_time > 200) sx = -sx;
    } else {
        shake_offset = 0;
    }

    if (phase == 0) {
        gui_text(fd, W/2 - 80, H/2 + 50, "Click or press any key", COL_HINT);
    } else {
        /* Password input */
        int fx = W/2 - 140 + sx;
        int fy = H/2 + 40;
        gui_fill(fd, fx, fy, 280, 28, COL_FIELD);

        if (retry_locked && ms < retry_unlock_time) {
            gui_text(fd, fx + 8, fy + 7, "Please wait...", COL_HINT);
        } else {
            retry_locked = 0;
            if (plen > 0) {
                char dots[32];
                int dl = plen > 30 ? 30 : plen;
                for (int i = 0; i < dl; i++) dots[i] = '*';
                dots[dl] = '\0';
                gui_text(fd, fx + 8, fy + 7, dots, COL_FG);
            } else {
                gui_text(fd, fx + 8, fy + 7, "Password", COL_HINT);
            }
        }

        /* Unlock button */
        gui_fill(fd, W/2 - 50 + sx, fy + 40, 100, 30, COL_BTN);
        gui_text(fd, W/2 - 28 + sx, fy + 47, "Unlock", COL_FG);

        if (err_shown && ms - err_time < 2000) {
            gui_text(fd, W/2 - 60, fy + 80, "Wrong password", COL_ERR);
        }
    }

    gui_flip(fd);
}

static void try_unlock(int fd)
{
    if (retry_locked) return;

    password[plen] = '\0';
    long rc = sys_login(cur_user, password);
    if (rc >= 0) {
        _exit(0);
    }
    err_shown = 1;
    err_time = sys_clock_ms();

    /* Shake animation + 1s retry delay */
    shake_offset = 1;
    shake_time = err_time;
    retry_locked = 1;
    retry_unlock_time = err_time + 1000;

    memset(password, 0, sizeof(password));
    plen = 0;
    draw(fd);
}

int main(void)
{
    memset(password, 0, sizeof(password));
    plen = 0;
    phase = 0;
    err_shown = 0;
    shake_offset = 0;
    retry_locked = 0;
    sys_username(cur_user, 32);

    int fd = gui_create(W, H, "Lock Screen");
    if (fd < 0) return 1;
    gui_set_state(fd, 2); /* maximized */

    long last_draw = 0;
    struct gui_event ev;

    for (;;) {
        long now = sys_clock_ms();
        if (now - last_draw > 1000) {
            draw(fd);
            last_draw = now;
        }

        if (gui_poll(fd, &ev) > 0) {
            if (ev.type == EV_CLOSE) break;

            if (phase == 0) {
                if (ev.type == EV_KEY || ev.type == EV_MOUSE_DOWN) {
                    phase = 1;
                    draw(fd);
                }
                continue;
            }

            if (ev.type == EV_KEY) {
                unsigned char k = ev.key;
                if (k == '\n' || k == '\r') { try_unlock(fd); continue; }
                if (k == 0x1b) { phase = 0; plen = 0; draw(fd); continue; }
                if (k == 0x08 && plen > 0) { password[--plen] = '\0'; draw(fd); continue; }
                if (k >= 0x20 && k < 0x7f && plen < 30) {
                    password[plen++] = (char)k;
                    password[plen] = '\0';
                    draw(fd);
                }
            }

            if (ev.type == EV_MOUSE_DOWN) {
                int fy = H/2 + 30;
                if (ev.x >= W/2-50 && ev.x < W/2+50 &&
                    ev.y >= fy+40 && ev.y < fy+70)
                    try_unlock(fd);
            }
        }
        __asm__ volatile("syscall"::"a"((long)ABI_SYS_YIELD):"rcx","r11","memory");
    }
    _exit(0);
    return 0;
}
