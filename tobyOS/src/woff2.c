/* woff2.c -- WOFF2 -> sfnt decoder (W3C WOFF2). Reverses the brotli
 * compression + the glyf/loca (and hmtx) table transforms so the kernel
 * TTF rasterizer (stb_truetype) can consume a web font delivered as
 * WOFF2. The transform algorithms follow the reference decoder
 * (google/woff2, MIT); ported to freestanding C over kmalloc buffers.
 * Checksums are left as-is (stb_truetype does not validate them). */

#include <tobyos/types.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/brotli.h>
#include <tobyos/woff2.h>

#define WOFF2_SIG      0x774F4632u        /* 'wOF2' */
#define TAG_GLYF       0x676C7966u
#define TAG_LOCA       0x6C6F6361u
#define TAG_HMTX       0x686D7478u
#define TAG_HEAD       0x68656164u
#define TAG_HHEA       0x68686561u
#define TAG_MAXP       0x6D617870u

#define XFORM          0x100u             /* table has a transform */

/* glyf simple-glyph flag bits */
#define G_ONCURVE  0x01
#define G_XSHORT   0x02
#define G_YSHORT   0x04
#define G_REPEAT   0x08
#define G_XSAME    0x10
#define G_YSAME    0x20
#define G_OVERLAP  0x40
/* composite-glyph flag bits */
#define C_ARGWORDS   0x0001
#define C_HAVE_SCALE 0x0008
#define C_MORE       0x0020
#define C_XY_SCALE   0x0040
#define C_2X2        0x0080
#define C_INSTR      0x0100

#define GLYF_HDR   10                     /* numContours + 4x bbox */

static const uint32_t kKnownTags[63] = {
  0x636D6170,0x68656164,0x68686561,0x686D7478,0x6D617870,0x6E616D65,
  0x4F532F32,0x706F7374,0x63767420,0x6670676D,0x676C7966,0x6C6F6361,
  0x70726570,0x43464620,0x564F5247,0x45424454,0x45424C43,0x67617370,
  0x68646D78,0x6B65726E,0x4C545348,0x50434C54,0x56444D58,0x76686561,
  0x766D7478,0x42415345,0x47444546,0x47504F53,0x47535542,0x45425343,
  0x4A535446,0x4D415448,0x43424454,0x43424C43,0x434F4C52,0x4350414C,
  0x53564720,0x73626978,0x61636E74,0x61766172,0x62646174,0x626C6F63,
  0x62736C6E,0x63766172,0x66647363,0x66656174,0x666D7478,0x66766172,
  0x67766172,0x68737479,0x6A757374,0x6C636172,0x6D6F7274,0x6D6F7278,
  0x6F706264,0x70726F70,0x7472616B,0x5A617066,0x53696C66,0x476C6174,
  0x476C6F63,0x46656174,0x53696C6C
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* ---- byte-range cursor -------------------------------------------- */
struct cur { const uint8_t *p; size_t n, off; };
static int c_u8 (struct cur *c, uint8_t  *v) { if (c->off + 1 > c->n) return 0; *v = c->p[c->off++]; return 1; }
static int c_u16(struct cur *c, uint16_t *v) { if (c->off + 2 > c->n) return 0; *v = rd16(c->p + c->off); c->off += 2; return 1; }
static int c_u32(struct cur *c, uint32_t *v) { if (c->off + 4 > c->n) return 0; *v = rd32(c->p + c->off); c->off += 4; return 1; }
static int c_skip(struct cur *c, size_t k)   { if (c->off + k > c->n) return 0; c->off += k; return 1; }
static int c_read(struct cur *c, uint8_t *d, size_t k) { if (c->off + k > c->n) return 0; memcpy(d, c->p + c->off, k); c->off += k; return 1; }

static int rd255(struct cur *c, unsigned *v) {
    uint8_t code;
    if (!c_u8(c, &code)) return 0;
    if (code == 253) { uint16_t r; if (!c_u16(c, &r)) return 0; *v = r; return 1; }
    if (code == 255) { uint8_t r; if (!c_u8(c, &r)) return 0; *v = (unsigned)r + 253; return 1; }
    if (code == 254) { uint8_t r; if (!c_u8(c, &r)) return 0; *v = (unsigned)r + 506; return 1; }
    *v = code; return 1;
}
static int rdbase128(struct cur *c, uint32_t *v) {
    uint32_t r = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t code;
        if (!c_u8(c, &code)) return 0;
        if (i == 0 && code == 0x80) return 0;
        if (r & 0xfe000000u) return 0;
        r = (r << 7) | (code & 0x7f);
        if (!(code & 0x80)) { *v = r; return 1; }
    }
    return 0;
}

int woff2_is(const uint8_t *buf, size_t len) {
    return buf && len >= 4 && rd32(buf) == WOFF2_SIG;
}

/* ---- glyf transform ------------------------------------------------ */
struct pt { int x, y, on; };

static int with_sign(int flag, int base) { return (flag & 1) ? base : -base; }

/* Decode WOFF2 point triplets into pts[]; returns bytes consumed or -1. */
static long triplet_decode(const uint8_t *flags, const uint8_t *in, size_t in_size,
                           unsigned n_points, struct pt *pts) {
    int x = 0, y = 0;
    size_t ti = 0;
    for (unsigned i = 0; i < n_points; i++) {
        uint8_t flag = flags[i];
        int on = !(flag >> 7);
        flag &= 0x7f;
        unsigned nb = (flag < 84) ? 1 : (flag < 120) ? 2 : (flag < 124) ? 3 : 4;
        if (ti + nb > in_size) return -1;
        int dx, dy;
        if (flag < 10) {
            dx = 0;
            dy = with_sign(flag, ((flag & 14) << 7) + in[ti]);
        } else if (flag < 20) {
            dx = with_sign(flag, (((flag - 10) & 14) << 7) + in[ti]);
            dy = 0;
        } else if (flag < 84) {
            int b0 = flag - 20, b1 = in[ti];
            dx = with_sign(flag, 1 + (b0 & 0x30) + (b1 >> 4));
            dy = with_sign(flag >> 1, 1 + ((b0 & 0x0c) << 2) + (b1 & 0x0f));
        } else if (flag < 120) {
            int b0 = flag - 84;
            dx = with_sign(flag, 1 + ((b0 / 12) << 8) + in[ti]);
            dy = with_sign(flag >> 1, 1 + (((b0 % 12) >> 2) << 8) + in[ti + 1]);
        } else if (flag < 124) {
            int b2 = in[ti + 1];
            dx = with_sign(flag, (in[ti] << 4) + (b2 >> 4));
            dy = with_sign(flag >> 1, ((b2 & 0x0f) << 8) + in[ti + 2]);
        } else {
            dx = with_sign(flag, (in[ti] << 8) + in[ti + 1]);
            dy = with_sign(flag >> 1, (in[ti + 2] << 8) + in[ti + 3]);
        }
        ti += nb;
        x += dx; y += dy;
        pts[i].x = x; pts[i].y = y; pts[i].on = on;
    }
    return (long)ti;
}

/* Store simple-glyph flags + x/y coordinate arrays into dst; sets *gsz. */
static int store_points(unsigned n_points, const struct pt *pts, unsigned n_contours,
                        unsigned instr_len, int overlap, uint8_t *dst, size_t dst_size,
                        size_t *gsz) {
    unsigned flag_off = GLYF_HDR + 2 * n_contours + 2 + instr_len;
    int last_flag = -1, repeat = 0, last_x = 0, last_y = 0;
    unsigned x_bytes = 0, y_bytes = 0;
    for (unsigned i = 0; i < n_points; i++) {
        int flag = pts[i].on ? G_ONCURVE : 0;
        if (overlap && i == 0) flag |= G_OVERLAP;
        int dx = pts[i].x - last_x, dy = pts[i].y - last_y;
        if (dx == 0) flag |= G_XSAME;
        else if (dx > -256 && dx < 256) { flag |= G_XSHORT | (dx > 0 ? G_XSAME : 0); x_bytes += 1; }
        else x_bytes += 2;
        if (dy == 0) flag |= G_YSAME;
        else if (dy > -256 && dy < 256) { flag |= G_YSHORT | (dy > 0 ? G_YSAME : 0); y_bytes += 1; }
        else y_bytes += 2;
        if (flag == last_flag && repeat != 255) {
            dst[flag_off - 1] |= G_REPEAT;
            repeat++;
        } else {
            if (repeat != 0) { if (flag_off >= dst_size) return 0; dst[flag_off++] = (uint8_t)repeat; }
            if (flag_off >= dst_size) return 0;
            dst[flag_off++] = (uint8_t)flag;
            repeat = 0;
        }
        last_x = pts[i].x; last_y = pts[i].y; last_flag = flag;
    }
    if (repeat != 0) { if (flag_off >= dst_size) return 0; dst[flag_off++] = (uint8_t)repeat; }
    unsigned xy = x_bytes + y_bytes;
    if (flag_off + xy > dst_size) return 0;
    unsigned xo = flag_off, yo = flag_off + x_bytes;
    last_x = last_y = 0;
    for (unsigned i = 0; i < n_points; i++) {
        int dx = pts[i].x - last_x;
        if (dx == 0) { }
        else if (dx > -256 && dx < 256) dst[xo++] = (uint8_t)(dx < 0 ? -dx : dx);
        else { wr16(dst + xo, (uint32_t)(uint16_t)dx); xo += 2; }
        last_x += dx;
        int dy = pts[i].y - last_y;
        if (dy == 0) { }
        else if (dy > -256 && dy < 256) dst[yo++] = (uint8_t)(dy < 0 ? -dy : dy);
        else { wr16(dst + yo, (uint32_t)(uint16_t)dy); yo += 2; }
        last_y += dy;
    }
    *gsz = yo;
    return 1;
}

static void compute_bbox(unsigned n, const struct pt *pts, uint8_t *dst) {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (n > 0) { x0 = x1 = pts[0].x; y0 = y1 = pts[0].y; }
    for (unsigned i = 1; i < n; i++) {
        if (pts[i].x < x0) x0 = pts[i].x; if (pts[i].x > x1) x1 = pts[i].x;
        if (pts[i].y < y0) y0 = pts[i].y; if (pts[i].y > y1) y1 = pts[i].y;
    }
    wr16(dst + 2, (uint32_t)(uint16_t)x0); wr16(dst + 4, (uint32_t)(uint16_t)y0);
    wr16(dst + 6, (uint32_t)(uint16_t)x1); wr16(dst + 8, (uint32_t)(uint16_t)y1);
}

/* Measure a composite glyph in the composite stream (does not consume it). */
static int composite_size(struct cur *cs, size_t *size, int *have_instr) {
    size_t start = cs->off;
    int instr = 0;
    uint16_t flags = C_MORE;
    while (flags & C_MORE) {
        if (!c_u16(cs, &flags)) return 0;
        instr |= (flags & C_INSTR) != 0;
        size_t arg = 2 + ((flags & C_ARGWORDS) ? 4 : 2);
        if (flags & C_HAVE_SCALE) arg += 2;
        else if (flags & C_XY_SCALE) arg += 4;
        else if (flags & C_2X2) arg += 8;
        if (!c_skip(cs, arg)) return 0;
    }
    *size = cs->off - start;
    *have_instr = instr;
    cs->off = start;                    /* rewind: caller re-reads */
    return 1;
}

/* ---- output sfnt table set ---------------------------------------- */
struct otab { uint32_t tag; uint8_t *data; uint32_t len; int owned; };

static uint32_t round4(uint32_t x) { return (x + 3) & ~3u; }

/* Reconstruct glyf + loca from the transformed glyf slice. Allocates the
 * glyf and loca table buffers into out_glyf/out_loca. Returns 0 or -1. */
static int reconstruct_glyf(const uint8_t *data, size_t data_len,
                            struct otab *glyf, struct otab *loca,
                            int16_t **x_mins_out, unsigned *num_glyphs_out) {
    struct cur f = { data, data_len, 0 };
    uint16_t version, flags, num_glyphs, index_format;
    if (!c_u16(&f, &version) || !c_u16(&f, &flags) ||
        !c_u16(&f, &num_glyphs) || !c_u16(&f, &index_format)) return -1;
    int has_overlap = (flags & 0x0001) != 0;

    uint32_t ss[7];
    if ((2 + 7) * 4u > data_len) return -1;
    for (int i = 0; i < 7; i++)
        if (!c_u32(&f, &ss[i]) || ss[i] > data_len) return -1;
    /* substream base offsets; the whole set must fit the slice. */
    uint32_t base = (2 + 7) * 4, so[7];
    for (int i = 0; i < 7; i++) {
        so[i] = base;
        if (ss[i] > data_len - base) return -1;
        base += ss[i];
    }
    struct cur n_contour = { data + so[0], ss[0], 0 };
    struct cur n_points  = { data + so[1], ss[1], 0 };
    struct cur flag_s    = { data + so[2], ss[2], 0 };
    struct cur glyph_s   = { data + so[3], ss[3], 0 };
    struct cur comp_s    = { data + so[4], ss[4], 0 };
    struct cur bbox_s    = { data + so[5], ss[5], 0 };
    struct cur instr_s   = { data + so[6], ss[6], 0 };

    const uint8_t *overlap_bm = 0;
    if (has_overlap) {
        overlap_bm = data + base;
        unsigned obl = (num_glyphs + 7u) >> 3;
        if (obl > data_len - base) return -1;
    }

    const uint8_t *bbox_bm = bbox_s.p;
    unsigned bm_len = ((num_glyphs + 31u) >> 5) << 2;
    if (!c_skip(&bbox_s, bm_len)) return -1;

    uint32_t *loca_vals = (uint32_t *)kmalloc((num_glyphs + 1u) * 4);
    int16_t  *x_mins    = (int16_t *)kmalloc((num_glyphs ? num_glyphs : 1) * 2);
    /* glyf grows; cap generously (each point expands from 1-5 to ~5 bytes). */
    uint32_t gcap = (uint32_t)data_len * 8 + 65536;
    uint8_t *gbuf = (uint8_t *)kmalloc(gcap);
    uint8_t *tmp  = (uint8_t *)kmalloc(65536);   /* per-glyph scratch */
    unsigned tmp_cap = 65536;
    if (!loca_vals || !x_mins || !gbuf || !tmp) goto oom;

    uint32_t gpos = 0;
    unsigned n_pv[512];                  /* per-contour point counts */
    struct pt *pts = (struct pt *)kmalloc(0x20000 * sizeof(struct pt));
    if (!pts) goto oom;

    for (unsigned i = 0; i < num_glyphs; i++) {
        size_t gsz = 0;
        uint16_t n_contours = 0;
        int have_bbox = (bbox_bm[i >> 3] & (0x80 >> (i & 7))) != 0;
        if (!c_u16(&n_contour, &n_contours)) goto fail;

        if (n_contours == 0xffff) {          /* composite */
            int have_instr = 0; unsigned instr_size = 0; size_t csz = 0;
            if (!have_bbox) goto fail;
            if (!composite_size(&comp_s, &csz, &have_instr)) goto fail;
            if (have_instr && !rd255(&glyph_s, &instr_size)) goto fail;
            size_t need = 12 + csz + instr_size;
            if (need > tmp_cap) { kfree(tmp); tmp_cap = (unsigned)need; tmp = kmalloc(tmp_cap); if (!tmp) goto oom; }
            wr16(tmp, n_contours); gsz = 2;
            if (!c_read(&bbox_s, tmp + gsz, 8)) goto fail; gsz += 8;
            if (!c_read(&comp_s, tmp + gsz, csz)) goto fail; gsz += csz;
            if (have_instr) {
                wr16(tmp + gsz, instr_size); gsz += 2;
                if (!c_read(&instr_s, tmp + gsz, instr_size)) goto fail; gsz += instr_size;
            }
            if (i < num_glyphs) x_mins[i] = 0;   /* composites: x_min unused here */
        } else if (n_contours > 0) {          /* simple */
            unsigned total = 0;
            if (n_contours > 512) goto fail;
            for (unsigned j = 0; j < n_contours; j++) {
                unsigned npc;
                if (!rd255(&n_points, &npc)) goto fail;
                n_pv[j] = npc; total += npc;
                if (total > 0x1FFFF) goto fail;
            }
            if (total > flag_s.n - flag_s.off) goto fail;
            const uint8_t *fb = flag_s.p + flag_s.off;
            const uint8_t *tb = glyph_s.p + glyph_s.off;
            size_t tsz = glyph_s.n - glyph_s.off;
            long consumed = triplet_decode(fb, tb, tsz, total, pts);
            if (consumed < 0) goto fail;
            if (!c_skip(&flag_s, total)) goto fail;
            if (!c_skip(&glyph_s, (size_t)consumed)) goto fail;
            unsigned instr_size;
            if (!rd255(&glyph_s, &instr_size)) goto fail;
            size_t need = 12 + 2 * n_contours + 5 * total + instr_size + 16;
            if (need > tmp_cap) { kfree(tmp); tmp_cap = (unsigned)need; tmp = kmalloc(tmp_cap); if (!tmp) goto oom; }
            memset(tmp, 0, need);
            wr16(tmp, n_contours);
            if (have_bbox) { if (!c_read(&bbox_s, tmp + 2, 8)) goto fail; }
            else compute_bbox(total, pts, tmp);
            unsigned pos = GLYF_HDR;
            int endpt = -1;
            for (unsigned k = 0; k < n_contours; k++) {
                endpt += (int)n_pv[k];
                if (endpt >= 65536) goto fail;
                wr16(tmp + pos, (uint32_t)endpt); pos += 2;
            }
            wr16(tmp + pos, instr_size); pos += 2;
            if (!c_read(&instr_s, tmp + pos, instr_size)) goto fail;
            gsz = GLYF_HDR + 2 * n_contours + 2 + instr_size;   /* store_points starts here */
            int ov = has_overlap && (overlap_bm[i >> 3] & (0x80 >> (i & 7)));
            if (!store_points(total, pts, n_contours, instr_size, ov, tmp, tmp_cap, &gsz)) goto fail;
            x_mins[i] = (int16_t)rd16(tmp + 2);
        } else {
            if (have_bbox) goto fail;        /* empty glyph, no bbox */
            if (i < num_glyphs) x_mins[i] = 0;
        }

        loca_vals[i] = gpos;
        if (gpos + round4((uint32_t)gsz) > gcap) goto fail;
        if (gsz) memcpy(gbuf + gpos, tmp, gsz);
        gpos += (uint32_t)gsz;
        while (gpos & 3) gbuf[gpos++] = 0;   /* pad to 4 */
    }
    loca_vals[num_glyphs] = gpos;

    /* build loca table from offsets */
    uint32_t offsize = index_format ? 4 : 2;
    uint32_t loca_len = (num_glyphs + 1u) * offsize;
    uint8_t *lbuf = (uint8_t *)kmalloc(loca_len ? loca_len : 1);
    if (!lbuf) goto oom;
    for (unsigned i = 0; i <= num_glyphs; i++) {
        if (index_format) wr32(lbuf + i * 4, loca_vals[i]);
        else wr16(lbuf + i * 2, loca_vals[i] >> 1);
    }

    kfree(pts); kfree(loca_vals); kfree(tmp);
    glyf->data = gbuf; glyf->len = gpos; glyf->owned = 1;
    loca->data = lbuf; loca->len = loca_len; loca->owned = 1;
    *x_mins_out = x_mins; *num_glyphs_out = num_glyphs;
    return 0;

fail:
    kfree(pts);
oom:
    if (loca_vals) kfree(loca_vals);
    if (x_mins) kfree(x_mins);
    if (gbuf) kfree(gbuf);
    if (tmp) kfree(tmp);
    return -1;
}

/* Reconstruct a transformed hmtx (rare) using x_mins as the lsb source. */
static int reconstruct_hmtx(const uint8_t *data, size_t len, unsigned num_glyphs,
                            unsigned num_hmetrics, const int16_t *x_mins,
                            struct otab *hmtx) {
    struct cur f = { data, len, 0 };
    uint8_t hflags;
    if (!c_u8(&f, &hflags)) return -1;
    int lsb_absent = hflags & 1, lb_absent = hflags & 2;
    uint32_t out_len = num_hmetrics * 4u + (num_glyphs - num_hmetrics) * 2u;
    uint8_t *o = (uint8_t *)kmalloc(out_len ? out_len : 1);
    if (!o) return -1;
    for (unsigned i = 0; i < num_hmetrics; i++) {
        uint16_t adv;
        if (!c_u16(&f, &adv)) { kfree(o); return -1; }
        int16_t lsb;
        if (lsb_absent) lsb = x_mins ? x_mins[i] : 0;
        else { uint16_t v; if (!c_u16(&f, &v)) { kfree(o); return -1; } lsb = (int16_t)v; }
        wr16(o + i * 4, adv); wr16(o + i * 4 + 2, (uint32_t)(uint16_t)lsb);
    }
    for (unsigned i = num_hmetrics; i < num_glyphs; i++) {
        int16_t lsb;
        if (lb_absent) lsb = x_mins ? x_mins[i] : 0;
        else { uint16_t v; if (!c_u16(&f, &v)) { kfree(o); return -1; } lsb = (int16_t)v; }
        wr16(o + num_hmetrics * 4 + (i - num_hmetrics) * 2, (uint32_t)(uint16_t)lsb);
    }
    hmtx->data = o; hmtx->len = out_len; hmtx->owned = 1;
    return 0;
}

int woff2_to_sfnt(const uint8_t *woff2, size_t len, uint8_t **out, size_t *out_len) {
    if (!woff2_is(woff2, len) || len < 48) return -1;
    struct cur h = { woff2, len, 0 };
    uint32_t sig, flavor, flen, sfnt_size, comp_size;
    uint16_t num_tables, reserved;
    c_u32(&h, &sig); c_u32(&h, &flavor); c_u32(&h, &flen);
    c_u16(&h, &num_tables); c_u16(&h, &reserved);
    c_u32(&h, &sfnt_size); c_u32(&h, &comp_size);
    /* skip majorVersion..privLength (2+2 + 3*4 + 2*4 = 24 bytes wait):
     * majorVersion(2)+minorVersion(2)+meta{Off,Len,Orig}(12)+priv{Off,Len}(8) */
    if (!c_skip(&h, 2 + 2 + 12 + 8)) return -1;
    if (num_tables == 0 || num_tables > 4096) return -1;

    struct otab *T = (struct otab *)kmalloc(num_tables * sizeof(struct otab));
    uint32_t   *xlen = (uint32_t *)kmalloc(num_tables * 4);   /* src (transform) len */
    uint32_t   *xoff = (uint32_t *)kmalloc(num_tables * 4);   /* src offset in dec buf */
    uint32_t   *tflags = (uint32_t *)kmalloc(num_tables * 4);
    if (!T || !xlen || !xoff || !tflags) goto oom0;
    memset(T, 0, num_tables * sizeof(struct otab));

    uint32_t src_off = 0;
    for (int i = 0; i < num_tables; i++) {
        uint8_t fb;
        if (!c_u8(&h, &fb)) goto oom0;
        uint32_t tag, flags = 0;
        if ((fb & 0x3f) == 0x3f) { if (!c_u32(&h, &tag)) goto oom0; }
        else tag = kKnownTags[fb & 0x3f];
        uint8_t xv = (fb >> 6) & 3;
        if (tag == TAG_GLYF || tag == TAG_LOCA) { if (xv == 0) flags |= XFORM; }
        else if (xv != 0) flags |= XFORM;
        uint32_t dst_len, tr_len;
        if (!rdbase128(&h, &dst_len)) goto oom0;
        tr_len = dst_len;
        if (flags & XFORM) {
            if (!rdbase128(&h, &tr_len)) goto oom0;
            if (tag == TAG_LOCA && tr_len) goto oom0;
        }
        T[i].tag = tag; T[i].len = dst_len; tflags[i] = flags;
        xoff[i] = src_off; xlen[i] = tr_len;
        src_off += tr_len;
    }

    /* brotli-decompress the compressed table data (rest of the file). */
    uint32_t dec_len = src_off;
    uint8_t *dec = (uint8_t *)kmalloc(dec_len ? dec_len : 1);
    if (!dec) goto oom0;
    {
        unsigned long dl = dec_len;
        int pr = brotli_decompress(dec, &dl, woff2 + h.off, len - h.off);
        if (!(pr == BROTLI_OK || pr == BROTLI_TRUNC) || dl != dec_len) {
            kprintf("[woff2] brotli decode failed (%d, %lu/%u)\n", pr, dl, dec_len);
            kfree(dec); goto oom0;
        }
    }

    /* locate glyf/loca/hmtx + numGlyphs/numHMetrics for transforms. */
    unsigned num_glyphs = 0, num_hmetrics = 0;
    int16_t *x_mins = 0;
    int gi = -1, li = -1, hi = -1;
    for (int i = 0; i < num_tables; i++) {
        if (T[i].tag == TAG_GLYF) gi = i;
        else if (T[i].tag == TAG_LOCA) li = i;
        else if (T[i].tag == TAG_HMTX) hi = i;
        else if (T[i].tag == TAG_HHEA && xlen[i] >= 36)
            num_hmetrics = rd16(dec + xoff[i] + 34);
    }

    /* reconstruct transformed glyf + loca. */
    if (gi >= 0 && (tflags[gi] & XFORM)) {
        if (li < 0) { kfree(dec); goto oom0; }
        if (reconstruct_glyf(dec + xoff[gi], xlen[gi], &T[gi], &T[li],
                             &x_mins, &num_glyphs) != 0) { kfree(dec); goto oom0; }
    }
    /* non-transformed tables (incl. non-transformed glyf/loca): point at dec */
    for (int i = 0; i < num_tables; i++) {
        if (T[i].owned) continue;
        if (i == hi && (tflags[i] & XFORM)) continue;   /* handled below */
        T[i].data = dec + xoff[i]; T[i].len = xlen[i]; T[i].owned = 0;
    }
    /* transformed hmtx (rare). */
    if (hi >= 0 && (tflags[hi] & XFORM)) {
        if (reconstruct_hmtx(dec + xoff[hi], xlen[hi], num_glyphs, num_hmetrics,
                             x_mins, &T[hi]) != 0) {
            /* fall back: treat as opaque */
            T[hi].data = dec + xoff[hi]; T[hi].len = xlen[hi]; T[hi].owned = 0;
        }
    }

    /* ---- assemble the sfnt ---------------------------------------- */
    uint32_t total = 12 + (uint32_t)num_tables * 16;
    for (int i = 0; i < num_tables; i++) total += round4(T[i].len);
    uint8_t *o = (uint8_t *)kmalloc(total);
    if (!o) { kfree(dec); if (x_mins) kfree(x_mins); goto oom_owned; }

    /* offset table */
    wr32(o, flavor); wr16(o + 4, num_tables);
    unsigned mp = 0; while ((1u << (mp + 1)) <= (unsigned)num_tables) mp++;
    wr16(o + 6, (1u << mp) << 4); wr16(o + 8, mp);
    wr16(o + 10, ((unsigned)num_tables - (1u << mp)) << 4);

    /* records must be sorted by tag; use a simple selection order */
    int *order = (int *)kmalloc(num_tables * 4);
    if (!order) { kfree(o); kfree(dec); if (x_mins) kfree(x_mins); goto oom_owned; }
    for (int i = 0; i < num_tables; i++) order[i] = i;
    for (int a = 0; a < num_tables; a++)
        for (int b = a + 1; b < num_tables; b++)
            if (T[order[b]].tag < T[order[a]].tag) { int t = order[a]; order[a] = order[b]; order[b] = t; }

    uint32_t rec = 12, dat = 12 + (uint32_t)num_tables * 16;
    for (int k = 0; k < num_tables; k++) {
        struct otab *t = &T[order[k]];
        wr32(o + rec, t->tag);
        wr32(o + rec + 4, 0);            /* checksum (stbtt ignores) */
        wr32(o + rec + 8, dat);
        wr32(o + rec + 12, t->len);
        rec += 16;
        memcpy(o + dat, t->data, t->len);
        uint32_t padded = round4(t->len);
        for (uint32_t p = t->len; p < padded; p++) o[dat + p] = 0;
        dat += padded;
    }

    kfree(order);
    for (int i = 0; i < num_tables; i++) if (T[i].owned) kfree(T[i].data);
    if (x_mins) kfree(x_mins);
    kfree(dec); kfree(T); kfree(xlen); kfree(xoff); kfree(tflags);
    *out = o; *out_len = dat;
    return 0;

oom_owned:
    for (int i = 0; i < num_tables; i++) if (T[i].owned) kfree(T[i].data);
oom0:
    if (T) kfree(T); if (xlen) kfree(xlen); if (xoff) kfree(xoff); if (tflags) kfree(tflags);
    return -1;
}
