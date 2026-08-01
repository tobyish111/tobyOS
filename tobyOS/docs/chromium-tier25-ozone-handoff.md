# Handoff: full Chromium on tobyOS — tier 2.5 headed Ozone close-out (post-slice-88)

**Read this whole file before touching the arc.** It supersedes
`docs/chromium-child-crash-handoff.md` (post-slice-87, now stale). Baseline
commit: **`7ae1bac`** (slice 88). Long-form evidence:
`docs/chromium-hypothesis-ledger.md` (slices 85–88) and the memory topic
`chromium-bringup.md`.

---

## 1. What this project is

tobyOS is a from-scratch OS that runs **real, unmodified Linux x86-64
binaries** through a "Track B" personality layer. The headline workload is
**genuine Chromium 151** — the stock binary, not a port — driven from a native
tobyTK window (`programs/chromewin`) over CDP `--remote-debugging-pipe`.

Two chrome flavours exist:

- **`chrome-headless-shell`** (default build, `logs/build_vid.sh`): works
  end-to-end — full multi-process engine, https, SwiftShader, VP9 on a real
  YouTube watch page, comments/sidebar at host parity, live window resize.
- **full `chrome`** (`CHROME_FULL=1`, `logs/build_full.sh`): required for
  tier 2.5 because **only the full binary has Ozone platform backends**.
  Stable for full sessions since slice 86/87. Currently launched with
  `--ozone-platform=x11` (headed) — see `programs/chromewin/main.c`.

## 2. THE TIERS ROADMAP — why any of this matters

Everything below serves one goal: **make video/page rendering as responsive as
real Chrome**. The perf arc was split into tiers; know where we are:

| Tier | Goal | Status | Evidence |
|---|---|---|---|
| **1** | Make our display path cheap | **DONE** | Frame-stage timers + in-place vectorized RGBA→ARGB swizzle adopting stbi's buffer. Decode ≈1 ms — the display path is ~free; frame *production* inside chrome is the bottleneck. |
| **2** | Remove kernel-side serialization | **DONE** | `sched_yield`/futex BKL fast paths; `/data` moved from ATA PIO to **virtio-blk** (BKL held **94% → 1.3%** of wall clock); event-driven poll wakeups (`epoll_wait` ~13× cheaper/call). react.dev **630 → 1050 frames**. |
| **2.5** | **Zero-copy frames** | **IN PROGRESS — you are here** | Sized at **~2.3×** (slice 68). Needs chrome to composite into memory we own = **Ozone X11 + MIT-SHM**. All infrastructure landed (slice 87); the headed bring-up wall was root-caused and mostly fixed (slice 88); **MapWindow reliability is the last gap** before wiring SHM as the default paint path. Cheap interims (device-scale-factor, screencast quality) were tested and **rejected** — no shortcut exists. |
| **3** | Real GPU (not SwiftShader) | Not started | Comes after 2.5. Likely path: virtio-gpu / Venus or passthrough experiments; nothing designed yet. |
| **4** | Audio | Not started | chrome currently runs with no audio backend (control shows it probing pulse and giving up cleanly). |

**Definition of done for tier 2.5:** headed chrome MapWindows on our in-kernel
fake X server, paints via `ShmPutImage` into the shared segment, `chromewin`
presents those pixels via `ABI_SYS_XFRAME_POLL` (both ends already
implemented!), screencast is dropped as the pixel source, and the frame rate is
re-measured against the slice-68 baseline (~1050 frames/react.dev,
933 frames/example.com @ q60 as the current JPEG-path reference).

## 3. What slice 88 established (all committed, all evidence-backed)

### The headed wall was a kernel ABI bug, not X protocol coverage

`lx_recvmsg`'s AF_UNIX branch honoured only `MSG_DONTWAIT` and **ignored
`s->nonblock`** (the fd's O_NONBLOCK) — unlike `lx_recv` and the UDP branch 30
lines above it. Chromium's X socket is O_NONBLOCK and its UI pump reads until
EAGAIN with `flags=0`: the first read on an empty ring parked the UI thread in
a **forever** blocking wait (`[uxstuck] pid=3 sock=4 xsrv=1 count=0 to=0`,
"READY in recvmsg" for 173 s). Every symptom in the old handoff — no
MapWindow, UI "parked on futex", `Target.createTarget` never answering — was
downstream of this. Mojo never tripped it because its channel reads pass
MSG_DONTWAIT explicitly; that is why ~90 slices of Mojo debugging never saw it.

### Control-rig experiments that killed whole theory families (minutes each)

- `logs/control_x11trace.sh` — Xvfb + xtrace **over TCP displays** (WSLg
  mounts `/tmp/.X11-unix` READ-ONLY; Unix-socket displays cannot bind).
  Captures the ground-truth wire sequence of this exact chrome+flags on a
  real X server. The reference logs live in `logs/control/`.
- `logs/control_x11deny.sh` — xtrace `--denyextensions`: chrome **maps its
  window with ZERO X extensions**. Our sparse extension surface (no XKB, no
  XI2, no RENDER, no SYNC…) is NOT the blocker. Do not add extensions to
  xserver.c on a hunch.
- `logs/control_x11nodbus.sh` — chrome maps with dbus dead. DBus exonerated.

### Three 1-vCPU scheduler fairness bugs, fixed

1. **Futex wake handoff** (`src/thread.c`): FUTEX_WAKE enqueued the waiter and
   let the waker keep the CPU; the waker re-acquired the mutex within
   microseconds and the woken thread lost EVERY retry — chrome's UI thread
   lost one mutex to a busy worker for 3+ minutes (130 ping-pongs on one
   address). Now a `val==1` wake whose waiter already waited **>5 ms** yields
   to it while the lock is still free. (20 ms was tried first and was TOO
   HIGH — the ping-pong re-waits every ~15 ms, so no cycle ever qualified.)
2. **`sched_yield` early-return path** (`src/sched.c`): the RUNNING+empty-queue
   fast path skipped the futex-timeout sweep and `poll_tick` entirely. Once
   the handoff calmed the system into that path, `[tick] fxsweep` froze for
   2+ minutes and six workers sat 110 s past their 60 s futex deadlines. The
   early path now drives both at 10 ms cadence (taking the BKL briefly for
   poll_tick when the yielder doesn't hold it).
3. **`SCHED_QUANTUM_BASE` 5 → 1 under CHROMIUM_BOOT** (`include/tobyos/sched.h`):
   50 ms slices were legal monopolies on one CPU; 10 ms interleaves chrome's
   ~40 threads the way Linux does.

**Counter-semantics gotcha that cost a build cycle:** `[tick] polltick=N`
counts wakes *performed*, not runs. A frozen counter is not a dead driver —
check what a counter counts before diagnosing.

### Where that leaves the headed run

Now happening (never happened pre-88): browser frame window created
(799×599), **full WM property suite** (WM_PROTOCOLS / WM_CLASS / _NET_WM_PID /
the 64 KB `_NET_WM_ICON` burst), `WM_NORMAL_HINTS` readback,
`AddKeepAlive(kBrowserWindow)`, navigation starts (`URL to scan`, omnibox
lines — the control's window-completion markers).

**Still open: `MapWindow` (op=8) has never been observed.** The stall now
*wanders* run to run — one run dies at the dri3 warning, another at WM
detection, another after the property burst. That shape is residual 1-vCPU
scheduling timing, **not** a missing feature: the control proves this exact
chrome+flags maps on a core-only X server, and every fixed bug moved the
average stall later.

## 4. YOUR TARGET — two candidate paths, pick by evidence

### Path A (recommended): fix the `-smp 4` AP arc, dissolve the whole class

One `-smp 4` trial froze at 7.6 s: **a dozen threads READY in clone/clone3 for
232 s with ZERO BKL acquisitions on cpu1-3** — the APs got SIPIs and did no
syscall work. This is the slice-87 `tg_vm_quiesce`/deferred-requeue machinery
(`sched_finish_switch`) interacting with AP ready queues: threads created
during a quiesce window are likely parked and the deferred requeue never fires
on the AP path. Fix that, and 4 CPUs make the entire waker/waiter starvation
class evaporate (full chrome ran whole sessions at `-smp 4` in the slice-86
era, before the quiesce landed). Start in `src/sched.c`
(`sched_finish_switch`, quiesce/resume) + `src/fork.c` (quiesce callers), and
check `enq_target_for` — the slice-39 comment says everything enqueues to the
BSP, so verify APs can even receive work post-quiesce. `run_watch.py` has the
`-smp` knob with the full story in a comment.

### Path B: keep grinding 1-vCPU fairness

The remaining stalls are contended-lock/timing lotteries. Tools that exist:
the wake-handoff threshold (5 ms — could go lower or become unconditional for
`val==1`), aging (`SCHED_AGE_STEP_TICKS`), and the `[xpoll]` probe (fires if a
poll times out while an x_server sock has unread data — if it EVER fires, the
poll readiness predicate lies and that's a real bug, not timing). This path
works but each experiment costs a ~7-minute guest run and the target moves.

### Then: tier 2.5 close-out (the actual prize)

Once MapWindow lands reliably:
1. Chrome's software presenter will `ShmAttach` + `ShmPutImage` into our
   segment (xserver.c implements MIT-SHM; `xframe_ensure` sizes the frame;
   gen counter bumps on paint).
2. `chromewin` already polls `ABI_SYS_XFRAME_POLL` and prefers xframe pixels
   when `gen` advances (`xframe_poll_once` in `programs/chromewin/main.c`) —
   it currently falls back to CDP screencast because gen never moves.
3. Drop `Page.startScreencast` once SHM frames flow; re-measure frames vs the
   slice-68 baseline. Claimed win: ~2.3×.
4. Only then mark tier 2.5 DONE, then design tier 3 (real GPU), then tier 4
   (audio).

## 5. Instruments you have (post-88)

- `[uxstuck]` — any blocking UNIX recv >5 s names its pid, socket slot,
  x_server flag, ring depth, peer, timeout. This is what cracked slice 88.
- `[xdbg]` — dumps the x_conn gate state (rlen/pend/armed/count) when
  [uxstuck] fires on an X socket.
- `[xpoll]` — poll(2) timing out while a polled x_server sock has queued
  data = readiness predicate lying. Has never fired; if it does, chase it.
- `poke MUTED` — the idle-wake flood guard went silent for a conn (≥8 unread
  events). Means the client stopped reading X — find where its thread went.
- `[tick] fxsweep/polltick`, `[cur]`, `[futex]` (per-addr, capped), `[lx-recent]`
  syscall ring WITH return values, `[sigfault]` + canary dump.
- WSL control rig: `control_x11trace.sh` / `x11deny` / `x11nodbus` /
  `x11vmod` (+ the older `control_*.sh`). Invocation form:
  `timeout N env VAR=... ./chrome` — env AFTER timeout.
- `--vmodule` is already wired in chromewin's argv for the UI-path modules.

## 6. Build & run

```bash
bash logs/build_full.sh                  # FULL chrome, headed x11 flags
timeout 300 python logs/run_watch.py     # QEMU/WHPX, -smp 1 -m 8192
```

- Serial lands in **`logs/run_watch.log`** (NOT `logs/serial.log`).
- Screenshots: `logs/wat_*.png`. chromewin markers stream to stdout.
- Headless flavour (regression check): `logs/build_vid.sh`.

### Hard rules — every one has burned a session

- Widely-included header changed, or `struct proc`/`struct sock` grew ⇒
  **delete ALL kernel `.o`** and rebuild. (`rm -f src/*.o`)
- `programs/chromewin/chromewin.o` must die on any flavour change (the build
  scripts do it; keep it that way).
- **Verify the ISO mtime after every build.** A silent failure once ran a
  stale ISO for a whole batch.
- `kprintf` has no `%o` (print modes as hex) and no `%f`.
- Never `yield` holding the BKL; shootdown waits: self-ack OK, never `sti`,
  never drop the BKL there.
- **Never `git add -A` from the repo root**: `programs/chromium/chrome-linux64/`
  is 574 MB. It is `.gitignore`d now, but stay paranoid — it nearly got
  committed once (caught only via CRLF warnings).
- The x_server sockets are exempt from the "peer gone ⇒ EOF" rule by design
  (`peer_ip==0` is their normal state). Don't "fix" that.

## 7. Method lessons (accumulated, all paid for)

- **A userspace stall with no failing syscall means a wrong VALUE or a wrong
  WAIT, not a wrong errno.** Slices 86 and 88 were both cracked by asking
  "what succeeded that shouldn't have / what is it actually blocked ON",
  never "which errno is wrong".
- **Trace the real thing on the control first.** One xtrace run against Xvfb
  produced the ground-truth spec; `--denyextensions` and dbus-dead runs each
  killed a theory family in under a minute of guest-equivalent time.
- **Instrument the general mechanism, not the specific suspicion** (syscall
  ring return values; [uxstuck] naming the socket).
- **Check what a counter counts** before declaring its driver dead.
- Disassembly neighbourhoods lie (clang pads with `int3` — slice 85's
  retraction). ASCII bytes in pointer registers ⇒ heap corruption.
- When a fix regresses something, **retract it in the ledger immediately** —
  this arc has been derailed twice by stale confident claims.
