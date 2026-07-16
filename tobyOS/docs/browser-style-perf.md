# Profiling the style pass: the ancestor bloom + class cache

After the network work, the style pass was the biggest measured cost of a
real page load. Rather than guess, `-DSTYLE_PROF` (rdtsc accumulators)
split `style_node` into sub-phases and `-DCSS_PERF` timed `layout`.

## What the profile said

Wikipedia (*Operating system*), style pass 181 ms:

```
[styleprof] Mtsc pres=3 match=644 inline=3 var=9 apply=100 | nodes=5604 matches=11698 cand=96238
[cssperf]   phase layout 61 ms
```

- **match was ~85% of the style pass.** Layout was a non-issue (61 ms).
- The rule index was already doing its job: **17 candidates per node**,
  only **3 rules** in the catch-all universal bucket.
- But `part_match` ran **962,719** times and `class_attr_contains`
  **659,323** times — ~10 ancestor hops and ~7 class re-parses *per
  candidate*. That, not candidate count, was the cost: ~6,350 cycles per
  candidate vs 340 on the (class-light) home page.

So the index picks the right rules; the expense is *confirming* each
descendant selector by walking ancestors and re-reading their `class`
attributes.

## Fix 1: ancestor bloom (Blink's "fast reject")

Each node carries a 64-bit bloom of the keys (tag / id / class) of itself
**and all its ancestors**, built top-down in `style_node` (the parent's
is final before the child is styled). Each rule precomputes `anc_bloom` —
the bits its **non-rightmost** compounds demand — when the rule index is
built.

`sel_match_rule` then rejects in O(1): if the node's parent bloom lacks
any bit in `anc_bloom`, no ancestor can satisfy the selector, so the
whole upward walk is skipped.

A bloom yields only false **positives** (we then walk and match
properly), never false negatives — a real ancestor's key is always OR'd
in. So the match set is unchanged.

## Fix 2: per-node class-span cache

`class_attr_contains` called `node_attr(nd, "class")` — a linear scan of
the node's attribute list with a per-char compare — on every one of its
~476 K calls, then re-tokenized the returned string. `style_node` now
caches the class-attribute span `(cls_voff, cls_vlen)` once, when it
computes the node's bloom, validated by a per-pass generation counter
(`g_cls_gen`, bumped in `rule_index_ensure`). The matcher reads the
cached span instead of re-finding it.

## Result

| style pass | indexed | + bloom | + class cache |
|---|---|---|---|
| wikipedia.org (953 rules, 11,157 nodes) | 181 ms | 160 ms | **125 ms** |
| github.com (7,008 rules, 3,402 nodes) — match phase | 378 | 243 | **243** Mtsc |

**~31 % off Wikipedia's style pass**, and the dominant `match` phase fell
from 644 → ~426 Mtsc. `part_match` dropped 29 %, and each surviving call
is cheaper (no attribute re-scan).

Correctness: `-DCSS_VERIFY` re-runs the pre-index linear scan for every
node and compares — **0 mismatches** on the home page, Wikipedia
(11,157 nodes) and GitHub (3,402 nodes), with both fixes live.

## Honest limits

- These are TCG-emulated numbers (~10-50x slower than native); on real
  hardware the whole style pass is single-digit ms. The optimisations
  help proportionally, but "125 ms" is an emulator artifact.
- The remaining `match` cost is genuine selector work — 11,698 matches
  over a 953-rule sheet. Further gains (e.g. precomputed per-node class
  hashes so `class_attr_contains` compares hashes instead of tokenising)
  are diminishing and were not pursued.
- Layout (~60 ms) is next after match, but it was never the bottleneck
  the stale priority list implied.

## Where page-load time goes now (wikipedia, TCG)

| phase | ms |
|---|---|
| dom_build (661 KiB HTML) | 33 |
| collect + sheets (network) | 90 |
| **style pass** | **125** |
| layout | 60 |

Style is still the largest single phase, but it is no longer pathological
and it is provably correct.
