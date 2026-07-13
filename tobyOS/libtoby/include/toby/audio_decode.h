/* toby/audio_decode.h -- Audio codec decode API for tobyOS userland.
 *
 * Provides init/decode/free wrappers for MP3, AAC, and Opus codecs.
 * MP3 is backed by minimp3; AAC and Opus are stubs for future use.
 */

#ifndef TOBY_AUDIO_DECODE_H
#define TOBY_AUDIO_DECODE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum PCM samples per decoded frame (stereo interleaved). */
#define TOBY_AUDIO_MAX_SAMPLES  (1152 * 2)

/* ---- MP3 decoder ---- */

typedef struct toby_mp3_decoder toby_mp3_decoder_t;

toby_mp3_decoder_t *toby_mp3_decode_init(void);
int  toby_mp3_decode_frame(toby_mp3_decoder_t *dec,
                           const uint8_t *mp3_data, size_t mp3_len,
                           int16_t *pcm_out,
                           int *out_samples, int *out_channels,
                           int *out_sample_rate);
void toby_mp3_decode_free(toby_mp3_decoder_t *dec);

/* ---- AAC decoder (stub) ---- */

typedef struct toby_aac_decoder toby_aac_decoder_t;

toby_aac_decoder_t *toby_aac_decode_init(void);
/* Decode one AAC frame. Returns the number of input bytes consumed
 * (>0) so a caller can advance a stream, or -1 on error. pcm_out needs
 * room for 1024*channels 16-bit samples. */
int  toby_aac_decode_frame(toby_aac_decoder_t *dec,
                           const uint8_t *aac_data, size_t aac_len,
                           int16_t *pcm_out,
                           int *out_samples, int *out_channels,
                           int *out_sample_rate);
/* Configure for raw (ADTS-less) AAC as carried in MP4 mp4a: profile is
 * the AudioSpecificConfig audioObjectType (2 = AAC-LC), plus the core
 * sample rate and channel count. Call once before decoding raw AUs. */
int  toby_aac_decode_set_raw(toby_aac_decoder_t *dec,
                             int profile, int sample_rate, int channels);
void toby_aac_decode_free(toby_aac_decoder_t *dec);

/* ---- Opus decoder (stub) ---- */

typedef struct toby_opus_decoder toby_opus_decoder_t;

toby_opus_decoder_t *toby_opus_decode_init(void);
int  toby_opus_decode_frame(toby_opus_decoder_t *dec,
                            const uint8_t *opus_data, size_t opus_len,
                            int16_t *pcm_out,
                            int *out_samples, int *out_channels,
                            int *out_sample_rate);
void toby_opus_decode_free(toby_opus_decoder_t *dec);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_AUDIO_DECODE_H */
