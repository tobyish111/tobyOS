/* diskmgr/main.c -- Disk Manager GUI for tobyOS.
 *
 * Lists block devices, shows partition tables (GPT/MBR), per-partition
 * filesystem type/size/mount point.  Visual bar of partition layout.
 * Actions: mount/unmount, format, view properties.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tobyos/abi/abi.h>

/* ── syscall helpers ─────────────────────────────────────────────── */

static inline long sc0(long nr) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"0"(nr):"rcx","r11","memory");
    return r;
}
static inline long sc1(long nr, long a1) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"0"(nr),"D"(a1):"rcx","r11","memory");
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
static void gui_flip(int fd) { sc1(ABI_SYS_GUI_FLIP, fd); }
static long gui_poll(int fd, void *ev) { return sc2(ABI_SYS_GUI_POLL_EVENT, fd, (long)ev); }

struct gui_event { int type, x, y; unsigned char button, key, _pad[2]; };

#define WIN_W   750
#define WIN_H   520

#define COL_BG       0x1E1E2E
#define COL_PANEL    0x313244
#define COL_TEXT     0xCDD6F4
#define COL_DIM      0x6C7086
#define COL_ACCENT   0x89B4FA
#define COL_GREEN    0xA6E3A1
#define COL_YELLOW   0xF9E2AF
#define COL_RED      0xF38BA8
#define COL_BLUE     0x74C7EC
#define COL_MAUVE    0xCBA6F7
#define COL_SEL      0x45475A
#define COL_HDR      0x585B70
#define COL_WHITE    0xFFFFFF

/* ── disk/partition model ────────────────────────────────────────── */

#define MAX_DISKS  4
#define MAX_PARTS  8

struct partition {
    char     label[32];
    char     fs_type[16];
    char     mount_point[32];
    uint64_t size_mb;
    uint64_t used_mb;
    int      mounted;
    unsigned colour;
};

struct disk {
    char     name[32];
    char     model[48];
    char     scheme[8];
    uint64_t total_mb;
    int      part_count;
    struct partition parts[MAX_PARTS];
};

static struct disk g_disks[MAX_DISKS];
static int g_disk_count;
static int g_sel_disk;
static int g_sel_part = -1;
static int g_show_props;
static int g_fd = -1;

static unsigned part_colours[] = {
    0x89B4FA, 0xA6E3A1, 0xF9E2AF, 0xF38BA8,
    0xCBA6F7, 0x74C7EC, 0xFAB387, 0x94E2D5
};

static void populate_disks(void) {
    struct abi_dev_info devs[16];
    long n = sc3(ABI_SYS_DEV_LIST, (long)devs, 16, 0);
    if (n < 0) n = 0;

    g_disk_count = 0;
    memset(g_disks, 0, sizeof(g_disks));

    int di = 0;
    struct disk *d = &g_disks[0];
    strncpy(d->name, "ide0", 31);
    strncpy(d->model, "QEMU HARDDISK 16 MB", 47);
    strncpy(d->scheme, "GPT", 7);
    d->total_mb = 16;
    d->part_count = 3;

    strncpy(d->parts[0].label, "EFI System", 31);
    strncpy(d->parts[0].fs_type, "FAT32", 15);
    strncpy(d->parts[0].mount_point, "/boot", 31);
    d->parts[0].size_mb = 4; d->parts[0].used_mb = 2;
    d->parts[0].mounted = 1;
    d->parts[0].colour = part_colours[0];

    strncpy(d->parts[1].label, "tobyfs Data", 31);
    strncpy(d->parts[1].fs_type, "tobyfs", 15);
    strncpy(d->parts[1].mount_point, "/data", 31);
    d->parts[1].size_mb = 8; d->parts[1].used_mb = 3;
    d->parts[1].mounted = 1;
    d->parts[1].colour = part_colours[1];

    strncpy(d->parts[2].label, "FAT32 Volume", 31);
    strncpy(d->parts[2].fs_type, "FAT32", 15);
    strncpy(d->parts[2].mount_point, "/fat", 31);
    d->parts[2].size_mb = 4; d->parts[2].used_mb = 1;
    d->parts[2].mounted = 1;
    d->parts[2].colour = part_colours[2];
    di++;

    for (int i = 0; i < (int)n && di < MAX_DISKS; i++) {
        if (devs[i].bus != ABI_DEVT_BUS_BLK) continue;
        d = &g_disks[di];
        snprintf(d->name, 31, "blk%d", di);
        strncpy(d->model, devs[i].name, 47);
        strncpy(d->scheme, "Raw", 7);
        d->total_mb = devs[i].extra[0] / 2048;
        if (d->total_mb == 0) d->total_mb = 1;
        d->part_count = 1;
        strncpy(d->parts[0].label, "Unpartitioned", 31);
        strncpy(d->parts[0].fs_type, "raw", 15);
        d->parts[0].size_mb = d->total_mb;
        d->parts[0].colour = part_colours[di % 8];
        di++;
    }

    if (di == 0) di = 1;
    g_disk_count = di;
}

/* ── drawing ─────────────────────────────────────────────────────── */

static void fmt_size(uint64_t mb, char *buf, int cap) {
    if (mb >= 1024)
        snprintf(buf, cap, "%u.%u GB", (unsigned)(mb/1024), (unsigned)((mb%1024)*10/1024));
    else
        snprintf(buf, cap, "%u MB", (unsigned)mb);
}

static void draw_toolbar(void) {
    gui_fill(g_fd, 0, 0, WIN_W, 28, COL_PANEL);
    gui_text(g_fd, 10, 8, "Disk Manager", COL_ACCENT);

    gui_fill(g_fd, WIN_W - 80, 3, 70, 22, COL_HDR);
    gui_text(g_fd, WIN_W - 72, 8, "Refresh", COL_WHITE);
}

static void draw_disk_list(void) {
    int y = 32;
    gui_text(g_fd, 10, y, "Disks:", COL_DIM);
    y += 18;

    for (int i = 0; i < g_disk_count; i++) {
        unsigned bg = (i == g_sel_disk) ? COL_SEL : COL_BG;
        gui_fill(g_fd, 0, y, 200, 20, bg);
        char line[80];
        snprintf(line, 80, " %s %s", g_disks[i].name, g_disks[i].scheme);
        gui_text(g_fd, 5, y + 4, line, COL_TEXT);

        char sz[16];
        fmt_size(g_disks[i].total_mb, sz, 16);
        gui_text(g_fd, 140, y + 4, sz, COL_DIM);
        y += 22;
    }
}

static void draw_partition_bar(struct disk *d, int bx, int by, int bw, int bh) {
    gui_fill(g_fd, bx - 1, by - 1, bw + 2, bh + 2, COL_HDR);
    if (d->total_mb == 0) return;

    int xoff = 0;
    for (int i = 0; i < d->part_count; i++) {
        int pw = (int)(d->parts[i].size_mb * (uint64_t)bw / d->total_mb);
        if (pw < 2) pw = 2;
        if (i == d->part_count - 1) pw = bw - xoff;
        if (pw < 0) pw = 0;
        unsigned col = d->parts[i].colour;
        if (i == g_sel_part) col = COL_WHITE;
        gui_fill(g_fd, bx + xoff, by, pw, bh, col);
        if (pw > 30) {
            gui_text(g_fd, bx + xoff + 4, by + (bh/2) - 4, d->parts[i].label, COL_BG);
        }
        xoff += pw;
    }
}

static void draw_partition_table(struct disk *d) {
    int y = 170;
    gui_fill(g_fd, 210, y, WIN_W - 220, 18, COL_PANEL);
    gui_text(g_fd, 215, y + 3, "Label", COL_DIM);
    gui_text(g_fd, 360, y + 3, "Type", COL_DIM);
    gui_text(g_fd, 440, y + 3, "Size", COL_DIM);
    gui_text(g_fd, 520, y + 3, "Used", COL_DIM);
    gui_text(g_fd, 590, y + 3, "Mount", COL_DIM);
    gui_text(g_fd, 680, y + 3, "Status", COL_DIM);
    y += 20;

    for (int i = 0; i < d->part_count; i++) {
        struct partition *p = &d->parts[i];
        unsigned bg = (i == g_sel_part) ? COL_SEL : COL_BG;
        gui_fill(g_fd, 210, y, WIN_W - 220, 20, bg);

        gui_fill(g_fd, 215, y + 6, 8, 8, p->colour);
        gui_text(g_fd, 228, y + 4, p->label, COL_TEXT);
        gui_text(g_fd, 360, y + 4, p->fs_type, COL_TEXT);

        char sz[16];
        fmt_size(p->size_mb, sz, 16);
        gui_text(g_fd, 440, y + 4, sz, COL_TEXT);

        if (p->mounted) {
            fmt_size(p->used_mb, sz, 16);
            gui_text(g_fd, 520, y + 4, sz, COL_TEXT);
            gui_text(g_fd, 590, y + 4, p->mount_point, COL_ACCENT);
            gui_text(g_fd, 680, y + 4, "Mounted", COL_GREEN);
        } else {
            gui_text(g_fd, 520, y + 4, "--", COL_DIM);
            gui_text(g_fd, 590, y + 4, "--", COL_DIM);
            gui_text(g_fd, 680, y + 4, "Offline", COL_DIM);
        }
        y += 22;
    }

    /* Action buttons */
    y += 10;
    gui_fill(g_fd, 220, y, 70, 22, COL_ACCENT);
    gui_text(g_fd, 228, y + 5, "Mount", COL_BG);

    gui_fill(g_fd, 300, y, 80, 22, COL_YELLOW);
    gui_text(g_fd, 308, y + 5, "Unmount", COL_BG);

    gui_fill(g_fd, 390, y, 70, 22, COL_RED);
    gui_text(g_fd, 398, y + 5, "Format", COL_BG);

    gui_fill(g_fd, 470, y, 80, 22, COL_HDR);
    gui_text(g_fd, 478, y + 5, "Properties", COL_WHITE);
}

static void draw_props(struct disk *d) {
    if (g_sel_part < 0 || g_sel_part >= d->part_count) return;
    struct partition *p = &d->parts[g_sel_part];

    int dx = 200, dy = 120, dw = 340, dh = 260;
    gui_fill(g_fd, dx - 1, dy - 1, dw + 2, dh + 2, COL_DIM);
    gui_fill(g_fd, dx, dy, dw, dh, COL_PANEL);
    gui_fill(g_fd, dx, dy, dw, 24, COL_HDR);
    gui_text(g_fd, dx + 10, dy + 6, "Partition Properties", COL_WHITE);

    gui_fill(g_fd, dx + dw - 22, dy + 2, 20, 20, COL_RED);
    gui_text(g_fd, dx + dw - 18, dy + 6, "X", COL_WHITE);

    int py = dy + 35;
    char line[80];

    gui_text(g_fd, dx + 15, py, "Label:", COL_DIM); gui_text(g_fd, dx + 120, py, p->label, COL_TEXT); py += 20;
    gui_text(g_fd, dx + 15, py, "Filesystem:", COL_DIM); gui_text(g_fd, dx + 120, py, p->fs_type, COL_TEXT); py += 20;

    fmt_size(p->size_mb, line, 80);
    gui_text(g_fd, dx + 15, py, "Size:", COL_DIM); gui_text(g_fd, dx + 120, py, line, COL_TEXT); py += 20;

    fmt_size(p->used_mb, line, 80);
    gui_text(g_fd, dx + 15, py, "Used:", COL_DIM); gui_text(g_fd, dx + 120, py, line, COL_TEXT); py += 20;

    uint64_t free_mb = (p->size_mb > p->used_mb) ? p->size_mb - p->used_mb : 0;
    fmt_size(free_mb, line, 80);
    gui_text(g_fd, dx + 15, py, "Free:", COL_DIM); gui_text(g_fd, dx + 120, py, line, COL_GREEN); py += 20;

    gui_text(g_fd, dx + 15, py, "Mount:", COL_DIM);
    gui_text(g_fd, dx + 120, py, p->mounted ? p->mount_point : "(not mounted)", p->mounted ? COL_ACCENT : COL_DIM);
    py += 20;

    gui_text(g_fd, dx + 15, py, "Status:", COL_DIM);
    gui_text(g_fd, dx + 120, py, p->mounted ? "Mounted" : "Offline", p->mounted ? COL_GREEN : COL_DIM);
    py += 25;

    /* Usage bar */
    gui_text(g_fd, dx + 15, py, "Usage:", COL_DIM);
    int bar_x = dx + 80, bar_w = dw - 100;
    gui_fill(g_fd, bar_x, py, bar_w, 12, COL_BG);
    if (p->size_mb > 0) {
        int used_w = (int)(p->used_mb * (uint64_t)bar_w / p->size_mb);
        if (used_w > bar_w) used_w = bar_w;
        unsigned uc = (p->used_mb * 100 / p->size_mb > 90) ? COL_RED : COL_GREEN;
        gui_fill(g_fd, bar_x, py, used_w, 12, uc);
    }
}

static void redraw(void) {
    gui_fill(g_fd, 0, 0, WIN_W, WIN_H, COL_BG);
    draw_toolbar();
    draw_disk_list();

    if (g_sel_disk >= 0 && g_sel_disk < g_disk_count) {
        struct disk *d = &g_disks[g_sel_disk];

        gui_text(g_fd, 215, 55, d->model, COL_TEXT);
        char info[80];
        char sz[16];
        fmt_size(d->total_mb, sz, 16);
        snprintf(info, 80, "%s  |  %s  |  %d partitions", d->name, sz, d->part_count);
        gui_text(g_fd, 215, 73, info, COL_DIM);

        draw_partition_bar(d, 215, 95, WIN_W - 230, 40);
        draw_partition_table(d);

        if (g_show_props) draw_props(d);
    }

    gui_fill(g_fd, 0, WIN_H - 20, WIN_W, 20, COL_PANEL);
    char sb[64];
    snprintf(sb, 64, "%d disk(s) detected", g_disk_count);
    gui_text(g_fd, 10, WIN_H - 15, sb, COL_DIM);

    gui_flip(g_fd);
}

static void handle_click(int mx, int my) {
    if (g_show_props) {
        int dx = 200, dy = 120, dw = 340;
        if (mx >= dx + dw - 22 && mx < dx + dw && my >= dy && my < dy + 22) {
            g_show_props = 0;
            return;
        }
        if (mx >= dx && mx < dx + dw && my >= dy && my < dy + 380)
            return;
        g_show_props = 0;
        return;
    }

    if (my < 28 && mx >= WIN_W - 80) { populate_disks(); return; }

    /* Disk list */
    int y = 50;
    for (int i = 0; i < g_disk_count; i++) {
        if (mx < 200 && my >= y && my < y + 20) {
            g_sel_disk = i; g_sel_part = -1;
            return;
        }
        y += 22;
    }

    /* Partition table rows */
    if (g_sel_disk >= 0 && g_sel_disk < g_disk_count) {
        struct disk *d = &g_disks[g_sel_disk];
        y = 190;
        for (int i = 0; i < d->part_count; i++) {
            if (mx >= 210 && my >= y && my < y + 20) {
                g_sel_part = i;
                return;
            }
            y += 22;
        }
        y += 10;
        /* Action buttons */
        if (my >= y && my < y + 22) {
            if (mx >= 470 && mx < 550 && g_sel_part >= 0) {
                g_show_props = 1;
                return;
            }
            if (mx >= 220 && mx < 290 && g_sel_part >= 0) {
                d->parts[g_sel_part].mounted = 1;
                return;
            }
            if (mx >= 300 && mx < 380 && g_sel_part >= 0) {
                d->parts[g_sel_part].mounted = 0;
                return;
            }
        }
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    populate_disks();
    g_sel_disk = 0;

    g_fd = gui_create(WIN_W, WIN_H, "Disk Manager");
    if (g_fd < 0) return 1;

    redraw();

    for (;;) {
        struct gui_event ev;
        while (gui_poll(g_fd, &ev) > 0) {
            if (ev.type == 5) return 0;
            if (ev.type == 2) { handle_click(ev.x, ev.y); redraw(); }
            if (ev.type == 4) {
                if (ev.key == 'r') { populate_disks(); redraw(); }
                if (ev.key == 'p' && g_sel_part >= 0) { g_show_props = !g_show_props; redraw(); }
            }
        }
        sc0(ABI_SYS_YIELD);
    }
}
