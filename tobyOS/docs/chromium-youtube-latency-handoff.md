# Handoff: make YouTube video actually PLAY on tobyOS Chromium (WHPX)

> **STATUS (slice 56, 2026-07-28): the central question of this handoff is
> ANSWERED and FIXED.** The ~55s bidirectional silences were `lx_recvmsg`'s
> UDP arm ignoring O_NONBLOCK: chrome's speculative QUIC/DNS reads blocked in
> an infinite in-kernel drain+hlt loop ON THE BSP, parking pid 0 (unstealable,
> never tick-preempted in kernel mode) and with it poll_tick AND the futex
> timeout sweep -- every timer and poller in the box froze until a stray
> datagram arrived. Fixed along with: yield-if-ready in the blocking-wait
> loops, any-CPU sweep/poll_tick drivers, FUTEX_CLOCK_REALTIME rebasing,
> proactive TCP window updates, non-blocking socket close(2), and audio-off
> chrome flags (no ALSA device; NOTE: initially blamed for the renderer
> death, RETRACTED after run 5 -- see the ledger). The watch page now renders
> its real player with a healthy kernel (when the flaky renderer self-exit,
> wall R1, doesn't fire). Remaining walls: R1 = watch-page renderer sometimes
> cleanly exit(0)s seconds after page handoff; R2 = browser->renderer
> CDP/Mojo flow stops ~35s in ([epset] proves waiters have nothing pending).
> See the slice-56 ledger entry -- read it BEFORE this document; §3/§4/§6
> below are the pre-slice-56 state.

You are picking up a **working, interactive** Chromium bring-up on tobyOS at ONE
remaining wall: **YouTube is ~50x too slow**. Real unmodified
`chrome-headless-shell` 151 runs on tobyOS's Linux ABI under hardware
virtualization (WHPX), renders real web pages in a native TobyTK window
(`programs/chromewin`), and **video playback itself is PROVEN WORKING** — both
progressive and MSE, with visible moving frames.

What is NOT working: on a YouTube watch page the whole pipeline crawls. The page
renders its real player, the media segment request IS issued and answered with
zero errors — but only ~21 HTTP requests complete in 120s, the media request
isn't issued until ~100s, and the video never buffers within a run.

The cause is a **repeating ~55-second BIDIRECTIONAL silence** that is already
measured to the point of naming what it is NOT. Start at §3, not at discovery.

Last commits: slices 49–55, HEAD `f30cc8f`. Full history:
`docs/chromium-hypothesis-ledger.md`.

---

## 0. Prime directives (hard-won; do NOT relearn)

1. **ASK THE APP FOR ITS STATE BEFORE THEORISING ABOUT THE KERNEL.** This arc
   burned two whole slices (a "YouTube throttling" theory and a "Mojo
   response-body data pipe" theory) inferring from ABSENCE — no frames — when a
   single CDP `Runtime.evaluate` probe answered it in one run. chrome knows its
   own readyState, video buffered ranges, and request lifecycle. Ask it first.
2. **NEVER conclude from ONE run.** WHPX is non-deterministic. This arc produced
   THREE separate false conclusions from single runs, including a "12KB CDP
   message kills chrome" bug that did not exist (a size ladder + a control
   injection + a re-run all disproved it). Repeat before believing.
3. **Get the REAL error.** Every wall here fell to reading registers / rings /
   chrome's own logs, never to speculation.
4. **After touching `struct proc` or any widely-embedded header, delete ALL
   kernel `.o` and rebuild** before trusting a single observation.
   `logs/build39.sh` already `touch src/*.c` for this reason.
5. **ASCII text in a pointer register = MEMORY CORRUPTION** (freed-page reuse /
   stale TLB), not a NULL-return.
6. After any kernel change run `logs/defboot.sh` (stock build, must reach login
   with ZERO faults) before trusting a CHROMIUM_BOOT run.
7. **PowerShell/heredocs mangle backslashes.** Write scripts with the `Write`
   tool, not `python - <<'EOF'`. The Bash tool's cwd drifts — use absolute paths.

---

## 1. What already works (do not re-verify, do not break)

| Tier | Status |
|---|---|
| chrome multi-process engine on the Linux ABI (Mojo/ipcz, SCM_RIGHTS, futex/eventfd, threads) | works |
| http + https (post-quantum X25519MLKEM768), SwiftShader pixels, WHPX live frames | works |
| TobyTK window host, CDP screencast + `Input.*` routing | works |
| **example.com renders its real page** | works |
| **YouTube watch page renders its REAL Kevlar player** (controls, progress bar, metadata skeleton), no crash over 360s | works |
| **Progressive video: a local `.webm` plays, moving frames** (slice 49) | works |
| **MSE video: MediaSource→SourceBuffer→appendBuffer→VP9 decode→VISIBLE moving frames** (slice 54) | works |

The MSE proof is worth knowing precisely, because it removes the entire media
stack from suspicion: `logs/gen_mse_test.py` builds a one-line JS file with a
tiny VP9 clip embedded as base64; chromewin injects it into `about:blank`
(`MSE_TEST_JS`) with NO network involved; the probe then reports
`vid=r4 n2 t1.3 d2 b2.0 e- p0 src=blob: mse=updateend buf=2.00` — HAVE_ENOUGH_DATA,
2.0s buffered, no error, `currentTime` advancing — and `logs/vid_c.png` shows the
decoded frame painted at 640x480 with its burned-in timecode.

---

## 2. Read first (in order)

| Resource | Why |
|---|---|
| `docs/chromium-hypothesis-ledger.md` **slices 49–55** | The whole arc with evidence, including every theory that died and why. |
| Memory: `chromium-bringup.md` + the `MEMORY.md` "Chromium bring-up" line | Cross-session summary + traps. `C:\Users\tdude\.claude\projects\c--CustomOS\memory\`. |
| `programs/chromewin/main.c` | The window host + ALL the instruments you inherit (§5). `START_URL` is a compile-time `#define` near the top. |
| `git log --oneline ead9086..HEAD` | Slices 54–55 commit bodies. |

---

## 3. THE WALL — exact measurement

On a YouTube watch page (`logs/run_watch.log`), a repeating ~55s window in which
**nothing happens in either direction**:

```
[rxdbg] 20282ms drains=5801   pkts=151 maxbatch=34 quiet=2902ms
[rxdbg] 45286ms drains=309182 pkts=151 maxbatch=34 quiet=27906ms
[rxdbg] 70290ms drains=655474 pkts=151 maxbatch=34 quiet=52910ms
```

Decode:
- **`pkts` FROZEN at 151 for 53 seconds** — not one packet arrives.
- **`drains` climbs 5,801 → 655,474 = ~14,000 NIC drains/second.** We are
  polling the hardware furiously. RX servicing is NOT the problem.
- **We transmit NOTHING either**: `[tls] TX` count over the 20–75s window is
  **0**. The silence is **BIDIRECTIONAL** — chrome simply isn't writing.
- During it: **47 threads BLOCKED** (mostly futex), **exactly ONE RUNNING**
  (pid 39, a network-service thread making only 119 syscalls in 60s),
  **NOTHING READY**, and only **1–4 ring-3 profiler samples per interval** —
  i.e. **the machine is IDLE**.

Meanwhile chrome's own Network domain says nothing is failing:
```
[net] MEDIA REQ #2: https://rr5---sn-vgqsknde.googlevideo.com/videoplayback?expire=...
probe #10 at 100s: net{req=21 media=1 resp=1 fin=15 fail=0}
```
The segment request IS issued, a response IS received, **zero failures**. It is
purely a pace problem.

**So: chrome is waiting for something internally, for ~55s at a time, while the
box is idle. Find what.**

---

## 4. Ruled OUT — do NOT re-litigate

Each of these was tested and killed with evidence this arc:

- **Zero-window / RX-buffer-full stall.** The `[tcp] RX buffer FULL` diagnostic
  does NOT fire during the stall. (It DOES fire later in a run on tcp[8] with
  `free=0` — that is a separate, real flow-control issue; see §6.)
- **RTO / retransmit storm.** `retx=0`, no dup-acks, no out-of-order during the
  stall (`[tcp] WIRE ... retx=0`).
- **Userspace spin convoy.** The profiler shows the CPUs are NOT in ring 3
  during the gap (1–4 samples/interval). Earlier `sched_yield`-heavy samples
  come from OTHER phases, not the stall.
- **"~50 ready threads serialized on the BSP."** FALSIFIED: the ready queues are
  EMPTY with one runnable thread. `enq_target_for()` really does `return 0`
  unconditionally, and `sched_yield`'s fast path really does return before the
  work-stealing code — both true, but **not this bug**. A rate-limited steal
  probe built on that model changed nothing measurable and was REVERTED.
- **MSE broken / decode broken / display path.** All proven working (§1).
- **Large IPC messages breaking Mojo.** A size ladder replied `ok` on every rung
  from 1KB to 48KB, including the exact size that once "killed" a session; a
  control injection of the same payload without MediaSource was fine; re-running
  the original case was fine. It was a WHPX flake.
- **YouTube throttling / corrupted chrome profile / disk state.** All killed
  (example.com stalled identically; `/data` is RAM-backed each boot so nothing
  persists).
- **e1000 RX ring overrun.** Was REAL and is FIXED (slice 55, ring 32→256 after
  measuring 302 late drains with `batch=32` = full ring = NIC dropping frames).
  It was not the stall.

---

## 5. Instruments you inherit (all in place — use them)

- **`[rxdbg]`** (src/e1000.c, CHROMIUM_BOOT): drains, packets, biggest batch per
  drain, and ms since last packet, reported every 5s + on any batch ≥ 8. This is
  what localised the silence.
- **CDP page/video probe** (chromewin `probe_page`, every 10s): reports
  `document.readyState`, title, body length, and the full `<video>` state —
  `r`(readyState) `n`(networkState) `t`(currentTime) `d`(duration)
  `b`(buffered end) `e`(error) `p`(paused) `src`, plus `window.__mse`.
- **CDP Network domain** (chromewin `note_network_event`): media URLs logged
  individually, plus counters `net{req= media= resp= fin= fail=}` on every probe
  line. Chrome telling you its own request lifecycle.
- **`[prof]`** ring-3 sampler: hottest user RIPs per interval, per pid. Its
  SAMPLE COUNT is itself the signal (low count = box not in userspace).
- **`[hb]`** per-proc scheduler heartbeat (state/prio/io_boost/onq/oncpu) and
  **`[wait]`** dump (every blocked thread, what it waits on, for how long).
- **`[lx-recent]`** 384-deep tid-tagged syscall ring, dumped on fatal faults.
- **MSE regression test**: `logs/gen_mse_test.py` + `MSE_TEST_JS` in chromewin.
- isr.c fatal path: full registers, `[ustk]`, VMA dumps, `[pgj]` page journal,
  `[pfres]`/`[pfrej]` fault rings, pointer-source-page autopsy.

---

## 6. Attack plan

**A. Measure wake latency directly — the one instrument that is missing.**
For the longest futex/epoll waiter at the moment of the stall, log its
**requested timeout vs actual elapsed time before it ran again**. That single
number splits the two live hypotheses:

1. **A deadline computed wrong.** Timed futex waits use `FUTEX_WAIT_BITSET` with
   ABSOLUTE deadlines compared against `perf_now_ns()` (src/thread.c). If a
   deadline is mis-scaled (or the clock the deadline is built from disagrees with
   the clock the wait compares against), a "50ms" wait becomes a ~55s wait and
   every timer-driven chrome retry stretches out — which would look EXACTLY like
   this. Check the timed-wait path end to end: what chrome passes, what we store,
   what we compare.
2. **A wakeup delivered late.** `poll_tick()` (src/thread.c) re-scans blocked
   pollers at most once per ~1ms, and when EVERY user thread is blocked its ONLY
   driver is **pid 0's loop** (kernel.c ~706). If pid 0's lane is delayed or its
   `g_poll_waiters` gate is wrong, every parked poller waits. Note the box is
   idle during the stall, so something SHOULD be running pid 0 — verify it
   actually is, and how often `poll_tick` really fires (add a counter).

**B. Resolve one contradiction in the evidence.** The heartbeat says exactly one
thread is RUNNING (pid 39) during the stall, but the ring-3 profiler says the
CPUs are almost never in userspace. So either that thread is sitting in the
KERNEL (where? a poll loop? the BKL?) or its RUNNING state is stale. Answering
this may hand you the bug directly — sample the KERNEL rip (not just ring-3) for
whatever is "running".

**C. Separately: fix the real receiver-side flow-control stall.**
`[tcp] RX buffer FULL tcp[8] plen=1 free=0` fires later in runs. `tcp_recv()`
only sends a window update when `before >= TCP_RX_BUF_BYTES/2`, and the
`r == -1` early-return path calls `rx_pop()` with NO window update at all. Worth
fixing on its own merits (any large download), independent of the stall.

**D. Only after the pace is fixed**, re-check YouTube video: the segment request
already succeeds, so the expectation is that with normal latency the player
buffers and plays. Confirm with the probe (`b>0`, `r>=2`, `t` advancing) and a
screenshot, not by eye alone.

**Definition of done:** a YouTube watch page buffers and plays video — probe
shows `b` (buffered) > 0 with `currentTime` advancing, and screendumps show
moving video frames — with chrome surviving.

---

## 7. Build / run mechanics

```bash
cd /c/CustomOS/tobyOS
# pick the target: START_URL near the top of programs/chromewin/main.c
#   https://www.youtube.com/watch?v=aqz-KE-bpKQ   (the wall)
#   https://www.youtube.com/embed/<id>?autoplay=1&mute=1  (lighter, same MSE)
#   file:///opt/chrome/vid.webm                   (progressive video, PROVEN)
#   https://example.com/                          (green default)
# or #define MSE_TEST_JS to run the local MSE test instead (PROVEN)

bash logs/build39.sh     # kernel change: touches ALL src/*.c (~4 min)
bash logs/build_vid.sh   # chromewin/initrd only, no kernel touch (~2.5 min)

python logs/run_watch.py   # 360s, serial -> logs/run_watch.log, logs/wat_*.png
python logs/run_embed.py   # 300s embed player
python logs/run_vid.py     # 190s, for local/MSE tests
bash   logs/defboot.sh     # stock build must reach login, ZERO faults
```
Python: `/c/Users/tdude/AppData/Local/Programs/Python/Python311/python`.
QEMU: `C:\Program Files\qemu\qemu-system-x86_64.exe`. Kill stuck guests with
`taskkill /F /IM qemu-system-x86_64.exe`; confirm `tobyOS.iso` mtime is fresh
before trusting a run.

Useful one-liners on a finished run:
```bash
L=logs/run_watch.log
grep -ao "tobyprobe[^\"]*" $L | tail -5                      # page + video state
grep -ao "probe #[0-9]* at [0-9]*s: frames=[0-9]* net{[^}]*}" $L | tail   # pace
grep -ao "\[rxdbg\][^m]*maxbatch=[0-9]* quiet=[0-9]*ms" $L | head -20     # silence
grep -acE "EXCEPTION 1[234]|terminating user process" $L                  # crashes
```

---

## 8. One-paragraph status to hold in your head

Everything works except pace. chrome renders real pages and YouTube's real
player; video decode, MSE and the display path are all proven with visible
moving frames. On a watch page the system instead enters repeating ~55-second
windows where it sends nothing, receives nothing, has 47 threads blocked, one
"running", nothing ready, and the CPUs idle — while chrome's own network
telemetry reports zero failures. It is not the NIC (we poll 14k/s and nothing
arrives), not TCP retransmission, not flow control, not spinning, and not queue
starvation — all measured and excluded. It is chrome waiting on a wakeup or a
timer that tobyOS delivers ~55s late. Measure requested-vs-actual wait time for
the longest blocked waiter, and the bug should name itself.
