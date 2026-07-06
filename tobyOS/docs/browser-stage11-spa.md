# Browser stage 11 — a real Preact SPA runs (roadmap milestone 4)

Branch: `browser-spa` (off `browser-events`). Proof milestone 4 from
the engine roadmap ladder: **an unmodified, off-the-shelf Preact 10 +
htm + hooks bundle (13 KB standalone UMD from unpkg) mounts, renders,
and reacts on tobyOS** — timer clicks drive `useState`, real keyboard
input flows through a controlled `<input>` (`onInput` → state →
re-render → `element.value` writeback), and an `onKeyDown` Enter
handler adds list items with `preventDefault` suppressing the form
default.

## What was added to get there

**DOM API gaps Preact's diff needs** (C primitives + prelude):
- `insertBefore(parent, child, ref)` (prev-scan on the singly-linked
  child list), `replaceChild`, `removeAttribute`, `hasAttribute`,
  `contains`.
- `childNodes` (ALL children incl. text nodes — `children` stays
  elements-only), `firstChild`, `lastChild`, `nextSibling`.
- Text-node mutation: `setText` on a `#text` node rewrites its pool
  slice (`nodeValue`/`.data` setters — Preact patches text this way).
- `nodeType` (1/3), `nodeName`, `localName` (Preact matches excess
  children by localName).
- `classList` (add/remove/toggle/contains over the class attribute),
  `style.setProperty`/`cssText`, `createElementNS`/
  `createDocumentFragment` fallbacks, `ownerDocument`.

**The listener-name trap (the one real bug)**: Preact decides whether
to lowercase an event name by probing `'onclick' in dom`; our wrappers
had no `on*` properties, so Preact registered listeners under
`"Click"`/`"Input"` — which a dispatched `"click"` never matched
(rendering worked, interaction was completely dead, zero errors).
Fixed by stubbing ~30 `on*` properties as null on Element.prototype
AND lowercasing in add/removeEventListener.

**Keyboard events**: `keydown` dispatches from both the focused-field
path (before default editing; a preventDefault swallows the key) and
the content path (before browser shortcuts), with DOM
`KeyboardEvent.key` names (`Enter`, `Backspace`, `Escape`, `Tab`,
`ArrowUp/Down/Left/Right`, `Home`, `End`, `Delete`, single chars) and
approximate `keyCode`.

**Navigation + environment**:
- `location` (href/protocol/host/pathname/search/hash/origin, assign/
  replace/reload) — `href` assignment is DEFERRED (`tab.js_nav`,
  executed by the pump/dispatch exit paths) because navigation tears
  down the running runtime.
- `history.pushState/replaceState` update the address bar + history
  without navigating (back/forward are no-ops for now — no popstate).
- `localStorage`/`sessionStorage`: in-memory per page load (SPAs need
  them to exist; persistence to /data is future work).

**Engine-side**:
- Script-added `<style>` now applies: `dnode.flags` gained a
  STYLE_DONE marker; the light (post-mutation) collect parses
  unprocessed style nodes into the rule pools — CSS-in-JS works.
- `<input>` registers as a field regardless of `<form>` (form-less
  inputs are legal; JS-created inputs become typeable via the light
  collect; submit paths guard `form < 0`).

## Verified in QEMU
`/spa` (Preact+htm+hooks todo/counter): initial mount with page CSS;
two `setTimeout`-driven `.click()`s advance the `useState` counter to
2; QMP-typed "neko from qemu" flows through the controlled input
(`typing: [neko from qemu]` echo live per keystroke); Enter adds the
item, clears the input, list shows 2 items; localStorage counter and
`location.pathname` render in the footer. Phase-10 `/js2` and
phase-9 `/js1` pages regression-clean.

## Notes
- Preact `render()` intentionally appends (never clears containers) —
  demo pages should mount into an empty element.
- Preact's `_listeners` object lands on our cached per-node wrapper
  (wrapper identity is stable by node index) — that identity is what
  makes its `eventProxy` (`this._listeners[e.type + false]`) work.
- Still ahead for bigger SPAs: popstate/back, persistent storage,
  keyup, focus events, true async fetch, SVG.
