/* programs/netui/main.c -- Network Settings UI for tobyOS.
 *
 * GUI window 600x400 with tabs: WiFi, Ethernet, VPN, Firewall.
 * Uses raw syscall stubs for the GUI and libtoby for string ops.
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
#define SYS_GUI_ROUNDED_RECT 40
#define SYS_GUI_LINE       41
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
/* static long sys_clock_ms(void) { return sc1(SYS_CLOCK_MS, 0); } */

/* ── constants ───────────────────────────────────────────────────── */

#define WIN_W   600
#define WIN_H   400

#define TAB_COUNT 4
#define TAB_H     30
#define TAB_W     (WIN_W / TAB_COUNT)

#define COL_BG       0x1E1E2E
#define COL_TAB_ACT  0x313244
#define COL_TAB_INACT 0x45475A
#define COL_TEXT     0xCDD6F4
#define COL_DIM      0x6C7086
#define COL_ACCENT   0x89B4FA
#define COL_GREEN    0xA6E3A1
#define COL_RED      0xF38BA8
#define COL_BTN      0x585B70
#define COL_BTN_HI   0x89B4FA
#define COL_INPUT_BG 0x313244
#define COL_WHITE    0xFFFFFF

static const char *tab_names[TAB_COUNT] = { "WiFi", "Ethernet", "VPN", "Firewall" };
static int g_active_tab = 0;
static int g_fd = -1;

/* ── WiFi state ──────────────────────────────────────────────────── */

struct wifi_entry {
    char name[33];
    int  signal;
    int  security;
    int  connected;
};

static struct wifi_entry g_wifi[8];
static int g_wifi_count = 0;
static int g_wifi_selected = -1;

static void wifi_populate(void) {
    struct { const char *n; int s; int sec; } demo[] = {
        {"HomeWiFi", 90, 2}, {"OfficeNet", 65, 2},
        {"CoffeeShop", 40, 0}, {"SecureNet_5G", 75, 3},
        {"Guest_Network", 25, 1},
    };
    g_wifi_count = 5;
    for (int i = 0; i < 5; i++) {
        memset(&g_wifi[i], 0, sizeof(g_wifi[i]));
        strncpy(g_wifi[i].name, demo[i].n, 32);
        g_wifi[i].signal = demo[i].s;
        g_wifi[i].security = demo[i].sec;
        g_wifi[i].connected = (i == 0) ? 1 : 0;
    }
}

/* ── Ethernet state ──────────────────────────────────────────────── */

static int g_eth_dhcp = 1;
static char g_eth_ip[16] = "192.168.1.100";
static char g_eth_mask[16] = "255.255.255.0";
static char g_eth_gw[16] = "192.168.1.1";
static char g_eth_dns[16] = "8.8.8.8";

/* ── VPN state ───────────────────────────────────────────────────── */

static int g_vpn_connected = 0;
static char g_vpn_endpoint[32] = "vpn.example.com";
static char g_vpn_port[8] = "51820";

/* ── Firewall state ──────────────────────────────────────────────── */

struct fw_entry {
    int id;
    char desc[32];
    int action; /* 0=allow, 1=deny */
    int port;
    int enabled;
};

static struct fw_entry g_fw_rules[8];
static int g_fw_count = 0;
static int g_fw_enabled = 0;

static void fw_populate(void) {
    struct { const char *d; int a; int p; } demo[] = {
        {"Allow HTTP",  0, 80},
        {"Allow HTTPS", 0, 443},
        {"Allow SSH",   0, 22},
        {"Deny Telnet", 1, 23},
    };
    g_fw_count = 4;
    for (int i = 0; i < 4; i++) {
        g_fw_rules[i].id = i + 1;
        strncpy(g_fw_rules[i].desc, demo[i].d, 31);
        g_fw_rules[i].action = demo[i].a;
        g_fw_rules[i].port = demo[i].p;
        g_fw_rules[i].enabled = 1;
    }
}

/* ── signal bars ─────────────────────────────────────────────────── */

static void draw_signal(int x, int y, int percent) {
    int bars = (percent > 80) ? 4 : (percent > 60) ? 3 : (percent > 30) ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int bh = 4 + i * 3;
        unsigned col = (i < bars) ? COL_GREEN : COL_DIM;
        sys_gui_fill(g_fd, x + i * 5, y + (16 - bh), 3, bh, col);
    }
}

/* ── button helper ───────────────────────────────────────────────── */

static int btn_hit(int bx, int by, int bw, int bh, int mx, int my) {
    return mx >= bx && mx < bx + bw && my >= by && my < by + bh;
}

static void draw_btn(int x, int y, int w, int h, const char *label, unsigned bg) {
    sys_gui_fill(g_fd, x, y, w, h, bg);
    int tx = x + (w - (int)strlen(label) * 8) / 2;
    int ty = y + (h - 8) / 2;
    sys_gui_text(g_fd, tx, ty, label, COL_WHITE, bg);
}

/* ── tab rendering ───────────────────────────────────────────────── */

static void draw_tabs(void) {
    for (int i = 0; i < TAB_COUNT; i++) {
        unsigned col = (i == g_active_tab) ? COL_TAB_ACT : COL_TAB_INACT;
        sys_gui_fill(g_fd, i * TAB_W, 0, TAB_W, TAB_H, col);
        int tx = i * TAB_W + (TAB_W - (int)strlen(tab_names[i]) * 8) / 2;
        sys_gui_text(g_fd, tx, 10, tab_names[i],
                     (i == g_active_tab) ? COL_ACCENT : COL_TEXT, col);
    }
    sys_gui_fill(g_fd, g_active_tab * TAB_W, TAB_H - 2, TAB_W, 2, COL_ACCENT);
}

/* ── WiFi tab ────────────────────────────────────────────────────── */

static void draw_wifi(void) {
    int y = TAB_H + 10;
    sys_gui_text(g_fd, 15, y, "Available Networks", COL_ACCENT, COL_BG);
    y += 20;

    draw_btn(WIN_W - 80, TAB_H + 5, 65, 22, "Scan", COL_BTN);

    for (int i = 0; i < g_wifi_count; i++) {
        unsigned bg = (i == g_wifi_selected) ? COL_TAB_ACT : COL_BG;
        sys_gui_fill(g_fd, 10, y, WIN_W - 20, 28, bg);

        char line[64];
        snprintf(line, sizeof(line), "%s", g_wifi[i].name);
        sys_gui_text(g_fd, 30, y + 6, line, COL_TEXT, bg);

        draw_signal(WIN_W - 100, y + 6, g_wifi[i].signal);

        const char *sec_names[] = {"Open","WEP","WPA2","WPA3"};
        int si = g_wifi[i].security;
        if (si < 0 || si > 3) si = 0;
        sys_gui_text(g_fd, WIN_W - 170, y + 6, sec_names[si], COL_DIM, bg);

        if (g_wifi[i].connected) {
            sys_gui_text(g_fd, WIN_W - 55, y + 6, "OK", COL_GREEN, bg);
        }

        y += 30;
    }

    y += 10;
    if (g_wifi_selected >= 0 && !g_wifi[g_wifi_selected].connected) {
        draw_btn(15, y, 90, 24, "Connect", COL_ACCENT);
    }
    if (g_wifi_selected >= 0 && g_wifi[g_wifi_selected].connected) {
        draw_btn(15, y, 100, 24, "Disconnect", COL_RED);
    }
}

/* ── Ethernet tab ────────────────────────────────────────────────── */

static void draw_ethernet(void) {
    int y = TAB_H + 15;
    sys_gui_text(g_fd, 15, y, "Ethernet Configuration", COL_ACCENT, COL_BG);
    y += 25;

    char line[64];

    /* DHCP toggle */
    draw_btn(15, y, 120, 22, g_eth_dhcp ? "DHCP: ON" : "DHCP: OFF",
             g_eth_dhcp ? COL_GREEN : COL_RED);
    y += 35;

    snprintf(line, sizeof(line), "IP Address:  %s", g_eth_ip);
    sys_gui_text(g_fd, 15, y, line, COL_TEXT, COL_BG);
    y += 20;

    snprintf(line, sizeof(line), "Subnet Mask: %s", g_eth_mask);
    sys_gui_text(g_fd, 15, y, line, COL_TEXT, COL_BG);
    y += 20;

    snprintf(line, sizeof(line), "Gateway:     %s", g_eth_gw);
    sys_gui_text(g_fd, 15, y, line, COL_TEXT, COL_BG);
    y += 20;

    snprintf(line, sizeof(line), "DNS Server:  %s", g_eth_dns);
    sys_gui_text(g_fd, 15, y, line, COL_TEXT, COL_BG);
    y += 30;

    sys_gui_text(g_fd, 15, y, "Status: Connected", COL_GREEN, COL_BG);
    y += 20;

    sys_gui_text(g_fd, 15, y, "Speed: 1000 Mbps Full Duplex", COL_DIM, COL_BG);
}

/* ── VPN tab ─────────────────────────────────────────────────────── */

static void draw_vpn(void) {
    int y = TAB_H + 15;
    sys_gui_text(g_fd, 15, y, "WireGuard VPN", COL_ACCENT, COL_BG);
    y += 25;

    char line[64];

    /* Status indicator */
    sys_gui_fill(g_fd, 15, y, 12, 12, g_vpn_connected ? COL_GREEN : COL_RED);
    snprintf(line, sizeof(line), "%s", g_vpn_connected ? "Connected" : "Disconnected");
    sys_gui_text(g_fd, 35, y + 2, line, COL_TEXT, COL_BG);
    y += 25;

    snprintf(line, sizeof(line), "Endpoint: %s", g_vpn_endpoint);
    sys_gui_text(g_fd, 15, y, line, COL_TEXT, COL_BG);
    y += 20;

    snprintf(line, sizeof(line), "Port: %s", g_vpn_port);
    sys_gui_text(g_fd, 15, y, line, COL_TEXT, COL_BG);
    y += 30;

    sys_gui_text(g_fd, 15, y, "Public Key:", COL_DIM, COL_BG);
    y += 15;
    sys_gui_text(g_fd, 15, y, "aB3d...xY9z (truncated)", COL_TEXT, COL_BG);
    y += 25;

    sys_gui_text(g_fd, 15, y, "Allowed IPs: 0.0.0.0/0", COL_TEXT, COL_BG);
    y += 20;

    sys_gui_text(g_fd, 15, y, "Keepalive: 25s", COL_DIM, COL_BG);
    y += 30;

    if (g_vpn_connected) {
        draw_btn(15, y, 110, 24, "Disconnect", COL_RED);
        y += 30;
        sys_gui_text(g_fd, 15, y, "Transfer: 12.4 MB sent, 45.2 MB recv", COL_DIM, COL_BG);
    } else {
        draw_btn(15, y, 90, 24, "Connect", COL_ACCENT);
    }
}

/* ── Firewall tab ────────────────────────────────────────────────── */

static void draw_firewall(void) {
    int y = TAB_H + 15;
    sys_gui_text(g_fd, 15, y, "Firewall Rules", COL_ACCENT, COL_BG);

    draw_btn(WIN_W - 130, TAB_H + 10, 55, 22, "Add", COL_BTN);
    draw_btn(WIN_W - 70, TAB_H + 10, 55, 22,
             g_fw_enabled ? "ON" : "OFF",
             g_fw_enabled ? COL_GREEN : COL_RED);
    y += 25;

    /* Header */
    sys_gui_fill(g_fd, 10, y, WIN_W - 20, 20, COL_TAB_ACT);
    sys_gui_text(g_fd, 15, y + 4, "#", COL_DIM, COL_TAB_ACT);
    sys_gui_text(g_fd, 40, y + 4, "Description", COL_DIM, COL_TAB_ACT);
    sys_gui_text(g_fd, 300, y + 4, "Action", COL_DIM, COL_TAB_ACT);
    sys_gui_text(g_fd, 400, y + 4, "Port", COL_DIM, COL_TAB_ACT);
    sys_gui_text(g_fd, 500, y + 4, "Status", COL_DIM, COL_TAB_ACT);
    y += 22;

    for (int i = 0; i < g_fw_count; i++) {
        unsigned bg = (i % 2 == 0) ? COL_BG : 0x262637;
        sys_gui_fill(g_fd, 10, y, WIN_W - 20, 22, bg);

        char num[8];
        snprintf(num, sizeof(num), "%d", g_fw_rules[i].id);
        sys_gui_text(g_fd, 15, y + 5, num, COL_TEXT, bg);

        sys_gui_text(g_fd, 40, y + 5, g_fw_rules[i].desc, COL_TEXT, bg);

        sys_gui_text(g_fd, 300, y + 5,
                     g_fw_rules[i].action ? "DENY" : "ALLOW",
                     g_fw_rules[i].action ? COL_RED : COL_GREEN, bg);

        char port[8];
        snprintf(port, sizeof(port), "%d", g_fw_rules[i].port);
        sys_gui_text(g_fd, 400, y + 5, port, COL_TEXT, bg);

        sys_gui_text(g_fd, 500, y + 5,
                     g_fw_rules[i].enabled ? "ON" : "OFF",
                     g_fw_rules[i].enabled ? COL_GREEN : COL_DIM, bg);
        y += 24;
    }
}

/* ── full redraw ─────────────────────────────────────────────────── */

static void redraw(void) {
    sys_gui_fill(g_fd, 0, 0, WIN_W, WIN_H, COL_BG);
    draw_tabs();

    switch (g_active_tab) {
    case 0: draw_wifi();     break;
    case 1: draw_ethernet(); break;
    case 2: draw_vpn();      break;
    case 3: draw_firewall(); break;
    }

    /* Status bar */
    sys_gui_fill(g_fd, 0, WIN_H - 20, WIN_W, 20, COL_TAB_ACT);
    sys_gui_text(g_fd, 10, WIN_H - 15, "Network Settings", COL_DIM, COL_TAB_ACT);

    sys_gui_flip(g_fd);
}

/* ── event handling ──────────────────────────────────────────────── */

static void handle_click(int mx, int my) {
    /* Tab bar */
    if (my < TAB_H) {
        int tab = mx / TAB_W;
        if (tab >= 0 && tab < TAB_COUNT)
            g_active_tab = tab;
        return;
    }

    switch (g_active_tab) {
    case 0: /* WiFi */
        /* Scan button */
        if (btn_hit(WIN_W - 80, TAB_H + 5, 65, 22, mx, my)) {
            wifi_populate();
            break;
        }
        /* Network list selection */
        {
            int y0 = TAB_H + 30;
            for (int i = 0; i < g_wifi_count; i++) {
                if (my >= y0 && my < y0 + 28) {
                    g_wifi_selected = i;
                    break;
                }
                y0 += 30;
            }
        }
        /* Connect/Disconnect */
        {
            int by = TAB_H + 30 + g_wifi_count * 30 + 10;
            if (g_wifi_selected >= 0 && btn_hit(15, by, 110, 24, mx, my)) {
                g_wifi[g_wifi_selected].connected = !g_wifi[g_wifi_selected].connected;
            }
        }
        break;

    case 1: /* Ethernet - DHCP toggle */
        if (btn_hit(15, TAB_H + 40, 120, 22, mx, my))
            g_eth_dhcp = !g_eth_dhcp;
        break;

    case 2: /* VPN */
        {
            int by = TAB_H + 15 + 25 + 25 + 20 + 30 + 15 + 25 + 20 + 30;
            if (btn_hit(15, by, 110, 24, mx, my))
                g_vpn_connected = !g_vpn_connected;
        }
        break;

    case 3: /* Firewall */
        /* Enable/disable toggle */
        if (btn_hit(WIN_W - 70, TAB_H + 10, 55, 22, mx, my))
            g_fw_enabled = !g_fw_enabled;
        /* Add rule */
        if (btn_hit(WIN_W - 130, TAB_H + 10, 55, 22, mx, my)) {
            if (g_fw_count < 8) {
                struct fw_entry *r = &g_fw_rules[g_fw_count];
                r->id = g_fw_count + 1;
                snprintf(r->desc, sizeof(r->desc), "New Rule %d", r->id);
                r->action = 0;
                r->port = 8080 + g_fw_count;
                r->enabled = 1;
                g_fw_count++;
            }
        }
        break;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    wifi_populate();
    fw_populate();

    g_fd = sys_gui_create(WIN_W, WIN_H, "Network Settings");
    if (g_fd < 0) return 1;

    redraw();

    for (;;) {
        struct gui_event ev;
        while (sys_gui_poll(g_fd, &ev)) {
            if (ev.type == GUI_EV_CLOSE) return 0;
            if (ev.type == GUI_EV_MOUSE_DOWN) {
                handle_click(ev.x, ev.y);
                redraw();
            }
            if (ev.type == GUI_EV_KEY) {
                if (ev.key == '1') g_active_tab = 0;
                else if (ev.key == '2') g_active_tab = 1;
                else if (ev.key == '3') g_active_tab = 2;
                else if (ev.key == '4') g_active_tab = 3;
                redraw();
            }
        }
        sys_yield();
    }
}
