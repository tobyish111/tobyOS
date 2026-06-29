/* user_gui_about/main.c -- /bin/gui_about: About / System Information panel.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The live system-metrics
 * gathering (SYS_SYSTEM_METRICS) is unchanged -- only the rendering moved to
 * TobyTK: a header + logo built from labels, and the info rows shown in a
 * TK_TABLE. The metrics are snapshotted at launch (an About box need not tick
 * live), and 'q'/Esc or the close button exit.
 */

#include <toby/tk.h>

typedef unsigned long long u64;
typedef unsigned int       u32;

#define SYS_CLOCK_MS        48
#define SYS_SYSTEM_METRICS  74

struct abi_system_metrics {
    u64 uptime_ms, total_pages, used_pages, free_pages;
    u64 cpu_user_ns, cpu_total_ns, total_syscalls, context_switches;
    u64 gui_frames, proc_spawns, proc_exits, vfs_ops, vfs_ns, gui_ns;
    u64 net_packets, net_ns;
    u32 process_count, ready_count, blocked_count, cpu_pct, ram_pct, gui_pct;
    u32 disk_pct, net_pct, syscalls_per_s, gui_fps, vfs_ops_per_s;
    u32 net_packets_per_s, net_up, net_ip_be, tsc_mhz, _reserved[9];
};

static inline long sys_clock_ms(void) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "0"((long)SYS_CLOCK_MS)
        : "rcx", "r11", "memory"); return r;
}
static inline long sys_system_metrics(struct abi_system_metrics *o) {
    long r; __asm__ volatile("syscall" : "=a"(r)
        : "0"((long)SYS_SYSTEM_METRICS), "D"(o) : "rcx", "r11", "memory"); return r;
}

/* ---- tiny no-libc string helpers ------------------------------------ */
static char *u64_to_dec(char *end, u64 v) {
    *--end = '\0';
    if (!v) { *--end = '0'; return end; }
    while (v) { *--end = (char)('0' + (v % 10)); v /= 10; }
    return end;
}
static char *scpy(char *d, const char *s) { while (*s) *d++ = *s++; *d = '\0'; return d; }
static char *sapp_u64(char *d, u64 v) { char t[24]; return scpy(d, u64_to_dec(t + sizeof(t), v)); }

/* ---- info rows (filled once from metrics) --------------------------- */
#define NROWS 10
static char g_rows[NROWS][2][72];
static const char *const tbl_headers[] = { "Property", "Value" };
static const int    tbl_widths[]  = { 150, 320 };

static const char *row_cell(void *u, int r, int c) {
    (void)u;
    if (r < 0 || r >= NROWS || c < 0 || c > 1) return "";
    return g_rows[r][c];
}

static void set_row(int i, const char *k, const char *v) {
    scpy(g_rows[i][0], k);
    scpy(g_rows[i][1], v);
}

static void fill_rows(const struct abi_system_metrics *m) {
    char b[72], *p;
    set_row(0, "Architecture", "x86-64 (amd64)");
    set_row(1, "Kernel", "TobyOS monolithic + ring-3 userland");
    p = sapp_u64(b, m->tsc_mhz); scpy(p, " MHz"); set_row(2, "CPU", b);
    {
        u64 used = (m->used_pages * 4) / 1024, total = (m->total_pages * 4) / 1024;
        p = sapp_u64(b, used); p = scpy(p, " / "); p = sapp_u64(p, total); scpy(p, " MiB");
    }
    set_row(3, "Memory", b);
    set_row(4, "Display", "1024x768 32-bit XRGB");
    if (m->net_up) {
        p = scpy(b, "Online  ");
        for (int i = 0; i < 4; i++) {
            p = sapp_u64(p, (m->net_ip_be >> (i * 8)) & 0xFF);
            if (i < 3) p = scpy(p, ".");
        }
    } else scpy(b, "Offline");
    set_row(5, "Network", b);
    {
        u64 sec = m->uptime_ms / 1000;
        p = sapp_u64(b, sec / 3600); p = scpy(p, "h ");
        p = sapp_u64(p, (sec / 60) % 60); p = scpy(p, "m ");
        p = sapp_u64(p, sec % 60); scpy(p, "s");
    }
    set_row(6, "Uptime", b);
    p = sapp_u64(b, m->process_count); scpy(p, " active"); set_row(7, "Processes", b);
    sapp_u64(b, m->total_syscalls); set_row(8, "Syscalls", b);
    sapp_u64(b, m->gui_fps); set_row(9, "GUI FPS", b);
}

static struct tk_window win;

static void on_key(struct tk_window *w, struct tk_event *ev) {
    if (ev->key == 'q' || ev->key == 'Q' || ev->key == 27) tk_quit(w);
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct abi_system_metrics met;
    sys_system_metrics(&met);
    (void)sys_clock_ms();
    fill_rows(&met);

    if (tk_window_open(&win, 540, 460, "About TobyOS") != 0)
        return 1;
    tk_on_key(&win, on_key);

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 16);
    root->gap = 6;

    /* header */
    struct tk_widget *hdr = tk_hbox(&win, root, 8);
    tk_bold(tk_font(tk_grow(tk_label(&win, hdr, "About TobyOS"), 1), 18));
    {
        struct tk_widget *st = tk_label(&win, hdr, met.net_up ? "Online" : "Offline");
        tk_colors(st, 0, met.net_up ? 0x0066BB6A : 0x00EF5350);
    }
    tk_separator(&win, root);

    /* logo */
    tk_align(tk_font(tk_colors(tk_label(&win, root, "TOBYOS"), 0, 0x004FC3F7), 26), TK_ALIGN_CENTER);
    tk_align(tk_colors(tk_label(&win, root, "Custom x86-64 Operating System"), 0, 0x00808890), TK_ALIGN_CENTER);
    tk_align(tk_colors(tk_label(&win, root, "Version 1.0  --  Nixie Desktop"), 0, 0x00808890), TK_ALIGN_CENTER);
    tk_separator(&win, root);

    /* info table */
    {
        struct tk_widget *tbl = tk_table(&win, root, tbl_headers, tbl_widths, 2);
        tk_table_rows(&win, tbl, NROWS, row_cell, 0);
        tk_grow(tbl, 1);
    }

    tk_separator(&win, root);
    tk_align(tk_colors(tk_label(&win, root, "Built with care. Press Q or close to exit."),
                       0, 0x00808890), TK_ALIGN_CENTER);

    return tk_run(&win);
}
