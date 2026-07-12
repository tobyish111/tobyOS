# High-profile H.264 `<video>` end-to-end in the browser (media slice 4)

Branch `browser-h264-high`, stacked on `openh264-decode` (slices 1-3).
Slice 3 made openh264 the decoder behind libtoby's `video_decoder`;
slice 4 drives a **High-profile H.264-in-MP4 clip all the way through the
browser's `<video>` path** -- MP4 (ISO-BMFF) demux -> `video_decoder`
(openh264) -> ARGB8888 -> the compositor -- and proves it decodes
bit-exactly against ffmpeg.

## What shipped
- **Browser pump (`media_pump` in `user_gui_browser/main.c`) drains the
  decoder at end-of-stream.** openh264 has a ~1-frame output delay (it
  buffers a picture before releasing it), so the old pump -- which wrapped
  or stopped the instant `cur_frame` reached `nframes` -- dropped the last
  frame and glitched the loop boundary. The H.264 branch now feeds samples
  in stream order until a picture lands, and once the samples run out it
  **flushes with `video_decoder_decode(vd, NULL, 0, ...)`** to release the
  held picture(s); it only wraps (loop) or stops once the decoder reports
  fully drained. The MJPEG path is unchanged. (The demux, `media_ready`
  priming with the avcC SPS/PPS, and the `media_blit_raw` ARGB compositing
  are all unchanged from the baseline/MJPEG video slices.)
- **`/bin/mp4play`** (`programs/user_mp4play/`) -- a boot self-test that
  runs the exact browser decode path on an embedded High-profile clip:
  a compact ISO-BMFF demux (recursive box walk -> avcC SPS/PPS as
  Annex-B, `stsz`/`stsc`/`stco` sample walk, 4-byte NAL lengths rewritten
  to Annex-B start codes in place) feeds `video_decoder` sample-by-sample
  and then flushes, matching each produced ARGB frame's FNV-1a against the
  reference. Same feed-then-flush sequence as `media_pump`.
- **`oh264_mp4.h`** -- a 96x96, 16-frame clip, libx264 `-profile:v high
  -bf 0` (CABAC + 8x8 transform, the High-profile features baseline /
  h264bsd cannot do) with P-frame inter-prediction, plus the per-frame
  reference checksums: ffmpeg's decode of that exact stream, run through
  `oh264_glue`'s exact integer BT.601 conversion (so the on-device ARGB
  is checked byte-for-byte, not just the YUV). Generated with
  imageio-ffmpeg; script in the scratchpad.

## Verified
Built `-DOPENH264_SELFTEST`, booted headless (`-smp 2 -m 512`,
`-display none`):
```
[mp4] avcC profile_idc=100 (High=100), 4-byte NAL len
[mp4] demux: 16 samples, 96x96, period=40ms
[vdec] profile_idc=100 -> openh264
[mp4] High-profile <video> decode: 16/16 frames match, dims-ok=1 ALL PASS
[boot] OPENH264: mp4play (pid=2) exit=0 (PASS)
```
Every frame of the High-profile MP4 is bit-identical to the ffmpeg
reference through the full container -> decoder -> ARGB chain. The
existing `oh264test` (8/8 High) and `bsdroute` (baseline QCIF) self-tests
still pass. Plain build (no flag) links and boots unchanged; `mp4play`
ships in the initrd but only runs under the flag.

## A note on B-frames (known limitation)
An earlier revision of this clip used `-bf 2` (B-frames). openh264's
**decoder** diverged from ffmpeg on a couple of B-frames (2-3 of 16,
different frames depending on encoder options; disabling weighted
prediction did not fix it) -- an accuracy gap in openh264's B-frame
reconstruction (Cisco's openh264 is encoder-focused and its encoder does
not even emit B-frames, so its B-decode path is lightly exercised). The
non-B-frame High path is bit-exact (this slice and slice 2). The test
clip therefore uses `-bf 0`; it still exercises the defining High-profile
tools (CABAC, 8x8 transform) plus inter-prediction. Bit-exact B-frame
decode is a future item (a different decoder, or an openh264 fix).

## On-screen proof (the visible payoff)
Captured live: the browser fetched a host test page over SLIRP
(`http://10.0.2.2:8099/`) with a `<video src="/rig.mp4">` (a 240x160,
40-frame High-profile clip), and the frames decode + play on screen.
Serial confirms the real browser path -- `[http] 200 OK ... type="video/
mp4"`, `[vdec] profile_idc=100 -> openh264`, `[med] mp4/h264 frames/period
40` -- and two screenshots ~15 s apart show different decoded frames
(frame 10 then frame 22, the burned-in counter advancing), i.e. the clip
is playing, not a still.

Reproduce: a host web server serves the page + mp4 on `127.0.0.1:8099`;
build with `-DTKAPP_BOOT -DTKAPP_MP4VIDEO` (a boot selector in `kernel.c`
that launches `/bin/gui_browser` and types the URL -- baked in-source so
MSYS doesn't path-mangle the slashes); boot QEMU with user networking and
QMP, wait for the guest to GET `/rig.mp4`, then screendump. (The headless
TKAPP desktop bring-up is intermittently flaky -- re-run a couple of times
for a clean composite; `-smp 1` is steadier.)

## What's next
- libgav1 / AVIF (the next codec on the list), then persisted Alt-Svc and
  the EliteDesk pass.
- Bit-exact B-frame decode (see the limitation above).
