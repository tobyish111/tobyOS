/* console.c -- text console on top of a linear 32-bpp framebuffer.
 *
 * Cell size is 8x8 pixels (matches font8x8). We track an integer cursor
 * (col, row) and scroll the entire framebuffer up by one row when output
 * runs past the bottom. No double-buffering yet; writes hit VRAM directly.
 *
 * Cursor: rendered as an XOR block over the cell at (cur_col, cur_row).
 * XOR is its own inverse, so toggling visibility is a single pass over
 * 64 pixels with no separate save/restore buffer. Blinking is driven by
 * console_tick(), which the kernel calls from the idle loop.
 *
 * UEFI/GOP on Intel often reports the framebuffer at raw physical
 * 0x400000. Every access path must convert that to HHDM+phys once the
 * PMM HHDM offset is known -- not only put_pixel().
 */

#include <tobyos/console.h>
#include <tobyos/klibc.h>
#include <tobyos/gui.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>

extern const uint8_t font8x8_basic[128][8];

#define CELL_W 8
#define CELL_H 8

#define BLINK_TICKS 50u
#define CURSOR_XOR_MASK 0x00FFFFFFu

/* VT100/ANSI escape parser state (see ansi_putc below). The framebuffer
 * itself is our cell store at pixel granularity, so cell-shifting ops
 * (scroll region, IL/DL/ICH/DCH) are implemented as pixel-block memmoves --
 * no separate logical character grid is needed. */
enum esc_state {
    ES_NORMAL = 0,
    ES_ESC,        /* saw 0x1B */
    ES_CSI,        /* saw ESC [   -- collecting params + final */
    ES_OSC,        /* saw ESC ]   -- swallow title until BEL or ST */
    ES_CHARSET,    /* saw ESC ( or ESC ) -- swallow one designator byte */
};

#define CSI_MAX_PARAMS 16

static struct {
    uint32_t *fb;                 /* canonical HHDM virt once PMM is up */
    uint32_t  pitch_px;
    uint32_t  width_px;
    uint32_t  height_px;
    uint32_t  cols;
    uint32_t  rows;
    uint32_t  cur_col;
    uint32_t  cur_row;
    uint32_t  fg;                 /* effective draw fg (after SGR + reverse) */
    uint32_t  bg;                 /* effective draw bg */
    bool      ready;
    bool      fb_mapped;          /* cached: FB writable under live page tables */
    bool      cursor_visible;
    bool      cursor_drawn;
    uint64_t  last_blink_tick;

    /* ---- VT100/ANSI terminal-emulator state ---- */
    enum esc_state esc;
    uint32_t  par[CSI_MAX_PARAMS]; /* parsed CSI numeric params */
    uint32_t  npar;
    bool      par_seen;            /* at least one digit seen for current param */
    bool      csi_priv;            /* CSI had a leading '?' (DEC private mode) */

    uint32_t  def_fg;              /* default fg/bg restored by SGR 0/39/49 */
    uint32_t  def_bg;
    uint32_t  sgr_fg;              /* SGR-selected fg/bg before reverse */
    uint32_t  sgr_bg;
    bool      attr_bold;
    bool      attr_reverse;

    uint32_t  saved_col, saved_row; /* DECSC/DECRC + CSI s/u */
    uint32_t  scroll_top, scroll_bot; /* DECSTBM region, 0-based inclusive */
    bool      autowrap;            /* DECAWM (?7) -- default on */
    bool      pending_wrap;        /* deferred wrap at right margin */

    /* Diagnostics: proof harnesses assert a full-screen TUI really drove the
     * emulator (issued CSI cursor ops + entered the alternate screen). */
    uint64_t  vt_csi_count;
    uint64_t  vt_altscreen_count;
} g;

/* 16-colour ANSI palette (0..7 normal, 8..15 bright), 0x00RRGGBB. */
static const uint32_t ansi_palette[16] = {
    0x00000000, 0x00CC0000, 0x0000CC00, 0x00CCCC00,
    0x000000CC, 0x00CC00CC, 0x0000CCCC, 0x00CCCCCC,
    0x00555555, 0x00FF5555, 0x0055FF55, 0x00FFFF55,
    0x005555FF, 0x00FF55FF, 0x0055FFFF, 0x00FFFFFF,
};

bool console_ready(void) { return g.ready; }

void console_notify_cr3_switch(void) {
    g.fb_mapped = false;
}

/* Resolve the stored pointer to a kernel-virtual address. Before pmm_init
 * g_hhdm is zero and Limine's own map makes the raw pointer work; after
 * vmm_init we require HHDM+phys and an active PTE. */
static uint32_t *console_fb_ptr(void) {
    if (!g.fb) return 0;

    uint64_t addr = (uint64_t)(uintptr_t)g.fb;
    uint64_t hhdm = pmm_hhdm_offset();
    if (hhdm && addr < hhdm)
        addr += hhdm;
    return (uint32_t *)(uintptr_t)addr;
}

/* Re-evaluate whether the FB is reachable under the current page
 * tables. Called once at console_init and again after vmm_post_cr3_hook
 * remaps the FB. The hot path (put_pixel) reads g.fb_mapped only -- we
 * must never walk page tables per pixel. */
static void console_refresh_mapping(void) {
    uint32_t *fb = console_fb_ptr();
    if (!fb) { g.fb_mapped = false; return; }

    /* Before vmm_init builds a PML4, we're still on Limine's tables.
     * Trust those and write the FB directly. vmm_translate is unsafe
     * here -- g_pml4 is NULL until vmm_init. */
    if (vmm_kernel_pml4_phys() == 0) { g.fb_mapped = true; return; }

    g.fb_mapped = vmm_translate((uint64_t)(uintptr_t)fb) != 0;
}

static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!g.fb_mapped) return;
    uint32_t *fb = console_fb_ptr();
    fb[y * g.pitch_px + x] = color;
}

static void fill_cell(uint32_t col, uint32_t row, uint32_t color) {
    uint32_t x0 = col * CELL_W;
    uint32_t y0 = row * CELL_H;
    for (uint32_t dy = 0; dy < CELL_H; dy++) {
        for (uint32_t dx = 0; dx < CELL_W; dx++) {
            put_pixel(x0 + dx, y0 + dy, color);
        }
    }
}

static void draw_glyph(uint32_t col, uint32_t row, char c) {
    uint8_t ch = (uint8_t)c;
    if (ch >= 128) ch = '?';
    const uint8_t *rows = font8x8_basic[ch];
    uint32_t x0 = col * CELL_W;
    uint32_t y0 = row * CELL_H;
    for (uint32_t dy = 0; dy < CELL_H; dy++) {
        uint8_t bits = rows[dy];
        for (uint32_t dx = 0; dx < CELL_W; dx++) {
            uint32_t color = (bits >> dx) & 1u ? g.fg : g.bg;
            put_pixel(x0 + dx, y0 + dy, color);
        }
    }
}

static void cursor_xor_toggle(void) {
    if (!g.fb_mapped) return;
    if (g.cur_col >= g.cols || g.cur_row >= g.rows) return;

    uint32_t *fb = console_fb_ptr();
    uint32_t x0 = g.cur_col * CELL_W;
    uint32_t y0 = g.cur_row * CELL_H;
    for (uint32_t dy = 0; dy < CELL_H; dy++) {
        for (uint32_t dx = 0; dx < CELL_W; dx++) {
            fb[(y0 + dy) * g.pitch_px + (x0 + dx)] ^= CURSOR_XOR_MASK;
        }
    }
    g.cursor_drawn = !g.cursor_drawn;
}

static void cursor_undraw(void) {
    if (g.cursor_drawn) cursor_xor_toggle();
}

/* Fill a span of `count` cells on `row` starting at `col` with the current
 * background colour. Clipped to the grid. */
static void erase_cells(uint32_t col, uint32_t row, uint32_t count) {
    while (count-- > 0 && col < g.cols) fill_cell(col++, row, g.bg);
}

/* Clear whole cell-rows [from_row .. to_row] inclusive with the current bg. */
static void clear_rows(uint32_t from_row, uint32_t to_row) {
    for (uint32_t r = from_row; r <= to_row && r < g.rows; r++)
        erase_cells(0, r, g.cols);
}

/* Scroll the cell-rows in [top..bot] up by `n` rows (content moves toward the
 * top; `n` fresh bg rows appear at the bottom). Pixel-block memmove -- the FB
 * is our cell store. */
static void scroll_region_up(uint32_t top, uint32_t bot, uint32_t n) {
    if (!g.fb_mapped || n == 0 || top > bot || bot >= g.rows) return;
    uint32_t span = bot - top + 1;
    if (n >= span) { clear_rows(top, bot); return; }

    uint32_t *fb = console_fb_ptr();
    uint32_t pitch_bytes = g.pitch_px * 4u;
    uint8_t *base = (uint8_t *)fb;
    uint32_t dst_y = top * CELL_H;
    uint32_t src_y = (top + n) * CELL_H;
    uint32_t move_rows_px = (span - n) * CELL_H;
    memmove(base + (size_t)pitch_bytes * dst_y,
            base + (size_t)pitch_bytes * src_y,
            (size_t)pitch_bytes * move_rows_px);
    clear_rows(bot - n + 1, bot);
}

/* Scroll [top..bot] down by `n` rows (content toward the bottom; fresh rows at
 * the top). Used by reverse-index / DL-at-top. */
static void scroll_region_down(uint32_t top, uint32_t bot, uint32_t n) {
    if (!g.fb_mapped || n == 0 || top > bot || bot >= g.rows) return;
    uint32_t span = bot - top + 1;
    if (n >= span) { clear_rows(top, bot); return; }

    uint32_t *fb = console_fb_ptr();
    uint32_t pitch_bytes = g.pitch_px * 4u;
    uint8_t *base = (uint8_t *)fb;
    uint32_t src_y = top * CELL_H;
    uint32_t dst_y = (top + n) * CELL_H;
    uint32_t move_rows_px = (span - n) * CELL_H;
    memmove(base + (size_t)pitch_bytes * dst_y,
            base + (size_t)pitch_bytes * src_y,
            (size_t)pitch_bytes * move_rows_px);
    clear_rows(top, top + n - 1);
}

/* Line feed honouring the scroll region: scroll the region when at its
 * bottom margin, otherwise advance the cursor row. Column is unchanged
 * (CR is separate, per VT100). */
static void line_feed(void) {
    if (g.cur_row == g.scroll_bot) {
        scroll_region_up(g.scroll_top, g.scroll_bot, 1);
    } else if (g.cur_row + 1 < g.rows) {
        g.cur_row++;
    }
}

static void newline(void) {
    g.cur_col = 0;
    line_feed();
}

bool console_init(void *fb, uint64_t pitch, uint64_t width, uint64_t height) {
    if (!fb || pitch < width * 4 || width < CELL_W || height < CELL_H) {
        return false;
    }

    uint64_t addr = (uint64_t)(uintptr_t)fb;
    uint64_t hhdm = pmm_hhdm_offset();
    if (hhdm && addr < hhdm)
        addr += hhdm;

    g.fb              = (uint32_t *)(uintptr_t)addr;
    g.pitch_px        = (uint32_t)(pitch / 4);
    g.width_px        = (uint32_t)width;
    g.height_px       = (uint32_t)height;
    g.cols            = g.width_px  / CELL_W;
    g.rows            = g.height_px / CELL_H;
    g.cur_col         = 0;
    g.cur_row         = 0;
    g.def_fg          = 0x00CCCCCC;
    g.def_bg          = 0x00000000;
    g.sgr_fg          = g.def_fg;
    g.sgr_bg          = g.def_bg;
    g.attr_bold       = false;
    g.attr_reverse    = false;
    g.fg              = g.def_fg;
    g.bg              = g.def_bg;
    g.cursor_visible  = true;
    g.cursor_drawn    = false;
    g.last_blink_tick = 0;
    g.esc             = ES_NORMAL;
    g.npar            = 0;
    g.par_seen        = false;
    g.csi_priv        = false;
    g.saved_col       = 0;
    g.saved_row       = 0;
    g.scroll_top      = 0;
    g.scroll_bot      = g.rows - 1;
    g.autowrap        = true;
    g.pending_wrap    = false;
    g.ready           = true;
    console_refresh_mapping();
    console_clear();
    return true;
}

void console_clear(void) {
    if (!g.ready) return;
    g.cursor_drawn = false;
    for (uint32_t y = 0; y < g.height_px; y++) {
        for (uint32_t x = 0; x < g.width_px; x++) {
            put_pixel(x, y, g.bg);
        }
    }
    g.cur_col = 0;
    g.cur_row = 0;
    g.pending_wrap = false;
}

void console_set_color(uint32_t fg) { g.sgr_fg = fg; g.fg = fg; }

void console_get_cursor(uint32_t *col, uint32_t *row) {
    if (col) *col = g.cur_col;
    if (row) *row = g.cur_row;
}

void console_set_cursor(uint32_t col, uint32_t row) {
    if (!g.ready) return;
    cursor_undraw();
    if (col >= g.cols) col = g.cols - 1;
    if (row >= g.rows) row = g.rows - 1;
    g.cur_col = col;
    g.cur_row = row;
}

void console_get_size(uint32_t *cols, uint32_t *rows) {
    if (cols) *cols = g.cols;
    if (rows) *rows = g.rows;
}

void console_vt_stats(uint64_t *csi_ops, uint64_t *altscreen_enters) {
    if (csi_ops)          *csi_ops          = g.vt_csi_count;
    if (altscreen_enters) *altscreen_enters = g.vt_altscreen_count;
}

void console_show_cursor(bool visible) {
    if (!g.ready) return;
    g.cursor_visible = visible;
    if (!visible) cursor_undraw();
}

void console_backspace(void) {
    if (!g.ready) return;
    cursor_undraw();
    if (g.cur_col == 0) {
        if (g.cur_row == 0) return;
        g.cur_row--;
        g.cur_col = g.cols - 1;
    } else {
        g.cur_col--;
    }
    fill_cell(g.cur_col, g.cur_row, g.bg);
}

void console_tick(uint64_t ticks, uint32_t hz) {
    (void)hz;
    if (!g.ready || !g.cursor_visible) return;
    if (ticks - g.last_blink_tick >= BLINK_TICKS) {
        g.last_blink_tick = ticks;
        cursor_xor_toggle();
    }
}

/* ---- VT100/ANSI terminal emulator ------------------------------------ *
 *
 * console_putc() is the single sink for every byte that reaches the
 * framebuffer (kprintf and Linux-app writes both funnel through kputc ->
 * console_putc), so making it a proper VT100 state machine turns the kernel
 * text console into a real terminal: full-screen ncurses programs (nano,
 * vim, htop, ...) render correctly. We keep the byte stream on the serial
 * log untouched (kputc writes serial *before* calling us). */

static void move_abs(uint32_t col, uint32_t row) {
    if (col >= g.cols) col = g.cols ? g.cols - 1 : 0;
    if (row >= g.rows) row = g.rows ? g.rows - 1 : 0;
    g.cur_col = col;
    g.cur_row = row;
    g.pending_wrap = false;
}

static void move_rel(int dx, int dy) {
    int col = (int)g.cur_col + dx;
    int row = (int)g.cur_row + dy;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    move_abs((uint32_t)col, (uint32_t)row);
}

static void copy_cell(uint32_t dcol, uint32_t drow, uint32_t scol, uint32_t srow) {
    if (!g.fb_mapped) return;
    uint32_t *fb = console_fb_ptr();
    for (uint32_t yy = 0; yy < CELL_H; yy++) {
        size_t dbase = (size_t)(drow * CELL_H + yy) * g.pitch_px + dcol * CELL_W;
        size_t sbase = (size_t)(srow * CELL_H + yy) * g.pitch_px + scol * CELL_W;
        for (uint32_t xx = 0; xx < CELL_W; xx++) fb[dbase + xx] = fb[sbase + xx];
    }
}

static void erase_display(uint32_t mode) {
    switch (mode) {
    case 0: /* cursor -> end of screen */
        erase_cells(g.cur_col, g.cur_row, g.cols - g.cur_col);
        if (g.cur_row + 1 < g.rows) clear_rows(g.cur_row + 1, g.rows - 1);
        break;
    case 1: /* start of screen -> cursor */
        if (g.cur_row > 0) clear_rows(0, g.cur_row - 1);
        erase_cells(0, g.cur_row, g.cur_col + 1);
        break;
    default: /* 2 / 3: whole screen */
        clear_rows(0, g.rows - 1);
        break;
    }
}

static void erase_line(uint32_t mode) {
    switch (mode) {
    case 0:  erase_cells(g.cur_col, g.cur_row, g.cols - g.cur_col); break;
    case 1:  erase_cells(0, g.cur_row, g.cur_col + 1);              break;
    default: erase_cells(0, g.cur_row, g.cols);                     break;
    }
}

static void insert_chars(uint32_t n) {
    if (n == 0) n = 1;
    if (n > g.cols - g.cur_col) n = g.cols - g.cur_col;
    for (int col = (int)g.cols - 1; col >= (int)(g.cur_col + n); col--)
        copy_cell((uint32_t)col, g.cur_row, (uint32_t)col - n, g.cur_row);
    erase_cells(g.cur_col, g.cur_row, n);
}

static void delete_chars(uint32_t n) {
    if (n == 0) n = 1;
    if (n > g.cols - g.cur_col) n = g.cols - g.cur_col;
    for (uint32_t col = g.cur_col; col + n < g.cols; col++)
        copy_cell(col, g.cur_row, col + n, g.cur_row);
    erase_cells(g.cols - n, g.cur_row, n);
}

static void insert_lines(uint32_t n) {
    if (n == 0) n = 1;
    if (g.cur_row < g.scroll_top || g.cur_row > g.scroll_bot) return;
    scroll_region_down(g.cur_row, g.scroll_bot, n);
}

static void delete_lines(uint32_t n) {
    if (n == 0) n = 1;
    if (g.cur_row < g.scroll_top || g.cur_row > g.scroll_bot) return;
    scroll_region_up(g.cur_row, g.scroll_bot, n);
}

/* xterm 256-colour index -> 0x00RRGGBB. */
static uint32_t xterm256_rgb(uint32_t idx) {
    if (idx < 16) return ansi_palette[idx];
    if (idx < 232) {
        idx -= 16;
        uint32_t r = (idx / 36) % 6, gch = (idx / 6) % 6, b = idx % 6;
        static const uint8_t lvl[6] = {0, 95, 135, 175, 215, 255};
        return ((uint32_t)lvl[r] << 16) | ((uint32_t)lvl[gch] << 8) | lvl[b];
    }
    uint32_t v = 8 + (idx - 232) * 10;
    return (v << 16) | (v << 8) | v;
}

static void recompute_attr(void) {
    uint32_t fg = g.sgr_fg, bg = g.sgr_bg;
    if (g.attr_reverse) { g.fg = bg; g.bg = fg; }
    else                { g.fg = fg; g.bg = bg; }
}

static void apply_sgr(uint32_t count) {
    if (count == 0) {  /* bare ESC[m == reset */
        g.sgr_fg = g.def_fg; g.sgr_bg = g.def_bg;
        g.attr_bold = false; g.attr_reverse = false;
        recompute_attr();
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t p = g.par[i];
        if (p == 0) { g.sgr_fg = g.def_fg; g.sgr_bg = g.def_bg;
                      g.attr_bold = false; g.attr_reverse = false; }
        else if (p == 1)  g.attr_bold = true;
        else if (p == 2 || p == 22) g.attr_bold = false;
        else if (p == 7)  g.attr_reverse = true;
        else if (p == 27) g.attr_reverse = false;
        else if (p >= 30 && p <= 37)   g.sgr_fg = ansi_palette[p - 30];
        else if (p == 39)              g.sgr_fg = g.def_fg;
        else if (p >= 40 && p <= 47)   g.sgr_bg = ansi_palette[p - 40];
        else if (p == 49)              g.sgr_bg = g.def_bg;
        else if (p >= 90 && p <= 97)   g.sgr_fg = ansi_palette[8 + (p - 90)];
        else if (p >= 100 && p <= 107) g.sgr_bg = ansi_palette[8 + (p - 100)];
        else if (p == 38 || p == 48) {
            uint32_t *dst = (p == 38) ? &g.sgr_fg : &g.sgr_bg;
            if (i + 1 < count && g.par[i + 1] == 5 && i + 2 < count) {
                *dst = xterm256_rgb(g.par[i + 2]); i += 2;
            } else if (i + 1 < count && g.par[i + 1] == 2 && i + 4 < count) {
                *dst = ((g.par[i + 2] & 0xFF) << 16) |
                       ((g.par[i + 3] & 0xFF) << 8) | (g.par[i + 4] & 0xFF);
                i += 4;
            }
        }
    }
    recompute_attr();
}

static void set_scroll_region(uint32_t top1, uint32_t bot1) {
    uint32_t top = top1 ? top1 - 1 : 0;
    uint32_t bot = bot1 ? bot1 - 1 : g.rows - 1;
    if (bot >= g.rows) bot = g.rows - 1;
    if (top >= bot) { top = 0; bot = g.rows - 1; }
    g.scroll_top = top;
    g.scroll_bot = bot;
    move_abs(0, 0);                 /* DECSTBM homes the cursor */
}

static void reset_terminal(void) {
    g.sgr_fg = g.def_fg; g.sgr_bg = g.def_bg;
    g.attr_bold = false; g.attr_reverse = false;
    recompute_attr();
    g.scroll_top = 0; g.scroll_bot = g.rows - 1;
    g.autowrap = true; g.pending_wrap = false;
    move_abs(0, 0);
    console_show_cursor(true);
}

static void set_dec_mode(bool set) {
    uint32_t count = (g.par_seen || g.npar > 0) ? g.npar + 1 : 0;
    for (uint32_t i = 0; i < count; i++) {
        switch (g.par[i]) {
        case 7:   g.autowrap = set; break;                 /* DECAWM */
        case 25:  console_show_cursor(set); break;         /* DECTCEM */
        case 47:
        case 1047:
        case 1049: /* alternate screen buffer (contents not preserved) */
            if (set) g.vt_altscreen_count++;
            if (set) { g.saved_col = g.cur_col; g.saved_row = g.cur_row; }
            g.scroll_top = 0; g.scroll_bot = g.rows - 1;
            clear_rows(0, g.rows - 1);
            if (set) move_abs(0, 0);
            else     move_abs(g.saved_col, g.saved_row);
            break;
        default: break;                                    /* mouse/paste/etc: ignore */
        }
    }
}

static void dispatch_csi(unsigned char f) {
    g.vt_csi_count++;
    uint32_t count = (g.par_seen || g.npar > 0) ? g.npar + 1 : 0;
    uint32_t p0 = count > 0 ? (g.par[0] ? g.par[0] : 1) : 1;
    uint32_t p1 = count > 1 ? (g.par[1] ? g.par[1] : 1) : 1;

    if (g.csi_priv) {           /* DEC private modes only via h/l */
        if (f == 'h') set_dec_mode(true);
        else if (f == 'l') set_dec_mode(false);
        return;
    }

    switch (f) {
    case 'H': case 'f': move_abs(p1 - 1, p0 - 1); break;          /* CUP */
    case 'A': move_rel(0, -(int)p0); break;                       /* CUU */
    case 'B': move_rel(0, +(int)p0); break;                       /* CUD */
    case 'C': move_rel(+(int)p0, 0); break;                       /* CUF */
    case 'D': move_rel(-(int)p0, 0); break;                       /* CUB */
    case 'E': g.cur_col = 0; move_rel(0, +(int)p0); break;        /* CNL */
    case 'F': g.cur_col = 0; move_rel(0, -(int)p0); break;        /* CPL */
    case 'G': case '`': move_abs(p0 - 1, g.cur_row); break;       /* CHA/HPA */
    case 'd': move_abs(g.cur_col, p0 - 1); break;                 /* VPA */
    case 'J': erase_display(count ? g.par[0] : 0); break;         /* ED */
    case 'K': erase_line(count ? g.par[0] : 0); break;            /* EL */
    case 'X': erase_cells(g.cur_col, g.cur_row, p0); break;       /* ECH */
    case '@': insert_chars(p0); break;                            /* ICH */
    case 'P': delete_chars(p0); break;                            /* DCH */
    case 'L': insert_lines(p0); break;                            /* IL */
    case 'M': delete_lines(p0); break;                            /* DL */
    case 'S': scroll_region_up(g.scroll_top, g.scroll_bot, p0); break;  /* SU */
    case 'T': scroll_region_down(g.scroll_top, g.scroll_bot, p0); break;/* SD */
    case 'm': apply_sgr(count); break;                            /* SGR */
    case 'r': set_scroll_region(count ? g.par[0] : 0,
                                count > 1 ? g.par[1] : 0); break; /* DECSTBM */
    case 's': g.saved_col = g.cur_col; g.saved_row = g.cur_row; break;
    case 'u': move_abs(g.saved_col, g.saved_row); break;
    case 'n': break;   /* DSR/CPR is answered in tty.c on the output scan */
    default: break;
    }
}

static void put_printable(unsigned char c) {
    if (g.pending_wrap) {
        g.cur_col = 0;
        line_feed();
        g.pending_wrap = false;
    }
    if (g.cur_col >= g.cols) g.cur_col = g.cols - 1;
    draw_glyph(g.cur_col, g.cur_row, (char)c);
    if (g.cur_col + 1 >= g.cols) {
        if (g.autowrap) g.pending_wrap = true;   /* deferred wrap (LCF) */
    } else {
        g.cur_col++;
    }
}

static void csi_begin(void) {
    g.esc = ES_CSI;
    g.npar = 0;
    g.par_seen = false;
    g.csi_priv = false;
    for (uint32_t i = 0; i < CSI_MAX_PARAMS; i++) g.par[i] = 0;
}

void console_putc(char c) {
    if (!g.ready) return;
    if (gui_active()) return;

    cursor_undraw();

    unsigned char ch = (unsigned char)c;

    switch (g.esc) {
    case ES_NORMAL:
        switch (ch) {
        case 0x1b: g.esc = ES_ESC; return;
        case '\n': case 0x0b: case 0x0c: line_feed(); g.pending_wrap = false; return;
        case '\r': g.cur_col = 0; g.pending_wrap = false; return;
        case '\b': if (g.cur_col > 0) g.cur_col--; g.pending_wrap = false; return;
        case '\t': {
            uint32_t next = (g.cur_col + 8) & ~7u;
            if (next >= g.cols) next = g.cols - 1;
            while (g.cur_col < next) { fill_cell(g.cur_col, g.cur_row, g.bg); g.cur_col++; }
            g.pending_wrap = false;
            return;
        }
        case 0x07: return;                       /* BEL */
        default:
            if (ch < 0x20) return;               /* swallow other C0 controls */
            put_printable(ch);
            return;
        }

    case ES_ESC:
        switch (ch) {
        case '[': csi_begin(); return;
        case ']': g.esc = ES_OSC; return;
        case '(': case ')': case '*': case '+': g.esc = ES_CHARSET; return;
        case '7': g.saved_col = g.cur_col; g.saved_row = g.cur_row; g.esc = ES_NORMAL; return;
        case '8': move_abs(g.saved_col, g.saved_row); g.esc = ES_NORMAL; return;
        case 'c': reset_terminal(); g.esc = ES_NORMAL; return;
        case 'D': line_feed(); g.esc = ES_NORMAL; return;          /* IND */
        case 'E': newline(); g.esc = ES_NORMAL; return;            /* NEL */
        case 'M': /* RI -- reverse index */
            if (g.cur_row == g.scroll_top) scroll_region_down(g.scroll_top, g.scroll_bot, 1);
            else if (g.cur_row > 0) g.cur_row--;
            g.esc = ES_NORMAL; return;
        default: g.esc = ES_NORMAL; return;       /* =, >, and unknowns: ignore */
        }

    case ES_CSI:
        if (ch >= '0' && ch <= '9') {
            g.par[g.npar] = g.par[g.npar] * 10 + (ch - '0');
            g.par_seen = true;
            return;
        }
        if (ch == ';') {
            if (g.npar + 1 < CSI_MAX_PARAMS) g.npar++;
            g.par_seen = false;
            return;
        }
        if (ch == '?') { g.csi_priv = true; return; }
        if (ch >= 0x20 && ch <= 0x2f) return;     /* intermediate bytes: ignore */
        /* final byte 0x40..0x7e */
        dispatch_csi(ch);
        g.esc = ES_NORMAL;
        return;

    case ES_OSC:
        /* swallow OSC string until BEL or ST (ESC \). */
        if (ch == 0x07) g.esc = ES_NORMAL;
        else if (ch == 0x1b) g.esc = ES_ESC;      /* may be the ST intro */
        return;

    case ES_CHARSET:
        g.esc = ES_NORMAL;                        /* swallow one designator */
        return;
    }
}

void console_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) console_putc(s[i]);
}

void console_puts(const char *s) {
    while (*s) console_putc(*s++);
}
