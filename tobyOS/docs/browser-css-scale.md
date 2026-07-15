# Scaling CSS to real stylesheets

## What a real stylesheet actually looks like

Measured on youtube.com's main bundle:

| metric | value | old cap |
|---|---|---|
| CSS bytes (one file) | **3,432,544** | `SHEET_FETCH_CAP` 256 KiB |
| rule blocks | 18,497 | `RULE_MAX` 8192 |
| declarations | ~66,531 | `DECL_MAX` 32768 |
| selectors (comma-split) | ~21,258 (max **56** in one group) | `PART_MAX` 24576 |
| @media blocks | 735 | — |
| custom properties | 13,361 | — |

Total CSS across its 5 sheets: **3.45 MiB**. Every cap was exceeded, so
the sheet truncated mid-rule and the page styled from a fragment.

## Caps raised

`PART_MAX` 24576→65536, `DECL_MAX` 32768→131072, `RULE_MAX` 8192→32768,
`CSSPOOL_CAP` 320 KiB→4 MiB, `SHEET_FETCH_CAP` 256 KiB→4 MiB,
`SHEET_MAX` 6→16. These pools live in the heap-allocated per-tab
`struct eng`, which grows to ~22 MiB (verified harmless: the user heap
ceiling is 256 MiB, and a probe adding 8 MiB of dead padding changed
nothing measurable). `SEL_GROUP_MAX` was already 64, so YT's 56-selector
group fits.

## The kernel was silently capping every fetch at 1 MiB

`SHEET_FETCH_CAP` alone wasn't enough: **both** HTTP paths clamped the
body kernel-side regardless of what the caller asked for.

- `syscall.c`: `HTTP_FETCH_KERNEL_MAX` 1 MiB → 8 MiB (sync fetch:
  external scripts + stylesheets)
- `http_async.c`: `HTTPA_KERNEL_MAX` 1 MiB → 8 MiB (async: the page)

This meant the browser's page cap was really 1 MiB no matter what it
requested — the earlier `RAW_CAP` 1.5 MiB raise had never actually taken
effect; the 777 KiB YT page only worked because it fit under 1 MiB.
With both raised, `unbrotli 298446 -> 3432588` — the full sheet lands.

## The cascade had to be indexed, or big sheets are unusable

`style_node` tested **every rule against every node** — O(nodes × rules).
With YT's sheet loaded that is ~5M selector matches per style pass:

```
[cssperf] style pass 750 ms, rules=10724 nodes=471
```

750 ms *per pass*, and a pass runs on every re-render.

**Fix (what Blink does):** a rule can only match a node if its
**rightmost compound selector** matches, and that compound nearly always
demands a specific id, class or tag. So bucket every rule under that key
(`rule_index_build`) and let a node test only the rules that could
possibly match: universal ∪ tag ∪ id ∪ one bucket per class.

Details that matter:
- `ridx_hash` lowercases, because `part_match` compares id/class
  case-insensitively (csspool text is pre-lowercased). Hash the other
  way and lookups silently miss.
- Two classes can hash to the **same** bucket; walking it twice would
  add its rules twice, so `style_collect` remembers which buckets it
  has walked.
- Chains are built back-to-front so they come out in ascending rule
  order, matching the old scan.
- The index is rebuilt when `nrules` changes and invalidated in
  `eng_reset` (`idx_nrules = -1`), so JS-injected `<style>` is picked up.

**Hybrid:** below `RIDX_MIN_RULES` (256) the linear scan still wins —
`rules[]` is a tight sequential array that sits in cache, while the
index hashes the node's id/class and chases pointers through a 128 KiB
`next[]`. Measured:

| page | linear | index-always | **hybrid (shipped)** |
|---|---|---|---|
| home page (118 rules, 146 nodes) | 43 ms | 79 ms | **45 ms** |
| youtube.com (10,724 rules, 471 nodes) | 750 ms | 31 ms | **29 ms** |

**26× faster on a real sheet, no regression on light pages.**

## Correctness

The index only ever skips rules that *cannot* match — `sel_match_rule`
still runs on every candidate, and `M[]` is sorted by `(key, order)`
which is unique per rule, so the match set is identical to the linear
scan's. `-DCSS_VERIFY` proves this at runtime by re-running the old
linear scan for every node and comparing: **0 mismatches** on the home
page and on youtube.com (10,724 rules × 471 nodes).

`-DCSS_PERF` reports style-pass time, rule count and node count.

## Harness gotcha

Three consecutive ESM runs died at ~3.2 s and looked exactly like a
regression from these caps. It was the known **intermittent TKAPP boot
stall** — the same build then passed 3/3. A bisect (revert caps, keep
index) and a dead-padding probe both cleared the caps before any code
was changed. Re-run 3x before believing a boot-time failure here.

## Still outstanding for youtube.com

Its main JS bundle decompresses past `JS_SRC_CAP` (4 MiB) and QuickJS
then spends minutes parsing what it does get. That is the app-hydration
wall documented in `browser-es-modules.md`, not a CSS problem.
