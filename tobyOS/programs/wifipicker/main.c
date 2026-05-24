/* wifipicker/main.c -- WiFi picker system tray popup for tobyOS.
 *
 * Frontend for the existing wifi.c backend. Shows available networks,
 * signal strength bars, security icons, password dialog, saved networks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Syscall numbers */
#define SYS_EXIT            0
#define SYS_WRITE           1
#define SYS_READ            2
#define SYS_YIELD           5
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_GUI_ROUNDED_RECT 82
#define SYS_GUI_LINE       80
#define SYS_SETTING_GET    21
#define SYS_SETTING_SET    22
#define SYS_CLOCK_MS       48
#define SYS_GUI_TEXT_SCALED 56

struct gui_event {
    int     type;
    int     x, y;
    unsigned char button;
    unsigned char key;
    unsigned char _pad[2];
};

#define EV_MOUSE_DOWN  2
#define EV_MOUSE_UP    3
#define EV_KEY         4
#define EV_CLOSE       5

static inline long sc1(long n,long a){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a):"rcx","r11","memory");return r;}
static inline long sc2(long n,long a,long b){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b):"rcx","r11","memory");return r;}
static inline long sc3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
static inline long sc5(long n,long a,long b,long c,long d,long e){long r;register long r10 __asm__("r10")=d;register long r8 __asm__("r8")=e;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");return r;}

static int g_win = -1;
#define W 320
#define H 480

/* Colors */
#define C_BG       0x1e1e2e
#define C_HEADER   0x252540
#define C_ITEM     0x2a2a45
#define C_ITEM_HL  0x3a3a60
#define C_CONN     0x2d4a2d
#define C_TEXT     0xffffff
#define C_DIM      0x888899
#define C_ACCENT   0x5588ff
#define C_LOCK     0xffaa33
#define C_GREEN    0x44cc44
#define C_BTN      0x4455aa
#define C_BTN_HL   0x5566bb
#define C_TOGGLE_ON  0x44aa44
#define C_TOGGLE_OFF 0x555566
#define C_INPUT_BG 0x333355
#define C_DIVIDER  0x404060

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
    unsigned rr=(unsigned)r;
    sc5(SYS_GUI_ROUNDED_RECT,g_win,(long)(unsigned)((unsigned)x|((unsigned)y<<16)),wh,rr,c);
}
static void sys_yield(void){sc1(SYS_YIELD,0);}

/* WiFi network data (simulated scan results) */
#define MAX_NETWORKS 12
#define MAX_SAVED    8

struct wifi_net {
    char ssid[33];
    int  signal;     /* 0-100 */
    int  security;   /* 0=open, 1=WEP, 2=WPA2, 3=WPA3 */
    int  connected;
};

static struct wifi_net g_nets[MAX_NETWORKS];
static int g_net_count;
static char g_saved[MAX_SAVED][33];
static int g_saved_count;
static int g_airplane_mode;
static int g_scroll;
static int g_selected;
static int g_show_password;
static char g_password[64];
static int g_pw_cursor;
static int g_connecting;
static int g_show_saved;

static void scan_networks(void) {
    /* Simulated scan results */
    g_net_count = 0;
    struct { const char *ssid; int sig; int sec; int conn; } demo[] = {
        {"HomeNetwork",     85, 2, 1},
        {"CoffeeShop_WiFi", 72, 0, 0},
        {"Office_5G",       65, 2, 0},
        {"Neighbor_Net",    45, 2, 0},
        {"FreeWiFi",        38, 0, 0},
        {"IoT_Device",      30, 1, 0},
        {"Hidden_Net",      22, 3, 0},
    };
    for (int i = 0; i < 7 && g_net_count < MAX_NETWORKS; i++) {
        struct wifi_net *n = &g_nets[g_net_count++];
        strncpy(n->ssid, demo[i].ssid, 32);
        n->ssid[32] = '\0';
        n->signal = demo[i].sig;
        n->security = demo[i].sec;
        n->connected = demo[i].conn;
    }

    g_saved_count = 0;
    strncpy(g_saved[g_saved_count++], "HomeNetwork", 32);
    strncpy(g_saved[g_saved_count++], "Office_5G", 32);
}

static void draw_signal_bars(int x, int y, int strength) {
    int bars = strength > 75 ? 4 : strength > 50 ? 3 : strength > 25 ? 2 : 1;
    for (int b = 0; b < 4; b++) {
        int bh = 4 + b * 3;
        int by = y + (16 - bh);
        unsigned color = (b < bars) ? C_GREEN : 0x444455;
        gui_fill(x + b * 5, by, 3, bh, color);
    }
}

static void draw_lock_icon(int x, int y, int sec) {
    if (sec == 0) return;
    gui_fill(x + 2, y, 4, 3, C_LOCK);
    gui_fill(x + 1, y + 3, 6, 5, C_LOCK);
}

static void draw(void) {
    gui_fill(0, 0, W, H, C_BG);

    /* Header */
    gui_fill(0, 0, W, 40, C_HEADER);
    gui_text_sc(12, 8, "WiFi", C_TEXT, C_HEADER, 2);

    /* Airplane mode toggle */
    unsigned toggle_c = g_airplane_mode ? C_TOGGLE_ON : C_TOGGLE_OFF;
    gui_rrect(W - 60, 10, 48, 22, 11, toggle_c);
    int knob_x = g_airplane_mode ? (W - 60 + 26) : (W - 60 + 2);
    gui_rrect(knob_x, 12, 18, 18, 9, C_TEXT);
    gui_text(W - 135, 14, "Airplane", C_DIM);

    /* Refresh button */
    gui_rrect(W - 155, 8, 50, 24, 4, C_BTN);
    gui_text(W - 148, 12, "Scan", C_TEXT);

    if (g_airplane_mode) {
        gui_text(80, 200, "Airplane Mode On", C_DIM);
        gui_text(60, 220, "WiFi is disabled", C_DIM);
        gui_flip();
        return;
    }

    /* Password dialog overlay */
    if (g_show_password) {
        gui_fill(0, 44, W, H - 44, C_BG);
        gui_fill(20, 100, W - 40, 200, C_HEADER);
        gui_rrect(20, 100, W - 40, 200, 8, C_HEADER);

        char title[64];
        snprintf(title, sizeof(title), "Connect to %s",
                 g_selected >= 0 ? g_nets[g_selected].ssid : "");
        gui_text(35, 115, title, C_TEXT);
        gui_text(35, 140, "Password:", C_DIM);

        /* Password input field */
        gui_rrect(35, 158, W - 70, 28, 4, C_INPUT_BG);
        char display[64];
        int len = (int)strlen(g_password);
        for (int i = 0; i < len && i < 60; i++) display[i] = '*';
        display[len < 60 ? len : 60] = '\0';
        gui_text(40, 164, display, C_TEXT);

        /* Connect button */
        gui_rrect(35, 210, 100, 30, 4, C_ACCENT);
        gui_text(55, 216, "Connect", C_TEXT);

        /* Cancel button */
        gui_rrect(W - 135, 210, 100, 30, 4, C_BTN);
        gui_text(W - 115, 216, "Cancel", C_TEXT);

        gui_flip();
        return;
    }

    int y = 44;

    /* Tabs: Networks / Saved */
    gui_fill(0, y, W / 2, 30, g_show_saved ? C_BG : C_ACCENT);
    gui_fill(W / 2, y, W / 2, 30, g_show_saved ? C_ACCENT : C_BG);
    gui_text(W / 4 - 32, y + 8, "Networks", g_show_saved ? C_DIM : C_TEXT);
    gui_text(3 * W / 4 - 20, y + 8, "Saved", g_show_saved ? C_TEXT : C_DIM);
    y += 34;

    if (g_show_saved) {
        /* Saved networks list */
        for (int i = 0; i < g_saved_count; i++) {
            int iy = y + i * 50;
            if (iy + 50 > H) break;

            gui_rrect(8, iy, W - 16, 44, 6, C_ITEM);
            gui_text(16, iy + 8, g_saved[i], C_TEXT);

            /* Check if this is the connected network */
            int is_connected = 0;
            for (int j = 0; j < g_net_count; j++) {
                if (g_nets[j].connected && strcmp(g_nets[j].ssid, g_saved[i]) == 0) {
                    is_connected = 1;
                    break;
                }
            }

            if (is_connected) {
                gui_text(16, iy + 24, "Connected", C_GREEN);
            }

            /* Forget button */
            gui_rrect(W - 72, iy + 8, 56, 24, 4, 0x663333);
            gui_text(W - 64, iy + 12, "Forget", 0xffaaaa);
        }
    } else {
        /* Network list */
        if (g_connecting) {
            gui_text(100, y + 10, "Connecting...", C_ACCENT);
            y += 30;
        }

        for (int i = g_scroll; i < g_net_count; i++) {
            int iy = y + (i - g_scroll) * 50;
            if (iy + 50 > H) break;

            struct wifi_net *n = &g_nets[i];
            unsigned item_c = n->connected ? C_CONN : (i == g_selected ? C_ITEM_HL : C_ITEM);
            gui_rrect(8, iy, W - 16, 44, 6, item_c);

            /* SSID */
            gui_text(16, iy + 6, n->ssid, C_TEXT);

            /* Signal bars */
            draw_signal_bars(W - 40, iy + 6, n->signal);

            /* Lock icon for secured networks */
            draw_lock_icon(W - 65, iy + 6, n->security);

            /* Connection status or security type */
            if (n->connected) {
                gui_text(16, iy + 24, "Connected  \x01", C_GREEN);
            } else {
                const char *sec_str = "Open";
                if (n->security == 1) sec_str = "WEP";
                else if (n->security == 2) sec_str = "WPA2";
                else if (n->security == 3) sec_str = "WPA3";

                char detail[48];
                snprintf(detail, sizeof(detail), "%s  %d%%", sec_str, n->signal);
                gui_text(16, iy + 24, detail, C_DIM);
            }
        }
    }

    gui_flip();
}

static void handle_click(int mx, int my) {
    /* Airplane toggle */
    if (my >= 10 && my <= 32 && mx >= W - 60 && mx <= W - 12) {
        g_airplane_mode = !g_airplane_mode;
        return;
    }

    /* Refresh button */
    if (my >= 8 && my <= 32 && mx >= W - 155 && mx <= W - 105) {
        scan_networks();
        return;
    }

    /* Tab buttons */
    if (my >= 44 && my <= 74) {
        g_show_saved = (mx >= W / 2);
        return;
    }

    /* Password dialog */
    if (g_show_password) {
        if (my >= 210 && my <= 240) {
            if (mx >= 35 && mx <= 135) {
                /* Connect */
                g_show_password = 0;
                g_connecting = 1;
                if (g_selected >= 0) g_nets[g_selected].connected = 1;
                g_connecting = 0;
            } else if (mx >= W - 135 && mx <= W - 35) {
                /* Cancel */
                g_show_password = 0;
                g_password[0] = '\0';
                g_pw_cursor = 0;
            }
        }
        return;
    }

    /* Saved networks: forget buttons */
    if (g_show_saved) {
        int y = 78;
        for (int i = 0; i < g_saved_count; i++) {
            int iy = y + i * 50;
            if (my >= iy + 8 && my <= iy + 32 && mx >= W - 72 && mx <= W - 16) {
                memmove(&g_saved[i], &g_saved[i + 1],
                        (g_saved_count - i - 1) * sizeof(g_saved[0]));
                g_saved_count--;
                return;
            }
        }
        return;
    }

    /* Network list clicks */
    int y_base = g_connecting ? 108 : 78;
    for (int i = g_scroll; i < g_net_count; i++) {
        int iy = y_base + (i - g_scroll) * 50;
        if (my >= iy && my <= iy + 44 && mx >= 8 && mx <= W - 8) {
            g_selected = i;
            if (g_nets[i].connected) return;
            if (g_nets[i].security == 0) {
                g_nets[i].connected = 1;
                for (int j = 0; j < g_net_count; j++)
                    if (j != i) g_nets[j].connected = 0;
            } else {
                g_show_password = 1;
                g_password[0] = '\0';
                g_pw_cursor = 0;
            }
            return;
        }
    }
}

static void handle_key(unsigned char key) {
    if (g_show_password) {
        if (key == 0x08) { /* backspace */
            if (g_pw_cursor > 0) g_password[--g_pw_cursor] = '\0';
        } else if (key == 0x0A) { /* enter */
            g_show_password = 0;
            g_connecting = 1;
            if (g_selected >= 0) {
                for (int j = 0; j < g_net_count; j++) g_nets[j].connected = 0;
                g_nets[g_selected].connected = 1;
            }
            g_connecting = 0;
        } else if (key >= 0x20 && key < 0x7f && g_pw_cursor < 62) {
            g_password[g_pw_cursor++] = (char)key;
            g_password[g_pw_cursor] = '\0';
        }
    }
}

int main(void) {
    g_win = (int)sc3(SYS_GUI_CREATE, W, H, (long)"WiFi");
    if (g_win < 0) { sc1(SYS_EXIT, 1); return 1; }

    g_airplane_mode = 0;
    g_scroll = 0;
    g_selected = -1;
    g_show_password = 0;
    g_show_saved = 0;
    g_connecting = 0;
    g_password[0] = '\0';
    g_pw_cursor = 0;

    scan_networks();
    draw();

    for (;;) {
        struct gui_event ev;
        while (gui_poll(&ev) > 0) {
            if (ev.type == EV_CLOSE) { sc1(SYS_EXIT, 0); return 0; }
            if (ev.type == EV_MOUSE_DOWN) handle_click(ev.x, ev.y);
            if (ev.type == EV_KEY) handle_key(ev.key);
            draw();
        }
        sys_yield();
    }
}
