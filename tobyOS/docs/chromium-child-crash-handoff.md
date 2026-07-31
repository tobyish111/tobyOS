# Handoff: full Chromium on tobyOS — the child-process GP fault (post-slice-86)

**Read this whole file before touching the arc.** It is the prompt for the next
session. Baseline commit: **`83d8b68`** (slice 86). Companion long-form notes:
`docs/chromium-hypothesis-ledger.md` and the memory topic file
`chromium-bringup.md`.

---

## 1. What this project is

tobyOS is a from-scratch OS that runs **real, unmodified Linux x86-64 binaries**
through a "Track B" Linux personality layer. The headline workload is
**genuine Chromium 151** — not a port, the stock Google binary — driven from a
native tobyOS window (`programs/chromewin`) that talks CDP over
`--remote-debugging-pipe`.

That already works end-to-end on the **headless-shell** flavour: full
multi-process engine, https, SwiftShader software GL, VP9 video playing on a
real YouTube watch page, YouTube comments and sidebar at host parity, and live
window resize with real reflow. `react.dev` renders.

The current work is on the **full `chrome` binary** (`CHROME_FULL=1`), which is
a different, larger executable with subsystems headless-shell simply does not
have. Slice 86 got the browser process from "dies at ~7s" to "runs the whole
session". A **child** process now dies at ~74s. That is your target.

---

## 2. Why full chrome matters — the performance tiers

Everything below is why we are bringing up full chrome at all. Do not lose this
thread; the crash you are fixing is a means, not the end.

| Tier | Goal | Status |
|---|---|---|
| **1** | Make the display path cheap | **DONE** (`eb01e08`). Frame-stage timers; in-place vectorized RGBA→ARGB swizzle that adopts stbi's buffer; serial-firehose fix. Measured result: **the display path is ~FREE (decode ≈1 ms)** — *frame production* inside chrome is the bottleneck, not our presentation. |
| **2** | Remove kernel-side serialization | **DONE**. `sched_yield`/`futex` BKL fast paths; `/data` moved off ATA PIO onto **virtio-blk** (BKL held **94% → 1.3%**); event-driven poll wakeups (`epoll_wait` ~**13×** cheaper per call). `react.dev` went **630 → 1050 frames**. |
| **2.5** | **Zero-copy frames** | **SIZED at ~2.3×, BLOCKED — this is the prize.** Needs chrome to composite directly into memory *we* own, i.e. an **Ozone X11 backend + MIT-SHM**. `chrome-headless-shell` has **no Ozone backend at all**, which is the entire reason we are booting the full binary. Cheap interim options were tested and **rejected** (device-scale-factor tricks, screencast quality knobs) — no shortcut exists. |
| **3** | Real GPU (not SwiftShader) | Not started. |
| **4** | Audio | Not started. |

**So the job is: get full chrome stable enough to reach tier 2.5, then implement
Ozone X11 + MIT-SHM zero-copy.**

---

## 3. Where slice 86 left things (and what it corrected)

Slice 86 fixed the browser's `exit 191`. Root cause was **ours**: `mkdir(2)`
discarded the caller's mode. `vfs_mkdir()` hardcoded `0755` for every directory
ever created and `sys_mkdir` explicitly dropped its mode argument. Chrome's
`ProcessSingleton::Create` does `mkdir(dir, 0700)` and then
`CHECK(GetPosixFilePermissions(dir,&m) && m == 0700)`; it read back `0755` and
took `IMMEDIATE_CRASH()`. **headless-shell has no ProcessSingleton — that is
precisely why ~40 slices never saw it.**

Also fixed in 86, each a real ABI gap independent of the above:

* **Named AF_UNIX sockets had no `bind`/`listen`/`accept` whatsoever** (bind fell
  into the `sockaddr_in` path → EINVAL; listen → EOPNOTSUPP). Registry lives in
  `socket.c` so `struct sock` does **not** change size.
* All sockaddr write-backs (`getsockname`/`getpeername`/`accept`/`recvfrom`×2)
  ignored the caller's `*addrlen` and wrote a fixed 16 bytes → now clamped with
  Linux truncation semantics (`lx_addr_writeback`).
* `getsockname` on an AF_UNIX socket claimed `AF_INET`; answers `sockaddr_un` now.
* `readlink` took `strlen()` of an **uninitialised kernel buffer** — memory
  disclosure, plus it could return a length never written.
* `chmod`/`fchmodat` were deliberate no-ops even though `vfs_chmod()` existed and
  worked. (Also clears a latent NSS failure: NSS chmods its key DB to 0600 and
  refuses the DB if that does not stick.)

**Slice 85 was retracted.** It claimed the trap was `__stack_chk_fail` (a stack
canary smash), inferred from disassembling the trap address. Wrong: **clang pads
between functions with `int3`**, so an `int3` next to a `call __stack_chk_fail`
proves nothing. A probe dumping *both* canary sides at fault time printed
`fs:0x28 = 0` and `[rbp-0x30] = 0` — **matching**, so that branch never runs. The
trap is `int3` followed by `ud2` = **Chromium's `IMMEDIATE_CRASH()` macro**.

---

## 4. YOUR TARGET: the child-process GP fault

Current run (`CHROME_FULL=1`) reaches ~74 s and then:

```
[73862 ms] [sigfault] pid=10 name=chrome+T vec=13 sig=11 code=2
                      rip=0x00000000036c3fc4 addr=0x0 -> user handler
  rax=0x0000000000000000  rbx=0x000010200021f888  rcx=0x00001000014e8800  rdx=0x000010200021f800
  rsi=0x0000000000000001  rdi=0x0000000000000000  rbp=0x0000102f055fdda0  rsp=0x0000102f055fdd10
  r8 =0x0000000011a84a40  r9 =0x0000000000000000  r10=0x0000000074736f01  r11=0x0000102f055fe320
  r12=0x0000000000000008  r13=0x0000000000001012  r14=0x000010200021f800  r15=0x0605010702020301
[74506 ms] [proc] pid=10 'chrome+T' exit code=191 (0xbf) cpu=39562 ms syscalls=311
```

Facts worth holding onto:

* **`vec=13` = #GP**, not a page fault. `addr=0x0` is meaningless for #GP.
* It is a **thread** (`+T`) of a **child** process, not the browser.
* It burned **39.5 s of CPU across only 311 syscalls** — it did a lot of real
  compute. That profile fits a renderer/GPU process, not a bootstrap path.
* **`r10=0x0000000074736f01`** and **`r15=0x0605010702020301`** carry
  ASCII-looking bytes (`0x74 0x73 0x6f` = `"tso"`). By this codebase's standing
  rule, **ASCII in registers means memory corruption**, not a wrong syscall
  return. This is a different bug class from everything slices 75–86 chased.
* A second child also dies earlier: `pid=29 'chrome+T' exit code=127` at ~24 s.
  127 usually means an exec/loader failure — likely a *separate, easier* bug and
  possibly worth doing first.
* `rip=0x036c3fc4` sits inside the chrome mapping (chrome ET_DYN base
  `0x500000`; the loader is separately at `0x40000000`), so the file address is
  `0x36c3fc4 - 0x500000 = 0x31c3fc4`. **Confirm pid 10's own
  `[elf] load OK ... type=ET_DYN` line before trusting that** — do not assume the
  base, that mistake has been made in this arc.

### Suggested opening moves

1. **Identify which child pid 10 is.** Its argv (`--type=renderer` / `--type=gpu-process`
   / `--type=utility`) is in the process-creation log lines. That alone narrows
   the search enormously.
2. **Chase pid=29's exit 127 first** if it is cheap — a loader failure is far
   easier to diagnose than corruption, and it may be upstream of the later crash.
3. **Disassemble `0x31c3fc4`** with `logs/control_disasm.sh` (edit the `TRAP`
   variable). A #GP with garbage in registers is usually a bad indirect call, a
   misaligned SSE/AVX access, or a non-canonical address load — the instruction
   will tell you which. **Remember the slice-85 lesson: do not build a theory on
   one instruction's neighbourhood.**
4. **Suspect our memory management, not our errnos.** The corruption class points
   at `mmap`/`munmap`/`mprotect` splitting, CoW/fork, TLS/`clone` setup, or a
   copy_to_user that writes past a caller's buffer. Slice 86 found *four*
   unclamped writers; assume more exist and audit systematically rather than
   one-by-one.
5. **Consider a systematic instrument** rather than another one-off probe: e.g. a
   debug mode that validates every `copy_to_user` destination against the target
   VMA's bounds and permissions, and logs any write that crosses a VMA end. That
   would catch this whole bug class at once instead of per-symptom.

---

## 5. Instruments you already have

* **The syscall ring now records RETURN VALUES** (added in slice 86 — this is what
  broke the case open). Dumped as `[lx-recent]` on any fatal fault and on any
  non-zero `exit_group` via `[xexit]`. Format:
  `[tid] NUM(name) = ret (0xhex)`. Sentinel `0x5eed` = "did not return".
* `[sigfault]` prints a full register dump plus `[canary] fs_base / fs:0x28 /
  [rbp-0x30]` and `mmap_debug_fault_vma()` for both the fault address and rip.
* `[mkdir]`, `[rdlink]`, `[unixbind]`, `[scm]`, `[uxref]`, `[uxclose]`, `[chan]`,
  `[peercred]`, `[procexe]` probes, all under `#ifdef CHROMIUM_BOOT`.
* **WSL2 Ubuntu control rig** — `logs/control_*.sh` runs the *same* chrome binary
  on real Linux so you can eliminate hypotheses without the ~7-minute guest loop.
  `control_strace.sh`, `control_disasm.sh`, `control_sym.sh`, `control_diff.sh`,
  `control_fullchrome.sh`. **Correct invocation form is
  `timeout N env VAR=... ./chrome`** — putting `env VAR=` before `timeout`
  applies it to `timeout`, which cost a batch once.

## 6. Build & run

```bash
bash logs/build_full.sh                 # FULL chrome flavour (CHROME_FULL=1)
timeout 420 python logs/run_watch.py    # boots QEMU/WHPX, drives the page
```

* Serial output lands in **`logs/run_watch.log`** (NOT `logs/serial.log` — that
  is a different harness and reading the wrong one has wasted time before).
* Screenshots: `logs/wat_*.png`.
* The headless flavour builds via `logs/build_vid.sh` / `build39.sh`.

### Hard rules — every one of these has burned someone

* **Touching a widely-included header (`socket.h`, `proc.h`, `file.h`) or growing
  `struct proc` / `struct sock` ⇒ delete ALL kernel `.o` and rebuild.** Stale
  objects produce impossible symptoms.
* **`programs/chromewin/chromewin.o` must be deleted on any flavour change** — it
  does not depend on the flavour define, so a stale one execs the wrong chrome
  path and dies with exit 127. Both build scripts already do this; keep it.
* **Verify the ISO mtime after every build.** A silent build failure once ran the
  previous ISO for an entire three-run batch and produced a completely wrong
  conclusion.
* `kprintf` has **no `%o`**. Print modes as hex.
* Never `yield` while holding the BKL.

## 7. Method lessons that actually paid off

* **A userspace abort with no failing syscall before it means look for a wrong
  VALUE in something that SUCCEEDED, not a wrong errno.** This is what finally
  cracked slice 86 after ten slices of errno archaeology.
* **Instrument the general thing, not the specific suspicion.** Adding return
  values to the ring answered a question that a dozen targeted probes had not.
* **Do not trust a disassembly neighbourhood.** clang's `int3` padding made a
  wrong conclusion look airtight for a full slice.
* **ASCII bytes in registers ⇒ memory corruption.**
* When a fix lands, **re-run before believing the log**; and when a conclusion is
  wrong, retract it explicitly in the ledger — this arc has been derailed twice by
  a confidently-wrong note left standing.
