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

## Next slices
- **Slice 2** — decode correctness: a program links libgav1's `Decoder`,
  decodes an embedded AV1/AVIF frame, and checks the output against a
  reference (bit-exact, the AV1 spec is deterministic). This is where the
  decode core, thread pool (run threads=1), and the C dsp shake out.
- **Slice 3** — wire AVIF into `toby_image_load` (ISO-BMFF `.avif`
  demux → libgav1 → RGBA), next to WebP.
- **Slice 4** — a browser `<img src=...avif>` renders on screen.
