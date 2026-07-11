/* h264_decode.c -- REAL H.264 decode for tobyOS userland (stage 13).
 *
 * The video_decoder API decodes every H.264 profile through the vendored
 * openh264 (Cisco, BSD-2) via the C-linkage glue (oh264_glue.cpp).
 * openh264 handles baseline, Main and High -- CABAC, the 8x8 transform
 * and B-frames included -- so it is the single decode backend; baseline
 * is simply the subset of the bitstream it already understands. (An
 * earlier build routed baseline to the lighter h264bsd, but h264bsd
 * faulted when co-linked with openh264, so the whole codec now runs on
 * openh264, which is the more robust production decoder anyway.)
 *
 * A completed picture comes back as ARGB8888 (the tobyOS pixel format),
 * valid until the next decode call. Feed Annex-B byte-stream chunks (one
 * access unit per call works well). A call with nal_len == 0 flushes the
 * frame the decoder holds after the last AU (High profile has output
 * delay, so the final picture is drained this way).
 */

#include "libtoby_internal.h"
#include <toby/video_decode.h>
#include <toby/oh264_glue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct video_decoder {
    int   codec;
    void *oh;        /* oh264_glue session, created on first decode */
};

/* Scan an Annex-B buffer for the first SPS NAL (type 7) and return its
 * profile_idc (the byte right after the 1-byte NAL header), or -1 if no
 * SPS is present in this buffer. Purely informational now. */
static int peek_profile_idc(const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 4 < n; i++) {
        size_t off;
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
            off = i + 3;
        else if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1)
            off = i + 4;
        else
            continue;
        if (off < n && (p[off] & 0x1f) == 7 && off + 1 < n)
            return p[off + 1];              /* profile_idc */
    }
    return -1;
}

struct video_decoder *video_decoder_create(int codec)
{
    if (codec != VIDEO_CODEC_H264) return NULL;   /* VP9: not supported */
    struct video_decoder *dec =
        (struct video_decoder *)malloc(sizeof(*dec));
    if (!dec) return NULL;
    memset(dec, 0, sizeof(*dec));
    dec->codec = codec;
    return dec;                                   /* openh264 opened lazily */
}

/* Decode one Annex-B chunk. Returns 1 when *out holds a picture (ARGB,
 * the buffer is the decoder's, valid until the next call), 0 when the
 * chunk was consumed without completing a picture, -1 on a stream error.
 * Pass nal_len == 0 to flush a held frame. */
int video_decoder_decode(struct video_decoder *dec,
                         const uint8_t *nal, size_t nal_len,
                         struct video_frame *out)
{
    if (!dec) return -1;

    if (!dec->oh) {
        if (!nal || nal_len == 0) return -1;   /* need the first AU to start */
        int prof = peek_profile_idc(nal, nal_len);
        dec->oh = oh264_open();
        if (!dec->oh) return -1;
        printf("[vdec] profile_idc=%d -> openh264\n", prof);
    }

    uint32_t *argb = NULL; int w = 0, h = 0;
    int r = (nal && nal_len)
                ? oh264_decode(dec->oh, nal, (int)nal_len, &argb, &w, &h)
                : oh264_flush(dec->oh, &argb, &w, &h);
    if (r == 1 && out) {
        out->data   = (uint8_t *)argb;
        out->width  = w;
        out->height = h;
        out->pts_ms = 0;
        return 1;
    }
    return r < 0 ? -1 : 0;
}

void video_decoder_destroy(struct video_decoder *dec)
{
    if (!dec) return;
    if (dec->oh) oh264_close(dec->oh);
    free(dec);
}
