/* minimp3.h -- STUB implementation of the minimp3 single-header MP3 decoder.
 *
 * This provides the correct public interface so that tobyOS code can
 * compile and link against it.  The decode function always returns 0
 * (no frames decoded) -- swap this file for the real minimp3.h from
 * https://github.com/libuv/minimp3 to get actual MP3 playback.
 */

#ifndef MINIMP3_H
#define MINIMP3_H

#include <stdint.h>

#define MINIMP3_MAX_SAMPLES_PER_FRAME (1152 * 2)

typedef struct {
    float mdct_overlap[2][9 * 32];
    float qmf_state[15 * 2 * 32];
    int   reserv;
    int   free_format_bytes;
    unsigned char header[4];
    unsigned char reserv_buf[511];
} mp3dec_t;

typedef struct {
    int frame_bytes;
    int channels;
    int hz;
    int layer;
    int bitrate_kbps;
} mp3dec_frame_info_t;

#ifdef __cplusplus
extern "C" {
#endif

static inline void mp3dec_init(mp3dec_t *dec)
{
    if (!dec) return;
    unsigned char *p = (unsigned char *)dec;
    for (unsigned long i = 0; i < sizeof(mp3dec_t); i++)
        p[i] = 0;
}

static inline int mp3dec_decode_frame(mp3dec_t *dec,
                                      const uint8_t *mp3,
                                      int mp3_bytes,
                                      int16_t *pcm,
                                      mp3dec_frame_info_t *info)
{
    (void)dec;
    (void)mp3;
    (void)mp3_bytes;
    (void)pcm;
    if (info) {
        info->frame_bytes  = 0;
        info->channels     = 0;
        info->hz           = 0;
        info->layer        = 0;
        info->bitrate_kbps = 0;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* MINIMP3_H */
