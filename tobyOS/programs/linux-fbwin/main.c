/* programs/linux-fbwin/main.c
 *
 * A GENUINE Linux x86-64 static ELF (raw syscalls, no libc) fbdev app -- the
 * same shape as programs/linux-fb but it STAYS ALIVE and animates, so it can be
 * one of the three live windows in the GUI-framework "three worlds" demo (a
 * native tobyOS app + an unmodified Windows .exe + this unmodified Linux ELF,
 * all composited as peer windows on one desktop).
 *
 * It opens /dev/fb0 (which, when the compositor is up, is presented INTO a
 * compositor window), queries geometry, mmaps the scanout, then loops: draw a
 * gradient + RGB marker squares + a bouncing white block, FBIOPAN_DISPLAY, and
 * nanosleep ~60 ms. It would run unmodified on a real Linux kernel.
 */

typedef unsigned long  u64;
typedef long           i64;
typedef unsigned int   u32;

static inline i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    register i64 r9  __asm__("r9")  = f;
    i64 r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

#define SYS_write       1
#define SYS_open        2
#define SYS_ioctl       16
#define SYS_mmap        9
#define SYS_nanosleep   35
#define SYS_exit_group  231

#define O_RDWR          2
#define PROT_READ       1
#define PROT_WRITE      2
#define MAP_SHARED      1

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { sc(SYS_write, 1, (i64)s, (i64)slen(s), 0, 0, 0); }

static void fill(volatile u32 *fb, u32 pitch, u32 W, u32 H,
                 int x0, int y0, int w, int h, u32 col) {
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || (u32)y >= H) continue;
        for (int x = x0; x < x0 + w; x++) {
            if (x < 0 || (u32)x >= W) continue;
            fb[(u32)y * pitch + (u32)x] = col;
        }
    }
}

__attribute__((force_align_arg_pointer))
void _start(void) {
    i64 fd = sc(SYS_open, (i64)"/dev/fb0", O_RDWR, 0, 0, 0, 0);
    if (fd < 0) { put("fbwin: open /dev/fb0 FAILED\n"); sc(SYS_exit_group, 2, 0,0,0,0,0); }

    u32 vinfo[40];
    for (int i = 0; i < 40; i++) vinfo[i] = 0;
    if (sc(SYS_ioctl, fd, FBIOGET_VSCREENINFO, (i64)vinfo, 0, 0, 0) < 0) {
        put("fbwin: VSCREENINFO FAILED\n"); sc(SYS_exit_group, 3, 0,0,0,0,0);
    }
    u32 W = vinfo[0], H = vinfo[1], bpp = vinfo[6];
    if (W == 0 || H == 0 || bpp != 32) { put("fbwin: bad geometry\n"); sc(SYS_exit_group, 3, 0,0,0,0,0); }

    unsigned char finfo[80];
    for (int i = 0; i < 80; i++) finfo[i] = 0;
    if (sc(SYS_ioctl, fd, FBIOGET_FSCREENINFO, (i64)finfo, 0, 0, 0) < 0) {
        put("fbwin: FSCREENINFO FAILED\n"); sc(SYS_exit_group, 4, 0,0,0,0,0);
    }
    u32 smem_len    = *(u32 *)(finfo + 24);
    u32 line_length = *(u32 *)(finfo + 48);
    if (smem_len == 0 || line_length == 0) { put("fbwin: bad fix info\n"); sc(SYS_exit_group, 4, 0,0,0,0,0); }
    u32 pitch = line_length / 4;

    i64 m = sc(SYS_mmap, 0, (i64)smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m < 0) { put("fbwin: mmap FAILED\n"); sc(SYS_exit_group, 5, 0,0,0,0,0); }
    volatile u32 *fb = (volatile u32 *)m;

    put("fbwin: live Linux fbdev window -- animating\n");

    /* 60ms frame delay. */
    i64 ts[2]; ts[0] = 0; ts[1] = 60000000L;

    int bx = 0, dir = 4;
    for (;;) {
        /* Vertical gradient: deep blue -> teal. */
        for (u32 y = 0; y < H; y++) {
            u32 g = (y * 200) / (H ? H : 1);
            u32 col = (g << 8) | (60u + (g >> 1));
            for (u32 x = 0; x < W; x++) fb[y * pitch + x] = col;
        }
        /* Three RGB marker squares. */
        u32 mx[3] = { 60, 160, 260 };
        u32 mc[3] = { 0x00FF3030u, 0x0030FF30u, 0x003060FFu };
        for (int s = 0; s < 3; s++) fill(fb, pitch, W, H, (int)mx[s], 40, 56, 56, mc[s]);
        /* Bouncing white block (proves the process is alive). */
        fill(fb, pitch, W, H, bx, (int)H / 2, 48, 48, 0x00F0F0F0u);
        /* White frame. */
        for (u32 x = 0; x < W; x++) { fb[x] = 0x00FFFFFFu; fb[(H - 1) * pitch + x] = 0x00FFFFFFu; }
        for (u32 y = 0; y < H; y++) { fb[y * pitch] = 0x00FFFFFFu; fb[y * pitch + (W - 1)] = 0x00FFFFFFu; }

        sc(SYS_ioctl, fd, FBIOPAN_DISPLAY, (i64)vinfo, 0, 0, 0);

        bx += dir;
        if (bx <= 0 || bx + 48 >= (int)W) dir = -dir;
        sc(SYS_nanosleep, (i64)ts, 0, 0, 0, 0, 0);
    }
}
