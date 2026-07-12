# libgav1 (AV1 / AVIF) — freestanding build on tobyOS (slice 1)

Branch `libgav1`, stacked on main (after the C++ STL merge). AVIF is an
AV1-encoded still image in an ISO-BMFF container; the hard part is a real
AV1 decoder. This slice **vendors Google's libgav1 decode subset and
compiles all of it freestanding on tobyOS** — the payoff of the C++ STL
bring-up (libgav1, unlike openh264, uses the STL heavily). No decode yet;
that is slice 2.

## What shipped
- **`third_party/libgav1/`** — the decode subset of libgav1 (BSD-3):
  `src/*.cc` (20), `src/dsp/*.cc` (23, the **C** reference implementations),
  `src/tile/`, `src/utils/`, `src/post_filter/`, all the `.inc` data
  tables, the `src/gav1/` public API, and the `src/dsp/{arm,x86}/` **header**
  stubs (the dsp headers `#include` them unconditionally; their bodies are
  guarded by the ENABLE macros, so they compile empty). **No** arch SIMD
  `.cc`, no tests, no examples. 61 `.cc` total (2.1 MB).
- **Build config** (Makefile `LIBGAV1_CFG`): `LIBGAV1_MAX_BITDEPTH=8`
  (8-bit AV1/AVIF), `LIBGAV1_ENABLE_{NEON,SSE4_1,AVX2}=0` (C fallbacks
  only — SIMD dispatch compiles out, so the arch dirs aren't needed),
  `LIBGAV1_THREADPOOL_USE_STD_MUTEX=1` (**the key to dropping abseil**: the
  only abseil user, `threadpool.h`, switches to `std::mutex`/
  `condition_variable` — which the STL now provides), `LIBGAV1_ENABLE_LOGGING=0`.
  Single include root (`-I third_party/libgav1`, everything is `src/...`).
  `-msse -msse2` (FP returns) + `-w` (vendored). Objects go into
  `libtoby.a`; **archive semantics keep libgav1 inert** until the image
  loader routes to it (a full `iso` builds + links unchanged, no undefined
  symbols pulled).

## STL gaps this "first pull" shook out (all filled)
libgav1 exercised more of the standard library than existed; added:
- `std::align_val_t` + the over-aligned `operator new`/`delete` set
  (`<new>` + `cxxrt.cpp`) — `MaxAlignedAllocable` uses aligned new.
- `std::shared_ptr` / `weak_ptr` (`.lock()`) / `make_shared` with an
  atomic-refcounted control block (`<memory>`) — `RefCountedBufferPtr`.
- `posix_memalign` in libtoby's `<stdlib.h>` + `stdlib.c` (`memory.h`'s
  `AlignedAlloc`; malloc-backed since malloc is 16-aligned and SIMD is off,
  so the pointer stays `free()`-able as `AlignedFree` requires).
- `std::once_flag` / `call_once` (`<mutex>`) — the dsp init-once.
- `std::partial_sort` (`<algorithm>`) — motion-vector candidate sort.
- `std::abs(long)` / `abs(long long)` (`<cstdlib>`) — else a 64-bit arg is
  ambiguous against the `<cmath>` float `abs`.
- a new `<numeric>` (`accumulate`/`iota`/`inner_product`/`partial_sum`).

## Verified
`make iso` builds every libgav1 `.cc` (61/61 `.o` + 61/61 PIC `.o`), the
kernel and all programs still link, and the ISO is produced — libgav1 is
compiled but not yet called.

## Slice 2 — decode correctness (DONE, bit-exact)
`/bin/av1test` (`programs/user_av1test/`) links libgav1's `libgav1::Decoder`
out of `libtoby.a`, decodes an embedded AV1 keyframe, and matches its
YUV against ffmpeg's reference. AV1 reconstruction is deterministic by
spec, so the FNV-1a in `av1_clip.h` is a strict oracle.
- Flow: `Init` (`threads=1`, `frame_parallel=false`, `blocking_dequeue=true`)
  → `EnqueueFrame(obu, len, 0, nullptr)` → `DequeueFrame(&buffer)` →
  read `buffer->plane[]/stride[]/displayed_width/height/bitdepth`, tighten
  Y+U+V (strip stride), FNV-1a.
- Test vector: a 96×96 AV1 keyframe encoded with ffmpeg `libaom-av1`, the
  raw OBU temporal unit extracted from the IVF (32-byte file header, then
  a 12-byte frame header + the OBU payload), and the reference checksum
  from `ffmpeg -f rawvideo -pix_fmt yuv420p`. Generated in the scratchpad.
- The vendored subset was completed here: `src/tile/bitstream/` (4 files —
  `mode_info`/`palette`/`partition`/`transform_size`, missed by the
  slice-1 `-maxdepth 1` copy) added, so 65/65 non-SIMD `.cc` now build.
- More STL shaken out: `std::merge`/`unique`/`inplace_merge` (`<algorithm>`),
  used by the palette decoder.

Verified `-DLIBGAV1_SELFTEST` headless (first try):
```
[av1] decoder up; max bitdepth=8; OBU 856 bytes
[av1] decoded frame 96x96, bitdepth=8, planes=3, fmt=0
[av1] YUV csum=0x1b8a3deb exp=0x1b8a3deb MATCH; dims OK
[av1] decode self-test: ALL PASS
```
The whole AV1 decode core — OBU parse, tile/symbol (CABAC) decode,
transforms, intra/inter prediction, loop filter, CDEF, loop restoration,
the C dsp — runs bit-exact single-threaded on the tobyOS STL.

## Next slices
- **Slice 3** — wire AVIF into `toby_image_load` (ISO-BMFF `.avif`
  demux → libgav1 → RGBA), next to WebP.
- **Slice 4** — a browser `<img src=...avif>` renders on screen.
