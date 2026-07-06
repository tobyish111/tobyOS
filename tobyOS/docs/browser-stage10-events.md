# Browser stage 10 — event loop, DOM events, timers, fetch

Branch: `browser-events` (off `browser-js`). Phase 10 of
`browser-engine-roadmap.md`: "where apps start working" — the page's
JS world now OUTLIVES the load and reacts to the user.

## Runtime lifecycle
The per-load throwaway runtime is gone: each tab owns a persistent
JSRuntime/JSContext (`tab.js_rt/js_cx`), created lazily when the page
has scripts and torn down on navigation (`render_html`/mono view),
tab close, and tab reset (`js_teardown` frees timers, the dispatcher,
context, runtime). The tab-shift struct copy moves ownership like the
engine/image pointers.

## The event loop
The browser's cooperative main loop gains `js_pump_all()`: for every
tab with a live JS world (flipping `g_active` like the image loader so
`__dom` primitives address the right engine) it runs due timers, then
drains the QuickJS pending-job queue (Promise microtasks). Microtasks
also drain after every JS entry point (scripts, events, timers), so
ordering is correct: microtasks before the next macrotask.
- **Timers**: real `setTimeout`/`setInterval`/`clearTimeout` backed by
  a 32-slot per-tab registry (`__dom.timer/untimer`), 15 ms loop
  resolution. `requestAnimationFrame` ≈ setTimeout(16ms).
- **queueMicrotask** via Promise.resolve().

## DOM events
- Display items now carry their source DOM node (`ditem.node`, set
  through inline/block/float layout), so a mouse-down in the content
  area hit-tests the TOPMOST item under the cursor (last in paint
  order) and dispatches a `click` that BUBBLES from the target up the
  ancestor chain — the prelude dispatcher runs `addEventListener`
  listeners AND legacy `onclick="..."` attributes (Function
  constructor), with `preventDefault()` (suppresses the default
  link/submit action) and `stopPropagation()`.
- Typing in form fields dispatches `input` events; `element.value`
  reads/writes the live field buffer (`__dom.getValue/setValue`).
- `Element.click()` dispatches synthetically; `DOMContentLoaded` and
  `load` fire on document/window listeners after the load scripts.

## Reactive re-render (closes the loop that makes JS visible)
Every mutating primitive sets a dirty flag; after events/timers/jobs,
`js_rerender()` runs: a LIGHT collect pass registers new links/images
(existing ones keep their indices — decoded image pixels survive;
JS-added `<img>` join the progressive loader), then re-cascade (JS-set
class/style attributes) and re-layout. The CSSOM is NOT rebuilt:
stylesheets added by scripts after load are ignored (documented gap),
as are JS-added forms/inputs.

## Network from JS
`fetch()` returns a real Promise (resolved from a synchronous kernel
HTTP fetch — honest blocking until an async HTTP ABI exists) with
ok/status/url/text()/json(); a minimal XMLHttpRequest shim rides on
the same primitive. 256 KiB cap, relative URLs resolve against the
page.

## Verified in QEMU (screenshots + serial)
`/js2` shows all of it at once: three `setTimeout`-driven synthetic
clicks advance a counter through a real click listener; `setInterval`
ticks 3x then `clearInterval` stops it; `fetch` → promise chain
updates the DOM from a live HTTP body; microtask order is
`micro1,micro2,macro`; a REAL QMP mouse click bubbles to a body
listener ("mouse: clicked <BODY>") through the C hit-test path;
navigating away tears down and rebuilds the JS world (phase-9 page
regression-clean).

## Test-rig note (QMP mouse)
The compositor scales PS/2 relative deltas by EXACTLY 3x — QMP drives
must send target/3 in small (≤4px) steps after a saturating reset to
(0,0). Landing on small buttons is still fiddly; prefer listeners on
larger targets (body) or synthetic `.click()` for determinism.

## Still ahead (phase 10 leftovers → phase 11)
True async fetch (kernel ABI), keyboard events beyond `input`
(keydown/keyup), `history`/`location` objects, storage
(localStorage), and re-running collect for JS-added stylesheets.
