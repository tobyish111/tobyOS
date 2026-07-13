# Web Components: template, custom elements, shadow DOM, slots

Branch `browser-shadow-dom` (off `browser-css-anim`). The browser engine
(programs/user_gui_browser/main.c) gains the Web Components trio in three
slices, all QEMU-verified on screen.

## Slice 1 — `<template>` + custom elements (DONE)
- **`<template>` is inert**: `collect_node` skips the subtree (no
  link/media/canvas/style collection) and the style pass forces
  `display:none` + skips styling the content. Content only renders once
  cloned out.
- **`D.clone(i)`** (`dom_clone_rec`): deep subtree clone; text/attr
  slices share the immutable tpool (a later `dom_set_attr` on a clone
  rewrites its own attr block, copy-on-write).
- **Custom tag names survive**: dnode tags are a fixed enum, so
  `<x-counter>` maps to `T_UNK` and the source name would be lost. The
  parser (and `D.create`) stash the lowercased name as a synthetic
  `__tag` attribute when it contains `-`; `D.tag()` reports it, and
  `D.byTagFrom(name, from)` scans nodes by it (drives upgrades).
- **Prelude `customElements`**: `define()` registers the class and
  upgrades existing elements (polyfill-style constructor-stack: the
  `HTMLElement`(=Element) constructor pops the node index being
  upgraded); `document.createElement` routes defined names through the
  class; `appendChild`/`insertBefore`/`innerHTML` re-run upgrades under
  the touched subtree. `connectedCallback` fires when an instance is
  (or becomes) reachable from body; `disconnectedCallback` on
  `removeChild`; `attributeChangedCallback` via `setAttribute` +
  `static observedAttributes`. `template.content` is a lightweight
  fragment handle whose `cloneNode(true)` stamps the template's
  children on append.
- **UA deviation**: custom elements default to `display:block` (spec:
  inline). This engine has no block-in-inline splitting, and virtually
  every real component's first style line is `:host{display:block}`
  anyway. Author CSS overrides (pre-cascade default).

Verified: template stamped twice (exactly two copies on screen, none
from the inert template itself); parsed `<x-hello name=world>` upgraded
at `define()` with `connectedCallback` seeing the right `tagName` and
attributes; dynamic `createElement` + `setAttribute` fired
`attributeChangedCallback` then `connectedCallback` on append.

## Slice 2 — `attachShadow` + style encapsulation (DONE)
- **`D.attachShadow(host)`**: creates the shadow root as a real block
  child recorded in `dnode.shadow`. The composed tree is driven from
  the style pass: a shadow host's other (light) children — element and
  text — are forced `display:none`, so layout renders only the shadow
  tree. Event bubbling keeps working through real parent links.
- **Scoped styles**: `crule` gains `scope` (the shadow-root node, -1 =
  document). `collect_node` tracks the current scope while walking (a
  shadow tree's `<style>`/`<link>` sheets stamp their rules with it);
  `style_node` tracks the same scope and skips author rules whose scope
  differs — document rules stop at shadow boundaries, shadow rules stay
  inside. UA rules apply everywhere. Inheritance still flows normally
  from the host into the shadow tree.
- Prelude: `Element.attachShadow({mode})` returns the shadow-root
  wrapper (an ordinary Element: `innerHTML`/`appendChild` work), with
  `.host`; `element.shadowRoot` getter (mode is ignored; always open).

## Slice 3 — `<slot>` projection (DONE)
- New `T_SLOT` tag. `distribute_slots()` runs before every style pass
  (initial render + every JS rerender): for each shadow host, each
  light child moves under the matching slot in its shadow tree
  (`slot="x"` attr vs `<slot name=x>`; unslotted children and text go
  to the unnamed slot). Idempotent — moved children are no longer light
  children. A slot's fallback children render until its first
  assignment (then they are dropped); unassigned light DOM stays
  hidden. `<slot>` defaults to `display:block` (same deviation).
- **v1 deviations (documented)**: distribution is physical — a
  distributed node's `parentNode` becomes the slot and it styles in the
  shadow scope (spec: document scope); there is no un-distribution when
  slots change; `assignedSlot`/`slotchange` are not implemented;
  `:host`/`::slotted` selectors are not supported.

## Verified (slices 2+3 together)
A page with a `<template>` carrying a shadow stylesheet
(`.card-title{color:#ffe08a;background:#254f9e}`), a document stylesheet
with a CONFLICTING `.card-title{background:#cc2222}`, and two `<x-card>`
custom elements whose `connectedCallback` does
`attachShadow` + template stamp:
- card 1 (`<span slot=body>` + unslotted span): SHADOW TITLE renders
  yellow-on-dark-blue (shadow rule wins inside; the document's red rule
  does NOT leak in), LIGHT BODY A replaces the named slot's fallback,
  LIGHT DEFAULT A lands in the default slot;
- card 2 (no light children): both slot fallbacks render;
- a document-level `.card-title` div below renders red — the shadow
  rule does NOT leak out.

## Remaining / follow-ups
`:host` and `::slotted()` selectors, `slotchange` events, closed-mode
enforcement, un-distribution on slot changes, `getRootNode()`,
event retargeting at shadow boundaries.
