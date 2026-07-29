# Handoff: full YouTube UI parity on tobyOS (comments, sidebar tiles, thumbnails)

**REWRITTEN after slices 61–61d (2026-07-29).** The previous version of this
document sent you toward headed chrome + Ozone X11 (Route B). That premise is
now **measured false** — do not build it. Read §1 before anything else.

Prior state still true: video PLAYS on a real watch page (best run this arc:
r4, **41 s buffered** — the volume fix nearly doubled the old 20–24 s best),
metadata/title/views populate, zero crashes on good runs. Per-slice evidence:
`docs/chromium-hypothesis-ledger.md` slices 56–61c (slice 61 entries carry
the full control matrix + the /data root cause).

---

## 1. What slices 60–61 proved (control matrix — do NOT re-derive)

Host Chrome, same watch page (aqz-KE-bpKQ), NO --virtual-time-budget, real
wall clock, census over CDP `Runtime.evaluate`, real `Input.dispatchMouseEvent`
mouseWheel scrolling (the tool: scratchpad `ctl_census.mjs`, node ≥22):

| mode                          | vis     | threads | lockup tiles | verdict |
|-------------------------------|---------|---------|--------------|---------|
| headless=old + wheel scroll   | visible | **20**  | 20           | FULL population |
| headless=new + wheel scroll   | visible | **20**  | 20           | FULL population |
| headless=old, JS poke, no wheel| visible| 0       | 20           | JS poke DEAD |
| headless=old, tobyOS-choreography | visible | **20** | 20        | no freeze on host |
| HEADED, window hidden         | hidden  | 0       | 3            | WORSE than headless |

1. **Slice 60 was wrong twice**: `--virtual-time-budget` starved the
   scroll-driven phase, and it counted `ytd-compact-video-renderer` — an
   OBSOLETE element. Modern YouTube sidebar tiles are `yt-lockup-view-model`
   (host: 20 of them while the old counter reads 0). Comment threads render
   with `ytd-comments` offsetHeight = 0 even on a fully-working host — never
   use cmtH/cmTop as a health signal.
2. **Route B is refuted.** headless=old — the exact engine tobyOS runs —
   reaches full population. A headed window that is hidden/occluded is
   throttled BELOW headless (Input acks defer forever). Headed chrome without
   a real visible display would be a REGRESSION.
3. **Only real wheel input triggers YouTube's lazy machinery.** scrollIntoView
   + synthetic scroll events + forced reflow do NOTHING even on the host.
4. tobyOS ALREADY HAS the sidebar: `lk=20` every good run. That half of the
   old "parity gap" was a measurement artifact.

## 2. The tobyOS-side root cause that WAS real (fixed in 61c, commit 3b099e6)

/data was a **4 MiB, 256-inode** tobyfs (host mkfs stamps fixed geometry into
the 16 MiB disk.img). Chrome fills it ~33 s in (profile + HTTP cache + POSIX
shm files — `--disable-dev-shm-usage` sends shm to TMPDIR=/data). Every
vfs_create then failed VFS_ERR_NOSPC, which the lx open path masked as
**EACCES** — chrome logged 64,858 × "Creating shared memory ... Permission
denied (13)", and when the compositor's shm pool drained the ENTIRE frame
pipeline froze deterministically (raf stops at exactly 705, twice). Fixes:

- `logs/mkdata_gpt.py` → disk.img is now a GPT image with one BLANK
  tobyOS-data partition; the kernel auto-provisions it device-sized at boot
  (verified: 1022 MiB, 4096 inodes). Old image kept as disk.img.16m.
- lx open maps NOSPC/NAMETOOLONG honestly now (syscall.c). Errno fidelity is
  diagnostic infrastructure — the masked errno cost most of a day.
- chromewin passes `--disk-cache-size=64MB`; run_watch.py uses
  `-drive ... snapshot=on` (pristine volume every run, no profile carryover).
- `logs/defboot.sh` gate was VACUOUS since the chromium payload arrived
  (-m 512 OOM-panicked before any marker; fault grep missed plain "PANIC:").
  Both fixed (-m 4096). Post-fix: 0 shm failures, video buffered 41 s.

## 3. The ONE remaining gap, and the live theory

On a good run tobyOS now reaches: page built (ytd≈3600), sidebar populated
(lk=20, real titles, secH≈1650), at true page bottom (sy+600 == sh), vis=1,
foc=1, heartbeat on, IntersectionObserver delivery PROVEN (io=fired1), rAF
alive — and still `th=0`: the post-scroll build (host: +4400 elements, the
comments module) never runs, and no continuation POST is ever issued.

**Theory (measured up to its edge): idle starvation.** SwiftShader raster of
a PLAYING video layer backpressures the compositor to ~0.25 main-frames/s
(that this is the real throughput was hidden before 61c: failing shm
allocations made raster "complete" instantly, so rAF free-ran at 25/s).
YouTube defers the comments build at idle priority; with the pipeline
perpetually backlogged the renderer may never report an idle period.

**Slices 61d/61e VALIDATED the theory and CLOSED most of the gap
(2026-07-29):** with the video PAUSED at dwell (+ re-paused each cruise
pass), `ric` unfreezes within seconds and the entire deferred build executes
(~50 s): ytd 54→7198, sh 837→5710, **th 0→20 with 20 non-empty
`#content-text` comment bodies** — host-parity field-for-field. Final batch
(warm profile + 900px/6s cruise): **2/3 runs at FULL parity (th=20,
lk=20–26, cti=15, sy≈5100), 3/3 healthy video (b39.7/41.2/59.1 — records),
zero crashes, zero 403s.** The 1/3 miss: the comments trigger occasionally
never fires despite bottom+pause (page stays ungrown, sy≈2300) — YouTube
A/B / build-order variance, not a tobyOS defect class. Depth matters: the
trigger must enter the viewport on the GROWN page (sy≈3900+).

## 4. Current probe + choreography (chromewin/main.c — extend, don't rebuild)

Probe fields (all in one `Runtime.evaluate`, printed as `[chromewin] CDP:`,
print cap 800): rs/title/blen/vid/imgs/tiles/cmt/meta/sy/sec/cmt2/gate/
secIt/secTc/secH/cmtTc/cmtH/ytd + slice-61 `sh vis foc th lk cti cmTop raf
io hb ric`. Success metric = **th** (ytd-comment-thread-renderer) and **lk**
(yt-lockup-view-model). Replies are PARSED (g_p_sy/ytd/sh/th) and drive the
scroll state machine: WAIT-BUILD (ytd≥1500) → DOWN (600px wheel/4s until
sy+700≥sh) → DWELL 60 s (video paused) → CRUISE (600px/8s, one §3A poke at
180 s — kept although proven useless, harmless) → TOP at 280 s.
`Emulation.setFocusEmulationEnabled` at bootstrap. The presentation
heartbeat (2×2 px div, style toggle each 100 ms, installed idempotently from
the probe) forces real BeginMainFrames — required; without it the lifecycle
only runs during app-build damage.

**After ANY probe edit run `python logs/check_probe.py`** — it extracts the
JS from the C literals and `node --check`s it. A missing paren once compiled
the whole probe to a SyntaxError and produced a diagnostic-blind run.

## 5. Build / run mechanics (updated)

```bash
cd /c/CustomOS/tobyOS
python logs/check_probe.py             # after any probe edit (node required)
bash logs/build_vid.sh                 # chromewin/initrd only, ~2.5 min
bash logs/build39.sh                   # full kernel touch (kernel change)
bash logs/defboot.sh                   # NOW A REAL GATE: -m 4096, PANIC-aware
bash logs/run_x3.sh                    # build once, run 3x, tabulate th/lk/cti/sy
python logs/mkdata_gpt.py disk.img 1024  # regenerate the /data image if needed
```
Read a finished run: the tabulation line per run, plus
`grep -ao "tobyprobe rs=[^\"]*" logs/x3_runN.log | tail -1` (800 chars) and
`paste` of `probe #N` lines with `raf=`/`ric=` greps for lifecycle health.
Screenshots: wat_c/d (play window), wat_e (240 s: comments region while
dwelling), wat_f (330 s: player after return-to-top).

## 6. Definition of done (unchanged) + honest status

On a mainstream watch page, across a run_x3 batch: `th > 0` with non-empty
thread text, sidebar `lk` populated (ALREADY TRUE), thumbnails decoding
(imgs grew to 11–16/65 at depth; host reference 37/92) — matching the
headless=old host control, which IS the correct reference (headed adds
nothing without a real display).

Status: **ACHIEVED at 2/3-of-batch level (slice 61e); polish landed (61f).**
Sidebar done; volume + idle-starvation causes fixed; comments render with
real text whenever YouTube serves a full page and the run reaches depth.
Slice 61f landed all four polish items: (a) stuck-bottom JIGGLE (alternate
-700/+900 wheels once pinned with th=0); (b) PARK — on th>0 the run stops
touring, aims the first thread via the `thTop` probe field (HALVING
controller, dy=(thTop-120)/2 per probe; measured wheel gain ~1.8x overshot
a full-delta aim) and holds with the video paused; validated live (th=20
held through PARK; first-ever deep-page visual: related tiles with full
metadata on screen); (c) the [pfrej] NO-VMA startup flake's mechanism FOUND
AND FIXED — sys_munmap's middle-split dropped VMA coverage silently when
vma_alloc failed (now kept-spanned + loud), plus an mprotect stale-`mid`
rollback; (d) raster throughput unchanged — still the ceiling, and the
reason comment PIXELS haven't landed on a QMP screenshot yet (the screencast
lags the DOM by minutes under load; parked frames show earlier regions).
Harness hardening after a stale-flavor incident: build_vid fails loudly,
run_x3 aborts on build failure, defboot warns + re-touches kernel.c (it
rewrites tobyOS.iso STOCK — always rebuild flavored before a batch).
Evening note: after ~15 runs/day YouTube degrades service to this IP (duds,
403s, halved buffers, slow builds) — measure th-rates in the morning or on
alternate watch URLs before concluding anything from a bad batch.
