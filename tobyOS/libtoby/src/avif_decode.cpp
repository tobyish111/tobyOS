/* avif_decode.cpp -- AVIF (AV1 still image in an ISO-BMFF/HEIF container)
 * decode for tobyOS, bridging the vendored libgav1 to libtoby's image
 * loader.
 *
 * v2 ("finish AVIF properly"):
 *   - item-property associations (ipco indexed + ipma), so each item's
 *     own av1C/colr/auxC are used instead of "first av1C in the file"
 *   - nclx color: BT.601 / BT.709 / BT.2020 matrix, limited/full range,
 *     identity (GBR); falls back to the AV1 bitstream CICP when no colr
 *   - alpha auxiliary item: iref 'auxl' + auxC "...:alpha" -> the aux
 *     image's Y plane becomes A (limited-range alpha expanded to full)
 *   - 10/12-bit high bitdepth: uint16 planes downshifted with rounding
 *     (libgav1 is built with LIBGAV1_MAX_BITDEPTH=10)
 *   - multi-extent iloc items (extents concatenated)
 *
 * Still v2-simple: 'prem' (premultiplied alpha) is ignored, ICC profiles
 * ('rICC'/'prof') are ignored, grid/derived items unsupported. */

#include <toby/avif_decode.h>
#include <stdlib.h>
#include <string.h>

#include "gav1/decoder.h"

/* ---- big-endian readers + box helpers ---- */
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint64_t rdn(const uint8_t *p, int n) {
    uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 8) | p[i]; return v;
}
static int m4(const uint8_t *p, const char *s) {
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] &&
           p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}

/* Find the first child box of `type` within [d, d+n). Returns the box
 * payload pointer (after the 8/16-byte box header) and sets *outlen. */
static const uint8_t *box_find(const uint8_t *d, long n, const char *type,
                               long *outlen) {
    long o = 0;
    while (o + 8 <= n) {
        uint64_t sz = rd32(d + o);
        const uint8_t *t = d + o + 4;
        long hdr = 8;
        if (sz == 1) { if (o + 16 > n) break; sz = rdn(d + o + 8, 8); hdr = 16; }
        if (sz == 0) sz = (uint64_t)(n - o);
        if ((long)sz < hdr || o + (long)sz > n) break;
        if (m4(t, type)) { *outlen = (long)sz - hdr; return d + o + hdr; }
        o += (long)sz;
    }
    return 0;
}

extern "C" int avif_sniff(const uint8_t *d, size_t n) {
    if (n < 12 || !m4(d + 4, "ftyp")) return 0;
    long flen = (long)rd32(d);
    if (flen < 12 || flen > (long)n) flen = (long)n;
    if (m4(d + 8, "avif") || m4(d + 8, "avis")) return 1;   /* major brand */
    for (long o = 16; o + 4 <= flen; o += 4)                 /* compatible brands */
        if (m4(d + o, "avif") || m4(d + o, "avis")) return 1;
    return 0;
}

static inline uint8_t clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

/* ---- item property container (ipco, 1-based index) ----------------- */

#define PROPS_MAX 32
struct prop_box { const uint8_t *p; long n; uint8_t type[4]; };

static int props_parse(const uint8_t *ipco, long n, struct prop_box *props) {
    int count = 0;
    long o = 0;
    while (o + 8 <= n && count < PROPS_MAX) {
        uint64_t sz = rd32(ipco + o);
        long hdr = 8;
        if (sz == 1) { if (o + 16 > n) break; sz = rdn(ipco + o + 8, 8); hdr = 16; }
        if (sz == 0) sz = (uint64_t)(n - o);
        if ((long)sz < hdr || o + (long)sz > n) break;
        props[count].p = ipco + o + hdr;
        props[count].n = (long)sz - hdr;
        memcpy(props[count].type, ipco + o + 4, 4);
        count++;
        o += (long)sz;
    }
    return count;
}

/* Everything we want to know about one item, resolved via ipma. */
struct item_info {
    const uint8_t *av1c; long av1c_len;
    int has_nclx, nclx_mc, nclx_full;
    int is_alpha_aux;
};

static void item_props_resolve(const uint8_t *ipma, long ipma_len,
                               const struct prop_box *props, int nprops,
                               uint32_t item_id, struct item_info *out) {
    memset(out, 0, sizeof(*out));
    if (!ipma || ipma_len < 8) return;
    int ver = ipma[0];
    uint32_t flags = rd32(ipma) & 0xFFFFFF;
    const uint8_t *p = ipma + 4, *end = ipma + ipma_len;
    uint32_t entries = rd32(p); p += 4;
    for (uint32_t e = 0; e < entries && p < end; e++) {
        uint32_t iid;
        if (ver < 1) { if (p + 2 > end) break; iid = rd16(p); p += 2; }
        else         { if (p + 4 > end) break; iid = rd32(p); p += 4; }
        if (p + 1 > end) break;
        int assoc = *p++;
        for (int a = 0; a < assoc && p < end; a++) {
            int idx;
            if (flags & 1) { if (p + 2 > end) break; idx = rd16(p) & 0x7FFF; p += 2; }
            else           { idx = *p & 0x7F; p += 1; }
            if (iid != item_id || idx < 1 || idx > nprops) continue;
            const struct prop_box *pr = &props[idx - 1];
            if (m4(pr->type, "av1C") && pr->n > 4) {
                out->av1c = pr->p + 4;         /* skip the config record hdr */
                out->av1c_len = pr->n - 4;
            } else if (m4(pr->type, "colr") && pr->n >= 4 &&
                       m4(pr->p, "nclx") && pr->n >= 11) {
                /* colour_primaries(2) transfer(2) matrix(2) full<<7 */
                out->has_nclx = 1;
                out->nclx_mc = rd16(pr->p + 8);
                out->nclx_full = (pr->p[10] >> 7) & 1;
            } else if (m4(pr->type, "auxC") && pr->n > 4) {
                /* FullBox(4) + aux_type C-string; alpha per MIAF is
                 * "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha" */
                const char *s = (const char *)pr->p + 4;
                long sl = pr->n - 4;
                for (long k = 0; k + 6 <= sl; k++)
                    if (memcmp(s + k, ":alpha", 6) == 0) { out->is_alpha_aux = 1; break; }
            }
        }
    }
}

/* ---- iref: find the alpha auxiliary item for the primary ----------- */

static uint32_t iref_find_aux(const uint8_t *mc, long mcl, uint32_t primary) {
    long irefl;
    const uint8_t *iref = box_find(mc, mcl, "iref", &irefl);
    if (!iref || irefl < 4) return 0;
    int ver = iref[0];
    const uint8_t *p = iref + 4, *end = iref + irefl;
    while (p + 8 <= end) {
        uint64_t sz = rd32(p);
        if (sz < 8 || p + sz > end) break;
        const uint8_t *t = p + 4, *q = p + 8;
        if (m4(t, "auxl")) {
            uint32_t from; uint16_t refs;
            if (ver == 0) {
                if (q + 4 > end) break;
                from = rd16(q); refs = rd16(q + 2); q += 4;
                for (uint16_t r = 0; r < refs && q + 2 <= end; r++, q += 2)
                    if (rd16(q) == primary) return from;
            } else {
                if (q + 6 > end) break;
                from = rd32(q); refs = rd16(q + 4); q += 6;
                for (uint16_t r = 0; r < refs && q + 4 <= end; r++, q += 4)
                    if (rd32(q) == primary) return from;
            }
        }
        p += sz;
    }
    return 0;
}

/* ---- iloc: collect an item's extents -------------------------------- */

#define EXT_MAX 8
struct item_extents { int n; uint64_t off[EXT_MAX], len[EXT_MAX]; uint64_t total; };

static int iloc_find(const uint8_t *mc, long mcl, uint32_t item_id,
                     struct item_extents *out) {
    memset(out, 0, sizeof(*out));
    long l;
    const uint8_t *iloc = box_find(mc, mcl, "iloc", &l);
    if (!iloc || l < 8) return 0;
    int ver = iloc[0];
    const uint8_t *p = iloc + 4;
    int offset_size = p[0] >> 4, length_size = p[0] & 0xf;
    int base_offset_size = p[1] >> 4;
    int index_size = (ver == 1 || ver == 2) ? (p[1] & 0xf) : 0;
    p += 2;
    uint32_t item_count = (ver < 2) ? rd16(p) : rd32(p);
    p += (ver < 2) ? 2 : 4;
    for (uint32_t i = 0; i < item_count; i++) {
        uint32_t iid = (ver < 2) ? rd16(p) : rd32(p);
        p += (ver < 2) ? 2 : 4;
        if (ver == 1 || ver == 2) p += 2;      /* construction_method */
        p += 2;                                /* data_reference_index */
        uint64_t base = rdn(p, base_offset_size); p += base_offset_size;
        uint16_t ext_count = rd16(p); p += 2;
        for (uint16_t e = 0; e < ext_count; e++) {
            if (index_size) p += index_size;
            uint64_t eoff = rdn(p, offset_size); p += offset_size;
            uint64_t elen = rdn(p, length_size); p += length_size;
            if (iid == item_id && out->n < EXT_MAX && elen > 0) {
                out->off[out->n] = base + eoff;
                out->len[out->n] = elen;
                out->total += elen;
                out->n++;
            }
        }
    }
    return out->n > 0;
}

/* ---- one-item decode helpers ---------------------------------------- */

/* Build the decoder input: av1C config OBUs (if any) + concatenated
 * extents. Returns malloc'd buffer, sets *outlen. */
static uint8_t *item_input(const uint8_t *data, size_t len,
                           const struct item_info *info,
                           const struct item_extents *ext, size_t *outlen) {
    size_t total = (size_t)info->av1c_len + (size_t)ext->total;
    if (total == 0) return 0;
    for (int e = 0; e < ext->n; e++)
        if (ext->off[e] + ext->len[e] > len) return 0;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return 0;
    size_t o = 0;
    if (info->av1c_len) { memcpy(buf, info->av1c, (size_t)info->av1c_len); o = (size_t)info->av1c_len; }
    for (int e = 0; e < ext->n; e++) {
        memcpy(buf + o, data + ext->off[e], (size_t)ext->len[e]);
        o += (size_t)ext->len[e];
    }
    *outlen = total;
    return buf;
}

/* Read one sample from a possibly-high-bitdepth plane row, downshifted
 * to 8 bits with rounding. */
static inline int plane_sample(const uint8_t *row, int x, int bd) {
    if (bd == 8) return row[x];
    int v = ((const uint16_t *)row)[x];
    int sh = bd - 8;
    v = (v + (1 << (sh - 1))) >> sh;
    return v > 255 ? 255 : v;
}

/* ---- YUV -> RGB matrix coefficients (x256) --------------------------- *
 * CICP matrix_coefficients: 0 identity(GBR), 1 BT.709, 5/6 BT.601,
 * 9/10 BT.2020; everything else falls back to BT.601. Two variants per
 * matrix: limited range (Y-16 scaled 255/219, C scaled 255/224) and
 * full range. */
struct yuv_coef { int cy, crv, cgu, cgv, cbu, ysub; };

static void pick_coef(int mc, int full, struct yuv_coef *c) {
    if (full) {
        c->cy = 256; c->ysub = 0;
        switch (mc) {
        case 1:  c->crv = 403; c->cgu =  48; c->cgv = 120; c->cbu = 475; break;
        case 9: case 10:
                 c->crv = 378; c->cgu =  42; c->cgv = 146; c->cbu = 482; break;
        default: c->crv = 359; c->cgu =  88; c->cgv = 183; c->cbu = 454; break;
        }
    } else {
        c->cy = 298; c->ysub = 16;
        switch (mc) {
        case 1:  c->crv = 459; c->cgu =  55; c->cgv = 136; c->cbu = 541; break;
        case 9: case 10:
                 c->crv = 430; c->cgu =  48; c->cgv = 167; c->cbu = 548; break;
        default: c->crv = 409; c->cgu = 100; c->cgv = 208; c->cbu = 516; break;
        }
    }
}

extern "C" uint8_t *avif_decode_rgba(const uint8_t *data, size_t len,
                                     int *out_w, int *out_h) {
    if (!avif_sniff(data, len)) return 0;

    long metal;
    const uint8_t *meta = box_find(data, (long)len, "meta", &metal);
    if (!meta) return 0;
    /* meta is a FullBox: 4 bytes version/flags before the child boxes. */
    const uint8_t *mc = meta + 4; long mcl = metal - 4;

    /* primary item id (pitm). */
    long l;
    const uint8_t *pitm = box_find(mc, mcl, "pitm", &l);
    if (!pitm || l < 6) return 0;
    uint32_t primary_id = (pitm[0] == 0) ? rd16(pitm + 4) : rd32(pitm + 4);

    /* item properties: ipco (indexed) + ipma (associations). */
    struct prop_box props[PROPS_MAX];
    int nprops = 0;
    const uint8_t *ipma = 0; long ipma_len = 0;
    long iprpl;
    const uint8_t *iprp = box_find(mc, mcl, "iprp", &iprpl);
    if (iprp) {
        long ipcol;
        const uint8_t *ipco = box_find(iprp, iprpl, "ipco", &ipcol);
        if (ipco) nprops = props_parse(ipco, ipcol, props);
        ipma = box_find(iprp, iprpl, "ipma", &ipma_len);
    }

    struct item_info pinfo;
    item_props_resolve(ipma, ipma_len, props, nprops, primary_id, &pinfo);
    /* pre-ipma fallback (v1 files/parsers): first av1C in ipco */
    if (!pinfo.av1c) {
        for (int i = 0; i < nprops; i++)
            if (m4(props[i].type, "av1C") && props[i].n > 4) {
                pinfo.av1c = props[i].p + 4; pinfo.av1c_len = props[i].n - 4;
                break;
            }
    }

    struct item_extents pext;
    if (!iloc_find(mc, mcl, primary_id, &pext)) return 0;

    /* ---- alpha auxiliary item (optional) --------------------------- */
    uint8_t *alpha = 0; int aw = 0, ah = 0;
    uint32_t alpha_id = iref_find_aux(mc, mcl, primary_id);
    if (alpha_id) {
        struct item_info ainfo;
        item_props_resolve(ipma, ipma_len, props, nprops, alpha_id, &ainfo);
        struct item_extents aext;
        if (ainfo.is_alpha_aux && iloc_find(mc, mcl, alpha_id, &aext)) {
            size_t ainlen = 0;
            uint8_t *ain = item_input(data, len, &ainfo, &aext, &ainlen);
            if (ain) {
                libgav1::Decoder adec;
                libgav1::DecoderSettings ast;
                ast.threads = 1;
                ast.frame_parallel = false;
                ast.blocking_dequeue = true;
                const libgav1::DecoderBuffer *ab = 0;
                if (adec.Init(&ast) == libgav1::kStatusOk &&
                    adec.EnqueueFrame(ain, ainlen, 0, nullptr) == libgav1::kStatusOk &&
                    adec.DequeueFrame(&ab) == libgav1::kStatusOk && ab &&
                    (ab->bitdepth == 8 || ab->bitdepth == 10 || ab->bitdepth == 12)) {
                    aw = ab->displayed_width[0]; ah = ab->displayed_height[0];
                    if (aw > 0 && ah > 0)
                        alpha = (uint8_t *)malloc((size_t)aw * ah);
                    if (alpha) {
                        int limited = (ab->color_range == kLibgav1ColorRangeStudio);
                        for (int y = 0; y < ah; y++) {
                            const uint8_t *row = ab->plane[0] + (size_t)y * ab->stride[0];
                            uint8_t *o = alpha + (size_t)y * aw;
                            for (int x = 0; x < aw; x++) {
                                int a = plane_sample(row, x, ab->bitdepth);
                                if (limited) a = clip8((298 * (a - 16) + 128) >> 8);
                                o[x] = (uint8_t)a;
                            }
                        }
                    }
                    adec.SignalEOS();
                }
                free(ain);
            }
        }
    }

    /* ---- primary (color) item --------------------------------------- */
    size_t inlen = 0;
    uint8_t *input = item_input(data, len, &pinfo, &pext, &inlen);
    if (!input) { free(alpha); return 0; }

    libgav1::Decoder dec;
    libgav1::DecoderSettings st;
    st.threads = 1;
    st.frame_parallel = false;
    st.blocking_dequeue = true;
    if (dec.Init(&st) != libgav1::kStatusOk) { free(input); free(alpha); return 0; }
    if (dec.EnqueueFrame(input, inlen, 0, nullptr) != libgav1::kStatusOk) {
        free(input); free(alpha); return 0;
    }
    const libgav1::DecoderBuffer *buf = 0;
    if (dec.DequeueFrame(&buf) != libgav1::kStatusOk || !buf) {
        free(input); free(alpha); return 0;
    }
    int bd = buf->bitdepth;
    if (bd != 8 && bd != 10 && bd != 12) { free(input); free(alpha); return 0; }

    int w = buf->displayed_width[0], h = buf->displayed_height[0];
    if (w <= 0 || h <= 0) { free(input); free(alpha); return 0; }

    /* chroma subsampling from the image format. */
    int mono = (buf->image_format == libgav1::kImageFormatMonochrome400);
    int subx = 0, suby = 0;
    if (buf->image_format == libgav1::kImageFormatYuv420) { subx = 1; suby = 1; }
    else if (buf->image_format == libgav1::kImageFormatYuv422) { subx = 1; suby = 0; }

    /* color model: colr/nclx wins, else the AV1 bitstream CICP (both use
     * CICP codes; libgav1's enums mirror them). MC=2 = unspecified. */
    int mcod, full;
    if (pinfo.has_nclx) {
        mcod = pinfo.nclx_mc;
        full = pinfo.nclx_full;
    } else {
        mcod = (int)buf->matrix_coefficients;
        full = (buf->color_range == kLibgav1ColorRangeFull);
    }
    struct yuv_coef cf;
    pick_coef(mcod, full, &cf);
    int identity = (mcod == 0 && !mono);       /* GBR passthrough */

    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!rgba) { free(input); free(alpha); return 0; }

    const uint8_t *Y = buf->plane[0];
    const uint8_t *U = mono ? 0 : buf->plane[1];
    const uint8_t *V = mono ? 0 : buf->plane[2];
    int sy = buf->stride[0], suv = mono ? 0 : buf->stride[1];

    for (int y = 0; y < h; y++) {
        const uint8_t *yr = Y + (size_t)y * sy;
        const uint8_t *ur = mono ? 0 : U + (size_t)(y >> suby) * suv;
        const uint8_t *vr = mono ? 0 : V + (size_t)(y >> suby) * suv;
        uint8_t *o = rgba + (size_t)y * w * 4;
        const uint8_t *ar = alpha ? alpha + (size_t)((long)y * ah / h) * aw : 0;
        for (int x = 0; x < w; x++) {
            int yv = plane_sample(yr, x, bd);
            if (identity) {
                /* MC=0: Y=G, U=B, V=R (never subsampled per AV1 spec) */
                int g = yv;
                int b = plane_sample(ur, x >> subx, bd);
                int r = plane_sample(vr, x >> subx, bd);
                if (!full) {
                    r = clip8((298 * (r - 16) + 128) >> 8);
                    g = clip8((298 * (g - 16) + 128) >> 8);
                    b = clip8((298 * (b - 16) + 128) >> 8);
                }
                o[x * 4 + 0] = (uint8_t)r;
                o[x * 4 + 1] = (uint8_t)g;
                o[x * 4 + 2] = (uint8_t)b;
            } else {
                int c = yv - cf.ysub;
                int d = mono ? 0 : plane_sample(ur, x >> subx, bd) - 128;
                int e = mono ? 0 : plane_sample(vr, x >> subx, bd) - 128;
                o[x * 4 + 0] = clip8((cf.cy * c + cf.crv * e + 128) >> 8);
                o[x * 4 + 1] = clip8((cf.cy * c - cf.cgu * d - cf.cgv * e + 128) >> 8);
                o[x * 4 + 2] = clip8((cf.cy * c + cf.cbu * d + 128) >> 8);
            }
            o[x * 4 + 3] = ar ? ar[(long)x * aw / w] : 255;
        }
    }

    dec.SignalEOS();
    free(input);
    free(alpha);
    *out_w = w; *out_h = h;
    return rgba;
}
