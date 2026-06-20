/* kfont.c -- kernel-side TrueType text rendering (Track C / C14b).
 *
 * stb_truetype rasterizer running in the kernel. The Win32 GDI text shims route
 * CreateFontA/TextOutA/DrawTextA/GetTextExtent through this so apps get real
 * antialiased glyphs at arbitrary sizes. The bundled Lato (OFL) TTF ships in the
 * initrd at /etc/Lato-Regular.ttf and is lazy-loaded on first use.
 *
 * FLOATING POINT IN THE KERNEL: stb_truetype is float-heavy and the kernel is
 * normally -mno-sse (it never touches XMM). This file is compiled -msse, and
 * every window that runs the float code is bracketed by kfpu_begin/kfpu_end,
 * which FXSAVE the calling user process's live SSE/x87 state and FXRSTOR it
 * afterwards -- a syscall doesn't save user FP on entry (only a context switch
 * does), so without this guard the rasterizer would corrupt the app's registers.
 * The guarded windows are pure computation (no sched_yield), so they are atomic
 * w.r.t. the user's FP state.
 */

#include <tobyos/kfont.h>
#include <tobyos/vfs.h>
#include <tobyos/cpu.h>      /* fpu_save / fpu_restore (fxsave/fxrstor) */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void *kmalloc(size_t);
void  kfree(void *);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
void *memmove(void *, const void *, size_t);
size_t strlen(const char *);
int   kprintf(const char *, ...);

/* Blend an 8-bit coverage glyph into a window's client backbuffer (gui.c). */
void gui_window_blend_coverage(struct window *w, int x, int y,
                               const uint8_t *cov, int gw, int gh, uint32_t xrgb);

/* ---- stb_truetype freestanding configuration (kernel) ---- */
#define STBTT_ifloor(x)   ((int)(x) - ((x) < 0 && (x) != (int)(x)))
#define STBTT_iceil(x)    ((int)(x) + ((x) > 0 && (x) != (int)(x)))
#define STBTT_malloc(s,u) ((void)(u), kmalloc(s))
#define STBTT_free(p,u)   ((void)(u), kfree(p))
#define STBTT_assert(x)   ((void)(x))
#define STBTT_memcpy      memcpy
#define STBTT_memset      memset
#define STBTT_strlen      strlen
#define STBTT_RASTERIZER_VERSION 1   /* integer rasterizer core */

/* Minimal libm shims (double) -- compiled -msse so these use SSE2. */
static double kf_sqrt(double x) {
    if (x <= 0) return 0;
    double r = x;
    for (int i = 0; i < 24; i++) r = (r + x / r) * 0.5;
    return r;
}
static double kf_fabs(double x)  { return x < 0 ? -x : x; }
static double kf_floor(double x) { return (double)(long long)x - (x < 0 && x != (long long)x); }
static double kf_fmod(double a, double b) { return b == 0 ? 0 : a - b * kf_floor(a / b); }
static double kf_pow(double b, double e) {
    if (e == 0) return 1; if (e == 1) return b; if (e == 2) return b * b;
    double r = 1; int n = (int)e; for (int i = 0; i < n; i++) r *= b; return r;
}
static double kf_acos(double x) {
    if (x >= 1) return 0; if (x <= -1) return 3.14159265358979;
    return 1.5707963 - x * (1.0 + x * x * (-0.1667 + x * x * 0.075));
}
static double kf_cos(double x) {
    x = kf_fmod(x + 3.14159265, 6.28318530) - 3.14159265;
    double x2 = x * x;
    return 1.0 - x2 * (0.5 - x2 * (0.04166667 - x2 * 0.001388889));
}
#define STBTT_sqrt(x)   kf_sqrt(x)
#define STBTT_fabs(x)   kf_fabs(x)
#define STBTT_fmod(x,y) kf_fmod(x,y)
#define STBTT_pow(x,y)  kf_pow(x,y)
#define STBTT_acos(x)   kf_acos(x)
#define STBTT_cos(x)    kf_cos(x)

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb_truetype.h"

/* ---- FPU guard (see file header) ---- */
static uint8_t  g_fpubuf[512] __attribute__((aligned(16)));
static int      g_fpudepth;
static inline void kfpu_begin(void) { if (g_fpudepth++ == 0) fpu_save(g_fpubuf); }
static inline void kfpu_end(void)   { if (--g_fpudepth == 0) fpu_restore(g_fpubuf); }

/* ---- font + glyph cache ---- */
#define KFONT_PATH      "/etc/Lato-Regular.ttf"
#define KFONT_CACHE     256
#define KFONT_PX_MIN    4
#define KFONT_PX_MAX    200

static struct {
    bool            tried, ok;
    unsigned char  *data;
    size_t          size;
    stbtt_fontinfo  info;
} g_font;

struct gcache {
    int            cp, px;
    unsigned char *bmp;          /* coverage, w*h, or NULL (e.g. space) */
    int            w, h, xoff, yoff, advance;
};
static struct gcache g_cache[KFONT_CACHE];
static int           g_cache_n;

static void kfont_try_load(void) {
    if (g_font.tried) return;
    g_font.tried = true;
    void  *buf = 0; size_t sz = 0;
    if (vfs_read_all(KFONT_PATH, &buf, &sz) != 0 || !buf || sz < 12) {
        kprintf("[kfont] no font at %s -- falling back to bitmap font\n", KFONT_PATH);
        return;
    }
    g_font.data = (unsigned char *)buf;
    g_font.size = sz;
    kfpu_begin();
    int ok = stbtt_InitFont(&g_font.info, g_font.data,
                            stbtt_GetFontOffsetForIndex(g_font.data, 0));
    kfpu_end();
    if (!ok) {
        kprintf("[kfont] stbtt_InitFont failed (%lu bytes)\n", (unsigned long)sz);
        kfree(buf); g_font.data = 0;
        return;
    }
    g_font.ok = true;
    kprintf("[kfont] loaded %s (%lu bytes) -- TrueType text enabled\n",
            KFONT_PATH, (unsigned long)sz);
}

bool kfont_available(void) {
    kfont_try_load();
    return g_font.ok;
}

/* Find or rasterize the (cp,px) glyph. FP-guarded on a cache miss. */
static struct gcache *kfont_glyph(int cp, int px) {
    if (px < KFONT_PX_MIN) px = KFONT_PX_MIN;
    if (px > KFONT_PX_MAX) px = KFONT_PX_MAX;
    for (int i = 0; i < g_cache_n; i++)
        if (g_cache[i].cp == cp && g_cache[i].px == px) return &g_cache[i];

    if (g_cache_n >= KFONT_CACHE) {                 /* evict oldest */
        if (g_cache[0].bmp) kfree(g_cache[0].bmp);
        memmove(&g_cache[0], &g_cache[1], (KFONT_CACHE - 1) * sizeof(g_cache[0]));
        g_cache_n = KFONT_CACHE - 1;
    }
    struct gcache *g = &g_cache[g_cache_n++];
    g->cp = cp; g->px = px;

    kfpu_begin();
    float scale = stbtt_ScaleForPixelHeight(&g_font.info, (float)px);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&g_font.info, cp, &adv, &lsb);
    g->advance = (int)(adv * scale + 0.5f);
    g->bmp = stbtt_GetCodepointBitmap(&g_font.info, 0, scale, cp,
                                      &g->w, &g->h, &g->xoff, &g->yoff);
    kfpu_end();
    return g;
}

/* Ascent in pixels at `px` (FP-guarded). */
static int kfont_ascent_px(int px) {
    kfpu_begin();
    float scale = stbtt_ScaleForPixelHeight(&g_font.info, (float)px);
    int a = 0, d = 0, lg = 0;
    stbtt_GetFontVMetrics(&g_font.info, &a, &d, &lg);
    int asc = (int)(a * scale + 0.5f);
    kfpu_end();
    return asc;
}

void kfont_vmetrics(int px, int *ascent, int *descent, int *line_height) {
    if (!kfont_available()) { if (ascent) *ascent = px; if (descent) *descent = 0;
                              if (line_height) *line_height = px; return; }
    kfpu_begin();
    float scale = stbtt_ScaleForPixelHeight(&g_font.info, (float)px);
    int a = 0, d = 0, lg = 0;
    stbtt_GetFontVMetrics(&g_font.info, &a, &d, &lg);
    kfpu_end();
    if (ascent)      *ascent      = (int)(a * scale + 0.5f);
    if (descent)     *descent     = (int)(-d * scale + 0.5f);
    if (line_height) *line_height = (int)((a - d + lg) * scale + 0.5f);
}

int kfont_text_width(const char *s, int len, int px) {
    if (!s || !kfont_available()) return 0;
    int w = 0, n = 0;
    for (const char *p = s; *p && (len < 0 || n < len); p++, n++) {
        struct gcache *g = kfont_glyph((unsigned char)*p, px);
        w += g ? g->advance : px / 2;
    }
    return w;
}

int kfont_draw_window(struct window *w, int x, int y, const char *s, int len,
                      uint32_t xrgb, int px) {
    if (!w || !s || !kfont_available()) return 0;
    int baseline = y + kfont_ascent_px(px);
    int x0 = x, n = 0;
    for (const char *p = s; *p && (len < 0 || n < len); p++, n++) {
        struct gcache *g = kfont_glyph((unsigned char)*p, px);
        if (g && g->bmp && g->w > 0 && g->h > 0)
            gui_window_blend_coverage(w, x + g->xoff, baseline + g->yoff,
                                      g->bmp, g->w, g->h, xrgb);
        x += g ? g->advance : px / 2;
    }
    return x - x0;
}
