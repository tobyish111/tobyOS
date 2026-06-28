/* programs/linux-paint/main.c
 *
 * A DYNAMICALLY-LINKED Linux x86-64 graphical app that is INTERACTIVE: it
 * draws to /dev/fb0 AND reads the mouse from /dev/input/event1 (evdev), i.e.
 * it uses both halves of a real Linux graphical session at once. It is a musl
 * PIE (PT_INTERP=/lib/ld-musl-x86_64.so.1) loaded through the genuine ld-musl
 * loader, so every call below is a real libc call.
 *
 * Behaviour: open /dev/fb0, query geometry, mmap the scanout, paint a dark
 * canvas. Then open /dev/input/event1, query identity, and run an event loop:
 * accumulate EV_REL motion into a cursor (cx,cy) and, on every BTN_LEFT press,
 * stamp a filled square at the cursor -- "paint where you click". A small
 * crosshair tracks the live cursor. After NCLICKS paints it presents the final
 * frame and exits 0. The kernel harness injects mouse motion + clicks at known
 * positions and samples those pixels off the scanout to prove the round trip.
 *
 * No tobyOS headers: the <linux/fb.h> + <linux/input.h> structures and ioctl
 * encodings below are the verbatim kernel UAPI, identical on every Linux.
 */

typedef unsigned long size_t;
typedef long ssize_t;
typedef struct _TOBY_FILE FILE;
extern FILE *const stdout;
extern FILE *const stderr;

int   open(const char *, int, ...);
int   close(int);
int   ioctl(int, unsigned long, ...);
ssize_t read(int, void *, size_t);
void *mmap(void *, size_t, int, int, int, long);
void *memset(void *, int, size_t);
int   printf(const char *, ...);
int   fprintf(FILE *, const char *, ...);
int   fflush(FILE *);

#define O_RDWR        2
#define O_RDONLY      0
#define PROT_READ     1
#define PROT_WRITE    2
#define MAP_SHARED    1
#define MAP_FAILED    ((void *)-1)

/* ---- verbatim <linux/fb.h> UAPI (trimmed to what we touch) ---------- */
struct fb_bitfield { unsigned int offset, length, msb_right; };
struct fb_var_screeninfo {
    unsigned int xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    unsigned int bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    unsigned int nonstd, activate, height, width, accel_flags, pixclock;
    unsigned int left_margin, right_margin, upper_margin, lower_margin;
    unsigned int hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    unsigned int reserved[4];
};
struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start; unsigned int smem_len, type, type_aux, visual;
    unsigned short xpanstep, ypanstep, ywrapstep;
    unsigned int line_length; unsigned long mmio_start;
    unsigned int mmio_len, accel; unsigned short capabilities, reserved[2];
};
#define FBIOGET_VSCREENINFO     0x4600
#define FBIOGET_FSCREENINFO     0x4602
#define FBIOPAN_DISPLAY         0x4606

/* ---- verbatim <linux/input.h> UAPI ---------------------------------- */
struct timeval { long tv_sec, tv_usec; };
struct input_event {
    struct timeval time;
    unsigned short type;
    unsigned short code;
    int            value;
};
#define EV_SYN   0x00
#define EV_KEY   0x01
#define EV_REL   0x02
#define REL_X    0x00
#define REL_Y    0x01
#define BTN_LEFT 0x110

#define _IOC(dir,type,nr,size) \
    (((unsigned long)(dir)<<30)|((unsigned long)(type)<<8)| \
     ((unsigned long)(nr))|((unsigned long)(size)<<16))
#define EVIOCGNAME(len)  _IOC(2u,'E',0x06,(len))

#define NCLICKS 3
#define BRUSH   16          /* half-size of the painted square */

static void fill_rect(unsigned int *fb, unsigned int stride, unsigned int W,
                      unsigned int H, int x0, int y0, int w, int h,
                      unsigned int color) {
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || (unsigned)y >= H) continue;
        unsigned int *row = fb + (unsigned)y * stride;
        for (int x = x0; x < x0 + w; x++) {
            if (x < 0 || (unsigned)x >= W) continue;
            row[x] = color;
        }
    }
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) { fprintf(stderr, "paint: open(/dev/fb0) failed\n"); return 1; }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    memset(&vinfo, 0, sizeof(vinfo));
    memset(&finfo, 0, sizeof(finfo));
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) != 0 ||
        ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) != 0) {
        fprintf(stderr, "paint: FBIOGET_*SCREENINFO failed\n"); return 2;
    }
    unsigned int W = vinfo.xres, H = vinfo.yres;
    unsigned int stride = finfo.line_length / 4u; if (!stride) stride = W;
    size_t maplen = finfo.smem_len ? (size_t)finfo.smem_len
                                   : (size_t)stride * H * 4u;
    unsigned int *fb = (unsigned int *)mmap(0, maplen, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, fbfd, 0);
    if (fb == (unsigned int *)MAP_FAILED) { fprintf(stderr, "paint: mmap failed\n"); return 3; }

    int msfd = open("/dev/input/event1", O_RDONLY);
    if (msfd < 0) { fprintf(stderr, "paint: open(/dev/input/event1) failed\n"); return 4; }
    char name[64] = { 0 };
    ioctl(msfd, EVIOCGNAME(sizeof(name)), name);
    printf("paint: fb %ux%u id='%s'; mouse '%s' (interactive, dynamic musl)\n",
           W, H, finfo.id, name[0] ? name : "?");
    fflush(stdout);

    /* Dark canvas. */
    fill_rect(fb, stride, W, H, 0, 0, (int)W, (int)H, 0x00101828u);
    vinfo.yoffset = 0;
    ioctl(fbfd, FBIOPAN_DISPLAY, &vinfo);

    /* Brush colours cycle so each click is visibly distinct. */
    static const unsigned int INK[NCLICKS] = {
        0x00FF3030u /* red   */, 0x0030FF30u /* green */, 0x003060FFu /* blue */,
    };

    int cx = (int)W / 2, cy = (int)H / 2;   /* cursor starts centred */
    int painted = 0;
    struct input_event evs[32];

    while (painted < NCLICKS) {
        ssize_t r = read(msfd, evs, sizeof(evs));
        if (r <= 0) break;
        int n = (int)(r / (ssize_t)sizeof(struct input_event));
        for (int i = 0; i < n; i++) {
            unsigned short t = evs[i].type, c = evs[i].code;
            int v = evs[i].value;
            if (t == EV_REL) {
                if (c == REL_X) cx += v;
                else if (c == REL_Y) cy += v;
                if (cx < 0) cx = 0; if ((unsigned)cx >= W) cx = (int)W - 1;
                if (cy < 0) cy = 0; if ((unsigned)cy >= H) cy = (int)H - 1;
            } else if (t == EV_KEY && c == BTN_LEFT && v == 1) {
                unsigned int ink = INK[painted % NCLICKS];
                fill_rect(fb, stride, W, H, cx - BRUSH, cy - BRUSH,
                          2 * BRUSH, 2 * BRUSH, ink);
                painted++;
                printf("paint: click %d at (%d,%d) ink=%06x\n",
                       painted, cx, cy, ink & 0xFFFFFFu);
                fflush(stdout);
                vinfo.yoffset = 0;
                ioctl(fbfd, FBIOPAN_DISPLAY, &vinfo);   /* present the stroke */
            }
        }
    }

    printf("paint: done, painted %d strokes from live mouse input\n", painted);
    fflush(stdout);
    close(msfd);
    close(fbfd);
    return (painted == NCLICKS) ? 0 : 5;
}
