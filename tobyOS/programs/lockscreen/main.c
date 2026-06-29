/* lockscreen/main.c -- Lock screen overlay for tobyOS.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The lock model is unchanged from
 * the original raw-gui_* version: a splash phase showing the clock + avatar,
 * an input phase with a masked password field, ABI_SYS_LOGIN authentication, a
 * shake animation on a wrong password, and a brief retry lock-out. Because the
 * whole screen is bespoke (big centred clock, avatar circle, shake offset), it
 * renders through a single full-window TK_CANVAS and drives its own paced loop
 * (tk_pump + periodic tk_redraw); keys arrive via the window-level key hook.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <tobyos/abi/abi.h>
#include <toby/tk.h>

static inline long sc0(long nr){long r;__asm__ volatile("syscall":"=a"(r):"0"(nr):"rcx","r11","memory");return r;}
static inline long sc2(long nr,long a1,long a2){long r;__asm__ volatile("syscall":"=a"(r):"0"(nr),"D"(a1),"S"(a2):"rcx","r11","memory");return r;}
static long sys_login(const char *u, const char *p) { return sc2(ABI_SYS_LOGIN, (long)u, (long)p); }
static long sys_clock_ms(void) { return sc0(ABI_SYS_CLOCK_MS); }
static long sys_username(char *buf, int sz) { return sc2(ABI_SYS_USERNAME, (long)buf, sz); }

#define W 1024
#define H 768

#define COL_BG    0x000A0A1Au
#define COL_TIME  0x00FFFFFFu
#define COL_DATE  0x0088AACCu
#define COL_HINT  0x00556688u
#define COL_FIELD 0x001A2A3Au
#define COL_FG    0x00FFFFFFu
#define COL_BTN   0x003366CCu
#define COL_ERR   0x00FF4444u

static char password[32];
static int  plen;
static int  phase;       /* 0=splash, 1=input */
static int  err_shown;
static long err_time;
static char cur_user[32];
static int  shake_offset;
static long shake_time;
static int  retry_locked;
static long retry_unlock_time;

static struct tk_window win;

static void format_time(long ms, char *buf) {
    long total_s = ms / 1000;
    int h = (int)((total_s / 3600) % 24);
    int m = (int)((total_s / 60) % 60);
    buf[0] = '0' + h/10; buf[1] = '0' + h%10; buf[2] = ':';
    buf[3] = '0' + m/10; buf[4] = '0' + m%10; buf[5] = '\0';
}

/* field / button geometry (paint + hit-test share these) */
#define FX (W/2 - 140)
#define FY (H/2 + 80)
#define FW 280
#define FH 30
#define BX (W/2 - 50)
#define BY (H/2 + 124)
#define BW 100
#define BH 32

static void paint(struct tk_window *w, struct tk_widget *c) {
    (void)c;
    long ms = sys_clock_ms();
    tk_draw_fill(w, 0, 0, W, H, COL_BG);

    /* big centred clock */
    char timebuf[8];
    format_time(ms, timebuf);
    int tw = tk_text_width(timebuf, 72, 1);
    tk_draw_text(w, W/2 - tw/2, H/2 - 210, timebuf, COL_TIME, 72, 1);

    int sub = tk_text_width("tobyOS", 20, 0);
    tk_draw_text(w, W/2 - sub/2, H/2 - 120, "tobyOS", COL_DATE, 20, 0);

    /* avatar */
    int cy = H/2 - 50;
    tk_draw_circle(w, W/2, cy, 36, COL_BTN);
    char init[2] = { '?', 0 };
    if (cur_user[0]) {
        init[0] = cur_user[0];
        if (init[0] >= 'a' && init[0] <= 'z') init[0] -= 32;
    }
    int iw = tk_text_width(init, 34, 1);
    tk_draw_text(w, W/2 - iw/2, cy - 18, init, COL_FG, 34, 1);

    if (cur_user[0]) {
        int uw = tk_text_width(cur_user, 18, 0);
        tk_draw_text(w, W/2 - uw/2, H/2 + 8, cur_user, COL_DATE, 18, 0);
    }

    /* shake offset for wrong-password animation */
    int sx = 0;
    if (shake_offset && ms - shake_time < 400) {
        int phase_ms = (int)((ms - shake_time) % 100);
        sx = (phase_ms < 50) ? 6 : -6;
        if (ms - shake_time > 200) sx = -sx;
    } else {
        shake_offset = 0;
    }

    if (phase == 0) {
        const char *hint = "Click or press any key";
        int hw = tk_text_width(hint, 16, 0);
        tk_draw_text(w, W/2 - hw/2, H/2 + 60, hint, COL_HINT, 16, 0);
    } else {
        tk_draw_rrect(w, FX + sx, FY, FW, FH, 4, COL_FIELD);
        if (retry_locked && ms < retry_unlock_time) {
            tk_draw_text(w, FX + sx + 8, FY + 7, "Please wait...", COL_HINT, 15, 0);
        } else {
            retry_locked = 0;
            if (plen > 0) {
                char dots[32];
                int dl = plen > 30 ? 30 : plen;
                for (int i = 0; i < dl; i++) dots[i] = '*';
                dots[dl] = '\0';
                tk_draw_text(w, FX + sx + 8, FY + 7, dots, COL_FG, 16, 0);
            } else {
                tk_draw_text(w, FX + sx + 8, FY + 7, "Password", COL_HINT, 15, 0);
            }
        }
        tk_draw_rrect(w, BX + sx, BY, BW, BH, 6, COL_BTN);
        tk_draw_text(w, BX + sx + 24, BY + 8, "Unlock", COL_FG, 16, 0);
        if (err_shown && ms - err_time < 2000) {
            const char *e = "Wrong password";
            int ew = tk_text_width(e, 16, 0);
            tk_draw_text(w, W/2 - ew/2, BY + BH + 14, e, COL_ERR, 16, 0);
        }
    }
}

static void try_unlock(void) {
    if (retry_locked) return;
    password[plen] = '\0';
    if (sys_login(cur_user, password) >= 0) _exit(0);
    err_shown = 1;
    err_time = sys_clock_ms();
    shake_offset = 1;
    shake_time = err_time;
    retry_locked = 1;
    retry_unlock_time = err_time + 1000;
    memset(password, 0, sizeof(password));
    plen = 0;
    tk_redraw(&win);
}

static void on_event(struct tk_window *w, struct tk_widget *c, struct tk_event *ev) {
    (void)c;
    if (ev->type != TK_EV_MOUSE_DOWN) return;
    if (phase == 0) { phase = 1; tk_redraw(w); return; }
    if (ev->x >= BX && ev->x < BX + BW && ev->y >= BY && ev->y < BY + BH)
        try_unlock();
}

static void on_key(struct tk_window *w, struct tk_event *ev) {
    unsigned char k = ev->key;
    if (phase == 0) { phase = 1; tk_redraw(w); return; }
    if (k == '\n' || k == '\r') { try_unlock(); return; }
    if (k == 0x1b) { phase = 0; plen = 0; password[0] = '\0'; tk_redraw(w); return; }
    if (k == 0x08) { if (plen > 0) password[--plen] = '\0'; tk_redraw(w); return; }
    if (k >= 0x20 && k < 0x7f && plen < 30) {
        password[plen++] = (char)k;
        password[plen] = '\0';
        tk_redraw(w);
    }
}

int main(void) {
    memset(password, 0, sizeof(password));
    plen = 0; phase = 0; err_shown = 0; shake_offset = 0; retry_locked = 0;
    sys_username(cur_user, 32);

    if (tk_window_open(&win, W, H, "Lock Screen") != 0)
        return 1;
    tk_maximize(&win);
    tk_on_key(&win, on_key);

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 0);
    struct tk_widget *cv = tk_canvas(&win, root, paint);
    tk_grow(cv, 1);
    tk_on_event(cv, on_event);

    long last_draw = 0;
    for (;;) {
        if (tk_pump(&win)) break;
        long now = sys_clock_ms();
        long interval = shake_offset ? 50 : 1000;
        if (now - last_draw >= interval) { tk_redraw(&win); last_draw = now; }
        usleep(30000);
    }
    _exit(0);
    return 0;
}
