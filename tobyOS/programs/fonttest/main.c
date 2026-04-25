/* programs/fonttest/main.c -- M27D: scaled bitmap-font validator.
 *
 * tobyOS deliberately does not pull in stb_truetype or FreeType for
 * M27D. Instead the kernel exposes a supersampled + corner-smoothed
 * bitmap-font path (see gfx_draw_text_scaled / gfx_draw_text_smooth
 * and the per-window gui_window_text_scaled). This program exercises
 * the per-window path through the new SYS_GUI_TEXT_SCALED syscall:
 *
 *   case bitmap   small text at the legacy 8x8 cell (scale=1).
 *   case scale2   "ABC123" at scale=2 (16x16 cell).
 *   case scale4   "ABC123" at scale=4 (32x32 cell).
 *   case smooth   same string at scale=4 with corner smoothing.
 *   case mixed    letters/numbers/symbols all on one window.
 *   case fallback -- one call with the legacy SYS_GUI_TEXT to prove
 *                    that the bitmap fallback didn't regress.
 *
 * Each case opens a transient 320x200 window, paints, presents,
 * destroys. We can't read the framebuffer back, so PASS = every
 * syscall returned >= 0. The kernel-side display_test_font test
 * verifies the gfx_text_bounds math; here we just exercise the
 * syscall path end-to-end.
 *
 * Exit codes: 0 all pass, 1 any FAIL, 2 bad usage. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <tobyos_devtest.h>

#define SYS_YIELD            5
#define SYS_GUI_CREATE      10
#define SYS_GUI_FILL        11
#define SYS_GUI_TEXT        12
#define SYS_GUI_FLIP        13
#define SYS_CLOSE            4
#define SYS_GUI_TEXT_SCALED 56  /* M27D */

#define GUI_TRANSPARENT_BG   0xFF000000u

static inline void sys_yield(void) {
    /* See note in rendertest -- rax must be tied as both in/out so the
     * compiler reloads SYS_YIELD before each `syscall`. Otherwise the
     * second consecutive yield fires SYS_EXIT(rdi) instead. */
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_YIELD)
        : "rcx", "r11", "memory");
    (void)r;
}
static inline int sys_close(int fd) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "0"((long)SYS_CLOSE), "D"((long)fd)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_create(unsigned w, unsigned h, const char *t) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_CREATE), "D"((long)w), "S"((long)h), "d"(t)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_fill(int fd, int x, int y, int w, int h,
                               unsigned color) {
    long r;
    unsigned whlen = ((unsigned)(unsigned short)w) |
                     (((unsigned)(unsigned short)h) << 16);
    register long r10 __asm__("r10") = (long)whlen;
    register long r8  __asm__("r8")  = (long)color;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FILL), "D"((long)fd),
          "S"((long)x), "d"((long)y), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_text(int fd, int x, int y, const char *s,
                               unsigned fg, unsigned bg) {
    long r;
    unsigned xy = ((unsigned)(unsigned short)x) |
                  (((unsigned)(unsigned short)y) << 16);
    register long r10 __asm__("r10") = (long)fg;
    register long r8  __asm__("r8")  = (long)bg;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_TEXT), "D"((long)fd),
          "S"((long)xy), "d"(s), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_flip(int fd) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "0"((long)SYS_GUI_FLIP), "D"((long)fd)
        : "rcx", "r11", "memory");
    return (int)r;
}
/* M27D: a5 packs (bg | scale<<24 | smooth<<31). */
static inline int sys_gui_text_scaled(int fd, int x, int y, const char *s,
                                      unsigned fg, unsigned bg,
                                      int scale, int smooth) {
    long r;
    unsigned xy = ((unsigned)(unsigned short)x) |
                  (((unsigned)(unsigned short)y) << 16);
    /* GFX_TRANSPARENT (0xFF000000) collides with the packed scale
     * byte; the kernel re-promotes the sentinel 0x00FFFFFE -> the
     * canonical transparent value. */
    if (bg == GUI_TRANSPARENT_BG) bg = 0x00FFFFFEu;
    unsigned packed = (bg & 0x00FFFFFFu) |
                      (((unsigned)scale  & 0x7Fu) << 24) |
                      (((unsigned)smooth & 0x1u ) << 31);
    register long r10 __asm__("r10") = (long)fg;
    register long r8  __asm__("r8")  = (long)packed;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_TEXT_SCALED), "D"((long)fd),
          "S"((long)xy), "d"(s), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}

/* ---- per-case scaffolding -------------------------------------- */

static int g_pass, g_fail;

static void emit(const char *name, int rc, const char *detail) {
    const char *tag = (rc == 0) ? "PASS" : "FAIL";
    if (rc == 0) g_pass++; else g_fail++;
    printf("[%s] fonttest %-10s -- %s\n", tag, name, detail ? detail : "");
}

/* All cases reuse this scratch window. */
static int open_scratch(const char *title) {
    int fd = sys_gui_create(320, 200, title);
    if (fd < 0) return -1;
    sys_gui_fill(fd, 0, 0, 320, 200, 0x00181820u);
    return fd;
}

static int case_bitmap(void) {
    int fd = open_scratch("ft-bitmap");
    if (fd < 0) return -1;
    int errs = 0;
    errs += (sys_gui_text(fd, 8, 8, "fonttest legacy 8x8 path",
                          0x00FFFFFFu, GUI_TRANSPARENT_BG) < 0);
    errs += (sys_gui_flip(fd) < 0);
    sys_close(fd);
    return errs ? -1 : 0;
}

static int case_scaled(int scale, int smooth, const char *label) {
    int fd = open_scratch(label);
    if (fd < 0) return -1;
    int errs = 0;
    errs += (sys_gui_text_scaled(fd, 8, 8, "ABC123",
                                 0x00FFFFFFu, GUI_TRANSPARENT_BG,
                                 scale, smooth) < 0);
    errs += (sys_gui_text_scaled(fd, 8, 8 + 8 * scale + 4,
                                 "Hello, World!",
                                 0x0080C8FFu, 0x00181820u,
                                 scale, smooth) < 0);
    errs += (sys_gui_flip(fd) < 0);
    sys_close(fd);
    return errs ? -1 : 0;
}

static int case_mixed(void) {
    int fd = open_scratch("ft-mixed");
    if (fd < 0) return -1;
    int errs = 0;
    /* Letters, numbers, symbols, multi-line. */
    errs += (sys_gui_text_scaled(fd, 8,  8,  "abcdef ABCDEF",
                                 0x00FFFFFFu, GUI_TRANSPARENT_BG,
                                 2, 0) < 0);
    errs += (sys_gui_text_scaled(fd, 8,  40, "0123456789",
                                 0x0080FF80u, GUI_TRANSPARENT_BG,
                                 2, 1) < 0);
    errs += (sys_gui_text_scaled(fd, 8,  72, "!@#$%^&*()_+",
                                 0x00FF8080u, GUI_TRANSPARENT_BG,
                                 2, 1) < 0);
    /* Multi-line at scale=3 to stress the bounds path. */
    errs += (sys_gui_text_scaled(fd, 8, 110, "MULTI\nLINE",
                                 0x00FFFF80u, GUI_TRANSPARENT_BG,
                                 3, 1) < 0);
    errs += (sys_gui_flip(fd) < 0);
    sys_close(fd);
    return errs ? -1 : 0;
}

/* Defensive: scale=0 must be treated as 1 server-side, scale=99 must
 * clamp to 32. Both should still return 0 -- if either fails the
 * kernel sanitiser is letting bad input through. */
static int case_clamp(void) {
    int fd = open_scratch("ft-clamp");
    if (fd < 0) return -1;
    int errs = 0;
    errs += (sys_gui_text_scaled(fd, 4, 4, "s=0",
                                 0x00FFFFFFu, GUI_TRANSPARENT_BG,
                                 0, 0) < 0);
    errs += (sys_gui_text_scaled(fd, 4, 24, "s=99",
                                 0x00FFFFFFu, GUI_TRANSPARENT_BG,
                                 99, 0) < 0);
    errs += (sys_gui_flip(fd) < 0);
    sys_close(fd);
    return errs ? -1 : 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Order matches the regression script. case_bitmap first proves
     * the legacy fallback still works; if it fails we don't have a
     * baseline to compare against. */
    emit("bitmap",   case_bitmap(),                  "8x8 legacy path");
    emit("scale2",   case_scaled(2, 0, "ft-s2"),     "16x16 cell");
    emit("scale4",   case_scaled(4, 0, "ft-s4"),     "32x32 cell");
    emit("smooth",   case_scaled(4, 1, "ft-sm"),     "32x32 + corner smoothing");
    emit("mixed",    case_mixed(),                   "letters/numbers/symbols + multiline");
    emit("clamp",    case_clamp(),                   "scale=0 -> 1, scale=99 -> 32");
    int total = g_pass + g_fail;
    printf("[fonttest] %d/%d PASS (%d FAIL)\n",
           g_pass, total, g_fail);
    return g_fail ? 1 : 0;
}
