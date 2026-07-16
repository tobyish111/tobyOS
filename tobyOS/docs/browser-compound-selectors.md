# Compound-selector class cap: fail closed, not open (the salmon boxes)

## Symptom

On Wikipedia (vector-2022 / Codex skin) the header showed solid salmon-pink
boxes (`#ffc8bd`) where the logo-area buttons, the Donate button, the language
selector and the search toggle should be. The boxes were `DI_RECT` items —
CSS-painted backgrounds, not images.

## Root-cause hunt (measure first)

The prior profiling pass left a strong lead: `#ffc8bd` is Codex's
`--background-color-destructive-subtle--active` token, used as the fallback in
`background-color: var(--background-color-destructive-subtle--active, #ffc8bd)`,
so the leading hypothesis was a custom-property scope overflow
(`VARSCOPE_MAX` = 512) silently dropping late `:root` tokens.

**Measurement disproved it.** A gated `-DVAR_PROF` accounting pass (kept
in-tree) counts pushes dropped to a full scope table / pool, lookup hits and
misses, high-water marks, and the first distinct missed names:

    [varprof] hwm_scope=381/512 hwm_pool=14200/65536 drop_scope=0 drop_pool=0
              hit=7526 miss=144
    [varprof] missed: --font-size-small --line-height-small --mw-file-upright

No drops, 381/512 live entries, and the destructive token never missed — so
its `var()` lookups **hit**: the token is defined (value `#ffc8bd`), and the
substitution is correct. The bug had to be that the *rule itself* applies at
rest.

A one-off gated trace on "who sets bg = #ffc8bd" gave the answer directly:

    [salmon] node=207 tag=40 class="cdx-button cdx-button--fake-button
             cdx-button--fake-button--enabled ... search-toggle"
             raw="var(--background-color-destructive-subtle--active,#ffc8bd)"

Every salmon node carries `cdx-button--fake-button--enabled`. The matching
rule in Wikipedia's sheet is:

    .cdx-button.cdx-button--fake-button--enabled.cdx-button--action-destructive.cdx-button--is-active

One compound, **four classes, no pseudo-class**. `struct cpart` stored at most
**2 classes per compound** (`cls_off[2]`) and `css_parse_part` silently
ignored the rest while keeping the selector valid — so the selector
degenerated to `.cdx-button.cdx-button--fake-button--enabled`, which matches
*every* fake-button at rest. Codex expresses all its button states this way
(BEM chains up to 5 classes: base + fake-button--enabled + weight-* +
action-* + is-active), so every state-variant rule (active, progressive,
destructive…) was being applied to every button, last-in-sheet winning —
which happened to be destructive-active salmon.

## Fix

- `PART_CLS_MAX` = 6 classes per compound (`struct cpart.cls_off/cls_len`
  arrays); Wikipedia's worst accepted compound has 5.
- **Fail closed**: a compound with more classes than the cap now makes the
  whole selector invalid (rule dropped, same as an unsupported pseudo-class)
  instead of over-matching with a silent prefix. Under-matching degrades
  gracefully; wrong-matching paints destructive-red buttons.
- The same fail-open pattern existed for a second `#id` or second `[attr]`
  in one compound (silent overwrite → fewer constraints → over-match): both
  now also invalidate the selector.
- `VARSCOPE_MAX` 512 → 4096 and `VARPOOL_CAP` 64 KiB → 512 KiB. Wikipedia
  measured 381 entries / 14 KiB so it was safe, but youtube.com ships ~13k
  custom-property declarations and would overflow the same silent way.
  `var_lookup` scans live entries, not the cap, so light pages pay nothing.

The cascade rule index and the ancestor bloom key off `cls_off[0]` only;
both stay correct with more stored classes (they only ever need a subset of
a part's constraints).

## Verification

- Wikipedia `Operating_system`: salmon boxes gone (buttons render quiet
  backgrounds; the still-black squares are the separate SVG-gradient gap),
  128 links, same 2 pre-existing JS errors as the unfixed baseline.
- `-DCSS_VERIFY` (indexed-vs-linear cascade comparison per node): 0
  mismatches on wikipedia (11,157 nodes × 953 rules).
- Style pass unchanged: 129 ms vs 125 ms baseline (TCG).
- Default home page renders; ESM self-test 3× ALL PASS.

## Cost

`struct cpart` grows by 4 class slots (~24 B) × `PART_MAX` 65536 ≈ +1.5 MiB
on the per-tab heap `struct eng` (~22 MiB → ~24 MiB).

## Gated diagnostics kept

`-DVAR_PROF` — per-style-pass custom-property accounting (drops, hit/miss,
high-water marks, first 12 distinct missed names), dumped after each style
pass like `-DSTYLE_PROF`.
