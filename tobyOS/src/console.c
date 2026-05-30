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

static struct {
    uint32_t *fb;                 /* canonical HHDM virt once PMM is up */
    uint32_t  pitch_px;
    uint32_t  width_px;
    uint32_t  height_px;
    uint32_t  cols;
    uint32_t  rows;
    uint32_t  cur_col;
    uint32_t  cur_row;
    uint32_t  fg;
    uint32_t  bg;
    bool      ready;
    bool      fb_mapped;          /* cached: FB writable under live page tables */
    bool      cursor_visible;
    bool      cursor_drawn;
    uint64_t  last_blink_tick;
} g;

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

static void scroll_up_one_row(void) {
    if (!g.fb_mapped) return;

    uint32_t *fb = console_fb_ptr();
    uint32_t pitch_bytes = g.pitch_px * 4u;
    uint8_t *base = (uint8_t *)fb;

    size_t bytes_to_move = (size_t)pitch_bytes * (g.height_px - CELL_H);
    memmove(base, base + (size_t)pitch_bytes * CELL_H, bytes_to_move);

    for (uint32_t y = g.height_px - CELL_H; y < g.height_px; y++) {
        for (uint32_t x = 0; x < g.width_px; x++) {
            put_pixel(x, y, g.bg);
        }
    }
}

static void newline(void) {
    g.cur_col = 0;
    if (g.cur_row + 1 >= g.rows) {
        scroll_up_one_row();
    } else {
        g.cur_row++;
    }
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
    g.fg              = 0x00CCCCCC;
    g.bg              = 0x00000000;
    g.cursor_visible  = true;
    g.cursor_drawn    = false;
    g.last_blink_tick = 0;
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
}

void console_set_color(uint32_t fg) { g.fg = fg; }

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

void console_putc(char c) {
    if (!g.ready) return;
    if (gui_active()) return;

    cursor_undraw();

    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        g.cur_col = 0;
        return;
    case '\b':
        if (g.cur_col > 0) g.cur_col--;
        return;
    case '\t': {
        uint32_t next = (g.cur_col + 4) & ~3u;
        while (g.cur_col < next && g.cur_col < g.cols) {
            fill_cell(g.cur_col, g.cur_row, g.bg);
            g.cur_col++;
        }
        return;
    }
    default:
        break;
    }

    if (g.cur_col >= g.cols) newline();
    draw_glyph(g.cur_col, g.cur_row, c);
    g.cur_col++;
}

void console_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) console_putc(s[i]);
}

void console_puts(const char *s) {
    while (*s) console_putc(*s++);
}
