/* user_gui_demo/main.c -- interactive GUI demo for /bin/gui_demo.
 *
 * Creates a 320x200 window, paints a checkerboard background with
 * instructions, then loops:
 *   - sys_gui_poll_event() for mouse events
 *   - on MOUSE_DOWN: drop a 16x16 coloured square at the click point
 *     and flip the window
 *   - on MOUSE_MOVE while a button is held: drag a paint trail
 *   - sys_yield() between polls so we don't peg a CPU
 *
 * Ctrl+C from the shell tears the process down; the kernel auto-closes
 * the window via the FILE_KIND_WINDOW close dispatch.
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;

#define SYS_WRITE           1
#define SYS_YIELD           5
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14

#define GUI_EV_NONE        0
#define GUI_EV_MOUSE_MOVE  1
#define GUI_EV_MOUSE_DOWN  2
#define GUI_EV_MOUSE_UP    3

struct gui_event {
    int     type;
    int     x;
    int     y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};

/* ---- syscall stubs --------------------------------------------- */

static inline ssize_t sys_write(int fd, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
/* sys_exit is intentionally not declared here -- start.S calls SYS_EXIT
 * with main's return value, so a return is enough to terminate. */

static inline void sys_yield(void) {
    __asm__ volatile ("syscall"
        : : "a"((long)SYS_YIELD)
        : "rcx", "r11", "memory");
}
static inline int sys_gui_create(uint32_t w, uint32_t h, const char *title) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_CREATE), "D"((long)w), "S"((long)h), "d"(title)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_fill(int fd, int x, int y, int w, int h,
                               uint32_t color) {
    long r;
    /* Pack (w, h) into one 32-bit value; both fit in 16 bits. */
    uint32_t whlen = ((uint32_t)(uint16_t)w) |
                     (((uint32_t)(uint16_t)h) << 16);
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
                               uint32_t fg, uint32_t bg) {
    long r;
    uint32_t xy = ((uint32_t)(uint16_t)x) | (((uint32_t)(uint16_t)y) << 16);
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

/* ---- helpers --------------------------------------------------- */

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}
static void putstr(const char *s) {
    sys_write(1, s, my_strlen(s));
}

/* Tiny LCG so each click gets a different colour without dragging in
 * a real RNG. Period is plenty for a demo. */
static uint32_t g_rng_state = 0xc0ffee01u;
static uint32_t prng(void) {
    g_rng_state = g_rng_state * 1103515245u + 12345u;
    return g_rng_state;
}

#define WIN_W 320
#define WIN_H 200

static void paint_background(int fd) {
    /* Checkerboard so it's obvious the window has its own framebuffer
     * separate from the desktop background. */
    const uint32_t a = 0x00203040u;
    const uint32_t b = 0x00405060u;
    for (int ty = 0; ty < WIN_H; ty += 16) {
        for (int tx = 0; tx < WIN_W; tx += 16) {
            uint32_t c = ((tx ^ ty) & 16) ? a : b;
            sys_gui_fill(fd, tx, ty, 16, 16, c);
        }
    }
    sys_gui_text(fd, 8,  8,  "tobyOS GUI demo",            0x00FFFFFFu, 0xFF000000u);
    sys_gui_text(fd, 8,  24, "drag the title to move me",  0x00CCCCCCu, 0xFF000000u);
    sys_gui_text(fd, 8,  36, "click to drop a paint cell", 0x00CCCCCCu, 0xFF000000u);
    sys_gui_text(fd, 8,  48, "Ctrl+C in shell to quit",    0x00CCCCCCu, 0xFF000000u);
}

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fd = sys_gui_create(WIN_W, WIN_H, "tobyOS demo");
    if (fd < 0) {
        putstr("gui_demo: sys_gui_create failed (no GUI? out of slots?)\n");
        return 1;
    }
    paint_background(fd);
    sys_gui_flip(fd);

    int last_left = 0;

    for (;;) {
        struct gui_event ev;
        int got = sys_gui_poll_event(fd, &ev);
        if (got < 0) { putstr("gui_demo: poll error\n"); return 1; }
        if (got == 0) {
            sys_yield();
            continue;
        }

        int redraw = 0;
        if (ev.type == GUI_EV_MOUSE_DOWN) {
            uint32_t c = (prng() & 0x00FFFFFFu) | 0x00404040u;
            sys_gui_fill(fd, ev.x - 8, ev.y - 8, 16, 16, c);
            last_left = (ev.button & 1);
            redraw = 1;
        } else if (ev.type == GUI_EV_MOUSE_UP) {
            last_left = 0;
        } else if (ev.type == GUI_EV_MOUSE_MOVE && last_left &&
                   (ev.button & 1)) {
            uint32_t c = (prng() & 0x00FFFFFFu) | 0x00404040u;
            sys_gui_fill(fd, ev.x - 3, ev.y - 3, 6, 6, c);
            redraw = 1;
        }
        if (redraw) sys_gui_flip(fd);
    }
}
