/* x2gui/main.c -- the tobyOS-NATIVE final stage of the X2 cross-personality
 * capstone pipeline:
 *
 *     /bin/win-xprod.exe | busybox sed 's/.../.../' | /bin/x2gui
 *      \__ Windows PE __/   \___ Linux (musl) ___/    \__ tobyOS GUI __/
 *
 * One busybox `sh` pipeline threads bytes through THREE personalities on one
 * kernel: a genuine Win32 PE produces a token, a real Linux/musl tool (busybox
 * sed) transforms it, and THIS native tobyOS app -- which speaks the tobyOS GUI
 * syscalls no Windows or Linux binary can -- consumes the bytes from its stdin
 * pipe, renders them into a real compositor window, and PROVES the render by
 * reading the window's own backbuffer pixels back (SYS_GUI_GETPIXELS). No
 * other OS can run this pipeline; tobyOS runs all three stages natively.
 *
 * It prints a serial verdict to stdout and exits 0 on full success, so the
 * pipeline's exit code (this is the last stage) carries the result up to the
 * X2 boot harness. Headless-friendly: PASS depends on the backbuffer pixels,
 * not on a visible display.
 */

typedef unsigned long      size_t;
typedef long               ssize_t;
typedef unsigned int       uint32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;

#define SYS_READ            2    /* tobyOS NATIVE ABI: 0=exit,1=write,2=read */
#define SYS_WRITE           1
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_GETPIXELS  88

static inline ssize_t sys_read(int fd, void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_READ), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
static inline ssize_t sys_write(int fd, const void *buf, size_t len) {
    ssize_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_WRITE), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return r;
}
static inline int sys_gui_create(uint32_t w, uint32_t h, const char *title) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_CREATE), "D"((long)w), "S"((long)h), "d"(title)
        : "rcx", "r11", "memory");
    return (int)r;
}
static inline int sys_gui_fill(int fd, int x, int y, int w, int h, uint32_t color) {
    long r;
    uint32_t whlen = ((uint32_t)(uint16_t)w) | (((uint32_t)(uint16_t)h) << 16);
    register long r10 __asm__("r10") = (long)whlen;
    register long r8  __asm__("r8")  = (long)color;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_FILL), "D"((long)fd), "S"((long)x), "d"((long)y),
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
        : "0"((long)SYS_GUI_TEXT), "D"((long)fd), "S"((long)xy), "d"(s),
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
static inline int sys_gui_getpixels(int fd, int x, int y, int w, int h, uint32_t *dst) {
    long r;
    uint32_t xy = ((uint32_t)(uint16_t)x) | (((uint32_t)(uint16_t)y) << 16);
    uint32_t wh = ((uint32_t)(uint16_t)w) | (((uint32_t)(uint16_t)h) << 16);
    register long r10 __asm__("r10") = (long)wh;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)SYS_GUI_GETPIXELS), "D"((long)fd), "S"((long)xy), "d"(dst),
          "r"(r10)
        : "rcx", "r11", "memory");
    return (int)r;
}

static size_t my_strlen(const char *s) {
    const char *p = s; while (*p) p++; return (size_t)(p - s);
}
static void putstr(const char *s) { sys_write(1, s, my_strlen(s)); }

/* naive substring search (no libc) */
static int contains(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = my_strlen(needle);
    if (nlen == 0 || hlen < nlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen && hay[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

#define WIN_W   360
#define WIN_H   120
#define TXT_X   12
#define TXT_Y   54
#define TXT_W   176        /* readback width (covers the rendered token row) */
#define TXT_H   8          /* one glyph row */
#define FG      0x00FFFFFFu   /* white glyph ink we count back */
#define BG      0xFF101828u   /* dark, opaque */

/* The token the Linux middle stage produces from the Windows token. */
#define X2_TOKEN  "X2_TOKEN_OK"

static char g_in[4096];

int main(int argc, char **argv);
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* 1) consume bytes from the pipe (Windows -> Linux -> us) */
    size_t total = 0;
    for (;;) {
        ssize_t n = sys_read(0, g_in + total, sizeof(g_in) - 1 - total);
        if (n <= 0) break;                 /* EOF when upstream stages close */
        total += (size_t)n;
        if (total >= sizeof(g_in) - 1) break;
    }
    int got_input = (total > 0);
    g_in[total] = 0;
    /* strip a trailing newline for tidy rendering */
    while (total && (g_in[total - 1] == '\n' || g_in[total - 1] == '\r'))
        g_in[--total] = 0;

    int token_ok = contains(g_in, total, X2_TOKEN);

    /* 2) render the received bytes into a NATIVE tobyOS window */
    int rendered = 0, inkpx = 0;
    int fd = sys_gui_create(WIN_W, WIN_H, "X2: Win -> Linux -> tobyOS");
    if (fd >= 0) {
        sys_gui_fill(fd, 0, 0, WIN_W, WIN_H, BG);
        sys_gui_text(fd, 12, 16, "tobyOS cross-personality capstone", FG, BG);
        sys_gui_text(fd, 12, 32, "Windows .exe -> Linux tool -> this GUI", 0x00B0C4DEu, BG);
        sys_gui_text(fd, TXT_X, TXT_Y, g_in, FG, BG);
        sys_gui_flip(fd);

        /* 3) PROVE the render: read the glyph row back out of the window's
         * own backbuffer and count white ink pixels. A blank window would
         * yield zero; rendered glyphs leave FG-coloured pixels behind.
         * The readback buffer is on the STACK (already-mapped pages near rsp):
         * the kernel's getpixels writes into the destination DIRECTLY, so a
         * demand-zero BSS buffer could fault an unmapped page in kernel mode.
         * A small stack buffer sidesteps that. */
        static volatile int sink;
        uint32_t px[TXT_W * TXT_H];
        for (int i = 0; i < TXT_W * TXT_H; i++) px[i] = 0x000000FFu;
        sink = px[0];                 /* defeat dead-store elision */
        int got = sys_gui_getpixels(fd, TXT_X, TXT_Y, TXT_W, TXT_H, px);
        if (got > 0) {
            int npx = got;
            if (npx > TXT_W * TXT_H) npx = TXT_W * TXT_H;
            for (int i = 0; i < npx; i++)
                if ((px[i] & 0x00FFFFFFu) == (FG & 0x00FFFFFFu)) inkpx++;
        }
        rendered = (inkpx >= 8);   /* a few glyphs' worth of ink */
    } else {
        putstr("[X2GUI] sys_gui_create failed (no GUI / no CAP_GUI)\n");
    }

    /* 4) Propagate the result via the EXIT CODE -- the only channel that
     * reliably reaches the serial log at boot (a pipeline tail's stdout does
     * not). The kernel logs "exit code=N" and our X2 harness asserts ==7:
     *   bit0 got_input, bit1 token transformed by the Linux stage, bit2 the
     *   tobyOS GUI render verified via backbuffer pixel readback.
     * The putstr lines below are extra diagnostics for when stdout IS a tty. */
    int code = (got_input ? 1 : 0) | (token_ok ? 2 : 0) | (rendered ? 4 : 0);
    int pass = (code == 7);
    putstr("[X2GUI] received='");
    putstr(g_in);
    putstr("'\n");
    {
        /* tiny manual int->dec for inkpx, no libc */
        char nb[16]; int p = 0, v = inkpx;
        if (v == 0) nb[p++] = '0';
        char tmp[16]; int t = 0;
        while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t > 0) nb[p++] = tmp[--t];
        nb[p] = 0;
        putstr("[X2GUI] VERDICT: ");
        putstr(pass ? "PASS" : "FAIL");
        putstr(" token=");
        putstr(token_ok ? "yes" : "no");
        putstr(" rendered=");
        putstr(rendered ? "yes" : "no");
        putstr(" inkpx=");
        putstr(nb);
        putstr("\n");
    }
    return code;   /* 7 = got_input | token | rendered (full cross-personality pass) */
}
