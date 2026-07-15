# Color emoji (COLR/CPAL)

Emoji codepoints now render as **full-color glyphs** inline with text
(Twemoji Mozilla via the COLR v0 + CPAL color-font tables), instead of the
monochrome Unifont fallback / tofu.

## What works

Verified on-screen: `Hello 😀 world 👍! I ❤️ tobyOS 🚀 🌈` and a row of
😀😎😂👍❤️🎉🚀🌈🔥🌟 all render as color Twemoji glyphs, mixed into normal
text layout at the surrounding font size, wrapping like any inline
content.

## How it works

- **Font**: `initrd/etc/emoji.ttf` = Twemoji Mozilla (COLR v0 / CPAL,
  glyf-based, ~1.5 MB, CC-BY 4.0). Loaded lazily by the browser.

- **Renderer** (`libtoby/src/emoji.c`, `toby/emoji.h`): a self-contained
  COLR/CPAL parser over its own stb_truetype instance.
  `toby_emoji_render(cp, px)` maps the codepoint → base glyph (cmap),
  looks up its COLR layer list (binary search of the BaseGlyphRecords),
  and composites each layer: rasterize the layer's outline
  (`stbtt_MakeGlyphBitmap`), tint it by its CPAL palette-0 color (BGRA),
  and alpha-over into an ARGB buffer. Returns a straight-alpha ARGB glyph
  + advance. `toby_emoji_has(cp)` is the fast "is this a color glyph?"
  check.

- **Browser integration** (`user_gui_browser`): inline word layout
  (`ic_word`) scans for emoji codepoints — gated on `cp >= 0x2000` then
  `toby_emoji_has`, so ASCII-heavy text pays almost nothing — and splits
  them out as atomic `DI_EMOJI` items (the codepoint rides in the item's
  `off` field); the surrounding text lays out normally. A trailing
  variation selector (U+FE0E/FE0F) after an emoji is swallowed so it
  doesn't render as tofu. The painter renders each `DI_EMOJI` via a small
  LRU cache (`emoji_get`, keyed by codepoint+px) and blits the ARGB glyph
  with `tk_draw_blit_blend`.

## Known limits

- **COLR v0 only** — layered solid-color glyphs (Twemoji). No COLR v1
  gradients, no CBDT/CBLC (bitmap) or sbix or OT-SVG color formats.
- **Single codepoints** — no ZWJ emoji sequences (👨‍👩‍👧, 🏳️‍🌈), skin-tone
  modifiers, or flag pairs; each base codepoint renders as its own glyph.
- Emoji are top/line-aligned atomics, not baseline-shifted; hit-testing
  treats them as non-interactive inline boxes.
- The color path is browser-side only (userspace libtoby); the kernel
  text raster (`sys_ttf_raster`) is still monochrome, so native
  (non-browser) UI text renders emoji via the Unifont fallback.

## Build

The font ships in the initrd font list + iso tar list; `emoji.c` builds
`-msse -msse2` (stb_truetype float scale) like `font.c`. The
`-DEMOJI_TEST` browser home page shows a sample page.
