# Handoff: make Chromium on tobyOS feel responsive

Read this file first, then `docs/chromium-handoff-post-slice-108.md` §5 (the
prior perf roadmap — it is still substantially correct and this file builds on
it rather than replacing it). Memory topics: `chromium-bringup.md`,
`browser-webgl-swiftshader.md`. Baseline commit: `88efd21` on
`feat/chromium-browser`.

**The user's ask, verbatim: "make it much more responsive like real chrome or
edge is."** They browse on a real HP EliteDesk 800 G1 (Haswell i5-4590, 4
cores, 8 GB), not in QEMU, and they report both slow page loads and a
generally sluggish browser.

---

## 1. The one number that matters, and where it comes from

Steady-state frame cadence on a real page (Bing SERP), QEMU, current default
build:

```
[cwif] frame 780 | gap avg=122ms max=714ms | cap avg=140ms | turn avg=...
```

**~8 fps.** On the user's real hardware, a static page measured `cap
avg=366ms` (~2.7 fps).

What the fields mean (`handle_screencast_frame`, programs/chromewin/main.c):

* `cap` — delta between **chrome's own** capture timestamps. Chrome's
  production rate.
* `turn` — our ack → next frame arriving.
* `gap` — wall time between frames landing in chromewin.

**The screencast is SINGLE-IN-FLIGHT: chrome captures frame N+1 only after we
ack frame N.** So `cap` ≈ our ack latency + chrome's capture+JPEG-encode+
transfer. Every frame pays a full JPEG encode, base64, pipe round trip,
base64 decode, JPEG decode, blit. That is the architectural ceiling, and it
is why this will not reach Chrome-like smoothness by tuning.

---

## 2. What was just fixed (do NOT re-chase these)

**Sleeping processes were busy-waiting (slice 128, commit `88efd21`).**
`sys_nanosleep` pause-spun to its deadline with a `sched_yield()` per
iteration — the process stayed READY and paid a BKL round trip per yield.
chromewin's main loop is one `usleep(15000)` per pass, so this was the
browser's steady state. Measured on the EliteDesk over 17.5 s:

| process | CPU | share |
| --- | --- | --- |
| `chromewin` | 6027 ms | ~34% of a core, doing nothing |
| `chrome-headless-shell` | 1492 ms | ~8.7%, the thing rendering |

Chrome was **starved, not render-bound**. Sleepers now park with
`sleep_deadline_ns` and are woken by a sweep at the top of `sched_yield`
beside the alarm/futex sweeps. **Measured: 570 → 780 frames (+37%) on the
same page and run length. Per-frame cadence UNCHANGED (~116 ms)** — which is
precisely what localises the remaining bottleneck to the screencast round
trip rather than to CPU.

Also fixed today and load-bearing for perf work: the shm-cache exhaustion
that silently un-shared MAP_SHARED regions past 256 (see
`bing-brjs-blank-serp` memory) — **multi-process chrome depends on that
path**, so measurements taken before it are suspect.

---

## 3. THE LIVE ITEM: the fast path exists and is not being used

`docs/chromium-handoff-post-slice-108.md` §5 item 2 records that slice 110
**measured chrome committing ~52.7 frames/s** through the viz shared-bitmap
path, at producer parity with the page's own rAF (61/s). That is ~6.5× what
we ship.

**We ship the slow path.** The default build is CDP JPEG screencast. The fast
path is `-DCW_VIZ`, and per `logs/gpuperf.sh` it is ALWAYS paired:

```
viz)  PROGF="$PROGF -DCW_MP -DCW_VIZ"     # multi-process + read viz shm bitmaps
```

**`CW_VIZ` requires `CW_MP`, and that is structural, not incidental**: viz
shared bitmaps only exist when the renderer and viz are separate processes.
Under `--single-process` (our default) the transport cannot exist at all —
`main.c` says so at the `--single-process` flag.

So the chain is: **multi-process → viz shared bitmaps → ~53 fps.**

### What I measured, and what I did NOT establish

* `-DCW_MP` **alone**, via `logs/cwnet.sh`: **zero frames**, a child exits
  with code 3. But this is a configuration the project never uses — CW_MP was
  only ever an shm-census arm. Do not read it as "multi-process is broken".
* `-DCW_MP -DCW_VIZ` via `bash logs/gpuperf.sh viz anim`: the gate failed
  (`GATE FAIL: full-length run with no '[bkl] cpu' report`) and the archived
  log `logs/gpuperf_viz_anim.013.log` **ends at 3733 ms while the gate reports
  the guest reached 250588 ms** — so the archive is truncated or points at the
  wrong file. **My analysis of that run is inconclusive, not a failure
  verdict.** Start by fixing that log plumbing so the run can be read at all.

### Job 1, concretely

1. Make `gpuperf.sh viz` produce a readable full-length log. Check which file
   it archives vs which one `run_watch.py` writes (`logs/run_watch.log`).
2. Determine whether multi-process chrome still bootstraps at all on the
   current kernel. Slice 109 root-caused an earlier multi-process flake (a
   proc-slot allocation race, fixed with an atomic PROC_EMBRYO claim) — check
   whether that regressed, and note that today's shm-cache fix changes the
   MAP_SHARED behaviour multi-process depends on.
3. If it bootstraps, measure `viz` against the `cpu` arm on the SAME page and
   ship whichever wins **as the default**, with the number in the commit.

That is the item with a measured ~6.5× on the table. Everything else on this
list is single-digit percent.

---

## 4. Ranked levers after that

2. **Kill the JPEG round trip even on the CDP path.** `chromewin` already
   switches to a zero-copy blit ON EVIDENCE (`XF_LIVE_FRAMES` real SHM
   frames, then `Page.stopScreencast`) — but `xframe_poll_once` is gated on
   `CHROME_FULL`, the headed/Ozone build. Its own comment sizes JPEG at ~2.3×
   waste.
3. **execve holds the BKL ~730 ms per exec'd chrome process** (slice 111,
   §5 item 3 of the older handoff). Bootstrap/navigation only, but the user
   explicitly complains about *load* time, so this lands where they feel it.
   Named follow-up there: phase-time `sys_execve` (vfs_read_all vs mapping vs
   BSS zero) before attempting the BKL drop — careful, `g_pml4_phys` editor
   root is a shared global.
4. **WebGL costs ~10–15%** (`cwwebgl` cap 136–150 ms vs `tcpfix` 119–140 ms).
   It ships enabled via `-DCW_SWGL` because the user asked for it. If perf
   wins over capability, drop it — but ASK, do not decide unilaterally.

### Closed — do not reopen

* **Tier 3 (host GPU) for chrome.** Measured no. `browser-webgl-swiftshader`
  explains why SwiftShader is NOT a reopening of it.
* **viz poll cadence.** 4 ms measured *worse* than 15 ms (slice 107).
* **Event-instead-of-poll viz delivery.** Bounded at ≤8% by slice 110.

---

## 5. How to measure (and how these harnesses lie)

```bash
# one page, real network, frame cadence + net failures
URL="https://www.bing.com/search?q=perf+test" TAG=myrun RUNSECS=220 \
  bash logs/cwnet.sh

# the perf A/B harness (arms: cpu gl gle gld mp viz vizp lat)
bash logs/gpuperf.sh viz anim
```

Read the log as: `tr -d '\000' < logs/X.log | tr '\r' '\n'` — **the serial
capture carries NUL bytes**, and a pipeline that forgets flips grep into
binary mode on some hosts.

**Traps that have each cost a wrong conclusion in this tree. Every one is
real and recent:**

* **The serial log splits every `printf` at its format conversions.** A line
  like `[chromewin] profile /data/cr2: REUSED (1 entries)` lands as
  `[chromewin] profile ` / `/data/cr2` / `: REUSED (` / `1` / …, interleaved
  with `[fd1] len=N:` markers. **Whole-line greps match verdict-less
  fragments.** Match tokens; do not try to reassemble (each fragment appears
  twice, so `grep -o` output concatenates to garbage).
* **Capped loggers hide the evidence they were added to collect.** Four
  separate instances in two days (`wu<24` window updates, `c<200` shm maps,
  `warns<8` TLB, `rx_full_episode`). **Every cap must announce itself when
  hit.** "The line stopped appearing" is not "the event stopped happening".
* **An instrument in the critical path becomes the bug.** The "tiny TCP
  window" pathology was a `kprintf` sitting between deciding to send a window
  update and sending it — ~13 ms of 38400-baud serial time, during which
  in-flight data refilled the ring. The tell was a **constant** 17280-byte
  gap across twelve samples: races vary, instruments do not. See
  `tcp-tiny-window-lead` memory.
* **Verify a gate against a KNOWN-GOOD run, not only a failing one.**
  `logs/cwwebgl.sh` v1 reported INCONCLUSIVE on a run that had passed, because
  it counted `net::ERR_ABORTED` as failure — that is chrome cancelling its own
  speculative loads and it appears on healthy runs.
* **"Never fires in my test" is not "never fires."** An RFC 1122 zero-window
  rule shipped on that reasoning and broke the browser on real hardware.
  Reverted in `d107f81`; read that commit before adding TCP behaviour.
* **`EXTRA_CFLAGS` does not reach user programs** — use `PROG_EXTRA_CFLAGS`,
  and gate on the marker being in the BINARY.
* **`PROG_EXTRA_CFLAGS` expands UNQUOTED in make's recipe shell.** A `;` in a
  value terminates the clang command mid-flag and surfaces as `clang: error:
  no input files`, which reads like a broken makefile.
* **make does not rebuild on a `-D` change.** `rm -f programs/chromewin/
  chromewin.o` (and `src/*.o` for kernel flags) or the A/B silently inverts.
* **QEMU under Chromium load runs ~2.4× slower than wall clock.** A slow
  capture is not a hang.

---

## 6. Ground truth to compare against

| config | cadence | note |
| --- | --- | --- |
| default (single-process, CDP JPEG) | `cap ~116–140 ms` (~8 fps) | what ships |
| same, real HW, static page | `cap ~366 ms` | user's report |
| viz path, slice 110 | **~52.7 commits/s** | the prize |
| WebGL on (`-DCW_SWGL`) | `cap 136–150 ms` | ~10–15% cost |

Total frames is the cleaner throughput metric (`grep -o "frame [0-9]*: "`);
`cap` is the latency metric. Report both.

**Finally: be honest with the user about the ceiling.** Even a working viz
path is a capture-and-blit architecture. Native Chrome draws straight to the
display with GPU compositing and never pays a per-frame copy. ~53 fps would
be a genuine transformation of how it feels, and it is worth doing — but
"identical to Edge" is not what is on the table, and saying so plainly is
better than implying otherwise.
