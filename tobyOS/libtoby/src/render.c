/* render.c -- Userland 2D rendering API (Milestone 2).
 *
 * Off-screen surface management + compositing helpers.  Presentation
 * goes through the existing GUI blit syscall so pixels end up in the
 * window's compositor back buffer.
 */

#include <toby/render.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- syscall stubs (minimal inline) ------------------------------ */

#define SYS_GUI_BLIT       17
#define SYS_GUI_FILL       11
#define SYS_GUI_FLIP       13

static inline long sys3(long nr, long a, long b, long c)
{
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(nr), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return ret;
}

/* ---- surface lifecycle ------------------------------------------- */

render_surface_t *render_create_surface(int w, int h)
{
    if (w <= 0 || h <= 0)
        return NULL;

    render_surface_t *s = malloc(sizeof(*s));
    if (!s)
        return NULL;

    s->pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!s->pixels) {
        free(s);
        return NULL;
    }

    s->width  = w;
    s->height = h;
    s->clip_x = 0;
    s->clip_y = 0;
    s->clip_w = w;
    s->clip_h = h;
    return s;
}

void render_destroy_surface(render_surface_t *s)
{
    if (!s) return;
    free(s->pixels);
    free(s);
}

/* ---- clipping helpers -------------------------------------------- */

void render_set_clip(render_surface_t *s, int x, int y, int w, int h)
{
    if (!s) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->width)  w = s->width  - x;
    if (y + h > s->height) h = s->height - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    s->clip_x = x;
    s->clip_y = y;
    s->clip_w = w;
    s->clip_h = h;
}

static void clip_rect(const render_surface_t *s,
                       int *x, int *y, int *w, int *h)
{
    int cx1 = s->clip_x;
    int cy1 = s->clip_y;
    int cx2 = cx1 + s->clip_w;
    int cy2 = cy1 + s->clip_h;

    if (*x < cx1) { *w -= (cx1 - *x); *x = cx1; }
    if (*y < cy1) { *h -= (cy1 - *y); *y = cy1; }
    if (*x + *w > cx2) *w = cx2 - *x;
    if (*y + *h > cy2) *h = cy2 - *y;
}

/* ---- drawing ----------------------------------------------------- */

void render_fill_rect(render_surface_t *s,
                      int x, int y, int w, int h, uint32_t color)
{
    if (!s || !s->pixels) return;
    clip_rect(s, &x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;

    for (int row = y; row < y + h; row++) {
        uint32_t *dst = s->pixels + row * s->width + x;
        for (int col = 0; col < w; col++)
            dst[col] = color;
    }
}

void render_blit(render_surface_t *dst, const render_surface_t *src,
                 int dst_x, int dst_y,
                 int src_x, int src_y, int w, int h)
{
    if (!dst || !src || !dst->pixels || !src->pixels)
        return;

    /* Clamp source rect. */
    if (src_x < 0) { w += src_x; dst_x -= src_x; src_x = 0; }
    if (src_y < 0) { h += src_y; dst_y -= src_y; src_y = 0; }
    if (src_x + w > src->width)  w = src->width  - src_x;
    if (src_y + h > src->height) h = src->height - src_y;

    /* Clamp dest rect to clip region. */
    int dx = dst_x, dy = dst_y, dw = w, dh = h;
    clip_rect(dst, &dx, &dy, &dw, &dh);
    int trim_l = dx - dst_x;
    int trim_t = dy - dst_y;
    src_x += trim_l;
    src_y += trim_t;
    w = dw; h = dh;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        const uint32_t *sp = src->pixels + (src_y + row) * src->width + src_x;
        uint32_t       *dp = dst->pixels + (dy + row)    * dst->width + dx;
        memcpy(dp, sp, (size_t)w * sizeof(uint32_t));
    }
}

void render_alpha_blend(render_surface_t *dst, const render_surface_t *src,
                        int dst_x, int dst_y, uint8_t alpha)
{
    if (!dst || !src || !dst->pixels || !src->pixels || alpha == 0)
        return;

    int w = src->width;
    int h = src->height;
    int sx = 0, sy = 0;

    if (dst_x < 0) { sx -= dst_x; w += dst_x; dst_x = 0; }
    if (dst_y < 0) { sy -= dst_y; h += dst_y; dst_y = 0; }
    if (dst_x + w > dst->width)  w = dst->width  - dst_x;
    if (dst_y + h > dst->height) h = dst->height - dst_y;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        const uint32_t *sp = src->pixels + (sy + row) * src->width + sx;
        uint32_t       *dp = dst->pixels + (dst_y + row) * dst->width + dst_x;
        for (int col = 0; col < w; col++) {
            uint32_t s_pix = sp[col];
            uint32_t d_pix = dp[col];
            uint32_t sr = (s_pix >> 16) & 0xFF;
            uint32_t sg = (s_pix >>  8) & 0xFF;
            uint32_t sb =  s_pix        & 0xFF;
            uint32_t dr = (d_pix >> 16) & 0xFF;
            uint32_t dg = (d_pix >>  8) & 0xFF;
            uint32_t db =  d_pix        & 0xFF;
            uint32_t a  = alpha;
            uint32_t ia = 255 - a;
            uint32_t rr = (sr * a + dr * ia + 127) / 255;
            uint32_t rg = (sg * a + dg * ia + 127) / 255;
            uint32_t rb = (sb * a + db * ia + 127) / 255;
            dp[col] = (rr << 16) | (rg << 8) | rb;
        }
    }
}

void render_present(render_surface_t *s, int window_fd)
{
    if (!s || !s->pixels || window_fd < 0)
        return;

    /* Push the surface row-by-row through the GUI blit syscall.
     * This matches SYS_GUI_BLIT which expects (fd, packed-args, pixels).
     * Since the full blit ABI packs x|y|w|h into two registers, we
     * do a full-surface push. */
    (void)sys3(SYS_GUI_BLIT, window_fd,
               (long)(uint32_t)((0 & 0xFFFF) | ((0 & 0xFFFF) << 16)),
               (long)s->pixels);

    /* Flip the window to present. */
    sys3(SYS_GUI_FLIP, window_fd, 0, 0);
}
