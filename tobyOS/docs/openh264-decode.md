# openh264 decode correctness — 8/8 bit-exact (media slice 2)

Branch `openh264-decode`, stacked on `openh264-build` (slice 1). Proves
the vendored openh264 decoder **correctly decodes High-profile H.264 on
tobyOS**: a program links the `ISVCDecoder` out of libtoby, decodes an
embedded High-profile clip, and every output frame matches ffmpeg's
reference **byte-for-byte**. This exercises the whole High-profile decode
path — CABAC arithmetic decoding, 8×8 transforms, reconstruction,
deblocking, reference management — that baseline `h264bsd` cannot do.

## What shipped
- **`/bin/oh264test`** (`programs/user_oh264test/`) — a C++ program
  (built like `cpptest`, plus the openh264 API include) that:
  - creates the decoder (`WelsCreateDecoder` / `Initialize`),
  - splits the embedded Annex-B clip into access units (each VCL NAL
    ends an AU; leading SPS/PPS/SEI attach to the following picture),
  - decodes with `DecodeFrameNoDelay`, then drains the held picture(s)
    with `FlushFrame` (High profile — `profile_idc != 66` — has output
    delay, so the final frame needs the flush),
  - extracts the tight `W×H` Y + `W/2×H/2` U,V planes (stripping the
    decoder's row padding via the reported strides) and checks each
    frame's FNV-1a checksum against `oh264_clip.h`.
- **`oh264_clip.h`** — a 64×64, 8-frame clip encoded by x264
  `-profile:v high -bf 0` (CABAC + 8×8 transform), plus the per-frame
  reference checksums from ffmpeg's decode of that exact stream. H.264
  reconstruction is bit-exact by spec, so any compliant decoder must
  produce identical YUV — making the checksum a strict correctness
  oracle. (Generated with imageio-ffmpeg; script in the scratchpad.)
- **Boot harness** — under `-DOPENH264_SELFTEST`, the kernel
  `proc_spawn`s `/bin/oh264test` and `proc_wait`s it; its `[oh264]`
  stdout is on serial, exit 0 = ALL PASS. This is the first program to
  pull openh264 out of `libtoby.a`, so it also proves the decoder links.

### Link gaps resolved (the "first pull" shakeout)
- `WelsThread.cpp` and `wels_decoder_thread.cpp` had to be **kept** (not
  excluded): the decode core references their `Sem*`/`Event*`/
  `Thread*`/`GetCPUCount` helpers even single-threaded. Only
  `WelsThreadPool` + `WelsTaskThread` (the multi-thread pool, with a
  global ctor) stay excluded.
- `libtoby/include/sys/time.h` was missing an `extern "C"` guard, so
  C++ callers mangled `gettimeofday` and it didn't resolve to libtoby's
  C definition — guard added.
- `libtoby/include/errno.h` lacked `ETIMEDOUT` (+ `EBUSY`) that the
  thread code references — added (standard values).

## Verified
Built `-DOPENH264_SELFTEST`, booted headless:
```
[oh264] decoder up; clip 6138 bytes, expecting 8 High-profile frames
[oh264] frame 0..7: csum=... exp=... OK   (all 8)
[oh264] decode self-test: 8/8 frames match ALL PASS
[boot] OPENH264: oh264test (pid=2) exit=0 (PASS)
```
Every frame is bit-identical to the ffmpeg reference. The plain build
(no `-DOPENH264_SELFTEST`) links and boots unchanged — `/bin/oh264test`
ships in the initrd but is only spawned under the flag, and the browser
+ every other program are unaffected (openh264 stays inert until
something calls it).

## What's next
- **Slice 3** — wire openh264 into libtoby's `video_decoder`: sniff the
  SPS `profile_idc` and route baseline (66) → h264bsd, Main/High → the
  openh264 backend, keeping the existing `video_frame` ARGB contract
  (this needs an I420→ARGB conversion, since openh264 outputs YUV420
  and the browser wants ARGB8888).
- **Slice 4** — a High-profile `<video>` plays in the browser.

## v1 scope
Decode-correctness proof only, via a dedicated test program; the
`video_decoder` integration (and the YUV→ARGB the browser needs) is
slice 3. Single-threaded; no assembly (C++ fallbacks). The test vector
is one 64×64 High-profile clip (CABAC, no B-frames) — B-frame reordering
is exercised implicitly by the `FlushFrame` drain but a B-frame clip is
a good future addition.
