/* eventview/main.c -- System Event Viewer for tobyOS.
 *
 * Reads the kernel slog ring buffer via ABI_SYS_SLOG_READ, displays
 * log entries with timestamp, severity (colour-coded), subsystem,
 * and message.  Severity/subsystem filter buttons, search box, and
 * 1-second auto-refresh.
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
static long sys_clock_ms(void) { return sc0(ABI_SYS_CLOCK_MS); }

struct gui_event { int type, x, y; unsigned char button, key, _pad[2]; };

#define WIN_W   800
#define WIN_H   550

#define COL_BG       0x1E1E2E
#define COL_PANEL    0x313244
#define COL_TEXT     0xCDD6F4
#define COL_DIM      0x6C7086
#define COL_ACCENT   0x89B4FA
#define COL_GREEN    0xA6E3A1
#define COL_YELLOW   0xF9E2AF
#define COL_RED      0xF38BA8
#define COL_CRIT     0xFF6666
#define COL_SEL      0x45475A
#define COL_HDR      0x585B70
#define COL_WHITE    0xFFFFFF
#define COL_SEARCH   0x1a2a3a

/* ── log entry model ─────────────────────────────────────────────── */

#define MAX_ENTRIES 256
#define VISIBLE_ROWS 24

struct log_entry {
    uint64_t time_ms;
    uint32_t level;
    int32_t  pid;
    char     sub[ABI_SLOG_SUB_MAX];
    char     msg[ABI_SLOG_MSG_MAX];
};

static struct log_entry g_entries[MAX_ENTRIES];
static int g_entry_count;
static uint64_t g_last_seq;

/* Filters: -1 = all, 0..3 = specific level */
static int g_level_filter = -1;

#define NUM_SUBSYSTEMS 7
static const char *g_subsystems[] = {"All","net","fs","driver","sched","boot","audio"};
static int g_sub_filter;    /* 0 = all */

static char g_search[32];
static int  g_search_len;

static int g_scroll;
static int g_fd = -1;

static void fetch_logs(void) {
    struct abi_slog_record recs[64];
    long n = sc3(ABI_SYS_SLOG_READ, (long)recs, 64, (long)g_last_seq);
    if (n <= 0) return;

    for (long i = 0; i < n && g_entry_count < MAX_ENTRIES; i++) {
        struct log_entry *e = &g_entries[g_entry_count];
        e->time_ms = recs[i].time_ms;
        e->level   = recs[i].level;
        e->pid     = recs[i].pid;
        memcpy(e->sub, recs[i].sub, ABI_SLOG_SUB_MAX);
        memcpy(e->msg, recs[i].msg, ABI_SLOG_MSG_MAX);
        g_last_seq = recs[i].seq;
        g_entry_count++;
    }
}

static int entry_matches(struct log_entry *e) {
    if (g_level_filter >= 0 && (int)e->level != g_level_filter) return 0;
    if (g_sub_filter > 0) {
        const char *want = g_subsystems[g_sub_filter];
        int match = 0;
        for (int i = 0; want[i] && e->sub[i]; i++) {
            if (want[i] != e->sub[i]) break;
            if (!want[i+1]) { match = 1; break; }
        }
        if (!match) return 0;
    }
    if (g_search_len > 0) {
        char *found = strstr(e->msg, g_search);
        if (!found) found = strstr(e->sub, g_search);
        if (!found) return 0;
    }
    return 1;
}

static const char *level_name(uint32_t lv) {
    switch (lv) {
    case 0: return "ERROR";
    case 1: return "WARN ";
    case 2: return "INFO ";
    case 3: return "DEBUG";
    }
    return "?    ";
}

static unsigned level_colour(uint32_t lv) {
    switch (lv) {
    case 0: return COL_RED;
    case 1: return COL_YELLOW;
    case 2: return COL_GREEN;
    case 3: return COL_DIM;
    }
    return COL_TEXT;
}

/* ── drawing ─────────────────────────────────────────────────────── */

static void draw_toolbar(void) {
    gui_fill(g_fd, 0, 0, WIN_W, 28, COL_PANEL);
    gui_text(g_fd, 10, 8, "Event Viewer", COL_ACCENT);

    /* Severity filter buttons */
    int bx = 140;
    const char *btns[] = {"All","Error","Warn","Info","Debug"};
    int bvals[] = {-1, 0, 1, 2, 3};
    for (int i = 0; i < 5; i++) {
        unsigned bc = (g_level_filter == bvals[i]) ? COL_ACCENT : COL_HDR;
        int bw = (int)strlen(btns[i]) * 8 + 12;
        gui_fill(g_fd, bx, 3, bw, 22, bc);
        gui_text(g_fd, bx + 6, 8, btns[i], (g_level_filter == bvals[i]) ? COL_BG : COL_WHITE);
        bx += bw + 4;
    }
}

static void draw_subsystem_bar(void) {
    int y = 30;
    gui_fill(g_fd, 0, y, WIN_W, 22, COL_BG);
    int bx = 10;
    for (int i = 0; i < NUM_SUBSYSTEMS; i++) {
        unsigned bc = (g_sub_filter == i) ? COL_ACCENT : COL_HDR;
        int bw = (int)strlen(g_subsystems[i]) * 8 + 12;
        gui_fill(g_fd, bx, y, bw, 20, bc);
        gui_text(g_fd, bx + 6, y + 4, g_subsystems[i], (g_sub_filter == i) ? COL_BG : COL_WHITE);
        bx += bw + 4;
    }

    /* Search box */
    gui_fill(g_fd, WIN_W - 220, y, 210, 20, COL_SEARCH);
    if (g_search_len > 0)
        gui_text(g_fd, WIN_W - 215, y + 4, g_search, COL_WHITE);
    else
        gui_text(g_fd, WIN_W - 215, y + 4, "Search...", COL_DIM);
}

static void draw_log_header(void) {
    int y = 55;
    gui_fill(g_fd, 0, y, WIN_W, 18, COL_PANEL);
    gui_text(g_fd, 10, y + 3, "Time", COL_DIM);
    gui_text(g_fd, 95, y + 3, "Level", COL_DIM);
    gui_text(g_fd, 155, y + 3, "PID", COL_DIM);
    gui_text(g_fd, 195, y + 3, "Subsystem", COL_DIM);
    gui_text(g_fd, 290, y + 3, "Message", COL_DIM);
}

static void draw_log_entries(void) {
    int y = 75;
    int row = 0;
    int skipped = 0;

    for (int i = 0; i < g_entry_count && row < VISIBLE_ROWS; i++) {
        struct log_entry *e = &g_entries[i];
        if (!entry_matches(e)) continue;
        if (skipped < g_scroll) { skipped++; continue; }

        unsigned bg = (row & 1) ? COL_BG : 0x24243a;
        gui_fill(g_fd, 0, y, WIN_W, 18, bg);

        char ts[16];
        unsigned s = (unsigned)(e->time_ms / 1000);
        unsigned ms = (unsigned)(e->time_ms % 1000);
        snprintf(ts, 16, "%02u:%02u.%03u", s / 60, s % 60, ms);
        gui_text(g_fd, 10, y + 3, ts, COL_DIM);

        gui_text(g_fd, 95, y + 3, level_name(e->level), level_colour(e->level));

        char pid[8];
        if (e->pid < 0) snprintf(pid, 8, "kern");
        else snprintf(pid, 8, "%d", e->pid);
        gui_text(g_fd, 155, y + 3, pid, COL_DIM);

        gui_text(g_fd, 195, y + 3, e->sub, COL_ACCENT);

        /* Truncate message for display */
        char disp[64];
        strncpy(disp, e->msg, 63);
        disp[63] = '\0';
        gui_text(g_fd, 290, y + 3, disp, COL_TEXT);

        y += 19;
        row++;
    }
}

static void redraw(void) {
    gui_fill(g_fd, 0, 0, WIN_W, WIN_H, COL_BG);
    draw_toolbar();
    draw_subsystem_bar();
    draw_log_header();
    draw_log_entries();

    /* Status bar */
    gui_fill(g_fd, 0, WIN_H - 20, WIN_W, 20, COL_PANEL);
    char sb[64];
    snprintf(sb, 64, "%d entries total  |  scroll: %d", g_entry_count, g_scroll);
    gui_text(g_fd, 10, WIN_H - 15, sb, COL_DIM);

    gui_flip(g_fd);
}

static void handle_click(int mx, int my) {
    /* Severity filter buttons */
    if (my < 28) {
        int bx = 140;
        int bvals[] = {-1, 0, 1, 2, 3};
        const char *btns[] = {"All","Error","Warn","Info","Debug"};
        for (int i = 0; i < 5; i++) {
            int bw = (int)strlen(btns[i]) * 8 + 12;
            if (mx >= bx && mx < bx + bw) {
                g_level_filter = bvals[i];
                g_scroll = 0;
                return;
            }
            bx += bw + 4;
        }
        return;
    }

    /* Subsystem filter */
    if (my >= 30 && my < 52) {
        int bx = 10;
        for (int i = 0; i < NUM_SUBSYSTEMS; i++) {
            int bw = (int)strlen(g_subsystems[i]) * 8 + 12;
            if (mx >= bx && mx < bx + bw) {
                g_sub_filter = i;
                g_scroll = 0;
                return;
            }
            bx += bw + 4;
        }
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    memset(g_entries, 0, sizeof(g_entries));
    g_entry_count = 0;
    g_last_seq = 0;
    g_search_len = 0;
    g_search[0] = '\0';

    fetch_logs();

    g_fd = gui_create(WIN_W, WIN_H, "Event Viewer");
    if (g_fd < 0) return 1;

    long last_fetch = 0;
    redraw();

    for (;;) {
        struct gui_event ev;
        int dirty = 0;
        while (gui_poll(g_fd, &ev) > 0) {
            if (ev.type == 5) return 0;
            if (ev.type == 2) { handle_click(ev.x, ev.y); dirty = 1; }
            if (ev.type == 4) {
                unsigned char k = ev.key;
                if (k == 0x08 && g_search_len > 0) {
                    g_search[--g_search_len] = '\0';
                    g_scroll = 0;
                    dirty = 1;
                } else if (k >= 0x20 && k < 0x7f && g_search_len < 30) {
                    g_search[g_search_len++] = (char)k;
                    g_search[g_search_len] = '\0';
                    g_scroll = 0;
                    dirty = 1;
                }
                /* Arrow keys for scroll (page up/down via shift) */
                if (k == '-' && g_scroll > 0) { g_scroll--; dirty = 1; }
                if (k == '=' || k == '+') { g_scroll++; dirty = 1; }
            }
        }

        long now = sys_clock_ms();
        if (now - last_fetch > 1000) {
            int old_count = g_entry_count;
            fetch_logs();
            last_fetch = now;
            if (g_entry_count != old_count) dirty = 1;
        }

        if (dirty) redraw();
        sc0(ABI_SYS_YIELD);
    }
}
