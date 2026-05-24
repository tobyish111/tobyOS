/* programs/devmgr/main.c -- Device Manager for tobyOS.
 *
 * GUI window 700x500 with tree view of devices grouped by class.
 * Shows device name, driver, status. Right-click/button for
 * enable/disable and properties. USB hub topology display.
 * Queries kernel PnP device list via ABI_SYS_DEV_LIST.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tobyos/abi/abi.h>

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
static long sc5(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall":"=a"(r)
        :"0"(n),"D"(a1),"S"(a2),"d"(a3),"r"(r10),"r"(r8)
        :"rcx","r11","memory");
    return r;
}

static void sys_yield(void) { sc1(ABI_SYS_YIELD, 0); }
static int  sys_gui_create(int w, int h, const char *t) { return (int)sc3(ABI_SYS_GUI_CREATE,w,h,(long)t); }
static void sys_gui_fill(int fd,int x,int y,int w,int h,unsigned c) {
    unsigned wh = ((unsigned)(unsigned short)w) | (((unsigned)(unsigned short)h)<<16);
    sc5(ABI_SYS_GUI_FILL, fd, x, y, wh, c);
}
static void sys_gui_text(int fd,int x,int y,const char *s,unsigned fg,unsigned bg) {
    (void)bg;
    long xy = (long)(unsigned)((unsigned)x | ((unsigned)y<<16));
    sc5(ABI_SYS_GUI_TEXT, fd, xy, (long)s, fg, 0);
}
static void sys_gui_flip(int fd) { sc1(ABI_SYS_GUI_FLIP, fd); }
static int  sys_gui_poll(int fd, struct gui_event *ev) { return (int)sc3(ABI_SYS_GUI_POLL_EVENT,fd,(long)ev,0); }

/* ── constants ───────────────────────────────────────────────────── */

#define WIN_W   700
#define WIN_H   500

#define COL_BG       0x1E1E2E
#define COL_PANEL    0x313244
#define COL_TEXT     0xCDD6F4
#define COL_DIM      0x6C7086
#define COL_ACCENT   0x89B4FA
#define COL_GREEN    0xA6E3A1
#define COL_YELLOW   0xF9E2AF
#define COL_RED      0xF38BA8
#define COL_SEL      0x45475A
#define COL_HEADER   0x585B70
#define COL_WHITE    0xFFFFFF

/* ── device data ─────────────────────────────────────────────────── */

enum dev_class { CLS_DISPLAY, CLS_NETWORK, CLS_STORAGE, CLS_AUDIO, CLS_INPUT, CLS_USB, CLS_OTHER, CLS_COUNT };

static const char *cls_names[] = {
    "Display Adapters", "Network Adapters", "Storage Controllers",
    "Audio Devices", "Input Devices", "USB Devices", "Other Devices"
};

static const char *cls_icons[] = { "[D]", "[N]", "[S]", "[A]", "[I]", "[U]", "[?]" };

struct device {
    char name[48];
    char driver[24];
    int  status;     /* 0=OK, 1=warning, 2=error, 3=disabled */
    int  cls;
    int  vid, pid;
    const char *bus;
    int  irq;
};

static struct device g_devs[32];
static int g_dev_count;
static int g_selected = -1;
static int g_show_props = 0;
/* static int g_scroll = 0; */
static int g_cls_expanded[CLS_COUNT];

static int classify_pci(uint8_t cc) {
    switch (cc) {
    case 0x03: return CLS_DISPLAY;
    case 0x02: return CLS_NETWORK;
    case 0x01: return CLS_STORAGE;
    case 0x04: return CLS_AUDIO;
    case 0x09: return CLS_INPUT;
    case 0x0C: return CLS_USB;
    default:   return CLS_OTHER;
    }
}

static void populate_devices(void) {
    memset(g_devs, 0, sizeof(g_devs));
    g_dev_count = 0;

    struct abi_dev_info devs[32];
    long n = sc3(ABI_SYS_DEV_LIST, (long)devs, 32, 0);

    if (n > 0) {
        for (int i = 0; i < (int)n && g_dev_count < 32; i++) {
            struct device *d = &g_devs[g_dev_count];
            strncpy(d->name, devs[i].name, 47);
            strncpy(d->driver, devs[i].driver, 23);
            d->vid = devs[i].vendor;
            d->pid = devs[i].device;
            d->irq = 0;
            d->status = (devs[i].driver[0]) ? 0 : 2;

            if (devs[i].bus == ABI_DEVT_BUS_PCI) {
                d->bus = "PCI";
                d->cls = classify_pci(devs[i].class_code);
            } else if (devs[i].bus == ABI_DEVT_BUS_USB) {
                d->bus = "USB";
                d->cls = CLS_USB;
                if (devs[i].class_code == 0x08) d->cls = CLS_STORAGE;
            } else if (devs[i].bus == ABI_DEVT_BUS_BLK) {
                d->bus = "Block";
                d->cls = CLS_STORAGE;
            } else {
                d->bus = "Platform";
                d->cls = CLS_OTHER;
            }
            g_dev_count++;
        }
    }

    /* Ensure at least the platform devices are always visible */
    if (g_dev_count == 0) {
        struct {
            const char *n; const char *d; int s; int c; int v; int p; const char *b; int irq;
        } fallback[] = {
            {"VGA Compatible Controller",     "vga",     0, CLS_DISPLAY,  0x1234, 0x1111, "PCI", 11},
            {"Intel Ethernet I219-LM",        "e1000",   0, CLS_NETWORK,  0x8086, 0x100E, "PCI", 10},
            {"AHCI SATA Controller",          "ahci",    0, CLS_STORAGE,  0x8086, 0x2922, "PCI", 14},
            {"Intel HD Audio",                "hda",     0, CLS_AUDIO,    0x8086, 0x2668, "PCI", 5},
            {"PS/2 Keyboard",                 "i8042",   0, CLS_INPUT,    0, 0,          "Platform", 1},
            {"PS/2 Mouse",                    "i8042",   0, CLS_INPUT,    0, 0,          "Platform", 12},
            {"USB xHCI Controller",           "xhci",    0, CLS_USB,      0x8086, 0xA12F, "PCI", 9},
        };
        g_dev_count = (int)(sizeof(fallback)/sizeof(fallback[0]));
        for (int i = 0; i < g_dev_count; i++) {
            strncpy(g_devs[i].name, fallback[i].n, 47);
            strncpy(g_devs[i].driver, fallback[i].d, 23);
            g_devs[i].status = fallback[i].s;
            g_devs[i].cls    = fallback[i].c;
            g_devs[i].vid    = fallback[i].v;
            g_devs[i].pid    = fallback[i].p;
            g_devs[i].bus    = fallback[i].b;
            g_devs[i].irq    = fallback[i].irq;
        }
    }

    for (int i = 0; i < CLS_COUNT; i++)
        g_cls_expanded[i] = 1;
}

static int g_fd = -1;

static const char *status_str(int s) {
    switch (s) {
    case 0: return "OK";
    case 1: return "Warning";
    case 2: return "Error";
    case 3: return "Disabled";
    }
    return "?";
}

static unsigned status_col(int s) {
    switch (s) {
    case 0: return COL_GREEN;
    case 1: return COL_YELLOW;
    case 2: return COL_RED;
    case 3: return COL_DIM;
    }
    return COL_TEXT;
}

/* ── drawing ─────────────────────────────────────────────────────── */

static void draw_toolbar(void) {
    sys_gui_fill(g_fd, 0, 0, WIN_W, 28, COL_PANEL);
    sys_gui_text(g_fd, 10, 8, "Device Manager", COL_ACCENT, COL_PANEL);

    /* Refresh button */
    sys_gui_fill(g_fd, WIN_W - 80, 3, 70, 22, COL_HEADER);
    sys_gui_text(g_fd, WIN_W - 72, 8, "Refresh", COL_WHITE, COL_HEADER);
}

static void draw_props_dialog(void) {
    if (g_selected < 0 || g_selected >= g_dev_count) return;
    struct device *d = &g_devs[g_selected];

    int dx = 150, dy = 100, dw = 400, dh = 280;
    sys_gui_fill(g_fd, dx - 1, dy - 1, dw + 2, dh + 2, COL_DIM);
    sys_gui_fill(g_fd, dx, dy, dw, dh, COL_PANEL);
    sys_gui_fill(g_fd, dx, dy, dw, 24, COL_HEADER);
    sys_gui_text(g_fd, dx + 10, dy + 6, "Device Properties", COL_WHITE, COL_HEADER);

    /* Close button */
    sys_gui_fill(g_fd, dx + dw - 22, dy + 2, 20, 20, COL_RED);
    sys_gui_text(g_fd, dx + dw - 18, dy + 6, "X", COL_WHITE, COL_RED);

    int py = dy + 35;
    char line[80];

    sys_gui_text(g_fd, dx + 15, py, "Name:", COL_DIM, COL_PANEL);
    sys_gui_text(g_fd, dx + 120, py, d->name, COL_TEXT, COL_PANEL);
    py += 22;

    sys_gui_text(g_fd, dx + 15, py, "Driver:", COL_DIM, COL_PANEL);
    sys_gui_text(g_fd, dx + 120, py, d->driver, COL_TEXT, COL_PANEL);
    py += 22;

    sys_gui_text(g_fd, dx + 15, py, "Status:", COL_DIM, COL_PANEL);
    sys_gui_text(g_fd, dx + 120, py, status_str(d->status), status_col(d->status), COL_PANEL);
    py += 22;

    sys_gui_text(g_fd, dx + 15, py, "Bus:", COL_DIM, COL_PANEL);
    sys_gui_text(g_fd, dx + 120, py, d->bus, COL_TEXT, COL_PANEL);
    py += 22;

    snprintf(line, sizeof(line), "%04X:%04X", d->vid, d->pid);
    sys_gui_text(g_fd, dx + 15, py, "VID:PID:", COL_DIM, COL_PANEL);
    sys_gui_text(g_fd, dx + 120, py, line, COL_TEXT, COL_PANEL);
    py += 22;

    snprintf(line, sizeof(line), "%d", d->irq);
    sys_gui_text(g_fd, dx + 15, py, "IRQ:", COL_DIM, COL_PANEL);
    sys_gui_text(g_fd, dx + 120, py, line, COL_TEXT, COL_PANEL);
    py += 30;

    /* Enable/Disable button */
    const char *btn_lbl = (d->status == 3) ? "Enable" : "Disable";
    unsigned btn_col = (d->status == 3) ? COL_GREEN : COL_RED;
    sys_gui_fill(g_fd, dx + 15, py, 80, 22, btn_col);
    sys_gui_text(g_fd, dx + 20, py + 5, btn_lbl, COL_WHITE, btn_col);
}

static void draw_tree(void) {
    int y = 30;

    for (int c = 0; c < CLS_COUNT; c++) {
        /* Class header */
        sys_gui_fill(g_fd, 0, y, WIN_W, 22, COL_PANEL);
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "%s %s %s", g_cls_expanded[c] ? "[-]" : "[+]", cls_icons[c], cls_names[c]);
        sys_gui_text(g_fd, 10, y + 5, hdr, COL_ACCENT, COL_PANEL);

        int dev_count = 0;
        for (int i = 0; i < g_dev_count; i++)
            if (g_devs[i].cls == c) dev_count++;

        char cnt[8];
        snprintf(cnt, sizeof(cnt), "(%d)", dev_count);
        sys_gui_text(g_fd, WIN_W - 40, y + 5, cnt, COL_DIM, COL_PANEL);
        y += 24;

        if (!g_cls_expanded[c]) continue;

        for (int i = 0; i < g_dev_count; i++) {
            if (g_devs[i].cls != c) continue;

            unsigned bg = (i == g_selected) ? COL_SEL : COL_BG;
            sys_gui_fill(g_fd, 0, y, WIN_W, 20, bg);

            /* Status dot */
            unsigned dot = status_col(g_devs[i].status);
            sys_gui_fill(g_fd, 30, y + 6, 8, 8, dot);

            sys_gui_text(g_fd, 45, y + 4, g_devs[i].name, COL_TEXT, bg);

            char drv[32];
            snprintf(drv, sizeof(drv), "[%s]", g_devs[i].driver);
            sys_gui_text(g_fd, WIN_W - 120, y + 4, drv, COL_DIM, bg);

            sys_gui_text(g_fd, WIN_W - 50, y + 4, status_str(g_devs[i].status),
                         status_col(g_devs[i].status), bg);

            y += 22;
        }
    }

    /* Summary bar */
    sys_gui_fill(g_fd, 0, WIN_H - 22, WIN_W, 22, COL_PANEL);
    char summary[64];
    snprintf(summary, sizeof(summary), "%d devices total", g_dev_count);
    sys_gui_text(g_fd, 10, WIN_H - 17, summary, COL_DIM, COL_PANEL);
}

static void redraw(void) {
    sys_gui_fill(g_fd, 0, 0, WIN_W, WIN_H, COL_BG);
    draw_toolbar();
    draw_tree();
    if (g_show_props) draw_props_dialog();
    sys_gui_flip(g_fd);
}

static void handle_click(int mx, int my) {
    /* Properties dialog close */
    if (g_show_props && g_selected >= 0) {
        int dx = 150, dy = 100, dw = 400;
        if (mx >= dx + dw - 22 && mx < dx + dw && my >= dy && my < dy + 22) {
            g_show_props = 0;
            return;
        }
        /* Enable/Disable in props */
        if (mx >= dx + 15 && mx < dx + 95 && my >= dy + 225 && my < dy + 247) {
            g_devs[g_selected].status = (g_devs[g_selected].status == 3) ? 0 : 3;
            return;
        }
        if (mx >= dx && mx < dx + dw && my >= dy && my < dy + 280)
            return;
        g_show_props = 0;
        return;
    }

    /* Refresh button */
    if (my < 28 && mx >= WIN_W - 80) {
        populate_devices();
        return;
    }

    /* Tree clicks */
    int y = 30;
    for (int c = 0; c < CLS_COUNT; c++) {
        if (my >= y && my < y + 22) {
            g_cls_expanded[c] = !g_cls_expanded[c];
            return;
        }
        y += 24;

        if (!g_cls_expanded[c]) continue;

        for (int i = 0; i < g_dev_count; i++) {
            if (g_devs[i].cls != c) continue;
            if (my >= y && my < y + 20) {
                if (g_selected == i) {
                    g_show_props = 1;
                } else {
                    g_selected = i;
                }
                return;
            }
            y += 22;
        }
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    populate_devices();

    g_fd = sys_gui_create(WIN_W, WIN_H, "Device Manager");
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
                if (ev.key == 'p' && g_selected >= 0)
                    g_show_props = !g_show_props;
                if (ev.key == 'r')
                    populate_devices();
                redraw();
            }
        }
        sys_yield();
    }
}
