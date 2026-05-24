/* programs/bluescreen/main.c -- Blue Screen of Death for tobyOS.
 *
 * Kernel calls this on panic. Sets the framebuffer to blue, displays
 * the error info, and waits for a keypress or auto-restart after 30s.
 */

typedef unsigned long      size_t;
typedef unsigned long long uint64_t;
typedef unsigned int       uint32_t;
typedef unsigned short     uint16_t;
typedef unsigned char      uint8_t;
typedef long               ssize_t;

#define SYS_WRITE           1
#define SYS_READ            2
#define SYS_EXIT            3
#define SYS_YIELD           5
#define SYS_OPEN            6
#define SYS_CLOSE           7
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_CLOCK_MS       48
#define SYS_REBOOT         80

struct gui_event {
    int     type;
    int     x, y;
    uint8_t button;
    uint8_t key;
    uint8_t _pad[2];
};

#define GUI_EV_KEY   4
#define GUI_EV_CLOSE 5

/* ── syscall wrappers ────────────────────────────────────────────── */

static long sc1(long n, long a) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a):"rcx","r11","memory");
    return r;
}
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");
    return r;
}
static long sc6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall":"=a"(r)
        :"0"(n),"D"(a1),"S"(a2),"d"(a3),"r"(r10),"r"(r8),"r"(r9)
        :"rcx","r11","memory");
    return r;
}

static void sys_yield(void) { sc1(SYS_YIELD, 0); }
static int  sys_gui_create(int w, int h, const char *t) { return (int)sc3(SYS_GUI_CREATE,w,h,(long)t); }
static void sys_gui_fill(int fd,int x,int y,int w,int h,unsigned c) { sc6(SYS_GUI_FILL,fd,x,y,w,((long)h<<32)|c,0); }
static void sys_gui_text(int fd,int x,int y,const char *s,unsigned fg,unsigned bg) { sc6(SYS_GUI_TEXT,fd,x,y,(long)s,fg,bg); }
static void sys_gui_flip(int fd) { sc1(SYS_GUI_FLIP, fd); }
static int  sys_gui_poll(int fd, struct gui_event *ev) { return (int)sc3(SYS_GUI_POLL_EVENT,fd,(long)ev,0); }
static long sys_clock_ms(void) { return sc1(SYS_CLOCK_MS, 0); }

/* ── string helpers ──────────────────────────────────────────────── */

__attribute__((unused))
static size_t my_strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

static void my_itoa(int v, char *buf, int base) {
    char tmp[20];
    int i = 0;
    int neg = 0;
    if (v < 0 && base == 10) { neg = 1; v = -v; }
    unsigned u = (unsigned)v;
    do {
        int d = (int)(u % (unsigned)base);
        tmp[i++] = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
        u /= (unsigned)base;
    } while (u > 0);
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void hex64(uint64_t v, char *buf) {
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        int d = (int)(v & 0xF);
        buf[2 + i] = (d < 10) ? (char)('0' + d) : (char)('A' + d - 10);
        v >>= 4;
    }
    buf[18] = '\0';
}

/* ── crash info structure (matches kernel crashdump.h) ───────────── */

struct crash_info {
    uint64_t rip, rsp, rbp;
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t cr2, cr3;
    uint32_t error_code;
    char     message[128];
    char     process_name[32];
    int      pid;
};

/* ── constants ───────────────────────────────────────────────────── */

#define WIN_W 640
#define WIN_H 480
#define BLUE  0x0000AA
#define WHITE 0xFFFFFF
#define GRAY  0xAAAAAA
#define AUTO_RESTART_MS 30000

static int g_fd = -1;

static void draw_line(int y, const char *text, unsigned fg) {
    sys_gui_text(g_fd, 40, y, text, fg, BLUE);
}

static void draw_reg(int y, const char *name, uint64_t val) {
    char line[48];
    char hx[20];
    hex64(val, hx);
    int i = 0;
    const char *p = name;
    while (*p && i < 5) line[i++] = *p++;
    line[i++] = ':';
    line[i++] = ' ';
    const char *h = hx;
    while (*h && i < 46) line[i++] = *h++;
    line[i] = '\0';
    draw_line(y, line, WHITE);
}

static void redraw(const struct crash_info *ci, int countdown) {
    sys_gui_fill(g_fd, 0, 0, WIN_W, WIN_H, BLUE);

    int y = 30;

    draw_line(y, ":-( tobyOS has encountered a critical error", WHITE);
    y += 30;
    draw_line(y, "================================================", GRAY);
    y += 25;

    if (ci->message[0])
        draw_line(y, ci->message, WHITE);
    else
        draw_line(y, "Unknown fatal error", WHITE);
    y += 25;

    if (ci->process_name[0]) {
        char pline[64];
        int i = 0;
        const char *s = "Process: ";
        while (*s) pline[i++] = *s++;
        s = ci->process_name;
        while (*s && i < 50) pline[i++] = *s++;
        pline[i++] = ' '; pline[i++] = '(';
        pline[i++] = 'P'; pline[i++] = 'I'; pline[i++] = 'D'; pline[i++] = ' ';
        char pidstr[8];
        my_itoa(ci->pid, pidstr, 10);
        s = pidstr;
        while (*s && i < 60) pline[i++] = *s++;
        pline[i++] = ')';
        pline[i] = '\0';
        draw_line(y, pline, WHITE);
        y += 20;
    }

    y += 5;
    draw_reg(y, "RIP", ci->rip);   y += 16;
    draw_reg(y, "RSP", ci->rsp);   y += 16;
    draw_reg(y, "RBP", ci->rbp);   y += 16;
    draw_reg(y, "RAX", ci->rax);   y += 16;
    draw_reg(y, "RBX", ci->rbx);   y += 16;
    draw_reg(y, "RCX", ci->rcx);   y += 16;
    draw_reg(y, "RDX", ci->rdx);   y += 16;
    draw_reg(y, "CR2", ci->cr2);   y += 16;
    draw_reg(y, "CR3", ci->cr3);   y += 20;

    draw_line(y, "================================================", GRAY);
    y += 25;

    char restart[64] = "Press any key to restart";
    if (countdown > 0) {
        char cd[8];
        my_itoa(countdown, cd, 10);
        int i = 0;
        const char *s = "Auto-restart in ";
        while (*s) restart[i++] = *s++;
        s = cd;
        while (*s) restart[i++] = *s++;
        s = " seconds...";
        while (*s) restart[i++] = *s++;
        restart[i] = '\0';
    }
    draw_line(y, restart, GRAY);

    sys_gui_flip(g_fd);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct crash_info ci;
    for (size_t i = 0; i < sizeof(ci); i++)
        ((uint8_t *)&ci)[i] = 0;

    /* Try to read crash dump from /system/crashdump.bin */
    int cfd = (int)sc3(SYS_OPEN, (long)"/system/crashdump.bin", 0, 0);
    if (cfd >= 0) {
        sc3(SYS_READ, cfd, (long)&ci, (long)sizeof(ci));
        sc1(SYS_CLOSE, cfd);
    } else {
        const char *msg = "Unknown critical error";
        for (int i = 0; msg[i] && i < 127; i++)
            ci.message[i] = msg[i];
        const char *pn = "system";
        for (int i = 0; pn[i] && i < 31; i++)
            ci.process_name[i] = pn[i];
    }

    g_fd = sys_gui_create(WIN_W, WIN_H, "");
    if (g_fd < 0) {
        sc3(SYS_WRITE, 1, (long)"bluescreen: no GUI\n", 19);
        sc1(SYS_EXIT, 1);
        for (;;) __asm__ volatile("hlt");
    }

    long start = sys_clock_ms();

    for (;;) {
        struct gui_event ev;
        while (sys_gui_poll(g_fd, &ev)) {
            if (ev.type == GUI_EV_KEY || ev.type == GUI_EV_CLOSE) {
                sc1(SYS_EXIT, 0);
                for (;;) __asm__ volatile("hlt");
            }
        }

        long now = sys_clock_ms();
        long elapsed = now - start;
        int secs_left = (int)((AUTO_RESTART_MS - elapsed) / 1000);
        if (secs_left < 0) secs_left = 0;

        redraw(&ci, secs_left);

        if (elapsed >= AUTO_RESTART_MS) {
            sc1(SYS_EXIT, 0);
            for (;;) __asm__ volatile("hlt");
        }

        sys_yield();
    }
}
