# Handoff: full YouTube UI parity on tobyOS (comments, sidebar tiles, thumbnails)

You are picking up a **working, interactive** Chromium bring-up. Do NOT restart
from discovery — read §1 (what is already proven) and §2 (the one control
experiment that redefines this arc) before touching anything.

Prior arcs, all landed: video PLAYS on a YouTube watch page (HAVE_ENOUGH_DATA,
20–24 s buffered, decoded frames on screen, zero crashes over 360 s); the ~55 s
kernel wake freezes, a one-byte AF_UNIX shear, un-acked TLB shootdowns,
VMA-table exhaustion, and TCP conn-pool exhaustion are all fixed. Full per-slice
evidence is in `docs/chromium-hypothesis-ledger.md` (slices 56–60). The prior
handoff `docs/chromium-youtube-latency-handoff.md` is the latency arc and is
DONE.

---

## 0. Prime directives (learned expensively in slices 59–60 — do NOT relearn)

1. **REPRODUCE ON A KNOWN-GOOD SYSTEM BEFORE BLAMING YOURS.** Slice 60's control
   experiment took 60 seconds and killed SIX theories built over ~8 six-minute
   guest runs. Host Chrome is on this machine:
   `/c/Program Files/Google/Chrome/Application/chrome.exe` and Edge. Run the
   reference first, every time.
2. **Ask the app to STATE its condition; do not infer it from counters.** The
   turning point was `document.querySelector(...).textContent` /
   `offsetHeight`, not more counting. `innerText` is layout-aware (empty for
   unrendered subtrees); `textContent` is not; `offsetHeight` says outright
   whether a box has geometry.
3. **Sanity-check every new probe field against a case where you KNOW the
   answer.** Two of the six dead theories died because MY OWN PROBE was wrong
   (a BOGUS-EXPIRE detector that flagged URLs with no `expire=`; a `gate`
   detector that matched `tp-yt-paper-dialog`, which YouTube ships hidden on
   every page).
4. **Batch questions per run.** Each guest run is ~6 min (build + WHPX boot +
   360 s). Put every probe you can think of into ONE `Runtime.evaluate` (mind
   `cdp_send`'s buffer — slice 59h raised it 2048→16384 after a long command
   truncated into `-32700 invalid token`). Use `logs/run_x3.sh` (build once,
   run 3×, auto-tabulate) because page-build completeness itself varies
   run-to-run (`ytd` element count ranges 137 … 3700+).

---

## 1. What is already working (do not re-verify, do not break)

On a real YouTube watch page, in the native TobyTK window (`programs/chromewin`):
- The page renders: title, channel, subscriber count, **view count (23M
  views)**, like/share/save, Sign in — all populated from the live page.
- **Video PLAYS**: `vid=r4 n2 b20.0` (HAVE_ENOUGH_DATA, 20 s buffered),
  `currentTime` advancing, decoded frames painted (screenshot proof:
  `logs/x3_run3_c.png`).
- The YouTube SPA builds itself fully when a run completes: `ytd`≈3700 custom
  elements; `/youtubei/v1/next` returns 200; thumbnail fetches ARE issued.
- Scroll works via real `Input.dispatchMouseEvent` mouseWheel (NOT injected JS —
  YouTube owns the scroll container; slice 59b).

---

## 2. THE CONTROL EXPERIMENT (slice 60) — this redefines the whole arc

Host Chrome, Windows, real GPU + network, SAME watch page, both headless modes:

| element                     | host `--headless=old` | host `--headless=new` | tobyOS |
|-----------------------------|-----------------------|-----------------------|--------|
| ytd-comment-thread-renderer | **0**                 | **0**                 | 0      |
| ytd-compact-video-renderer  | **0**                 | **0**                 | 2      |
| ytd-comments (component)    | present               | 0                     | present (73 desc) |
| `<img>` tags                | 89                    | 7                     | 68     |

**Headless Chrome does not render YouTube comments or sidebar tiles on a normal
machine either. tobyOS is AT PARITY with headless chrome — there is NO tobyOS
defect in this gap.** YouTube gates comments and the related-videos sidebar
behind `IntersectionObserver` + real compositing/frame-presentation signals
that a headless renderer never generates.

Reproduce the control yourself (60 s, do this first):
```bash
"/c/Program Files/Google/Chrome/Application/chrome.exe" --headless=old \
  --disable-gpu --no-sandbox --mute-audio --window-size=800,600 \
  --virtual-time-budget=30000 --dump-dom \
  "https://www.youtube.com/watch?v=aqz-KE-bpKQ" > ctl.html
for t in ytd-comment-thread-renderer ytd-compact-video-renderer ytd-comments; do
  printf "%-40s %s\n" "$t" "$(grep -o "$t" ctl.html | wc -l)"; done
```
Caveat (honest): the host runs used `--virtual-time-budget`, which can starve
network I/O, so host counts are a LOWER bound. A cleaner control (no virtual
time, longer wall clock) is the very first task in §4.

---

## 3. What full UI parity actually requires

The bundled binary is **`chrome-headless-shell`** (196 MB,
`programs/chromium/chrome-headless-shell-linux64/`). It is the stripped
headless-ONLY variant — there is no flag, cookie, or CDP call that makes it
composite to a display. Getting comments/sidebar means the renderer must
believe its frames are being PRESENTED. Two routes, cheap-first:

**Route A — de-risk WITHOUT headed chrome (try first; may be enough, may prove
it isn't).** Before committing to the big arc, spend one or two runs finding out
whether the deferral can be defeated in the headless renderer:
- Force visibility/focus via CDP: `Emulation.setFocusEmulationEnabled{enabled:
  true}`, and check `document.visibilityState` / `document.hasFocus()` in the
  probe. A headless page often reports `hidden`, which alone suppresses
  IntersectionObserver callbacks.
- The screencast we run (`Page.startScreencast`) may already mark the page
  visible; verify with the probe rather than assuming.
- Directly poke the observers: in `Runtime.evaluate`, scroll the specific
  containers into view and call `window.dispatchEvent(new Event('scroll'))` +
  force a layout read; see if `ytd-comments`/secondary-results gain
  `offsetHeight`. If they populate, parity is a chromewin-side nudge, not a
  bring-up arc. If they do NOT even on the HOST headless chrome, Route A is
  dead and you have PROVEN you need Route B — record that and move on.

**Route B — headed chrome (the real "normal browser", a large arc).** Requires
BOTH:
1. **The full `chrome` binary**, not `chrome-headless-shell`. Same version line
   (151.x) to match the existing sysroot/ABI work. Sourcing + fitting it
   (~300–400 MB, more `NEEDED` DSOs) into the RAM initrd is milestone one.
2. **A real Ozone display backend.** chrome's Ozone platforms are `x11`,
   `wayland`, `drm` (KMS/GBM), `headless`. Pick by least-new-code:
   - **Ozone X11 against an EXPANDED in-kernel fake X server** — RECOMMENDED
     first attempt. tobyOS ALREADY has a minimal fake X server (`src/socket.c`,
     `x_server`/`xserver_handle`, answers `xcb_connect` for SwiftShader's Vulkan
     WSI probe) and chromewin ALREADY routes `Input.*`. The gap: the fake server
     must grow from "handshake only" to real windowing — `CreateWindow`,
     `MapWindow`, `GetGeometry`, expose/damage, and pixel delivery
     (`PutImage`/MIT-SHM or a shared pixmap). Chrome composites into that
     surface; you read pixels back the way chromewin already does. The sysroot
     already carries `libX11/libxcb/libxkbcommon` (`programs/chromium/sysroot/`).
   - **Ozone DRM/GBM against a virtual KMS device** — most faithful (chrome's
     real hardware compositing path), but needs a DRM/KMS driver in tobyOS. See
     the `igpu-i915lite` memory (real gen-7 blit/flip proven) — reusable but
     heavy. Defer unless X11 stalls.
   - **Ozone Wayland against a tiny in-guest compositor** — the sysroot has
     `libwayland-server`; writing a minimal wl compositor is real work with no
     existing asset to reuse. Lowest priority.

---

## 4. Concrete next steps (ordered, each a checkpoint)

1. **Cleaner control (30 min).** Re-run the host reference WITHOUT
   `--virtual-time-budget`, ~60 s wall clock, both headless modes, and — if you
   can — a HEADED host run (`chrome.exe` with NO `--headless`, `--dump-dom`
   after a delay). This establishes the real ceiling: does headed chrome on a
   known-good machine actually show comments/sidebar? If even headed host chrome
   is empty at this URL, the target may be wrong (age-gate, region, this
   specific video) — switch to a mainstream video with known-busy comments
   before building anything.
2. **Route A spike (1–2 runs).** Focus/visibility CDP + observer poke as in §3A.
   Probe `visibilityState`, `hasFocus`, and the `offsetHeight` of
   `ytd-comments` + `ytd-watch-next-secondary-results-renderer`. Decision: if
   they populate → finish the chromewin-side nudge, DONE, no bring-up. If not,
   and the HOST headless proves the same → Route A is closed; commit to Route B.
3. **Route B milestone 1 — obtain + boot the full chrome binary** headless
   first (prove the bigger binary + its extra DSOs load and run on the Linux ABI
   at all, reusing everything the headless-shell arc built). No display yet.
4. **Route B milestone 2 — Ozone X11 up** against the expanded fake X server;
   target: `chrome --ozone-platform=x11` reaches first paint into a fake window,
   pixels read back into the TobyTK canvas. Everything after (visibility →
   comments/sidebar populate) should then follow from the compositor being real.

Definition of done for THIS arc: on a mainstream watch page, `ytd-comments`
gains real threads (`cmt2` children > 0, non-empty `textContent`, `offsetHeight`
> 0) and the sidebar shows populated video tiles — matching a HEADED host-chrome
control, verified across a `logs/run_x3.sh` batch, not one run.

---

## 5. Build / run mechanics

```bash
cd /c/CustomOS/tobyOS
bash logs/build_vid.sh                 # chromewin/initrd only, ~2.5 min
bash logs/build39.sh                   # full kernel touch, ~4 min (kernel change)
bash logs/defboot.sh                   # stock build must reach login, ZERO faults
/c/Users/tdude/AppData/Local/Programs/Python/Python311/python logs/run_watch.py  # 360 s WHPX
bash logs/run_x3.sh                    # build once, run 3x, auto-tabulate (USE THIS)
```
`START_URL` is a `#define` near the top of `programs/chromewin/main.c`. The DOM
census probe (`sec`/`cmt2`/`secIt`/`secTc`/`secH`/`cmtTc`/`cmtH`/`gate`/`ytd`)
is in `probe_page()` — extend it, do not rebuild it.

Read a finished run:
```bash
L=logs/run_watch.log
grep -ao "tobyprobe[^\"]*" $L | tail -3          # full page/video/richness state
grep -ao "ytd=[0-9]*" $L | sort -t= -k2 -n | tail # did the page finish building?
```

---

## 6. One-paragraph status

Video plays; the page and metadata render; the kernel is healthy. The remaining
"YouTube like a normal browser" gap (comments, sidebar tiles, most thumbnails)
is NOT a tobyOS bug — headless chrome omits those surfaces on real hardware too
(slice-60 control). Closing it means convincing a renderer its frames are
presented: try the cheap CDP-visibility spike first (Route A), and if that's
disproven on a known-good host, it's the headed-chrome arc (Route B) — full
`chrome` binary + an Ozone X11 backend onto tobyOS's existing (but
handshake-only) fake X server. Reproduce every claim on host Chrome first.
