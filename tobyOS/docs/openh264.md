# openh264 High-profile H.264 — freestanding build (media slice 1)

Branch `openh264-build`, stacked on main (post the 5th/QUIC merge). The
first slice of High-profile H.264 support: vendor Cisco's **openh264**
decoder and get the whole thing **compiling freestanding into libtoby**.
This is the biggest de-risking step — a from-scratch OS building a
production H.264 decoder as freestanding C++ — before any decode/
integration work.

## Why openh264
The existing video path uses **h264bsd** (baseline profile only —
CAVLC, no CABAC, no B-frames). Most real web H.264 is **High profile**
(CABAC arithmetic coding, 8×8 transforms, B-frames, weighted
prediction), which h264bsd cannot decode. openh264 is a full profile
decoder; it's C++, so it's unblocked by the userland C++ runtime that
landed earlier this stage (`docs/cpp-runtime.md`).

## What shipped
- **Vendored `third_party/openh264/`** — the decoder subset of openh264
  v2.4.1 (BSD-2): `api/` (public ISVCDecoder), `common/inc`+`src`,
  `decoder/core/inc`+`src`, `decoder/plus/src/welsDecoderExt.cpp`
  (~37 C++ files). The arch-specific assembly (`x86/`, `arm/`, …) is
  **not** vendored: openh264 selects C++ fallbacks when CPU-feature
  detection reports nothing, so the pure-C++ paths are used and no NASM
  is needed.
- **Freestanding shims** (`third_party/openh264/shim/`) — libtoby
  already has real pthreads, so only the genuinely missing OS bits are
  shimmed: `semaphore.h` (single-threaded `sem_t` stubs + the few
  pthread attr/name functions libtoby's `<pthread.h>` lacks) and empty/
  trivial `sys/param.h` + `sys/sysctl.h` (the CPU-count query falls back
  to `ProcessorCount = 1`). libtoby is left untouched.
- **Single-threaded subset** — the four multi-threaded files
  (`WelsThreadPool`, `WelsTaskThread`, `WelsThread`,
  `wels_decoder_thread`) are excluded: they carry a global constructor
  and are only used at `threadCount > 1`, which the tobyOS integration
  never enables (`DecodeFrameNoDelay`). The kept set needs only
  `WelsMutex*` (no-op single-threaded).
- **Makefile** — the 33 kept files build with `clang++` +
  `LIBTOBY_CXXFLAGS` (`-std=c++17 -fno-exceptions -fno-rtti -nostdinc++`,
  the cpp-runtime flags) + `-msse -msse2`, into both `libtoby.a` and
  `libtoby.so`. **Archive semantics** mean a program that never calls the
  H.264 path does not pull these objects, so this is inert until
  `video_decode.c` routes to it — the browser and every other libtoby
  program link and build unchanged.

## Verified
- All 33 kept files **compile clean freestanding** (no STL — it's
  C-style C++ with classes — so `-nostdinc++` suffices).
- A full `make iso` builds `libtoby.a` + `libtoby.so` with the openh264
  objects included, and the whole system (browser, every userland app)
  still links and the ISO builds — proving the vendored decoder is
  inert and non-disruptive.

## What's next
- **Slice 2** — a decode self-test: link a program against the openh264
  `ISVCDecoder`, feed it a known High-profile Annex-B clip, and check
  the decoded YUV/RGB against a reference (this is where any remaining
  link gaps + decode correctness — CABAC, reference management,
  deblocking — get shaken out).
- **Slice 3** — wire into libtoby's `video_decoder`: sniff the SPS
  `profile_idc` and route baseline → h264bsd, High → openh264 (or use
  openh264 for both), keeping the existing `video_frame` ARGB contract.
- **Slice 4** — browser end-to-end: a High-profile `<video>` plays.

## v1 scope
This slice is the freestanding **build** only — openh264 compiles into
libtoby but nothing calls it yet. Decoding, correctness verification,
and integration are the following slices. Single-threaded decode; no
assembly (C++ fallbacks).
