# Browser — `<video>` + `<audio>` (v1)

Branch `browser-media`, stacked on `browser-idb`. The browser plays
media: `<video>` renders moving frames into the page and `<audio>`
plays real MP3 through the kernel audio engine. This lands the media
*pipeline* — element → async fetch → demux → decode → present, all
driven from the existing cooperative main loop — with deliberately
modest v1 codecs (see limits).

## What shipped
- **Real minimp3** — `third_party/minimp3.h` was a stub whose header
  said "swap this file for the real minimp3.h"; done (lieff/minimp3,
  CC0, 1.9k lines, no libm). `libtoby/src/mp3_decode.c` now defines
  `MINIMP3_IMPLEMENTATION`, so `toby_mp3_decode_frame()` yields real
  PCM (it also un-stubs the standalone `media.c` player for free).
  Builds with `-msse -msse2` (float synthesis + SSE2 intrinsics),
  matching the stb_image/stb_truetype precedent.
- **Browser (`main.c`)**:
  - `<video src>` / `<audio src>` register a per-tab media element in
    `collect_node`; `<video>` gets an ARGB backing store (the canvas
    pattern — lays out and paints as an image, opaque black until
    frames arrive); UA sheet: `video{display:inline-block}`,
    `audio{display:none}`. `autoplay` and `loop` attributes honored;
    `el.play()`/`el.pause()` from JS via a `mediaCtl` prim.
  - The whole file downloads through the async-HTTP worker (one media
    transfer in flight, the image-loader pattern; 4 MiB cap).
  - **Video = MJPEG-in-AVI**: a ~90-line RIFF demuxer indexes the
    `##dc` chunks and reads the frame period from `avih`; each due
    tick decodes ONE JPEG frame (stb JPEG via `toby_image_load`) and
    nearest-blits it into the backing store, then repaints — no
    relayout, and the schedule rebases rather than spiraling when
    decode is slower than the nominal fps.
  - **Audio = MP3**: decoded incrementally into the kernel audio
    engine (`SYS_AUDIO_OPEN/WRITE`, PCM16); the stream opens lazily
    with the rate/channels the first decoded frame reports, and the
    pump throttles on the ring's non-blocking write (leftover PCM
    carries to the next tick). Only the active tab's media advances.
  - Cleanup mirrors images: media freed on navigate/close, in-flight
    fetch cancelled before tab shifts.

## Verified (QEMU, `/media` test page)
- A 40-frame 320×240 MJPEG AVI (10 fps, autoplay loop) **plays on
  screen**: three screenshots ~2 s apart show frame 01 → 03 → 05 with
  the ball orbiting and the progress bar growing (TCG emulation
  throttles JPEG decode below nominal fps; the mechanism rebases and
  keeps advancing).
- A 4 s 128 kbps MP3 (autoplay) decodes through real minimp3: serial
  shows `[med] audio bytes 64783` → `[audio] stream 0 opened:
  rate=44100 ch=2 fmt=1` — the decoder's true parameters — with the
  QEMU intel-hda device draining the DMA ring and the taskbar volume
  indicator lighting up.
- `[med] video frames/period 40 100` confirms the demuxer indexed
  every frame with the right period.
- Regression: the `/canvas` page (same backing-store paint path) is
  unchanged.

## v1 limits
- Codecs: MJPEG video and MP3 audio only. H.264 (`h264_decode.c` is
  still the placeholder) and AAC/Opus/WebM are the known next step —
  the pipeline (fetch/demux/schedule/present) is codec-agnostic.
- No muxed A/V sync: a `<video>`'s embedded audio track is ignored;
  `<audio>` is a separate element and free-runs.
- Whole-file download before playback (4 MiB cap) — no streaming.
- No controls UI, no `currentTime`/`duration`/events (`play`/`pause`
  and `autoplay`/`loop` only); background tabs pause.
- 2 media elements per tab; 512 frames per video.
