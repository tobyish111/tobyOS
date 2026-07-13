/* user_aactest/main.c -- /bin/aactest: decode-correctness test for the
 * vendored Helix AAC-LC decoder wired to libtoby's toby_aac_decode API.
 *
 * Decodes an embedded 0.1s 440Hz sine ADTS clip (mono, 44100Hz, 128k)
 * frame by frame and checks the PCM is PLAUSIBLE: right channels/rate,
 * a sensible sample count, non-silent, and a ~440Hz tone (RMS +
 * zero-crossings in range). AAC decode is not bit-exact across
 * implementations (Helix is fixed-point, ffmpeg float), so we verify
 * signal characteristics rather than a byte checksum.
 *
 * "[aac] ..." markers on serial; exit 0 iff all checks pass. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include <toby/audio_decode.h>
#include "aac_clip.h"

static int isqrt_i(long v) {
    if (v <= 0) return 0;
    long r = v, p = 0;
    for (int i = 0; i < 40 && r != p; i++) { p = r; r = (r + v / r) / 2; }
    return (int)r;
}

int main(void) {
    printf("[aac] Helix AAC decode test: %d bytes ADTS\n", g_aac_len);

    toby_aac_decoder_t *dec = toby_aac_decode_init();
    if (!dec) { printf("[aac] init FAIL\n"); return 1; }

    static int16_t pcm[2048];         /* one frame (<=1024 * 2ch) */
    static int16_t all[16384];        /* accumulated mono/stereo PCM */
    int total = 0, chans = 0, rate = 0, frames = 0;

    const uint8_t *p = g_aac;
    int left = g_aac_len;
    while (left > 8 && total < (int)(sizeof(all) / sizeof(all[0])) - 2048) {
        int s = 0, c = 0, r = 0;
        int used = toby_aac_decode_frame(dec, p, (size_t)left, pcm, &s, &c, &r);
        if (used <= 0) break;
        p += used; left -= used;
        if (s <= 0 || c <= 0) continue;
        chans = c; rate = r; frames++;
        for (int i = 0; i < s * c && total < (int)(sizeof(all)/sizeof(all[0])); i++)
            all[total++] = pcm[i];
    }
    toby_aac_decode_free(dec);

    int per_chan = chans ? total / chans : 0;
    printf("[aac] decoded %d frames, %d samples/chan, %d ch, %d Hz\n",
           frames, per_chan, chans, rate);

    /* RMS + zero-crossings on channel 0 (skip the leading encoder-delay
     * silence so the tone dominates the metrics). */
    long sq = 0; int nz = 0, zc = 0, prev_sign = 0, counted = 0;
    for (int i = 0; i < total; i += (chans ? chans : 1)) {
        int v = all[i];
        if (v > 200 || v < -200) nz = 1;    /* found the tone */
        if (!nz) continue;
        sq += (long)v * v; counted++;
        int sign = v < 0 ? -1 : 1;
        if (prev_sign && sign != prev_sign) zc++;
        prev_sign = sign;
    }
    int rms = counted ? isqrt_i(sq / counted) : 0;
    printf("[aac] tone: RMS=%d (ref~%d), zero-crossings=%d (ref~%d)\n",
           rms, AAC_REF_RMS, zc, AAC_REF_ZC);

    int ok = 1;
    if (chans != AAC_REF_CHANS)  { printf("[aac] FAIL channels %d != %d\n", chans, AAC_REF_CHANS); ok = 0; }
    if (rate  != AAC_REF_RATE)   { printf("[aac] FAIL rate %d != %d\n", rate, AAC_REF_RATE); ok = 0; }
    if (per_chan < 3072 || per_chan > 8192) { printf("[aac] FAIL sample count %d\n", per_chan); ok = 0; }
    if (rms < 800 || rms > 12000) { printf("[aac] FAIL RMS %d out of range\n", rms); ok = 0; }
    /* 440Hz over ~0.09s of tone ~= 40 cycles ~= 80 crossings; allow slack. */
    if (zc < 40 || zc > 200) { printf("[aac] FAIL zero-crossings %d\n", zc); ok = 0; }

    printf("[aac] decode self-test: %s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}
