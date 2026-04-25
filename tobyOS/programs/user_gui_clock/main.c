/* user_gui_clock/main.c -- minimal "hello window" with a tick counter.
 *
 * Demonstrates the GUI API without any input handling: opens one
 * window, prints a message, then redraws an incrementing counter once
 * per ~100 yields. Useful as the simplest possible reference for
 * future GUI apps + as a second window the demo can layer on top of.
 *
 * Ctrl+C from the shell tears it down; the kernel auto-closes the
 * window through the file_close -> gui_window_close path.
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

/* ---- syscall stubs --------------------------------------------- */

static inline ssize_t sys_write(int fd, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
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

/* ---- helpers --------------------------------------------------- */

static size_t my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}
static void putstr_console(const char *s) {
    sys_write(1, s, my_strlen(s));
}

static void fmt_uint(char *out, unsigned v) {
    char tmp[16]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    else while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int i = 0;
    while (n--) out[i++] = tmp[n];
    out[i] = '\0';
}

#define WIN_W 200
#define WIN_H 80

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fd = sys_gui_create(WIN_W, WIN_H, "hello window");
    if (fd < 0) {
        putstr_console("gui_clock: sys_gui_create failed\n");
        return 1;
    }
    sys_gui_fill(fd, 0, 0, WIN_W, WIN_H, 0x00102030u);
    sys_gui_text(fd, 12, 12, "hello from /bin/gui_clock",
                 0x00FFFFFFu, 0xFF000000u);
    sys_gui_flip(fd);

    /* Update a tick counter forever. We don't have wall-clock time
     * accessible from userspace yet, so this is just a visible heart-
     * beat that the GUI is repainting. */
    unsigned ticks = 0;
    char buf[32];
    for (;;) {
        for (int i = 0; i < 4000; i++) sys_yield();
        ticks++;
        sys_gui_fill(fd, 12, 40, 180, 16, 0x00102030u);
        const char *prefix = "ticks: ";
        char line[40];
        size_t off = 0;
        for (size_t k = 0; k < my_strlen(prefix); k++) line[off++] = prefix[k];
        fmt_uint(buf, ticks);
        for (size_t k = 0; buf[k]; k++) line[off++] = buf[k];
        line[off] = '\0';
        sys_gui_text(fd, 12, 44, line, 0x00FFFF80u, 0xFF000000u);
        sys_gui_flip(fd);
    }
}
