/* volctrl/main.c -- Volume control popup for tobyOS.
 *
 * System tray volume control with master slider, mute toggle,
 * and per-app volume sliders.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYS_EXIT            0
#define SYS_WRITE           1
#define SYS_YIELD           5
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_GUI_ROUNDED_RECT 82
#define SYS_GUI_LINE       80
#define SYS_GUI_TEXT_SCALED 56
#define SYS_CLOCK_MS       48
#define SYS_SETTING_GET    21
#define SYS_SETTING_SET    22

struct gui_event { int type,x,y; unsigned char button,key,_pad[2]; };
#define EV_MOUSE_DOWN 2
#define EV_MOUSE_UP   3
#define EV_MOUSE_MOVE 1
#define EV_KEY        4
#define EV_CLOSE      5

static inline long sc1(long n,long a){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a):"rcx","r11","memory");return r;}
static inline long sc2(long n,long a,long b){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b):"rcx","r11","memory");return r;}
static inline long sc3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
static inline long sc5(long n,long a,long b,long c,long d,long e){long r;register long r10 __asm__("r10")=d;register long r8 __asm__("r8")=e;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");return r;}

static int g_win = -1;
#define W 280
#define H 380

#define C_BG       0x1e1e2e
#define C_HEADER   0x252540
#define C_TEXT     0xffffff
#define C_DIM      0x888899
#define C_SLIDER_BG 0x333355
#define C_SLIDER_FG 0x5588ff
#define C_SLIDER_MUTE 0x884444
#define C_KNOB     0xccccdd
#define C_BTN      0x4455aa
#define C_MUTE     0xcc4444
#define C_UNMUTE   0x44aa44
#define C_DIVIDER  0x404060
#define C_APP_BG   0x2a2a45

static void gui_fill(int x,int y,int w,int h,unsigned c){
    unsigned wh=((unsigned)(unsigned short)w)|(((unsigned)(unsigned short)h)<<16);
    sc5(SYS_GUI_FILL,g_win,x,y,wh,c);
}
static void gui_text(int x,int y,const char*s,unsigned fg){
    long xy=(long)(unsigned)((unsigned)x|((unsigned)y<<16));
    sc5(SYS_GUI_TEXT,g_win,xy,(long)s,fg,0);
}
static void gui_text_sc(int x,int y,const char*s,unsigned fg,unsigned bg,int scale){
    long xy=(long)(unsigned)((unsigned)x|((unsigned)y<<16));
    unsigned a5=(bg&0x00FFFFFFu)|((unsigned)(scale&0x7F)<<24);
    sc5(SYS_GUI_TEXT_SCALED,g_win,xy,(long)s,fg,a5);
}
static void gui_flip(void){sc1(SYS_GUI_FLIP,g_win);}
static long gui_poll(struct gui_event*ev){return sc2(SYS_GUI_POLL_EVENT,g_win,(long)ev);}
static void gui_rrect(int x,int y,int w,int h,int r,unsigned c){
    unsigned wh=((unsigned)(unsigned short)w)|(((unsigned)(unsigned short)h)<<16);
    sc5(SYS_GUI_ROUNDED_RECT,g_win,(long)(unsigned)((unsigned)x|((unsigned)y<<16)),wh,(unsigned)r,c);
}
static void sys_yield(void){sc1(SYS_YIELD,0);}

/* Volume state */
static int g_master_vol = 75;
static int g_muted;

#define MAX_APPS 8
struct app_vol {
    char name[24];
    int  volume;
    int  pid;
};
static struct app_vol g_apps[MAX_APPS];
static int g_app_count;

static int g_dragging = -1;  /* -1=none, 0=master, 1..N=app index */

/* Slider geometry */
#define SLIDER_X    20
#define SLIDER_W    200
#define SLIDER_H    8
#define KNOB_R      8

static void init_demo_apps(void) {
    g_app_count = 3;
    strncpy(g_apps[0].name, "Media Player", 23); g_apps[0].volume = 80; g_apps[0].pid = 10;
    strncpy(g_apps[1].name, "Browser", 23);      g_apps[1].volume = 60; g_apps[1].pid = 15;
    strncpy(g_apps[2].name, "Terminal", 23);      g_apps[2].volume = 40; g_apps[2].pid = 20;
}

static void draw_slider(int x, int y, int w, int vol, unsigned fg_color) {
    /* Track background */
    gui_rrect(x, y, w, SLIDER_H, 4, C_SLIDER_BG);

    /* Filled portion */
    int fill_w = vol * w / 100;
    if (fill_w > 0)
        gui_rrect(x, y, fill_w, SLIDER_H, 4, fg_color);

    /* Knob */
    int knob_x = x + fill_w - KNOB_R;
    if (knob_x < x) knob_x = x;
    gui_rrect(knob_x, y - (KNOB_R - SLIDER_H / 2), KNOB_R * 2, KNOB_R * 2, KNOB_R, C_KNOB);
}

static void draw_speaker_icon(int x, int y, int muted_flag) {
    unsigned c = muted_flag ? C_MUTE : C_TEXT;
    gui_fill(x, y + 4, 4, 8, c);
    gui_fill(x + 4, y + 2, 2, 12, c);
    gui_fill(x + 6, y, 2, 16, c);
    if (!muted_flag) {
        gui_fill(x + 12, y + 3, 1, 10, c);
        gui_fill(x + 15, y + 1, 1, 14, c);
    } else {
        /* X mark for muted */
        gui_fill(x + 12, y + 4, 6, 2, C_MUTE);
        gui_fill(x + 14, y + 2, 2, 6, C_MUTE);
    }
}

static void draw(void) {
    gui_fill(0, 0, W, H, C_BG);

    /* Header */
    gui_fill(0, 0, W, 40, C_HEADER);
    gui_text_sc(12, 8, "Volume", C_TEXT, C_HEADER, 2);

    int y = 50;

    /* Speaker icon + mute button */
    draw_speaker_icon(20, y, g_muted);
    gui_rrect(50, y - 2, 60, 20, 4, g_muted ? C_MUTE : C_UNMUTE);
    gui_text(56, y + 2, g_muted ? "Muted" : "Unmute", C_TEXT);

    /* Volume percentage */
    char vol_str[8];
    snprintf(vol_str, sizeof(vol_str), "%d%%", g_muted ? 0 : g_master_vol);
    gui_text(W - 50, y + 2, vol_str, C_TEXT);
    y += 28;

    /* Master volume label */
    gui_text(SLIDER_X, y, "Master Volume", C_DIM);
    y += 18;

    /* Master slider */
    unsigned master_c = g_muted ? C_SLIDER_MUTE : C_SLIDER_FG;
    draw_slider(SLIDER_X, y, SLIDER_W, g_muted ? 0 : g_master_vol, master_c);
    y += 30;

    /* Divider */
    gui_fill(10, y, W - 20, 1, C_DIVIDER);
    y += 10;

    /* Per-app volume */
    gui_text(SLIDER_X, y, "Application Volume", C_DIM);
    y += 22;

    for (int i = 0; i < g_app_count; i++) {
        /* App background */
        gui_rrect(10, y, W - 20, 52, 6, C_APP_BG);

        /* App name and volume */
        gui_text(20, y + 6, g_apps[i].name, C_TEXT);
        char app_vol[8];
        snprintf(app_vol, sizeof(app_vol), "%d%%", g_apps[i].volume);
        gui_text(W - 50, y + 6, app_vol, C_DIM);

        /* App slider */
        draw_slider(20, y + 30, SLIDER_W, g_apps[i].volume, C_SLIDER_FG);
        y += 58;
    }

    /* Output device selector */
    y += 5;
    gui_fill(10, y, W - 20, 1, C_DIVIDER);
    y += 10;
    gui_text(SLIDER_X, y, "Output Device", C_DIM);
    y += 18;
    gui_rrect(10, y, W - 20, 28, 4, C_APP_BG);
    gui_text(20, y + 6, "Speakers (Intel HDA)", C_TEXT);

    gui_flip();
}

static int vol_from_x(int mx) {
    int v = (mx - SLIDER_X) * 100 / SLIDER_W;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

static void handle_click(int mx, int my) {
    /* Mute button */
    if (my >= 48 && my <= 68 && mx >= 50 && mx <= 110) {
        g_muted = !g_muted;
        return;
    }

    /* Master slider area */
    int master_y = 96;
    if (my >= master_y - KNOB_R && my <= master_y + SLIDER_H + KNOB_R &&
        mx >= SLIDER_X && mx <= SLIDER_X + SLIDER_W + KNOB_R) {
        g_dragging = 0;
        g_master_vol = vol_from_x(mx);
        return;
    }

    /* App sliders */
    int app_base_y = 166;
    for (int i = 0; i < g_app_count; i++) {
        int sy = app_base_y + i * 58 + 30;
        if (my >= sy - KNOB_R && my <= sy + SLIDER_H + KNOB_R &&
            mx >= 20 && mx <= 20 + SLIDER_W + KNOB_R) {
            g_dragging = 1 + i;
            g_apps[i].volume = vol_from_x(mx);
            return;
        }
    }
}

static void handle_move(int mx, int my) {
    (void)my;
    if (g_dragging < 0) return;
    int v = vol_from_x(mx);
    if (g_dragging == 0) {
        g_master_vol = v;
    } else {
        int idx = g_dragging - 1;
        if (idx >= 0 && idx < g_app_count)
            g_apps[idx].volume = v;
    }
}

int main(void) {
    g_win = (int)sc3(SYS_GUI_CREATE, W, H, (long)"Volume");
    if (g_win < 0) { sc1(SYS_EXIT, 1); return 1; }

    g_muted = 0;
    g_dragging = -1;
    init_demo_apps();
    draw();

    for (;;) {
        struct gui_event ev;
        while (gui_poll(&ev) > 0) {
            if (ev.type == EV_CLOSE) { sc1(SYS_EXIT, 0); return 0; }
            if (ev.type == EV_MOUSE_DOWN) { handle_click(ev.x, ev.y); draw(); }
            if (ev.type == EV_MOUSE_MOVE && g_dragging >= 0) { handle_move(ev.x, ev.y); draw(); }
            if (ev.type == EV_MOUSE_UP) { g_dragging = -1; }
        }
        sys_yield();
    }
}
