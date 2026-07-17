# Task: bring up Chromium on tobyOS — both the Linux build (Track B) and the Windows build (Track C)

You are working on **tobyOS**, a from-scratch x86-64 OS that has a rare
superpower: it runs **unmodified Linux x86-64 binaries** (Track B ABI
layer) **and stock Windows x64 `.exe`s** (Track C Win32/PE layer). That is
the entire reason this task is tractable. You are **not** writing a
browser engine — that was evaluated and rejected as a multi-year
company-scale project (read `tobyOS/docs/browser-chromium-v8-roadmap.md`
for the full analysis and why this bring-up path was chosen instead). You
are doing a **porting / bring-up** job: getting a real, pre-built
Chromium binary to run on tobyOS's existing ABI layers by closing the gap
between what Chromium demands and what the ABI layers currently provide.

**Goal:** Chromium rendering real web pages (including real JavaScript
apps) inside tobyOS, from **both** its Linux build and its Windows build,
because tobyOS runs both kinds of application. Ship **Chromium** (the
open BSD-licensed upstream, e.g. an official `chrome-linux`/`chrome-win`
build or ungoogled-chromium) — Chrome and Edge proper are proprietary and
not redistributable, though any Chrome/Chromium/Edge build is fine to
*test* with during bring-up.

## Read first (context + prior art)

- `tobyOS/docs/browser-chromium-v8-roadmap.md` — the strategic picture;
  section 0 (the fork in the road) and sections 1/13 are the ones that
  matter for this task.
- Memory files (in `C:\Users\tdude\.claude\projects\c--CustomOS\memory\`):
  `linux-abi-compat-b1.md` (Track B — what Linux syscalls/features
  already work: glibc dynamic, CPython, bash, epoll, /proc+/sys, ptys,
  virtio-gpu headless proof), `win32-pe-compat-c1.md` (Track C — what
  Win32 already works: GUI/GDI/controls/dialogs/TrueType/registry/winsock;
  next-ups were TLS/TTF/shell32), `cross-personality-pipeline-x1.md`,
  `tobyos-build-env.md` (how to build/run on this Windows host),
  `igpu-i915lite.md` (the real-HW GPU path — a *late* tier here),
  `storage-provisioning.md` + `initrd-tar-explicit-list.md` (how to get
  files onto the tobyOS filesystem), and the `browser-*.md` set.
- Update `MEMORY.md` + a new `chromium-bringup.md` memory file as you go,
  per this project's memory discipline. Track the **gap-list burn-down**
  as your primary progress metric.

## The core methodology: INSTRUMENT-FIRST GAP-CLOSING

This project's hardest-won lesson is *measure before you build* — this
session alone, three handed-down root-cause hypotheses turned out wrong,
each caught by instrumenting first. Apply it literally here. **The gap
list is the measurement, and it drives everything.** The loop, per
milestone:

1. **Obtain the target Chromium binary.** Prefer a **fully-bundled /
   mostly-static** build so it carries its own shared libraries (avoids
   the shared-lib/sysroot rabbit hole for as long as possible). For
   Phase 1 that's a Linux x86-64 **headless** Chromium
   (`chrome-headless-shell` or a full `chrome-linux` run with
   `--headless`).
2. **Enable the "unimplemented syscall / import" trace.** Track B almost
   certainly already logs unhandled syscalls; if not, add a gated log in
   the syscall dispatch that prints the number + args of anything it
   doesn't implement (and returns `-ENOSYS`). For Track C, the equivalent
   is logging unresolved/unimplemented PE imports at load + stub-hit at
   call. **This log IS the gap-list generator.**
3. **Run Chromium, collect the gap list** from the serial log
   (`grep -a`, binary-safe — serial is noisy).
4. **Triage and close gaps in priority order** — the ones blocking
   startup first. Many syscalls are a few lines; a handful are real walls
   (see below). Implement, rebuild the kernel/ABI layer, re-run.
5. **Watch the gap list shrink; iterate** until Chromium renders.
6. **Verify.** Headless writes a PNG — compare it to the SAME Chromium
   build's output on the host. Because it's the identical engine, they
   should be **near pixel-identical** (unlike the from-scratch
   Edge-oracle harness, where cross-engine diffs were expected). Reuse the
   `tools/compare/` harness pattern; a large diff means an ABI bug, not a
   rendering gap.

Do **not** implement anything before the first gap list exists. The
ranked list of missing syscalls/APIs from the first run is what scopes
the whole effort.

## Strategic sequencing (do them in this order)

Linux first because Track B is the more mature layer and Chromium is a
Linux-native citizen with a documented, tractable headless path. Windows
Chromium is a heavier consumer (DirectWrite/D3D/Media Foundation/Windows
sandbox) on the earlier-stage Track C, so it follows.

- **M0 — First gap list (Linux headless).** Get a static Linux headless
  Chromium onto a tobyOS **data/disk volume** (it's ~150–200 MB — too big
  for initrd; use a mounted image, see `storage-provisioning.md`), enable
  the unimplemented-syscall trace, run it `--no-sandbox --headless
  --screenshot`, and produce the first ranked gap list. **Deliverable: the
  gap list.** Nothing else.
- **M1 — Linux headless renders.** Close the gap list until headless
  Chromium writes a valid PNG of a simple static page on tobyOS that
  matches host Chromium.
- **M2 — Linux headless runs a real JS app.** Point it at a React/Angular
  SPA; confirm it hydrates and renders. This proves V8's JIT (W^X exec
  pages), threads, and the platform surface all work under the ABI —
  i.e. the thing the from-scratch engine *couldn't* do.
- **M3 — Linux windowed.** Give Chromium a display. Two routes: bring up a
  minimal X11 server / Wayland compositor on tobyOS, **or** write a
  **TobyTK Ozone backend** that blits Chromium's shared-memory framebuffer
  into a native window (likely less work and better integrated). On-screen
  QEMU screenshot verifies.
- **M4 — Windows headless (Track C).** Same loop with a Windows x64
  headless Chromium build; the gap list will be unresolved Win32 imports
  (graphics/DirectWrite/process/sandbox APIs).
- **M5 — Windows windowed (Track C).** Chromium painting into a window via
  its Windows backend (GDI/D3D → likely WARP/SwiftShader first).

Once the Linux loop is proven (through ~M2), the Windows track (M4–M5)
can run in parallel as a second agent if you want — but establish the
methodology on Linux first.

## The walls (know these going in; don't rediscover them)

- **Sandbox → start `--no-sandbox`.** Chromium's renderer sandbox needs
  seccomp-bpf, user namespaces, and specific `clone`/`prctl` flags
  (Windows: job objects / AppContainer / restricted tokens). Disable it
  for bring-up. Implementing the real sandbox later is a large, *optional*
  sub-project. **Caveat to document:** `--no-sandbox` runs untrusted web
  JS with full process privilege — acceptable for bring-up, a real risk
  for hostile browsing.
- **Graphics → headless + SwiftShader first.** Headless mode needs no
  display server, and Chromium bundles **SwiftShader** (software GL), so
  no GPU driver is required for M0–M2 / M4. Windowed (M3/M5) needs the
  Ozone/Windows display backend. GPU-accelerated GL via the i915-lite
  path (`igpu-i915lite.md`) is a much later tier — do not start there.
- **Threads / futex / W^X are load-bearing.** Chromium is heavily
  multi-threaded and V8's JIT writes then executes code pages. Verify
  Track B's `futex`, `clone` (thread flags), `mmap`/`mprotect` with
  `PROT_EXEC`, and TLS are solid under contention early — a flaky futex
  or a missing W^X flip will look like random Chromium crashes.
- **Dependencies.** Use a bundled/static build; if you must go dynamic,
  populate a Linux sysroot (glibc, NSS, fontconfig, libX11…) on the FS.
  New FS files need the Makefile tar list (`initrd-tar-explicit-list.md`)
  or a data volume.
- **Widevine / DRM is a non-goal.** Closed-source; premium streaming
  (Netflix) won't play. Clear Key only. Document, don't chase.
- **Windows specifics (M4–M5).** Windows Chromium wants DirectWrite
  (text), Direct3D/DirectComposition (or falls back to WARP/SwiftShader),
  Media Foundation (codecs), and a big ntdll/kernel32/user32/gdi32
  surface. Track C has GUI/GDI/controls/TrueType/registry/winsock; expect
  the gaps to be graphics (DirectWrite/D3D), the process/sandbox APIs, and
  possibly TLS (schannel) — same instrument-first loop.

## Environment & build/test

- Build tobyOS on this Windows host via MSYS2/UCRT64 clang
  (`tobyos-build-env.md`). Boot in QEMU with QMP screenshots — reuse
  `tools/gui_shot.py` and the `tools/compare/` harness patterns already in
  the repo.
- Chromium binary onto the FS: a **raw disk image mounted as a data
  volume** is the practical route for a 150–200 MB payload
  (`storage-provisioning.md`), not initrd.
- Serial logs are binary-ish: always `grep -a`. The intermittent
  ~3 s TKAPP boot stall is real — re-run 3× before believing a boot
  failure.

## Working conventions (match the project exactly)

- One coherent slice per git branch off `main`; write/extend a
  `docs/chromium-bringup-*.md`; commit with trailer
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`; merge
  `--no-ff` into `main`; push.
- **Verify before committing.** Headless: the rendered PNG (diff vs host
  Chromium). Windowed: an on-screen QEMU screenshot.
- Keep the ABI-layer trace flags gated (like the browser's
  `-DVAR_PROF`/`-DDL_TRACE` diagnostics) and leave them in-tree — the
  syscall/import trace is the permanent instrument for this whole effort.
- Append a per-milestone summary to `chromium-bringup.md` + a `MEMORY.md`
  index line as each milestone lands, tracking the gap-list burn-down.

## Scope honesty

This is real bring-up work — expect weeks per milestone of syscall/API
whack-a-mole, not download-and-run. But unlike the from-scratch engine, it
is **bounded and it stays done**: once a syscall is implemented it's
implemented, and the payoff is the *actual* Chromium — real apps, real V8,
real everything — running on tobyOS in both its Linux and Windows
personalities. Start with M0: **produce the first gap list and stop.**
That single artifact tells us how big this really is.
