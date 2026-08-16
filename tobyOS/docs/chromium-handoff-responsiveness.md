# Handoff: make Chromium on tobyOS feel responsive

Read this file first, then `docs/chromium-handoff-post-slice-108.md` §5.
Memory topics: `chromium-bringup.md`, `browser-webgl-swiftshader.md`.
Baseline commit for slice 129: `eb82bb2` on `feat/chromium-browser`.

**The user's ask, verbatim: "make it much more responsive like real chrome or
edge is."** They browse on a real HP EliteDesk 800 G1 (Haswell i5-4590, 4
cores, 8 GB), not in QEMU, and they report both slow page loads and a
generally sluggish browser.

---

## 1. WHAT SLICE 129 SETTLED — read this before re-planning anything

Three things, two of them retractions of what the previous version of this
file told you to go do.

**A. The "~6.5× sitting on the table" was not real.** This file's §3 said
slice 110 measured ~52.7 fps through the viz shared-bitmap path while we ship
~8 fps of CDP JPEG, and told you to go make multi-process the default. Those
two numbers were measured **on different pages**. `anim.html` is a 60 fps rAF
animation benchmark; the ~8 fps figure came from a Bing SERP. Measured on the
SAME page, both arms rebuilt from the same tree on the same day:

| arm | page | steady fps |
| --- | --- | --- |
| `cpu` (single-process CDP, what ships) | Bing SERP | **7.32** |
| `viz` (multi-process + viz shm) | Bing SERP | **6.16** |
| `cpu` | anim.html | 46.31 |
| `viz` | anim.html | **61.27** |

**On a real page the viz path LOSES by 16%.** The mechanism is not subtle and
`main.c` already documented it at slice 114: viz frames arrive when chrome
*commits* a new frame, and a real page that has finished loading commits
~6×/s, not 60. anim.html commits 61×/s because it is written to. So viz was
never a 6.5× on browsing — it was a faithful measurement of an animation
benchmark. Multi-process additionally costs: the same run's `cap` rose from
123 ms to 166 ms.

**DO NOT ship multi-process/viz as the default. DO NOT re-plan around the
6.5×.** viz remains the right path for animation-heavy content and the arm
still works; it is simply not what browsing looks like.

**B. Multi-process chrome is NOT broken.** The previous file called its own
`-DCW_MP -DCW_VIZ` run "inconclusive" because the archived log "ends at 3733
ms while the gate says the guest ran 250 s". The log was complete and correct.
The guest really did run 250 s and really did print nothing after 3.7 s —
because `gpuperf.sh` force-rebuilt `chromewin.o` and `mmap.o` but **not
`src/kernel.o`**, which is where `TKAPP_CHROMEWIN` lives. The ISO booted a
kernel that had never been told to launch a browser. Rebuilt properly, the
same arm runs 61.27 fps and spawns a real `--type=renderer` and
`--type=gpu-process`. See §4 for the gates that now make this impossible.

**C. The one real win was an instrument, not an architecture.** See §2.

---

## 2. WHAT SHIPPED, AND THE NUMBER

`shm_census_dump()` ran every 3 s from `sched_tick`, **unconditionally, in the
default build**, hashing every shared region the kernel owned and printing one
line per region. Measured on a 260 s Bing run: 86 rounds × ~641 regions =
55,164 lines = 3.9 MB, which is **84% of all serial output**.

That put the guest at 18.1 KB/s of log against a 38400-baud UART that carries
3.84 KB/s — **4.7× oversubscribed on the user's real hardware**, where every
byte past the ring costs an `inb` + spinlock with interrupts disabled, and in
QEMU costs a VM exit. Gated behind `-DSHM_CENSUS`:

| arm | steady fps | serial | `cap` med | 1st frame |
| --- | --- | --- | --- | --- |
| A baseline (census ON) | 7.32 | 18149 B/s | 129 ms | 52.4 s |
| B + 64 KiB pipe (census ON) | 7.02 | 17789 B/s | 123 ms | 46.6 s |
| C + census OFF | **9.79** | 3152 B/s | 100 ms | 35.1 s |
| C′ rerun | **10.00** | 2982 B/s | 100 ms | 30.5 s |

**+38% frame cadence, −22% capture latency, and first paint ~18 s earlier**,
all on the same page with the same build otherwise. Serial output fell 5.8×,
to **below** what the real UART carries — so on the EliteDesk the log no
longer oversubscribes the wire at all, which is the part that should matter
most there and which QEMU cannot show you.

Why it was safe to delete: the census existed to answer "does a
framebuffer-sized shared region exist inside chrome and change per frame?"
Slice 110 answered yes; slice 129 (§1A) then measured that path losing on a
real page. The question is settled in both directions.

**The 64 KiB pipe (arm B) measured NEUTRAL — do not re-chase it as a win.**
It is still in the tree, and here is exactly what it does and does not do.
`PIPE_BUF_SZ` was 4096 while this pipe carries chrome's CDP frames: a Bing
frame is a 22–40 KiB JPEG → 29–54 KiB of base64 in ONE `write()`. Direct
sighting: `[devpipe] pid=33 write(fd=4, 27888)`. With a 4 KiB ring that one
write blocked and handed off ~7–14 times internally. 64 KiB clears the
largest measured frame in a single handoff, and the copy loops became memcpy
runs instead of a byte-at-a-time loop with a `%` per byte. **All of that is
real and none of it moved the number** (7.02 vs 7.32, inside run-to-run
spread) — which localises the remaining ~100 ms/frame to chrome's own
capture+encode, not to our transport. Committed separately so the negative
result stays legible in history.

---

## 3. THE HONEST CEILING, AND WHERE TO LOOK NEXT

Say this plainly to the user: **this is a capture-and-blit architecture and it
will not become Edge.** Native Chrome draws to the display with GPU
compositing and never pays a per-frame copy. What we have is chrome rendering
offscreen, encoding a JPEG, shipping it through a pipe, and us decoding and
blitting it.

What the numbers say about where the time actually goes, on a real page,
post-fix (`cap` med 100 ms ≈ 10 fps):

* **Our side is ~1 ms of it.** `b64=0ms dec=1ms paint=0ms` per frame, every
  run. We ack before decode already (slice 94). There is no meaningful win
  left on the consumer side.
* **`cap` ≈ `turn`**, i.e. essentially the whole cycle is chrome producing the
  frame after our ack. That is chrome's capture + JPEG encode + pipe write.
* On a **static** page chrome re-encodes and re-ships a **byte-identical**
  JPEG forever (`jpeg=22044 bytes` on every frame from 630 to 780). Those
  frames carry no information. A change-detection skip would save the decode
  and blit — but that is the 1 ms, not the 100 ms.

So the ranked list from here:

1. **Measure input latency, not frame cadence.** On a static page fps is close
   to meaningless — nothing is moving. `-DCW_LAT` (slice 114) already
   instruments inject→pixel and navigate→pixel and it works on the CDP path
   (`lat_note_frame` is called from `install_b64_frame`). `gpuperf.sh`'s `lat`
   arm forces `-DCW_MP -DCW_VIZ`; add a single-process `latcpu` arm and get
   the number the user's word "sluggish" actually refers to. **This is the
   next job.**
2. **Page load.** Chrome reaches its CDP session ~11 s in and first paint
   ~30 s in. `execve` of the 192 MB chrome image is 388 ms (`[exectime]
   total=388ms load=364ms`), better than the 730 ms slice 111 recorded, so the
   remaining bootstrap time is chrome's own. Phase-time it before assuming.
3. **Audit for the next census.** The method that found it: dump the log,
   bucket every line by its `[tag]`, and rank by BYTES. `[shm]` (2000-line
   cap) and `[chan]` are bounded one-offs; the census was unbounded and
   recurring, which is what made it the outlier. Nothing else currently
   exceeds a few percent.

### Closed — do not reopen

* **Tier 3 (host GPU) for chrome.** Measured no. `browser-webgl-swiftshader`
  explains why SwiftShader is not a reopening of it.
* **viz/multi-process as the browsing default.** §1A. Measured, on a real
  page, twice the wrong way round before it was measured right.
* **viz poll cadence** (4 ms worse than 15 ms, slice 107) and
  **event-instead-of-poll viz delivery** (≤8%, slice 110).
* **The 64 KiB pipe as a perf lever.** §2. Neutral, measured.

---

## 4. THE HARNESS — what was broken and what now catches it

`gpuperf.sh` gained three gates this slice, all because of §1B.

* **Kernel-flag stamp.** `build/.gpuperf_kcflags` records `EXTRA_CFLAGS`; when
  it changes, `rm -f src/*.o`. `make` does not rebuild on a `-D` change, and
  the script previously force-rebuilt only `chromewin.o` and `mmap.o`. The
  stamp is written only after a *successful* build.
* **GATE 0 — the flags are in the BINARIES, checked before the run.** The
  stamp cannot see `make`, `defboot.sh` or a hand build leaving objects with
  other defines, which is how §1B happened. So ask the binaries: `tobyos.bin`
  must contain `TKAPP] launching` and `/bin/chromewin`; `chromewin.elf` must
  contain `CW_URL`'s value, must NOT contain `--single-process` in `mp`/`viz`
  arms and MUST contain it otherwise, and must contain `cwviz` for viz arms.
  Fails in seconds instead of six minutes.
* **GATE 1b — did the browser even start?** `TKAPP] launching` must appear in
  the run log. The old gate said "full-length run with no `[bkl] cpu`", which
  reads as "the kernel wedged"; it had not.

**The `FRAMES(...)` line is not an fps figure and never was** — it is the last
counter value. The new `STEADY-STATE CADENCE` block is the A/B number. Getting
it right needed three separate corrections, each of which had already produced
a wrong reading in this tree:

* counter/RUNSECS divides a **guest** counter by **wall** seconds, and
  multi-process runs the guest at ~46% of wall clock;
* counting `[cwviz] frame` **markers** undercounts 30×, because chromewin
  prints one line per 30 frames;
* the frame lines **lose their `[N ms]` prefix** once the `[fd1]` chunk logger
  stops wrapping them, so a per-line timestamp regex silently drops the whole
  steady state and reports on the first 25 s.

The block carries the last guest timestamp forward and reads the counter.
**Validated against a known-good run before being trusted**: it reproduces
slice 110's 52.68 fps from `gpuperf_viz_anim.011.log` to the digit.

`gpuperf.sh` also gained a `url` arm (`URL=... bash logs/gpuperf.sh cpu url`,
default a Bing SERP). Every other arm is a local file, and that is the single
biggest reason this harness's numbers did not describe the user's experience.

### The standing traps (all still real)

```bash
tr -d '\000' < logs/X.log | tr '\r' '\n'      # the capture carries NULs
```

* **The serial log splits every `printf` at its format conversions.** Match
  tokens; do not reassemble (each fragment appears twice).
* **Capped loggers hide the evidence they were added to collect.** Every cap
  must announce itself when hit.
* **An instrument in the critical path becomes the bug.** This slice is the
  fourth instance and the largest: 84% of the log. Before that, a `kprintf`
  between deciding to send a TCP window update and sending it. Races vary,
  instruments do not — a *constant* anomaly means an instrument.
* **Verify a gate against a KNOWN-GOOD run, not only a failing one.** Applied
  twice this slice: gate 1b was checked against `.011`/`.012` (passes) as well
  as `.013` (fails), and the cadence metric against slice 110's number.
* **"Never fires in my test" is not "never fires."** See `d107f81`.
* **`EXTRA_CFLAGS` does not reach user programs** — use `PROG_EXTRA_CFLAGS`,
  and gate on the marker being in the BINARY.
* **`PROG_EXTRA_CFLAGS` expands UNQUOTED in make's recipe shell**; a `;`
  surfaces as `clang: error: no input files`.
* **QEMU under Chromium load runs ~2.4× slower than wall clock.** A slow
  capture is not a hang.
* Header dependency tracking (`-MMD -MP` + `ALL_DEPS`) **does** now exist, so
  a `pipe.h` change really does rebuild its includers. The `-D` problem is
  separate and unfixed by it — hence the stamp.

---

## 5. How to run things

```bash
bash logs/gpuperf.sh cpu url          # the A/B that matters: a REAL page
URL=https://example.com/ bash logs/gpuperf.sh cpu url
bash logs/gpuperf.sh viz anim         # the animation benchmark
bash logs/defboot.sh                  # stock-path regression, after ANY kernel change
```

Report **both** total frames and `cap`; they are different questions
(throughput vs latency).
</content>
