/* user_bsdroute/main.c -- /bin/bsdroute: baseline-decode regression
 * check for media slice 3, in a C program (matching the browser, which
 * is C -- unlike the C++ oh264test). Decodes a QCIF baseline clip
 * (profile_idc 66) through libtoby's video_decoder; openh264 -- the
 * single H.264 backend -- must decode baseline just as it decodes the
 * High-profile clip. "[bsd] ..." markers on serial; exit 0 = OK. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <toby/video_decode.h>
#include "../user_oh264test/oh264_base.h"

static void feed(struct video_decoder *vd, const unsigned char *au, int len,
                 int *frames, int *bad) {
    struct video_frame f;
    memset(&f, 0, sizeof f);
    if (video_decoder_decode(vd, au, (size_t)len, &f) == 1 && f.data) {
        if (f.width == OHB_W && f.height == OHB_H) (*frames)++;
        else (*bad)++;
    }
}

int main(void) {
    struct video_decoder *vd = video_decoder_create(VIDEO_CODEC_H264);
    if (!vd) { printf("[bsd] video_decoder_create FAILED\n"); return 1; }

    int frames = 0, bad = 0, au_start = 0;
    for (int i = 0; i + 3 < OHB_CLIP_LEN; ) {
        int sc = 0, sclen = 0;
        if (ohb_clip[i] == 0 && ohb_clip[i + 1] == 0 && ohb_clip[i + 2] == 1) {
            sc = 1; sclen = 3;
        } else if (i + 4 < OHB_CLIP_LEN && ohb_clip[i] == 0 && ohb_clip[i + 1] == 0 &&
                   ohb_clip[i + 2] == 0 && ohb_clip[i + 3] == 1) {
            sc = 1; sclen = 4;
        }
        if (!sc) { i++; continue; }
        int ntype = ohb_clip[i + sclen] & 0x1f;
        int j = i + sclen;
        while (j + 3 < OHB_CLIP_LEN) {
            if (ohb_clip[j] == 0 && ohb_clip[j + 1] == 0 &&
                (ohb_clip[j + 2] == 1 ||
                 (j + 3 < OHB_CLIP_LEN && ohb_clip[j + 2] == 0 && ohb_clip[j + 3] == 1)))
                break;
            j++;
        }
        int nal_end = (j + 3 < OHB_CLIP_LEN) ? j : OHB_CLIP_LEN;
        if (ntype >= 1 && ntype <= 5) {
            feed(vd, ohb_clip + au_start, nal_end - au_start, &frames, &bad);
            au_start = nal_end;
        }
        i = nal_end;
    }
    video_decoder_destroy(vd);

    int ok = (bad == 0 && frames >= 1);
    printf("[bsd] baseline decode -> openh264: %d frames %dx%d, bad=%d %s\n",
           frames, OHB_W, OHB_H, bad, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
