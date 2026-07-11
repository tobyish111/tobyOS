# openh264 wired into `video_decoder` — the browser's H.264 path (media slice 3)

Branch `openh264-decode` (slice-3 commit, stacked on slice 2). Slices 1
and 2 vendored openh264 and proved it decodes High-profile H.264
bit-exact via a direct `ISVCDecoder`. Slice 3 makes that the **actual
decoder behind libtoby's `video_decoder` API** — the same API the browser
already calls for `<video>` — and returns **ARGB8888** frames (the tobyOS
pixel format), so a High-profile clip now flows all the way to the pixels
the compositor draws.

## What shipped
- **`libtoby/src/oh264_glue.cpp`** + **`libtoby/include/toby/oh264_glue.h`**
  — a tiny C-linkage bridge over openh264's C++ `ISVCDecoder`:
  - `oh264_open()` → `WelsCreateDecoder` + `Initialize`
    (`eEcActiveIdc = ERROR_CON_DISABLE`, `eVideoBsType = VIDEO_BITSTREAM_AVC`),
  - `oh264_decode(sess, nal, len, &argb, &w, &h)` decodes one Annex-B
    access unit; returns `1` when a picture is ready (ARGB, decoder-owned,
    valid until the next call), `0` when the AU was consumed without
    completing a picture, `-1` on error,
  - `oh264_flush(...)` drains a held picture (High profile has output
    delay, so the last frame arrives via a flush),
  - **I420 → ARGB8888** in `emit_argb`: integer BT.601 limited-range
    (`r = clip((298·c + 409·e + 128) >> 8)`, etc.), reading the reported
    `iStride[0]`/`iStride[1]` so the decoder's row padding is stripped.
    The ARGB buffer is grown on demand and kept on the session.
- **`libtoby/src/h264_decode.c`** rewritten to be **openh264-only**:
  `video_decoder_create`/`_decode`/`_destroy` open the glue session
  lazily on the first AU and forward every chunk to it. `nal_len == 0`
  flushes. The `video_frame` ARGB contract is unchanged, so the browser
  and every other caller are untouched.

## Why openh264 for *all* H.264 (baseline included)
The original slice-3 plan was to route by SPS `profile_idc`: baseline
(66) → the lighter `h264bsd`, Main/High → openh264. That routing was
built and worked for High, but **`h264bsd` page-faulted (write to a
read-only page, `#PF err=0x7`) inside its first `h264bsdDecode` whenever
it was co-linked with openh264** — reproduced from both a C++ caller and
a plain C caller (`/bin/bsdroute`), with baseline clips of several sizes,
and with no symbol collision or `malloc`/`free` override found between the
two decoders. Rather than ship a latent fault on the browser's baseline
path, the codec now runs **entirely on openh264**, which decodes baseline
as the subset of the bitstream it already understands — and is the more
robust production decoder regardless. `h264bsd` is no longer referenced by
any active code (its vendored tree stays in the archive but is never
linked in).

## Verified
Built `-DOPENH264_SELFTEST`, booted headless (`-smp 2 -m 512`,
`-display none`). Two programs run at boot:

`/bin/oh264test` (C++) — the 64×64 8-frame **High** clip, both directly
and through `video_decoder`:
```
[oh264] direct ISVCDecoder (YUV): 8/8 ALL PASS      ← bit-exact vs ffmpeg (YUV)
[vdec] profile_idc=100 -> openh264
[oh264] video_decoder (ARGB, routed): 8/8 ALL PASS  ← bit-exact ARGB (BT.601)
[oh264] decode self-test: ALL PASS
[boot] OPENH264: oh264test (pid=2) exit=0 (PASS)
```

`/bin/bsdroute` (C, browser-representative) — a 176×144 QCIF **baseline**
clip (`profile_idc 66`) through `video_decoder`:
```
[vdec] profile_idc=66 -> openh264
[bsd] baseline decode -> openh264: 6 frames 176x144, bad=0 OK
[boot] OPENH264: bsdroute (pid=2) exit=0 (PASS)
```
Baseline decodes cleanly on openh264 with correct dimensions and **no
fault**. The ARGB checksums (`oh_argb_csum` in `oh264_clip.h`) are the
per-frame FNV-1a of the glue's BT.601 output over the same reference YUV,
so the routed ARGB path is checked byte-for-byte too.

The plain build (no `-DOPENH264_SELFTEST`) links and boots unchanged; the
two test programs ship in the initrd but only run under the flag.

## What's next
- **Slice 4** — a real High-profile `<video>` element plays end-to-end in
  the browser (demux → `video_decoder` → compositor), the visible payoff.
- Follow-ups: a B-frame test clip; SIMD (the decoder currently uses the
  C++ fallbacks, no assembly).
