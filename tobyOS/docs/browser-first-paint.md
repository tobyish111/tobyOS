# First paint before scripts + JS watchdog

Punch-list item #1 (see browser-visual-punchlist.md): bbc.com/news fetched
its HTML in 2 s and all 46 subresources by ~32 s, yet at 70 s the tab still
showed the previous page and "Loading…" — CPU pegged, memory draining.

## Root cause

Two compounding behaviors:

1. `render_html` runs dom → sheets → style → layout → **run_scripts**
   synchronously, and nothing painted the window until the whole chain
   returned. The server-rendered page — which Edge paints immediately —
   was held hostage by script execution.
2. BBC's React boot grinds effectively unboundedly on the QuickJS
   interpreter (TCG makes it worse). With no execution limit, the tab
   never reaches "Done".

## Fix

1. **First paint** (`render_html`): after the initial style+layout and
   before `run_scripts`, `update_title()` + `tk_redraw(&win)`. Scripts
   that mutate the DOM still trigger the usual re-render afterwards.
2. **JS watchdog**: `JS_SetInterruptHandler` on the page runtime; the
   handler trips when `js_now_ms()` passes `g_js_deadline`. Budgets:
   - `JS_LOAD_BUDGET_MS` = 20 s for the *total* page-load script pass
     (wiki/github complete in a few seconds under TCG; only runaway
     bundles hit it);
   - `JS_TICK_BUDGET_MS` = 3 s per timer/fetch-callback pump pass and
     per DOM event dispatch.
   On expiry the current eval aborts with an exception (logged once:
   `[js] page-script budget exhausted…`); the page keeps its
   server-rendered content — degrade like a no-JS browser, never a hung
   tab. Entry points save/restore the deadline, so nesting is safe.

## Limits

- QuickJS polls the interrupt from the interpreter loop: parse time and
  blocked sync fetches inside an eval are not interruptible (execution
  after them is).
- Worker runtimes are not budgeted (worker scripts here are small).

## Verification

- bbc.com/news: before = home page + "Loading…" forever; after = BBC's
  server-rendered page paints (harness composite), budget log appears,
  browser stays responsive.
- Regressions: wikipedia composite unchanged, home page renders, ESM
  self-test 3× ALL PASS (its scripts finish far under budget).
