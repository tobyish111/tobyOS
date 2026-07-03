/* diskmgr/main.c -- Disk Manager GUI for tobyOS.
 *
 * Real data now: enumerates the kernel block registry via
 * ABI_SYS_BLK_LIST (name/model/geometry/partitions/filesystem
 * signatures + the kernel guard's provisioning verdict) and can CREATE
 * a persistent tobyOS data volume on an eligible disk via
 * ABI_SYS_DATA_PROVISION.
 *
 * Safety model: the kernel re-runs its guard on every provision
 * request -- this UI can only ever offer what the kernel already
 * classified as BLANK (empty disk) or TOBYOS (our own data / an empty
 * partition table; needs FORCE). Foreign/Windows disks are refused by
 * the kernel no matter what this app sends. On top of that the UI
 * requires the user to TYPE THE DEVICE NAME on a confirmation page
 * that shows the disk model + exact byte size before the syscall is
 * issued.
 */

#include <stdio.h>
#include <string.h>
#include <tobyos/abi/abi.h>
#include <toby/tk.h>

static long sc3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}

#define COL_ACCENT 0x0089B4FAu
#define COL_GREEN  0x00A6E3A1u
#define COL_YELLOW 0x00F9E2AFu
#define COL_RED    0x00F38BA8u
#define COL_DIM    0x006C7086u
#define COL_BG     0x001E1E2Eu
#define COL_HDR    0x00585B70u

/* ── model ─────────────────────────────────────────────────────────────── */

static struct abi_blk_info g_blk[ABI_BLK_MAX_DEVICES];
static int g_nblk;
static int g_disks[ABI_BLK_MAX_DEVICES];   /* g_blk indices, class==disk */
static int g_ndisk, g_sel;                 /* selection into g_disks[]   */

static char g_disk_labels[ABI_BLK_MAX_DEVICES][48];
static const char *g_disk_items[ABI_BLK_MAX_DEVICES];

/* view: 0 = browser, 1 = confirm, 2 = result */
static int  g_view;
static char g_confirm[33];                  /* typed device name */
static long g_result;                       /* provision return code */

static unsigned part_colours[] = { 0x89B4FA,0xA6E3A1,0xF9E2AF,0xCBA6F7,0x74C7EC,0xFAB387,0x94E2D5,0xF38BA8 };

static void u64_str(unsigned long long v, char *out) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    int w = 0;
    while (n) out[w++] = t[--n];
    out[w] = 0;
}

static void fmt_size(unsigned long long sectors, char *buf, int cap) {
    unsigned long long mb = sectors / 2048ull;
    if (mb >= 10240ull)
        snprintf(buf, cap, "%u.%u GB", (unsigned)(mb / 1024),
                 (unsigned)((mb % 1024) * 10 / 1024));
    else
        snprintf(buf, cap, "%u MB", (unsigned)mb);
}

static void populate(void) {
    long n = sc3(ABI_SYS_BLK_LIST, (long)g_blk, ABI_BLK_MAX_DEVICES, 0);
    if (n < 0) n = 0;
    g_nblk = (int)n;
    g_ndisk = 0;
    for (int i = 0; i < g_nblk; i++) {
        if (g_blk[i].class != 1) continue;      /* disks only */
        g_disks[g_ndisk] = i;
        char sz[16];
        fmt_size(g_blk[i].sector_count, sz, sizeof(sz));
        snprintf(g_disk_labels[g_ndisk], sizeof(g_disk_labels[0]),
                 "%s  %s%s", g_blk[i].name, sz,
                 (g_blk[i].flags & ABI_BLK_F_RAM) ? "  (RAM)" : "");
        g_ndisk++;
    }
    if (g_sel >= g_ndisk) g_sel = g_ndisk ? g_ndisk - 1 : 0;
}

static int part_count_of(int bi) {
    int c = 0;
    for (int i = 0; i < g_nblk; i++)
        if (g_blk[i].parent == bi && g_blk[i].class == 2) c++;
    return c;
}
static int part_at(int bi, int idx) {
    int c = 0;
    for (int i = 0; i < g_nblk; i++)
        if (g_blk[i].parent == bi && g_blk[i].class == 2) {
            if (c == idx) return i;
            c++;
        }
    return -1;
}

static const char *verdict_text(const struct abi_blk_info *d) {
    if (d->flags & ABI_BLK_F_GONE) return "Removed";
    if (d->flags & ABI_BLK_F_RAM)  return "RAM disk (contents lost on reboot)";
    switch (d->verdict) {
    case ABI_BLKV_BLANK:   return "Blank disk -- eligible for a tobyOS data volume";
    case ABI_BLKV_TOBYOS:  return "tobyOS data / empty table -- re-creating ERASES it";
    case ABI_BLKV_FOREIGN: return "Foreign filesystem or partition table -- protected";
    case ABI_BLKV_MOUNTED: return "In use (backs a live mount)";
    default:               return "Not provisionable";
    }
}

static int disk_eligible(const struct abi_blk_info *d) {
    if (d->class != 1) return 0;
    if (d->flags & (ABI_BLK_F_RAM | ABI_BLK_F_GONE)) return 0;
    return d->verdict == ABI_BLKV_BLANK || d->verdict == ABI_BLKV_TOBYOS;
}

/* ── TobyTK UI ─────────────────────────────────────────────────────────── */

static struct tk_window win;
static int g_base;
static void rebuild(void);

static void bar_paint(struct tk_window *w, struct tk_widget *c) {
    int x, y, bw, bh; tk_geom(c, &x, &y, &bw, &bh);
    tk_draw_fill(w, x, y, bw, bh, COL_HDR);
    if (g_sel < 0 || g_sel >= g_ndisk) return;
    int bi = g_disks[g_sel];
    unsigned long long total = g_blk[bi].sector_count;
    if (!total) return;
    int np = part_count_of(bi);
    for (int i = 0; i < np; i++) {
        int pi = part_at(bi, i);
        if (pi < 0) continue;
        int px = (int)(g_blk[pi].offset_lba * (unsigned long long)bw / total);
        int pw = (int)(g_blk[pi].sector_count * (unsigned long long)bw / total);
        if (pw < 2) pw = 2;
        if (px + pw > bw) pw = bw - px;
        tk_draw_fill(w, x + px, y, pw, bh, part_colours[i % 8]);
        const char *lbl = g_blk[pi].label[0] ? g_blk[pi].label
                                             : g_blk[pi].name;
        if (pw > 60) tk_draw_text(w, x + px + 4, y + bh / 2 - 7, lbl,
                                  COL_BG, 12, 0);
    }
}

static const char *part_cell(void *u, int r, int c) {
    (void)u; static char b[24];
    if (g_sel < 0 || g_sel >= g_ndisk) return "";
    int pi = part_at(g_disks[g_sel], r);
    if (pi < 0) return "";
    struct abi_blk_info *p = &g_blk[pi];
    switch (c) {
    case 0: return p->label[0] ? p->label : p->name;
    case 1: return p->fs[0] ? p->fs : "-";
    case 2: fmt_size(p->sector_count, b, sizeof(b)); return b;
    case 3: return (p->flags & ABI_BLK_F_DATA)    ? "/data"
               : (p->flags & ABI_BLK_F_MOUNTED)   ? "mounted" : "-";
    }
    return "";
}
static const char *const part_hdr[] = {"Partition","FS","Size","Status"};
static const int         part_w[]   = {200,90,90,110};

static void on_refresh(struct tk_window *w, struct tk_widget *b){ (void)w;(void)b; populate(); rebuild(); }
static void on_disk(struct tk_window *w, struct tk_widget *l){ (void)w; g_sel = tk_selected(l); rebuild(); }
static void on_back(struct tk_window *w, struct tk_widget *b){ (void)w;(void)b; g_view = 0; populate(); rebuild(); }
static void on_create(struct tk_window *w, struct tk_widget *b){
    (void)w;(void)b;
    if (g_sel < 0 || g_sel >= g_ndisk) return;
    if (!disk_eligible(&g_blk[g_disks[g_sel]])) return;
    g_confirm[0] = 0;
    g_view = 1;
    rebuild();
}

static void do_provision(void) {
    struct abi_blk_info *d = &g_blk[g_disks[g_sel]];
    if (strcmp(g_confirm, d->name) != 0) return;
    struct abi_provision_req req;
    memset(&req, 0, sizeof(req));
    strncpy(req.dev, d->name, sizeof(req.dev) - 1);
    if (d->verdict == ABI_BLKV_TOBYOS) req.flags |= ABI_PROV_F_FORCE;
    g_result = sc3(ABI_SYS_DATA_PROVISION, (long)&req, 0, 0);
    g_view = 2;
    populate();
    rebuild();
}
static void on_do_create(struct tk_window *w, struct tk_widget *b){ (void)w;(void)b; do_provision(); }

static void prop_row(struct tk_widget *p, const char *k, const char *v,
                     uint32_t vc) {
    struct tk_widget *row = tk_hbox(&win, p, 8);
    tk_size(tk_colors(tk_label(&win, row, k), 0, COL_DIM), 130, 0);
    tk_colors(tk_label(&win, row, v), 0, vc ? vc : win.theme.text);
}

static void build_browser(struct tk_widget *root) {
    struct tk_widget *main = tk_hbox(&win, root, 0); tk_grow(main, 1);

    struct tk_widget *side = tk_vbox(&win, main, 2);
    tk_pad(side, 6); tk_size(side, 210, 0); tk_colors(side, 0x00252536u, 0);
    tk_colors(tk_label(&win, side, "Disks"), 0, COL_DIM);
    for (int i = 0; i < g_ndisk; i++) g_disk_items[i] = g_disk_labels[i];
    struct tk_widget *lst = tk_listbox(&win, side, g_disk_items, g_ndisk);
    lst->on_change = on_disk; lst->sel = g_sel; tk_grow(lst, 1);

    struct tk_widget *right = tk_vbox(&win, main, 6);
    tk_pad(right, 8); tk_grow(right, 1);
    if (g_sel >= 0 && g_sel < g_ndisk) {
        struct abi_blk_info *d = &g_blk[g_disks[g_sel]];
        tk_bold(tk_label(&win, right,
                         d->model[0] ? d->model : d->name));
        {
            char info[96], sz[16];
            fmt_size(d->sector_count, sz, sizeof(sz));
            snprintf(info, sizeof(info), "%s  |  %s  |  %d partition(s)%s%s",
                     d->name, sz, part_count_of(g_disks[g_sel]),
                     (d->flags & ABI_BLK_F_GPT) ? "  |  GPT" : "",
                     (d->flags & ABI_BLK_F_TOBYFS) ? "  |  tobyfs" : "");
            tk_colors(tk_label(&win, right, info), 0, COL_DIM);
        }
        {
            unsigned vc = COL_DIM;
            if (d->verdict == ABI_BLKV_BLANK)  vc = COL_GREEN;
            if (d->verdict == ABI_BLKV_TOBYOS) vc = COL_YELLOW;
            if (d->verdict == ABI_BLKV_FOREIGN) vc = COL_RED;
            tk_colors(tk_label(&win, right, verdict_text(d)), 0, vc);
        }
        tk_size(tk_canvas(&win, right, bar_paint), 0, 40);
        struct tk_widget *tbl = tk_table(&win, right, part_hdr, part_w, 4);
        tk_table_rows(&win, tbl, part_count_of(g_disks[g_sel]), part_cell, 0);
        tk_grow(tbl, 1);

        struct tk_widget *acts = tk_hbox(&win, right, 8);
        if (disk_eligible(d)) {
            tk_colors(tk_button(&win, acts, "Create tobyOS data volume...",
                                on_create), COL_ACCENT, 0x00181825u);
        } else {
            tk_colors(tk_label(&win, acts,
                (d->flags & ABI_BLK_F_RAM)
                    ? "RAM fallback volume -- attach a blank disk/SSD/USB to persist"
                    : "This disk is protected -- no destructive actions offered"),
                0, COL_DIM);
        }
    } else {
        tk_colors(tk_label(&win, right, "No disks detected."), 0, COL_DIM);
    }
}

static void build_confirm(struct tk_widget *root) {
    struct abi_blk_info *d = &g_blk[g_disks[g_sel]];
    struct tk_widget *pp = tk_vbox(&win, root, 8);
    tk_pad(pp, 18); tk_grow(pp, 1);
    tk_bold(tk_font(tk_label(&win, pp, "Create tobyOS data volume"), 16));
    tk_separator(&win, pp);

    char sz[16], bytes[28], bytestr[48];
    fmt_size(d->sector_count, sz, sizeof(sz));
    u64_str((unsigned long long)d->sector_count * 512ull, bytes);
    snprintf(bytestr, sizeof(bytestr), "%s bytes (%s)", bytes, sz);
    prop_row(pp, "Device:",  d->name, COL_ACCENT);
    prop_row(pp, "Model:",   d->model[0] ? d->model : "(unknown)", 0);
    prop_row(pp, "Size:",    bytestr, 0);
    prop_row(pp, "Content:", verdict_text(d),
             d->verdict == ABI_BLKV_TOBYOS ? COL_YELLOW : COL_GREEN);

    tk_colors(tk_label(&win, pp,
        d->verdict == ABI_BLKV_TOBYOS
            ? "WARNING: ALL DATA ON THIS DISK WILL BE ERASED."
            : "The whole disk becomes a persistent /data volume (GPT + tobyfs)."),
        0, d->verdict == ABI_BLKV_TOBYOS ? COL_RED : COL_DIM);

    {
        char ask[80];
        snprintf(ask, sizeof(ask),
                 "Type the device name (%s) to confirm, then press Enter:",
                 d->name);
        tk_label(&win, pp, ask);
    }
    {
        char typed[44];
        snprintf(typed, sizeof(typed), "> %s_", g_confirm);
        tk_bold(tk_colors(tk_label(&win, pp, typed), 0,
                strcmp(g_confirm, d->name) == 0 ? COL_GREEN : COL_YELLOW));
    }

    struct tk_widget *acts = tk_hbox(&win, pp, 8);
    tk_button(&win, acts, "Cancel", on_back);
    if (strcmp(g_confirm, d->name) == 0)
        tk_colors(tk_button(&win, acts, "Create volume", on_do_create),
                  COL_RED, 0x00181825u);
    tk_grow(tk_label(&win, pp, ""), 1);
}

static const char *result_text(long rc) {
    if (rc == ABI_PROV_OK_MOUNTED)
        return "Success -- the new volume is live as /data now.";
    if (rc == ABI_PROV_OK_NEXT_BOOT)
        return "Success -- volume created; it becomes /data on the next boot.";
    switch ((int)-rc) {
    case ABI_EPERM:  return "Refused: foreign filesystem/partition table (protected).";
    case ABI_EBUSY:  return "Refused: the disk backs a live mount.";
    case ABI_EEXIST: return "Refused: tobyOS data already present (confirm again to erase).";
    case ABI_EINVAL: return "Refused: not a provisionable disk (RAM disk / too small).";
    case ABI_ENOENT: return "Device disappeared -- refresh and retry.";
    case ABI_EIO:    return "I/O error while writing GPT / formatting.";
    }
    return "Failed (unknown error).";
}

static void build_result(struct tk_widget *root) {
    struct tk_widget *pp = tk_vbox(&win, root, 10);
    tk_pad(pp, 18); tk_grow(pp, 1);
    tk_bold(tk_font(tk_label(&win, pp, g_result >= 0 ? "Volume created"
                                                     : "Provisioning failed"), 16));
    tk_separator(&win, pp);
    tk_colors(tk_label(&win, pp, result_text(g_result)), 0,
              g_result >= 0 ? COL_GREEN : COL_RED);
    if (g_result >= 0)
        tk_colors(tk_label(&win, pp,
            "Files saved under /data now survive reboots and power loss."),
            0, COL_DIM);
    struct tk_widget *acts = tk_hbox(&win, pp, 8);
    tk_button(&win, acts, "Back", on_back);
    tk_grow(tk_label(&win, pp, ""), 1);
}

static void rebuild(void) {
    tk_clear_children(&win, win.root);
    tk_rewind(&win, g_base);
    win.capture = 0; win.focus = 0;

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 0); root->gap = 0;

    struct tk_widget *tb = tk_hbox(&win, root, 8);
    tk_pad(tb, 6); tk_colors(tb, win.theme.panel_bg, 0);
    tk_grow(tk_bold(tk_colors(tk_label(&win, tb, "Disk Manager"), 0,
                              COL_ACCENT)), 1);
    if (g_view == 0) tk_button(&win, tb, "Refresh", on_refresh);
    else             tk_button(&win, tb, "Back", on_back);

    if      (g_view == 1) build_confirm(root);
    else if (g_view == 2) build_result(root);
    else                  build_browser(root);

    struct tk_widget *sb = tk_hbox(&win, root, 0);
    tk_pad(sb, 4); tk_colors(sb, win.theme.panel_bg, 0);
    {
        char s[48];
        snprintf(s, sizeof(s), "%d disk(s), %d block device(s)",
                 g_ndisk, g_nblk);
        tk_colors(tk_label(&win, sb, s), 0, COL_DIM);
    }
    tk_redraw(&win);
}

static void on_key(struct tk_window *w, struct tk_event *ev) {
    if (g_view == 1) {
        /* typed-confirmation editor */
        size_t len = strlen(g_confirm);
        if (ev->key == 27) { g_view = 0; rebuild(); return; }
        if (ev->key == TK_KEY_BACKSPACE) {
            if (len) g_confirm[len - 1] = 0;
            rebuild(); return;
        }
        if (ev->key == TK_KEY_ENTER || ev->key == '\r') {
            do_provision(); return;
        }
        if (ev->key >= 0x20 && ev->key <= 0x7E &&
            len + 1 < sizeof(g_confirm)) {
            g_confirm[len] = (char)ev->key;
            g_confirm[len + 1] = 0;
            rebuild();
        }
        return;
    }
    if (g_view == 2) {
        if (ev->key == 27 || ev->key == TK_KEY_ENTER) { on_back(w, 0); }
        return;
    }
    if (ev->key == 'q' || ev->key == 27) tk_quit(w);
    else if (ev->key == 'r') { populate(); rebuild(); }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    populate();
    g_sel = 0;
    if (tk_window_open(&win, 780, 540, "Disk Manager") != 0) return 1;
    tk_on_key(&win, on_key);
    g_base = tk_checkpoint(&win);
    rebuild();
    return tk_run(&win);
}
