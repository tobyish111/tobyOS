# Handoff: make YouTube VIDEO play on tobyOS Chromium (WHPX)

You are picking up a **working, interactive** Chromium bring-up on tobyOS at ONE
remaining wall: **video playback**. Real unmodified `chrome-headless-shell`
151.0.7922.34 runs on tobyOS's Linux ABI under hardware virtualization (WHPX),
renders live web pages in a native TobyTK window (`programs/chromewin`), streams
frames over CDP `Page.startScreencast`, and accepts mouse + keyboard input routed
back through DevTools `Input.*`. HTTPS (post-quantum X25519MLKEM768), SwiftShader
pixels, and fontconfig text all work. **YouTube's homepage renders its real UI**
(search bar, Sign in, feed). The last commits are slices 41–48 in
`docs/chromium-hypothesis-ledger.md` (HEAD `5d75a7c`).

Your job: **get an actual YouTube video to play** (or at least to paint its
player). The blocker is precisely captured below — start at the fault, not at
discovery.

---

## 0. Prime directives (hard-won this arc; do not relearn)

1. **Get the REAL error before theorizing.** Every wall this project fell to
   reading the fault registers / the mprotect+VMA rings / chrome's NetLog — not
   speculation. Two prior sessions were lost to plausible-but-wrong theories.
   For THIS wall the fault is already symbolized to the register level (§3).
2. **Symbolize, then reason about what returned NULL.** This is a NULL-pointer
   deref inside chrome. chrome does not NULL-deref on real Linux, so **something
   tobyOS did returned NULL/0 where chrome expected a valid object.** The value
   in `rip` names the chrome code; the ROOT is upstream. Find the source of the
   NULL, don't just look at the crash site.
3. **After changing `struct proc` / `struct file` / any widely-embedded header,
   delete ALL kernel `.o` and rebuild** before trusting a single observation.
   `logs/build39.sh` already `touch src/*.c` for this reason. Stale objects
   produce convincing, stable, FALSE symptoms (cost a wrong documented
   conclusion twice).
4. **PowerShell mangles quotes/backslashes.** Run everything through
   `C:\msys64\usr\bin\bash.exe -lc '...'` or a script. Python heredocs that
   write C strings will turn `\n` into a real newline — write such edits with the
   `Edit`/`Write` tools, not `python - <<'EOF'` string-replace.
5. Python: `/c/Users/tdude/AppData/Local/Programs/Python/Python311/python`.
   QEMU: `C:\Program Files\qemu\qemu-system-x86_64.exe` (has `tcg` and `whpx`).
   Kill stuck guests: `taskkill /F /IM qemu-system-x86_64.exe`. The build's
   `rm tobyOS.iso` sometimes says "Device or resource busy" (AV/stale qemu) —
   `taskkill` first and re-check the ISO's mtime is fresh before every run.
6. **The watch page is NON-DETERMINISTIC.** Under WHPX only ~half of runs paint
   at all within a few minutes; the rest sit on "connecting to chrome". Do not
   conclude "regression" from a single blank run — repeat, or watch the serial
   markers, not just the screenshot.

---

## 1. Read first (in order)

| Resource | Why |
|---|---|
| `docs/chromium-hypothesis-ledger.md` **slices 41–48** (the tail) | The full WHPX + Phase-3 arc with evidence. Slices 46/47 are the two watch-page crashes: slice 47 (VMA race) is FIXED; **this handoff is the crash BEHIND it.** |
| Memory: `chromium-bringup.md` + the `MEMORY.md` "Chromium bring-up" line | Cross-session summary + traps. Path `C:\Users\tdude\.claude\projects\c--CustomOS\memory\`. |
| `programs/chromewin/main.c` | The window host. `START_URL` is a compile-time `#define` near the top; the CDP/screencast/input plumbing is all here. |
| `src/mmap.c` `mmap_handle_page_fault` / `mmap_try_fault` | The slice-47 VMA-race fix (retry-under-BKL). Context for how faults resolve. |
| `git log --oneline f6a9dfa..HEAD` | slices 44–48. Read the commit bodies. |

---

## 2. Build / run mechanics

```bash
cd /c/CustomOS/tobyOS
# 1. point chromewin at a video (flip START_URL near the top of main.c):
#    #define START_URL "https://www.youtube.com/watch?v=aqz-KE-bpKQ"
bash logs/build39.sh            # TKAPP_CHROMEWIN + CHROMIUM_BOOT iso (~2.5 min)
python logs/run41yt.py          # WHPX, 360s, serial -> logs/run41yt.log,
                                # screendumps logs/ytwx_{a,b,c}.png
```
`run41yt.py` is `-accel whpx,kernel-irqchip=off -smp 4 -m 6144 -cpu qemu64,+smep,+smap`.
Serial markers: `[chromewin] chrome pid` → `[devpipe]` → `[chromewin] sessionId`
→ `bootstrap OK; screencast started` → `[chromewin] frame N: WxH jpeg=B bytes`.
The crash lands at guest **~277 s**. After ANY kernel change, also boot WITHOUT
`-DCHROMIUM_BOOT` to confirm the base OS is clean: `logs/defboot.sh`-style
(clean-config `make iso`, boot `-m 512`, must reach login/desktop, zero faults).

Reproduction is flaky — if a run paints 0 frames and shows no fault, re-run.
Consider `-m 8192`, or a lighter target (`m.youtube.com`, or
`youtube.com/embed/<id>`) to de-risk the render while keeping the media path.

---

## 3. THE WALL — exact fault (measured, `logs/run41yt.log` ~277 s)

```
*** EXCEPTION 14: Page Fault  (in user mode) ***
  rip=0x000000000208c13a  cs=0x23  rflags=0x10206
  rsp=0x0000103801bfed40  ss=0x1b  err=0x4
  rax=0x000010240d3a3800  rbx=0x0  rcx=0x378  rdx=0x0
  rsi=0x000000000c130a00  rdi=0x0000000000000000
  r12=0x00001024080e3c00  r13=0x0000104903253e3c  r15=0x000010240d3a3800
  cr2=0x0000000000000006  cr3=0x0000000053dfd000
[isr] TLS: ... pid=52 tgid=49 is_thread=1 name=chrome-headless-shell+T
[pf] addr=0x6 NOT covered by any mmap-VMA (3624 total)
[isr] ... terminating user process pid=52
```

Decode:
- **`err=0x4`** = user-mode **READ** of a **not-present** page. **`cr2=0x6`,
  `rdi=0x0`** → a `mov 0x6(%rdi),…` with `rdi == NULL`: reading field **+6 off a
  NULL pointer**. A classic NULL-object dereference.
- **`rip=0x208c13a`** is in chrome's MAIN binary `.text` (PIE load base
  `0x500000`, `.text` runtime `0x1fc7000..0xba33e70`). Objdump offset =
  `rip - 0x500000 = 0x1b8c13a`.
- **Faulting thread: `pid=52 tgid=49 is_thread=1`** — a WORKER thread of chrome
  CHILD process `tgid 49` (chrome spawned `--type=renderer` ×2 and
  `--type=utility` ×1 this run; 49 is one of those, NOT the browser main which is
  pid 3/tgid 3). The thread died `exit=-1`; the fault is **isolated (1 total, not
  a loop)**. `cr3=0x53dfd000` is that child's address space.
- No OOM (7 GiB, `[pmm] free` healthy). This is NOT the slice-47 VMA race — that
  is fixed and its `WRITE to non-writable VMA` refusal count is now 0.

So a renderer/utility worker read a NULL object pointer + 6 and was SIGSEGV'd.
Whatever set that pointer to NULL is the bug.

---

## 4. What is proven / ruled OUT (do not re-litigate)

- **VMA-table race — FIXED (slice 47, `a06be20`).** The earlier watch-page crash
  (`EXCEPTION 14 err=0x6 WRITE to non-writable VMA`, ~247 s) was the BKL-free
  fault handler reading a VMA mid-split during chrome's W^X `mprotect` storm.
  `mmap_handle_page_fault` now re-verifies a refusal once under the BKL. This
  handoff's crash is 30 s LATER and a different signature (READ of NULL+6). Don't
  re-chase the VMA race.
- **Not OOM** (7 GiB managed, no `demand-page OOM`, no `[pmm] BUG`).
- **Not the homepage / render / input tiers** — youtube.com homepage renders its
  real UI, `startScreencast` pushed 156 frames on a watch page (vs 2 polled), and
  keyboard+mouse route to chrome (`Input.*`, slice 48). All solid.
- **Not sandbox / GPU-process** — `--no-sandbox --no-zygote --in-process-gpu`
  passed; GPU runs on a browser thread.

---

## 5. Attack plan

**A. Symbolize the crash site.** `objdump -d` the chrome binary
(`programs/chromium/chrome-headless-shell-linux64/.../chrome-headless-shell`)
around `--start-address=0x1b8c13a` (= `rip - 0x500000`). The binary is STRIPPED
(only `.dynsym` = imports/exports; the render-gl arcs hit this wall), so you'll
get instructions + maybe a nearby exported symbol, not the internal function
name. Read the faulting instruction and the few before it to see what object
`rdi` came from (which prior load produced the NULL). Use `bt_dump_group(49)` /
`bt_dump_one` (fires from `signal_send` on an `is_renderer` SIGKILL, and can be
called at the fault) + `[ustk]` (the fatal-path user-stack dump already prints ~96
qwords at `rsp`) + `[lopen]`/`[libmap]` to map any `.so` return addresses on the
stack to `libc`/`ld.so`/`libvulkan`/`swiftshader` offsets (only those have
symbols). The stack qwords + the `0x1024…`/`0x1049…` mmap addresses tell you
which subsystem's arena this is.

**B. Find what returned NULL.** This is the crux. A renderer/utility worker got a
NULL where an object was expected. Instrument the SUSPECTED source and watch it
return 0/NULL. Prime suspects, roughly in order:
  1. **The media pipeline.** A watch page is the FIRST tier that heavily
     exercises video/audio decode. chrome uses its bundled VP9/AV1 + Opus
     decoders (no H.264/AAC needed). Suspect a decoder/buffer/`SharedImage`/
     Mojo handle that a tobyOS syscall handed back as 0 — e.g. a shared-memory
     region (`memfd`/`shm` cache), an `eventfd`/`fd` from `SCM_RIGHTS`, a
     `pipe`/socket, or an `ioctl`/`mmap` for a "GPU" buffer — that chrome wraps in
     an object and later derefs. Grep the recent-syscall ring right before the
     fault (already dumped after the register block) for the last calls THIS
     thread (tid) made; the last non-`sched_yield` syscalls name the subsystem.
  2. **A syscall returning an unexpected sentinel.** tobyOS stubs some calls
     (accept/ignore). If a media/GPU/audio syscall returns 0 where chrome treats
     0 as "no object" vs "success", that yields exactly this. Turn on
     `-DLINUX_SYSCALL_TRACE` (firehose) OR add a targeted `[linux] UNHANDLED` /
     return-value trace for the syscalls that fire in tgid 49 in the last ~2 s
     before 277 s.
  3. **The audio path.** Even with `--autoplay-policy=no-user-gesture-required`
     and a null audio sink, chrome initializes an audio service; a NULL there
     could crash a media worker. (No ALSA/HDA in tobyOS yet — see the "sound"
     scope note.)

**C. Make repro reliable BEFORE deep instrumenting.** The watch page is flaky
(§0.6). Options: raise `-m`, try `m.youtube.com/watch?v=…` or a `/embed/…` URL
(lighter shell, same media stack), or a non-YouTube `<video>` test page with a
public VP9/WebM file to isolate "does ANY video decode+paint" from "does YouTube's
app get there". A minimal `<video autoplay>` data/URL is the cleanest first probe:
if THAT paints video frames, the media pipeline works and YouTube's wall is
higher-level; if it NULL-derefs the same way, you've reproduced the core bug with
a 5-second page instead of a 277-second one.

**Definition of done:** a YouTube watch page (or a minimal autoplay `<video>`)
paints moving video frames into the chromewin window under WHPX, with chrome
surviving (no `exit=-1`).

---

## 6. Instruments you inherit (all `CHROMIUM_BOOT`-gated; keep + extend)

- **isr.c fatal path:** full GP-register snapshot, `cr2`, `[ustk]` user-stack dump
  (~96 qwords at rsp), `[usrc]` memcpy-source dump, `mmap_debug_fault_vma` (prints
  EVERY covering VMA + OVERLAP flag), `mprotect_ring_dump`, `[pgj]` page journal,
  `[pfres]` resolved-fault ring, `[pfrej]` fault-REFUSAL reasons (names WHY
  `mmap_handle_page_fault` returned false).
- **`[lx-recent]`** recent-syscall ring (384 deep, tid-tagged) — dumped after any
  fatal user fault; the last non-`sched_yield` calls name the subsystem.
- **`[libmap]` / `[lopen]`** — every `.so`/segment mapping + opened file path per
  fd, so a stack address → module+offset → `objdump`.
- **`bt_dump_group(tgid)` / `bt_dump_one`** (src/syscall.c) — walk a proc's user
  stack for code-region return addresses; fires from `signal_send` on SIGKILL of
  an `is_renderer` proc.
- **`[devpipe]`** (fds 3/4 CDP traffic), **`[tcp] WIRE`/`[tls]`** (TLS wire),
  **`[hb]`/`[hb-x]`** (run queues, BKL tickets, per-CPU mono clock, `pollit`),
  **`[amap]`/`[mprot]`** (mmap/mprotect history), **`[efd]`/`[shm]`/`[chan]`**
  (eventfd/shm/Mojo-channel identity), **`[clkchk]`** (clock monotonicity).
- Symbolizer: `logs/sym38.sh` maps `[ustk]` qwords → module+offset via the run's
  own `[libmap]` table.

---

## 7. Scope note — "sound" is a separate arc

Video-WITHOUT-sound is a legitimate first milestone. Audio is a NEW subsystem
(QEMU Intel HDA/AC97 device + kernel driver + a minimal ALSA PCM surface) — a big
lift, deferred. chrome's null audio sink SHOULD let video render silently; if the
NULL-deref turns out to be in the audio-service init, stubbing/short-circuiting
that path (rather than building audio) may be the unblock.

---

## 8. One-paragraph status to hold in your head

Everything up to video works: WHPX frames, HTTPS, pixels, the TobyTK window,
`startScreencast`, input routing, and the YouTube homepage UI. A YouTube WATCH
page is the heaviest tier; it used to crash the browser on a VMA-table race
(fixed, slice 47), and now — one layer deeper — a renderer/utility WORKER thread
reads a NULL object pointer (`cr2=0x6`, `rdi=0`, at chrome `rip 0x208c13a`) ~277 s
in and dies. Something tobyOS returns NULL under the media pipeline; symbolize the
site, find the NULL's source (start with the last syscalls tgid 49 makes before
the fault, and a minimal `<video autoplay>` repro), and make video paint.
