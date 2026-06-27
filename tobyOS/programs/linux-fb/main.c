/* programs/linux-fb/main.c
 *
 * A GENUINE Linux x86-64 static ELF (raw syscalls, no libc) that is a real
 * Linux FRAMEBUFFER app -- it would run unmodified on a Linux kernel against a
 * /dev/fb0 fbdev device. It proves tobyOS's new fbdev surface end to end:
 *
 *   1. open("/dev/fb0", O_RDWR)
 *   2. ioctl(FBIOGET_VSCREENINFO)  -> xres, yres, bits_per_pixel
 *   3. ioctl(FBIOGET_FSCREENINFO)  -> line_length, smem_len
 *   4. mmap(NULL, smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)
 *   5. draw a graphical scene into the mapped scanout: a vertical gradient
 *      background plus three solid marker squares in known colours at known
 *      coordinates (so the kernel harness can verify the pixels reached the
 *      framebuffer deterministically)
 *   6. ioctl(FBIOPAN_DISPLAY) to present the frame
 *
 * Exit code:  0 = drew + presented OK;  2 open;  3 vinfo;  4 finfo;  5 mmap.
 *
 * Pixels are XRGB8888 (red at bit 16, green 8, blue 0) -- the bitfields the
 * FBIOGET_VSCREENINFO query reports.
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
#define SYS_close       3
#define SYS_ioctl       16
#define SYS_mmap        9
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

__attribute__((force_align_arg_pointer))
void _start(void) {
    i64 fd = sc(SYS_open, (i64)"/dev/fb0", O_RDWR, 0, 0, 0, 0);
    if (fd < 0) { put("fb: open /dev/fb0 FAILED\n"); sc(SYS_exit_group, 2, 0, 0, 0, 0, 0); }

    /* struct fb_var_screeninfo: xres@0, yres@4, bits_per_pixel@24 (u32s). */
    u32 vinfo[40];
    for (int i = 0; i < 40; i++) vinfo[i] = 0;
    if (sc(SYS_ioctl, fd, FBIOGET_VSCREENINFO, (i64)vinfo, 0, 0, 0) < 0) {
        put("fb: FBIOGET_VSCREENINFO FAILED\n"); sc(SYS_exit_group, 3, 0, 0, 0, 0, 0);
    }
    u32 xres = vinfo[0], yres = vinfo[1], bpp = vinfo[6];
    if (xres == 0 || yres == 0 || bpp != 32) {
        put("fb: bad geometry\n"); sc(SYS_exit_group, 3, 0, 0, 0, 0, 0);
    }

    /* struct fb_fix_screeninfo: smem_len@24, line_length@48 (byte offsets). */
    unsigned char finfo[80];
    for (int i = 0; i < 80; i++) finfo[i] = 0;
    if (sc(SYS_ioctl, fd, FBIOGET_FSCREENINFO, (i64)finfo, 0, 0, 0) < 0) {
        put("fb: FBIOGET_FSCREENINFO FAILED\n"); sc(SYS_exit_group, 4, 0, 0, 0, 0, 0);
    }
    u32 smem_len    = *(u32 *)(finfo + 24);
    u32 line_length = *(u32 *)(finfo + 48);
    if (smem_len == 0 || line_length == 0) {
        put("fb: bad fix info\n"); sc(SYS_exit_group, 4, 0, 0, 0, 0, 0);
    }
    u32 pitch = line_length / 4;   /* stride in pixels */

    i64 m = sc(SYS_mmap, 0, (i64)smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m < 0) { put("fb: mmap FAILED\n"); sc(SYS_exit_group, 5, 0, 0, 0, 0, 0); }
    volatile u32 *fb = (volatile u32 *)m;

    /* Vertical gradient: deep blue -> teal. */
    for (u32 y = 0; y < yres; y++) {
        u32 g = (y * 200) / (yres ? yres : 1);
        u32 col = (0u << 16) | (g << 8) | (60u + (g >> 1));
        for (u32 x = 0; x < xres; x++) fb[y * pitch + x] = col;
    }

    /* Three solid marker squares at known coords/colours (the harness checks
     * the framebuffer pixels at the square centres). */
    u32 mx[3] = { 50, 150, 250 };
    u32 mc[3] = { 0x00FF0000u /*red*/, 0x0000FF00u /*green*/, 0x000000FFu /*blue*/ };
    for (int s = 0; s < 3; s++)
        for (u32 yy = 40; yy < 80; yy++)
            for (u32 xx = mx[s]; xx < mx[s] + 40 && xx < xres; xx++)
                fb[yy * pitch + xx] = mc[s];

    /* A white frame around the screen edge for good measure. */
    for (u32 x = 0; x < xres; x++) { fb[x] = 0x00FFFFFFu; fb[(yres - 1) * pitch + x] = 0x00FFFFFFu; }
    for (u32 y = 0; y < yres; y++) { fb[y * pitch] = 0x00FFFFFFu; fb[y * pitch + (xres - 1)] = 0x00FFFFFFu; }

    /* Present the frame (double-buffered fbdev pan). */
    sc(SYS_ioctl, fd, FBIOPAN_DISPLAY, (i64)vinfo, 0, 0, 0);

    put("fb: drew gradient + RGB markers + presented OK\n");
    sc(SYS_close, fd, 0, 0, 0, 0, 0);
    sc(SYS_exit_group, 0, 0, 0, 0, 0, 0);
    for (;;) { }
}
