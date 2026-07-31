# Handoff: HEADED Chrome on tobyOS — real YouTube UI parity (comments, thumbnails, click-through)

You are picking up a **working** Chromium-on-tobyOS system at its measured
ceiling: **everything headless Chrome can do, tobyOS now does.** The YouTube
watch page loads, metadata populates, and **video plays** (readyState 4,
20–24s buffered, decoded frames painted, zero crashes, verified 3/3 runs).
What does NOT appear — comments, populated sidebar thumbnails — does not
appear in real headless Chrome on a normal Windows machine either
(measured; see §2). The goal of THIS arc is to run **full headed Chrome**
on a real display surface so YouTube serves and renders those surfaces.

Read `docs/chromium-hypothesis-ledger.md` slices 56–60 first. Last commit of
the prior arc: `a8e7849`.

---

## 0. Prime directives (every one of these was paid for in runs)

1. **CONTROL FIRST.** Before attributing any behaviour to tobyOS, reproduce
   it on a known-good system. Host Chrome AND Edge exist on this machine
   (`/c/Program Files/Google/Chrome/Application/chrome.exe`). The prior arc
   burned ~8 six-minute guest runs and SIX dead theories on a "gap" that a
   60-second host control proved was not a defect at all.
2. **Never conclude from one run.** WHPX is non-deterministic; page-build
   completeness itself varies. `logs/run_x3.sh` builds once, runs 3x, and
   auto-tabulates — use it for any claim.
3. **Sanity-check every new probe field against a case where you know the
   answer.** Two probe false-positives (an expire detector, a consent-gate
   selector matching hidden dialogs) each cost multiple runs.
4. **Ask the app, don't infer.** `innerText` vs `textContent` vs
   `offsetHeight` name a DOM region's state outright. The CDP probe harness
   in `programs/chromewin/main.c` is rich — extend it rather than reasoning
   from counters.
5. **Batch questions.** One question per 6-minute boot cycle is the slowest
   possible science. The probe can carry a dozen fields.
6. Read `[prof]`/`[wait]`/`[cur]` before writing any performance or blocking
   conclusion. Idle-but-incomplete means WAITING, not slow.
7. Coherent "corrupt" bytes = a DIFFERENT SOURCE, not a mutation (the
   "corrupted URL" was YouTube's own second