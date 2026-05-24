/* toby/video_decode.h -- Video codec decode API for tobyOS userland.
 *
 * Provides a minimal H.264 NAL unit parser and decode interface.
 * VP9 is defined but stubbed out.  The actual decoder currently
 * generates solid-colour placeholder frames at the correct dimensions.
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
