/* mp3_decode.c -- MP3/AAC/Opus decode wrappers for tobyOS.
 *
 * MP3 decoding is backed by minimp3.h (third_party stub).
 * AAC and Opus init/decode/free are placeholder stubs that return -1.
 */

#include "libtoby_internal.h"
#include <toby/audio_decode.h>
#include <stdlib.h>
#include <string.h>

#include "../../third_party/minimp3.h"

/* ---- MP3 ---- */

struct toby_mp3_decoder {
    mp3dec_t            dec;
    mp3dec_frame_info_t info;
};

toby_mp3_decoder_t *toby_mp3_decode_init(void)
{
    toby_mp3_decoder_t *d = (toby_mp3_decoder_t *)malloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    mp3dec_init(&d->dec);
    return d;
}

int toby_mp3_decode_frame(toby_mp3_decoder_t *dec,
                          const uint8_t *mp3_data, size_t mp3_len,
                          int16_t *pcm_out,
                          int *out_samples, int *out_channels,
                          int *out_sample_rate)
{
    if (!dec || !mp3_data || mp3_len == 0) return -1;

    int16_t pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int16_t *dst = pcm_out ? pcm_out : pcm_buf;

    int samples = mp3dec_decode_frame(&dec->dec,
                                      mp3_data, (int)mp3_len,
                                      dst, &dec->info);

    if (out_samples)    *out_samples    = samples;
    if (out_channels)   *out_channels   = dec->info.channels;
    if (out_sample_rate)*out_sample_rate = dec->info.hz;

    return dec->info.frame_bytes;
}

void toby_mp3_decode_free(toby_mp3_decoder_t *dec)
{
    free(dec);
}

/* ---- AAC (stub) ---- */

struct toby_aac_decoder { int dummy; };

toby_aac_decoder_t *toby_aac_decode_init(void)
{
    toby_aac_decoder_t *d = (toby_aac_decoder_t *)malloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    return d;
}

int toby_aac_decode_frame(toby_aac_decoder_t *dec,
                          const uint8_t *aac_data, size_t aac_len,
                          int16_t *pcm_out,
                          int *out_samples, int *out_channels,
                          int *out_sample_rate)
{
    (void)dec; (void)aac_data; (void)aac_len; (void)pcm_out;
    if (out_samples)    *out_samples    = 0;
    if (out_channels)   *out_channels   = 0;
    if (out_sample_rate)*out_sample_rate = 0;
    return -1;
}

void toby_aac_decode_free(toby_aac_decoder_t *dec)
{
    free(dec);
}

/* ---- Opus (stub) ---- */

struct toby_opus_decoder { int dummy; };

toby_opus_decoder_t *toby_opus_decode_init(void)
{
    toby_opus_decoder_t *d = (toby_opus_decoder_t *)malloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    return d;
}

int toby_opus_decode_frame(toby_opus_decoder_t *dec,
                           const uint8_t *opus_data, size_t opus_len,
                           int16_t *pcm_out,
                           int *out_samples, int *out_channels,
                           int *out_sample_rate)
{
    (void)dec; (void)opus_data; (void)opus_len; (void)pcm_out;
    if (out_samples)    *out_samples    = 0;
    if (out_channels)   *out_channels   = 0;
    if (out_sample_rate)*out_sample_rate = 0;
    return -1;
}

void toby_opus_decode_free(toby_opus_decoder_t *dec)
{
    free(dec);
}
