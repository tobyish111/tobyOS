# Browser stage 13E: web fonts (@font-face)

Branch `browser-webfonts` (stacked on `browser-overflow`). Real sites
brand their typography with downloaded fonts; without `@font-face` all
text falls back to the bundled Lato, so pages look wrong even when
everything else is correct. This is the first stage-13 feature to span
both the kernel rasterizer and the browser.

## What landed

### Kernel (src/kfont.c, include/tobyos/kfont.h, abi.h, syscall.c)
- The kfont face table grew from the 4 bundled Lato faces to
  `KFONT_TOTAL` = 4 + `KFONT_WEB_MAX` (8) slots. Face ids
  `[KFONT_WEB_BASE, KFONT_TOTAL)` are **registered web fonts**.
- `kfont_register(ttf, len)` takes a private copy of a downloaded
  TTF/OTF blob, `stbtt_InitFont`s it into a free web slot, and returns
  the face id (or -1). `kface_info` resolves a web face to its
  registered font, falling back to Regular if the slot is empty.
- New syscall `ABI_SYS_GUI_FONT_REGISTER` (176): copies the user font
  buffer (≤ 2 MiB) and registers it, returning the face id. The
  existing `GUI_TEXT_TTF` / `GUI_TEXT_TTF_WIDTH` already pack a face
  into `a5`; the face mask was widened from 2 bits to 8 so web faces
  (≥ 4) are addressable.

### Browser (programs/user_gui_browser/main.c)
- **`@font-face` parsing**: a new at-rule handler in `css_parse_sheet`
  captures the block and extracts `font-family` + a usable `src` url,
  preferring a `format('truetype'/'opentype')` or `.ttf`/`.otf` source
  and skipping compressed `woff`/`woff2` (see limits). Entries live in
  a session cache keyed by url (so re-visits reuse the kernel slot).
- **Loading**: `load_webfonts()` runs after collect (once per font),
  downloads each src via the kernel HTTP fetch, and registers it →
  face id.
- **Family matching**: `cstyle` gained an inherited `webface`. The
  `font-family` handler resolves the first listed family that names a
  registered `@font-face` to its face id.
- **Text pipeline**: the run's resolved face (`run_face`: a web face
  wins, else bold/regular) threads through `istyle` → the `DI_TEXT`
  display item (`ditem.face`) → paint, and through the advance cache
  (now keyed by `(px, face)`, using the face-aware width syscall for
  web faces). Text is now painted with `sys_text_ttf(win.fd, …, face)`
  instead of the bold-only `tk_draw_text`.

## Verified in QEMU (screenshot in the session scratchpad)
The `/webfonts` page (a downloaded Georgia served as `MyWebFont`):
1. Default Lato — clean sans-serif.
2. `@font-face 'MyWebFont'` — the same sentence in the **downloaded
   Georgia**, visibly serif (green).
3. The web font at 40px — "Serif Aa Bb Gg" with obvious serifs (red).
Serial confirms `[kfont] registered web face 4 (219712 bytes)`.
CSS torture page regression clean (the new paint path renders the
bundled regular/bold/mono faces correctly too).

## v1 limits
- **Raw TTF/OTF only.** WOFF (zlib) and WOFF2 (Brotli + glyph
  transforms) sources are skipped — a family offered *only* in those
  formats falls back to the default. (Most self-hosted fonts and
  Google Fonts' legacy endpoint offer TTF; WOFF/WOFF2 decode is a
  natural follow-up, feeding the same registration path.)
- One face per family — `@font-face` weight/style variants aren't
  separated, so a page's bold-web-font run uses the same registered
  face (bundled-font bold still works as before).
- 8 kernel web-font slots total (evict/unregister not implemented);
  the browser dedups by url so a page's fonts persist across
  re-visits, but > 8 distinct web fonts in a session fall back.
- `src` urls resolve against the page, not the stylesheet's own url
  (matters only for cross-origin sheets).
