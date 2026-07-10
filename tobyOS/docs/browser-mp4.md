# Browser — H.264 in MP4 (`<video>` container demux)

Branch `browser-mp4`, stacked on the QUIC-crypto tip. Extends the
stage-13 media pipeline: `<video>` now plays **H.264 in an MP4
(ISO base media file format) container** — the format almost all web
video actually ships in — not just the H.264-in-AVI test container.
The real h264bsd decoder (already merged) does the pixels; this adds
the demuxer that feeds it.

## What shipped
- **`media_demux_mp4()`** (`programs/user_gui_browser/main.c`) — a
  focused ISO-BMFF box-tree walker:
  - `mp4_find()` iterates the length-prefixed boxes at one level
    (`size`/`type`, 64-bit `largesize`, size-0-to-EOF), used to
    descend `moov → trak → mdia → minf → stbl`.
  - Selects the **video track** by its `hdlr` handler type (`vide`)
    and reads the media timescale from `mdhd`; frame period comes from
    the first `stts` entry (constant-fps assumption).
  - Pulls **SPS/PPS out of `avcC`** (the AVCDecoderConfigurationRecord
    inside `stsd → avc1`) and emits them as an Annex-B blob that
    primes the decoder before the first frame.
  - Reconstructs each **sample's file offset + size** from the sample
    table — `stsc` (sample-to-chunk runs), `stco`/`co64` (chunk file
    offsets), `stsz` (per-sample sizes) — the standard walk.
  - Converts each sample's **length-prefixed NALs to Annex-B in
    place**: a 4-byte AVCC length overwrites cleanly with a
    `00 00 00 01` start code, so no copy and the existing H.264 pump
    path plays it verbatim.
- **`media_ready()`** now sniffs the `ftyp` box first (`ftyp` at
  offset 4) and routes MP4 to the new demuxer + h264bsd, feeding the
  avcC SPS/PPS once at setup; the AVI/MJPEG and MP3 paths are
  unchanged and fall through.
- Container/codec support: MP4 with H.264 (avc1/avc3), 4-byte NAL
  lengths (`lengthSizeMinusOne == 3`, the near-universal case).

## Verified (QEMU, `/mp4` test page)
- A 40-frame 320×240 **baseline H.264 MP4** (x264 `-profile baseline`,
  `+faststart`, keyint 10 — mostly inter-predicted frames) plays and
  loops on screen: screenshots ~2.5 s apart show the frame counter
  advancing with the ball arcing and the progress bar growing — every
  pixel out of the real bitstream, demuxed from the MP4 sample table.
- Serial: `[med] mp4/h264 frames/period 40 100` (demux + timing).
- Regression: the `/h264` (H.264-in-AVI) and `/media` (MJPEG + MP3)
  pages still play.

## v1 limits
- H.264 video track only (baseline profile, via h264bsd); the audio
  track (`mp4a`/AAC) is ignored — no muxed A/V.
- 4-byte NAL length prefix only (`avcC` lengthSizeMinusOne 3).
- Constant frame rate (first `stts` delta); no edit lists, no B-frame
  reordering (baseline has none), no fragmented MP4 (`moof`/`traf`).
- Whole-file download before playback (the media pipeline's 4 MiB
  cap), progressive `+faststart` layout assumed (moov before mdat is
  not required since the whole file is in memory).
