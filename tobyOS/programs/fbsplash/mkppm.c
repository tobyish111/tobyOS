/* programs/fbsplash/mkppm.c -- host tool that emits the binary P6 PPM splash
 * image consumed by the off-the-shelf busybox `fbsplash` applet. We draw a
 * blue->teal vertical gradient plus three solid RGB marker squares at
 * (70,60)/(170,60)/(270,60) so the boot harness can sample those pixels off
 * the scanout and prove the real fbsplash binary actually rendered the image
 * to /dev/fb0. Built + run on the BUILD host (native), not inside tobyOS. */
#include <stdio.h>
#include <stdlib.h>

#define W 480
#define H 320

static unsigned char img[H][W][3];

static void rect(int x0, int y0, int w, int h,
                 unsigned char r, unsigned char g, unsigned char b) {
    for (int y = y0; y < y0 + h && y < H; y++)
        for (int x = x0; x < x0 + w && x < W; x++) {
            img[y][x][0] = r; img[y][x][1] = g; img[y][x][2] = b;
        }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: mkppm OUT.ppm\n"); return 2; }

    for (int y = 0; y < H; y++) {
        unsigned char b = (unsigned char)(40 + (y * 180) / H);   /* blue ramp */
        unsigned char g = (unsigned char)(20 + (y * 140) / H);   /* teal ramp */
        for (int x = 0; x < W; x++) {
            img[y][x][0] = 0; img[y][x][1] = g; img[y][x][2] = b;
        }
    }
    /* Three solid RGB markers the harness samples (centres 70,60/170,60/270,60). */
    rect(50, 40, 40, 40, 255, 0,   0);   /* red   @ (70,60)  */
    rect(150, 40, 40, 40, 0,   255, 0);  /* green @ (170,60) */
    rect(250, 40, 40, 40, 0,   0,   255);/* blue  @ (270,60) */

    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror("fopen"); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(img, 1, sizeof(img), f);
    fclose(f);
    return 0;
}
