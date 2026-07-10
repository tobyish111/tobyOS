# Browser — real H.264 `<video>` decode

Branch `browser-h264`, stacked on `browser-media`. The media pipeline
landed codec-agnostic with MJPEG as its v1 video codec; this replaces
the toy codec with **real H.264** (baseline profile) — actual inter +
intra prediction, CAVLC, deblocking — via the vendored **h264bsd**
decoder. The `<video>` element now plays what ffmpeg/x264 emits.

On the "other languages are allowed" question: openh264 (C++) would
buy High-profile support but first needs a freestanding C++ runtime
for tobyOS userspace (new/delete, static-init in the ELF loader,
no-exceptions build of a 40k-line codebase) — that's the documented
path to High profile, not the first step. h264bsd is pure integer C
and plays real H.264 today.

## What shipped
- **Vendored h264bsd** (`third_party/h264bsd/`, Apache-2.0, Android
  AOSP-derived): the complete baseline-profile decoder — NAL/slice
  parsing, CAVLC, intra + inter prediction (motion compensation),
  deblocking filter, DPB — 26 pure-integer C TUs. Compiles against
  libtoby's freestanding libc with zero patches (one new
  `libtoby/include/memory.h` alias header for its `<memory.h>`
  include).
- **`libtoby/src/h264_decode.c`** — the placeholder ("produces a
  solid-colour placeholder frame") is gone; the existing
  `video_decoder_*` API is now backed by h264bsd. Feed Annex-B
  chunks; a completed picture returns as **ARGB8888 directly** —
  h264bsd's "BGRA" converter packs `0xFF<<24|r<<16|g<<8|b`, which is
  exactly the tobyOS pixel format, so no browser-side conversion.
  Returns 1 = picture ready (decoder-owned buffer, mb-aligned
  dimensions), 0 = consumed without a picture (parameter sets),
  -1 = stream error.
  **Calling-protocol gotcha (cost a debug cycle):** on
  `H264BSD_HDRS_RDY` (and the two-phase paths) `h264bsdDecode`
  deliberately reports `readBytes == 0` and expects the SAME buffer
  again — it resumes internally via `prevBytesConsumed`. Treating a
  zero read as "no progress" and bailing meant the PPS/IDR behind
  the SPS never decoded and every later slice failed on missing
  parameter sets (black video). The wrapper's loop honors the
  resume protocol, bounded by an iteration guard + zero-read streak
  counter.
- **Browser** — the AVI path sniffs the first video chunk: an
  Annex-B start code selects the H.264 decoder (stateful, per
  element), `FF D8` keeps the MJPEG path. On each due tick the pump
  feeds chunks until a picture lands (parameter-set chunks yield
  none), then blits it into the element's backing store. Loop replay
  re-feeds from chunk 0 (SPS/PPS + IDR), which resets the decoder's
  references naturally. Everything else — async fetch, scheduling,
  autoplay/loop, play()/pause(), teardown — is the stage-13 media
  pipeline unchanged.

## Verified (QEMU, `/h264` test page)
- A 40-frame 320×240 baseline H.264 AVI (x264, `-profile:v
  baseline`, keyint 10, autoplay loop) plays on screen: screenshots
  show frame 25/40 (ball mid-arc, progress bar ~60%) and then frame
  01/40 after the loop wrapped — every pixel from the real bitstream,
  inter-predicted frames included (only 4 of 40 frames are IDR).
  Every chunk decodes to a picture (h264bsd runs faster than the
  per-frame stb JPEG path).
- Serial: `[med] h264 frames/period 40 100` (codec sniff + demux).
- Regression: the `/media` MJPEG+MP3 page still plays (frame counter
  advancing, audio stream open at 44100/2).

## v1 limits
- Baseline profile only (h264bsd's scope): no CABAC, no B-frames, no
  High-profile 8x8 transforms. Most camera/x264-baseline content
  works; typical web MP4s are High profile — that's the
  openh264/C++ step.
- Container: H.264-in-AVI (Annex-B chunks). MP4 (`avcC` +
  length-prefixed NALs → Annex-B conversion + `stbl` demux) is the
  natural next container.
- Picture dimensions are macroblock-aligned (frame cropping is not
  applied); encode at multiples of 16 (e.g. 320×240) for exact size.
- Decoded-picture buffer is the decoder's; the pump blits it
  immediately, so no frame queue.
