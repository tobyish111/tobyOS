/* user_av1test/main.cpp -- /bin/av1test: decode-correctness test for the
 * vendored libgav1 AV1 decoder (libgav1 slice 2). Links libgav1's
 * Decoder out of libtoby.a, decodes an embedded AV1 keyframe (a raw OBU
 * temporal unit), and checks the reconstructed YUV against ffmpeg's
 * reference decode of the same stream. AV1 reconstruction is bit-exact
 * by spec, so the FNV-1a in av1_clip.h is a strict oracle. Single-
 * threaded (threads=1). "[av1] ..." on serial; exit 0 = PASS. This is the
 * first program to pull libgav1 out of libtoby.a, so it also proves the
 * decoder links + runs. */

#include <cstdio>
#include <cstdint>
#include <cstddef>

#include "gav1/decoder.h"
#include "av1_clip.h"

static unsigned fnv1a_upd(unsigned h, const unsigned char *p, int n) {
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}

extern "C" int main(void) {
    libgav1::Decoder decoder;
    libgav1::DecoderSettings settings;
    settings.threads = 1;
    settings.frame_parallel = false;
    settings.blocking_dequeue = true;

    if (decoder.Init(&settings) != libgav1::kStatusOk) {
        std::printf("[av1] Decoder::Init FAILED\n");
        return 1;
    }
    std::printf("[av1] decoder up; max bitdepth=%d; OBU %u bytes\n",
                libgav1::Decoder::GetMaxBitdepth(), AV1_OBU_LEN);

    libgav1::StatusCode s = decoder.EnqueueFrame(av1_obu, AV1_OBU_LEN,
                                                 /*user_private_data=*/0,
                                                 /*buffer_private_data=*/nullptr);
    if (s != libgav1::kStatusOk) {
        std::printf("[av1] EnqueueFrame FAILED (status=%d)\n", (int)s);
        return 1;
    }

    const libgav1::DecoderBuffer *buffer = nullptr;
    s = decoder.DequeueFrame(&buffer);
    if (s != libgav1::kStatusOk || buffer == nullptr) {
        std::printf("[av1] DequeueFrame FAILED (status=%d, buf=%p)\n",
                    (int)s, (const void *)buffer);
        return 1;
    }

    int w = buffer->displayed_width[0], h = buffer->displayed_height[0];
    int nplanes = buffer->NumPlanes();
    std::printf("[av1] decoded frame %dx%d, bitdepth=%d, planes=%d, fmt=%d\n",
                w, h, buffer->bitdepth, nplanes, (int)buffer->image_format);

    int ok_dims = (w == AV1_W && h == AV1_H && buffer->bitdepth == 8 &&
                   buffer->image_format == libgav1::kImageFormatYuv420);

    /* Tight Y + U + V (strip per-plane stride padding), in the same order
     * as ffmpeg's yuv420p reference. */
    unsigned csum = 0x811c9dc5u;
    for (int p = 0; p < nplanes; p++) {
        int pw = buffer->displayed_width[p];
        int ph = buffer->displayed_height[p];
        int st = buffer->stride[p];
        const unsigned char *plane = buffer->plane[p];
        for (int row = 0; row < ph; row++)
            csum = fnv1a_upd(csum, plane + (size_t)row * st, pw);
    }

    int ok_csum = (csum == AV1_YUV_CSUM);
    std::printf("[av1] YUV csum=0x%08x exp=0x%08x %s; dims %s\n",
                csum, AV1_YUV_CSUM, ok_csum ? "MATCH" : "MISMATCH",
                ok_dims ? "OK" : "BAD");

    decoder.SignalEOS();

    int ok = ok_dims && ok_csum;
    std::printf("[av1] decode self-test: %s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
