# UTF-8 text rendering (browser + kernel TTF)

Branch `browser-utf8-text`. Until now the whole text pipeline was
ASCII: the browser TRANSLITERATED every decoded codepoint at parse time
(`tp_put_cp` mapped Latin-1 letters to their base ASCII letter, curly
quotes to straight quotes, everything else to `?`), and the kernel TTF
rasterizer iterated bytes. Real pages lost every accent and typographic
character.

## What changed
- **Kernel (`src/kfont.c`)**: the three entry points
  (`kfont_draw_window_f`, `kfont_text_width_f`, `kfont_raster_cov`)
  decode UTF-8 (`kf_utf8_next`; invalid bytes render as `?`), so every
  consumer — browser text, TobyTK labels, window titles, compositing
  layers — is Unicode-aware with byte-buffer APIs unchanged.
  `kfont_glyph` looks up glyph indices; a codepoint the face doesn't
  cover synthesizes a hollow "tofu" box coverage bitmap into the glyph
  cache, so draw/width/raster need no special-casing and missing
  glyphs are visibly boxed, never invisible.
- **Browser (`programs/user_gui_browser/main.c`)**:
  - `tp_put_cp` now emits UTF-8 into the tpool (transliteration table
    deleted). Zero-width characters are dropped; NBSP stays a normal
    space (the wrapper has no no-break support yet).
  - `text_px_w` decodes UTF-8: ASCII uses the existing per-size advance
    table; non-ASCII goes through a new exact-keyed per-codepoint
    advance cache (`cp_adv`, 1024 slots, one kernel width query per
    unique (cp, px, face)). Wrapping math stays in userspace.
  - The two DI_TEXT paint paths (window + compositing layer) pass
    bytes >= 0x80 through to the kernel; 8x8-mono runs show `?` per
    byte (that face is ASCII-only).
  - Word wrapping was already UTF-8-safe: words are whitespace-split
    byte slices and never break mid-word.

## Verified
A served page renders on screen with: typographic characters (curly
quotes, em/en dash, ellipsis, guillemets, bullet), French/German
(Straße, brûlée, Äpfel), Polish/Czech Latin Extended (zażółć gęślą
jaźń, žluťoučký kůň), Greek (γρήγορη καφετιέρα), Russian (съешь ещё
этих мягких булок), math (½ × π ≈ ∞ °) — all as real glyphs from the
shipped Lato faces — and CJK/emoji as tofu boxes (Lato has no coverage;
font fallback is future work).

## Limits / follow-ups
No shaping (no combining-mark positioning, ligatures, kerning pairs),
no bidi, no font fallback chain (CJK/emoji = tofu until a fallback
face ships), NBSP breaks, canvas fillText is still 8x8 ASCII, the
kernel text syscalls cap runs at 256 bytes (~85 CJK-class chars).
