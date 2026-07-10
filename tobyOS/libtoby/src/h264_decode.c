/* h264_decode.c -- REAL H.264 decode for tobyOS userland (stage 13).
 *
 * Backed by the vendored h264bsd baseline-profile decoder (Android
 * AOSP-derived, pure integer C) -- this replaced the solid-colour
 * placeholder. Feed Annex-B byte-stream chunks (e.g. one AVI '##dc'
 * chunk = one access unit per call); when a picture completes it
 * comes back as ARGB8888 (h264bsd's "BGRA" u32 packing --
 * 0xFF<<24|r<<16|g<<8|b -- IS the tobyOS pixel format), valid until
 * the next decode call.
 */

#include "libtoby_internal.h"
#include <toby/video_decode.h>
#include <stdlib.h>
#include <string.h>

#include "../../third_party/h264bsd/h264bsd_decoder.h"

struct video_decoder {
    int        codec;
    storage_t *storage;
    u32        pic_seq;
};

struct video_decoder *video_decoder_create(int codec)
{
    if (codec != VIDEO_CODEC_H264) return NULL;   /* VP9: not supported */
    struct video_decoder *dec =
        (struct video_decoder *)malloc(sizeof(*dec));
    if (!dec) return NULL;
    memset(dec, 0, sizeof(*dec));
    dec->codec = codec;
    dec->storage = h264bsdAlloc();
    if (!dec->storage) { free(dec); return NULL; }
    if (h264bsdInit(dec->storage, 1 /* no output reordering */) != 0) {
        h264bsdFree(dec->storage);
        free(dec);
        return NULL;
    }
    return dec;
}

/* Decode one Annex-B chunk. Returns 1 when *out holds a picture
 * (mb-aligned dimensions; the buffer is the decoder's, valid until
 * the next call), 0 when the chunk was consumed without completing a
 * picture (parameter sets, partial data), -1 on a stream error. */
int video_decoder_decode(struct video_decoder *dec,
                         const uint8_t *nal, size_t nal_len,
                         struct video_frame *out)
{
    if (!dec || !dec->storage || !nal || nal_len == 0) return -1;
    u8 *p = (u8 *)nal;
    u32 left = (u32)nal_len;
    int got = 0, err = 0, zero_streak = 0;
    /* h264bsd's calling protocol: some returns (HDRS_RDY, the two-
     * phase paths) deliberately report readBytes == 0 and expect the
     * SAME buffer again -- the decoder resumes internally via
     * prevBytesConsumed. So a zero read is "call again", not "stuck";
     * the streak counter and iteration guard bound the loop. */
    for (int guard = 0; left > 0 && guard < 512; guard++) {
        u32 read_bytes = 0;
        u32 rc = h264bsdDecode(dec->storage, p, left, dec->pic_seq,
                               &read_bytes);
        if (read_bytes > left) break;
        p += read_bytes;
        left -= read_bytes;
        zero_streak = read_bytes ? 0 : zero_streak + 1;
        if (rc == H264BSD_PIC_RDY) {
            dec->pic_seq++;
            u32 pic_id = 0, is_idr = 0, err_mbs = 0;
            u32 *argb = h264bsdNextOutputPictureBGRA(dec->storage, &pic_id,
                                                     &is_idr, &err_mbs);
            if (argb && out) {
                out->data   = (uint8_t *)argb;
                out->width  = (int)(h264bsdPicWidth(dec->storage) * 16);
                out->height = (int)(h264bsdPicHeight(dec->storage) * 16);
                out->pts_ms = 0;
                got = 1;
            }
        } else if (rc == H264BSD_ERROR || rc == H264BSD_PARAM_SET_ERROR ||
                   rc == H264BSD_MEMALLOC_ERROR) {
            err = 1;
            if (read_bytes == 0) break;   /* can't step past the bad NAL */
        }
        if (zero_streak > 3) break;        /* resume states get re-calls */
    }
    return got ? 1 : (err ? -1 : 0);
}

void video_decoder_destroy(struct video_decoder *dec)
{
    if (!dec) return;
    if (dec->storage) {
        h264bsdShutdown(dec->storage);
        h264bsdFree(dec->storage);
    }
    free(dec);
}
