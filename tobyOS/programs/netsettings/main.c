/* netsettings/main.c -- Network configuration GUI for tobyOS.
 *
 * 650x450 window with four tabs: WiFi, Ethernet, Firewall, VPN.
 * Reads/writes /system/config/network.conf for persistent settings.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYS_EXIT            0
#define SYS_WRITE           1
#define SYS_READ            2
#define SYS_YIELD           5
#define SYS_CLOSE           4
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_GUI_ROUNDED_RECT 82
#define SYS_GUI_LINE       80
#define SYS_OPEN           35
#define SYS_SETTING_GET    21
#define SYS_SETTING_SET    22
#define SYS_GUI_TEXT_SCALED 56
#define SYS_CLOCK_MS       48

struct gui_event { int type,x,y; unsigned char button,key,_pad[2]; };
#define EV_MOUSE_DOWN 2
#define EV_KEY        4
#define EV_CLOSE      5

static inline long sc1(long n,long a){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a):"rcx","r11","memory");return r;}
static inline long sc2(long n,long a,long b){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b):"rcx","r11","memory");return r;}
static inline long sc3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
static inline long sc5(long n,long a,long b,long c,long d,long e){long r;register long r10 __asm__("r10")=d;register long r8 __asm__("r8")=e;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");return r;}

static int g_win = -1;
#define W 650
#define H 450

#define C_BG       0x1e1e2e
#define C_HEADER   0x252540
#define C_TAB      0x333355
#define C_TAB_SEL  0x4455aa
#define C_TEXT     0xffffff
#define C_DIM      0x888899
#define C_INPUT    0x333355
#define C_BTN      0x4455aa
#define C_BTN_HL   0x5566bb
#define C_ACCENT   0x5588ff
#define C_GREEN    0x44cc44
#define C_RED      0xcc4444
#define C_DIVIDER  0x404060
#define C_PANEL    0x252540

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

static int g_tab;  /* 0=WiFi, 1=Ethernet, 2=Firewall, 3=VPN */

/* Ethernet settings */
static int  g_dhcp = 1;
static char g_ip[16]      = "192.168.1.100";
static char g_mask[16]    = "255.255.255.0";
static char g_gateway[16] = "192.168.1.1";
static char g_dns[16]     = "8.8.8.8";

/* Firewall rules */
#define MAX_FW_RULES 8
struct fw_rule_ui {
    char app[32];
    int  allowed;
};
static struct fw_rule_ui g_fw_rules[MAX_FW_RULES];
static int g_fw_count;

/* VPN settings */
static char g_vpn_endpoint[64] = "vpn.example.com:51820";
static char g_vpn_privkey[48]  = "";
static char g_vpn_allowed[48]  = "0.0.0.0/0";
static int  g_vpn_active;

/* Input field tracking */
static int g_active_field = -1;
static char *g_active_buf;
static int  g_active_max;
static int  g_active_cursor;

static const char *g_tab_labels[] = {"WiFi", "Ethernet", "Firewall", "VPN"};
#define TAB_COUNT 4
#define TAB_W     (W / TAB_COUNT)
#define TAB_H     32
#define CONTENT_Y 42

static void init_firewall(void) {
    g_fw_count = 3;
    strncpy(g_fw_rules[0].app, "browser", 31);  g_fw_rules[0].allowed = 1;
    strncpy(g_fw_rules[1].app, "terminal", 31); g_fw_rules[1].allowed = 1;
    strncpy(g_fw_rules[2].app, "updater", 31);  g_fw_rules[2].allowed = 1;
}

static void draw_input(int x, int y, int w, const char *label, const char *val, int field_id) {
    gui_text(x, y, label, C_DIM);
    unsigned bg = (g_active_field == field_id) ? 0x444466 : C_INPUT;
    gui_rrect(x, y + 16, w, 24, 4, bg);
    gui_text(x + 6, y + 20, val, C_TEXT);
}

static void draw_button(int x, int y, int w, int h, const char *label, unsigned color) {
    gui_rrect(x, y, w, h, 4, color);
    int tx = x + (w - (int)strlen(label) * 8) / 2;
    gui_text(tx, y + (h - 10) / 2, label, C_TEXT);
}

static void draw_tab_wifi(void) {
    int y = CONTENT_Y + 10;
    gui_text_sc(20, y, "WiFi Settings", C_TEXT, C_BG, 2);
    y += 30;

    gui_text(20, y, "WiFi is managed by the WiFi Picker.", C_DIM);
    y += 20;
    gui_text(20, y, "Click the WiFi icon in the system tray", C_DIM);
    y += 20;
    gui_text(20, y, "to scan and connect to networks.", C_DIM);
    y += 40;

    gui_text(20, y, "Status:", C_DIM);
    gui_text(80, y, "Connected to HomeNetwork", C_GREEN);
    y += 20;
    gui_text(20, y, "IP:", C_DIM);
    gui_text(80, y, "192.168.1.42", C_TEXT);
    y += 20;
    gui_text(20, y, "Signal:", C_DIM);
    gui_text(80, y, "Excellent (85%)", C_TEXT);
    y += 20;
    gui_text(20, y, "Security:", C_DIM);
    gui_text(80, y, "WPA2", C_TEXT);
    y += 30;

    draw_button(20, y, 160, 30, "Open WiFi Picker", C_BTN);
}

static void draw_tab_ethernet(void) {
    int y = CONTENT_Y + 10;
    gui_text_sc(20, y, "Ethernet", C_TEXT, C_BG, 2);
    y += 35;

    /* DHCP toggle */
    gui_text(20, y, "DHCP:", C_DIM);
    unsigned toggle_c = g_dhcp ? 0x44aa44 : 0x555566;
    gui_rrect(80, y - 2, 44, 20, 10, toggle_c);
    int knob = g_dhcp ? (80 + 24) : (80 + 2);
    gui_rrect(knob, y, 16, 16, 8, C_TEXT);
    gui_text(140, y, g_dhcp ? "Automatic" : "Manual", C_DIM);
    y += 30;

    if (!g_dhcp) {
        draw_input(20, y, 200, "IP Address", g_ip, 0);
        y += 50;
        draw_input(20, y, 200, "Subnet Mask", g_mask, 1);
        y += 50;
        draw_input(20, y, 200, "Gateway", g_gateway, 2);
        y += 50;
        draw_input(20, y, 200, "DNS Server", g_dns, 3);
        y += 50;
    } else {
        gui_text(20, y, "IP:", C_DIM);
        gui_text(100, y, "192.168.1.42", C_TEXT);
        y += 18;
        gui_text(20, y, "Mask:", C_DIM);
        gui_text(100, y, "255.255.255.0", C_TEXT);
        y += 18;
        gui_text(20, y, "Gateway:", C_DIM);
        gui_text(100, y, "192.168.1.1", C_TEXT);
        y += 18;
        gui_text(20, y, "DNS:", C_DIM);
        gui_text(100, y, "8.8.8.8", C_TEXT);
        y += 30;
    }

    draw_button(20, y, 100, 28, "Apply", C_ACCENT);
}

static void draw_tab_firewall(void) {
    int y = CONTENT_Y + 10;
    gui_text_sc(20, y, "Firewall", C_TEXT, C_BG, 2);
    y += 35;

    /* Column headers */
    gui_text(20, y, "Application", C_DIM);
    gui_text(200, y, "Status", C_DIM);
    gui_text(300, y, "Action", C_DIM);
    y += 20;
    gui_fill(20, y, W - 40, 1, C_DIVIDER);
    y += 6;

    for (int i = 0; i < g_fw_count; i++) {
        gui_text(20, y, g_fw_rules[i].app, C_TEXT);
        gui_text(200, y, g_fw_rules[i].allowed ? "Allowed" : "Blocked",
                 g_fw_rules[i].allowed ? C_GREEN : C_RED);

        unsigned btn_c = g_fw_rules[i].allowed ? C_RED : C_GREEN;
        const char *btn_l = g_fw_rules[i].allowed ? "Block" : "Allow";
        draw_button(300, y - 4, 70, 22, btn_l, btn_c);
        y += 32;
    }

    y += 10;
    draw_button(20, y, 120, 28, "Add Rule", C_BTN);
}

static void draw_tab_vpn(void) {
    int y = CONTENT_Y + 10;
    gui_text_sc(20, y, "VPN (WireGuard)", C_TEXT, C_BG, 2);
    y += 35;

    /* Status */
    gui_text(20, y, "Status:", C_DIM);
    gui_text(100, y, g_vpn_active ? "Connected" : "Disconnected",
             g_vpn_active ? C_GREEN : C_RED);
    y += 30;

    draw_input(20, y, 350, "Endpoint", g_vpn_endpoint, 10);
    y += 50;
    draw_input(20, y, 350, "Private Key", g_vpn_privkey, 11);
    y += 50;
    draw_input(20, y, 350, "Allowed IPs", g_vpn_allowed, 12);
    y += 60;

    unsigned conn_c = g_vpn_active ? C_RED : C_GREEN;
    const char *conn_l = g_vpn_active ? "Disconnect" : "Connect";
    draw_button(20, y, 130, 30, conn_l, conn_c);
    draw_button(170, y, 100, 30, "Apply", C_ACCENT);
}

static void draw(void) {
    gui_fill(0, 0, W, H, C_BG);

    /* Tab bar */
    for (int t = 0; t < TAB_COUNT; t++) {
        unsigned tc = (t == g_tab) ? C_TAB_SEL : C_TAB;
        gui_fill(t * TAB_W, 0, TAB_W, TAB_H, tc);
        int tx = t * TAB_W + (TAB_W - (int)strlen(g_tab_labels[t]) * 8) / 2;
        gui_text(tx, 10, g_tab_labels[t], C_TEXT);
    }
    gui_fill(0, TAB_H, W, 2, C_ACCENT);

    switch (g_tab) {
    case 0: draw_tab_wifi(); break;
    case 1: draw_tab_ethernet(); break;
    case 2: draw_tab_firewall(); break;
    case 3: draw_tab_vpn(); break;
    }

    gui_flip();
}

static void activate_field(int field_id) {
    g_active_field = field_id;
    switch (field_id) {
    case 0:  g_active_buf = g_ip;           g_active_max = 15; break;
    case 1:  g_active_buf = g_mask;         g_active_max = 15; break;
    case 2:  g_active_buf = g_gateway;      g_active_max = 15; break;
    case 3:  g_active_buf = g_dns;          g_active_max = 15; break;
    case 10: g_active_buf = g_vpn_endpoint; g_active_max = 63; break;
    case 11: g_active_buf = g_vpn_privkey;  g_active_max = 47; break;
    case 12: g_active_buf = g_vpn_allowed;  g_active_max = 47; break;
    default: g_active_field = -1; g_active_buf = NULL; return;
    }
    g_active_cursor = (int)strlen(g_active_buf);
}

static void handle_click(int mx, int my) {
    /* Tab bar */
    if (my < TAB_H) {
        int t = mx / TAB_W;
        if (t >= 0 && t < TAB_COUNT) { g_tab = t; g_active_field = -1; }
        return;
    }

    /* Ethernet DHCP toggle */
    if (g_tab == 1 && my >= CONTENT_Y + 43 && my <= CONTENT_Y + 63 && mx >= 80 && mx <= 124) {
        g_dhcp = !g_dhcp;
        g_active_field = -1;
        return;
    }

    /* Ethernet input fields */
    if (g_tab == 1 && !g_dhcp) {
        int field_y = CONTENT_Y + 75;
        for (int f = 0; f < 4; f++) {
            if (my >= field_y + 16 && my <= field_y + 40) {
                activate_field(f);
                return;
            }
            field_y += 50;
        }
    }

    /* Firewall toggle buttons */
    if (g_tab == 2) {
        int fy = CONTENT_Y + 71;
        for (int i = 0; i < g_fw_count; i++) {
            if (my >= fy - 4 && my <= fy + 18 && mx >= 300 && mx <= 370) {
                g_fw_rules[i].allowed = !g_fw_rules[i].allowed;
                return;
            }
            fy += 32;
        }
    }

    /* VPN input fields */
    if (g_tab == 3) {
        int field_y = CONTENT_Y + 45;
        for (int f = 0; f < 3; f++) {
            if (my >= field_y + 16 && my <= field_y + 40) {
                activate_field(10 + f);
                return;
            }
            field_y += 50;
        }
        /* VPN connect/disconnect */
        int btn_y = CONTENT_Y + 205;
        if (my >= btn_y && my <= btn_y + 30 && mx >= 20 && mx <= 150) {
            g_vpn_active = !g_vpn_active;
            return;
        }
    }

    g_active_field = -1;
}

static void handle_key(unsigned char key) {
    if (g_active_field < 0 || !g_active_buf) return;

    if (key == 0x08) {
        if (g_active_cursor > 0) g_active_buf[--g_active_cursor] = '\0';
    } else if (key >= 0x20 && key < 0x7f && g_active_cursor < g_active_max) {
        g_active_buf[g_active_cursor++] = (char)key;
        g_active_buf[g_active_cursor] = '\0';
    } else if (key == 0x09) {
        /* Tab to next field */
        if (g_tab == 1 && !g_dhcp && g_active_field < 3) activate_field(g_active_field + 1);
        else if (g_tab == 3 && g_active_field >= 10 && g_active_field < 12) activate_field(g_active_field + 1);
    }
}

int main(void) {
    g_win = (int)sc3(SYS_GUI_CREATE, W, H, (long)"Network Settings");
    if (g_win < 0) { sc1(SYS_EXIT, 1); return 1; }

    g_tab = 0;
    g_active_field = -1;
    g_vpn_active = 0;
    init_firewall();
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
