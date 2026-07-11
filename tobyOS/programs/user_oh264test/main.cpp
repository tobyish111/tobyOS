/* user_oh264test/main.cpp -- /bin/oh264test: decode-correctness test for
 * the vendored openh264 High-profile H.264 decoder (media slice 2).
 *
 * Decodes an embedded High-profile Annex-B clip (CABAC + 8x8 transform,
 * the features baseline h264bsd cannot do) through the openh264
 * ISVCDecoder, and checks each output frame's tight-YUV FNV-1a checksum
 * against the reference from ffmpeg's decode of the same stream. H.264
 * reconstruction is bit-exact by spec, so a match proves openh264
 * decodes correctly on tobyOS. Results print as "[oh264] ..." serial
 * markers; exit 0 on all-pass, 1 on any failure. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "codec_api.h"
#include "codec_app_def.h"
#include "codec_def.h"
#include "oh264_clip.h"

static unsigned fnv1a(const unsigned char *p, int n) {
    unsigned h = 0x811c9dc5u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}

/* Extract the tight W x H Y plane + W/2 x H/2 U,V planes (stripping the
 * decoder's row padding) and checksum them, matching the reference. */
static unsigned frame_csum(unsigned char *const dst[3], const SBufferInfo *bi) {
    static unsigned char t[OH_W * OH_H * 3 / 2];
    int W = bi->UsrData.sSystemBuffer.iWidth;
    int H = bi->UsrData.sSystemBuffer.iHeight;
    int sy = bi->UsrData.sSystemBuffer.iStride[0];
    int suv = bi->UsrData.sSystemBuffer.iStride[1];
    if (W != OH_W || H != OH_H) return 0;          /* wrong dims -> mismatch */
    int o = 0;
    for (int y = 0; y < H; y++) { memcpy(t + o, dst[0] + y * sy, W); o += W; }
    for (int y = 0; y < H / 2; y++) { memcpy(t + o, dst[1] + y * suv, W / 2); o += W / 2; }
    for (int y = 0; y < H / 2; y++) { memcpy(t + o, dst[2] + y * suv, W / 2); o += W / 2; }
    return fnv1a(t, o);
}

static int g_frame, g_pass, g_fail;

/* Checksum a ready output frame and compare to the reference. */
static void process(unsigned char *const dst[3], const SBufferInfo *bi) {
    unsigned got = frame_csum(dst, bi);
    unsigned exp = (g_frame < OH_NFRAMES) ? oh_frame_csum[g_frame] : 0;
    int ok = (got == exp);
    printf("[oh264] frame %d: csum=0x%08x exp=0x%08x %s\n",
           g_frame, got, exp, ok ? "OK" : "MISMATCH");
    if (ok) g_pass++; else g_fail++;
    g_frame++;
}

/* Feed one access unit; if a frame comes out, check it. */
static void feed(ISVCDecoder *dec, const unsigned char *au, int len) {
    unsigned char *dst[3] = { 0, 0, 0 };
    SBufferInfo bi;
    memset(&bi, 0, sizeof bi);
    dec->DecodeFrameNoDelay(au, len, dst, &bi);
    if (bi.iBufferStatus == 1 && dst[0]) process(dst, &bi);
}

extern "C" int main(void) {
    ISVCDecoder *dec = 0;
    if (WelsCreateDecoder(&dec) != 0 || !dec) {
        printf("[oh264] WelsCreateDecoder FAILED\n");
        return 1;
    }
    SDecodingParam p;
    memset(&p, 0, sizeof p);
    p.eEcActiveIdc = ERROR_CON_DISABLE;
    p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    if (dec->Initialize(&p) != 0) {
        printf("[oh264] Initialize FAILED\n");
        WelsDestroyDecoder(dec);
        return 1;
    }
    printf("[oh264] decoder up; clip %d bytes, expecting %d High-profile frames\n",
           OH_CLIP_LEN, OH_NFRAMES);

    /* Split the Annex-B clip into access units: each VCL NAL (type 1..5)
     * ends the current AU (this clip is one slice per frame, no B-frames),
     * so leading SPS/PPS/SEI attach to the following coded picture. */
    int au_start = 0;
    for (int i = 0; i + 3 < OH_CLIP_LEN; ) {
        int sc = 0, sclen = 0;
        if (oh_clip[i] == 0 && oh_clip[i + 1] == 0 && oh_clip[i + 2] == 1) {
            sc = 1; sclen = 3;
        } else if (i + 4 < OH_CLIP_LEN && oh_clip[i] == 0 && oh_clip[i + 1] == 0 &&
                   oh_clip[i + 2] == 0 && oh_clip[i + 3] == 1) {
            sc = 1; sclen = 4;
        }
        if (!sc) { i++; continue; }
        int ntype = oh_clip[i + sclen] & 0x1f;
        /* find the next start code to bound this NAL */
        int j = i + sclen;
        while (j + 3 < OH_CLIP_LEN) {
            if (oh_clip[j] == 0 && oh_clip[j + 1] == 0 &&
                (oh_clip[j + 2] == 1 ||
                 (j + 3 < OH_CLIP_LEN && oh_clip[j + 2] == 0 && oh_clip[j + 3] == 1)))
                break;
            j++;
        }
        int nal_end = (j + 3 < OH_CLIP_LEN) ? j : OH_CLIP_LEN;
        if (ntype >= 1 && ntype <= 5) {            /* VCL NAL -> AU complete */
            feed(dec, oh_clip + au_start, nal_end - au_start);
            au_start = nal_end;
        }
        i = nal_end;
    }
    /* Drain frames the decoder holds after the last AU. High profile
     * (profile_idc != 66) has output delay, so FlushFrame() is the API
     * that releases the trailing picture(s). */
    for (int f = 0; f < OH_NFRAMES && g_frame < OH_NFRAMES; f++) {
        unsigned char *dst[3] = { 0, 0, 0 };
        SBufferInfo bi; memset(&bi, 0, sizeof bi);
        dec->FlushFrame(dst, &bi);
        if (bi.iBufferStatus == 1 && dst[0]) process(dst, &bi);
        else break;
    }

    dec->Uninitialize();
    WelsDestroyDecoder(dec);

    int ok = (g_fail == 0 && g_pass == OH_NFRAMES);
    printf("[oh264] decode self-test: %d/%d frames match %s\n",
           g_pass, OH_NFRAMES, ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
