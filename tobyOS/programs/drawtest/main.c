/* programs/drawtest/main.c -- M27A: bounded shape drawing validator.
 *
 * Opens a 320x240 GUI window and exercises every primitive the kernel
 * currently exposes:
 *   1. solid rectangles (4 corners + a centred hero)
 *   2. a horizontal colour ramp (gradient fallback in 32-step bands)
 *   3. text labels in three colour pairs (incl. the GFX_TRANSPARENT bg)
 *   4. an "off-screen partially overlapping" rect to exercise clipping
 *
 * The point of `drawtest` is to PROVE the primitives don't write
 * out-of-bounds. We can't see "did the wall behind the framebuffer
 * change?" from userland, but we CAN:
 *   - assert every syscall returned non-negative;
 *   - confirm subsequent calls (which a corrupted heap would crash)
 *     still complete;
 *   - rely on the kernel's clip_rect() machinery to do its job, then
 *     query gfx state via SYS_DISPLAY_INFO to verify the back buffer
 *     hasn't drifted (bpp, dimensions, pitch all preserved).
 *
 * The window auto-closes after one full draw cycle. Test mode (the
 * default) writes a PASS/FAIL summary to stdout and exits 0/1. With
 * --interactive the program loops on poll_event so the user can
 * eyeball the result before hitting close.
 *
 * Exit codes:
 *   0  every primitive succeeded; display state intact.
 *   1  at least one syscall returned a negative value, or display
 *      info changed mid-test (back-buffer corruption).
 *   2  bad usage. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include <tobyos_devtest.h>

/* ---- raw GUI syscalls (no libtoby wrappers yet) ------------------ */

#define SYS_YIELD            5
#define SYS_GUI_CREATE      10
#define SYS_GUI_FILL        11
#define SYS_GUI_TEXT        12
#define SYS_GUI_FLIP        13
#define SYS_GUI_POLL_EVENT  14

#define GUI_TRANSPARENT_BG  0xFF000000u

struct gui_event {
    int           type;
    int           x;
    int           y;
    unsigned char button;
    unsigned char key;
    unsigned char _pad[2];
};
#define GUI_EV_NONE  0

static inline void sys_yield(void) {
    /* rax is in/out -- if we don't tie it back, the compiler caches
     * SYS_YIELD across calls and the next `syscall` fires SYS_EXIT(0). */
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_YIELD)
        : "rcx", "r11", "memory");
    (void)r;
}
static inline int sys_gui_create(unsigned w, unsigned h, const char *title) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_CREATE), "D"((long)w), "S"((long)h), "d"(title)
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
          "S"((long)x), "d"((long)y),
          "r"(r10), "r"(r8)
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
          "S"((long)xy), "d"(s),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_flip(int fd) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FLIP), "D"((long)fd)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_poll_event(int fd, struct gui_event *ev) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_POLL_EVENT), "D"((long)fd), "S"(ev)
        : "rcx", "r11", "memory");
    return (int)r;
}

#define WIN_W 320
#define WIN_H 240

/* Track every syscall result so we can verify ALL of them succeeded. */
static int g_fails;
static int g_calls;
static const char *g_first_fail;

static void check_call(int rc, const char *label) {
    g_calls++;
    if (rc < 0) {
        g_fails++;
        if (!g_first_fail) g_first_fail = label;
    }
}

/* ---- draw passes ------------------------------------------------ */

static void draw_corners(int fd) {
    /* Four 32x24 rectangles, one in each corner. Corner ordering is
     * NW, NE, SW, SE so the printout below remains stable. */
    check_call(sys_gui_fill(fd, 0,           0,            32, 24, 0x00FF0000u), "fill NW");
    check_call(sys_gui_fill(fd, WIN_W - 32,  0,            32, 24, 0x0000FF00u), "fill NE");
    check_call(sys_gui_fill(fd, 0,           WIN_H - 24,   32, 24, 0x000000FFu), "fill SW");
    check_call(sys_gui_fill(fd, WIN_W - 32,  WIN_H - 24,   32, 24, 0x00FFFF00u), "fill SE");
}

static void draw_hero_rect(int fd) {
    /* Centred 200x80 with a 1px outline (drawn as 4 thin rects so we
     * exercise the same path desktop chrome uses for the selection
     * highlight). */
    int x = (WIN_W - 200) / 2;
    int y = 60;
    check_call(sys_gui_fill(fd, x,       y,       200, 80, 0x00404060u), "hero fill");
    check_call(sys_gui_fill(fd, x,       y,       200,  1, 0x00FFFFFFu), "hero top");
    check_call(sys_gui_fill(fd, x,       y + 79,  200,  1, 0x00FFFFFFu), "hero bot");
    check_call(sys_gui_fill(fd, x,       y,         1, 80, 0x00FFFFFFu), "hero left");
    check_call(sys_gui_fill(fd, x + 199, y,         1, 80, 0x00FFFFFFu), "hero right");
}

static void draw_color_ramp(int fd) {
    /* 32-step horizontal ramp across the bottom 16 px of the window.
     * Each step is WIN_W/32 wide. */
    int strip_y = WIN_H - 24;
    int step    = WIN_W / 32;
    for (int i = 0; i < 32; i++) {
        unsigned shade = (unsigned)((i * 255) / 31);
        unsigned color = (shade << 16) | (shade << 8) | shade;
        check_call(sys_gui_fill(fd, i * step, strip_y, step, 8, color),
                   "ramp step");
    }
}

static void draw_labels(int fd) {
    check_call(sys_gui_text(fd, 8,  4,  "drawtest M27A",
                            0x00FFFFFFu, 0x00000000u), "label title");
    check_call(sys_gui_text(fd, 8, 32,  "rect/grad/text/clip",
                            0x00CCCCCCu, GUI_TRANSPARENT_BG),
                            "label subtitle");
    check_call(sys_gui_text(fd, 8, WIN_H - 12, "PASS",
                            0x0000FF00u, 0x00000000u), "label PASS");
}

static void draw_clipping_test(int fd) {
    /* Three rects that intentionally extend beyond the window:
     * negative origin, oversized width, and one fully off-screen.
     * The kernel must clip silently; if any of these returned an
     * error the back buffer would be in an indeterminate state. */
    check_call(sys_gui_fill(fd, -16, 100,  64, 32, 0x00FF00FFu), "clip neg");
    check_call(sys_gui_fill(fd, WIN_W - 32, 150, 128, 32, 0x0000FFFFu), "clip wide");
    check_call(sys_gui_fill(fd, WIN_W + 100, 200, 32, 32, 0x00FFFFFFu), "clip off");
}

/* ---- one full pass -------------------------------------------- */

static int pre_state_ok(struct abi_display_info *snap) {
    /* Compare with state captured before drawing. Width/height/bpp/
     * pitch are immutable; if any of those changed we know the
     * back buffer was reallocated under us. */
    static struct abi_display_info post[ABI_DISPLAY_MAX_OUTPUTS];
    int n = tobydisp_list(post, ABI_DISPLAY_MAX_OUTPUTS);
    if (n < 0) {
        printf("FAIL: drawtest: tobydisp_list errno=%d\n", errno);
        return 0;
    }
    if (n == 0) {
        /* Headless. We accept it -- drawing was a no-op then, but
         * that's still PASS for the syscall ABI. */
        return 1;
    }
    if (snap->width != post[0].width ||
        snap->height != post[0].height ||
        snap->pitch_bytes != post[0].pitch_bytes ||
        snap->bpp != post[0].bpp) {
        printf("FAIL: drawtest: display geometry drifted "
               "(%ux%u/%u/%u -> %ux%u/%u/%u)\n",
               (unsigned)snap->width,  (unsigned)snap->height,
               (unsigned)snap->pitch_bytes, (unsigned)snap->bpp,
               (unsigned)post[0].width, (unsigned)post[0].height,
               (unsigned)post[0].pitch_bytes, (unsigned)post[0].bpp);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    int interactive = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--interactive")) interactive = 1;
        else {
            fprintf(stderr, "FAIL: drawtest: unknown flag '%s'\n", argv[i]);
            fprintf(stderr, "usage: drawtest [--interactive]\n");
            return 2;
        }
    }

    /* Snapshot the display BEFORE we open a window. */
    static struct abi_display_info pre[ABI_DISPLAY_MAX_OUTPUTS];
    int npre = tobydisp_list(pre, ABI_DISPLAY_MAX_OUTPUTS);
    if (npre < 0) {
        fprintf(stderr,
                "FAIL: drawtest: tobydisp_list (pre) errno=%d\n", errno);
        return 1;
    }
    if (npre == 0) {
        printf("INFO: drawtest: no display present -- skipping draw cycle\n");
        printf("PASS: drawtest: 0 primitives (headless)\n");
        return 0;
    }

    int fd = sys_gui_create(WIN_W, WIN_H, "drawtest");
    if (fd < 0) {
        fprintf(stderr, "FAIL: drawtest: sys_gui_create rc=%d\n", fd);
        return 1;
    }

    /* Background paint first. Counts as one of the primitive checks. */
    check_call(sys_gui_fill(fd, 0, 0, WIN_W, WIN_H, 0x00101820u),
               "bg fill");

    draw_corners       (fd);
    draw_hero_rect     (fd);
    draw_color_ramp    (fd);
    draw_clipping_test (fd);
    draw_labels        (fd);

    int flip_rc = sys_gui_flip(fd);
    check_call(flip_rc, "flip");

    if (!pre_state_ok(&pre[0])) return 1;

    if (interactive) {
        printf("INFO: drawtest: window left open. close it to exit.\n");
        for (;;) {
            struct gui_event ev;
            int got = sys_gui_poll_event(fd, &ev);
            if (got == 0) { sys_yield(); continue; }
            if (got < 0)  break;
        }
    }

    if (g_fails) {
        printf("FAIL: drawtest: %d/%d primitive(s) failed; first=%s\n",
               g_fails, g_calls, g_first_fail ? g_first_fail : "(unknown)");
        return 1;
    }
    /* g_calls is whatever the structure of the draw passes produced;
     * we report it verbatim so any test script can grep an integer. */
    printf("PASS: drawtest: %d primitive(s) ran cleanly; "
           "%dx%d window flipped\n", g_calls, WIN_W, WIN_H);
    return 0;
}
