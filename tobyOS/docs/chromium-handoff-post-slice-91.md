# Handoff: Chromium on tobyOS — RECONFIGURED perf roadmap (post-slice-91)

**Read this whole file before touching the arc.** It **supersedes
`docs/chromium-tier25-ozone-handoff.md`**, which is now actively misleading:
its target (tier 2.5, zero-copy frames via Ozone X11 + MIT-SHM) was
**disproven** in slice 91. Baseline commit: `1c5142a`. Long-form evidence:
`docs/chromium-hypothesis-ledger.md` (slices 85–91) and the memory topic
`chromium-bringup.md`.

---

## 1. What this project is

tobyOS is a from-scratch OS that runs **real, unmodified Linux x86-64
binaries** through a "Track B" personality layer. The headline workload is
**genuine Chromium 151** — the stock binary, not a port — driven from a
native tobyTK window (`programs/chromewin`) over CDP
`--remote-debugging-pipe`.

Two flavours:

- **`chrome-headless-shell`** (default, `logs/build_vid.sh`): works
  end-to-end — multi-process engine, https, SwiftShader, VP9 on a real
  YouTube watch page, comments/sidebar at host parity, live resize.
  **The published frame baselines were measured on THIS flavour.**
- **full `chrome`** (`CHROME_FULL=1`, `logs/build_full.sh`): brought up
  across slices 69–91 specifically to serve tier 2.5. Now reaches
  **`bootstrap OK`** headed (live CDP session) — but see §3, that no longer
  buys what it was brought up for.

## 2. THE RECONFIGURED ROADMAP

The single goal is unchanged: **make page/video rendering as responsive as
real Chrome.** What changed is which tiers can deliver it.

| Tier | Goal | Status |
|---|---|---|
| **1** | Make our display path cheap | **DONE, MEASURED.** Frame-stage timers + vectorized RGBA→ARGB swizzle. Decode ≈1 ms. **Its own conclusion, which the arc then ignored: the display path is ~free; frame PRODUCTION inside chrome is the bottleneck.** |
| **2** | Remove kernel serialization | **DONE, MEASURED.** BKL fast paths, `/data` ATA PIO → virtio-blk (BKL held 94% → 1.3%), event-driven poll wakeups. react.dev **630 → 1050 frames**. |
| **2.5** | Zero-copy frames (Ozone X11 + MIT-SHM) | **CLOSED — PREMISE DISPROVEN (slice 91).** Not abandoned, not deferred: the mechanism does not exist. See §3. |
| **A** | **Fix the intermittent SMP freeze** | **NEW, AND THE BLOCKER.** Real, reproducible, unfixed. Invalidates every measurement, so it gates everything below. |
| **B** | **Measure where frame time actually goes** | **NEW.** Never done. This is the measurement tier 2.5 should have been built on. |
| **C** | **Act on B** | **NEW.** Candidates ranked in §6, chosen by B's data — not by assumption. |
| **3** | Real GPU | Re-scoped and **gated on B**. May be a PRESENTATION fix, not just a rasterization one — which would make it more valuable than the old roadmap assumed, and more work. |
| **4** | Audio | Unchanged, independent of all the above. No backend exists; chrome probes PulseAudio and gives up cleanly. |

## 3. Why tier 2.5 is closed — do not reopen it as "make ShmPutImage work"

Tier 2.5 assumed: chrome, on an X11 Ozone backend, composites into an
MIT-SHM segment we own, and we present those pixels with zero encode/decode.
Today's path instead has chrome **encode every frame as JPEG** over CDP and
us **decode** it — pure overhead a native browser never pays. Slice 68 sized
removing it at ~2.3×.

**Chromium does not present pixels through X. Measured on the CONTROL — a
real X server (Xvfb + xtrace), real page (`https://example.com`), 90–120 s,
the same chrome binary:**

| flags | PutImage | ShmPutImage |
|---|---|---|
| our guest set (`--use-gl=disabled --disable-gpu --single-process`) | 0 | 0 |
| `--disable-gpu-compositing --enable-unsafe-swiftshader --single-process` | 0 | 0 |
| same, **multi-process** | 0 | 0 |

The third run had **66 extensions `present=true`** — MIT-SHM, DAMAGE,
RENDER, GLX, Composite, SYNC, XFIXES, RANDR, SHAPE, XKEYBOARD, XTEST — with
the page confirmed loaded. Chrome **attached** an SHM segment, **detached**
it, queried GLX fbconfigs, created and destroyed a **GLX pbuffer**, and
rendered **offscreen**. It never pushed a pixel to its X window.

Therefore: **no amount of work in `src/xserver.c` can produce a tier-2.5
frame.** Slice 68's "~2.3×" was an estimate of what zero-copy *would* save,
never a measurement of it working — **treat it as void.**

**Reproduce in ~2 minutes** (do this before doubting the above):

```bash
sed 's/^timeout 45 env/timeout 120 env/; s#x11wire.txt#x11check.txt#g' \
    logs/control_x11trace.sh > /c/t/ctl_check.sh
MSYS2_ARG_CONV_EXCL='*' wsl -e bash /mnt/c/t/ctl_check.sh >/dev/null 2>&1
grep -acE "Request\(72\)|PutImage" logs/control/x11check.txt   # expect 0
grep -aoE "MIT-SHM-Request\([0-9]+,[0-9]+\): ?[A-Za-z]*" \
    logs/control/x11check.txt | sort | uniq -c
```

**What tier 2.5 DID buy, and is worth keeping:** the fake X server is now
correct enough that headed full chrome reaches a live CDP session
(`bootstrap OK`) — MapWindow, the WM property suite, EWMH activation, and
the reply-vs-silence fix (§5). Keep it. Just do not expect frames from it.

**OPEN QUESTION worth answering early (cheap):** the 933/1050 frame
baselines were measured on **chrome-headless-shell**. Headed full chrome may
be *slower*, and it no longer offers a zero-copy payoff. Measure both
flavours on the same page and keep whichever wins for the perf goal. If
headless-shell wins, the headed path's value is UX/fidelity, not speed —
decide deliberately rather than by inertia.

## 4. TIER A — the freeze. Fix this FIRST.

Everything downstream is a measurement, and **you cannot measure on a guest
that freezes.** Several slices' worth of X-protocol conclusions were drawn
from frozen runs before this was understood.

**Signature (intermittent, `-smp 4`):**

```
[cur] pid=19..25 READY in clone3 for ~293000 ms   (six threads)
[bkl] cpu0 acq=0  cpu1 acq=0  cpu2 acq=0  cpu3 acq=0
```

Zero BKL acquisitions on **all** CPUs = the whole kernel is wedged, not
chrome stalling. Worst shape is total serial silence — even the 3 s
`sched_tick` heartbeat stops — so nothing in-guest can report it.

**GATE, apply to every run before believing anything:**

```bash
grep -a '\[bkl\] cpu' logs/run_watch.log   # EMPTY => guest froze; the run
                                           # says NOTHING about chrome
```

**Prime suspect** (flagged since slice 87, still unproven): the CoW-fork
stop-the-world handoff. `cow_fork_lock_acquire` parks a forker that observes
`vm_quiesce` as `PROC_BLOCKED` spinning on the flag; `tg_vm_resume` declines
to requeue a sibling still `on_cpu` and leaves `vm_quiesced=1` for
`sched_finish_switch` to finish. **If that never runs for that proc, nothing
ever requeues it.** Every other lock here has a timeout backstop
(`cow_fork_lock` steals after 20M spins, `tg_vm_quiesce` gives up after
500k); the deferred requeue has none. Slice 89 added SEQ_CST fences to both
sides (release/acquire let each side read the other's stale value on x86) —
correct, keep them, **not sufficient**.

**Tools built for this, never yet successful:**

- `logs/run_freeze.py` — runs the real config; on serial stall captures
  `info registers -a` for **every vCPU, twice, 3 s apart** via QMP.
  **Identical RIPs = halted/deadlock; moving = live spin.** Same method that
  cracked the BKL double-acquire freeze.
- `logs/symbolize_freeze.sh` — `addr2line` those RIPs against `tobyos.bin`
  (full symbols; ring-3 RIPs are reported as such).
- `[qstuck]` in `sched_tick`'s heartbeat — requeues any proc with
  `vm_quiesced && !vm_quiesce && BLOCKED && !on_cpu`, logging loudly.
  **It has never fired.** That is itself information: either the mechanism
  is different, or the procs are stuck `on_cpu` (which the sweep skips by
  design). Consider logging the near-misses too.

**Caveat on `[qstuck]`, and it is mine:** it runs in **IRQ context** and
calls `sched_enqueue`, which takes run-queue spinlocks. It never fired, but
runs after it landed froze earlier (~7.6 s) than runs before (14–16 s). I
could not establish cause. **If the freeze looks more frequent, revert
`3a17751` first and re-measure** — this codebase deliberately moved futex
sweeps *out* of IRQ context for related reasons.

## 5. What slice 91 fixed (all committed, all keep)

- **Unhandled reply-bearing X requests now return an ERROR, not silence.**
  In X11 a request carrying a reply that never gets one blocks the client in
  `recvmsg` **forever** — measured `pid=5 RUNNING in recvmsg for 174 s` with
  our ring drained (`rx=0 pend=0`). That is the mechanism behind the
  "wandering stall" that ate four slices: chrome stops at whichever
  reply-bearing opcode it reaches first that we lack, and *which* one
  depends on timing. The old comment feared error replies; the control shows
  chrome probes the None window, takes `BadWindow` + `BadDrawable` back to
  back, and carries on. **Errors are normal traffic.** Void opcodes keep the
  silent ignore. `x_op_has_reply()` encodes the core-protocol reply set.
  **Result: `bootstrap OK` at 45.8 s — first live headed CDP session ever.**
- Missing post-MapWindow events from the control: `VisibilityNotify`,
  `ConfigureNotify`, `FocusOut`/`FocusIn`.
- **`SendEvent(25)` had no handler at all.** Chromium doesn't call
  `SetInputFocus` to activate its own frame — it asks the WM via a
  `_NET_ACTIVE_WINDOW` ClientMessage, which vanished every time.
- **Stopped claiming to be a WM** (`_NET_SUPPORTING_WM_CHECK` → None). The
  control is Xvfb with no WM, where chrome self-manages; claiming a WM put
  chrome on the delegating path, waiting for cooperation that never came.
  **Still UNVERIFIED** — its test run froze (see the §4 gate).
- **A counter that would have faked the close-out:** `xframe_ensure()` (a
  RESIZE, handing back a blanked buffer) bumped the same `gen` chromewin
  reads as "pixels painted". A run where chrome never painted still printed
  `[chromewin] xframe 1: 799x599 (MIT-SHM path)`. Only real pixel writes
  bump it now. **A frame counter that counts non-frames cannot decide
  whether a tier is done.**

Earlier in the session: the crashpad handler "quiet death" was **our own
stub** (the Makefile staged the `xdg-settings` stub, body `exit_group(0)`,
as `chrome_crashpad_handler`); `programs/linux-crashpad` now performs the
real handshake and stays alive, plus `rt_sigtimedwait(128)` /
`rt_tgsigqueueinfo(297)` were filled.

## 6. TIER B → C — the measurement, and what to do with it

**B: measure where a frame's time actually goes, now that tier 2 is done.**
Nobody has. Tier 1 measured *our* side (~1 ms decode) and correctly said the
bottleneck is inside chrome — then the arc spent twenty slices on the
display path anyway. Do not repeat that.

What to instrument:
- our side: the tier-1 frame-stage timers already exist;
- chrome's side: the ring-3 sampling profiler (`prof_sample` /
  `prof_dump_and_reset` in `sched.c`) names hot user RIPs per interval;
  correlate with `[libmap]` bases to attribute them.

The question B must answer: **of the wall-clock between frames, how much is
chrome's JPEG encode, how much is CDP pipe transport, how much is our
decode (~1 ms, known), and how much is chrome's raster/layout?**

**C: candidates, ranked by structural promise. Pick by B's data, not this
ordering.**

1. **viz shared-memory frames over Mojo.** Chrome's renderer→viz transport
   already uses shared-memory buffers over Mojo — and **we already implement
   Mojo transport, `memfd`, and `SCM_RIGHTS`.** This is a zero-copy-shaped
   opportunity through a mechanism chrome demonstrably uses *internally*,
   unlike X. Strongest candidate; also the least explored.
2. **Cheaper screencast transport.** Only worth it if B shows encode/pipe
   dominating. Note device-scale-factor and quality knobs were already
   **tested and rejected** (they changed nothing) — don't re-run those.
3. **Tier 3 (GPU).** See §7.
4. **Accept the current path** and re-aim at responsiveness elsewhere
   (input latency, navigation time) if B shows frame cost is already small.

## 7. TIER 3 — re-scoped

`docs/chromium-tier3-gpu-design.md` still stands, with two corrections.

Already in that doc: the tier description ("replace SwiftShader") was stale —
the headed path runs `--disable-gpu --use-gl=disabled`, i.e. **CPU
rasterization, no GL at all**. And `grep -r DRM_IOCTL src/` returns
**nothing**: no `/dev/dri/card0`, no ioctl surface. That missing device node
*is* tier 3. The plan is to stage prebuilt Mesa (same trade as chrome's 60+
DSOs) and implement the virtio-gpu ioctls — **not** to write a GL driver.

New from slice 91:

- **Tier 3 is no longer gated on tier 2.5** (that gate assumed 2.5 would
  change the display contract; it won't). It is now gated on **B**.
- **Tier 3 may be a PRESENTATION fix, not just a rasterization one.** The
  control showed chrome doing GLX work — `glXGetFBConfigs`, pbuffer
  create/destroy. That hints its real presentation path here is GL-shaped
  (`glXSwapBuffers`, or DRI3/Present), not MIT-SHM. If so, tier 3 could be
  the thing that makes presentation happen at all — more valuable than the
  old roadmap assumed, and more work, because you need the GL *presentation*
  path, not just a rasterizer.
- **Phase 0 must be stricter than "does QEMU advertise VirGL".** Tier 2.5
  died of an unverified mechanism; do not repeat the shape. Before building
  anything, run a **control** proving chrome, on a real system with a real
  GPU stack, actually **presents frames** through the mechanism you intend
  to implement. `run_watch.py` attaches **no GPU device at all** today.

## 8. Instruments available

`[xsum]` (per-X-conn seq + **ms since last request** — splits "chrome is
waiting on us" from "chrome stopped talking"), `[xsrv] req c=/op=/seq=`,
`[xsrv] unhandled opcode` (now says whether a reply was owed),
`[uxstuck]`/`[xdbg]` (blocking UNIX recv >5 s names its socket — **note it
can no longer fire on the X socket, because our own 100 ms idle-poke keeps
every wait short: slice 88's fix blinded slice 88's instrument**),
`[qstuck]`, `[dsched]` (dual-schedule tripwire), `[xexit]` with **QUIET
DEATH** (fires on exit 0 with <32 syscalls — the old non-zero gate made the
one exit that mattered the one exit never seen), the syscall ring **with a1
(the fd) and return values**, `[uxgen]` (stale AF_UNIX slot links),
`[bkl]`/`[cur]`/`[wait]`/`[tick]`, `[libmap]`, `prof_sample`.

Control rig: `logs/control_x11trace.sh` / `x11deny` / `x11nodbus` /
`x11vmod`, plus the `sed`-a-variant recipe in §3. Invocation form:
`timeout N env VAR=... ./chrome` — **env AFTER timeout**. From the Windows
shell, WSL paths need `MSYS2_ARG_CONV_EXCL='*'`.

## 9. Build & run

```bash
bash logs/build_full.sh                  # FULL chrome, headed x11 flags
bash logs/build_vid.sh                   # headless-shell flavour
SMP=4 timeout 420 python logs/run_watch.py
python logs/run_freeze.py                # freeze hunt w/ QMP register capture
bash logs/defboot.sh                     # stock-config boot regression
```

Serial lands in **`logs/run_watch.log`** (not `serial.log`). Screenshots:
`logs/wat_*.png`.

### Hard rules — every one has burned a session

- **Check `grep -a '\[bkl\] cpu' logs/run_watch.log` is non-empty before
  believing a run.** Empty = frozen guest = the run says nothing.
- Widely-included header changed, or `struct proc`/`struct sock`/`struct
  file` grew ⇒ **delete ALL kernel `.o`** (`rm -f src/*.o`) and rebuild.
- `programs/chromewin/chromewin.o` must die on any flavour change.
- **Verify the ISO mtime after every build** (`ls -la --time-style=+%H:%M:%S
  tobyOS.iso` vs `date`). A stale ISO once ran for a whole batch.
- `bash logs/defboot.sh` **rebuilds the STOCK ISO** — rebuild the chrome
  flavour before the next chromium run.
- `kprintf` has no `%o` (print modes as hex) and no `%f`.
- Never `yield` holding the BKL. **Any blocking syscall wait must drop the
  BKL and yield/hlt cooperatively** — copy `sock_unix_recv`'s shape. A
  `sched_yield` spin holding the BKL wedged the whole guest at 8.7 s.
- Linux `sigset_t` uses bit *(signo-1)*; tobyOS `SIGMASK(s)` is `1u << s`,
  i.e. bit *== signo*. Convert explicitly.
- **Never `git add -A` from the repo root**: `programs/chromium/chrome-linux64/`
  is 574 MB. It is `.gitignore`d — stay paranoid. `logs/` is also ignored;
  new tools there need `git add -f`.
- The Bash tool's **cwd persists** — a `cd /c/CustomOS` in one call breaks
  relative paths in the next. Use absolute paths.
- The x_server sockets are exempt from "peer gone ⇒ EOF" by design
  (`peer_ip==0` is normal). Don't "fix" it.

## 10. Method lessons — all paid for, several this slice

- **Verify your control actually DOES the thing before trusting it as the
  spec for that thing.** `control/x11wire.txt` was used as ground truth for
  "reaching MapWindow + ShmPutImage" across several slices. It never reached
  the paint stage either — it ends at MapWindow and teardown.
- **Never exonerate an INTERMITTENT failure from a handful of clean runs.**
  Slice 89 declared the `-smp 4` freeze dead after four clean sessions. It
  reproduces. Absence in 4 samples is not absence.
- **A fix that changes nothing is a disproof, not a win.** The AF_UNIX slot
  generation counter (slice 91) was built to test slice 78's hazard;
  `[uxgen]` fired **zero** times and the signature was byte-identical. Kept
  as a real hazard closed, credited with nothing.
- **When a program behaves impossibly for its TYPE, check you are running
  the program you think you are** before theorising about the kernel. The
  crashpad "quiet death" was our own stub; one ELF-header dump settled what
  a slice of kernel theory did not.
- **Never put a stall diagnostic on the idle path.** `[xsum]` printed
  nothing for a whole 360 s run because it hung off pid 0's `idle_loop`,
  which never runs under chrome load.
- **A counter that counts non-frames cannot decide whether a tier is done.**
- **A userspace abort with no failing syscall before it means a wrong VALUE
  in something that SUCCEEDED**, not a wrong errno.
- **Check what a counter counts** before declaring its driver dead
  (`[tick] polltick` counts wakes performed, not runs).
- Disassembly neighbourhoods lie (clang pads with `int3`). ASCII bytes in
  pointer registers ⇒ heap corruption.
- **When a fix regresses something, retract it in the ledger immediately.**
  This arc has been derailed more than once by stale confident claims.

---

# ADDENDUM (slice 92, 2026-08-01/02): TIER A IS CLOSED — the freeze was caught, symbolized, root-caused, fixed, and re-verified

**Read `docs/chromium-hypothesis-ledger.md` slices 92/92b for the full
evidence chain.** Summary:

* **The freeze specimen was captured** (freeze-hunt run 5, QMP registers
  x2): one CPU looping FOREVER inside `futex_expire_timeouts`' waiter-list
  walk while HOLDING `g_futex_lock` with IRQs off — **the waiter list was
  CYCLIC** — with the other CPUs spinning on the same lock. Total serial
  silence, `acq=0`, `[qstuck]` blind: every §4 signature fact falls out of
  this one state. It landed microseconds after `[fork] clone-returning`.
* **Cycle mechanism:** any spurious READY of a futex-parked thread (it
  returns "woken" still linked; glibc re-parks; head-insert closes the
  loop). Spurious sources found: `sched_finish_switch`'s deferred CoW-STW
  requeue firing on a STALE `vm_quiesced` flag planted by apic.c's TLB-ack
  demotion (hence post-fork, hence -smp 4), and the signal path (wakes
  BLOCKED procs without futex-unlink).
* **Fixes (4ee50f9 + 0dd4228):** `futex_unlink_self()` after every futex
  park (structural: any spurious waker degrades to a legal spurious
  wakeup); bounded+self-healing expire walk (`[fxcycle]`); apic.c undoes
  its demotion; `vm_quiesce_park_oncpu()` drops the BKL in the kernel-#PF
  quiesce park (rule violation removed; the deadlock theory built on it
  was RETRACTED by its own A/B — see ledger 92).
* **Verification:** `[fxspur]` fired ONCE in 6 fresh 360s -smp 4 headed
  runs — at guest 10.5s, the freeze's exact signature window — and the run
  sailed on; 6/6 runs passed the `[bkl]` gate; `[fxcycle]` never fired.
  Causal, not statistical: the fix targets the captured state and the
  trigger was observed firing and being defused.
* **Watch items:** `[fxspur]` firing occasionally is the mechanism being
  absorbed (fine). `[fxcycle]` firing means a cycle STILL formed — hunt
  the new spurious source immediately.
* **New permanent guard:** QFREEZETEST (`programs/linux-qfreeze`, console
  flavour) — fork-STW + syscall-hammer under true SMP (it enables AP run;
  the boot harness is otherwise BSP-only, which had been silently
  weakening every console guard's SMP claims). Exit 2 = VACUOUS (hammers
  starved), never trust a PASS without it having been live.

## §3's OPEN QUESTION is ANSWERED — with data, not inertia

Across **six 360 s headed full-chrome runs** (-smp 4, all gate-clean):
`bootstrap OK` was reached ONCE (at 107 s; the arc's second time ever) and
**ZERO screencast frames were produced in 2160 s of headed runtime**.
Headless-shell's baseline on the same page is **933 frames / 360 s**.

**Decision: the perf vehicle is chrome-headless-shell.** Tier B (frame-time
breakdown: chrome's JPEG encode vs CDP transport vs our ~1 ms decode vs
raster/layout) proceeds on the headless flavour. The headed path remains a
fidelity/UX bring-up (its own blocker: chrome goes silent pre-bootstrap on
the X conns — timing, not missing protocol), NOT a perf path. Do not spend
perf slices on it.

## Where tier B starts (next session)

1. `bash logs/build_vid.sh`, `SMP=4 timeout 420 python logs/run_watch.py`
   — re-baseline headless frames post-slice-92 (the futex fix may itself
   move the number; measure before instrumenting).
2. Our side is already timed per 30-frame window (`[chromewin] frame N:
   ... b64= dec= paint=`). The missing numbers are inside chrome:
   correlate `prof_dump_and_reset`'s hot user RIPs (+`[libmap]`) across
   the frame interval, and consider timestamping `Page.screencastFrame`
   arrivals vs acks in chromewin to split encode vs transport.
3. Pick tier C from that data (§6 candidates), not from the ranking.

## Post-fix headless re-baseline (slice 92, measured 2026-08-02)

`build_vid` + `run_watch.py`, example.com, 360 s, both gate-clean,
`fxspur=0`, zero freezes (`logs/rebase_smp1.log` / `rebase_smp4.log`):

| config | frames/360 s | our per-frame cost |
|---|---|---|
| SMP=1 | ~270–299 (last window: frame 270) | b64=0 ms dec=1 ms paint=0 ms |
| **SMP=4** | **~930–959 (last window: frame 930)** | b64=0 ms dec=1 ms paint=0 ms |

-smp 4 is now both SAFE (tier A fixed) and ~3.3x SMP=1 in frame delivery —
**make -smp 4 the default operating point for all tier-B measurement.**
The SMP=4 number matches the historical 933; treat ~930/360 s @ SMP=4 as
the post-slice-92 baseline. Tier-1's conclusion re-verified: our display
path is ~1 ms/frame; the interframe time lives inside chrome.

## ADDENDUM (slice 93): TIER B DONE — see ledger slice 93 for the tables

One line each: the interframe time was **capture policy**, not
encode/transport/display (those are ~1-10 ms). `everyNthFrame` 3->1 in
chromewin (now the default, `CW_NTH` knob) took animated delivery
**13 -> 27 fps** (ack loop sustains ~21 ms) and the example.com metric
**930 -> ~2820 frames/360 s**. The renderer composits at ~52 Hz. New
instruments: `[cwif]` interframe decomposition, `[cwping]` CDP RTT +
JS-liveness, `/etc/anim.html` damage generator (`-DCW_URL` to use).
Next ceiling: the single-in-flight ack loop (~21 ms = capture+encode+2
pipe hops). Candidates: ack-before-decode in chromewin; re-test the
JPEG quality knob AT nth=1; kernel IPC wake latency (pings cost
32-86 ms under load). Baselines before slice 93 were nth=3 — annotate
any cross-slice comparison.
