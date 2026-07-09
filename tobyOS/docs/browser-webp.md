# Browser — WebP image decode

Branch `browser-webp`, stacked on main after the stage-13 chain merge
(browser-websocket, d874e77). Google, Wikipedia, and most CDNs serve
WebP by default, so real pages showed `[image failed]` boxes wherever
content negotiation picked it. `<img>` sources in WebP — lossy,
lossless, and alpha — now decode and render like any other format.

## What shipped
- **Vendored libwebp v1.4.0** (`third_party/libwebp/`) — the
  decode-only subset of Google's reference codec: `src/dec` (all ten
  decoder TUs: VP8 lossy, VP8L lossless, alpha, incremental,
  container), the decode-side `src/dsp` (plain-C + SSE2/SSE4.1
  variants; the MIPS/NEON/MSA and encoder files are not vendored),
  the decode subset of `src/utils` (+ `palette.c`, which
  `utils.c` references), and the public `src/webp` headers. Picked
  1.4.0: current, self-contained, and past the 2023 VP8L
  CVE-2023-4863 fix. Compiles against libtoby's freestanding libc
  with **zero patches** — only the enc *headers* are carried because
  the shared `dsp/lossless.h` includes them.
- **`libtoby/src/image.c`** — `toby_image_load()` now sniffs the
  `RIFF….WEBP` container signature and routes it to
  `WebPDecodeRGBA()` before the stb_image path; the RGBA→ARGB8888
  conversion is shared. Every libtoby app that renders images (the
  browser, gui_files previews, canvas `drawImage`) gets WebP
  transparently — the browser itself needed no changes.
- **Makefile** — the libwebp objects compile into `libtoby.a` (and
  `.pic.o` into `libtoby.so`) with the package-rooted include path
  and `-msse -msse2`, matching stb_image's rule; the SSE2 dsp paths
  are active, the SSE4.1 variants compile empty without `-msse4.1`
  and runtime dispatch never selects them. `WEBP_USE_THREAD` stays
  undefined (sequential decode).

## Verified (QEMU)
- **`/webp` test page** — all three container layouts decode and
  render on screen:
  - lossy **VP8** (smooth gradient + circles, photo-style);
  - lossless **VP8L** (sharp color blocks + a 1-px grid — pixel-exact
    content that lossy coding would smear);
  - **VP8X + ALPH** alpha (a lossy blue ring whose transparent hole
    shows the green box behind it — alpha compositing through the
    existing ARGB paint path).
- **Real internet over TLS**: Google's official gallery files —
  `gstatic.com/webp/gallery/4.webp` (lossy photo) and
  `gallery3/2_webp_ll.webp` (the lossless+alpha Tux) — fetched via
  the async image loader and rendered crisply.
- Regressions clean: PNG on the same page, and the `/img` PNG page
  through the same decoder funnel.

## v1 limits
- Still images only: animated WebP (ANIM/ANMF chunks) decodes its
  first frame at best via the container path — no animation loop
  (GIF has the same static-first-frame treatment today).
- No incremental/progressive decode — images decode when the fetch
  completes (matches the existing PNG/JPEG behavior).
- The browser does not advertise `Accept: image/webp`, so
  content-negotiating servers keep sending PNG/JPEG; explicit `.webp`
  URLs (the common CDN case) are what this enables.
- ICC/EXIF chunks are ignored (no color management, same as PNG).
