/* user_oh264test/main.cpp -- /bin/oh264test: decode-correctness test for
 * the vendored openh264 High-profile H.264 decoder (media slices 2 + 3).
 *
 * Two phases over one embedded High-profile Annex-B clip (CABAC + 8x8
 * transform, which baseline h264bsd cannot do):
 *   1. direct ISVCDecoder -> YUV, checked against ffmpeg's YUV (slice 2);
 *   2. libtoby's video_decoder -> ARGB, checked against the same YUV run
 *      through the oh264_glue BT.601 conversion, proving the profile
 *      routing (High -> openh264) + the ARGB path the browser uses.
 * H.264 reconstruction is bit-exact by spec, so the FNV-1a checksums in
 * oh264_clip.h are strict oracles. "[oh264] ..." markers on serial;
 * exit 0 = all pass. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "codec_api.h"
#include "codec_app_def.h"
#include "codec_def.h"
#include <toby/video_decode.h>
#include "oh264_clip.h"

static unsigned fnv1a(const unsigned char *p, int n) {
    unsigned h = 0x811c9dc5u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}

/* ---- AU splitter: each VCL NAL (type 1..5) ends an access unit ---- */
typedef void (*au_cb)(void *ctx, const unsigned char *au, int len);
static void walk_aus(const unsigned char *clip, int clen, au_cb cb, void *ctx) {
    int au_start = 0;
    for (int i = 0; i + 3 < clen; ) {
        int sc = 0, sclen = 0;
        if (clip[i] == 0 && clip[i + 1] == 0 && clip[i + 2] == 1) {
            sc = 1; sclen = 3;
        } else if (i + 4 < clen && clip[i] == 0 && clip[i + 1] == 0 &&
                   clip[i + 2] == 0 && clip[i + 3] == 1) {
            sc = 1; sclen = 4;
        }
        if (!sc) { i++; continue; }
        int ntype = clip[i + sclen] & 0x1f;
        int j = i + sclen;
        while (j + 3 < clen) {
            if (clip[j] == 0 && clip[j + 1] == 0 &&
                (clip[j + 2] == 1 ||
                 (j + 3 < clen && clip[j + 2] == 0 && clip[j + 3] == 1)))
                break;
            j++;
        }
        int nal_end = (j + 3 < clen) ? j : clen;
        if (ntype >= 1 && ntype <= 5) {
            cb(ctx, clip + au_start, nal_end - au_start);
            au_start = nal_end;
        }
        i = nal_end;
    }
}

struct phase { int frame, pass, fail; const char *tag; };

static void check(struct phase *ph, unsigned got, unsigned exp) {
    int ok = (got == exp);
    printf("[oh264] %s frame %d: csum=0x%08x exp=0x%08x %s\n",
           ph->tag, ph->frame, got, exp, ok ? "OK" : "MISMATCH");
    if (ok) ph->pass++; else ph->fail++;
    ph->frame++;
}

/* ---- phase 1: direct ISVCDecoder -> tight YUV ---- */
struct dctx { ISVCDecoder *dec; struct phase ph; };

static unsigned yuv_csum(unsigned char *const dst[3], const SBufferInfo *bi) {
    static unsigned char t[OH_W * OH_H * 3 / 2];
    int W = bi->UsrData.sSystemBuffer.iWidth, H = bi->UsrData.sSystemBuffer.iHeight;
    int sy = bi->UsrData.sSystemBuffer.iStride[0], suv = bi->UsrData.sSystemBuffer.iStride[1];
    if (W != OH_W || H != OH_H) return 0;
    int o = 0;
    for (int y = 0; y < H; y++) { memcpy(t + o, dst[0] + y * sy, W); o += W; }
    for (int y = 0; y < H / 2; y++) { memcpy(t + o, dst[1] + y * suv, W / 2); o += W / 2; }
    for (int y = 0; y < H / 2; y++) { memcpy(t + o, dst[2] + y * suv, W / 2); o += W / 2; }
    return fnv1a(t, o);
}

static void feed_direct(void *ctx, const unsigned char *au, int len) {
    struct dctx *c = (struct dctx *)ctx;
    unsigned char *dst[3] = { 0, 0, 0 };
    SBufferInfo bi; memset(&bi, 0, sizeof bi);
    c->dec->DecodeFrameNoDelay(au, len, dst, &bi);
    if (bi.iBufferStatus == 1 && dst[0])
        check(&c->ph, yuv_csum(dst, &bi), oh_frame_csum[c->ph.frame < OH_NFRAMES ? c->ph.frame : 0]);
}

/* ---- phase 2: libtoby video_decoder -> ARGB (routing + conversion) ---- */
struct vctx { struct video_decoder *vd; struct phase ph; };

static void feed_vd(void *ctx, const unsigned char *au, int len) {
    struct vctx *c = (struct vctx *)ctx;
    struct video_frame f; memset(&f, 0, sizeof f);
    if (video_decoder_decode(c->vd, au, (size_t)len, &f) == 1 && f.data) {
        unsigned got = fnv1a(f.data, f.width * f.height * 4);
        check(&c->ph, got, oh_argb_csum[c->ph.frame < OH_NFRAMES ? c->ph.frame : 0]);
    }
}

extern "C" int main(void) {
    /* ---- Phase 1: direct ISVCDecoder ---- */
    struct dctx d; memset(&d, 0, sizeof d); d.ph.tag = "yuv";
    if (WelsCreateDecoder(&d.dec) != 0 || !d.dec) {
        printf("[oh264] WelsCreateDecoder FAILED\n"); return 1;
    }
    SDecodingParam p; memset(&p, 0, sizeof p);
    p.eEcActiveIdc = ERROR_CON_DISABLE;
    p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    if (d.dec->Initialize(&p) != 0) {
        printf("[oh264] Initialize FAILED\n"); WelsDestroyDecoder(d.dec); return 1;
    }
    printf("[oh264] clip %d bytes, %d High-profile frames\n", OH_CLIP_LEN, OH_NFRAMES);
    walk_aus(oh_clip, OH_CLIP_LEN, feed_direct, &d);
    for (int f = 0; f < OH_NFRAMES && d.ph.frame < OH_NFRAMES; f++) {
        unsigned char *dst[3] = { 0, 0, 0 }; SBufferInfo bi; memset(&bi, 0, sizeof bi);
        d.dec->FlushFrame(dst, &bi);
        if (bi.iBufferStatus == 1 && dst[0])
            check(&d.ph, yuv_csum(dst, &bi), oh_frame_csum[d.ph.frame < OH_NFRAMES ? d.ph.frame : 0]);
        else break;
    }
    d.dec->Uninitialize(); WelsDestroyDecoder(d.dec);
    int p1 = (d.ph.fail == 0 && d.ph.pass == OH_NFRAMES);
    printf("[oh264] direct ISVCDecoder (YUV): %d/%d %s\n",
           d.ph.pass, OH_NFRAMES, p1 ? "ALL PASS" : "FAILURES");

    /* ---- Phase 2: video_decoder (routing + ARGB) ---- */
    struct vctx v; memset(&v, 0, sizeof v); v.ph.tag = "argb";
    v.vd = video_decoder_create(VIDEO_CODEC_H264);
    if (!v.vd) { printf("[oh264] video_decoder_create FAILED\n"); return 1; }
    walk_aus(oh_clip, OH_CLIP_LEN, feed_vd, &v);
    for (int f = 0; f < OH_NFRAMES && v.ph.frame < OH_NFRAMES; f++) {
        struct video_frame fr; memset(&fr, 0, sizeof fr);
        if (video_decoder_decode(v.vd, 0, 0, &fr) == 1 && fr.data) {  /* flush */
            unsigned got = fnv1a(fr.data, fr.width * fr.height * 4);
            check(&v.ph, got, oh_argb_csum[v.ph.frame < OH_NFRAMES ? v.ph.frame : 0]);
        } else break;
    }
    video_decoder_destroy(v.vd);
    int p2 = (v.ph.fail == 0 && v.ph.pass == OH_NFRAMES);
    printf("[oh264] video_decoder (ARGB, routed): %d/%d %s\n",
           v.ph.pass, OH_NFRAMES, p2 ? "ALL PASS" : "FAILURES");

    int ok = p1 && p2;
    printf("[oh264] decode self-test: %s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
