# Browser stage 12B: real table layout

Branch `browser-tables` (stage-12 scope item B, stacked on
`http-chunked-keepalive`). Before this, `<td>`/`<th>` flowed inline in
a block `<tr>` — Hacker News, Wikipedia infoboxes and every data table
degraded to word soup.

## What landed (programs/user_gui_browser/main.c)

### Style plumbing
- New display types: `D_TABLE`, `D_TSEC` (row groups), `D_TROW`,
  `D_TCELL`, `D_CAPTION`; `display:` parses table / inline-table /
  table-row-group|header-group|footer-group / table-row / table-cell /
  table-caption. UA sheet gives table elements their proper displays.
- `vertical-align` (top/baseline→0, middle, bottom) on cells +
  `border-collapse` (SF_BCOLLAPSE) — new `cstyle.valign` byte.
- Legacy presentational attributes (pre-cascade, CSS wins): `bgcolor`
  on table/tr/td, `width`/`height` on table/td (px + %), `align` and
  `valign` on cells (+ tr valign fallback), table `border=` ⇒ 1px cell
  borders, `cellspacing=` ⇒ grid gap, `cellpadding=` ⇒ cell padding
  (applied only when the cascade left the UA default `1px 8px`).
- Tables reset INHERITED text-align: real browsers center a table BOX
  under `<center>`, not the text in its cells — HN's whole page sits
  in `<center>` and was center-justified without this.

### The grid (`lay_table_grid` in the layout core)
- lay_block's child loop factored into `lay_flow` (shared by blocks
  and cells); `disp == D_TABLE` routes to the grid.
- Pass 1 measures every cell's min-content / preferred border-box
  widths using the frozen provisional-layout trick from floats (lay at
  cw=100000 for preferred, cw=8 for min, roll items/render/floats
  back). Explicit px widths short-circuit; % widths recorded per
  column. colspan distributes its measure evenly (rowspan degrades
  to 1).
- Width distribution over the target grid width W (explicit width ⇒
  the resolved content width; auto ⇒ shrink-to-fit at Σpref, clamped):
  pref fits ⇒ pref (+ proportional stretch only for explicit widths);
  between Σmin and Σpref ⇒ proportional interpolation; below Σmin ⇒
  scale mins down (no horizontal overflow, there is no h-scroll).
- Pass 2 lays rows: cells are block containers laid by `lay_flow` at
  final coordinates; row height = max cell border-box (or tr height);
  cell/tr backgrounds stretch to the row height; `valign` shifts the
  cell's item range; borders drawn on the final row-height box.
  Floats cannot escape their row. `cellspacing` separates everything;
  border-collapse = spacing 0.
- Caption lays as a block above the grid. Auto-width tables pull the
  table box (and its background) in to the used grid width after the
  grid runs (`g_tbl_used_w`).
- All grid state is on the stack — nested tables recurse safely
  (Wikipedia infoboxes are tables inside tables).

### Two engine bugs found by the acid test
1. **The 16-selector group cap** (`css_parse_ruleset`): selectors past
   the 16th in a grouped ruleset were silently dropped. The UA sheet's
   own block-display group has 34 members — `blockquote, form, header,
   footer, nav, main, section, article, aside, figure, center`... have
   rendered INLINE since stage 7. On HN (whose page sits inside
   `<center>`) the inline `<center>` routed the whole document through
   the inline walker, so the new table grid never ran. Cap raised to
   64 (`SEL_GROUP_MAX`).
2. **Measure-pass alignment inflation**: `ic_break` applied
   text-align, and lay_block applied auto-margin centering, against
   the huge provisional width during frozen measure passes — a
   right-aligned HN rank cell measured ~100000px wide and swallowed
   the whole table. Both are now suppressed under `g_flt_freeze`
   (also fixes latent shrink-to-fit float measurement of
   centered/right-aligned content).

## Verified in QEMU (screenshots in the session scratchpad)
- Local torture page: HN-style attribute table (border=0 cellpadding=0
  cellspacing=0, width=92%, bgcolor, colspan skip, spacer row),
  bordered data table (border-collapse, blue th header, colspan=4 with
  bgcolor, caption), right-floated infobox table with wrapping text,
  valign top/middle/bottom against a 90px cell.
- **Live news.ycombinator.com**: masthead + nav in the orange bar,
  ranked stories with left-aligned title links, domains, gray subtext
  rows — all in their columns. (Ladder target 2.)
- **Live en.wikipedia.org/wiki/Cat**, maximized window: the infobox is
  a real right-floated table (caption, temporal-range strip) with the
  article wrapping beside it. NOTE: at the default ~715px window the
  infobox is intentionally NOT floated — wiki's mobile @media matches
  and sets `.infobox{width:100%}`; maximize first when testing.
- Regressions: floats page, CSS torture page, /js2 events, /spa Preact.

## Known v1 limits
rowspan degrades to height-1; row-group ordering is source order
(thead not hoisted); border-collapse only zeroes spacing (adjacent
1px borders double); caption-side top only; no `<col>`/`<colgroup>`
widths; a float taller than its row is clipped out of later rows'
consideration; anonymous cells (bare text in `<tr>`) are skipped.
