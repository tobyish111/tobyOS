# Handoff: full Chromium on tobyOS — CoW-after-fork corruption (post-slice-87)

**Read this whole file before touching the arc.** Baseline for the *previous*
wall was slice 86 (`83d8b68`). Slice 87 landed shootdown + AT_RANDOM fixes;
the remaining wall is below. Companion notes: `docs/chromium-hypothesis-ledger.md`
(Slice 87 section) and memory topic `chromium-bringup.md`.

---

## 1. What this project is

tobyOS runs **unmodified Linux x86-64 binaries** via Track B. Headline
workload: **genuine Chromium 151** from `programs/chromewin` over CDP
`--remote-debugging-pipe`.

**headless-shell** already works end-to-end (YouTube, react.dev, resize).
Current work is **full `chrome`** (`CHROME_FULL=1`) — required for tier 2.5
(Ozone X11 + MIT-SHM zero-copy frames). Headless-shell has no Ozone backend.

| Tier | Goal | Status |
|---|---|---|
| **1** | Cheap display path | DONE |
| **2** | Kernel-side serialization | DONE |
| **2.5** | Zero-copy frames (Ozone X11 + MIT-SHM) | **INFRA LANDED; OPEN — headed MapWindow wall** |
| **3** | Real GPU | Not started |
| **4** | Audio | Not started |

---

## 2. What slice 87 settled

### The ~74s crash is PartitionAlloc ThreadCache, in a browser THREAD
- pid=10 is `clone` tid=10 in **tgid=3** (browser), not a child process.
- Flags include `--in-process-gpu --no-zygote`.
- Fault was `#GP` on `mov (%r15),%rdx` with non-canonical freelist head
  `r15=0x0605010702020301` at file addr `0x31c3fc4` (chrome base `0x500000`).
- Code is PA ThreadCache: `pthread_getspecific` + `imul $0x5e0`, freelist
  encoding (`bswap`, `second == ~first`), super-page math.

### pid=29 exit 127 is NOT your bug
`execve(xdg-settings check default-web-browser ...)` — missing helper. Benign.

### Shootdown IRQ-off deadlock — FIXED
Two CPUs in CoW faults with IRQs off deadlocked on each other's shootdown
IPIs → both timed out → stale WRITABLE TLBs → PA freelist corruption.
Baseline had `[tlb] ERROR: shootdown STILL un-acked after 16 rounds`.

**Fix:** `tlb_shootdown_self_ack()` in the wait loop (`apic.c`) — reload CR3
+ bump our tlb gen so peers waiting on us complete. **Do not `sti`** (caused
kstack_top=0 SMEP panic). **Do not drop BKL** (VMA races → INT3 at ~12s).
Validation: tlb WARN/ERROR **→ 0**.

### AT_RANDOM was zeros — FIXED
Stack pad at `USER_STACK_TOP_VA-16` was never filled; canaries were 0.
Now `rng_fill` on exec (`proc.c` + `fork.c`). Live: non-zero matching canary.

### Also
- brk shrink: free-before-shootdown → batch + shootdown-before-free
- `#GP` sigfault path dumps pointer-source VMA/pgj (rbx/r14/r15)

---

## 3. YOUR TARGET: CoW-after-fork stale-writable window

With shootdowns acking cleanly, threads still die in PA with poison/ASCII.
Page journal on the pointer-source page of a representative crash:

```
map4k → cow-fork-wp → cow-copy
```

`vmm_cow_fork` (`page_fault.c`) write-protects the **entire** user half, then
calls `tlb_shootdown_remote()` **once at the end**. Sibling threads keep
running in user mode with cached WRITABLE translations for that whole walk.

**Tried and retracted:** periodic shootdowns every 256 WP pages mid-walk —
caused mass simultaneous `#GP` and browser `exit 191` at ~13s. Do not retry
without a stop-the-world design.

### Suggested next moves (in order)

1. **Stop-the-world the thread group around `vmm_cow_fork`'s WP walk.**
   Soft-stop siblings before clearing PTE_WRITABLE; shootdown; resume.
   Do **not** retry mid-walk periodic shootdowns without that — every-256
   caused mass `#GP` and browser death at ~13 s.
2. **Teardown race:** on chrome `exit_group(191)` a sibling was scheduled
   with `kstack_top=0` → `kpanic` in `sched.c`. Same family as the old
   reaped-slot-still-runnable bug; close it on the crash/exit path before
   trusting long runs.
3. Re-measure tlb ERROR rate on a clean non-panic run (post-panic logs
   are contaminated). Self-ack should keep them at 0 when the tree is live.
4. GLib getauxval abort: richer auxv (AT_HWCAP etc.) looks sufficient;
   only add `/proc/self/auxv` if it returns.
5. Then: browser stays up whole session, zero PA `sigfault` in `0x36c????`,
   then tier 2.5 Ozone/MIT-SHM.
6. WSL control rig (`logs/control_*.sh`); trap math `rip - 0x500000` only
   after confirming that process's `[elf] load OK` base.

---

## 4. Tier 2.5 infra landed — close-out NOT DONE (MapWindow wall)

Stability gate (full `CHROME_FULL` session) is met on **headless ozone**:
`timeout 360 python logs/run_watch.py`, example.com, `--single-process
--ozone-platform=headless --use-gl=disabled`, `-smp 1 -m 8192`:

- Browser alive full session (no `exit_group(191)`, no `kstack_top=0` panic)
- `[tlb] ERROR` = 0; no live `#GP`/`sigfault` on the good run
- CDP screencast frames ≈ **930+** @ q60 800×457 (JPEG path). Slice 68
  baseline was ~1050 on react.dev — **not** a SHM win; still encode+CDP.
- react.dev under `--single-process` still ImmediateCrash/PA-poison right
  after bootstrap — keep measure URL light until that is fixed.

| Piece | Status |
|---|---|
| Teardown `on_cpu` wait + `kstack_top==0` sched guard | Done |
| `tg_vm_quiesce` / `tg_vm_resume` + deferred requeue in `sched_finish_switch` | Done |
| Eager private copy for writable pages in `vmm_cow_fork` (CHROMIUM_BOOT) | Done |
| PI futex: pass `uaddr2`, `WAIT_REQUEUE_PI` acquires mutex, `UNLOCK_PI` handoff | Done |
| Fake X (`xserver.c`): CreateWindow/GC/EWMH props/MIT-SHM/PutImage/ShmPutImage | Done |
| SysV shm + `ABI_SYS_XFRAME_POLL` + chromewin SHM blit path | Done |
| Headed `--ozone-platform=x11` → browser MapWindow / ShmPutImage | **OPEN** |
| SHM as default paint (no `Page.startScreencast`) | **OPEN** |

**Headed wall (current):** after RANDR `Displays updated` 800×600 only 10×10
clipboard `CreateWindow`s; UI `pid=3` parks on `futex(to=-1)`;
`Target.createTarget` never answers. Dropping `--single-process` → utility
children die “after 15 seconds with no connection”. `--headless=new` +
`ozone-platform=x11` ignores X (no CreateWindow). Do **not** mark tier 2.5
DONE until headed MapWindow + SHM frames are the default paint path.

**Next:** unblock headed UI after Displays updated (likely remaining
sync/event or process-model issue — PI futex `uaddr2` fix did not clear it);
then ShmPutImage → `xframe_poll` → re-measure vs Slice 68.

## 5. Build & run

```bash
bash logs/build_full.sh
timeout 420 python logs/run_watch.py
```

Serial: **`logs/run_watch.log`** (not `logs/serial.log`). Screenshots:
`logs/wat_*.png`.

### Hard rules (unchanged)

* Touching widely-included headers / growing `struct proc` / `struct sock`
  ⇒ delete ALL kernel `.o` and rebuild.
* Delete `programs/chromewin/chromewin.o` on flavour change (build scripts do).
* Verify ISO mtime after every build.
* `kprintf` has no `%o`. Never `yield` while holding the BKL.
* Shootdown waits: self-ack OK; **never sti / never drop BKL** there.

---

## 6. Method lessons this slice

* ASCII / shuffle-mask bytes in pointer regs ⇒ heap/freelist corruption;
  disassemble before theorizing (slice-85 int3-padding lesson still applies).
* Instrument the general thing: zeroing tlb ERROR count proved the self-ack
  fix; pgj on pointer-source proved the residual is CoW-after-fork.
* When a clever fix regresses (sti, BKL drop, mid-walk shootdown), **retract
  it in the ledger** immediately — this arc has been derailed by stale claims.
