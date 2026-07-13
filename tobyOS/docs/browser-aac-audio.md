# AAC-LC audio decode (Helix) — browser `<audio>`/`<video>` audio track

Adds a real AAC-LC decoder to tobyOS so the browser can play AAC audio —
both standalone ADTS streams (`audio/aac`) and the AAC track carried inside
MP4 (`mp4a`, alongside the openh264 H.264 video path). Backend is the
vendored **Helix AAC** fixed-point decoder; it is wired to libtoby's
`toby_aac_decode_*` API and routed through `media.c`.

## What works

- `/bin/aactest` decodes an embedded 0.1 s 440 Hz sine ADTS clip (mono,
  44100 Hz, 128 kbit) frame-by-frame and verifies the PCM is plausible.
  Boot self-test (`-DAAC_SELFTEST`) result, verified in QEMU:

  ```
  [aac] Helix AAC decode test: 1868 bytes ADTS
  [aac] decoded 6 frames, 6144 samples/chan, 1 ch, 44100 Hz
  [aac] tone: RMS=2730 (ref~2492), zero-crossings=94 (ref~272)
  [aac] decode self-test: ALL PASS
  [boot] AAC: aactest (pid=2) exit=0 (PASS)
  ```

- `media.c` now detects an ADTS AAC stream (`CONTAINER_AAC`) by content-type
  (`audio/aac`, `audio/aacp`) or by the ADTS syncword, and decodes it via
  `decode_aac_audio()` to PCM out through the audio ABI.

## Why not bit-exact

AAC decode is **not** bit-exact across implementations: Helix is fixed-point,
ffmpeg is float. The self-test therefore checks signal *characteristics*
(channel count, sample rate, plausible sample count, non-silence, and a
~440 Hz tone via RMS + zero-crossings) rather than a byte checksum. This
mirrors how the openh264 path is verified against ffmpeg for the video side,
except AAC can't be made bit-exact even in principle.

## Pieces

- `third_party/libhelix-aac/` — vendored Helix AAC-LC (28 `.c`). Freestanding
  shims were added because the upstream drop assumes an embedded build system:
  - `ConfigHelix.h` — enables the MPEG/SBR feature macros; includes
    `utils/helix_pgm.h`.
  - `utils/helix_pgm.h` — `PROGMEM`/`pgm_read_*` no-ops (we're not on AVR).
  - `hlxclib/stdlib.h` — re-exports `<stdlib.h>` + `<string.h>`.
  - `utils/helix_memory.h` — `helix_malloc`/`helix_free` → `malloc`/`free`.
  - `sbr.c` needs `#include <stdio.h>` (it calls `printf` on OOM) after its
    `hlxclib` include.

- `libtoby/src/mp3_decode.c` — `toby_aac_decode_*` (was a stub) now drives
  Helix: `struct toby_aac_decoder { HAACDecoder h; int raw_configured; }`.
  - `toby_aac_decode_frame()` — `AACFindSyncWord` → `AACDecode` →
    `AACGetLastFrameInfo`; returns **input bytes consumed** so a caller can
    walk an ADTS stream.
  - `toby_aac_decode_set_raw(profile, rate, channels)` — `AACSetRawBlockParams`
    for the raw (ADTS-less) AUs that MP4 `mp4a` carries. Call once before
    decoding raw AUs.

- `libtoby/include/toby/audio_decode.h` — `toby_aac_decode_set_raw` declared;
  `toby_aac_decode_frame` documented to return consumed bytes.

- `libtoby/src/media.c` — `CONTAINER_AAC` (5); `struct media_player.aac_dec`;
  `detect_container()` routes `audio/aac` + ADTS syncword to it (checked
  **before** the looser MP3 sync, since both begin `0xFF`);
  `decode_aac_audio()`; `aac_dec` freed in cleanup.

- `programs/user_aactest/` — `/bin/aactest` + embedded `aac_clip.h`
  (generated from an ffmpeg-encoded sine, refs baked in).

## Known follow-ups

- **MP4-embedded AAC needs demux.** `CONTAINER_MP4` allocates an `aac_dec`,
  but `media_player_play()` only auto-plays the standalone ADTS path today.
  Pulling `mp4a` AUs out of the MP4 sample tables and feeding them via
  `toby_aac_decode_set_raw` + `toby_aac_decode_frame` is the remaining wiring
  to get audio on an MP4 `<video>`.
- **SBR / HE-AAC** feature macros are enabled in `ConfigHelix.h` but not
  exercised by the LC self-test.

## Build / test

```
# self-test at boot
make ... EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DAAC_SELFTEST" iso
# then boot; look for "[boot] AAC: aactest (pid=2) exit=0 (PASS)"
```

The `aactest` program is in the Makefile `PROGRAMS` list, the initrd `cp`
list, and the ISO tar list. The Helix sources compile via
`HELIXAAC_DIR` rules; `LIBTOBY` links the resulting objects.
