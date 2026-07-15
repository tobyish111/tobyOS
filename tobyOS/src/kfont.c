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
#include <tobyos/woff2.h>    /* WOFF2 -> sfnt decode for web fonts */
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

/* ---- font faces + glyph cache (C18c: multiple weights/styles) ----
 *
 * Four faces of the Lato family ship in the initrd; CreateFontA's weight/italic
 * select among them. Each face lazy-loads independently on first use; a missing
 * bold/italic file transparently falls back to Regular (so the system still
 * works if only Regular ships). The glyph cache keys on (face,cp,px). */
#define KFONT_CACHE     384
#define KFONT_PX_MIN    4
#define KFONT_PX_MAX    200

static const char *const g_face_path[KFONT_NFACES] = {
    [KFONT_REGULAR]    = "/etc/Lato-Regular.ttf",
    [KFONT_BOLD]       = "/etc/Lato-Bold.ttf",
    [KFONT_ITALIC]     = "/etc/Lato-Italic.ttf",
    [KFONT_BOLDITALIC] = "/etc/Lato-BoldItalic.ttf",
};
/* Broad-coverage fallback (GNU Unifont, whole BMP). */
static const char *const g_fallback_path = "/etc/fallback.otf";

static struct kface {
    bool            tried, ok;
    unsigned char  *data;
    size_t          size;
    stbtt_fontinfo  info;
} g_faces[KFONT_TOTAL];

struct gcache {
    int            face, cp, px;
    unsigned char *bmp;          /* coverage, w*h, or NULL (e.g. space) */
    int            w, h, xoff, yoff, advance;
};
static struct gcache g_cache[KFONT_CACHE];
static int           g_cache_n;

/* Load one face's TTF (idempotent). Does not fall back -- the caller does. */
static void kface_try_load(int face) {
    struct kface *f = &g_faces[face];
    if (f->tried) return;
    f->tried = true;
    const char *path = (face == KFONT_FALLBACK) ? g_fallback_path
                     : (face < KFONT_NFACES)     ? g_face_path[face]
                     : NULL;
    if (!path) return;                 /* web faces load via kfont_register */
    void  *buf = 0; size_t sz = 0;
    if (vfs_read_all(path, &buf, &sz) != 0 || !buf || sz < 12) {
        if (face == KFONT_REGULAR)
            kprintf("[kfont] no font at %s -- falling back to bitmap font\n", path);
        else if (face == KFONT_FALLBACK)
            kprintf("[kfont] no fallback font at %s -- missing glyphs -> tofu\n", path);
        else
            kprintf("[kfont] no %s -- falling back to Regular\n", path);
        return;
    }
    f->data = (unsigned char *)buf;
    f->size = sz;
    kfpu_begin();
    /* GetFontOffsetForIndex returns -1 for unrecognized data; stb_truetype
     * would then read at data + (unsigned)-1. Gate on a valid in-bounds
     * offset (see kfont_register for why this must never be skipped). */
    int off = stbtt_GetFontOffsetForIndex(f->data, 0);
    int ok = (off >= 0 && (size_t)off < sz) &&
             stbtt_InitFont(&f->info, f->data, off);
    kfpu_end();
    if (!ok) {
        kprintf("[kfont] stbtt_InitFont failed for %s (%lu bytes)\n",
                path, (unsigned long)sz);
        kfree(buf); f->data = 0;
        return;
    }
    f->ok = true;
    kprintf("[kfont] loaded %s (%lu bytes)\n", path, (unsigned long)sz);
}

/* Register a downloaded TTF/OTF/WOFF2 blob as a web face (stage 13E; WOFF2
 * added post-13H). A WOFF2 blob is transparently decoded to an sfnt first
 * (brotli + glyf/loca transform reversal), then handed to stb_truetype. */
int kfont_register(const unsigned char *ttf, size_t len) {
    if (!ttf || len < 12) return -1;

    /* WOFF2 -> sfnt: decode into an owned buffer that becomes f->data. */
    unsigned char *decoded = 0;
    if (woff2_is(ttf, len)) {
        uint8_t *sfnt = 0; size_t sfnt_len = 0;
        if (woff2_to_sfnt(ttf, len, &sfnt, &sfnt_len) != 0 || !sfnt) {
            kprintf("[kfont] WOFF2 decode failed (%lu bytes)\n", (unsigned long)len);
            return -1;
        }
        decoded = sfnt; ttf = sfnt; len = sfnt_len;
    }

    int slot = -1;
    for (int i = KFONT_WEB_BASE; i < KFONT_WEB_BASE + KFONT_WEB_MAX; i++)
        if (!g_faces[i].data) { slot = i; break; }   /* not the fallback slot */
    if (slot < 0) { if (decoded) kfree(decoded); return -1; }
    unsigned char *copy = (unsigned char *)kmalloc(len);
    if (!copy) { if (decoded) kfree(decoded); return -1; }
    memcpy(copy, ttf, len);
    if (decoded) kfree(decoded);
    struct kface *f = &g_faces[slot];
    f->tried = true;
    f->data = copy;
    f->size = len;
    kfpu_begin();
    /* This blob is WEB-SUPPLIED: any site can hand us a truncated or
     * non-font body (an error page, a redirect, a format we don't grok).
     * GetFontOffsetForIndex answers -1 for those, and passing that on as
     * an offset made stb_truetype dereference data + (unsigned)-1 ==
     * data + 4 GiB -> ring-0 page fault. A remote font must never be
     * able to panic the kernel. */
    int off = stbtt_GetFontOffsetForIndex(f->data, 0);
    int ok = (off >= 0 && (size_t)off < len) &&
             stbtt_InitFont(&f->info, f->data, off);
    kfpu_end();
    if (!ok) {
        kprintf("[kfont] web font register: stbtt_InitFont failed (%lu bytes)\n",
                (unsigned long)len);
        kfree(copy); f->data = 0; f->size = 0;
        return -1;
    }
    f->ok = true;
    kprintf("[kfont] registered web face %d (%lu bytes)\n", slot,
            (unsigned long)len);
    return slot;
}

/* Resolve a requested face to a loaded stbtt_fontinfo, falling back to Regular
 * when the requested weight/style isn't shipped. Returns NULL only if even
 * Regular is unavailable. */
static stbtt_fontinfo *kface_info(int face) {
    /* fallback face (Unifont): lazy-load; NULL if it's not present so
     * the caller can draw a tofu box instead. */
    if (face == KFONT_FALLBACK) {
        kface_try_load(KFONT_FALLBACK);
        return g_faces[KFONT_FALLBACK].ok ? &g_faces[KFONT_FALLBACK].info : NULL;
    }
    /* web faces: use if registered, else fall back to Regular */
    if (face >= KFONT_WEB_BASE && face < KFONT_WEB_BASE + KFONT_WEB_MAX) {
        if (g_faces[face].ok) return &g_faces[face].info;
        face = KFONT_REGULAR;
    }
    if (face < 0 || face >= KFONT_NFACES) face = KFONT_REGULAR;
    kface_try_load(face);
    if (g_faces[face].ok) return &g_faces[face].info;
    /* Style-aware fallback: bold-italic -> bold -> regular; italic -> regular. */
    if (face == KFONT_BOLDITALIC) {
        kface_try_load(KFONT_BOLD);
        if (g_faces[KFONT_BOLD].ok) return &g_faces[KFONT_BOLD].info;
    }
    if (face != KFONT_REGULAR) {
        kface_try_load(KFONT_REGULAR);
        if (g_faces[KFONT_REGULAR].ok) return &g_faces[KFONT_REGULAR].info;
    }
    return NULL;
}

bool kfont_available(void) {
    return kface_info(KFONT_REGULAR) != NULL;
}

/* Decode one UTF-8 sequence at *ps (bounded by `end` when non-NULL);
 * returns the codepoint and advances *ps. Invalid bytes decode as '?'
 * one byte at a time so rendering keeps moving. Makes every kfont
 * entry point (draw/width/raster) Unicode-aware: callers keep passing
 * byte buffers + byte lengths. */
static unsigned kf_utf8_next(const char **ps, const char *end) {
    const unsigned char *p = (const unsigned char *)*ps;
    unsigned c = *p, cp;
    int extra;
    if (c < 0x80) { (*ps)++; return c; }
    if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { (*ps)++; return '?'; }
    if (end && (const char *)p + extra >= end) { (*ps)++; return '?'; }
    for (int k = 1; k <= extra; k++) {
        if ((p[k] & 0xC0) != 0x80) { (*ps)++; return '?'; }
        cp = (cp << 6) | (p[k] & 0x3F);
    }
    *ps += extra + 1;
    return cp;
}

/* Find or rasterize the (face,cp,px) glyph. FP-guarded on a cache miss.
 * A codepoint the face doesn't cover renders as a hollow "tofu" box
 * (synthesized coverage bitmap, so the draw/width/raster paths need no
 * special-casing). */
static struct gcache *kfont_glyph(int face, int cp, int px) {
    if (px < KFONT_PX_MIN) px = KFONT_PX_MIN;
    if (px > KFONT_PX_MAX) px = KFONT_PX_MAX;
    stbtt_fontinfo *info = kface_info(face);
    if (!info) return NULL;
    for (int i = 0; i < g_cache_n; i++)
        if (g_cache[i].face == face && g_cache[i].cp == cp && g_cache[i].px == px)
            return &g_cache[i];

    if (g_cache_n >= KFONT_CACHE) {                 /* evict oldest */
        if (g_cache[0].bmp) kfree(g_cache[0].bmp);
        memmove(&g_cache[0], &g_cache[1], (KFONT_CACHE - 1) * sizeof(g_cache[0]));
        g_cache_n = KFONT_CACHE - 1;
    }
    struct gcache *g = &g_cache[g_cache_n++];
    g->face = face; g->cp = cp; g->px = px;

    kfpu_begin();
    float scale = stbtt_ScaleForPixelHeight(info, (float)px);
    int gi = stbtt_FindGlyphIndex(info, cp);
    /* Font fallback: a codepoint the primary face lacks (CJK, symbols,
     * ...) is drawn from the broad-coverage fallback face (Unifont)
     * instead of a tofu box. The fallback's own metrics/scale are used
     * so the glyph is proportioned correctly. */
    if (gi == 0 && cp > 0x20 && face != KFONT_FALLBACK) {
        stbtt_fontinfo *fb = kface_info(KFONT_FALLBACK);
        if (fb) {
            int fgi = stbtt_FindGlyphIndex(fb, cp);
            if (fgi != 0) {
                float fscale = stbtt_ScaleForPixelHeight(fb, (float)px);
                int adv = 0, lsb = 0;
                stbtt_GetGlyphHMetrics(fb, fgi, &adv, &lsb);
                g->advance = (int)(adv * fscale + 0.5f);
                g->bmp = stbtt_GetGlyphBitmap(fb, 0, fscale, fgi,
                                              &g->w, &g->h, &g->xoff, &g->yoff);
                kfpu_end();
                return g;
            }
        }
    }
    if (gi == 0 && cp > 0x20) {                     /* still missing: tofu box */
        int bw = px * 11 / 20, bh = px * 7 / 10;
        if (bw < 4) bw = 4;
        if (bh < 5) bh = 5;
        unsigned char *bm = (unsigned char *)kmalloc((size_t)bw * bh);
        if (bm) {
            int t = px >= 20 ? 2 : 1;
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++)
                    bm[y * bw + x] =
                        (y < t || y >= bh - t || x < t || x >= bw - t) ? 190 : 0;
        }
        g->bmp = bm; g->w = bm ? bw : 0; g->h = bm ? bh : 0;
        g->xoff = 1; g->yoff = -bh;
        g->advance = bw + 3;
        kfpu_end();
        return g;
    }
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
    g->advance = (int)(adv * scale + 0.5f);
    g->bmp = stbtt_GetGlyphBitmap(info, 0, scale, gi,
                                  &g->w, &g->h, &g->xoff, &g->yoff);
    kfpu_end();
    return g;
}

/* Ascent in pixels at `px` for a face (FP-guarded). */
static int kfont_ascent_px(int face, int px) {
    stbtt_fontinfo *info = kface_info(face);
    if (!info) return px;
    kfpu_begin();
    float scale = stbtt_ScaleForPixelHeight(info, (float)px);
    int a = 0, d = 0, lg = 0;
    stbtt_GetFontVMetrics(info, &a, &d, &lg);
    int asc = (int)(a * scale + 0.5f);
    kfpu_end();
    return asc;
}

void kfont_vmetrics_f(int px, int *ascent, int *descent, int *line_height, int face) {
    stbtt_fontinfo *info = kface_info(face);
    if (!info) { if (ascent) *ascent = px; if (descent) *descent = 0;
                 if (line_height) *line_height = px; return; }
    kfpu_begin();
    float scale = stbtt_ScaleForPixelHeight(info, (float)px);
    int a = 0, d = 0, lg = 0;
    stbtt_GetFontVMetrics(info, &a, &d, &lg);
    kfpu_end();
    if (ascent)      *ascent      = (int)(a * scale + 0.5f);
    if (descent)     *descent     = (int)(-d * scale + 0.5f);
    if (line_height) *line_height = (int)((a - d + lg) * scale + 0.5f);
}
void kfont_vmetrics(int px, int *ascent, int *descent, int *line_height) {
    kfont_vmetrics_f(px, ascent, descent, line_height, KFONT_REGULAR);
}

int kfont_text_width_f(const char *s, int len, int px, int face) {
    if (!s || !kface_info(face)) return 0;
    const char *end = len >= 0 ? s + len : 0;
    int w = 0;
    while (*s && (!end || s < end)) {
        unsigned cp = kf_utf8_next(&s, end);
        struct gcache *g = kfont_glyph(face, (int)cp, px);
        w += g ? g->advance : px / 2;
    }
    return w;
}
int kfont_text_width(const char *s, int len, int px) {
    return kfont_text_width_f(s, len, px, KFONT_REGULAR);
}

int kfont_draw_window_f(struct window *w, int x, int y, const char *s, int len,
                        uint32_t xrgb, int px, int face) {
    if (!w || !s || !kface_info(face)) return 0;
    int baseline = y + kfont_ascent_px(face, px);
    int x0 = x;
    const char *end = len >= 0 ? s + len : 0;
    while (*s && (!end || s < end)) {
        unsigned cp = kf_utf8_next(&s, end);
        struct gcache *g = kfont_glyph(face, (int)cp, px);
        if (g && g->bmp && g->w > 0 && g->h > 0)
            gui_window_blend_coverage(w, x + g->xoff, baseline + g->yoff,
                                      g->bmp, g->w, g->h, xrgb);
        x += g ? g->advance : px / 2;
    }
    return x - x0;
}
int kfont_draw_window(struct window *w, int x, int y, const char *s, int len,
                      uint32_t xrgb, int px) {
    return kfont_draw_window_f(w, x, y, s, len, xrgb, px, KFONT_REGULAR);
}

int kfont_raster_cov(uint8_t *cov, int w, int h, int x, int y,
                     const char *s, int len, int px, int face) {
    if (!cov || w <= 0 || h <= 0 || !s || !kface_info(face)) return 0;
    int baseline = y + kfont_ascent_px(face, px);
    int x0 = x;
    const char *end = len >= 0 ? s + len : 0;
    while (*s && (!end || s < end)) {
        unsigned cp = kf_utf8_next(&s, end);
        struct gcache *g = kfont_glyph(face, (int)cp, px);
        if (g && g->bmp && g->w > 0 && g->h > 0) {
            int gx = x + g->xoff, gy = baseline + g->yoff;
            for (int r = 0; r < g->h; r++) {
                int dy = gy + r;
                if (dy < 0 || dy >= h) continue;
                const uint8_t *src = g->bmp + (long)r * g->w;
                uint8_t *dst = cov + (long)dy * w;
                for (int c = 0; c < g->w; c++) {
                    int dx = gx + c;
                    if (dx < 0 || dx >= w) continue;
                    if (src[c] > dst[dx]) dst[dx] = src[c];
                }
            }
        }
        x += g ? g->advance : px / 2;
    }
    return x - x0;
}
