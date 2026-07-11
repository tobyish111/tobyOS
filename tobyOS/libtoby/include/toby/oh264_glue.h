/* toby/oh264_glue.h -- C-linkage bridge to the vendored openh264
 * (Cisco) High-profile H.264 decoder, so the C video_decoder in
 * h264_decode.c can drive the C++ ISVCDecoder. The glue owns the
 * decoder + a per-frame ARGB8888 buffer (I420 -> ARGB conversion done
 * here), matching the video_frame contract. Single decode at a time.
 * See libtoby/src/oh264_glue.cpp. */

#ifndef TOBY_OH264_GLUE_H
#define TOBY_OH264_GLUE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create/destroy an openh264 decoder session. Returns NULL on failure. */
void *oh264_open(void);
void  oh264_close(void *h);

/* Feed one Annex-B access unit. If a picture completes it is converted
 * to ARGB8888 into the session's buffer: *out_argb points at it (owned
 * by the session, valid until the next call), *out_w/*out_h are the
 * display dimensions. Returns 1 = frame ready, 0 = consumed no frame,
 * -1 = error. */
int oh264_decode(void *h, const uint8_t *au, int len,
                 uint32_t **out_argb, int *out_w, int *out_h);

/* Drain a picture the decoder is holding after the last AU (High
 * profile has output delay). Same return contract as oh264_decode. */
int oh264_flush(void *h, uint32_t **out_argb, int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_OH264_GLUE_H */
