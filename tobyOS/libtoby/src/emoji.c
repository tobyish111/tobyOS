/* emoji.c -- color emoji via COLR v0 + CPAL. See toby/emoji.h.
 *
 * stb_truetype (implementation in font.c) rasterizes the layer outlines;
 * the COLR/CPAL tables are parsed here by walking the sfnt directory. */

#include <toby/emoji.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Own, self-contained stb_truetype implementation (font.c's is STBTT_STATIC
 * so its symbols aren't linkable). Same freestanding config: heap malloc,
 * integer rasterizer, no SSE-dependent intrinsics. */
#define STBTT_ifloor(x)    ((int)(x) - ((x) < 0 && (x) != (int)(x)))
#define STBTT_iceil(x)     ((int)(x) + ((x) > 0 && (x) != (int)(x)))
#define STBTT_malloc(x,u)  ((void)(u), malloc(x))
#define STBTT_free(x,u)    ((void)(u), free(x))
#define STBTT_assert(x)    ((void)(x))
#define STBTT_RASTERIZER_VERSION 1
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb_truetype.h"

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Find an sfnt table by 4-byte tag. */
static const uint8_t *find_table(const uint8_t *d, uint32_t len,
                                 const char *tag, uint32_t *out_len) {
    if (len < 12) return NULL;
    uint16_t n = be16(d + 4);
    if (12 + (uint32_t)n * 16 > len) return NULL;
    for (uint16_t i = 0; i < n; i++) {
        const uint8_t *rec = d + 12 + i * 16;
        if (memcmp(rec, tag, 4) == 0) {
            uint32_t off = be32(rec + 8), l = be32(rec + 12);
            if ((uint64_t)off + l > len) return NULL;
            if (out_len) *out_len = l;
            return d + off;
        }
    }
    return NULL;
}

struct toby_emoji {
    stbtt_fontinfo info;
    const uint8_t *colr;         /* COLR table */
    uint16_t       nbase;        /* base glyph records */
    const uint8_t *base_recs;    /* [nbase] x (gid,firstLayer,numLayers) u16 */
    const uint8_t *layer_recs;   /* (gid,paletteIndex) u16 */
    const uint8_t *pal0;         /* CPAL palette-0 color records (BGRA) */
    uint16_t       npal_entries;
    /* GSUB ligature substitution subtables (LigatureSubstFormat1), used to
     * fold a ZWJ emoji sequence into one ligature glyph. */
    const uint8_t *lig_sub[128];
    int            n_lig;
};

/* Coverage table -> index of `gid`, or -1. Formats 1 (glyph list) and 2
 * (range records). */
static int coverage_index(const uint8_t *cov, int gid) {
    uint16_t fmt = be16(cov);
    if (fmt == 1) {
        uint16_t n = be16(cov + 2);
        for (uint16_t i = 0; i < n; i++)
            if (be16(cov + 4 + i * 2) == gid) return i;
    } else if (fmt == 2) {
        uint16_t n = be16(cov + 2);
        for (uint16_t i = 0; i < n; i++) {
            const uint8_t *r = cov + 4 + i * 6;
            int start = be16(r), end = be16(r + 2), sci = be16(r + 4);
            if (gid >= start && gid <= end) return sci + (gid - start);
        }
    }
    return -1;
}

toby_emoji_t *toby_emoji_load(const uint8_t *ttf, size_t len) {
    if (!ttf || len < 12) return NULL;
    uint32_t colr_len = 0, cpal_len = 0;
    const uint8_t *colr = find_table(ttf, (uint32_t)len, "COLR", &colr_len);
    const uint8_t *cpal = find_table(ttf, (uint32_t)len, "CPAL", &cpal_len);
    if (!colr || !cpal || colr_len < 14 || cpal_len < 12) return NULL;
    if (be16(colr) != 0) return NULL;              /* COLR v0 only */

    toby_emoji_t *e = (toby_emoji_t *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    if (!stbtt_InitFont(&e->info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
        free(e); return NULL;
    }
    /* COLR v0 header: version, numBaseGlyphRecords, baseGlyphRecordsOffset,
     * layerRecordsOffset, numLayerRecords. */
    e->nbase      = be16(colr + 2);
    e->base_recs  = colr + be32(colr + 4);
    e->layer_recs = colr + be32(colr + 8);
    e->colr = colr;

    /* CPAL: version, numPaletteEntries, numPalettes, numColorRecords,
     * colorRecordsArrayOffset, colorRecordIndices[numPalettes]. Palette 0
     * starts at colorRecordsArrayOffset + colorRecordIndices[0]. */
    e->npal_entries = be16(cpal + 2);
    uint32_t cr_off = be32(cpal + 8);
    uint16_t idx0   = be16(cpal + 12);
    e->pal0 = cpal + cr_off + (uint32_t)idx0 * 4;

    /* GSUB: collect ligature-substitution (lookup type 4) subtables so a
     * ZWJ emoji sequence can be folded into one glyph. */
    uint32_t gsub_len = 0;
    const uint8_t *gsub = find_table(ttf, (uint32_t)len, "GSUB", &gsub_len);
    if (gsub && gsub_len >= 10) {
        const uint8_t *ll = gsub + be16(gsub + 8);      /* LookupList */
        uint16_t nlook = be16(ll);
        for (uint16_t i = 0; i < nlook && e->n_lig < 128; i++) {
            const uint8_t *lk = ll + be16(ll + 2 + i * 2);
            if (be16(lk) != 4) continue;                /* ligature subst */
            uint16_t nsub = be16(lk + 4);
            for (uint16_t s = 0; s < nsub && e->n_lig < 128; s++) {
                const uint8_t *st = lk + be16(lk + 6 + s * 2);
                if (be16(st) == 1) e->lig_sub[e->n_lig++] = st;  /* format 1 */
            }
        }
    }
    return e;
}

/* Binary search the (sorted) base glyph records for `gid`. */
static int find_base(toby_emoji_t *e, int gid,
                     uint16_t *first, uint16_t *num) {
    int lo = 0, hi = (int)e->nbase - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *r = e->base_recs + mid * 6;
        int g = be16(r);
        if (g == gid) { *first = be16(r + 2); *num = be16(r + 4); return 1; }
        else if (g < gid) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

int toby_emoji_has(toby_emoji_t *e, int cp) {
    if (!e) return 0;
    int gid = stbtt_FindGlyphIndex(&e->info, cp);
    if (!gid) return 0;
    uint16_t first, num;
    return find_base(e, gid, &first, &num);
}

/* Fold a glyph sequence into one ligature glyph via GSUB (matches ZWJ
 * emoji sequences). Returns the ligature glyph, or 0 if none applies. The
 * match starts at gids[0] and must consume all n glyphs. */
static int resolve_ligature(toby_emoji_t *e, const int *gids, int n) {
    if (n < 2) return 0;
    for (int s = 0; s < e->n_lig; s++) {
        const uint8_t *st = e->lig_sub[s];              /* LigatureSubstFormat1 */
        const uint8_t *cov = st + be16(st + 2);
        int ci = coverage_index(cov, gids[0]);
        if (ci < 0) continue;
        uint16_t nset = be16(st + 4);
        if (ci >= nset) continue;
        const uint8_t *ligset = st + be16(st + 6 + ci * 2);
        uint16_t nlig = be16(ligset);
        for (uint16_t l = 0; l < nlig; l++) {
            const uint8_t *lig = ligset + be16(ligset + 2 + l * 2);
            int lig_glyph = be16(lig);
            int comp_count = be16(lig + 2);             /* incl. first (cov) */
            if (comp_count != n) continue;              /* must consume all */
            int ok = 1;
            for (int c = 1; c < comp_count; c++)
                if (be16(lig + 2 + c * 2) != gids[c]) { ok = 0; break; }
            if (ok) return lig_glyph;
        }
    }
    return 0;
}

/* Composite a COLR base glyph's layers into a fresh px*px ARGB buffer. */
static uint32_t *render_gid(toby_emoji_t *e, int gid, int px,
                            int *ow, int *oh, int *oadv) {
    uint16_t first, num;
    if (!find_base(e, gid, &first, &num) || num == 0) return NULL;

    float scale = stbtt_ScaleForPixelHeight(&e->info, (float)px);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&e->info, &ascent, &descent, &lineGap);
    int baseline = (int)(ascent * scale + 0.5f);
    int W = px, H = px;

    uint32_t *out = (uint32_t *)calloc((size_t)W * H, 4);
    if (!out) return NULL;

    for (uint16_t L = 0; L < num; L++) {
        const uint8_t *lr = e->layer_recs + (uint32_t)(first + L) * 4;
        int lgid = be16(lr);
        int palIdx = be16(lr + 2);
        uint8_t r, g, b, a;
        if (palIdx == 0xFFFF) { r = g = b = 0; a = 255; }   /* "text color" */
        else {
            const uint8_t *c = e->pal0 + (uint32_t)palIdx * 4;
            b = c[0]; g = c[1]; r = c[2]; a = c[3];          /* CPAL is BGRA */
        }
        int lx0, ly0, lx1, ly1;
        stbtt_GetGlyphBitmapBox(&e->info, lgid, scale, scale,
                                &lx0, &ly0, &lx1, &ly1);
        int gw = lx1 - lx0, gh = ly1 - ly0;
        if (gw <= 0 || gh <= 0 || gw > 1024 || gh > 1024) continue;
        unsigned char *cov = (unsigned char *)malloc((size_t)gw * gh);
        if (!cov) continue;
        stbtt_MakeGlyphBitmap(&e->info, cov, gw, gh, gw, scale, scale, lgid);
        int ox = lx0, oy = baseline + ly0;
        for (int yy = 0; yy < gh; yy++) {
            int dy = oy + yy;
            if (dy < 0 || dy >= H) continue;
            for (int xx = 0; xx < gw; xx++) {
                int dx = ox + xx;
                if (dx < 0 || dx >= W) continue;
                int cv = cov[yy * gw + xx];
                if (!cv) continue;
                int la = a * cv / 255;                       /* layer alpha */
                if (la <= 0) continue;
                uint32_t *po = &out[dy * W + dx];
                int da = (*po >> 24) & 255, dr = (*po >> 16) & 255,
                    dg = (*po >> 8) & 255, db = *po & 255;
                int na = la + da * (255 - la) / 255;
                if (na <= 0) { *po = 0; continue; }
                int nr = (r * la + dr * da * (255 - la) / 255) / na;
                int ng = (g * la + dg * da * (255 - la) / 255) / na;
                int nb = (b * la + db * da * (255 - la) / 255) / na;
                *po = ((uint32_t)na << 24) | ((uint32_t)nr << 16) |
                      ((uint32_t)ng << 8) | (uint32_t)nb;
            }
        }
        free(cov);
    }

    int adv, lsb;
    stbtt_GetGlyphHMetrics(&e->info, gid, &adv, &lsb);
    int advpx = (int)(adv * scale + 0.5f);
    if (advpx < px) advpx = px;                 /* emoji are ~square+ */
    if (ow) *ow = W;
    if (oh) *oh = H;
    if (oadv) *oadv = advpx;
    return out;
}

uint32_t *toby_emoji_render(toby_emoji_t *e, int cp, int px,
                            int *ow, int *oh, int *oadv) {
    if (!e || px <= 0 || px > 512) return NULL;
    int gid = stbtt_FindGlyphIndex(&e->info, cp);
    if (!gid) return NULL;
    return render_gid(e, gid, px, ow, oh, oadv);
}

/* Map a codepoint sequence -> glyph ids, fold via GSUB ligatures, render
 * the resulting color glyph. For a plain single codepoint this is the
 * same as toby_emoji_render. Returns NULL if the (folded) glyph has no
 * color layers. */
uint32_t *toby_emoji_render_seq(toby_emoji_t *e, const int *cps, int n, int px,
                                int *ow, int *oh, int *oadv) {
    if (!e || n <= 0 || px <= 0 || px > 512) return NULL;
    int gids[32];
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        gids[i] = stbtt_FindGlyphIndex(&e->info, cps[i]);
        if (!gids[i]) return NULL;
    }
    int gid = (n == 1) ? gids[0] : resolve_ligature(e, gids, n);
    if (!gid) return NULL;
    return render_gid(e, gid, px, ow, oh, oadv);
}

/* Longest ZWJ emoji sequence starting at cps[0] the font has a ligature
 * for (>=1). cps holds codepoints; returns the number consumed. */
int toby_emoji_seq_len(toby_emoji_t *e, const int *cps, int n) {
    if (!e || n <= 0) return 0;
    int gids[32];
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        gids[i] = stbtt_FindGlyphIndex(&e->info, cps[i]);
        if (!gids[i]) { n = i; break; }
    }
    /* try the longest prefix that resolves to a ligature */
    for (int k = n; k >= 2; k--)
        if (resolve_ligature(e, gids, k)) return k;
    return 0;                                   /* no multi-cp ligature */
}

void toby_emoji_free(toby_emoji_t *e) { if (e) free(e); }
