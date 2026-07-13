# CSS decoration: border-radius, box-shadow, linear-gradient

Branch `browser-css-deco` (Chrome-parity push — visual polish). These
three are what make modern pages *look* modern (rounded buttons/cards,
elevation shadows, gradient heroes/buttons); their absence made every
page look boxy and dated. All three are paint-time and fit the
immediate-mode painter — no layout changes.

## Storage
- `cstyle.radius` (uint8 px; 255 = sentinel for `50%` → pills/circles)
  and `cstyle.deco` (int16 index into a per-style-pass `g_deco[]` pool,
  −1 = none). Decoration does not inherit (reset in `st_init`).
- `struct deco` holds the bulkier bits: linear-gradient (direction +
  up to 4 color stops with positions) and box-shadow (dx, dy, blur,
  spread, ARGB color). `deco_of(st)` lazily allocates a node's slot so
  several decls (gradient + shadow) group onto one entry.
- `ditem` gained `radius` + `deco` (reusing former pad bytes) so the
  display item carries them to paint; `DI_SHADOW` is a new item kind.

## Parse
- `border-radius`: first value; px or `50%` (→ sentinel 255). **Note:
  `LK_PX == 0`, so the length parse is tested against the return code,
  not truthiness — the initial version silently no-op'd both radius and
  shadow because `if (css_len_tok(...))` treated a successful px parse
  as false.**
- `box-shadow`: `[inset] dx dy [blur] [spread] color` (inset ignored);
  default color ~25% black, forced to a visible alpha.
- `background: linear-gradient([to <dir>|<deg>,] c0 [p%], c1 [p%], …)`:
  axis-aligned (angles snap to the nearest of to-bottom/right/top/left),
  2–4 stops. Solid `background` clears a prior gradient.

## Layout (`lay_block`)
The box's background rect is stamped with `radius` + (gradient) `deco`;
a `DI_SHADOW` item is emitted **before** it (paints behind) carrying the
same radius + deco. Both heights are patched once the box height is
known.

## Paint
One shared technique (the compositing layer's `tk_draw_blit_blend`
rows), so everything composites over the already-painted backdrop —
rounded corners and soft shadows blend against the real content behind
them, no backdrop-color guessing:
- `rbox_sdf`: integer signed-distance to an axis-aligned rounded rect
  (`isqrt32` for the corner arcs).
- `paint_deco_rect`: per pixel, color = gradient sample or solid,
  alpha = coverage from the SDF (1px anti-aliased edge).
- `paint_deco_shadow`: the box grown by spread, corners `radius+spread`,
  alpha full inside and ramping to 0 over `blur` px outward.

## Verified
A page with a purple→pink gradient hero (radius 14), a white card
(radius 12 + `box-shadow: 0 6px 18px rgba(0,0,0,.25)`), a blue gradient
rounded button, a green pill, and a `border-radius:50%` gradient avatar.
Pixel checks confirm: hero corners cut to the page background (gradient
starts ~10px in), the avatar renders as a true circle (bbox corners are
background, center is gradient), and the card casts a real soft shadow
(edge darkens `195` → `221` → page `238` outward).

## Limits / follow-ups
Uniform corner radius only (no per-corner `border-radius: a b c d`);
axis-aligned gradients (diagonal angles snap to an axis); no
radial/conic gradients, multiple backgrounds, `background-image` URLs,
inset shadows, multiple shadows, or `border-image`; rounded clipping
doesn't clip child content/overflow (only the background paints
rounded); decorations inside a `transform` compositing layer paint
square (the layer renderer predates this).
