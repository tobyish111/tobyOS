# Handoff: full Chromium on tobyOS — tier 2.5 headed Ozone close-out (post-slice-88)

> # ⚠️ SUPERSEDED AND PARTLY WRONG — DO NOT WORK FROM THIS FILE
>
> **Read `docs/chromium-handoff-post-slice-91.md` instead.**
>
> This document's entire target — tier 2.5, zero-copy frames via Ozone X11
> + MIT-SHM — was **DISPROVEN in slice 91**. Chromium does not present
> pixels through X: measured on a REAL X server (Xvfb + xtrace, real page,
> 90–120 s, 66 extensions `present=true`), across three flag
> configurations including multi-process, `PutImage` and `ShmPutImage` are
> **zero every time**. Chrome attaches an SHM segment, detaches it, and
> renders into an offscreen GLX pbuffer. **No work in `src/xserver.c` can
> produce the frame this document is chasing.**
>
> Also wrong here: §4's "Path A — fix the `-smp 4` AP arc" was retracted in
> slice 89 and then **re-instated in slice 91** — the freeze is real and
> intermittent, and slice 89's exoneration was sampling error.
>
> Kept for its evidence trail (the slice-88 root cause, the control-rig
> method, the instrument list), which remains accurate and useful.

**Historical.** Baseline commit: **`7ae1bac`** (slice 88). Long-form
evidence: `docs/chromium-hypothesis-ledger.md` (slices 85–91) and the
memory topic `chromium-bringup.md`.

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
| **2.5** | **Zero-copy frames** | **CLOSED — PREMISE DISPROVEN (slice 91); this row is historical** | Sized at **~2.3×** (slice 68). Needs chrome to composite into memory we own = **Ozone X11 + MIT-SHM**. All infrastructure landed (slice 87); the headed bring-up wall was root-caused and mostly fixed (slice 88); **MapWindow reliability is the last gap** before wiring SHM as the default paint path. Cheap interims (device-scale-factor, screencast quality) were tested and **rejected** — no shortcut exists. |
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

## 3b. SLICE 89 SUPERSEDES §4 BELOW — read this before acting on it

**Path A is dead: `-smp 4` does NOT reproduce the slice-88 freeze.** Four
runs at `-smp 4`: APs took real work every time (cpu1 1.2M BKL acquisitions,
cpu2 407k, cpu3 313k), no clone3 pile-up, no 7.6s freeze. `run_watch.py`
now has an `SMP=` env knob (default still 1). The quiesce/AP-queue arc is
NOT what stands between us and tier 2.5 — don't spend a session on it.

**THE CURRENT WALL — repeatable signature, and the cause is UPSTREAM of the
socket.** Every failing run ends the same way:

```
[31] 46(sendmsg) a1=7 = -32     <-- EPIPE
     rt_sigaction(11) getpid gettid exit_group(191)
stderr: "Crashing due to FD ownership violation:"
```

**The EPIPE is a CONSEQUENCE, not the bug** — this is the same divergence
slice 83 recorded (Linux does sendmsg → recvmsg → PR_SET_PTRACER →
sigaltstack → 8× rt_sigaction; we do sendmsg → EPIPE → abort). What is new
is WHY the peer is gone:

```
[6952 ms] [execve] pid=1 now running 'chrome_crashpad_handler'
[6954 ms] [proc]   pid=1 'chrome_crashpad_handler' exit code=0
                   cpu=0 ms syscalls=4
```

**chrome_crashpad_handler execs and exits TWO MILLISECONDS later having
issued FOUR syscalls.** Slice 76 fixed the double-fork so the exec happens
at all, and nobody then checked that the handler SURVIVES. It does not. Its
socket endpoint dies with it, so every later client sendmsg EPIPEs and that
client aborts 191.

[xexit] now also fires on a QUIET DEATH (exit 0 with <32 syscalls) -- the
old non-zero gate made the one exit that matters the one exit we never saw.
It caught the four immediately:

```
[1] 14(rt_sigprocmask) a1=0 = 0
[1] 14(rt_sigprocmask) a1=2 = 0
[1] 59(execve)             = <no-return sentinel>   <-- exec SUCCEEDS
[1] 231(exit_group) a1=0                            <-- FIRST call of the
                                                        NEW image
```

**SLICE 90 RESOLVED THIS, AND THE SLICE-89 DIAGNOSIS ABOVE WAS WRONG.**
There was no execve/auxv bug. `entry=0x201120` was the tell: it is the
SAME entry as the `xdg-settings` stub in the same log, because the Makefile
**deliberately staged a copy of that stub as
`/opt/chrome/chrome_crashpad_handler`** — and that stub's entire body is
`exit_group(0)`. The handler "quiet-died" because we told it to. (The real
handler is DYNAMIC — `PT_INTERP=/lib64/ld-linux-x86-64.so.2`, e_type=DYN,
entry 0x3fc00 — so "no ld.so syscalls at all" should have been read as
"this is not the binary you think it is", not as a broken process image.
Checking the ELF header costs one command and would have skipped the whole
detour.)

### WHAT THE FIXED HANDSHAKE REVEALED — the real remaining crash

With the handler answering, the failure signature CHANGED, which is the
point of the fix:

```
before: sendmsg a1=7 = -32 (EPIPE)  -> abort 191
after:  sendmsg a1=7 = 40 (SUCCESS) -> 128(?) = -38 ENOSYS -> abort 191
```

Syscalls 128 (`rt_sigtimedwait`) and 297 (`rt_tgsigqueueinfo`) were both
`-ENOSYS`; both are now implemented. But they were only the crash-REPORTING
path failing. The actual crash is upstream and now visible:

```
[sigfault] pid=30 chrome+T vec=14 sig=11 code=1
           rip=0x59be96f addr=0x18  rax=0x0  r14=0x1024000f3e80
```

Disassembled at `rip - 0x500000` (main PIE base):

```
mov 0xb8(%r14),%rax    ; rax = *(r14 + 0xb8)   -> NULL
mov 0x18(%rax),%rdi    ; FAULT reading 0x18
test %rsi,%rsi ; sete %al
test %rdi,%rdi ; sete %cl ; or %al,%cl ; je ...
```

Chrome loads a member at `+0xb8` and dereferences it **with no null check**,
then null-checks the *result* two instructions later — i.e. it treats
`[r14+0xb8]` as an invariant some earlier init was supposed to establish.
**This is the "wrong VALUE in something that SUCCEEDED" class from the
method lessons, not a wrong errno.** Find which subsystem owns that object
and what we let it skip: something reported success (or degraded silently)
and left the member null. `r14` is a heap object; `rbx+2` is compared
against `0x41` just above, so there is a type tag to identify it by.

**CONFIRMED ACROSS RUNS (slice 90 final):** the crash rip is
**deterministic** -- `0x59be96f`, `addr=0x18`, every time. And the dying
thread now exits **139 (128+SIGSEGV)** instead of the blind
`exit_group(191)`: with rt_sigtimedwait/rt_tgsigqueueinfo present, chrome
RAISES the signal properly instead of aborting. MapWindow fired **twice**
in that same run (`op=8 seq=239`, `seq=283`). So the sequence is now:
handshake OK -> window mapped -> this one thread NULL-derefs -> no
ShmPutImage ever happens. **One deterministic bug stands between here and
the first zero-copy frame.**

**IMPLEMENTATION GOTCHA worth keeping:** the first cut of `rt_sigtimedwait`
spun on `sched_yield()` **while holding the BKL**. Crashpad parks a thread
there with NO timeout, forever, so one thread hammering the global lock in
an infinite loop wedged the entire guest at ~8.7s (the serial log simply
stopped). Blocking syscalls must drop the BKL and hlt/yield cooperatively —
the same shape `sock_unix_recv` already uses. Never add a wait loop without
checking who holds the BKL across it.

FIX (slice 90): `programs/linux-crashpad` — a stub that actually performs
the handshake slice 76b decoded: parse `--initial-client-fd=N` off the raw
SysV stack, `recvmsg` the 40-byte request, reply 8 bytes (the kernel
attaches SCM_CREDENTIALS because the browser set SO_PASSCRED), and **loop
forever** so the endpoint never dies. `_start` is naked — reading `rsp`
from inside a normal C function is a lie the moment the compiler emits a
prologue. Verified on the WSL control BEFORE booting it (slice-82 method):
40-byte request received, 8-byte reply, `SCM_CREDENTIALS pid=16066`, and
the handler still alive afterwards.

HYPOTHESIS TESTED AND **NOT CONFIRMED** — AF_UNIX slot recycling. Slice 78
named it ("sock_alloc recycles pool indices IMMEDIATELY, peer_ip stores
index+1 with NO GENERATION COUNTER") and it fit the symptom exactly, so a
generation counter was implemented: `struct sock.gen`/`peer_gen`, every
lookup via `sock_peer_checked()`, `struct file.sock_gen` for descriptors,
`[uxgen]` logging each stale link caught. **Result: `[uxgen]` fired ZERO
times and the failure signature is byte-identical.** The aliasing was real
in theory but is not what breaks this run. **The code is KEPT** (it closes a
genuine hazard and costs nothing) but do NOT re-chase it as the cause, and
do not credit it with any behaviour change.

**MapWindow (op=8) HAS now been observed** (`req c=4 op=8 seq=253`, frame
resized to 799×599) — the first time in this arc.

Also fixed, each a real bug: two SMP lost-wakeups needing SEQ_CST (not
release/acquire) — `sched_finish_switch` vs `tg_vm_resume`, and
`sys_fork_share` vs `vfork_child_done`; group teardown calling
`close_all_fds` with the BKL dropped over non-atomic refcounts; an
unbounded `proc_wait_off_cpu` that could hang teardown forever.

**Extension surface is now MIT-SHM ONLY** (`-DXSRV_EXT_ALL` restores the
old set). We had been in a MIXED state — advertising RANDR/XFIXES/BIG-
REQUESTS — which the control never validated; the control proved chrome
maps with ZERO extensions.

**Two instrument lessons worth more than the fixes:**
- chromewin passed **two** `--vmodule=` flags; chrome keeps the last, so
  all the X11 narration added for this investigation was silently disabled.
- `[xsum]` printed **nothing for a whole 360s run** because it hung off
  `xserver_tick` → pid 0's idle_loop, and **pid 0 never runs under chrome
  load**. Never put a stall diagnostic on the idle path.
- The wait-graph is **blind to blocking recvmsg** (only epoll_wait/futex
  register), and `[uxstuck]`'s 5s threshold can no longer fire because our
  own 100ms idle-poke keeps every wait short — slice 88's fix blinded slice
  88's instrument. Use `[xsum] lastreq=<ms>`.

## 4. (SUPERSEDED — see §3b) original two candidate paths

### Path A (NOT VIABLE, see §3b): fix the `-smp 4` AP arc

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
