/* toby/video_decode.h -- Video codec decode API for tobyOS userland.
 *
 * H.264 baseline profile decodes for REAL via the vendored h264bsd
 * (stage 13 media). Feed Annex-B byte-stream chunks (one access unit
 * per call works well); video_decoder_decode returns 1 when *out
 * holds a picture (ARGB8888, mb-aligned dimensions, buffer owned by
 * the decoder until the next call), 0 when the chunk was consumed
 * without completing a picture, -1 on a stream error.
 * VP9 is not supported (create returns NULL).
 */

#ifndef TOBY_VIDEO_DECODE_H
#define TOBY_VIDEO_DECODE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIDEO_CODEC_H264  0
#define VIDEO_CODEC_VP9   1

struct video_frame {
    uint8_t *data;      /* ARGB8888 pixel data */
    int      width;
    int      height;
    int64_t  pts_ms;    /* presentation timestamp */
};

struct video_decoder;

struct video_decoder *video_decoder_create(int codec);
int  video_decoder_decode(struct video_decoder *dec,
                          const uint8_t *nal, size_t nal_len,
                          struct video_frame *out);
void video_decoder_destroy(struct video_decoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_VIDEO_DECODE_H */
