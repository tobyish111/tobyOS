/* mp3_decode.c -- MP3/AAC/Opus decode wrappers for tobyOS.
 *
 * MP3 decoding is backed by the REAL minimp3 (lieff/minimp3, CC0) --
 * the stage-13 media work swapped out the placeholder stub, so
 * toby_mp3_decode_frame now yields actual PCM.
 * AAC and Opus init/decode/free remain placeholder stubs (-1).
 */

#include "libtoby_internal.h"
#include <toby/audio_decode.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
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

/* ---- AAC-LC / HE-AAC via the vendored Helix decoder ---- */

#include "../../third_party/libhelix-aac/aacdec.h"

struct toby_aac_decoder {
    HAACDecoder h;
    int raw_configured;      /* AACSetRawBlockParams done for raw (MP4) input */
};

toby_aac_decoder_t *toby_aac_decode_init(void)
{
    toby_aac_decoder_t *d = (toby_aac_decoder_t *)malloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->h = AACInitDecoder();
    if (!d->h) { free(d); return NULL; }
    return d;
}

/* Decode one AAC frame. Input may be ADTS-framed (self-describing sync)
 * or raw AAC access units. For ADTS we let AACDecode find the sync; for
 * raw input (MP4 mp4a) the caller must first set params via
 * toby_aac_decode_set_raw(). PCM is 16-bit interleaved in pcm_out
 * (needs >= 1024*channels shorts). Returns 0 on success. */
int toby_aac_decode_frame(toby_aac_decoder_t *dec,
                          const uint8_t *aac_data, size_t aac_len,
                          int16_t *pcm_out,
                          int *out_samples, int *out_channels,
                          int *out_sample_rate)
{
    if (out_samples)    *out_samples    = 0;
    if (out_channels)   *out_channels   = 0;
    if (out_sample_rate)*out_sample_rate = 0;
    if (!dec || !dec->h || !aac_data || !aac_len || !pcm_out) return -1;

    unsigned char *inbuf = (unsigned char *)aac_data;
    int bytesLeft = (int)aac_len;

    /* ADTS input: skip to the next sync word so a partial buffer still
     * lands on a frame boundary. Raw input already starts at an AU. */
    if (!dec->raw_configured) {
        int off = AACFindSyncWord(inbuf, bytesLeft);
        if (off >= 0) { inbuf += off; bytesLeft -= off; }
    }

    int err = AACDecode(dec->h, &inbuf, &bytesLeft, (short *)pcm_out);
    if (err) return -1;

    AACFrameInfo fi;
    memset(&fi, 0, sizeof(fi));
    AACGetLastFrameInfo(dec->h, &fi);
    if (fi.nChans <= 0 || fi.outputSamps <= 0) return -1;

    if (out_channels)    *out_channels    = fi.nChans;
    if (out_sample_rate) *out_sample_rate = fi.sampRateOut;
    if (out_samples)     *out_samples     = fi.outputSamps / fi.nChans;
    /* bytes consumed from the input, so callers can advance a stream. */
    return (int)aac_len - bytesLeft;
}

/* Configure the decoder for raw (ADTS-less) AAC, as carried in MP4:
 * profile (1=Main,2=LC,3=SSR..; the AudioSpecificConfig audioObjectType),
 * sampRateCore Hz, nChans. Call once before decoding raw AUs. */
int toby_aac_decode_set_raw(toby_aac_decoder_t *dec,
                            int profile, int sample_rate, int channels)
{
    if (!dec || !dec->h) return -1;
    AACFrameInfo fi;
    memset(&fi, 0, sizeof(fi));
    fi.nChans      = channels;
    fi.sampRateCore = sample_rate;
    fi.profile     = profile > 0 ? profile - 1 : 1;   /* Helix: 0=Main,1=LC */
    if (AACSetRawBlockParams(dec->h, 0, &fi) != 0) return -1;
    dec->raw_configured = 1;
    return 0;
}

void toby_aac_decode_free(toby_aac_decoder_t *dec)
{
    if (dec) { if (dec->h) AACFreeDecoder(dec->h); free(dec); }
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
