# Browser stage 13B: calc() length expressions

Branch `browser-calc` (stacked on `browser-css-vars`). `calc()` is the
other half of modern CSS sizing — `width: calc(100% - 250px)` for
"fill minus a fixed sidebar" is everywhere. Paired with custom
properties (13A), `calc(var(--gap) * 2)` is common too.

## What landed (programs/user_gui_browser/main.c)

- **Evaluator**: a recursive-descent `calc_expr`/`calc_term`/
  `calc_factor` that evaluates a `calc(...)` expression into an
  **absolute px part + a percentage part** (`struct calcval`), so a
  mixed `calc(100% - 250px)` is fully representable rather than
  collapsing to one or the other. `+`/`-` (lowest precedence), then
  `*`/`/`, then a factor (number+unit or a parenthesized subexpression,
  nesting supported). One operand of `*`/`/` must be unitless. Units:
  px, em, rem, pt, ch, vw, vh, `%`, and plain numbers.
- **Wiring**: `css_len_tok` recognizes a `calc(` token and evaluates
  it. Pure-absolute → `LK_PX`. Pure-percent → `LK_PCT`. **Mixed** →
  `LK_PCT` for the percentage part, with the px offset stashed in
  `g_calc_off_px`.
- **Mixed width offset**: `cstyle` gained `woff`; the `width` handler,
  on a mixed calc, sets `width` = percent + `SF_WPCT` + `woff` = px
  offset, and `lay_block`/`lay_float` apply `content_w += woff` after
  the percentage resolves against the real containing block. So
  `calc(100% - 200px)` is **correct at any nesting depth**, not an
  approximation — the offset rides along until the width is actually
  computed.
- **Everywhere else** (padding, margin, gap, font-size, max-width,
  position offsets, flex-basis): absolute-only calc evaluates to px;
  a mixed calc on those returns `LK_PCT` which those handlers ignore
  (falling back to their existing behavior) — never a wrong value.
- **Composes with 13A**: because `var()` substitution happens on the
  raw value string *before* the property parser runs,
  `calc(100% - var(--sidebar))` works — the variable expands, then
  calc parses the result.

## Verified in QEMU (screenshot in the session scratchpad)
The `/calc` page, all six cases correct:
1. `width: calc(100% - 200px)` — a blue box exactly 200px narrower than
   its container.
2. `width: calc(50% + 40px)` — green box just past half width.
3. `width: calc(100px * 3)` — 300px box (multiplication).
4. `padding: calc(4px + 6px)` — orange box with 10px padding
   (absolute-only calc).
5. `width: calc(100% - var(--sidebar))` — same 200px inset as #1,
   proving var()+calc composition.
6. `font-size: calc(12px + 10px)` — 22px text.

Regressions: variables page unaffected; flex unaffected (a px/percent
flex-basis takes the non-calc path, `woff` is 0 for any non-calc
width).

## v1 limits
- Mixed `%±px` is stored only for `width` (the dominant real case).
  Height/max-width/padding calc are absolute-only; a mixed calc on
  them falls back rather than producing a wrong value.
- No `min()`/`max()`/`clamp()` (each a small addition later), no unit
  algebra beyond length (`calc(1s + ...)`), no calc in color/other
  non-length contexts.
