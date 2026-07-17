# Roadmap: a Chromium-class engine + V8-class JavaScript for tobyOS

A build-and-test plan for the largest possible browser undertaking: a
from-scratch rendering engine with a JIT-compiling JavaScript engine, to
the point where real JavaScript web apps (React/Angular/Polymer SPAs)
run and are usable — i.e. matching what Edge/Chrome do, not just the
content-rendering ceiling the current tobyOS browser reaches.

This document is written for autonomous agents. It gives the
architecture, the dependency-ordered build phases, the walls, the
testing/conformance strategy, and how to parallelize the work. Read
section 0 before anything else — it contains a strategic fork that
changes everything downstream.

---

## 0. Read this first: scope reality and the strategic fork

**Magnitude, stated plainly.** Chromium is ~35 million lines; V8 alone is
~1.5 million. Both represent thousands of engineer-years and are
maintained *continuously* (see docs/browser-visual-punchlist.md and the
"how browsers auto-update" context: a dedicated team + build farm +
staged rollout keeps them current every ~4 weeks). Writing an equivalent
from scratch is not a feature; it is a multi-year, multi-agent program
the size of the rest of tobyOS combined. This document does not pretend
otherwise. It exists because the work was requested; it is honest about
where the effort actually goes.

**Two properties make this categorically harder than the incremental
parity work this project has done well:**

1. **Correctness coupling — almost no partial credit.** A content page
   degrades gracefully when you get a detail wrong (a missing property
   just doesn't apply). A web *app* does not: if `getBoundingClientRect`
   is off by a pixel, or a Promise resolves one microtask early, or the
   event order differs, the app throws and the page goes blank. You need
   *high* fidelity across a *huge* surface before any complex app works
   at all. This inverts the usual 80/20 — the last 20% of fidelity is
   the gate, not the polish.

2. **Moving target.** The web platform ships every ~4 weeks and Chrome
   auto-updates to match. A burned OS image cannot chase it. **Mandatory
   first decision: pick a spec snapshot** ("the web platform as of date
   X") and freeze the whole program to it. Document every deviation.
   Without this the target never stops moving and nothing is ever "done."

### The fork in the road — decide before writing one line of code

- **Option A — port the whole of Chromium (or WebKit/Servo).** Least
  *novel* code. The work becomes a porting job: shim tobyOS's freestanding
  libc/pthreads/mmap/graphics under Chromium's build system, satisfy its
  POSIX/Windows platform assumptions, and bring up its multi-process
  model. Still enormous, but it is porting, not invention — and it is
  what any rational team does. Cost: loses "from scratch"; inherits a
  build system that assumes a full OS.

- **Option B — write all of it from scratch.** What the bulk of this
  document covers. Maximum novelty, maximum cost, honestly the worst ROI.
  Included because it was asked for.

- **Option C — port V8 only, keep our from-scratch engine (RECOMMENDED
  if the real goal is "run web apps").** Keep tobyOS's existing
  DOM/CSS/layout/paint (which already render content sites near-parity —
  see the browser-*.md docs) and bolt a V8-class JIT underneath via a new
  bindings layer. This buys the *one load-bearing thing* — a fast JS
  engine — without writing a compiler from scratch, and lets the existing
  engine grow into the app-platform surface incrementally. It is the only
  option on this list with a realistic finish line.

**Recommendation:** if the goal is capability ("apps run"), do Option C.
If the goal is the artifact ("we wrote a browser engine from scratch"),
do Option B with eyes open and expect it to be a standing program, not a
project. The rest of this document is written for **Option B** (the full
ask) but flags at each phase what Option C would keep vs replace.

---

## 1. OS prerequisites (build these in the kernel/platform first)

The engine assumes an OS surface tobyOS only partly has. Land these
before the engine work, or the engine phases stall on them.

- **Real threads + atomics + futexes + TLS.** V8 (GC, background compile)
  and the compositor are multi-threaded. tobyOS has SMP and threads
  (see memory); verify atomics, a futex-equivalent, and thread-local
  storage are solid under contention.
- **W^X executable memory.** The JIT allocates pages RW, writes machine
  code, then flips them to RX (never W+X simultaneously — that is both a
  correctness and a security requirement). Need an `mprotect`-class
  syscall and an executable-page allocator.
- **A large, fast heap + memory-pressure signals.** The GC needs a big
  address space, a good allocator, and a way to learn the system is under
  memory pressure so it can collect harder.
- **High-resolution monotonic clock + timers.** For the event loop,
  `performance.now()`, rAF pacing, and profiler-guided JIT tiering.
- **A multi-process model + fast IPC + a sandbox.** Chromium isolates the
  renderer (untrusted content) from the browser (trusted, owns the OS).
  An engine this size *will* have memory-safety bugs; the sandbox is the
  containment. tobyOS has processes and fork/CoW/signals (memory) — needs
  a low-latency IPC channel and a renderer sandbox that drops privileges.
- **A GPU path (eventually).** See Phase P. Start CPU-only; treat GPU as
  a later tier.

Option C note: C still needs threads + W^X + memory pressure for V8.
It can defer the sandbox/multi-process work longer since the existing
engine already runs in one process.

---

## 2. Phase J — the JavaScript engine (V8-class) — THE CRITICAL PATH

This is the load-bearing phase. Nothing else matters until this exists,
and it is the majority of the total effort. Build it in tiers; each tier
is independently testable and shippable.

- **J1 — Front end.** Lexer, parser, AST, scope analysis, early-error
  detection. Full modern ECMAScript grammar (destructuring, modules,
  async/await, generators, optional chaining, classes, BigInt literals).
  *Test:* the parsing subset of **test262**.

- **J2 — Bytecode + interpreter (Ignition-class).** A register-based
  bytecode, a generator from AST → bytecode, and an interpreter loop.
  **Design the object model with hidden classes/"maps" (shapes) and
  inline caches from day one** — property access speed and every later
  JIT optimization depend on them, and retrofitting them is a rewrite.
  *Test:* runtime subset of test262.

- **J3 — Runtime, builtins, regexp, GC.** The full object model
  (prototypes, property attributes, getters/setters, Proxy, Reflect), all
  builtins (Array, Object, String, Number, Map/Set/WeakMap/WeakSet,
  TypedArray/ArrayBuffer/DataView, Promise, Symbol, BigInt, JSON, Date,
  Intl subset), a **regexp engine** (Irregexp-class — itself a small
  compiler with backtracking + a bytecode/JIT), and a **garbage
  collector**. Start the GC as a precise mark-sweep-compact; it *must*
  evolve to **generational + incremental + concurrent** (Orinoco-class)
  because stop-the-world pauses destroy app frame rates. GC design also
  dictates the bindings layer (Phase B) — cross-heap references must be
  traceable. *Test:* full test262 — and it should *pass the large
  majority*; this is the JS engine's report card.

- **J4 — Baseline JIT (Sparkplug-class).** A non-optimizing compiler that
  maps bytecode ~1:1 to machine code with no IR. Large constant-factor
  speedup for low complexity. Needs a **macro-assembler** for x86-64
  (and any other target ISA) and the W^X pages from Phase 1. *Test:*
  benchmarks show the speedup; correctness suites unchanged.

- **J5 — Optimizing JIT (Maglev/TurboFan-class).** The subtle,
  bug-dense heart of the engine, and a multi-year sub-project on its own:
  an SSA IR, **type feedback** gathered from inline caches, **speculative
  optimization** (assume types stay stable, compile fast paths), and —
  critically — **deoptimization**: when a speculation is violated at
  runtime, bail out to the interpreter reconstructing exact interpreter
  state (stack, locals, this) at that bytecode offset. Deopt must be
  correct for *every* operation or you get silent miscompiles, which are
  both wrong answers and exploitable security bugs. Plus inlining, escape
  analysis, range analysis, a register allocator, and a codegen backend
  per ISA. *Test:* **Speedometer / JetStream / Octane** for speed;
  **differential fuzzing** (run random JS through interpreter and JIT,
  compare results — the standard way real engines find miscompiles) for
  correctness.

- **J6 — WASM.** tobyOS already vendors wasm3 (interpreter + our SIMD
  work — see docs/browser-wasm.md). A V8-class engine wants baseline +
  optimizing WASM compilers, but a pragmatic path is to keep wasm3 for
  correctness and skip JIT'd WASM until J5 is stable.

**Option C:** this whole phase *is* V8 — port it instead of writing it.
The port work is: build V8 standalone against tobyOS's freestanding
toolchain, satisfy its platform layer (`src/base/platform` — threads,
time, mmap, page allocation, W^X), and wire its embedder API. That is a
hard but *bounded* port versus an unbounded compiler project.

---

## 3. Phase B — DOM, HTML parsing, and the bindings layer

- **B1 — HTML5 parser.** The spec tokenizer + tree-construction algorithm
  with full error recovery. It is literally a state machine — implement
  it to the letter (do not hand-roll a "good enough" parser; app pages
  rely on exact error recovery). *Test:* **html5lib-tests**.

- **B2 — DOM core.** Node/Element/Document/DocumentFragment/Text/etc., the
  full interface set, live `NodeList`/`HTMLCollection`, `Range`,
  `TreeWalker`, `MutationObserver`, custom elements + Shadow DOM (tobyOS
  has a subset — see docs/browser-shadow-dom.md). *Test:* WPT `dom/`.

- **B3 — The bindings layer (the underestimated giant).** The connective
  tissue between the JS engine and C++ DOM objects: Web IDL definitions, a
  **code generator** that emits the wrapper objects, and **lifetime/GC
  integration** — a DOM node reachable from JS must be kept alive and
  *traced by the JS GC*, and reference cycles that cross the JS↔C++
  boundary must be collected. In Chromium this is millions of generated
  lines. Budget for it explicitly; teams routinely underestimate it by an
  order of magnitude.

- **B4 — The event loop (must be spec-exact).** Tasks vs microtasks with
  the *exact* ordering the HTML spec mandates, `queueMicrotask`, Promise
  job ordering, `requestAnimationFrame` timing relative to layout/paint,
  timer clamping. Apps depend on this ordering; "close enough" throws.
  tobyOS has a cooperative loop (docs/browser-first-paint.md) — this
  replaces it with a spec-faithful one. *Test:* WPT `html/`.

**Option C:** keep tobyOS's existing DOM (it works for content), but
**rewrite the bindings** to target V8's embedder API instead of QuickJS —
this is the main integration cost of Option C, and it is where the GC
tracing across the boundary must be gotten right.

---

## 4. Phase C — CSS (large head start already exists)

tobyOS already has a real CSS engine: tokenizer/CSSOM, selector matching
with `:not()` + sibling combinators + specificity + a bloom-indexed
cascade, computed values, custom properties with correct
invalid-at-computed-value-time semantics, `@media` with calc/range
syntax, gradients, masks (see browser-css-scale.md, browser-light-dark.md,
browser-media-eval.md, browser-not-selector.md). Carry it forward.
Remaining for parity: `@supports`, `@container`, cascade layers,
`:has()`, the full property set, subgrid, container queries, color-mix,
and exact computed-value serialization (apps read `getComputedStyle`).
*Test:* WPT `css/`.

---

## 5. Phase L — layout (LayoutNG-class) + text

- **Box/layout tree + formatting contexts.** Block, inline, flex, grid,
  table, floats, positioning (tobyOS has usable subsets — see
  browser-stage12-*.md, browser-measure-float.md, browser-clip-shift.md),
  plus fragmentation, writing modes, and correct intrinsic sizing
  (min/max-content). The bar is not "looks right" — apps *measure* boxes
  and react, so layout must match the reference engine's numbers closely.

- **Text layout is its own sub-project.** Real **shaping** (HarfBuzz-class:
  glyph selection, ligatures, kerning, complex scripts like Arabic/Indic),
  **bidi** (UAX#9 / an ICU-class implementation), **line breaking**
  (UAX#14), and font matching/fallback. tobyOS has UTF-8 + a fallback face
  + basic advance-width measurement (docs/browser-utf8-text.md) but no
  shaping/bidi/kerning — this is a large, genuinely hard addition.

- *Test:* WPT `css/` reftests (reference-image comparisons) + the
  **Edge-oracle harness already built** (tools/compare) extended to
  per-element `getBoundingClientRect` diffing against Edge, and a much
  larger site corpus.

---

## 6. Phase P — paint, compositing, graphics

- **P1 — 2D rasterizer (Skia-class).** Paths, fills/strokes, gradients,
  image blends, antialiasing, clipping (tobyOS has clip rects), text
  rasterization, color management. tobyOS has a software compositor +
  alpha blits + the deco/paint path; a full 2D library is a large lift.
- **P2 — Display-list paint + layerization + a compositor thread.** So
  scrolling and CSS animations run at 60fps *independently of the main
  JS thread* — apps expect this and jank badly without it.
- **P3 — GPU.** A GPU process + a GL/Vulkan-class driver stack + GPU
  raster, then WebGL/WebGPU on top. tobyOS has an i915-lite path on real
  gen7 hardware (docs/igpu-i915lite.md) — reaching a general GL from there
  is a major driver effort. Start CPU-only; make GPU a late tier.
- *Test:* reftests, pixel tests, later the WebGL conformance suite.

---

## 7. Phase M — media

MSE (Media Source Extensions), the demux→decode→present pipeline (tobyOS
has openh264 H.264, Helix AAC, MP4 demux, AV1 via libgav1, PTS A/V sync —
see the media docs), audio output, and EME/DRM. **Wall: Widevine is
closed-source** — Netflix-class DRM playback is unreachable from scratch;
only Clear Key (the open EME key system) is achievable.

---

## 8. Phase A — the rest of the web platform (breadth)

Fetch/Streams/Cache, Workers/Service Workers, WebSockets, IndexedDB,
Web Components, Canvas2D, Web Audio, WebRTC (large), Intersection/Resize
Observer, History/Navigation, Web Crypto, Web Animations, Pointer/Touch
events, Clipboard, Permissions/Notifications, storage quotas, and dozens
more. tobyOS already has usable subsets of many (see the browser-*.md
docs). **Strategy: breadth-first, driven by what real target sites
actually touch** — use the compare harness + the JS error log to
prioritize exactly as this project already does (the `performance`,
`Image`, `self`, `URL` shims were all found this way). Do not implement
APIs speculatively; implement what unblocks a target site, measured.

---

## 9. Phase S — process model, sandbox, security

Not optional for visiting untrusted sites. Multi-process (browser /
renderer / GPU / network / utility), **site isolation** (cross-origin
docs in separate processes), **sandboxed renderers** (a compromised
renderer must not own the kernel), a security-reviewed IPC boundary, CSP,
CORS (subset exists — docs/browser-fetch-cors.md), mixed-content
blocking, SameSite cookies, and cert validation (tobyOS has TLS 1.3 —
docs/tls13-engine.md). Safe Browsing needs list-distribution infra
(the component-updater story) — likely drop it or stub it.

---

## 10. Testing & conformance strategy (runs through every phase)

Stand this up **first**, before building, so every step has an objective
score. This is the single most important process decision.

- **test262** — JavaScript conformance (tens of thousands of tests). The
  JS engine's report card; track pass rate as the Phase J north-star.
- **Web Platform Tests (WPT)** — DOM/HTML/CSS/APIs (>1M subtests). The
  platform report card; run continuously, track pass rate per area.
- **Reftests / pixel tests** — layout + paint correctness.
- **Benchmarks** — Speedometer (the realistic app benchmark; the "can it
  run React" proxy), JetStream, Octane. The JIT tiers are judged here.
- **The Edge-oracle harness** (tools/compare, already built and
  documented in browser-compare-harness.md) — real-site visual
  regression. Extend it to per-element rect diffs and a large corpus.
- **Differential fuzzing** — JIT vs interpreter on random JS (finds
  miscompiles); HTML/CSS/DOM fuzzing under sanitizers (finds the
  memory-safety bugs that the sandbox exists to contain). This is how
  real engines find the bugs that matter; it is not optional at this
  scale.
- **CI discipline** — mirror this project's proven workflow: one feature
  per branch, verify before merge, a shared pass-rate dashboard, no
  regression merges. The `-DCSS_VERIFY`-style "prove the fast path equals
  the reference path" pattern generalizes to every phase.

---

## 11. Multi-agent orchestration

**Dependency graph (what gates what):**

```
  Test-infra  ──────────────────────────────────►  (start FIRST)
  OS prereqs (threads, W^X, IPC, sandbox)  ──────►  gates J4/J5, S
  J: JS engine ─────────────┐
                            ├─► B: DOM + bindings ─► A: platform APIs
  C: CSS ──► L: layout ─────┘         │
                    └─► P: paint/gpu   └─► (needs J object model + GC)
  S: process/sandbox/security ─── parallel from early
```

- **J (JS engine) is the critical path.** It gates everything that runs
  script. Staff it heaviest and start it earliest after test-infra.
- **B (DOM + bindings) depends on J's object model + GC tracing.** Do not
  start bindings before the GC design is fixed.
- **C → L → P** can run as their own track largely parallel to J (tobyOS
  already has a real CSS/layout base to extend), joining J at the bindings
  layer.
- **S (process/sandbox)** can proceed in parallel from early on.
- **Test-infra must start before everything** so each track has a score
  from day one.

**Suggested agent tracks (each with its own persistent memory/design
doc, like this project's memory files):** JS-engine, DOM+HTML+bindings,
CSS+layout+text, paint+graphics, platform-APIs (breadth-first),
security+process, and test-infra.

**The context-limit reality:** no single agent (or human) holds 35M LOC
of design in working memory. Each track needs an authoritative, living
design doc; a top-level architecture doc must stay canonical; and
integration bugs — not feature bugs — will dominate because of the
correctness-coupling problem (GC × bindings, layout × paint, JIT ×
deopt, event-loop × everything). Budget the majority of late-stage time
for cross-boundary debugging, not new features.

---

## 12. Milestones (how you know you are progressing)

- **M0** — Test harness (test262 + WPT + benchmarks + oracle) running and
  scored; spec snapshot frozen; OS prereqs (threads, W^X, IPC) landed.
- **M1** — JS engine passes >90% of test262 in the **interpreter tier**
  (no JIT). Already a massive milestone; Option C reaches it by porting.
- **M2** — DOM + HTML + bindings pass a strong WPT `dom/`+`html/` subset;
  scripted pages mutate the DOM and re-render correctly.
- **M3** — **Baseline JIT** lands; Speedometer runs (slowly); real content
  sites' light JS executes without timing out.
- **M4** — Layout reaches LayoutNG-parity on WPT `css/` + reftests; the
  oracle harness shows content sites at near-pixel parity.
- **M5** — **Optimizing JIT** lands; Speedometer within ~3–5× of V8; **a
  real React app boots and is usable.** ← the "run web apps" goal.
- **M6** — GPU raster + compositor thread; 60fps scroll/animation; WebGL.
- **M7** — Sandbox + site isolation hardened; safe to browse untrusted
  sites.

At small team/agent size these milestones are *years* apart. M1 and M5
are the two that matter most: M1 proves the engine exists; M5 is the
finish line for the stated goal.

---

## 13. Off-ramps and decision points

- **After M1:** if the JS engine alone consumed the realistic budget
  (likely), stop and reconsider **Option C** — you will have proven you
  *can* build an engine while learning it is not the efficient path to
  the goal.
- **The DRM wall:** Widevine is closed; premium streaming is unreachable
  from scratch. Ship Clear Key only and document it.
- **The maintenance wall:** even at parity, a frozen spec snapshot drifts
  from the live web. Either budget standing maintenance (a team, forever —
  which is what Google/Microsoft do) or accept a browser that slowly ages.
- **The honest bottom line, restated:** if the goal is *capability*
  ("apps run"), **Option C — port V8 under our existing engine — reaches
  it for a small fraction of Option B's cost and is the path I would
  actually take.** Option B (full from-scratch) is worth doing only if the
  from-scratch *artifact itself* is the point, and only as a standing
  program rather than a finishable project. This project's own track
  record — bounded, measured, instrument-first slices that "stay done" —
  is the opposite of what Option B is, which is the clearest signal about
  which option fits.
