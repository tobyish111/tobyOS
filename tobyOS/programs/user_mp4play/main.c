/* user_mp4play/main.c -- /bin/mp4play: end-to-end High-profile H.264
 * <video> decode proof for media slice 4, exercising the SAME path the
 * browser uses -- MP4 (ISO-BMFF) container demux -> libtoby video_decoder
 * (openh264) -> ARGB8888 -- on an embedded HIGH-profile clip (CABAC +
 * 8x8 transform, the things baseline / h264bsd cannot do) with P-frame
 * inter-prediction. Frames are fed in decode order and drained with the
 * decoder's output-delay flush, exactly as media_pump() in the browser
 * does, and each produced ARGB frame's FNV-1a must match ohm_argb_csum[]
 * (the ffmpeg reference run through oh264_glue's exact BT.601
 * conversion). "[mp4] ..." markers on serial; exit 0 = ALL PASS. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <toby/video_decode.h>
#include "oh264_mp4.h"

static unsigned fnv1a(const unsigned char *p, int n) {
    unsigned h = 0x811c9dc5u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static int m4(const uint8_t *p, const char *s) {
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] &&
           p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}

/* Recursive ISO-BMFF box finder: first box of `type` within [d, d+n),
 * descending into the container boxes that hold our metadata (never into
 * mdat, which we don't search). Returns the payload pointer + length. */
static int is_container(const uint8_t *t) {
    static const char *c[] = { "moov", "trak", "mdia", "minf", "stbl",
                               "stsd", "avc1", "avc3", "edts", "udta" };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++)
        if (m4(t, c[i])) return 1;
    return 0;
}
static const uint8_t *box_find(const uint8_t *d, long n,
                               const char *type, long *outlen) {
    long o = 0;
    while (o + 8 <= n) {
        uint32_t sz = rd32(d + o);
        const uint8_t *t = d + o + 4;
        long hdr = 8, boxsz;
        if (sz == 1) {                       /* 64-bit largesize */
            if (o + 16 > n) break;
            uint64_t big = ((uint64_t)rd32(d + o + 8) << 32) | rd32(d + o + 12);
            hdr = 16; boxsz = (long)big;
        } else {
            boxsz = sz;
        }
        if (boxsz < hdr || o + boxsz > n) break;
        const uint8_t *pay = d + o + hdr;
        long paylen = boxsz - hdr;
        if (m4(t, type)) { *outlen = paylen; return pay; }
        if (is_container(t)) {
            /* stsd has an 8-byte version/count preamble before entries. */
            long skip = m4(t, "stsd") ? 8 : (m4(t, "avc1") || m4(t, "avc3")) ? 78 : 0;
            if (paylen > skip) {
                const uint8_t *r = box_find(pay + skip, paylen - skip, type, outlen);
                if (r) return r;
            }
        }
        o += boxsz;
    }
    return NULL;
}

/* Emit one NAL as Annex-B (4-byte start code) into buf. */
static int emit_nal(uint8_t *buf, int cap, int p, const uint8_t *nal, int len) {
    if (p + 4 + len > cap) return p;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 1;
    memcpy(buf + p, nal, (size_t)len); return p + len;
}

int main(void) {
    const uint8_t *d = ohm_mp4;
    long n = OHM_MP4_LEN;

    long moovl;
    const uint8_t *moov = box_find(d, n, "moov", &moovl);
    long stbll = 0;
    const uint8_t *stbl = moov ? box_find(moov, moovl, "stbl", &stbll) : NULL;
    if (!stbl) { printf("[mp4] no stbl -- demux FAILED\n"); return 1; }

    /* avcC -> SPS/PPS as Annex-B (vcfg), primed once. */
    long avccl;
    const uint8_t *avcc = box_find(stbl, stbll, "avcC", &avccl);
    if (!avcc || avccl < 7 || (avcc[4] & 3) != 3) {
        printf("[mp4] no/again avcC -- demux FAILED\n"); return 1;
    }
    printf("[mp4] avcC profile_idc=%d (High=100), 4-byte NAL len\n", avcc[1]);
    static uint8_t vcfg[512];
    int vcfg_len = 0, ap = 5;
    int nsps = avcc[ap++] & 0x1f;
    for (int i = 0; i < nsps && ap + 2 <= avccl; i++) {
        int sl = (avcc[ap] << 8) | avcc[ap + 1]; ap += 2;
        if (ap + sl > avccl) break;
        vcfg_len = emit_nal(vcfg, sizeof vcfg, vcfg_len, avcc + ap, sl); ap += sl;
    }
    int npps = (ap < avccl) ? avcc[ap++] : 0;
    for (int i = 0; i < npps && ap + 2 <= avccl; i++) {
        int pl = (avcc[ap] << 8) | avcc[ap + 1]; ap += 2;
        if (ap + pl > avccl) break;
        vcfg_len = emit_nal(vcfg, sizeof vcfg, vcfg_len, avcc + ap, pl); ap += pl;
    }

    /* Sample table: sizes (stsz), sample->chunk (stsc), chunk offsets
     * (stco/co64). Walk samples in DECODE order, recording absolute file
     * offset + size, exactly as the browser's media_demux_mp4. */
    long stszl, stscl, stcol;
    const uint8_t *stsz = box_find(stbl, stbll, "stsz", &stszl);
    const uint8_t *stsc = box_find(stbl, stbll, "stsc", &stscl);
    const uint8_t *stco = box_find(stbl, stbll, "stco", &stcol);
    int co64 = 0;
    if (!stco) { stco = box_find(stbl, stbll, "co64", &stcol); co64 = 1; }
    if (!stsz || !stsc || !stco) { printf("[mp4] no sample table\n"); return 1; }

    uint32_t samp_size0 = rd32(stsz + 4);
    uint32_t nsamp = rd32(stsz + 8);
    uint32_t nsc = rd32(stsc + 4);

    static long samp_off[OHM_NFRAMES * 2];
    static int  samp_len[OHM_NFRAMES * 2];
    int ns = 0;
    uint32_t sc_idx = 0, chunk = 1, samp_in_chunk = 0;
    uint32_t spc = (nsc > 0) ? rd32(stsc + 8 + 4) : 1;
    long chunk_off = (nsc > 0) ? (co64 ? (long)(((uint64_t)rd32(stco + 8) << 32) |
                                                 rd32(stco + 12))
                                       : (long)rd32(stco + 8)) : 0;
    long sample_off = chunk_off;
    uint32_t stsz_p = 12;
    for (uint32_t s = 0; s < nsamp && ns < OHM_NFRAMES * 2; s++) {
        while (sc_idx + 1 < nsc) {
            uint32_t next_first = rd32(stsc + 8 + (sc_idx + 1) * 12);
            if (chunk >= next_first) { sc_idx++;
                spc = rd32(stsc + 8 + sc_idx * 12 + 4);
            } else break;
        }
        uint32_t sz = samp_size0 ? samp_size0 : rd32(stsz + stsz_p);
        stsz_p += 4;
        samp_off[ns] = sample_off; samp_len[ns] = (int)sz; ns++;
        sample_off += sz;
        if (++samp_in_chunk >= spc) {           /* next chunk */
            samp_in_chunk = 0; chunk++;
            uint32_t nchunks = rd32(stco + 4);
            if (chunk <= nchunks) {
                chunk_off = co64
                    ? (long)(((uint64_t)rd32(stco + 8 + (chunk - 1) * 8) << 32) |
                             rd32(stco + 12 + (chunk - 1) * 8))
                    : (long)rd32(stco + 8 + (chunk - 1) * 4);
                sample_off = chunk_off;
            }
        }
    }
    printf("[mp4] demux: %d samples, %dx%d, period=%dms\n",
           ns, OHM_W, OHM_H, OHM_PERIOD_MS);

    /* Build a modifiable copy so we can overwrite the 4-byte AVCC NAL
     * lengths with Annex-B start codes in place (same size, no growth). */
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { printf("[mp4] OOM\n"); return 1; }
    memcpy(buf, d, (size_t)n);
    for (int i = 0; i < ns; i++) {
        long o = samp_off[i]; int left = samp_len[i];
        while (left >= 4) {
            uint32_t nl = rd32(buf + o);
            buf[o] = 0; buf[o + 1] = 0; buf[o + 2] = 0; buf[o + 3] = 1;
            long step = 4 + (long)nl;
            if (nl == 0 || step > left) break;
            o += step; left -= (int)step;
        }
    }

    /* Decode: prime vcfg, feed each sample, then flush held frames --
     * the browser media_pump() sequence. Collect emitted (display-order)
     * ARGB frames and check each against the reference checksum. */
    struct video_decoder *vd = video_decoder_create(VIDEO_CODEC_H264);
    if (!vd) { printf("[mp4] video_decoder_create FAILED\n"); free(buf); return 1; }
    struct video_frame vf;
    if (vcfg_len > 0) { memset(&vf, 0, sizeof vf);
        video_decoder_decode(vd, vcfg, (size_t)vcfg_len, &vf); }

    int shown = 0, pass = 0, fail = 0, dimbad = 0;
    for (int i = 0; i < ns; i++) {
        memset(&vf, 0, sizeof vf);
        int rc = video_decoder_decode(vd, buf + samp_off[i],
                                      (size_t)samp_len[i], &vf);
        if (rc == 1 && vf.data) {
            if (vf.width != OHM_W || vf.height != OHM_H) dimbad++;
            unsigned got = fnv1a(vf.data, vf.width * vf.height * 4);
            unsigned exp = (shown < OHM_NFRAMES) ? ohm_argb_csum[shown] : 0;
            if (got == exp) pass++; else { fail++;
                printf("[mp4] frame %d MISMATCH got=0x%08x exp=0x%08x\n",
                       shown, got, exp); }
            shown++;
        }
    }
    for (;;) {                                  /* drain output-delay */
        memset(&vf, 0, sizeof vf);
        int rc = video_decoder_decode(vd, NULL, 0, &vf);
        if (rc != 1 || !vf.data) break;
        if (vf.width != OHM_W || vf.height != OHM_H) dimbad++;
        unsigned got = fnv1a(vf.data, vf.width * vf.height * 4);
        unsigned exp = (shown < OHM_NFRAMES) ? ohm_argb_csum[shown] : 0;
        if (got == exp) pass++; else { fail++;
            printf("[mp4] frame %d MISMATCH got=0x%08x exp=0x%08x\n",
                   shown, got, exp); }
        shown++;
    }
    video_decoder_destroy(vd);
    free(buf);

    int ok = (shown == OHM_NFRAMES && fail == 0 && dimbad == 0);
    printf("[mp4] High-profile <video> decode: %d/%d frames match, "
           "dims-ok=%d %s\n", pass, OHM_NFRAMES, dimbad == 0,
           ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
