# :not() selectors + the `hidden` attribute (github's stacked nav)

Punch-list closeout, item 1: github (grid-diff 39.6) rendered its
logged-out marketing mega-nav as ~1708 px of stacked links at the top,
pushing the repo content below the fold.

## Root causes (DL_TRACE named the box, the CSS named the rule)

    [dl] RECT x=23 y=129 w=662 h=1708 class="HeaderMenu-wrapper …"

1. GitHub hides its mobile menu with
   `.header-logged-out:not(.open) .HeaderMenu { visibility:hidden; … }`.
   Our parser rejected **any** pseudo-class, so the whole selector was
   dropped and the hiding rule never existed → the menu rendered.
2. The "You signed in with another tab…" flash banners are
   `<div hidden="hidden" …>` — we ignored the global **`hidden`
   attribute** entirely (25 uses on this one page).

## Fixes (both general)

- **Simple `:not(<simple>)` in compound selectors**: one argument of
  class / id / tag / `[attr]`-presence form, up to `PART_NOT_MAX` (4)
  negations per compound (wikipedia chains three:
  `table:not(.infobox):not(.navbox-inner):not(.navbox)`).
  `struct cpart` carries kind/off/len triples; `part_match` rejects when
  a negated simple matches. Specificity counts the argument's own level,
  per spec. **Fail closed** stays the rule: complex arguments
  (combinators, commas, nested :not, `[a*=v]`) still invalidate the
  selector. Index interplay is safe by construction — the rule-index
  bucket key and the ancestor bloom only ever read *positive* parts, so
  negations cannot cause false negatives (`-DCSS_VERIFY` proves the
  match set).
- **Global `hidden` attribute** = UA-level `display:none`, applied as a
  pre-cascade hint in `style_node` so an author `display:` rule still
  overrides (matching real UAs).

## Verification

- github composite: the 1708 px menu is gone; repo header
  (`tobyish111/tobyOS` + Public), Code/Issues/PR/Actions tabs, branch
  button, Go-to-file, and the file-list column headers all render above
  the fold, matching Edge's structure. Remaining deltas: Edge's dark
  header band, octocat logo, deferred file rows.
- `-DCSS_VERIFY` 0 mismatches on wikipedia (which now *gains* its
  `:not(…)` table rules); wiki + hn composites regression-checked;
  home page; ESM 3×.
