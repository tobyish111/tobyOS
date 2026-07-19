# Task: get real headless Chromium to `--dump-dom` `<h1>tobyOS</h1>` on tobyOS — grind the remaining ABI tail

You are picking up a **live, deep, successful Chromium bring-up**. A prior agent
took real, unmodified `chrome-headless-shell` (V8 + Blink + SwiftShader, on
tobyOS's Linux ABI / Track B) from **crashing at GL init** all the way to
**running its full engine — GL, storage, signals, and V8 memory all working — for
~22.5 s**, by fixing **eight genuine kernel/ABI bugs** (slices 12–18, all merged).
Chrome no longer crashes at GL, no longer panics the OS, and drives its whole
startup. **Exactly one class of wall remains between you and the deliverable: a
sequence of downstream ABI gaps that chrome hits as it finishes bring-up and tries
to navigate.** Each fix reveals the next — this is normal for a program of Chrome's
size — but every one so far has been a real, bounded kernel bug, not a rewrite.

**Your job: close the remaining gaps until `--dump-dom` prints `<h1>tobyOS</h1>`,
then switch the harness to `--screenshot` and host-diff the PNG.** The GL/render
subject of the *original* handoff is DONE; this is the tail past it.

---

## ⚠️ CORRECTION (2026-07-19, slice 20) — READ THIS BEFORE "The current wall (slice 19)" BELOW

**Slice 19 as written below is a MISDIAGNOSIS. The "`read()` into a read-only page"
is a NON-BUG — do NOT chase it.** Measured (see `docs/chromium-bringup-m1.md`
"Slice 20" + memory `chromium-bringup.md` SLICE 20):

- `0xc0da000`–`0xc0dc000` is chrome's own ELF section **`protected_memory`**
  (`base::ProtectedMemory`, a RO-after-init function-pointer table; `objdump -h`).
  **Chrome itself `mprotect`s it `PROT_READ`**, then *verifies* it's read-only by
  issuing a **syscall write** — `prlimit64(0,RLIMIT_NPROC,NULL,old_limit=0xc0db000)` —
  and **expecting `-EFAULT`**. Slice-18's `copy_to_user → -EFAULT` is therefore
  **exactly correct**; chrome's check passes. There is nothing to fix here.
- The "downstream NULL deref at 0x210" is a *separate*, non-deterministic teardown
  crash, not caused by the EFAULT.

**The REAL front is the render tier** (not a loader/RELRO bug): (1) a flaky,
timing-dependent crash that when it fires is *deterministically* `memcpy+0x35d` in
`libc.so.6` (a **stack overflow from a corrupt/too-large length** during SwiftShader
Vulkan extension enumeration) — the **same memory-corruption family** as the ld.so
`check_match` crash reading libvulkan's `link_map->l_versyms = garbage`; and (2)
even when GL init *succeeds*, chrome never navigates/dumps the DOM (idle-exits
`code=0`). **VLOG is compiled out of official chrome** (`--vmodule`/`-v=1` = zero
output). A **lib load map** (`[libmap]` in `linux_mmap_file`) and a PTE-flag dump in
the uaccess path are now in-tree (CHROMIUM_BOOT). **NEXT = root-cause the single
memory-corruption root** (ld.so link_map vs V8-cage/stack allocator collision?) — it
gates both the crash and probably the no-navigation. Ignore the "slice 19" section
below except as historical context.

---

## THE PRIME DIRECTIVE: instrument-first, reuse what exists, add the smallest shim

Every win in this arc came from **measuring, not guessing** — and repeatedly
disproving handed-down hypotheses (the "render NULL dispatch" was a signal bug; the
"V8 SEGV" was an mprotect bug; the "needs a surfaceless ANGLE" was a `./` path
bug). Before writing anything: reproduce, read the serial log, symbolize the fault,
and confirm the mechanism. Then fix the actual bug with the smallest change.

Instruments already in the tree (keep them; extend as needed):
- **`src/isr.c`** on a fatal user fault: register dump + a **user-stack scan** that
  flags main-PIE qwords (`[0x500000,0x0d000000)` → `CODE main+…`) AND
  `.so`-region qwords (`0x1000_0000_0000+` → `LIB?`), plus `fault_count` /
  `last_fault_rip`. For a **kernel** fault on a user address it also prints
  `[kpf] KERNEL fault at user addr=… err=… rip=…` + the VMA (see below).
- **`src/mmap.c` `mmap_debug_fault_vma(addr)`** — dumps whether an address is
  covered by an mmap-VMA (and its `prot`/`flags`) or the nearest mappings. This is
  how the V8-heap prot bug and the read-only-page bug were both pinned.
- **`src/page_fault.c` `uaccess_prepare_write`** logs `[uaccess] copy_to_user ->
  EFAULT: … addr=0x…` + the recent-syscall ring (first hit) when a syscall writes
  to a read-only user buffer.
- **`src/syscall.c`** `[linux] UNHANDLED syscall N (name)` first-hit logger + the
  recent-syscall ring (dumped by isr on a fatal fault). `TRACE=1 bash
  logs/chromium-m0.sh` = the full firehose.
- **Symbolization recipe (proven).** Main PIE loads at `0x500000`: `objdump -d
  --start-address=<rip-0x500000> programs/chromium/chrome-headless-shell-linux64/
  chrome-headless-shell`. A `.so` return address needs the **library load map**:
  temporarily log each executable file-backed `mmap` base + the `.so` open path by
  fd (this arc's `[dlopen]`/`[libmap]` pattern — reintroduce from git history of
  `src/syscall.c` if needed) and `objdump` the `.so` at `retaddr - so_base + off`.

If something is genuinely missing, add the smallest shim that lets chrome's
existing code path work — the way this arc added `rename`, a fake X server, VMA
splitting, and `copy_to_user` fault-safety rather than reimplementing subsystems.

---

## What is DONE (measured; do NOT redo) — slices 12–18, all merged to `main`

The render-GL wall the ORIGINAL handoff (`docs/chromium-render-gl-bug-prompt.md`)
was about is **solved**. On top of it, the storage + engine tiers are solved too:

| Slice | Fix (all merged) | Effect |
|---|---|---|
| 12 `2f2525a` | `resolve_user_path` collapses interior `.`/`..` (`path_lexical_clean`) | Vulkan loader loads the SwiftShader ICD → WSI-extension wall passed (the deep-dive's "surfaceless ANGLE" theory was WRONG — it was `/opt/chrome/./libvk…`) |
| 13 `ef4ffb3` | AF_UNIX `socket`/`connect` + **in-kernel fake X server** at `/tmp/.X11-unix/X0` + `recvmsg`/`recvfrom`/`sendmsg` on UNIX (were `ENOTSOCK`) + stream partial-consume | **`xcb_connect` + `eglInitialize` SUCCEED — SwiftShader Vulkan initializes headless** |
| 14 `cd5ad9f` | stable per-file inode (`vfs_file.ino` from tobyfs; `lx_fd_ino` prefers it) | chrome's shm inode-match passes (8s→47s) |
| 15 `04b940b` | tobyfs `rename` (new vfs op) + `fsync`/`fdatasync`/`unlinkat`/`flock` | leveldb + SQLite profile stores work |
| 16 `a3fbada` | **SA_RESETHAND** one-shot handler was zeroed *before* `rip` read it (`signal.c`, both paths) | the "render NULL dispatch" was **SIGSEGV delivered to a NULL handler** — fixed, unmasking the real fault |
| 17 `16f6c4f` | **`sys_mprotect` now SPLITS VMAs** (was clobbering the whole region's prot) | V8 W^X sub-range mprotects work → V8-heap writes work (14s→16.5s) |
| 18 `22ae599` | **`copy_to_user`/`clear_user` return `-EFAULT`, never panic** (`uaccess_prepare_write` pre-validates writability) | kernel no longer panics on a bad user out-pointer (16.5s→22.5s) |

**Verify state before starting:** `dd if=/dev/zero of=disk.img bs=1M count=16`
(fresh `/data`) then `bash logs/chromium-m0-clean.sh`. Expect: 0 GL/EGL/Vulkan
errors, 0 KERNEL PANIC, leveldb/SQLite quiet, chrome ~22 s, no DOM yet.

---

## The current wall (slice 19) — measured, do not re-derive

Chrome does a **`read()` (tid 8) into a page that is present + READ-ONLY**
(`0xc0db000`/`0xc0da000`, in chrome's LOW ELF-loaded region — NOT any mmap-VMA, so
almost certainly a **rodata / `GNU_RELRO`** segment or a RW segment the ELF loader
mapped read-only). With slice 18 in, `copy_to_user` now returns `-EFAULT` (no
panic), but the `read()` then fails and chrome trips a **downstream `SIGSEGV`
(NULL deref at `0x210`)** — it never expected that buffer to be unwritable.

The decision to make first (instrument, don't guess):

1. **Is `0xc0db000` SUPPOSED to be writable?** Dump chrome's ELF `PT_LOAD`
   flags + the `PT_GNU_RELRO` range and compare to what the loader (`src/elf.c`)
   and ld.so actually mapped. If a **RW segment was mapped read-only** (or RELRO
   was applied to a page that should stay writable), that's the real bug — fix the
   loader / the RELRO mprotect. Chrome writes to `.data`/`.bss` fine for 22 s, so
   the read-only region is specific — pin which segment `0xc0db000` is in.
2. **Or is chrome genuinely handing `read()` a read-only buffer?** Then trace the
   fd + call site (which fd is tid 8 reading? what's the buffer's provenance?). A
   read-only read() target usually means a wrong pointer upstream (a prior syscall
   returned bad data, or a struct field the kernel filled wrong) — follow that.

Then the downstream NULL deref at `0x210` should vanish once the `read()` succeeds
(it's a consequence of the `-EFAULT`). If it persists, symbolize it (main-PIE
`objdump`; it's a `<something>->field` at offset `0x210` off a NULL base).

---

## Reusable kernel/ABI wins so far (context — these are general, not chrome hacks)

interior-dot path normalization · named/abstract AF_UNIX + `recvmsg`/`recvfrom` on
UNIX · stable per-file inodes · tobyfs `rename`/`fsync`/`unlinkat`/`flock` ·
**SA_RESETHAND** one-shot signal delivery · **`mprotect` VMA splitting** ·
**`copy_to_user` `-EFAULT` robustness**. Expect the tail to keep surfacing bugs of
this flavor (VM/mmap edge cases, ELF/RELRO, a syscall filling a struct slightly
wrong, thread/signal interactions) — each bounded, each found by instrumenting.

---

## Read first

- **`docs/chromium-bringup-m1.md`** — the full burn-down. **Slices 15–19** at the
  end are this tail; read them (esp. the exact fault addresses + diagnostics).
- **Memory** (`C:\Users\tdude\.claude\projects\c--CustomOS\memory\`):
  `chromium-bringup.md` (read the "SLICES 17-19" entry FIRST, then 12–16),
  `linux-abi-compat-b1.md`, `tobyos-build-env.md`.
- `docs/chromium-render-gl-bug-prompt.md` — the ORIGINAL handoff (GL subject, now
  solved) for methodology + the symbolization/build conventions.

---

## Environment, build, verify (match the project exactly)

- **Build/run:** MSYS2/UCRT64 clang (`tobyos-build-env.md`). Reproduce:
  `bash logs/chromium-m0.sh` (incremental) — **but see the gotcha below**;
  `bash logs/chromium-m0-clean.sh` for a full clean rebuild. `TRACE=1` adds the
  firehose. Carry the `TMP` fix on `$(CC)`/`$(HOST_CC)` (the scripts already do it).
- **CLEAN BUILDS ONLY right now.** After the struct changes in this arc (`struct
  sock`, `struct vfs_file`, `struct vma_table`), **incremental builds boot-hang at
  Limine module-load** (symptom: ~1280-byte log stuck at `Loading module
  install.img`, QEMU exits ~35 s). Always `logs/chromium-m0-clean.sh`. Root cause
  of the incremental hang was never chased — a `make` dependency issue; if you fix
  it, incremental is ~5 min vs ~15.
- **FRESH `disk.img` per investigation:** `dd if=/dev/zero of=disk.img bs=1M
  count=16`. A `disk.img` with a `/data/cr` profile corrupted by an earlier
  crashed run makes chrome spin on leveldb recovery and confounds the read.
- **BUILD/BOOT LAUNCH GOTCHA:** `nohup … &` (or a trailing `&`) inside a
  `run_in_background` Bash call only tracks the *launch*, not the build — you get a
  premature "done". Run `bash logs/chromium-m0-clean.sh` **directly** with
  `run_in_background: true` (no `&`), or attach a poller that greps `run.out` for
  `=== [4/4] GAP LIST ===`.
- **Symbolization:** main PIE at `0x500000`; kernel at `0xffffffff80000000`
  (`objdump -d tobyos.bin`). `.so`'s need the library load map (see prime
  directive). Serial logs are binary-ish — always `grep -a`.
- **Verify:** `--dump-dom` must print `<h1>tobyOS</h1>` (self-verifying in the
  serial log). Then switch the `#ifdef CHROMIUM_BOOT` harness in `src/kernel.c` to
  `--screenshot=/data/shot.png` and get the PNG out (dump it over serial as base64
  from the harness after chrome exits, or parse `/data` from `disk.img`), then diff
  vs the SAME chrome build on the host. **Never** `taskkill msedge` (the user's
  real browser). The harness is on the committed loader route (`--use-gl=angle
  --use-angle=vulkan --enable-unsafe-swiftshader` + `VK_ICD_FILENAMES` + `DISPLAY=:0`).
- **One coherent slice per branch off `main`; extend `docs/chromium-bringup-m1.md`
  + a `MEMORY.md` line each slice; commit trailer
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`; merge `--no-ff`.**

---

## Walls / non-goals (know them; don't rediscover)

- **`--incognito` does NOT skip the storage/profile tier** (measured, slice 15):
  `shared_proto_db` is browser-global; chrome then HANGS ~5 min. Flags can't route
  around storage or the render tier — the ABI has to actually work.
- **Software-raster flags** (`--disable-gpu-rasterization` etc.) were inconclusive
  (slice 16) — don't re-chase them as a shortcut.
- **GPU-accelerated GL is a much later tier**; stay on SwiftShader (software).
- Sandbox stays off (`--no-sandbox --single-process --no-zygote`, already set).
- Once `--dump-dom` + `--screenshot` land, the next milestones are M2 (a real
  JS/React SPA) and M3 (WINDOWED via the tobyOS compositor — see
  `docs/chromium-bringup-render-prompt.md`).

---

## Scope honesty

This is a **long, layer-by-layer tail** — Chrome is one of the largest programs in
existence and the last mile to a painted/DOM'd page crosses many small ABI gaps.
But the hard, uncertain parts (GL bring-up, V8 memory, signals, storage) are
**done**; what remains has been, so far, a steady sequence of bounded, real kernel
bugs each cracked by instrumenting first. Reproduce, symbolize the current fault,
fix the smallest thing, re-measure. Chrome is already running its full engine —
you are closing the gap between "runs" and "renders."
