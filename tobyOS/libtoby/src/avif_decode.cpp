/* avif_decode.cpp -- AVIF (AV1 still image in an ISO-BMFF/HEIF container)
 * decode for tobyOS, bridging the vendored libgav1 to libtoby's image
 * loader. Parses the minimal HEIF box structure (meta -> pitm/iloc/iprp),
 * pulls the primary AV1 item's OBU data (prepending the av1C sequence-
 * header OBUs if the box carries any), decodes it with libgav1, and
 * converts the YUV to RGBA8888 (BT.601 limited range). v1: primary item
 * only, no alpha auxiliary item (opaque). See toby/avif_decode.h. */

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
    uint32_t item_id = (pitm[0] == 0) ? rd16(pitm + 4) : rd32(pitm + 4);

    /* item location (iloc) -> the primary item's (offset, length). */
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
    uint64_t data_off = 0, data_len = 0;
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
            if (iid == item_id && data_len == 0) { data_off = base + eoff; data_len = elen; }
        }
    }
    if (data_len == 0 || data_off + data_len > len) return 0;

    /* av1C config OBUs (sequence header), if the box carries any. */
    const uint8_t *cfg_obus = 0; long cfg_len = 0;
    long iprpl;
    const uint8_t *iprp = box_find(mc, mcl, "iprp", &iprpl);
    if (iprp) {
        long ipcol;
        const uint8_t *ipco = box_find(iprp, iprpl, "ipco", &ipcol);
        if (ipco) {
            long av1cl;
            const uint8_t *av1c = box_find(ipco, ipcol, "av1C", &av1cl);
            if (av1c && av1cl > 4) { cfg_obus = av1c + 4; cfg_len = av1cl - 4; }
        }
    }

    /* Decode input = config OBUs (if any) + the item's coded data. */
    size_t inlen = (size_t)cfg_len + (size_t)data_len;
    uint8_t *input = (uint8_t *)malloc(inlen ? inlen : 1);
    if (!input) return 0;
    if (cfg_len) memcpy(input, cfg_obus, (size_t)cfg_len);
    memcpy(input + cfg_len, data + data_off, (size_t)data_len);

    libgav1::Decoder dec;
    libgav1::DecoderSettings st;
    st.threads = 1;
    st.frame_parallel = false;
    st.blocking_dequeue = true;
    if (dec.Init(&st) != libgav1::kStatusOk) { free(input); return 0; }
    if (dec.EnqueueFrame(input, inlen, 0, nullptr) != libgav1::kStatusOk) {
        free(input); return 0;
    }
    const libgav1::DecoderBuffer *buf = 0;
    if (dec.DequeueFrame(&buf) != libgav1::kStatusOk || !buf) { free(input); return 0; }
    if (buf->bitdepth != 8) { free(input); return 0; }  /* v1: 8-bit only */

    int w = buf->displayed_width[0], h = buf->displayed_height[0];
    if (w <= 0 || h <= 0) { free(input); return 0; }

    /* chroma subsampling from the image format. */
    int mono = (buf->image_format == libgav1::kImageFormatMonochrome400);
    int subx = 0, suby = 0;
    if (buf->image_format == libgav1::kImageFormatYuv420) { subx = 1; suby = 1; }
    else if (buf->image_format == libgav1::kImageFormatYuv422) { subx = 1; suby = 0; }

    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!rgba) { free(input); return 0; }

    const uint8_t *Y = buf->plane[0];
    const uint8_t *U = mono ? 0 : buf->plane[1];
    const uint8_t *V = mono ? 0 : buf->plane[2];
    int sy = buf->stride[0], suv = mono ? 0 : buf->stride[1];

    for (int y = 0; y < h; y++) {
        const uint8_t *yr = Y + (size_t)y * sy;
        const uint8_t *ur = mono ? 0 : U + (size_t)(y >> suby) * suv;
        const uint8_t *vr = mono ? 0 : V + (size_t)(y >> suby) * suv;
        uint8_t *o = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            int c = (int)yr[x] - 16;
            int d = mono ? 0 : (int)ur[x >> subx] - 128;
            int e = mono ? 0 : (int)vr[x >> subx] - 128;
            o[x * 4 + 0] = clip8((298 * c + 409 * e + 128) >> 8);
            o[x * 4 + 1] = clip8((298 * c - 100 * d - 208 * e + 128) >> 8);
            o[x * 4 + 2] = clip8((298 * c + 516 * d + 128) >> 8);
            o[x * 4 + 3] = 255;
        }
    }

    dec.SignalEOS();
    free(input);
    *out_w = w; *out_h = h;
    return rgba;
}
