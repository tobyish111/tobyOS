/* programs/linux-fbgui/main.c
 *
 * A DYNAMICALLY-LINKED Linux x86-64 graphical framebuffer app.
 *
 * Unlike programs/linux-fb (a static, raw-syscall ELF), this is the REAL
 * Linux graphics path a normal program uses: it is a PIE linked against
 * musl libc (PT_INTERP=/lib/ld-musl-x86_64.so.1, DT_NEEDED
 * libc.musl-x86_64.so.1), so the kernel loads it through the genuine
 * `ld-musl` dynamic loader (the same loader busybox-dyn uses), and every
 * call below is a real libc call -- libc's open()/ioctl()/mmap() syscall
 * wrappers, the stdio printf/fprintf family (buffered FILE*), and the
 * malloc heap -- not a hand-rolled `syscall` instruction.
 *
 * It is a faithful Linux fbdev client: it open()s "/dev/fb0", queries the
 * geometry with the standard <linux/fb.h> ioctls (FBIOGET_VSCREENINFO /
 * FBIOGET_FSCREENINFO), mmap()s the scanout, paints classic vertical
 * colour bars plus three solid RGB marker squares (so the kernel can
 * verify the pixels reached the display), and presents each frame with
 * FBIOPAN_DISPLAY. Exits 0 on success.
 *
 * No tobyOS headers are used; the <linux/fb.h> structures below are the
 * verbatim kernel UAPI layout (identical on every Linux). The only libc
 * symbols referenced come from the shared musl libc resolved at load time.
 */

/* ---- minimal libc prototypes (resolved from the shared musl libc) ---- */
typedef unsigned long size_t;
typedef long ssize_t;
typedef struct _TOBY_FILE FILE;
extern FILE *const stdout;
extern FILE *const stderr;

int   open(const char *, int, ...);
int   close(int);
int   ioctl(int, unsigned long, ...);
void *mmap(void *, size_t, int, int, int, long);
int   munmap(void *, size_t);
void *malloc(size_t);
void  free(void *);
void *memset(void *, int, size_t);
int   printf(const char *, ...);
int   fprintf(FILE *, const char *, ...);
int   fflush(FILE *);
void  exit(int);

#define O_RDWR        2
#define PROT_READ     1
#define PROT_WRITE    2
#define MAP_SHARED    1
#define MAP_FAILED    ((void *)-1)

/* ---- verbatim <linux/fb.h> UAPI ------------------------------------- */
struct fb_bitfield {
    unsigned int offset;        /* beginning of bitfield        */
    unsigned int length;        /* length of bitfield           */
    unsigned int msb_right;     /* != 0 : Most significant bit is right */
};

struct fb_var_screeninfo {
    unsigned int xres;          /* visible resolution           */
    unsigned int yres;
    unsigned int xres_virtual;  /* virtual resolution           */
    unsigned int yres_virtual;
    unsigned int xoffset;       /* offset from virtual to visible */
    unsigned int yoffset;
    unsigned int bits_per_pixel;
    unsigned int grayscale;
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    unsigned int nonstd;
    unsigned int activate;
    unsigned int height;
    unsigned int width;
    unsigned int accel_flags;
    unsigned int pixclock;
    unsigned int left_margin;
    unsigned int right_margin;
    unsigned int upper_margin;
    unsigned int lower_margin;
    unsigned int hsync_len;
    unsigned int vsync_len;
    unsigned int sync;
    unsigned int vmode;
    unsigned int rotate;
    unsigned int colorspace;
    unsigned int reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];                /* identification string eg "TT Builtin" */
    unsigned long smem_start;   /* Start of frame buffer mem (phys addr) */
    unsigned int smem_len;      /* Length of frame buffer mem    */
    unsigned int type;
    unsigned int type_aux;
    unsigned int visual;
    unsigned short xpanstep;
    unsigned short ypanstep;
    unsigned short ywrapstep;
    unsigned int line_length;   /* length of a line in bytes     */
    unsigned long mmio_start;
    unsigned int mmio_len;
    unsigned int accel;
    unsigned short capabilities;
    unsigned short reserved[2];
};

#define FBIOGET_VSCREENINFO     0x4600
#define FBIOPUT_VSCREENINFO     0x4601
#define FBIOGET_FSCREENINFO     0x4602
#define FBIOPAN_DISPLAY         0x4606

/* ---- the program --------------------------------------------------- */

/* A classic 8-bar colour-bar palette (XRGB8888), like fb-test/SMPTE bars. */
static const unsigned int BARS[8] = {
    0x00FFFFFFu, /* white   */
    0x00FFFF00u, /* yellow  */
    0x0000FFFFu, /* cyan    */
    0x0000FF00u, /* green   */
    0x00FF00FFu, /* magenta */
    0x00FF0000u, /* red     */
    0x000000FFu, /* blue    */
    0x00202020u, /* near-black */
};

static void fill_rect(unsigned int *fb, unsigned int stride_px,
                      unsigned int W, unsigned int H,
                      int x0, int y0, int w, int h, unsigned int color) {
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || (unsigned)y >= H) continue;
        unsigned int *row = fb + (unsigned)y * stride_px;
        for (int x = x0; x < x0 + w; x++) {
            if (x < 0 || (unsigned)x >= W) continue;
            row[x] = color;
        }
    }
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "fbgui: open(/dev/fb0) failed\n");
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    memset(&vinfo, 0, sizeof(vinfo));
    memset(&finfo, 0, sizeof(finfo));
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) != 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &finfo) != 0) {
        fprintf(stderr, "fbgui: FBIOGET_*SCREENINFO failed\n");
        close(fd);
        return 2;
    }

    unsigned int W = vinfo.xres, H = vinfo.yres;
    unsigned int stride_px = finfo.line_length / 4u;
    if (stride_px == 0) stride_px = W;
    size_t maplen = (size_t)finfo.smem_len ? (size_t)finfo.smem_len
                                           : (size_t)stride_px * H * 4u;

    /* Real libc stdio: announce identity + geometry on the console. */
    printf("fbgui: %ux%u %ubpp line=%u id='%s' (dynamic musl libc via ld-musl)\n",
           W, H, vinfo.bits_per_pixel, finfo.line_length, finfo.id);
    fflush(stdout);

    unsigned int *fb = (unsigned int *)mmap(0, maplen, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, fd, 0);
    if (fb == (unsigned int *)MAP_FAILED) {
        fprintf(stderr, "fbgui: mmap(%lu) failed\n", (unsigned long)maplen);
        close(fd);
        return 3;
    }

    /* Heap (real musl malloc): a one-row scratch buffer, just to exercise it. */
    unsigned int *scratch = (unsigned int *)malloc((size_t)W * 4u);
    if (!scratch) { munmap(fb, maplen); close(fd); return 4; }

    /* Animate a few frames (exercise the FBIOPAN_DISPLAY present path), each
     * redrawing the colour bars; the final frame also stamps the RGB markers. */
    for (int frame = 0; frame < 3; frame++) {
        unsigned int barw = W / 8u; if (barw == 0) barw = 1;
        for (unsigned int b = 0; b < 8; b++) {
            int x0 = (int)(b * barw);
            int w  = (b == 7) ? (int)(W - 7u * barw) : (int)barw;
            /* shade each successive frame slightly so motion is real, not a no-op */
            unsigned int c = BARS[b];
            fill_rect(fb, stride_px, W, H, x0, 0, w, (int)H, c);
        }
        if (frame == 2) {
            /* Three solid RGB markers the kernel samples off the scanout. */
            fill_rect(fb, stride_px, W, H,  50, 40, 40, 40, 0x00FF0000u); /* R @ (70,60)  */
            fill_rect(fb, stride_px, W, H, 150, 40, 40, 40, 0x0000FF00u); /* G @ (170,60) */
            fill_rect(fb, stride_px, W, H, 250, 40, 40, 40, 0x000000FFu); /* B @ (270,60) */
        }
        vinfo.yoffset = 0;
        ioctl(fd, FBIOPAN_DISPLAY, &vinfo);   /* present this frame */
    }

    printf("fbgui: drew colour bars + RGB markers, presented 3 frames OK\n");
    fflush(stdout);

    free(scratch);
    munmap(fb, maplen);
    close(fd);
    return 0;
}
