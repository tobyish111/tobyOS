/* oh264_glue.cpp -- C-linkage bridge to the openh264 ISVCDecoder for
 * libtoby's video_decoder (media slice 3). Wraps create/decode/flush/
 * destroy and converts openh264's I420 (YUV420 planar) output to
 * ARGB8888 (the tobyOS pixel format the browser + video_frame expect).
 * See toby/oh264_glue.h. */

#include <toby/oh264_glue.h>
#include <stdlib.h>
#include <string.h>

#include "codec_api.h"
#include "codec_app_def.h"
#include "codec_def.h"

struct oh264_sess {
    ISVCDecoder *dec;
    uint32_t    *argb;      /* ARGB8888 output, grown on demand */
    size_t       argb_cap;  /* in pixels */
};

extern "C" void *oh264_open(void) {
    oh264_sess *s = (oh264_sess *)calloc(1, sizeof *s);
    if (!s) return 0;
    if (WelsCreateDecoder(&s->dec) != 0 || !s->dec) { free(s); return 0; }
    SDecodingParam p;
    memset(&p, 0, sizeof p);
    p.eEcActiveIdc = ERROR_CON_DISABLE;
    p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    if (s->dec->Initialize(&p) != 0) {
        WelsDestroyDecoder(s->dec);
        free(s);
        return 0;
    }
    return s;
}

/* Integer BT.601 (limited range) I420 -> ARGB8888. This exact formula
 * is mirrored by the reference-checksum generator, so the on-device
 * ARGB is verified bit-for-bit. */
static inline uint8_t clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

static int emit_argb(oh264_sess *s, unsigned char *const dst[3],
                     const SBufferInfo *bi, uint32_t **out_argb,
                     int *out_w, int *out_h) {
    int W = bi->UsrData.sSystemBuffer.iWidth;
    int H = bi->UsrData.sSystemBuffer.iHeight;
    int sy = bi->UsrData.sSystemBuffer.iStride[0];
    int suv = bi->UsrData.sSystemBuffer.iStride[1];
    if (W <= 0 || H <= 0) return -1;
    size_t need = (size_t)W * (size_t)H;
    if (need > s->argb_cap) {
        uint32_t *nb = (uint32_t *)realloc(s->argb, need * 4);
        if (!nb) return -1;
        s->argb = nb;
        s->argb_cap = need;
    }
    const unsigned char *Y = dst[0], *U = dst[1], *V = dst[2];
    for (int y = 0; y < H; y++) {
        const unsigned char *yr = Y + y * sy;
        const unsigned char *ur = U + (y >> 1) * suv;
        const unsigned char *vr = V + (y >> 1) * suv;
        uint32_t *o = s->argb + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            int c = (int)yr[x] - 16;
            int d = (int)ur[x >> 1] - 128;
            int e = (int)vr[x >> 1] - 128;
            uint8_t r = clip8((298 * c + 409 * e + 128) >> 8);
            uint8_t g = clip8((298 * c - 100 * d - 208 * e + 128) >> 8);
            uint8_t b = clip8((298 * c + 516 * d + 128) >> 8);
            o[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
    *out_argb = s->argb;
    *out_w = W;
    *out_h = H;
    return 1;
}

extern "C" int oh264_decode(void *h, const uint8_t *au, int len,
                            uint32_t **out_argb, int *out_w, int *out_h) {
    oh264_sess *s = (oh264_sess *)h;
    if (!s || !s->dec) return -1;
    unsigned char *dst[3] = { 0, 0, 0 };
    SBufferInfo bi;
    memset(&bi, 0, sizeof bi);
    s->dec->DecodeFrameNoDelay(au, len, dst, &bi);
    if (bi.iBufferStatus == 1 && dst[0])
        return emit_argb(s, dst, &bi, out_argb, out_w, out_h);
    return 0;
}

extern "C" int oh264_flush(void *h, uint32_t **out_argb, int *out_w, int *out_h) {
    oh264_sess *s = (oh264_sess *)h;
    if (!s || !s->dec) return -1;
    unsigned char *dst[3] = { 0, 0, 0 };
    SBufferInfo bi;
    memset(&bi, 0, sizeof bi);
    s->dec->FlushFrame(dst, &bi);
    if (bi.iBufferStatus == 1 && dst[0])
        return emit_argb(s, dst, &bi, out_argb, out_w, out_h);
    return 0;
}

extern "C" void oh264_close(void *h) {
    oh264_sess *s = (oh264_sess *)h;
    if (!s) return;
    if (s->dec) { s->dec->Uninitialize(); WelsDestroyDecoder(s->dec); }
    if (s->argb) free(s->argb);
    free(s);
}
