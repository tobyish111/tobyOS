# Custom-property `initial` semantics + `light-dark()` (MDN's black page)

Punch-list item #2: MDN rendered a near-black page background where Edge
renders white — the single largest harness score (grid-diff 203).

## Root cause (from MDN's stylesheets, no boot needed)

MDN themes every color token through the csstools `light-dark()`
polyfill:

    html { --csstools-color-scheme--light: initial; }
    @media (prefers-color-scheme: dark) {
        html { --csstools-color-scheme--light: ; }
    }
    --color-background-page:
        var(--csstools-light-dark-toggle-…-0, var(--color-white));

The whole scheme rides on a spec subtlety (css-variables-1): a custom
property declared as `initial` is **guaranteed-invalid** — it counts as
*not set*, so any `var()` chain referencing it collapses to the
fallback (the light value). Our resolver substituted the literal text
`initial`, which made the toggle var "defined", so the dark arm of every
token leaked through — and the paint-side color parser then found the
dark color. The page went black in *light* mode precisely because the
light-mode switch is the invalid one.

## Fix (all three general)

1. **`--x: initial` = guaranteed-invalid** (`var_push`): stored as a
   `vlen = -1` sentinel. `var_lookup` treats it as *unset* (fallback
   taken) while still **shadowing outer definitions** (scan stops at the
   sentinel rather than skipping it — an inner `initial` must hide an
   ancestor's real value, per spec).
2. **Invalid at computed-value time** (`var_subst`) — the piece that
   actually cures the black page; the sentinel alone was measured
   insufficient (grid-diff 203 → 202). Per css-variables-1 §3,
   `var(--x)` of an unset property with **no fallback** does not expand
   to empty: it poisons the *whole containing value*. And a referenced
   property whose own value poisons counts as invalid, so the
   *referrer's* fallback is taken. That is exactly the toggle chain:
   `--toggle: var(--scheme-light) #18191b` must collapse to invalid in
   light mode so `var(--toggle, var(--color-white))` yields white; our
   old expand-to-empty left `#18191b` standing. `var_subst` now carries
   a poison out-param through the recursion (a poisoned reference falls
   through to the local fallback; a poisoned top-level value makes
   `st_apply` skip the declaration entirely).
3. **Native `light-dark(A, B)` → A** (`lightdark_subst` in `st_apply`,
   applied after `var()` substitution since either arm usually comes
   from custom props). Paren-aware; nested functions survive. MDN's
   newer sheets use the native function directly; adoption is growing
   across real sites. We render the light scheme only — documented.

## Verification

- mdn composite: dark page → white page (grid-diff 203 → see punch
  list update), matching Edge.
- `-DCSS_VERIFY` zero mismatches (cascade behavior touched via var
  resolution: verified on wikipedia).
- wikipedia composite unchanged; home page renders; ESM 3× ALL PASS.
