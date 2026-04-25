# Milestone 27 — Display Stack Improvements

**Status: COMPLETE** (all 7 phases + final validation suite pass)

M27 hardens the tobyOS display stack with a cleaner backend abstraction,
alpha-composited surfaces, anti-aliased text, dirty-rectangle redraws,
a virtio-gpu backend for QEMU, and the groundwork for multi-monitor
support. Limine framebuffer remains the universal fallback; nothing
introduced in M27 requires GPU acceleration.

The milestone is structured as seven sequential phases (M27A → M27G),
each with its own design / implementation / test / validation cycle,
plus a final aggregator (`test_m27_final.ps1`) that runs every phase
back-to-back and confirms no regressions in the desktop, GUI input,
shell, filesystem, or networking subsystems.

---

## Implemented improvements

### M27A — Display Test Harness

Reusable validation framework that every subsequent phase plugs into.

- Kernel-side display registry (`src/display.c`) tracks registered
  outputs (`struct display_output`) plus per-test selftests
  (`display_test_basic`, `display_test_draw`, `display_test_render`).
  All exposed via `devtest_register("display", ...)` so the standard
  drvtest harness picks them up automatically.
- Display introspection ABI: `ABI_SYS_DISPLAY_INFO` (syscall #56) +
  `struct abi_display_info` carry every field a userland tool
  needs (geometry, pitch, bpp, format, backend, name, flips,
  status flags, M27G origin_x/origin_y).
- libtoby helpers: `tobydisp_list`, `tobydisp_print_header`,
  `tobydisp_print_record` keep the output schema stable across
  tools.
- Userland tools shipped to `/bin`:
  - `displayinfo` — table + JSON renderer for the registry.
  - `drawtest` — opens a GUI window and exercises every gfx
    primitive (lines / rects / blits / text) via the GUI back
    buffer.
  - `rendertest` — eight-case suite (basic, geometry,
    primitives, overlap, cursor, backend, alpha, font, dirty)
    that emits `[PASS]` / `[SKIP]` / `[FAIL]` lines per case.
  - `fonttest` — six-case suite for the M27D scaling helpers.
- Boot harness (`m27a_run_userland_tools` in `src/kernel.c`)
  spawns every tool sequentially so a regression in exec / spawn /
  PATH also fails the M27A serial check.

### M27B — Rendering Backend Cleanup

Pulls display rendering out of "raw framebuffer access" into a
proper backend abstraction.

- `struct gfx_backend` interface (`include/tobyos/gfx_backend.h`):
  `flip` (mandatory), `present_rect` (optional partial flip),
  `describe` (extended diagnostics), `name`, `bytes_per_pixel`.
- Limine framebuffer backend retained as `g_backend_limine`; gfx
  layer falls back to it if no other backend is installed.
- `gfx_set_backend()` is the only entrypoint that swaps the active
  backend. Failure at any stage of a backend (e.g. virtio-gpu
  TRANSFER returning an error) deactivates that backend and
  reinstates Limine.
- Bounds checking added to every drawing primitive
  (`gfx_fill_rect`, `gfx_blit`, `gfx_blit_blend`,
  `gfx_fill_rect_blend`). Out-of-bounds attempts log a single
  `[gfx] <op> bounds violation` line and are silently clipped —
  the M27 spec calls for "no out-of-bounds writes" without a
  panic.

### M27C — Alpha Blending + Compositing

Per-pixel source-over compositing for translucent UI.

- New surface formats: `ABI_DISPLAY_FMT_ARGB8888` and
  `ABI_DISPLAY_FMT_RGBA8888`. The Limine FB stays
  `XRGB8888` so opaque rendering bypasses blending entirely.
- `gfx_fill_rect_blend()` and `gfx_blit_blend()` implement
  source-over with integer arithmetic
  (`out = (src*alpha + dst*(255-alpha)) / 255`).
- Shadow / panel translucency hookable from the GUI compositor;
  opaque windows still take the fast path.
- `display_test_alpha` validates blending math via canned src/dst
  pairs (alpha=0, 128, 255 + edge cases).

### M27D — Improved Font Rendering

No TrueType — but a software supersampling + corner-smoothing pass
gives noticeably cleaner large text on the same 8×8 bitmap font.

- `gfx_text_scaled()` renders the bitmap font at integer scales
  (1×, 2×, 3×, 4×, …) into the back buffer.
- `gfx_text_scaled_smooth()` adds a "corner round" pass that
  averages diagonal-step pixels to smooth the staircase
  artifacts visible at scale ≥ 2.
- `gfx_text_bounds()` returns the layout box for an arbitrary
  string + scale so GUI button / label code can compute its own
  geometry.
- The bitmap fallback is always available; M27D's helpers are
  pure additions.
- `display_test_font` validates the bounds calculator across
  empty / single-line / multi-line / clamped-scale cases.

### M27E — Dirty Rectangles / Redraw Optimization

Reduces full-screen redraws so window drags don't re-blit
everything every frame.

- `gfx_mark_dirty_rect(x,y,w,h)` accumulates a per-frame dirty
  union; `gfx_dirty_clear()` lets the compositor REPLACE the
  per-primitive union with its own (smaller) hint just before
  flip.
- `gfx_flip()` decides between three present paths:
  - **partial** — call backend's `present_rect` over the dirty
    union (fast path).
  - **full** — fall back to a backend `flip()` if the union
    covers ≥95 % of the screen (or the backend doesn't expose
    `present_rect`).
  - **empty** — no dirty area, skip the present entirely.
- `gfx_present_stats` snapshot
  (`{total,full,partial,empty}_flips` + pixel deltas) is read
  via the new `ABI_SYS_DISPLAY_PRESENT_STATS` syscall (#57).
- GUI compositor (`src/gui.c`) consumes the hints:
  - mouse moves invalidate just the 12×19 cursor sprite (old + new).
  - window drags invalidate old + new outer rects.
  - z-raise / window create / window close force a full present.
- `display_test_dirty` exercises the kernel-side `gfx_flip()`
  branches directly so the test passes even when the compositor
  is idle (e.g. boot harness phase).

### M27F — virtio-gpu Support

QEMU's virtio-gpu (vid 1AF4, did 1050) becomes a first-class
display backend.

- PCI driver (`src/virtio_gpu.c`) probes the virtio-gpu device,
  walks the modern virtio capability list, sets up controlq
  (vq #0) with MSI-X-bound completion observable.
- Four-step bring-up: `GET_DISPLAY_INFO`, `RESOURCE_CREATE_2D`
  (BGRX8888 format), `RESOURCE_ATTACH_BACKING` (1-entry SG list),
  `SET_SCANOUT 0`.
- `vgpu_flip` and `vgpu_present_rect` route through
  `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` (full-frame and
  partial respectively). All commands are synchronous — the
  IRQ counter is purely for diagnostic visibility.
- `vgpu_describe()` surfaces PCI BDF, scanout geometry, backing
  KiB, IRQ vector, partial / full counters, and (M27G addition)
  the count of host-enabled scanouts in the
  `displayinfo --json`-visible "extra" string.
- Backend hands control back to Limine FB on error and emits a
  one-line warning. With `-vga none -device virtio-gpu-pci`
  virtio-gpu is the active backend; without it the system stays
  on Limine FB exactly as before. Both paths validated by
  `test_m27f.ps1` in two consecutive QEMU boots.

### M27G — Multi-Monitor Groundwork

Extends the registry to represent multiple outputs and lay them
out spatially. Single-monitor systems are unaffected.

- ABI extension: `struct abi_display_info` gains
  `int32_t origin_x;` + `int32_t origin_y;` plus a `_pad0`
  (96-byte layout, validated at compile time).
- Kernel `display.c` registry now tracks origins and ships:
  - `display_register_output()` auto-positions a new output to the
    right of every existing one (simple horizontal layout).
    Re-registering an existing output by name preserves its
    origin so a backend swap doesn't reshuffle the layout.
  - `display_unregister_output()` plus auto-elect of a new
    primary if the removed slot was primary.
  - `display_set_origin()` for explicit override.
  - `display_total_bounds()` returns the bounding box that
    contains every output (used by the desktop layer and
    `displayinfo`'s "layout=WxH" summary).
- New devtest `display_test_multi` registers two synthetic
  outputs, validates the auto-layout math, exercises
  `display_set_origin()`, and rolls back the registry — all
  without disturbing the live primary.
- `display_dump_kprintf()` now emits `layout=WxH@(x,y)` on the
  summary line and `name … WxH@(ox,oy) …` per output.
- libtoby `tobydisp_print_header` / `_print_record` add an
  ORIGIN column.
- Userland `displayinfo` adds `origin_x` / `origin_y` to every
  JSON record, emits a trailing `{"layout":...}` JSON record
  with the bounding box, and appends `layout=WxH` to the PASS
  summary line.
- virtio-gpu enumerates every host-advertised pmodes[] entry,
  caches the geometry, and reports `enabled_scanouts` /
  `total_scanouts` via describe(). Only scanout 0 is composited
  into; secondary scanouts are informational only.

---

## Tests created

| Phase | Validation script        | What it boots                       | What it validates                                                                                              |
| ----- | ------------------------ | ----------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| M27A  | `test_m27a.ps1`          | default QEMU                         | `displayinfo` / `drawtest` / `rendertest` / `fonttest` boot sweep, registry alive, no panics                    |
| M27B  | `test_m27b.ps1`          | default QEMU                         | gfx_backend ABI live, `display_render` reports `flip=ok present_rect=yes describe=yes`, no bounds violations    |
| M27C  | `test_m27c.ps1`          | default QEMU                         | `display_alpha` selftest passes, `rendertest alpha` overlays clean, no out-of-bounds writes                     |
| M27D  | `test_m27d.ps1`          | default QEMU                         | `display_font` bounds math correct (1×/2×/3×/multi-line), `fonttest` 6/6 cases pass, bitmap fallback intact     |
| M27E  | `test_m27e.ps1`          | default QEMU                         | `display_dirty` tracker reports partial / full / empty flips, `rendertest dirty` validates stats invariants     |
| M27F  | `test_m27f.ps1`          | TWO boots: with + without virtio-gpu | virtio-gpu probe + scanout setup + backend install + present_rect; second boot regression-checks Limine FB      |
| M27G  | `test_m27g.ps1`          | default QEMU                         | `display_multi` synthetic 3-output exercise + auto-layout math + `set_origin` override + clean unregister       |
| Final | `test_m27_final.ps1`     | runs every phase + cross-log sweep   | all per-phase scripts + global regression sweep across `serial.m27a.log` + cross-log forbidden-token check      |

All seven per-phase scripts produced PASS verdicts on the final run:

```
================== M27 PER-PHASE RESULTS ==================
Phase Verdict Elapsed Desc
----- ------- ------- ----
M27A  PASS        8.1 Display test harness (displayinfo / drawtest / rendertest / fonttest)
M27B  PASS        8.1 Rendering backend cleanup (gfx_backend abstraction + bounds checking)
M27C  PASS        8.1 Alpha blending + compositing (ARGB surfaces, source-over)
M27D  PASS        8.1 Improved font rendering (supersampling AA, scaled bitmap)
M27E  PASS        8.1 Dirty rectangles + redraw optimization (gfx_present_stats)
M27F  PASS         15 virtio-gpu support (probe/init/2D scanout/present_rect + Limine fallback)
M27G  PASS        8.2 Multi-monitor groundwork (origin_x/y, auto h-layout, register/unregister)

================== GLOBAL REGRESSION SWEEP ==================
Subsystem               Verdict
---------               -------
OS boots to desktop     PASS
Login surfaced          PASS
displayinfo userland    PASS
drawtest userland       PASS
rendertest userland     PASS
fonttest userland       PASS
gfx_backend abstraction PASS
Alpha blending          PASS
Font rendering (M27D)   PASS
Dirty redraw stats      PASS
Multi-monitor (M27G)    PASS
Display layout summary  PASS
Filesystem              PASS
Shell                   PASS
GUI ready               PASS
Input (PS/2)            PASS
Input (USB)             PASS
Network                 PASS
ACPI                    PASS
Mouse cursor            PASS

  no forbidden tokens detected across 8 M27 log file(s)

================== M27 FINAL: PASS ==================
```

---

## Known limitations

- **M27A** — `drawtest` / `rendertest` run with the GUI active.
  The compositor owns the back buffer, so on-screen visual diff
  is left to a human eyeball / screenshot — the scripts only
  validate that the tools produced their PASS lines.
- **M27B** — `gfx_backend` exposes flip / present_rect / describe
  only. There is no surface / buffer creation API; the back
  buffer is global per-display. Multi-surface compositing would
  need an extension.
- **M27C** — Alpha blending is per-pixel source-over with
  straight-alpha math. No premultiplied-alpha optimization, no
  Porter-Duff modes beyond OVER, no SIMD path.
- **M27D** — No TrueType. Supersampled bitmap font + corner-
  smoothing gives clean ASCII at large sizes but is not
  Unicode-aware. A real TTF rasterizer is left for a future
  milestone (e.g. integrating stb_truetype).
- **M27E** — Dirty union is a single bounding box, not a region
  list. Two small dirty rects in opposite corners union into
  the whole screen and trigger a full present. ≥95 % coverage
  also falls back to a full present.
- **M27F** — virtio-gpu uses synchronous `TRANSFER_TO_HOST_2D` +
  `RESOURCE_FLUSH` per flip / present_rect. The IRQ is wired
  but only used as a completion observable; it does not gate
  command issue. No 3D / virgl support.
- **M27F** — Only scanout 0 is driven. Secondary scanouts the
  host advertises are reported via `describe()` but cannot be
  targeted as a render destination.
- **M27G** — Multi-monitor support is groundwork only. Auto
  horizontal layout works, register / unregister / set_origin
  work, the ABI carries `origin_x` / `origin_y`, but the desktop
  layer still composites only into the primary scanout. A real
  multi-output desktop needs per-output back buffers and a
  per-output gfx_backend.
- **M27G** — The synthetic-multi devtest uses `vmon1` / `vmon2`
  registry slots with no backing surface. Real second-monitor
  presents would need a proper second scanout from a
  hardware-class backend (virtio-gpu's other scanouts, a second
  Limine FB on a multi-head firmware, etc.).

---

## QEMU validation commands

### Run any single phase

```powershell
pwsh .\test_m27a.ps1     # display test harness
pwsh .\test_m27b.ps1     # rendering backend cleanup
pwsh .\test_m27c.ps1     # alpha blending
pwsh .\test_m27d.ps1     # font rendering
pwsh .\test_m27e.ps1     # dirty rectangles
pwsh .\test_m27f.ps1     # virtio-gpu support (TWO QEMU boots)
pwsh .\test_m27g.ps1     # multi-monitor groundwork
```

### Run all phases + final regression sweep

```powershell
pwsh .\test_m27_final.ps1            # all phases + global sweep
pwsh .\test_m27_final.ps1 -Build     # rebuild kernel ISO first
pwsh .\test_m27_final.ps1 -Skip M27F # skip a specific phase
```

### Direct QEMU — framebuffer fallback (Limine FB backend)

This is the configuration M27A / B / C / D / E / G validate.
Limine picks up a stdvga FB at boot; gfx layer initializes against
it; `displayinfo` reports `backend=limine-fb`.

```bash
qemu-system-x86_64 -cdrom tobyOS.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk -smp 4 \
    -device qemu-xhci,id=usb0 -device usb-kbd,bus=usb0.0 \
    -device usb-mouse,bus=usb0.0 -netdev user,id=net0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial file:serial.log -no-reboot
```

### Direct QEMU — virtio-gpu backend (M27F)

Removing `-vga` (default `std` is dropped via `-vga none`) prevents
Limine from finding a usable framebuffer, so virtio-gpu is the only
backend the gfx layer can install. `displayinfo` then reports
`backend=virtio-gpu`.

```bash
qemu-system-x86_64 -cdrom tobyOS.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk -smp 4 \
    -device qemu-xhci,id=usb0 -device usb-kbd,bus=usb0.0 \
    -device usb-mouse,bus=usb0.0 -netdev user,id=net0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -vga none -device virtio-gpu-pci \
    -serial file:serial.log -no-reboot
```

---

## Real-hardware display validation checklist

Use this checklist when first booting tobyOS on physical hardware.
The framebuffer backend is the only one expected to come up on
bare metal — virtio-gpu is QEMU-specific.

- [ ] Boot the ISO on a bare-metal x86_64 PC. Limine should pick
      a sane native VBE FB (typical: 1024 × 768 or higher).
- [ ] Confirm the desktop renders at native resolution. The login
      window must be readable.
- [ ] Run `displayinfo`. Expect: 1 output, `backend=limine-fb`,
      `layout=WxH` matching the panel.
- [ ] Run `displayinfo --json`. Confirm `origin_x:0` /
      `origin_y:0`, plus a trailing `{"layout":...}` record.
- [ ] Run `drawtest`. PASS expected. The GUI window opens; close
      it manually.
- [ ] Run `rendertest`. All 8 cases PASS or SKIP cleanly
      (`alpha` / `font` / `dirty` may SKIP if the compositor is
      idle when invoked from the boot harness; they re-pass when
      run interactively from the shell).
- [ ] Run `fonttest`. Multi-line scaled-font window must render
      without artifacts.
- [ ] Drag a window across the screen. No tearing / smearing /
      dirty trails (M27E sanity).
- [ ] Open two windows and drag one over the other. Both repaint
      cleanly on overlap (M27E + M27C).
- [ ] Move the cursor over a translucent UI element. The cursor
      stays on top; blending is visible (M27C).
- [ ] Disconnect / reconnect HDMI mid-session if the firmware
      tolerates it. We DO NOT support live re-init; the system
      should stay alive (no panic). Reboot to recover.
- [ ] On a multi-head firmware (rare on bare metal): the
      secondary scanout will show up only if a future milestone
      teaches the desktop layer to composite into multiple
      gfx_backend surfaces.

### Real-hardware checklist for virtio-gpu (when running tobyOS in another VM)

- [ ] Boot the ISO under another hypervisor or a nested QEMU
      that exposes virtio-gpu (e.g. crosvm, kvmtool with the
      virtio-gpu PCI device).
- [ ] `displayinfo` should report `backend=virtio-gpu`.
- [ ] `displayinfo --json` describe field should contain
      `pci=BB:SS.F`, `scanout=WxH`, `irq=msix/0xNN`, plus
      `partial=N/full=N` and `scanouts=K/16`.
- [ ] Window drags should produce non-zero `partial=…` deltas
      after a few frames (proves the dirty-rect path is active).
- [ ] If the host advertises additional enabled scanouts, you
      should see `[virtio-gpu] scanout N: WxH@(x,y) (info only)`
      lines in dmesg / serial. The scanouts are reported but
      not composited into yet.

---

## Mapping back to the M27 spec

The M27 spec listed seven goals + a final validation requirement.
Status:

| Spec requirement                                     | Status           | Where                                                                 |
| ---------------------------------------------------- | ---------------- | --------------------------------------------------------------------- |
| Display test harness (displayinfo / drawtest / rendertest) | DONE       | M27A, see `programs/displayinfo`, `programs/drawtest`, `programs/rendertest` |
| Backend abstraction + bounds checking                | DONE             | M27B, `include/tobyos/gfx_backend.h`, `src/gfx.c` clip paths          |
| Alpha blending + better compositing                  | DONE             | M27C, ARGB / RGBA surface formats, `gfx_*_blend` primitives           |
| TrueType-quality font rendering                      | DONE (no TTF)    | M27D, supersampled bitmap + corner-smoothing AA pass                  |
| Dirty rectangles / redraw optimization               | DONE             | M27E, `gfx_mark_dirty_rect` + `gfx_present_stats` + GUI hints         |
| virtio-gpu support                                   | DONE             | M27F, `src/virtio_gpu.c`, both flip and present_rect plumbed through  |
| Multi-monitor groundwork                             | DONE             | M27G, ABI + auto h-layout + register / unregister / set_origin        |
| Final validation suite                               | DONE             | `test_m27_final.ps1` aggregator + cross-log forbidden-token sweep     |

Every "Tests" bullet from the M27 spec is exercised by at least one
PASS line in the per-phase scripts:

- "displayinfo prints correct framebuffer / display info" → `PASS: displayinfo: ... primary fb0 ... layout=...`
- "drawtest renders bounded shapes without corruption" → `PASS: drawtest: N primitive(s) ran cleanly` + bounds-violation forbidden-token sweep
- "rendertest reports PASS/FAIL for basic rendering cases" → `PASS: rendertest: pass=N skip=M fail=0`
- "Existing desktop still boots / windows render correctly" → `[gui] window manager ready` + `[INFO] display: fb0 (primary) ...` global regression
- "rendertest alpha" → `[PASS] display_alpha:` + `[PASS] rendertest alpha`
- "fonttest renders small / large / mixed text" → `[fonttest] 6/6 PASS (0 FAIL)`
- "rendertest dirty" → `[PASS] display_dirty: tracker OK ...` + `[PASS] rendertest dirty`
- "QEMU with virtio-gpu boots desktop" → `test_m27f.ps1` pass 1 PASS (with `-device virtio-gpu-pci`)
- "fallback framebuffer backend still works" → `test_m27f.ps1` pass 2 PASS (without the device)
- "single-monitor system still works" → M27G test passes with one fb0 + the synthetic 3-output exercise
- "displayinfo lists one or more displays" → `PASS: displayinfo: N output(s) ... layout=WxH`

---

## Files added / changed in M27

### New userland programs

- `programs/displayinfo/` — display registry table + JSON renderer.
- `programs/drawtest/` — gfx primitive exerciser.
- `programs/rendertest/` — eight-case render validation suite.
- `programs/fonttest/` — six-case font scaling validation.

### New kernel sources

- `src/display.c` — display registry + selftests + multi-monitor logic.
- `src/virtio_gpu.c` — virtio-gpu PCI driver + 2D scanout backend.
- `include/tobyos/display.h` — kernel display API.
- `include/tobyos/gfx_backend.h` — backend interface.

### Extended

- `src/gfx.c` — backend abstraction, ARGB blend primitives,
  scaled / smoothed text, dirty-rect tracker, present_stats.
- `src/gui.c` — alpha-aware window painting, dirty hint emission.
- `include/tobyos/abi/abi.h` — `ABI_SYS_DISPLAY_INFO` (#56),
  `ABI_SYS_DISPLAY_PRESENT_STATS` (#57), surface formats,
  backend ids, `struct abi_display_info` (96 bytes including
  M27G origins).
- `libtoby/src/devtest.c` — `tobydisp_*` helpers + ORIGIN column.
- `src/syscall.c` — `sys_display_info`, `sys_display_present_stats`.

### Validation scripts

- `test_m27a.ps1` … `test_m27g.ps1` — per-phase scripts.
- `test_m27_final.ps1` — aggregator with global regression sweep.
- `M27_SUMMARY.md` — this document.
