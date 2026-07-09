# Browser — WOFF2 web fonts

Branch `browser-woff2`, stacked on `kernel-tls-certs` (13H). Closes the
stage-13E limitation "raw TTF/OTF only": `@font-face` sources delivered as
**WOFF2** — the format essentially every modern site and Google Fonts
ships — now decode and render. WOFF2 is brotli-compressed sfnt tables plus
a glyf/loca (and optional hmtx) transform; brotli landed in 13G, so this
adds the transform reversal on top.

## What shipped
- **`src/woff2.c`** — a freestanding WOFF2 → sfnt decoder (kernel-side,
  where brotli lives). It:
  - parses the WOFF2 header + table directory (known-tag table,
    `UIntBase128` / `255UInt16` varints, per-table transform flags);
  - brotli-decompresses the compressed table block;
  - **reverses the glyf transform** — the 7 sub-streams (nContours,
    nPoints, flags, glyphs, composites, bbox, instructions) are decoded
    back into standard `glyf` records (triplet point decoding, flag
    run-length re-encoding, bbox reconstruction, composite copy) and the
    `loca` table is rebuilt alongside;
  - reverses the hmtx transform (rare) using the per-glyph xMin;
  - reassembles a valid sfnt (offset table + tag-sorted table records +
    4-byte-aligned tables) that stb_truetype parses directly.
  The transform algorithms follow the reference decoder (google/woff2,
  MIT); ported to C over kmalloc buffers.
- **`src/kfont.c`** — `kfont_register()` now detects the `wOF2` signature
  and transparently runs `woff2_to_sfnt()` before handing the bytes to
  stb_truetype, so the whole web-font path (download → register → raster)
  works unchanged for WOFF2.
- **Browser** — `webfont_pick_url()` now treats `format('woff2')` /
  `.woff2` as a first-class, usable source (raw TTF/OTF still preferred
  when both are listed; WOFF1 still skipped).

## Verified (QEMU, `/woff2` test page)
- A page with `@font-face { src: url(x.woff2) format('woff2') }` renders
  its text in the decoded web face. The 102 532-byte test WOFF2 (a serif
  face) decodes in-kernel to a 212 844-byte sfnt with 21 tables and 864
  glyphs (brotli 102 419 → 194 085 bytes, then the glyf transform rebuilds
  `glyf`=158 420 / `loca`=3 460). Screenshot shows the same face rendered
  crisply at 26 px and 44 px, visibly distinct from the default Lato
  sans-serif.
- Regressions clean: the raw-TTF `/webfonts` page and the `/css` torture
  page still render.

## v1 limits
- TrueType-outline (`glyf`) fonts and CFF fonts (opaque tables) both work;
  the `glyf` transform is the reconstructed path.
- The overlap-simple bitmap is handled; hinting instructions are carried
  through verbatim.
- No WOFF1 (zlib per-table) — a much rarer format; the brotli/puff pieces
  exist to add it later if needed.
- Font collections (`ttcf`) are not split (single-font WOFF2 only).
