# tobyOS GUI Framework

This document describes the tobyOS windowing/graphics architecture: the layered
model, the native widget toolkit (TobyTK), the window ABI, theming, and how the
three application worlds tobyOS runs (native, Win32 PE, Linux ELF) converge onto
one compositor as equals.

> Status: Milestone 1 (toolkit + Settings refactor) is landed. Milestones 2
> (compositor hardening) and 3 (Linux-in-a-window) are designed here and being
> implemented incrementally.

## The three layers

```
   ┌──────────────────────────────────────────────────────────┐
   │  Apps:  native (TobyTK)  │  Win32 PE (user32/gdi32)  │ Linux (fbdev/evdev) │
   ├──────────────────────────────────────────────────────────┤
   │  Toolkit:  libtoby  toby/tk.h  (retained widgets + layout)│   (per-stack shims)
   ├──────────────────────────────────────────────────────────┤
   │  Window protocol:  gui_* syscalls  (the low-level ABI)    │
   ├──────────────────────────────────────────────────────────┤
   │  Compositor:  src/gui.c  (z-order, damage, decorations,   │
   │               input routing, animation, taskbar/launcher) │
   ├──────────────────────────────────────────────────────────┤
   │  Scanout:  src/gfx.c  (limine-fb / virtio-gpu, WC + sfence)│
   └──────────────────────────────────────────────────────────┘
```

Everything that appears on screen is a `struct window` (src/gui.c). Native,
Win32, and Linux windows are all the *same* object in the compositor's z-order
list — they share decorations, focus, move/resize/snap, and input routing. That
is the whole point: a Windows `.exe`, a Linux ELF, and a native app open as three
peer windows on one desktop.

## Layer 1 — the window protocol (gui_* syscalls)

The low-level ABI. Numbers are in `include/tobyos/abi/abi.h`; kernel handlers in
`src/syscall.c`; per-window drawing in `src/gui.c` (`gui_window_*`). This layer is
deliberately thin and stable — toolkits and shims target it.

| op | # | args (after fd) | notes |
|----|---|-----------------|-------|
| GUI_CREATE | 10 | w, h, title | → window fd (a FILE_KIND_WINDOW) |
| GUI_FILL | 11 | x, y, (w\|h<<16), color | client coords |
| GUI_TEXT | 12 | (x\|y<<16), str, fg, bg | 8×8 bitmap |
| GUI_FLIP | 13 | — | mark dirty / present |
| GUI_POLL_EVENT | 14 | &gui_event | → 1 if event, 0 if empty |
| GUI_TEXT_SCALED | 56 | (x\|y<<16), str, fg, (bg\|scale<<24\|smooth<<31) | scaled+smoothed bitmap |
| **GUI_TEXT_TTF** | **94** | (x\|y<<16), str, fg, (px\|face<<16) | **kernel TrueType (kfont.c)** |
| **GUI_TEXT_TTF_WIDTH** | **95** | str, (px\|face<<16) | TTF advance width, for layout |
| GUI_SET_STATE | 75 | state | NORMAL/MIN/MAX |
| GUI_SET_TITLE | 76 | title | |
| GUI_LINE/RECT/ROUNDED_RECT/CIRCLE | 80–85 | see syscall.c | **packing varies** |
| GUI_BLIT/GRADIENT/GETPIXELS | 86–89 | | |
| GUI_SET_OPACITY | 91 | alpha | 0..255 |

**Arg-packing caveat (was expensive to learn).** The packing is *not* uniform:
`FILL` passes x and y as separate args with `w|h<<16` packed; `ROUNDED_RECT` and
`GRADIENT` pack `x` in the low 32 bits and `y` in the **high 32 bits** of one arg
(not `x|y<<16`); `LINE`/`TEXT`/`TEXT_TTF` pack `x|y<<16` as signed int16. Two
in-tree callers historically disagreed on `ROUNDED_RECT`. The toolkit's wrappers
(`libtoby/src/tk.c`, the `g_*` helpers) are the reference — they were written
against the actual kernel handlers, with the packing documented per wrapper.

### Native TrueType text (new in M1)

`GUI_TEXT_TTF`/`GUI_TEXT_TTF_WIDTH` route the native path to the same kernel
stb_truetype rasterizer (`src/kfont.c`, Lato/OFL, lazy-loaded) that the Win32
GDI text path uses, so native apps get real antialiased glyphs at arbitrary pixel
sizes instead of the scaled 8×8 bitmap. Glyphs are alpha-blended onto the window
backbuffer (the toolkit fills the widget background first). If no font is loaded
the kernel falls back to the smoothed scaled-bitmap path so text never vanishes.

### Event ABI (read this before defining a local copy)

`struct gui_event { int type; int x, y; uint8_t button, key; uint8_t _pad[2]; }`
is mirrored verbatim to userspace by `GUI_POLL_EVENT` — its layout must stay
stable. Event types (`include/tobyos/gui.h`, `GUI_EV_*`):

```
NONE=0  MOUSE_MOVE=1  MOUSE_DOWN=2  MOUSE_UP=3  KEY=4  CLOSE=5  RESIZE=6
```

**Footgun:** a stale local enum once defined `CLOSE=1`, colliding with
`MOUSE_MOVE=1`, so every pointer-move over a window read as a close and the app
exited. Mouse coordinates delivered to a window are in **client** coordinates
(the compositor subtracts the title-bar/border). `toby/tk.h` defines `TK_EV_*`
equal to these and documents the history; do not re-derive the numbers.

## Layer 2 — the native toolkit (TobyTK)

`libtoby/include/toby/tk.h` + `libtoby/src/tk.c`. A retained-mode widget toolkit
— the canonical native app layer. It supersedes two earlier half-finished
attempts: `programs/common/toby_gui.c` (worked but absolute-positioned, 8×8 text,
outside libtoby) and `libtoby/src/ui.c` "TobyUI" (had a layout engine but its
text rendering was a no-op stub and it drew into a disconnected buffer).

**Status (2026-06-30): TobyTK is now universal.** Every native GUI app is on the
toolkit — the `user_gui_*` family (files, browser, settings, term, edit, viewer,
clock, calc, taskmgr, widgets/Notes, about, demo, helloapp, login) plus the
standalone inspectors (devmgr, diskmgr, eventview, mediaplayer) and the
tray/system apps (volctrl, wifipicker, appinstall, updater, lockscreen). Nothing
ships on raw `gui_*` hand-drawing except `bluescreen` (panic context). `ui.c`
"TobyUI" is dead (no includer) and slated for removal; `toby_gui.c` is no longer
used by any in-tree app but is still wrapped by the SDK as `libtoby_gui.a` for
out-of-tree apps, so it stays until the SDK drops it.

Design:
- **No malloc.** Widgets live in a fixed pool inside `struct tk_window` (declare
  it `static`), tree-structured so the layout engine can recurse.
- **Drawing = gui_\* syscalls**; text = `GUI_TEXT_TTF`.
- **Layout:** `vbox`/`hbox` containers with padding, gap, flex-grow, fixed and
  shrink-to-fit (measured) sizing. Root is a vbox filling the window.
- **Widgets:** panel/vbox/hbox, label, button, text field, checkbox, scrollable
  listbox, slider, progress, separator, plus `TK_CANVAS` (custom-draw escape
  hatch: `on_paint`/`on_event` + the `tk_draw_*` primitives incl.
  `tk_draw_text_mono` for column-aligned 8×16 bitmap text — terminals, editors,
  the clock face, the partition bar), `TK_TABLE` (columns + scrollable rows via
  an app cell-accessor — no slot per row) and `TK_TEXTAREA` (scrollable
  read-only multiline). Window-level key hook via `tk_on_key`.
- **Event loop:** `tk_run()` polls events, hit-tests the tree, manages
  focus/capture, fires callbacks, repaints only when dirty, and **self-paces with
  `nanosleep(~15ms)` when idle** — never a busy-yield (a tight yield loop with
  pid 0's whole-iteration BKL hold can livelock the desktop under SMP).
- **Theming:** a userspace `struct tk_theme` palette (default: dark Plasma-like);
  apps can override per widget.
- **Dynamic rebuild:** `tk_checkpoint`/`tk_rewind`/`tk_clear_children` let an app
  swap a container's contents (e.g. a settings page) without leaking pool slots.

Minimal app:
```c
static struct tk_window win;
tk_window_open(&win, 720, 480, "My App");
struct tk_widget *col = tk_pad(tk_vbox(&win, 0, 8), 12);   // NULL parent = root
tk_label(&win, col, "Hello");
tk_button(&win, col, "Click me", on_click);
tk_run(&win);
```

`/bin/gui_settings` (`programs/user_gui_settings/main.c`) is the first app on the
toolkit and the M1 proof — a sidebar of pages + a content panel rebuilt per page,
reading live metrics and persisting settings via `SYS_SETTING_GET/SET`.

## Layer 3 — the compositor (src/gui.c)

One global compositor owns a z-ordered doubly-linked `struct window` list
(`g.z_top` = topmost = keyboard focus). Already implemented: dirty-rectangle
tracking + `gui_invalidate_*` damage hints, double-buffering, window decorations,
title-bar drag (`g.drag_win`), edge resize (`g.resize_win` + `RESIZE_BORDER`),
snap zones (`g.snap_zone`), focus-blur, fade/minimize/restore animations
(`anim_tick`), taskbar, launcher, and notification toasts. It is pumped by
`gui_tick()` from the pid-0 `idle_loop` (via `gui_set_driver_ctx`, because the
gs-relative `current_proc()` can read back stale under real-hardware SMP).

Window geometry (`include/tobyos/gui.h`): outer top-left `(x,y)`; a
`GUI_TITLE_BAR_H` (30px) title bar; `GUI_BORDER` (1px) frame; client area is
`client_w × client_h` at `(x+BORDER, y+TITLE_BAR_H)`. Apps work purely in client
coordinates.

### M2 — hardening (in progress)
- **Self-healing animations.** Animation state must never strand a window
  (e.g. left at opacity 0 mid-fade). `anim_tick` clamps every animation to
  converge — on completion (or if its start time is in the future / duration has
  elapsed) it snaps the property to the final value and clears `active` — and a
  watchdog snaps any window left with `opacity < 255` but no owning fade back to
  visible.
- Move/resize clamping (the title bar can never hide under the taskbar), focus
  follows raise, and dirty-rect unions stay correct under rapid drag.

GT/Intel acceleration is opt-in and out of scope here; the software scanout path
(`gfx.c`, write-combining framebuffer + sfenced flips on real hardware) is the
contract.

### Real-hardware responsiveness — non-blocking serial + opt-in trace
On the EliteDesk bring-up the desktop was very slow and "ate" clicks on real HW
but was fine in QEMU. Root cause was **not** the compositor (the FB is already
write-combining and the present is damage-tracked). It was serial output:
`serial_putc()` busy-waited the 38400-baud UART per byte (free in QEMU, where THRE
is always ready; ~40 ms per log line on real HW), and entering the desktop
auto-enabled NORMAL trace, so the per-tick heartbeat *and the mouse IRQ handler*
(`on_mouse_event`, which logs on every button edge / hit-test) spun the CPU — with
IRQs disabled — on the UART. Fixes:
- **Async serial TX** (`src/serial.c`): `serial_putc()` pushes into a ring and
  returns; COM1's THRE IRQ (IRQ4) drains it, with a pid-0 idle-loop
  `serial_tx_pump()` safety net (so output flows even where legacy IRQ4 doesn't
  route) and a **bounded-spin** sync fallback for early boot / `kpanic`
  (`serial_set_sync_mode()`). `kprintf` never blocks.
- **Trace is opt-in** (`trace on`), no longer auto-enabled in
  `gui_set_desktop_mode()`.
- The dead-NIC DHCP retry storm is capped (`net.c`) so it stops flooding the log.

## Unifying the three stacks

**Native** apps use TobyTK → `gui_*` → `struct window`. Done.

**Win32 PE** apps are already first-class: the user32/gdi32 shims turn
`RegisterClass`/`CreateWindowEx` into `gui_window_create`, run a user-mode
`DispatchMessage` trampoline, and translate `gui_event`→`WM_*` in `GetMessage`.
Their windows are ordinary `struct window`s. (See `win32-pe-compat` notes.)

**Linux** GUI apps draw to `/dev/fb0` (fbdev) and read `/dev/input/event0`
(evdev). Today `fbdev_present()` (`src/syscall.c`) blits a global shadow straight
to the scanout, fullscreen, bypassing the compositor.

### M3 — Linux fbdev/evdev in a compositor window (LANDED)

A Linux GUI app now appears as a normal, movable, composited window —
conceptually like XWayland or an fbdev-in-a-window shim. Proven by the
`-DTHREEWORLDS_BOOT` demo: a native TobyTK app (Settings), an unmodified Win32
`.exe` (win-gui8.exe), and an unmodified Linux ELF (`linux-fbwin`) open as three
peer windows on one desktop (`[3W] VERDICT: PASS`), +smap clean. Mechanism:
- **fbdev → window.** When a Linux app opens/mmaps `/dev/fb0` while the compositor
  is active, create a per-app `struct window` and report **its client size** as
  the fbdev `xres/yres`. `fbdev_present()` copies the app's shadow into that
  window's backbuffer and `gui_window_flip()`s it, instead of blitting to the raw
  scanout. The direct-scanout path stays as a fallback for the pre-compositor /
  boot-harness case (so existing fbsplash/REALTUI proofs don't regress). The
  single global `fbdev_ctx` generalizes to a small per-owner table so multiple
  Linux GUI apps can be windowed at once.
- **evdev → focused window.** A windowed Linux app's input is sourced from *its*
  compositor window's event ring: the window's `GUI_EV_*` (client-relative mouse,
  keys) are translated into `struct input_event` records on the app's evdev fd —
  mirroring how Win32's `GetMessage` drains `gui_window_poll_event` into `WM_*`.
  Only the focused window's events flow, so input routing matches the other
  stacks.

The payoff is the headline demo: a Windows `.exe`, a Linux ELF, and a native
TobyTK app open as three peer windows on one desktop — something neither Windows
nor Linux can do.

**Demo-harness gotcha (kernel.c, THREEWORLDS_BOOT):** the desktop pump path
stalls if the user-proc count drops to zero, so the harness spawns the native
app *before* dismissing the login proc (never zero live procs), and uses a
guard-capped pump (`TW_PUMP`) rather than the tight `pit_ticks`-deadline
`winpe8_pump_ms`. The Linux app for the demo is `programs/linux-fbwin` — a static
(no-musl) hand-rolled Linux fbdev ELF that *loops/animates* (unlike the one-shot
`linux-fb`), so its window stays up for the screenshot.

## Build & test

MSYS2 UCRT64: put `C:\msys64\ucrt64\bin` (clang, ld.lld) and `C:\msys64\usr\bin`
(make) on PATH, then `cd tobyOS && make iso`. The toolkit is `libtoby/src/tk.c`
(in `LIBTOBY_C_SRCS`, so it lands in both `libtoby.a` and the PIC `libtoby.so`).
Headless validation: QEMU `-display none -serial file:…`, with `-qmp` for QMP
`screendump`. The `-DTKDEMO_BOOT` harness (`src/kernel.c`) auto-logs-in and
launches Settings for a screenshot; the future `-DTHREEWORLDS_BOOT` harness adds
the Win32 + Linux windows for the three-stacks demo.

> Note (Windows/MSYS build gotcha): clang/gcc creating temp files *through make*
> can fail with "unable to make temporary file" — MSYS2 mangles `TMP`/`TMPDIR`
> across the `make → sh → tool` layers, so the native tool gets an invalid temp
> dir (`.c` compiles don't need a temp, so they're fine; `.S` integrated-assembler
> and host-tool builds do). A literal `TMP='C:\t'` prefix *inside* the recipe
> works, so the universal fix is to carry it on the compiler vars:
>
> ```sh
> mkdir -p /c/t
> make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
>      "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
>      EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT" iso
> ```
>
> (`/c/t` is just a short writable dir; the default Windows temp path can also
> get into a bad state under heavy parallel building.) Also: QEMU TCG on a loaded
> host runs ~3× slow, and *much* slower (≫10×) when a build is running
> concurrently — quiesce the host before judging boot liveness, and never confuse
> "slow capture" with a freeze.
