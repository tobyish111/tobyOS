# CSS animations in the browser (arc)

Branch `browser-css-anim`, off `main`. Adds CSS animation to the engine —
the last of the "feels modern" visual gaps. This arc is sliced:
1. **`@keyframes` animations** (DONE) — driving the *cheap*
   properties that work in the immediate-mode painter.
2. **CSS transitions** (DONE) — change-detection on restyle.
3. **`transform: scale`/`rotate` + `opacity`** (DONE) — per-element
   compositing layers.

## Slice 1 — `@keyframes` animations (DONE)
An element with `animation:` runs autonomously on an animation clock; no
change-detection needed. Supported animatable properties (chosen because
they don't need new paint machinery): `transform: translate` (px), `color`,
`background-color`.

### How it works
- **`@keyframes` registry** (`g_kf[]`, parsed in `css_parse_sheet`): each
  named set holds up to 8 stops; each stop carries only the animated subset
  (translate px, color, bg). `from`/`to`/`N%` selectors, comma-shared
  stops, stops sorted by percent. Reset per page in `page_reset`.
- **`animation` shorthand + longhands** (`CP_ANIMATION`, `animation-name`/
  `-duration`/`-iteration-count`/`-delay`/`-direction`/`-timing-function`)
  parse into non-inherited `cstyle` fields (`anim_kf`/`anim_dur`/
  `anim_delay`/`anim_iter`/`anim_flags`). `<time>` accepts `s`/`ms`;
  `infinite` → 0 iterations; `alternate` and `ease*` set flags.
- **The clock** (`anim_apply`, run right after the style pass and before
  layout in both `render_html` and `js_rerender`): for each animating
  node, compute elapsed → cycle/fraction (with delay, iteration count,
  `alternate` reversal, and an integer smoothstep for `ease-in-out`),
  find the surrounding keyframe stops, interpolate, and overwrite the
  node's computed `transform`/`color`/`bg` **before layout bakes them**.
  Sets `g_anim_active` while anything is still running. Per-node start
  times live in `g_anim_t0[]`.
- **The pump** (`anim_pump`, in the main loop next to `media_pump`):
  while `g_anim_active`, a full restyle→relayout→repaint per frame
  (~30 fps), the same reflow path JS uses — necessary because
  transform/color are baked at layout, not read at paint.

### Verified
Built with the browser; a host page with
`animation: slide 2s infinite alternate ease-in-out` (translateX 0→210px)
and `animation: huepulse 3s infinite` (background-color through three
color stops) fetched over SLIRP. Screenshots ~3 s apart show the box at
different x positions and the pulse box at different interpolated colors,
with the GUI monitor counting frames — the clock drives continuous reflow.

### v1 limits
Translate/color/background only; scale/rotate and `opacity` need the
compositing work in slice 3. `animationend`/`transitionend` aren't fired
yet (the events are registered in the prelude; wiring the dispatch is a
follow-up). Per-frame full reflow is fine for a few animated elements but
not free — a display-list-only fast path is a later optimization.

## Slice 2 — CSS transitions (DONE, on-screen verified)
The engine side: `transition` shorthand + longhands parse into
non-inherited `cstyle` fields (`trans_mask`/`trans_dur`/`trans_delay`/
`trans_flags`); a per-node `g_trans[]` runtime holds from/to/current per
transitionable property (color, background-color, transform: translate);
`trans_apply` runs after `anim_apply` and before layout, detects when a
freshly-cascaded value differs from its target, and eases from the current
displayed value to the new one (shared `css_ease` smoothstep), overriding
`st` and keeping `g_anim_active` set while running.

### Verified
Host page with `transition: background-color 2s linear, transform 2s
linear` whose inline style is toggled by a `setInterval(fn, 6000)` — JS
sets `backgroundColor` `#ee3333`↔`#3399ff` and `transform`
`translateX(0)`↔`translateX(190px)`. 32 QMP screenshots 2 s apart across
~5 toggle cycles show the box **easing**: e.g. idle `x=5 #ee3333` →
mid-transition `x=107 #896aa1` (the exact 50% linear lerp of the two
colors) → settled `x=195 #3399ff`, then back through `#4192f0`/`#c34a61`.
Serial instrumentation confirmed the whole chain: `setInterval` fires
every 6 s of guest time while the page is idle, `trans_apply` starts the
per-node transitions on each toggle, and `anim_pump` renders ~30 fps
(32 ms throttle) while `g_anim_active`, dropping back to 0 frames when the
transition completes.

### The earlier "verification BLOCKED" was a harness artifact, not a bug
A previous attempt concluded idle `setInterval` never fires, and that a
keydown-triggered transition freezes the render loop. Instrumenting the
main loop disproved all of it — the loop iterates ~65×/guest-second when
idle, timers fire on schedule, and JS-triggered transitions sustain
`anim_pump` exactly like initial-render `@keyframes`. The real causes:
1. **The test page bound the toggle handler twice** —
   `document.addEventListener('keydown', ...)` *and* `window.onkeydown`.
   Keydown bubbles to both (spec-correct), so one keypress toggled twice
   and the 1 s transition instantly reversed back to its start color:
   the screen "never changed" because the test canceled itself.
2. **The rig's wall-clock window was too short.** `TKA_PUMP` deadlines
   run on PIT ticks, and under heavy TCG load PIT interrupt delivery lags
   ~10× behind the TSC/wall clock (userland `js_now_ms` → `SYS_CLOCK_MS`
   → `perf_now_ns` is TSC-based and tracks wall time). The TKAPP
   typing/hold phases therefore consumed nearly the whole driver window,
   and the run ended before a 1 s interval timer was ever observed.
3. The "0 fps freeze / ~15% real speed starvation" was the same skew plus
   full-desktop composites costing seconds of wall time under TCG —
   sparse frames, not a starved process or a stuck loop.

Harness rules that came out of this (used by the verifying rig
`drive_trans3.py`/`websrv_trans3.py`): make the page self-driving via
`setInterval` (≥6 s period so a 2 s transition completes between
toggles), give the driver a long GET deadline (240 s) and a long
screenshot window (32 shots × 2 s), and never bind the same test handler
at two bubble levels.

### Kernel fix that fell out: TSC calibration vs a lagging PIT
Three later clean-build runs looked **wedged** (serial + repaints stopped
seconds after boot; heartbeat printed once and never again) — QMP
`info registers` on the "wedged" guest showed it was alive and healthy
(browser parked in `sys_nanosleep`, kernel in the yield path) but
`perf_now_ns` values in the registers advanced at **6.4% of wall time**,
with `g_tsc_khz` ≈ 67 GHz on a ~4.3 GHz host. Root cause: `perf_init()`
calibrates the TSC by counting cycles across 5 **PIT interrupts**; under
loaded QEMU TCG those IRQs arrive many times late, so the measured rate
is inflated by the same factor and the ENTIRE OS clock (`SYS_CLOCK_MS` →
`js_now_ms`, `sys_nanosleep`, heartbeats, service timers) runs slow by
that factor for the whole session. Run-to-run variance of that boot-time
lag is exactly the long-standing "TKAPP boot intermittently stalls"
flake, and it produced every timing symptom above.

Fix (this branch): `perf_recalibrate_pmt()` in `src/perf.c` re-measures
the TSC rate against the ACPI PM timer — a free-running 3.579545 MHz
counter read by port I/O, immune to IRQ delivery — across a 50 ms
window, then rebases `g_boot_tsc` so the `perf_now_ns` timeline stays
continuous. `parse_fadt` now captures `PM_TMR_BLK` (X-GAS preferred,
`TMR_VAL_EXT` for 24- vs 32-bit width), and `smp_init_bsp` calls the
recalibration right after `acpi_init` — before `apic_init_bsp`, so the
LAPIC timer calibration (`pit_sleep_ms` → `perf_now_ns`) also inherits
the honest rate. The boot log prints the correction factor
(`[perf] TSC recalibrated vs ACPI PM timer: ...`).

### Kernel fix #2: pid 0 stopped scheduling untracked procs
With the clock honest, transitions rendered on screen (eased frames in
the screenshots) — but the browser then froze mid-transition, minutes
in, at random. Heartbeat diagnostics showed browser + the `httpa`
kernel worker READY **on** cpu 0's ready queue with cpu-time/syscall
counters frozen for 40+ s while pid 0 idled in HLT: nothing was popping
the queue. Root cause: `gui_tick`'s pid-0 cooperative yield was gated
on `any_tracked_alive()` — "is a *desktop-launcher-tracked* app still
alive" — but TKAPP-harness session apps (`winpe_spawn_session_app` →
`proc_spawn`, no `track_pid`) and kernel workers are not in
`tracked_pids`. The only tracked proc was login; when its service
restarts ended, pid 0 stopped yielding permanently and every runnable
proc starved. The stall never bit inside TKA_PUMP holds (that pump
calls `sched_yield` unconditionally), which is why all previous
short-window TKAPP rigs passed — and it is very likely the real
identity of the long-standing "TKAPP boot intermittently stalls" flake.
Fix: `gui_tick` pid-0 path now calls `sched_yield()` unconditionally
(the scheduler's fast path returns in ~25 cycles when the ready queue
is empty, so the tracked-alive gate saved nothing); the dead
`any_tracked_alive()` helper is removed.

## Slice 3 — `transform: scale`/`rotate` + `opacity` (DONE, on-screen verified)
Scale/rotate/opacity can't bake at layout the way translate/color do —
they need pixels. Slice 3 adds **per-element compositing layers** to the
immediate-mode painter:

- **Kernel: `ABI_SYS_GUI_TEXT_TTF_RASTER` (182)** — rasterize a text run
  as 0..255 coverage bytes into a user buffer (`kfont_raster_cov`, same
  glyph cache as on-window TTF), so offscreen layer text is
  pixel-identical to normal text. `struct abi_ttf_raster`; kernel
  renders into a capped scratch then one `copy_to_user`.
- **CSS**: `css_parse_transform` scans every function in a transform
  list — translate/translateX/Y (px/%), scale/scaleX/scaleY (fractions,
  per-mille in `cstyle.scx/scy`), rotate (deg/turn, raw degree count in
  `cstyle.rot` so `rotate(360deg)` keyframe endpoints don't collapse to
  0). New `opacity` property → `cstyle.opa` 0..255. All non-inherited.
- **Animation/transitions**: `@keyframes` stops carry scale/rotate/
  opacity (`kf_stop.has` bits 8/16/32); scale/rotate ride the existing
  bit-2 `transform` transition channel (CSS transitions animate the
  whole transform as one value), opacity gets its own bit-3 channel.
- **Layers**: `lay_block` registers a `struct clayer {node, i0, i1,
  scx, scy, rot, opa}` when the styled node needs one (skipped in
  measure passes; the item range [i0,i1) is contiguous because a
  node's subtree is emitted in one run). The paint-time bbox is the
  live union of the item rects, so ancestor shifts need no bookkeeping.
- **Paint**: `paint_layer` renders the items into a 1 MiB offscreen
  ARGB buffer (userland replicas of the item painters; text via the
  raster syscall + source-over blend), then affine-blits row by row:
  each destination pixel inverse-maps through S(1/s)·R(−θ) about the
  layer center (integer math: sin table ×10000, doubled coords for
  exact half-pixel centers), nearest-samples the buffer, scales alpha
  by opacity, and pushes rows with `tk_draw_blit_blend`.

### Verified
Host page with a red 150×70 box running `animation: spin 16s linear
infinite` (`rotate(0)` → `rotate(360deg)`) and a blue box whose
`setInterval` toggles `scale(1.6) rotate(25deg)` + `opacity 0.35` under
`transition: transform 2.5s linear, opacity 2.5s linear`. 32 shots 2 s
apart: the red box spins continuously (AABB cycles 152×76 → 148×158 →
86×156 → 158×148 at 45°/shot) **with its TTF text rotating in the
layer** (upside-down "SPIN" at 180°); the blue box shows simultaneous
scale+rotate+fade (pale semi-transparent blue over the white page,
text visible through it) and settles back to a crisp opaque unrotated
box when the transition returns home. 11 interval ticks, no faults,
~30 fps in the GUI monitor.

### v1 limits
transform-origin is the layer-bbox center (≈ 50% 50%); compose order is
fixed (scale·rotate about the center + separately-baked translate —
matches CSS for uniform scale; differs for non-uniform scale combined
with rotate); nearest-neighbor sampling (edges are unsmoothed); hit
testing and getBoundingClientRect stay untransformed; nested transformed
descendants flatten into the outermost layer; layers beyond the 1 MiB /
1024-px-wide budget fall back to untransformed painting; overflow-clip
ancestors are not intersected inside layers; form-field text inside
layers is skipped (box chrome only); mono runs use the 8×8 canvas font.
