# Chromium bring-up: hypothesis ledger

Every hypothesis tested, what killed or confirmed it, and the evidence. Kept so
nobody (including me) re-runs a disproven idea, and so a confirmed-looking story
has to show its receipts.

**Status key:** CONFIRMED (fixed, verified) · DISPROVEN (measured, dead) ·
REAL-BUT-NOT-THE-CAUSE (genuine defect, did not explain the symptom) · OPEN.

---

## Blocker B — "Corruption detected in shared-memory segment" ×3

| # | Hypothesis | Status | Evidence |
|---|---|---|---|
| B1 | Stale/zero pages when the shm cache grows | DISPROVEN | Populated newly-allocated pages on every mapping; corruption stayed at 3. |
| B2 | The page double-free was corrupting segments | DISPROVEN | Double-free fixed; corruption still 3. |
| B3 | **Inode NUMBERS are recycled, so the inode-keyed shm cache aliased unrelated regions** | **CONFIRMED** | `[ino]` trace: inode 9 allocated **6×** in one boot; `[shm]` showed six processes "attaching" to ino 9 at **four different sizes**. Keyed on (ino, incarnation) → corruption **3 → 0**, stable across every run since. |

Lesson: the `[shm]` trace saying "attached" *looked* like success but meant
*aliased*. Verifying an instrument extends to verifying an identity.

---

## Blocker A — Mojo children never reply ("no connection")

| # | Hypothesis | Status | Evidence |
|---|---|---|---|
| A1 | Socket layer broken | DISPROVEN | Send and receive both demonstrably worked cross-process. |
| A2 | fd inheritance wrong | DISPROVEN | Child had the channel at the right fd, `FILE_KIND_SOCKET`. |
| A3 | SCM_RIGHTS broken | DISPROVEN | Verified live, both directions. |
| A4 | Shared-memory aliasing (B3) was starving Mojo | DISPROVEN | B3 fixed; the stall survived completely unchanged. |
| A5 | **`sendto()` on AF_UNIX was never implemented** | **CONFIRMED** | `[chan]` args+returns showed `sendto fd=5 len=80 -> -88` (ENOTSOCK). `lx_send` handled only UDP/TCP; `recvfrom` and `sendmsg` both had AF_UNIX arms, `sendto` did not. Fixed → every -88 gone, channel round-trips (`sendto 232 -> 232` / `recvmsg -> 232`), ops **11 → 234**. |

Lesson: the framing "browser never sends **or** child never receives" was wrong
on *both* counts — the child replied every time and **the kernel refused the
call**. The recent-syscall ring logs names only and structurally could not show
a failing return; that is why this survived three slices.

---

## GPU process dies at ~14 s

| # | Hypothesis | Status | Evidence |
|---|---|---|---|
| G1 | GPU watchdog self-terminates | DISPROVEN | Source: `DeliberatelyTerminateToRecoverFromHang` → `TerminateCurrentProcessImmediately` = `_exit()`, not a signal. |
| G2 | **The browser SIGKILLs its own GPU child** | **CONFIRMED** | `[sig] sig=9 -> pid=16 FROM pid=2 'chrome'`. Chrome's own recovery path; logs WARNING and respawns. **Not a tobyOS bug.** |
| G3 | It is purely an emulation-speed timeout artifact | LARGELY DISPROVEN | Still happens under WHPX at ~4.5× TCG speed. |

Trap recorded: the harness DOES pass `--disable-gpu`; the swiftshader flags in
`[execve-argv]` belong to a **utility** child, not the browser.

---

## Native browser could not start (YouTube test)

| # | Hypothesis | Status | Evidence |
|---|---|---|---|
| N1 | My struct-layout changes broke it | DISPROVEN | Clean rebuild reproduced identically. |
| N2 | Partial-build struct mismatch | DISPROVEN | Same after `make clean`. |
| N3 | **Chrome's 262 MB payload in the initrd starved guest RAM** | **CONFIRMED** | `[pmm] managed: 34 MiB` vs 511 MiB historically; initrd was 387 MB with 413 chrome files. Parked payload → 337 MiB, `gui_browser ALIVE`. |

Result: YouTube renders its pre-hydration shell only (`Done - 15 links`), no
content grid, no video — matching the documented architectural ceiling.

---

## WHPX / real-hardware fidelity

| # | Hypothesis | Status | Evidence |
|---|---|---|---|
| W1 | CPU-model divergence under WHPX | DISPROVEN | Identical #GP with `-cpu qemu64`. |
| W2 | SMP/AP GDT or TSS bring-up | DISPROVEN | Identical with `-smp 1`. |
| W3 | **STAR[63:48] = 0x10 → SYSRET leaves SS.RPL = 0** | **CONFIRMED** | #GP error code `0x18` named GDT index 3; dumped iret frame read `cs=0x23 ss=0x18`. SYSRET forces CS.RPL but **not** SS.RPL. Base → 0x13. WHPX boots clean (0 panics); TCG unchanged; stock boot clean. **Every ring-3 return since the first syscall was riding an illegal frame.** Strong candidate for the open EliteDesk real-HW divergences. |

---

## The system wedge at ~14 s (current front)

| # | Hypothesis | Status | Evidence |
|---|---|---|---|
| D1 | Renderer spins in user code | DISPROVEN | Ring-3 profiler: only 1–4 samples per 3 s interval — barely any user time. |
| D2 | `clock_gettime` spin loop starves the renderer | DISPROVEN | Per-caller breakdown: renderer's mix is `mprotect`/`openat`/`fstat`/`rt_sigaction` — healthy init; clock calls spread across 5 tids (normal TimeTicks). **I read an aggregate before decomposing it.** |
| D3 | `clock_gettime` ignores clock_id + 1 ms granularity | REAL-BUT-NOT-THE-CAUSE | Genuine bug — all clocks returned ms-since-boot, so everything believed it was Jan 1 1970. Fixed (chrome now reports the true date). **Did not change the wedge.** |
| D4 | The renderer just needs more time | DISPROVEN | 25 min wall → **0.8 s** of guest progress. Not slow: wedged. |
| D5 | The wedge is a WHPX artifact (`MSI ... lost`) | DISPROVEN | TCG wedges identically, same signature. |
| D6 | `sched_enqueue` self-cycles the queue TAIL | REAL-BUT-NOT-THE-CAUSE | Guard was `next_ready != NULL`, but the tail has `next_ready == NULL` → re-enqueuing the tail sets `p->next_ready = p`. Genuine latent bug, fixed with an `on_rq` flag under the lock. **But children ran fine before the fix (37) and after (35): not this hang.** |
| D7 | `proc_reap` zeroes a slot some list still points at | DISPROVEN | `[reap!]` audit walked every ready queue before each `memset`: **0 hits** in a full run. |
| D8 | The parent never returns from fork | DISPROVEN | 4 forks, **4** clone-returns. First attempt showed 0 returns only because I instrumented `LX_fork` while chrome forks via `clone()` — the count mismatch caught my own instrument error. |
| D9 | Missed futex wakeup | LARGELY DISPROVEN | Address-matched `[futex]` instrument: 75 WAIT / 181 WAKE, and the waits observed around the stop **return 0** (i.e. they were woken). Note the instrument's blind spot: it logs *after* the call, so a permanently-blocked wait is invisible to it — but nothing points to one. Long 6–9 s waits are chrome's idle thread-pool workers parking on purpose. |
| D10 | Serial TX ring fills → output dropped, so the "wedge" is log loss not a hang | DISPROVEN | Added a drop counter and grew the ring 8 KiB → 256 KiB: **`logdrop=0`**, run still stops at ~13.9 s with 4 heartbeats. Not one byte was ever lost. **Also retracted the reasoning:** the "more tracing → earlier stop" trend was confounded — the early 22–24 s runs predate the `sendto` fix, when chrome stalled instead of respawning GPU processes. Different workload, not different log volume. |

| D11 | The guest HALTS rather than hangs | CONFIRMED (and it is worse than a halt) | `timeout 400` returned **rc=0**: QEMU exited on its own. A hung guest keeps QEMU alive. With `-no-reboot` a **triple fault** makes QEMU exit — which is what it is. |
| D12 | **`proc_context_switch` loads a CR3 that does not map the kernel** | **CONFIRMED — root cause of the "wedge"** | `-d int,cpu_reset` caught the chain: `v=0e e=0018 cpl=0 IP=ffffffff80151154 CR2=ffffffff80151154` (page fault where **CR2 == IP**, instruction-fetch, present-bit clear) → `v=08` (#DF) → `Triple fault`. Symbolized: `0x...151154` is the instruction **immediately after `mov %rdx,%cr3`** in `proc_context_switch`. Switching into a dead address space unmaps the kernel, so the next fetch faults and no handler can run. |

### Why every earlier reading was wrong

There was never a hang, a scheduler cycle, a futex deadlock, or log loss. The
machine **triple-faults and dies instantly**. No panic could ever print, because
the panic handler was unmapped along with the rest of the kernel. Everything
that looked like a "wedge" was simply the machine being gone.

Corollary for future work: **a silent stop with QEMU exiting rc=0 under
`-no-reboot` means a triple fault.** Reach for `-d int,cpu_reset` FIRST; it took
one run to produce the exact faulting instruction after a long detour through
scheduler, futex and serial theories.

### RESOLVED — the triple fault was a reaped proc left in the run queue

Two root causes in the same family, both fixed:

- **PML4 use-after-free** (commit f1883bb): `proc_reap` destroyed a thread
  group's address space while a member was still alive. Fixed by handing PML4
  ownership to a surviving member; the last one reaped tears it down.
- **Reaped-slot-still-in-runqueue** (commit 91f43a9): the thread-group-exit
  loop and `proc_reap` freed a proc's kernel stack and recycled its slot
  WITHOUT unlinking it from the ready queue. The scheduler then popped it and
  switched in with `kstack_top == NULL` → triple fault in `syscall_entry`.
  Fixed with `sched_dequeue()` before every teardown.

**Result: the ~14 s triple fault is gone. Chrome runs 571 s of guest time
(40×), 190 heartbeats, zero faults, and settles into a stable ~31-process tree
idling in epoll_wait/futex.** The GPU respawn loop stopped (1 crash, not
endless). Guards kept permanently: `vmm_pml4_is_live` + kstack validation at the
switch, so this whole class can never again present as a silent death.

### New front (chrome now runs deep enough to show real IPC errors)

Still no DOM, but the failures are now specific and downstream:
- `VALIDATION_ERROR_MESSAGE_HEADER_INVALID_FLAGS`
  (`mojo/public/cpp/bindings/lib/validation_errors.cc:136`) — a Mojo message
  arrived with a malformed header. Data-integrity bug in the IPC path.
- `Network service crashed or was terminated, restarting service.`

Only syscalls 93 (fchown, benign) and 444 (landlock, a probe) are unhandled, so
neither is a missing syscall. Next: chase the Mojo header validation — decode
what flags chrome expects vs what the peer received.

### DOM front — what a full 571 s run now shows (measured, machine survives)

| Fact | Value |
|---|---|
| renderer spawns in 571 s | **exactly 1** (pid 33) |
| renderer threads created | 5 (was 0 when the box died at 14 s) |
| renderer CPU before death | 6.7 s — real work, past early init |
| renderer fate | **SIGKILLed by the browser (pid 2) at 23 s, never respawned** |
| browser after 23 s | alive but idle to 571 s; never retries a renderer |
| "no connection" children | 1 (pid 18, clean exit 0) |

Because `--dump-dom` needs the FIRST renderer to finish loading
`chrome://headless/headless_command.html` before `executeCommands()` ever runs,
that renderer dying at 23 s is the prime DOM suspect.

Source read (`mojo/public/cpp/bindings/lib/message_header_validator.cc:65`): the
`INVALID_FLAGS` error means a header arrived with **both** `kFlagExpectsResponse`
and `kFlagIsResponse` set (`flags & 3 == 3`) — mutually exclusive. That is
**data corruption of a single message**, not a logic error.

| # | Hypothesis | Status | Evidence |
|---|---|---|---|
| M1 | AF_UNIX delivers datagram/message boundaries where Mojo's channel expects a BYTE STREAM, so the reader's framing drifts and it misreads the next header's flags | OPEN — prime lead | Handoff flagged it: "tobyOS's AF_UNIX is message-queued with a tail_off partial-read path — Mojo's channel framing may want byte-stream semantics." `flags & 3 == 3` is exactly what reading a header at the wrong offset looks like. |
| M2 | Browser kills the renderer because of a bad Mojo message on the renderer pipe (`bad_message`) | OPEN | Validation error and renderer SIGKILL both cluster at ~23 s; not yet causally linked. |
| M3 | Renderer is killed for hanging (never completes chrome://headless load), unrelated to the Mojo corruption | LIKELY (mechanism found) | See the traced chain below. |

### The renderer chain, fully traced (logs/amap.log)

Now that the machine survives, the renderer's whole life is visible for the
first time:

1. Renderer (pid 33) spawns, does V8-cage setup — a **bounded** burst of huge
   32 GiB (`0x800000000`) mmap/munmap reserve-and-trim pairs (align-by-
   over-allocate; our mmap is only 4 KiB-granular). **Completes** by ~13 s.
   *(This retires the "mmap/munmap churn is the blocker" reading from slice 20 —
   it is expensive but it finishes.)*
2. Renderer sends ONE 80-byte accept on its primary channel (`sendto fd=5 -> 80`
   at 13.577 s). The browser thread (pid 8) **receives it** (`recvmsg -> 80`)
   and immediately **floods setup replies back** — dozens of messages
   (312/1120/10488 B …).
3. **The renderer never reads any of them.** Its channel shows exactly one send
   and zero receives for its entire life. In its final ~5 s it is **spinning in
   user code** (profiler: 54 then 37 ring-3 samples/interval vs 1–4 when idle),
   syscalls dominated by a tight `gettid()` loop.
4. Browser SIGKILLs the unresponsive renderer at ~18–23 s. Never respawns.

| # | Hypothesis | Status | Evidence |
|---|---|---|---|
| R1 | The `gettid()` loop is a single stuck spinlock in the renderer | DISPROVEN | Profiler hot rips are VARIED (6 distinct addresses, hits=1) and gettid is heavy across MANY pids (33,18,2,16). Not one lock — the renderer executes varied code (progressing), calling gettid constantly. |
| R2 | `gettid` returns a wrong/colliding value, breaking a lock | DISPROVEN (for the obvious form) | Each tobyOS thread is its own proc with a unique slot pid; `gettid = current_proc()->pid` is unique + stable per thread. |
| R3 | **`PlatformThread::CurrentId()`'s thread-local cache is defeated**, so every process syscalls gettid constantly; combined with ~4–18% emulation speed the renderer can't finish init before the browser's kill timeout | OPEN — prime lead | gettid is called heavily in EVERY chrome process, which only happens if the TLS-cached CurrentId misses. Would explain chrome-wide slowness + the renderer missing a fixed browser-side deadline. |
| R4 | Renderer init is genuinely stuck (not just slow) somewhere before the Mojo message loop | OPEN | Varied rips = progress, but it never reaches "read my channel." Distinguish from R3 by extending/removing the browser's renderer-kill timeout: if the renderer then finishes and reads its channel, it was slow (R3); if it still never reads, it is stuck (R4). |

**Next concrete step:** determine slow-vs-stuck. Either extend the browser's
renderer deadline (a chrome flag) so a healthy-but-slow renderer can finish, or
verify TLS/`set_tid_address` + arch_prctl FS-base are correct so CurrentId can
cache. If gettid caching is fixed and the renderer still never reads its channel,
the front moves inside Blink/V8 renderer init.

### SLOW-vs-STUCK — ANSWERED: STUCK (slice 25)

Probe: `--timeout=600000` + `--disable-hang-monitor`, run under both WHPX and TCG.

| # | Hypothesis | Status | Evidence |
|---|---|---|---|
| R3 | Renderer is merely SLOW; the browser kills it too early | DISPROVEN | Under TCG the renderer is STILL SIGKILLed at ~18 s WITH `--disable-hang-monitor`, and STILL sends 1 / receives 0. More time (10-min timeout) and more speed (WHPX) never let it complete the handshake. |
| R4 | **Renderer is genuinely STUCK in user-space init between "bootstrap accept sent" and "message loop entered"** | **CONFIRMED** | It sends its one 80-byte accept, the browser floods setup replies, the renderer never reads them, spins in user code, and no amount of time/speed/kill-suppression changes that. The browser kills it after ~15 s of an incomplete Mojo bootstrap. |

Two traps avoided this slice:
- WHPX showed the renderer exiting **clean (code 0)** with NO SIGKILL and a
  7×-repeating "no connection" + network-service-crash loop. That was a **WHPX
  artifact** (its `MSI ... lost` interrupt drops), NOT the flag effect — TCG with
  identical flags shows the SIGKILL and only 1× no-connection. Don't draw
  behavioural conclusions from WHPX where it diverges from TCG.
- The `copy_to_user -> EFAULT` burst before the WHPX exit looked like a CoW bug,
  but reading the code first showed the path already calls the CoW fault handler,
  and slice-19 documents these exact EFAULTs as **benign** (chrome's
  `base::ProtectedMemory` writes a RO page via syscall *expecting* EFAULT). Not a
  bug. 0 occurrences under TCG.

**The front is now firmly INSIDE the renderer's user-space init** (Blink/V8/base),
between sending the Mojo bootstrap accept and entering the IPC message loop. This
is the outcome the tail-prompt predicted: with the process model cleared, the
renderer's failure to complete a load is its own problem. Prime lead remains the
gettid storm → `PlatformThread::CurrentId()` TLS cache / FS-base: verify the
renderer's thread-local storage is correct, since broken TLS would both explain
constant gettid syscalls AND could hang thread-local-dependent init.

### CORRECTION (slice 26) — the renderer's Mojo WORKS; the IO threads stall in epoll_wait

I made several wrong sub-conclusions before the WAIT-GRAPH (not the syscall ring)
gave the true picture. Recording them so they are not repeated:

- WRONG: "renderer sends 1, receives 0, never talks Mojo." That was only the
  MAIN thread (pid 33). The renderer's **bootstrap IO thread (tid 37) has a full
  healthy bidirectional Mojo conversation** — ~40 ops, correct byte counts both
  ways, 13.5–15.9 s.
- WRONG: "main thread stuck in a tight spinlock." Its hot rips span ~29 MB of
  code + libraries + ld.so — genuine heavy V8/Blink work, progressing.
- WRONG: "no renderer thread uses epoll." The syscall RING only shows COMPLETED
  syscalls; a thread BLOCKED in epoll_wait never returns, so it is invisible
  there. The **wait-graph** shows it.

**The actual chain:**
1. Renderer bootstraps Mojo via tid 37 (blocking `recvmsg` on fd 5) — healthy
   two-way conversation until 15.9 s.
2. tid 37 finishes and **exits normally** at 16 s (clean, clear_child_tid wake).
3. The persistent IO threads take over with **epoll**: at death, **tid 34 is in
   `epoll_wait(0x6)` for 4.2 s and tid 35 in `epoll_wait(0xb)` for 4.6 s** —
   blocked, never waking.
4. So after 16 s nothing services the renderer's channel; the browser's messages
   go unanswered and the browser SIGKILLs the "hung" renderer at ~18.6 s
   (robust to `--disable-hang-monitor` + `--timeout=600000`).

`file_poll_ready` DOES report an AF_UNIX socket readable on `sock->count > 0`,
and `lx_epoll_wait` pumps `net_poll()` and re-scans — so the readiness logic
looks right in isolation. The open question is why tids 34/35 never wake:

| # | Hypothesis | Status | Test |
|---|---|---|---|
| E1 | The channel fd (5) is not registered in the epoll instance (0x6/0xb) those threads wait on | OPEN | Instrument epoll_wait to dump its registered fds + each fd's poll-readiness when blocked > N ms. |
| E2 | The browser's post-16 s messages never arrive at the renderer's fd-5 socket (count stays 0) — they go to a different pipe/transport | OPEN | Same instrument: is any registered socket's `count > 0` while epoll_wait sits blocked? Cross-check with `[unix]` peer trace of pid 2's sends. |
| E3 | epoll_wait wakes but `file_poll_ready` misreports the specific fd, so it re-sleeps | LESS LIKELY | The AF_UNIX arm returns POLLIN on count>0; would need count>0 AND no POLLIN. |

**Next step:** the epoll_wait blocked-fd dump (E1/E2 discriminator). This is the
first concrete, tobyOS-side, fixable locus for the DOM since the process-model
work — the renderer is healthy right up to an epoll readiness/registration gap.

### LOCALIZED (slice 26 cont.) — the IO threads wait on an EVENTFD that never fires

`[epreg]` (epoll_ctl registration trace) shows what chrome's IO threads poll on:
**32 registrations are kind=12 EVENTFD, 20 are kind=5 SOCKET.** The renderer's
hanging IO threads specifically:
- tid 34 → `epoll_wait(epfd=6)`, and epfd 6 has ONLY `fd=10 kind=12` (eventfd).
- tid 35 → `epoll_wait(epfd=11)`, epfd 11 has ONLY `fd=12 kind=12` (eventfd).

(That the hanging epolls contain no socket is also why the earlier `saw_sock`
dump got 0 hits — those threads never take the socket-pump path.)

**So the post-bootstrap Mojo transport is shared-memory + EVENTFD notification:**
the bootstrap socket handshake works (tid 37), then chrome moves the live channel
to shared memory and signals "message ready" by writing an eventfd the peer
epolls. The renderer's IO threads block on that eventfd and **it is never
signaled from the browser side**, so they never wake, never service the channel,
and the browser kills the renderer at ~18 s.

`struct eventfd` IS shared across fork AND SCM_RIGHTS (`file_clone` shares the
same struct, refcounted), so a cross-process write *should* be visible — which
means the bug is specific, not "eventfd isn't shared at all". Candidates:

| # | Hypothesis | Status | Test |
|---|---|---|---|
| V1 | The browser never WRITES the notification eventfd (its signal path uses something tobyOS routes elsewhere) | OPEN — prime | Trace eventfd_write: does pid 2 ever write an eventfd the renderer polls? |
| V2 | Browser writes ITS eventfd but the renderer polls a DIFFERENT (non-shared) instance — the SCM_RIGHTS/fork pairing is crossed | OPEN | Correlate the eventfd object identity (struct pointer / a stamped id) across the two processes. |
| V3 | Write propagates (count>0) but epoll_wait doesn't re-check that eventfd / eventfd_pollin misreads | LESS LIKELY | `file_poll_ready` EVENTFD arm calls `eventfd_pollin`; unit-check count>0 ⇒ POLLIN. |

This is the tightest, most concrete DOM locus reached all session: the renderer
is fully healthy through Mojo bootstrap and Blink/V8 init, and stalls only at the
eventfd wakeup of the shared-memory message transport. Committable `[epreg]`
instrument added.

### CORRECTION (slice 27) — eventfd cross-process signaling mostly WORKS; V1 is too broad

Stamped each `struct eventfd` with a unique id and traced create / SCM-pass /
write / poll across processes (`[efd]` + `[scm]` id + `[epreg]` id). Tracing the
five eventfds the renderer group polls:

| efd_id | created by | written by | result |
|---|---|---|---|
| 27 | renderer | **pid 17 (cross-process)** | works |
| 12 | renderer | **pid 17 (cross-process)** | works |
| 13 | renderer | renderer's own group | works |
| 15 | renderer | renderer's own group | works |
| **14** | **pid 36 (renderer)** | **NOBODY, 0 writes** | **hangs** |

So **V1 ("browser never writes the notification eventfd") is DISPROVEN as a
blanket claim** — cross-process eventfd signaling demonstrably works (pid 17
signals efd 27 & 12; earlier run pid 27 signaled efd 23). The failure is narrow
and specific: **efd 14 is created and polled by the renderer but signaled by no
one.** None of these were SCM-passed, so they are INTRA-renderer notifications —
efd 13 & 15 get signaled by sibling threads, efd 14 never does. That means a
renderer thread that *should* write efd 14 is itself blocked. This is threads
inside the renderer waiting on each other, not a kernel eventfd transport bug.

**Honest reassessment:** 4 of the renderer's 5 polled eventfds DO get signaled,
so the renderer is NOT globally stalled on eventfd delivery — it makes progress
on most channels. Whether the single orphaned efd 14 is the load-blocking one, or
a side effect of the main thread being busy elsewhere, is NOT established. The
kernel-trace approach has localized the transport (eventfd) and proven it mostly
works, but the remaining fault is an intra-renderer thread dependency inside
stripped Blink/V8 code, where kernel traces can localize but not name the cause.

**Method note for next agent:** this front needs chrome-side visibility, not more
kernel archaeology. Options: (a) read `RenderProcessHostImpl` source for the
exact browser-side renderer-kill trigger (prime-directive move, under-used this
session); (b) a build of `chrome-headless-shell` with logging, or a lighter
headless target; (c) accept the renderer/Blink tier as the documented frontier
and bank the ~10 kernel bugs fixed this session. Instruments `[efd]`/`[epreg]`
committed.

### SOURCE READ (slice 27) — the renderer kill is `ShutdownForBadMessage`, and the run is NON-DETERMINISTIC

Read `content/browser/renderer_host/render_process_host_impl.cc`
(151.0.7922.34). The forceful renderer SIGKILL path is
`RenderProcessHostImpl::ShutdownForBadMessage()` →
`Shutdown(RESULT_CODE_KILLED_BAD_MESSAGE)` → `child_process_launcher_->Terminate`
— fired when the browser receives an **illegal Mojo message** from the renderer.
It is gated only by `--disable-kill-after-bad-ipc`, NOT by `--disable-hang-monitor`
or `--timeout`, which is exactly why the ~18 s kill was robust to both.

This ties back to the `VALIDATION_ERROR_MESSAGE_HEADER_INVALID_FLAGS`
(`flags & 3 == 3`, mutually-exclusive request/response bits both set) seen early:
a message arriving with a corrupt header IS an illegal message → bad-message kill.

**But the behaviour is NON-DETERMINISTIC across runs — the decisive new fact:**

| run | renderer killed? | validation error? | DOM |
|---|---|---|---|
| amap.log | YES (pid 33) | 0 | no |
| dq.log | YES (pid 33) | 1 | no |
| efd3.log | **NO** (pid 32 survived; only GPU-type pids 17/19 killed) | 0 | no |

So: (1) the renderer is NOT always killed; (2) when it survives, there is STILL
no DOM. The bad-message kill is therefore ONE manifestation, not the whole story
— there is an underlying "renderer never completes the chrome://headless load"
that persists even when the renderer lives, plus an OCCASIONAL, timing-dependent
Mojo message corruption that sometimes trips the bad-message kill.

**Net root-cause shape:** a race/timing-dependent data-integrity issue in the
Mojo transport (socket framing or shared-memory coherence) that corrupts a
message header intermittently, layered on a renderer that is slow/incomplete in
Blink/V8 init. Both live where kernel traces can localize but not name. The
`flags & 3 == 3` corruption is the single most concrete, reproducible-in-1-of-N
artifact; the next productive instrument is a Mojo-header integrity check at the
socket boundary (log sender-side flags vs receiver-side flags for channel
messages) to prove transport corruption vs higher-layer, but it needs the node-
channel framing decoded first.

### (historical) Open: which CR3, and why is it dead

Prime suspect is a **PML4 use-after-free**. `proc_reap` calls
`vmm_destroy_user_pml4(p->cr3)` when `owns_pml4 && !is_thread`. If any THREAD of
that group is still alive (threads share the leader's `cr3` and have
`owns_pml4 == false`), destroying the leader's tables leaves those threads
pointing at freed page tables — and the first switch into one triple-faults.
Chrome's GPU respawn loop (SIGKILL the process, reap it, immediately fork a
replacement into the same slot) is exactly the churn that would expose it.

Next step: assert in `proc_context_switch`'s caller that the target `cr3` is
live, and in `proc_reap` that no other proc shares the `cr3` about to be
destroyed.

### What is established about the wedge

Not fork. The parent forks the replacement GPU process, returns, creates its
shared memory, and sends the 184-byte Mojo invitation — *then* everything stops:

```
[14440] fork returns 17
[14441] shm region created
[14442] scm sendmsg passing 1 fd
[14442] sendmsg fd=64 -> 184      <- silence, heartbeat dead
```

Silent: no crash, no panic, no exception. Reproducible on **both** TCG and WHPX.

---

## Standing lessons

- **Decompose before concluding.** D2 and D8 both came from reading a total
  instead of a per-caller/per-path breakdown. Both were one command from truth.
- **Put counters on instruments.** The 4-vs-0 fork mismatch exposed a bad
  instrument rather than producing a false finding.
- **Check the run actually got there.** Counts for anything deadline-driven
  (`with no connection` fires at 15 s) are meaningless without the last
  `[N ms]`. Chrome now runs at ~4 % real-time under TCG.
- **A "verified working" claim is only verified for the path that exercised it.**

---

## SLICE 28 — a real transport bug fixed; renderer now deadlocks on an intra-renderer wakeup

**FOUND & FIXED (commit f10e66b): the AF_UNIX ring DROPPED messages.** The
`[unixdrop]` trace confirmed the 8-deep socket ring dropped the oldest message on
overflow -- 80+ drops/run on every Mojo socketpair. On a reliable stream a
dropped message breaks framing -> the `flags&3==3` corruption + non-deterministic
renderer kills. Fix: ring 8->64, full ring returns EAGAIN (backpressure) instead
of dropping, and `file_poll_ready` reports POLLOUT only when the peer ring has
room. Measured: **drops 80 -> 0; channel ops 230 -> 401; the RENDERER now
SURVIVES (was killed); the error moved from INVALID_FLAGS on the renderer to a
later ILLEGAL_MEMORY_RANGE on the network service.** defboot still clean.

**The socket is byte-perfect now.** A per-dgram FNV checksum (`[sockchk]`),
stamped at enqueue and verified at dequeue: **0 mismatches**. So the residual
ILLEGAL_MEMORY_RANGE is NOT socket corruption -- it is above the socket (ipcz
shared-memory message data, uncovered by the checksum) or chrome-internal, and it
is on the network service, which the internal chrome://headless page does NOT
need.

**Current DOM blocker = the renderer DEADLOCKS.** With the drop fix the renderer
survives, runs ~75 channel ops until ~17 s, then EVERY thread blocks for the rest
of the run (wait-graph at 225 s): main thread pid 33 in **futex 216 s**; IO thread
pid 37 in **epoll_wait 135 s on efd_id=14**; pid 36 in epoll_wait on efd 13 (13 IS
signaled, 14 is NOT); others futex 208-216 s.

**efd 14 is created by renderer thread pid 37, registered in its own epoll, NEVER
SCM-passed, and written by NO ONE.** An intra-renderer wakeup a sibling thread
should post and doesn't. With the main thread's 216 s futex wait this is a
circular deadlock inside the renderer's threading.

| # | Hypothesis | Status | Next |
|---|---|---|---|
| DL1 | Chrome-internal deadlock (waker legitimately blocked, no wake issued) | OPEN | plausible |
| DL2 | **tobyOS futex/eventfd WAKE-DELIVERY bug** (a wake IS issued but never reaches the waiter) | OPEN -- NOT ruled out | the `[futex]` trace capped (400 lines / 24.8 s) while the deadlock runs to 225 s, so wake-after-block is unobservable. Need an UNCAPPED instrument keyed on pid 33's futex addr + efd 14, full run. |

**Decisive next fork:** DL2 would be a tobyOS-fixable wakeup-delivery bug and
likely THE DOM blocker; DL1 puts the front inside chrome's renderer threading.
Distinguish with an uncapped wake-targeting-the-stuck-addresses trace.

---

## SLICE 29 — DL2 CONFIRMED: timed FUTEX_WAIT can't be woken by FUTEX_WAKE (fix reverted, unvalidated)

Built an uncapped, per-address FUTEX_WAKE tracker (count + last-wake timestamp +
"is the waiter on the futex list") and dumped it in the wait-graph. **Decisive:**
threads showed `wake_after_block_ms >= 0` with `onlist=0` -- a FUTEX_WAKE landed
on their address AFTER they blocked, yet they stayed blocked 140-220 s. So **DL2
is real: a wake was issued and not delivered.**

Root cause READ FROM THE CODE: `futex()` had two wait paths. UNTIMED
(`deadline==0`) registered on the wait list, so FUTEX_WAKE could wake it. TIMED
(`deadline!=0`, i.e. glibc `pthread_cond_timedwait`/`mutex_timedlock`, which
chrome uses pervasively) did **poll-with-idle**: it watched `*uaddr` for a value
change and was **NOT on the wait list** -- so **FUTEX_WAKE could not wake it at
all**, only a `*uaddr` change could. glibc wakes condvar/mutex waiters with
FUTEX_WAKE and does not always change the word the waiter parked on, so those
wakeups were silently lost -> the renderer's threads deadlocked. The in-code
comment even stated the flawed assumption ("the waker changes *uaddr before
FUTEX_WAKE, which we detect on recheck").

**Fix attempted and REVERTED (not shipped):** made timed waits also block on the
wait list, added a 10 ms `futex_expire_timeouts()` sweep for deadlines, and
`futex_forget_proc()` to unlink an exiting thread from futex lists (the last was
necessary and correct -- without it the sweep woke a reaped proc and my
DEAD-cr3 guard fired, exactly as designed). Result: defboot clean, no panic,
BUT **no DOM, the renderer froze even earlier (~9.6 s vs ~17 s), and the stuck
waiters still read `onlist=0`** -- meaning the fix did not put them on the list
as intended, or the instrument mis-reports. Rather than ship an unvalidated,
possibly-regressing change to a CORE primitive every threaded program depends on,
I reverted to the last validated commit.

**Status:** the DIAGNOSIS is solid and confirmed (timed futex waits are not
wakeable by FUTEX_WAKE -- a genuine tobyOS bug and a real contributor to the
renderer deadlock). The FIX needs a careful, separately-validated
implementation: (1) understand why the list-registration read back as
`onlist=0` (bug in the merge, or in `futex_num_waiters`); (2) confirm timed
waiters are woken by a matching FUTEX_WAKE with a targeted unit test before
trusting it under chrome; (3) keep `futex_forget_proc` in teardown regardless,
it is a real latent-bug fix. Do NOT ship a futex change without a green
defboot AND a targeted wake test AND a chrome run that shows the renderer
progressing past the ~17 s deadlock.

---

## SLICE 30 — DL2 FIXED and validated: timed FUTEX_WAIT now wakeable by FUTEX_WAKE

**Committed 7da9d16.** The confirmed DL2 bug (timed FUTEX_WAIT polled *uaddr and
was not on the wait list, so FUTEX_WAKE was silently lost) is fixed: both timed
and untimed waiters block ON the list; FUTEX_WAKE wakes them; a deadline sweep
(`futex_expire_timeouts`) handles genuine timeouts; `futex_forget_proc` unlinks
exiting threads.

**Test-driven (as the ledger required).** Added `/bin/linux-futex` -- clones a
thread, does a TIMED FUTEX_WAIT (10 s deadline) on an UNCHANGED word, then
FUTEX_WAKEs it. Boot harness prints `[FUTEXTEST] VERDICT`. Rigorous FAIL->PASS:
**old code = FAIL (exit 1, lost wakeup); fixed code = PASS (exit 3, woken by
FUTEX_WAKE).** Keep this test green for any future futex change.

Two traps recorded:
- The test harness itself had a bug: `clone` saved the thread-entry fn in **r11,
  which `syscall` clobbers**, so the child jumped to garbage and never ran --
  which looked like the futex FAILing. Use a callee-saved reg (r12) for anything
  that must survive a syscall. (This is why an earlier "reproduced in isolation"
  was a false positive.)
- The deadline sweep MUST NOT run in the timer IRQ (`sched_tick`): it faulted
  `current_proc()`/`smp_this_cpu()` under chrome (2 kernel panics). Moving it to
  `sched_yield`'s slow path (normal kernel context) removed the panics entirely.

Validation: unit test PASS; defboot clean to login+GUI; **chrome runs 276 s with
0 panics / 0 DEAD-cr3.**

### STILL NO DOM -- the remaining blocker is the eventfd, not the futex

The futex fix did NOT produce the DOM. The renderer still deadlocks, now clearly
on the SEPARATE **intra-renderer eventfd** path already documented (slice 27):
`efd 14` is created by a renderer thread, registered in its own epoll, **never
SCM-passed and written by NO ONE**, so the IO thread parked in `epoll_wait` on it
never wakes. A sibling thread that SHOULD signal efd 14 does not -- and now that
the futex lost-wakeup is fixed, THAT sibling's failure to signal is the live
lead, not a futex artifact.

**Next front (for whoever picks this up):** apply the SAME test-driven method to
the eventfd/epoll path. Build a `/bin/linux-eventfd` unit test: thread A parks in
`epoll_wait` on an eventfd; thread B writes the eventfd; assert A wakes. If it
FAILs in isolation, that is the DOM bug and it is tobyOS-fixable. If it PASSes,
the renderer's efd-14 signal genuinely never happens in chrome's logic and the
front moves inside Blink. Either way: **prove the primitive in isolation before
trusting a chrome run** -- this session's biggest time sinks were all chrome-only
inferences that a 5 s unit test would have settled.

### Session tally of shipped, validated fixes (all merged path or committed)
shm inode-incarnation aliasing; AF_UNIX sendto; SYSRET STAR base; initrd RAM;
PML4 use-after-free; reaped-slot-in-runqueue triple fault; NULL-kstack guard;
clock_gettime clock-id/ns; AF_UNIX message-drop -> backpressure; timed
FUTEX_WAIT wakeable by FUTEX_WAKE. ~11 real kernel bugs. Chrome went from
triple-faulting at 14 s to running its full multi-process engine 270 s+ clean
with working Mojo. DOM remains blocked on the eventfd path above.

---

## SLICE 31 — eventfd primitive PROVEN correct; futex ops complete; deadlock is chrome-internal

Per the ledger's own prescription, tested the two suspect primitives in ISOLATION
before trusting chrome:

- **`/bin/linux-eventfd` (committed be3dff2): PASS.** Thread A parks in epoll_wait
  on an eventfd; another thread writes it; A wakes promptly with EPOLLIN. So the
  tobyOS eventfd/epoll wakeup is CORRECT -- the renderer's efd-14 park is NOT a
  tobyOS bug; efd 14 is simply never *written* by chrome.
- **Futex ops: complete for chrome.** A `[futexop]` trace on the -EINVAL path saw
  **0** unhandled ops -- chrome uses only WAIT/WAKE/WAIT_BITSET/WAKE_BITSET, all
  handled. Missing CMP_REQUEUE/WAKE_OP/PI is NOT the blocker.

**So both wakeup primitives are correct, and chrome uses no unhandled futex op --
yet the renderer still deadlocks** (post-futex-fix run: main thread + workers
futex-blocked 200-270 s, then the browser SIGKILLs the renderer). The remaining
deadlock is therefore a chrome-internal CIRCULAR WAIT: thread A waits a
FUTEX_WAKE that thread B should issue, but B is itself blocked -- a cycle. Chrome
does not deadlock on real Linux, so some subtler tobyOS behaviour steers its
threads into the cycle, but it is NOT a broken WAIT/WAKE/eventfd primitive.

### Honest position on the DOM

Every concretely-identifiable, independently-validatable tobyOS bug on the path
has been found and fixed (~11). The residual blocker is a chrome-internal thread
deadlock with no broken primitive under it -- exactly the tier where kernel
traces localize but cannot name the cause, and chrome's own logging is compiled
out. Cracking it likely needs chrome-side visibility (a logging-enabled
chrome-headless-shell build, or driving DevTools over --remote-debugging-pipe to
see which IPC the renderer is stuck awaiting), NOT more kernel archaeology.

**Method that worked and should continue:** prove each primitive in isolation
with a tiny FAIL/PASS unit test (linux-futex, linux-eventfd) before ever
attributing a chrome hang to it. Every multi-hour detour this arc was a
chrome-only inference a 5-second unit test would have settled. The next suspect
primitive to unit-test if pursued: the exact glibc condvar wait/signal SEQUENCE
(a 3-thread cond_wait/cond_signal test), since a subtle ordering gap there could
produce the observed circular wait even with WAIT/WAKE individually correct.

---

## SLICE 32 — chrome-side visibility attempt: kernel user-stack walk of deadlocked threads

Built `waitt_dump_stacks()` (syscall.c): for each thread in the blocked-in-a-
syscall table, read its saved user context from the `syscall_regs` frame at
`kstack_top`, then walk its USER stack via `get_pte(p->cr3, ...)` + HHDM (reads
ANOTHER proc's memory from an unrelated context) and print return-address-looking
qwords in the code region `0x1000_0000_0000+`. Chrome is `-fomit-frame-pointer`,
so this raw-scans rather than frame-walks. Committable, CHROMIUM_BOOT-gated,
behaviour-neutral.

**It works** -- it produced real call chains for live threads (browser + IO
threads in epoll_wait; a futex-blocked worker). But it did NOT crack the DOM, for
two concrete barriers, both recorded so the next agent doesn't rediscover them:

1. **The renderer is SIGKILLed (~11 s) before the deadlock can be dumped.** The
   kill is `ShutdownForBadMessage`. Adding `--disable-kill-after-bad-ipc` did NOT
   keep it alive (a renderer thread was still killed -- a DIFFERENT teardown
   path, unidentified), so catching the renderer's own deadlocked main thread is
   unreliable. Threads that ARE dumpable at 40 s are browser/utility IO threads
   in NORMAL epoll/futex waits, not the deadlock.
2. **Stripped-chrome symbolization is the wall.** Return addresses land in
   `0x1000_0000_0000+`, but `[libmap]` records only base+len (fd=3, ino=0) -- NO
   file path -- so you cannot tell which `.so` a base is to `objdump` it, and the
   main chrome binary is stripped anyway (no symbol names). To make the stack
   walk actionable you must FIRST teach `[libmap]` (linux_mmap_file) to log the
   opened FILE PATH per mapping, then objdump the matching library at
   `addr - base`; even then only libc/ld.so have usable symbols.

### Honest standing of the DOM (end of this arc)

Both wakeup primitives are proven correct in isolation (linux-futex,
linux-eventfd), chrome uses no unhandled futex op, and ~11 real kernel bugs are
fixed and validated. The residual blocker is a chrome-internal circular wait with
no broken tobyOS primitive under it. The two viable ways forward, both real
projects rather than another kernel fix:
  (a) make the stack walk actionable: `[libmap]` file paths + keep the renderer
      alive (find/neutralize the non-bad-ipc kill path), then symbolize the
      renderer's futex caller against libc/ld.so to see what chrome awaits;
  (b) a logging-enabled `chrome-headless-shell` build, or driving DevTools over
      `--remote-debugging-pipe`, to ask chrome directly.

The kernel-bug tier is essentially exhausted; the remaining work is chrome-symbol
archaeology, which is a different discipline. This is a clean, well-documented
stopping point: green tree, two permanent unit tests, a stack-walk tool, and the
exact next infrastructure step named.

---

## Slice 33 (2026-07-22) — Reliable renderer-stack capture, the GPU-FATAL browser death, and the semantic-Mojo wall

This slice picked up the two barriers slice 32 named and cleared BOTH, then used
the resulting visibility to find the actual chain that kills the DOM.

### Barrier 1 cleared: reliable renderer stack capture (hook the kill, not a timer)

The timer-heartbeat approach (dump all blocked threads at a fixed guest time)
could never reliably hit the renderer's ~1 s deadlock window, and worse, the
gating clock was unreliable: under `-smp 4` the per-CPU TSCs are unsynchronised,
so `perf_now_ns()`/`klog_ms()` read on the BSP in `sched_tick` diverged from the
`[N ms]` log prefix — a gate of `klog_ms() >= 10500` fired at printed `[3044 ms]`.
Abandoned timer gating entirely.

**The robust trigger is the KILL itself.** `bt_dump_group(tgid)` (src/syscall.c)
walks every proc in a thread group and prints its user call chain; it is invoked
from TWO chokepoints:
  - `signal_send()` (src/signal.c) the instant a `SIGKILL` targets a proc with
    `p->is_renderer` — captures the renderer's stacks at the exact moment the
    browser kills it;
  - `proc_exit()` (src/proc.c, CHROMIUM_BOOT) if the renderer self-terminates
    (the Mojo "no connection" watchdog) instead of being SIGKILLed.
One-shot inside `bt_dump_group`. `p->is_renderer` is latched in fork.c's execve
path when argv carries `--type=renderer`, and inherited by clone threads
(thread.c) so an `exit_group` from any thread still fires it. The stack-walk
region filter was also fixed to include the MAIN chrome PIE at load base
`0x500000` (`.text` runtime `0x1fc7000..0xba33e70`) — the prior filter only had
the `.so` region `0x1000_0000_0000+`, so every chrome-code frame was invisible.

Result: we now capture the renderer group's full call chains deterministically.
The renderer's own main-thread frame is often garbage (`urip=0x10202 ursp=0x1b`)
because it is mid-context-switch at kill time, but its worker/IO threads dump
cleanly (futex + epoll_wait waiters, plus one actively-running worker with a deep
live chrome stack — the renderer IS doing real work, not uniformly deadlocked).

### Barrier 2 (stripped symbolization) — partially cleared, still the wall

`[lopen] pid fd path` traces added to LX_open/openat map library paths to fds;
libraries load deterministically (e.g. libc.so.6 base `0x100000ac3000`, verified
`urip 0x100000b9b7bf -> libc.so.6+0xd87bf`). But the MAIN binary is stripped
(BuildID present, debug info is in a separate `headless_shell.debug` we do NOT
have) and its `.dynsym` (2784 syms) covers only imports/exports, not the base::/
mojo::/blink:: internals we need. So MAIN frames give addresses, not names. This
stayed a wall — but the NEXT finding made symbolization unnecessary.

### THE CHAIN THAT KILLS THE DOM (found via chrome's own stderr, not stacks)

Target URL is `data:text/html,<h1>tobyOS</h1>` — a data: URL that needs NO network
fetch. Yet the browser reliably DIES at guest ~20 s:

```
FATAL:content/browser/gpu/gpu_data_manager_impl_private.cc:417]
      GPU process isn't usable. Goodbye.
[proc] pid=2 'chrome' exit code=-1        <-- THE BROWSER ITSELF EXITS
```

Even with `--disable-gpu`, chrome still spawns a separate `--type=gpu-process`
for GpuDataManager info collection. That GPU process HANGS in its Mojo bootstrap
(futex-blocked), the browser's GPU watchdog SIGKILLs it (exit 137 = signal 9),
counts a crash, and after 3 crashes the browser LOG(FATAL)s "GPU process isn't
usable. Goodbye." and the whole browser exits — long before the renderer can dump
a DOM. The network-service `ILLEGAL_MEMORY_RANGE` crash-loop is a PARALLEL
symptom, not the cause (a data: URL needs no network service).

**Fix applied: `--in-process-gpu`.** Runs the GPU implementation on a browser-
process thread — no separate GPU process to hang, no watchdog kill, no FATAL.
Result (measured):
  - Browser now SURVIVES to 39 s+ (was: dead at 20 s), idle in `poll()`.
  - No `--type=gpu-process` spawned at all; the GPU-FATAL is gone.
  - Renderer (pid 26) now runs 9 s / 6.8 s CPU / 2207 syscalls — the DEEPEST
    chrome has ever gotten — before the browser SIGKILLs it at ~15.3 s for never
    completing its Mojo connection.
This is a keepable config change, not a hack: for `--dump-dom` (no rendering)
running GPU in-process is exactly right. Committed in kernel.c (argc 15->16).

### The residual blocker, now precisely localized: SEMANTIC Mojo/ipcz malformation

With the browser alive, the renderer still never commits a document (zero
navigate/DidCommit markers) and is killed at ~15.3 s "with no connection." The
one deterministic error, identical every run:

```
mojo/public/cpp/bindings/lib/validation_errors.cc:136]
      Invalid message: VALIDATION_ERROR_ILLEGAL_MEMORY_RANGE
network.mojom.NetworkServiceMessageHeaderValidator
```

Everything BELOW Mojo is proven correct, so the corruption is SEMANTIC (a decoded
message's internal offset/length runs past its valid payload), NOT byte-level:
  - **Socket transport byte-perfect**: the `[sockchk]` FNV hash (stamped at
    enqueue, re-checked at dequeue) reports 0 corruptions across the whole run.
  - **Byte counts match end-to-end**: `[chan]` trace shows browser
    `sendmsg -> 264` then `sendto -> 232`; renderer `recvmsg -> 264` then
    `recvmsg -> 232` (note: the a3 in `[chan]` is the flags word — 0x40
    MSG_DONTWAIT / 0x4000 MSG_NOSIGNAL — NOT a length). Framing preserved, not
    coalesced across message/fd boundaries.
  - **MAP_SHARED shared memory coherent**: per-inode shm cache (syscall.c
    linux_mmap_file) maps the SAME physical pages for all mappers; `[shm]` traces
    show CREATED-then-`attached` for the same inode across processes.
  - **SCM_RIGHTS fd passing works**: the failing 264-byte message carries 1 fd,
    delivered as `kind=2` (FILE_KIND_VFS = chrome's temp-file shared-memory
    region, since `--disable-dev-shm-usage` avoids memfd).

So the wall is now entirely ABOVE the kernel primitives: an ipcz/Mojo protocol-
level mismatch where a well-formed-on-the-wire message decodes to an illegal
range. Leading suspects for the NEXT agent, in order:
  1. **ipcz driver-object / handle-count encoding**: the message carries 1 fd
     (kind=2). If the ipcz message's driver-object array count or the data-vs-
     handle section boundary is off by the handle accounting, a data pointer
     computed relative to it lands out of range. Instrument the ipcz message
     header (num_bytes, num_handles, first-object offset) at the failing recv.
  2. **NodeLinkMemory fragment coherence**: ipcz places most message data in a
     shared-memory "fragment" and sends only a descriptor (buffer_id, offset,
     size) over the socket. Verify the receiver's mapping of that buffer_id is
     the SAME region+size as the sender's — a resized (ftruncate-after-map) or
     off-by-a-page region would give exactly ILLEGAL_MEMORY_RANGE with perfect
     socket bytes. Our per-inode cache populates newly-grown pages from the file
     but may not track a post-map ftruncate growth.
  3. The message is `network.mojom` — but the SAME failure kills the renderer's
     bootstrap, so it is a GENERIC ipcz issue, not network-specific.

### Standing of the DOM (end of slice 33)

New, higher-water state: browser survives, renderer runs 9 s of real work. The
kernel-primitive tier remains exhausted and now DOUBLY confirmed clean (socket
hash, byte counts, shm coherence, fd kind all verified at the failing message).
The single remaining blocker is a semantic ipcz decode mismatch —
`VALIDATION_ERROR_ILLEGAL_MEMORY_RANGE` — that must be attacked at the ipcz
message layer (instrument num_bytes/num_handles/fragment-descriptor at the
failing recv), NOT with another socket/shm/futex fix. Diagnostic tooling landed:
`bt_dump_group` (kill-hook renderer stack capture), `is_renderer` tagging,
`[lopen]` path traces, MAIN-region stack filter, `[chan]` size trace.

### Slice 33 addendum — channel framing is size-consistent (message-header decode)

Enhanced `[chan]` (syscall.c linux_syscall) to decode the front of each received
channel message. MEASURED frame layout on chrome-headless-shell 151:
```
off 0: u16 num_header_bytes (=16)   |  u16 version (0 or 1)   -> printed chdr=0x...
off 4: u32 num_bytes  == TOTAL message size                   -> printed num_bytes=
```
The offset-4 `num_bytes` EQUALS the delivered byte count `r` for every fully-
delivered message (184, 168, 288, 232 ... all match). The only case where
num_bytes > r is a message larger than the reader's buffer (e.g. 8504 into a 4096
read) — normal multi-read stream reassembly, expected and correct.

=> The AF_UNIX channel is size-consistent end to end; there is NO truncation and
NO mis-framing at the channel level. (An earlier pass mis-read offset 0 as the
size and flagged a phantom "num_bytes=65552 mismatch" — 65552 = 0x10010 is just
`num_header_bytes=16 | version=1`, NOT a size. Ignore that; the size is at
offset 4.)

Therefore `VALIDATION_ERROR_ILLEGAL_MEMORY_RANGE` is a NESTED pointer/offset
inside a correctly-framed, fully-delivered message that runs past the message's
own payload. The next concrete step is to decode the MOJOM message body that
begins at offset `num_header_bytes` (16) within the channel payload: read its
`internal::MessageHeader` (StructHeader{num_bytes, version}, interface_id, name,
flags, and — for V2 — the payload + payload_interface_ids pointers) and find
which field's (offset, size) exceeds the mojom payload. That is the field chrome's
MessageHeaderValidator rejects. Since the same failure also stalls the renderer's
own bootstrap (not just network.mojom), it is a GENERIC ipcz encoding mismatch;
the strongest remaining suspect is the ipcz driver-object (handle) accounting —
the failing message carries 1 SCM_RIGHTS fd (kind=2), and if the data-vs-handle
section boundary is computed differently on the receive side, an out-of-line
pointer computed relative to it lands out of range.

### Slice 33 addendum 2 — the mojom message is ipcz-wrapped; socket-byte tier exhausted

Widened `[chan]` to read 64 bytes and tried to decode a mojom internal::MessageHeader
at channel-payload offset 16. It flagged 132 "MOJOM-HDR-OVERRUN" with absurd sizes
(nb=65560, 1310744, 2228248) and a field at off24 that INCREMENTS per message
(16,17,18...). That is NOT a mojom header — it is an **ipcz transport-message
header with a per-message sequence number**. CORRECTED the instrument (the fields
are relabelled `ipcz[w0 seq]`, no overrun flag). Lesson: do NOT read a mojom
num_bytes at socket offset 16.

**Why this matters:** chrome-headless-shell 151 uses **ipcz** (MojoIpcz). The socket
does NOT carry the mojom message at a fixed offset — it carries ipcz control
messages, and the actual `network.mojom` parcel is reassembled by chrome from ipcz
parcels, which for anything non-trivial live in **shared-memory fragments**
(NodeLinkMemory), not on the socket. So the failing message that trips
`VALIDATION_ERROR_ILLEGAL_MEMORY_RANGE` is NOT visible in the socket bytes. Socket-
byte instrumentation is therefore EXHAUSTED for finding it.

**Shared-memory coherence RE-VERIFIED clean (negative result):** analysed the
`[shm]` map log for the renderer's run:
  - ZERO gen==0 (ramfs) copy-fallback regions — every shm region has a real
    tobyfs inode and goes through the coherent per-inode page cache.
  - The renderer ATTACHES to the shared NodeLinkMemory (e.g. pid29 attaches
    ino=13/1 np=64 + ino=13/2, created by its browser-side peer) — same physical
    pages, coherent.
  - The "created-by-one, attached-by-none" regions (e.g. pid28 ino 33..49) have
    real inodes and are legitimately single-process (V8 snapshot / code cache),
    not shared buffers that failed to attach.
So the primary + secondary NodeLinkMemory the renderer uses IS coherent. The
blocker is NOT a kernel shm bug.

**Net:** every tobyOS-side primitive under Mojo is now proven clean at the failing
point (socket bytes, channel framing sizes, shm coherence incl. no copy-fallback,
SCM_RIGHTS fd delivery). The residual `ILLEGAL_MEMORY_RANGE` is an ipcz protocol-
level decode mismatch inside a message that is NOT socket-resident. The next tier
is a DIFFERENT discipline than kernel fixes or socket tracing:
  (a) decode the ipcz wire format from source (fetch ipcz `src/ipcz/message.h`,
      `node_messages.h`, and the fragment/BufferPool code via
      chromium.googlesource.com ?format=TEXT|base64 -d) to interpret the traces
      and find which parcel/fragment field is out of range; OR
  (b) a logging-enabled chrome-headless-shell build, or driving DevTools over
      `--remote-debugging-pipe`, to have chrome report the failing interface/method
      directly (official builds compile VLOG out, so stderr won't).
Caveat for the next agent: it is entirely possible the tobyOS side is fully correct
and the mismatch is an ipcz expectation we haven't found yet (e.g. an mmap/fragment
size rounding, or a driver-object handle-index convention) — budget for the
possibility that this needs ipcz-source archaeology, not another primitive fix.

### Slice 33 addendum 3 — MAP_SHARED coherence PROVEN in isolation; kernel shm exonerated

Added a third permanent unit test, **programs/linux-mapshare** (`/bin/linux-mapshare`,
spawned at boot as MAPTEST alongside FUTEXTEST/EFDTEST). It reproduces chrome's exact
shared-memory shape in ISOLATION:
  parent: openat(O_RDWR|O_CREAT) temp file -> ftruncate(8192) -> mmap#1 MAP_SHARED
          -> write MAGIC1 -> UNLINK the path (chrome unlinks immediately) -> fork();
  child : INDEPENDENT mmap#2 MAP_SHARED of the same inherited fd
          -> read must see parent's MAGIC1 (forward), write MAGIC2 (reverse), exit;
  parent: wait, then read mmap#1 must see child's MAGIC2.
Exit 3=PASS (both directions coherent), 1=FAIL fwd, 4=FAIL rev, 2=ERROR.

**RESULT: MAPTEST VERDICT: PASS exit=3.** Two independent MAP_SHARED mappings of an
unlinked file in two processes are coherent BOTH directions. The kernel shared-memory
layer is therefore DIRECTLY proven correct (not merely inferred from "attached" logs),
and the DOM blocker is NOT a kernel shm bug.

IMPORTANT PROCESS NOTE: the first MAPTEST run HUNG the kernel and FUTEXTEST spuriously
FAILED. Root cause was NOT a real bug -- it was STALE OBJECT FILES. This slice added a
field to `struct proc` (`is_renderer`), which changes the struct layout; incremental
`make` does not reliably recompile every .c that embeds struct proc, so objects
disagreed on the layout -> corruption. Fix: remove ALL kernel .o (not programs/libtoby)
and rebuild. After the clean rebuild, FUTEXTEST/EFDTEST/MAPTEST all PASS. LESSON (already
in MEMORY, re-confirmed the hard way): after growing struct proc, force a clean kernel
rebuild before trusting ANY run.

### Where slice 33 leaves the DOM (final)

Three isolated primitive tests now pass — futex wakeup, eventfd/epoll wakeup, and
cross-process MAP_SHARED coherence — so EVERY tobyOS primitive under Mojo is proven
correct in isolation, and additionally proven clean at the live failing point (socket
bytes byte-perfect, channel framing size-consistent, shm attaches coherent, SCM_RIGHTS
fd delivered). The browser now survives (--in-process-gpu) and the renderer runs ~9 s of
real work. The single residual blocker is a SEMANTIC ipcz decode:
`VALIDATION_ERROR_ILLEGAL_MEMORY_RANGE` on a network.mojom parcel that is
FRAGMENT-RESIDENT (the network service maps NodeLinkMemory at ~14.9 s, then gets the bad
message at ~15.8 s), hence NOT visible in socket bytes and NOT attributable to any broken
kernel primitive.

The kernel-primitive tier is now EXHAUSTED and TRIPLY confirmed. The only ways forward
are a different discipline:
  (a) ipcz-source archaeology: fetch third_party/ipcz message/parcel/fragment code +
      the mojo MessageHeaderValidator, then instrument the RECEIVER'S SHARED-MEMORY read
      of the failing parcel (dump the NodeLinkMemory fragment content the network service
      reads at ~15.8 s) to find which encoded (offset,size) overruns the parcel. This is
      the only way to SEE the failing message, since it never touches the socket.
  (b) a logging-enabled chrome-headless-shell build or DevTools over
      --remote-debugging-pipe, to have chrome name the failing interface/method directly
      (official builds compile VLOG out).
Realistic caveat: it is possible tobyOS is fully correct and the mismatch is an ipcz
convention we implement subtly differently (a fragment size rounding, a driver-object
handle-index base, a BufferPool block-size assumption). Budget for that; this is no
longer a "find the broken syscall" problem.

---

## Slice 34 — THE DOM. fork() was CoW-ing MAP_SHARED; three more walls behind it

**RESULT: `--dump-dom` prints `<html><head></head><body><h1>tobyOS</h1></body></html>`,
chrome exit=0.** The data: URL loads, the renderer commits the navigation, Blink
builds the DOM, the browser dumps it. Four distinct kernel walls fell in this slice.

| # | Hypothesis | Status | Evidence |
|---|---|---|---|
| S34-1 | **`vmm_cow_fork` write-protects MAP_SHARED pages, so the browser's POST-FORK writes into live ipcz NodeLinkMemory CoW-diverge into a private copy** | **CONFIRMED** | Extended MAPTEST with a post-fork phase (parent writes MAGIC3 through its PRE-fork mapping AFTER fork; child polls): **FAIL exit=6 on the old kernel, PASS exit=3 after the fix**. Chrome: `ILLEGAL_MEMORY_RANGE` count went to **0** and never returned. |
| S34-2 | Grow-down stack expansion leaves HOLES above the new base | **CONFIRMED** | Renderer SEGV_MAPERR at stack_top-0xDFF0 with err=P0/W1: a multi-KiB prologue faults on its LOWEST touch first, the old code mapped ONE page and moved base below untouched pages; the next write hit an "above-base" hole = unresolvable. Fix: map every page from the fault up to the old base (Linux expand_stack shape). SEGV gone. |
| S34-3 | Renderer spins forever re-trying mmap for ALIGNMENT | **CONFIRMED** | Per-pid `[amap]`: V8's pointer-compression cage wants a ~4 GiB reservation 4 GiB-ALIGNED (PartitionAlloc wants 2 MiB super-pages); the 4 KiB bump allocator virtually never satisfies the probe, so the renderer burned its whole --timeout in an alloc/free-4GiB storm (each giant munmap walks pages under the BKL = everything starves). Fix: `find_free_region` returns naturally-aligned bases (>=4G requests 4G-aligned, >=2M requests 2M-aligned). First probe now succeeds. |
| S34-4 | Honor non-FIXED mmap HINTS (Linux semantics) | **REAL-BUT-REVERTED** | Correct per Linux, but empirically WEDGED the whole system at ~14 s (browser main thread in a clock_gettime storm, all IPC quiet, --timeout never fired) in two consecutive runs; reverting it (keeping S34-3) restored the healthy shape. Root cause unidentified -- likely some chrome allocator invariant around hint placement vs its own reservations. NOT needed for the DOM; left OUT. A one-shot clock_gettime-storm backtrace dump (`bt_dump_self`) is now armed in the syscall if it ever recurs. |
| S34-5 | After all four: load is merely SLOW under TCG, not stuck | **CONFIRMED** | The data: URL's own renderer (2nd renderer, site isolation) spawns ~12-15 s in and was SIGKILLed MID-STARTUP, healthy, threads parked in epoll_wait message loops. `--timeout=5000` -> `--timeout=120000`: **DOM dumped at ~23 s, exit 0.** |

### The S34-1 mechanism (the actual multi-slice DOM blocker)

`vmm_cow_fork` stripped PTE_WRITABLE from EVERY writable user page -- including
MAP_SHARED shm-cache/memfd pages. The parent's FIRST post-fork write through a
pre-fork mapping then faulted; the CoW handler saw refcount>1 (guaranteed: the shm
cache itself holds a ref on every page) and silently copied -- from that instant the
browser wrote its ipcz parcels into a PRIVATE page while children kept reading the
stale SHARED one. That is precisely an ipcz "protocol-level decode mismatch in
fragment-resident data": children decode stale/half-old fragment headers whose
(offset,size) no longer match -> `VALIDATION_ERROR_ILLEGAL_MEMORY_RANGE` on the
network service, mojo kills the connection, navigation never commits.

Fix: a `PTE_SHARED` software PTE bit (bit 52), stamped by `vmm_map(VMM_SHARED)`
from `shm_cache_mmap`/`memfd_map`/shared demand-fills; `vmm_cow_fork` copies such
PTEs as-is (both sides keep writing the SAME frame); `vmm_protect` preserves the
bit across mprotect; `mmap_cow_clone` never marks SHARED/NOFREE VMAs CoW.

### Why 33 slices of instrumentation missed it

Slice 33's MAPTEST proved forward+reverse coherence -- but both its writes happened
either BEFORE fork or through a FRESH post-fork mapping. The one shape chrome
actually depends on (write AFTER fork through a PRE-fork mapping) was the exact
blind spot. The socket traces were byte-perfect because the corruption never
touched the socket; the `[shm]` traces said "attached" because attachment WAS
correct -- the pages silently diverged only when the browser forked its next child.
LESSON: a coherence test must cover the TEMPORAL shape (write-after-fork), not just
the topological one (two mappers, one file).

### Validation
- MAPTEST: FAIL exit=6 pre-fix (proved the test sees the bug), PASS exit=3 post-fix.
- FUTEXTEST / EFDTEST: still PASS.
- chrome: `ILLEGAL_MEMORY_RANGE` 0 across all post-fix runs; renderer no longer
  crashes; `--dump-dom` prints the real DOM; exit 0.
- defboot: clean (see logs/defboot.log).

---

## SLICE 34 (2026-07-22) — ***THE DOM PRINTS.*** Goal achieved; and a CORRECTION to slice 33

```
[23861 ms] [stdout] <html><head></head><body><h1>tobyOS</h1></body></html>
[24388 ms] [proc] pid=2 'chrome' exit code=0 (0x0) cpu=2394 ms syscalls=13570
[24394 ms] [boot] CHROMIUM: chrome (pid=2) exit=0
```

Real, unmodified `chrome-headless-shell` 151.0.7922.34 loads `data:text/html,<h1>tobyOS</h1>`
and prints the parsed DOM via `--dump-dom`, exiting **0**. **REPRODUCED on two consecutive
runs.** Same run: FUTEXTEST / EFDTEST / MAPTEST all PASS, `ILLEGAL_MEMORY_RANGE` = **0**,
GPU "isn't usable" FATAL = **0**, `[sockchk]` corruption = 0. (A `SIGKILL victim group`
line still appears, but AFTER the DOM prints and after `sys_exit code=0` -- that is normal
renderer teardown during clean shutdown, not a failure.)

### CORRECTION: slice 33's "semantic ipcz wall" conclusion was WRONG

Slice 33 concluded the residual blocker was a SEMANTIC ipcz decode mismatch
(`VALIDATION_ERROR_ILLEGAL_MEMORY_RANGE` on a fragment-resident network.mojom parcel) and
that the next tier had to be ipcz-source archaeology. **That was a phantom.** It is now 0
occurrences per run with no ipcz work whatsoever. The error -- and very likely the
"renderer killed at ~15.3 s with no connection" and the deterministic login livelock that
followed -- were ARTIFACTS of **stale-object kernel corruption**.

Root cause of the phantom: slice 33 added a field (`is_renderer`) to `struct proc`, which
changes the struct's layout. Incremental `make` does NOT reliably recompile every .c that
embeds `struct proc`, so different objects disagreed on field offsets. The resulting
corruption did not announce itself as a crash -- it produced **convincing, stable, false
symptoms** at the application layer: a Mojo message that failed validation, a renderer that
never completed its connection, and (later) a boot that hung right after `login` 4/4 times
and even under `-smp 1`. Every one of those vanished after removing all kernel .o and
rebuilding. Cost: a large amount of this session, plus a wrong documented conclusion that
would have sent the next agent into ipcz internals for nothing.

**HARD RULE (this bit us TWICE in one session -- treat it as non-negotiable):** after ANY
change to `struct proc` (or any widely-embedded struct/header), delete all kernel objects
and rebuild before trusting a single observation:
```
find . -maxdepth 3 -name '*.o' | grep -vE 'programs/|libtoby|sdk' | xargs rm -f
```
Corollary, and the real lesson: when a *userspace* symptom is exotic and stable (a protocol
validator rejecting a well-formed message; bytes that are provably byte-perfect on the wire
yet "malformed" on arrival), suspect YOUR OWN BUILD before you suspect the application's
protocol. A clean rebuild is minutes; protocol archaeology is days.

### What is actually required for the DOM (attribution -- one part still OPEN)

Two changes were in place when it first worked:
1. **`--in-process-gpu`** (kernel.c, argc 16). This one is independently evidenced: the
   browser was observed LOG(FATAL)-ing "GPU process isn't usable. Goodbye."
   (gpu_data_manager_impl_private.cc:417) and exiting at ~20 s after its watchdog SIGKILLed
   the hung gpu-process 3x. Running GPU on a browser thread removes that chain entirely.
2. **A clean kernel rebuild** (eliminating the stale-object corruption above).

**OPEN:** it has NOT been isolated whether `--in-process-gpu` is still NECESSARY once the
build is clean, or whether the clean build alone suffices. The one clean build that predates
the DOM run (bt25) was killed right after the unit tests, so it never reached the dump. To
settle it: clean-build with `--in-process-gpu` removed and see whether the DOM still prints.
Do NOT assume either change is load-bearing until that test runs.

### Standing

The Track B / Chromium `--dump-dom` milestone is **MET**: a real, unmodified, off-the-shelf
Chromium runs its full multi-process engine on tobyOS (browser + gpu-in-process + network +
utility + renderer, Mojo/ipcz, cross-process shared memory, SCM_RIGHTS, futex/eventfd
wakeups) and produces a correct parsed DOM. Three permanent unit tests (linux-futex,
linux-eventfd, linux-mapshare) guard the primitives underneath it. Remaining chrome work
(screenshot/render tier, the flaky Vulkan/SwiftShader path) is unchanged and out of scope
for this milestone.

---

## SLICE 35 (2026-07-23) — ***REAL NETWORK NAVIGATION.*** chrome fetches and parses a live page

```
[23463 ms] [stdout] <!DOCTYPE html>
<html lang="en"><head><title>Example Domain</title>...<h1>Example Domain</h1>
<p>This domain is for use in documentation examples without needing permission...
[31056 ms] [boot] CHROMIUM: chrome (pid=2) exit=0
```

Real `chrome-headless-shell` resolves `example.com` over DNS, opens TCP, fetches over
HTTP/1.1, Blink parses it, `--dump-dom` prints the live page. **exit=0, ~23 s.**
FUTEXTEST / EFDTEST / MAPTEST all still PASS in the same run. The harness URL is now a
real `http://` URL rather than a `data:` URL, so the boot asserts the whole path.

### The five fixes, and why each mattered

| # | Fix | Evidence it was load-bearing |
|---|---|---|
| S35-1 | **`getsockname` never filled `sin_addr`** (only family+port, i.e. always 0.0.0.0) | glibc's getaddrinfo implements RFC 3484 destination sorting by `connect()`ing a UDP socket per candidate and reading `getsockname()` for the source address. 0.0.0.0 reads as "no route", which is why every resolution retried on an exact **3-second cadence, 5 times** (measured: 7 socket + 7 connect + 5 sendto + 2 recvfrom per burst). `getpeername` was also aliased to `getsockname` -- it answered with the LOCAL address for every caller asking who it was talking to. |
| S35-2 | **`SOCK_NONBLOCK`/`O_NONBLOCK` parsed and thrown away** | `lx_socket` did `type & 0xff` with the comment "strip SOCK_NONBLOCK" and never stored it. Chrome's network service creates every socket non-blocking and drives it from epoll; a blocking `recv` parks its IO thread. Now tracked on `struct sock`, honoured at socket/accept4/fcntl(F_SETFL), and read back by F_GETFL. |
| S35-3 | **`recvfrom`'s `flags` argument was DROPPED at the dispatch** (`lx_recv(..., a5, 0)` -- a4 never passed) | So `MSG_DONTWAIT` was silently ignored and every "poll me" read blocked. This is what chrome's netlink drain loop is built on. |
| S35-4 | **blocking-only connect/send** | `connect()` waited out the handshake (no EINPROGRESS); `tcp_send` blocked until every byte was ACKed. Added `tcp_connect_nb()` + `tcp_send_nb()` (queue what the window allows, short-write, 0 = EAGAIN), EINPROGRESS + poll-writable completion + read-and-clear `SO_ERROR`, and a deadline so a handshake drawing NO reply cannot leave poll silent forever. |
| S35-5 | **no `AF_NETLINK` at all** | `ERROR:net/base/address_tracker_linux.cc:224 Could not create NETLINK socket` every run. Implemented NETLINK_ROUTE answering RTM_GETADDR/RTM_GETLINK dumps (lo + eth0) terminated by NLMSG_DONE. |

### TRAP: a half-implemented AF_NETLINK is WORSE than none (cost a full run)

Making `socket(AF_NETLINK)` succeed without a working `recvmsg` **broke DNS outright** --
a strict regression from the EINVAL that preceded it. glibc's `__check_pf` (which
getaddrinfo runs on EVERY resolution) has this asymmetry:

- netlink **fails to open** -> glibc assumes both families exist and carries on;
- netlink **opens but answers unusably** -> `seen_ipv4 = false` -> getaddrinfo filters
  out every IPv4 result -> no resolution at all.

Two independent bugs produced "answers unusably", and the SECOND would never have been
guessed from the symptom:

1. `__check_pf` reads with **`recvmsg`**, not `recv` -- the netlink arm existed only in
   `recvfrom`/`read`. Symptom: glibc's own
   `Unexpected error 88 on netlink descriptor N (address family 16)` (88 = ENOTSOCK).
2. glibc **never calls `bind()`**, then discards any reply failing
   `nladdr.nl_pid != 0 || nlmh->nlmsg_pid != pid || nlmh->nlmsg_seq != req.seq`.
   Linux auto-binds an unbound netlink socket to the caller's pid on first send and
   echoes that in replies; without that auto-bind every reply is dropped SILENTLY even
   once recvmsg works.

**LESSON (a sharper form of the slice-34 lesson): when adding a capability that
userspace probes for, the fallback path you are removing may be the one that WORKS.
Either implement it completely enough to be believed, or keep failing the probe
cleanly.** Both facts above came from READING THE REAL SOURCE (`address_tracker_linux.cc`
and glibc `check_pf.c`), which also corrected two things that would have been wrong by
assumption: chrome's netlink socket is **NOT** created non-blocking (it drives the drain
purely with MSG_DONTWAIT), and a link only counts as ONLINE with
`IFF_UP|IFF_LOWER_UP|IFF_RUNNING` -- miss `IFF_LOWER_UP` and chrome still concludes
CONNECTION_NONE.

### https:// -- NOT working. Two real bugs fixed behind it; the wall is transport-tier

`https://example.com/` still dumps an empty body (`chrome exit=0`, ~31 s). Two genuine
bugs were found and fixed on the way, both far bigger than chrome:

- **`getrandom(2)` ZERO-FILLED** (`/* Best-effort: zero-fill (deterministic) */`).
  NSS's freebl seeds its RNG from it, runs a continuous health check, sees a constant
  stream and fails softoken init with CKR_DEVICE_ERROR -> `SEC_ERROR_PKCS11_DEVICE_ERROR`
  (-8023) -> `[FATAL:crypto/nss_util.cc:146]`, aborting every https navigation before the
  handshake. tobyOS already had an RDRAND-seeded CSPRNG (`rng.h`) that TLS uses -- the
  syscall was simply never wired to it. Also removed the silent 256-byte truncation
  (Linux fills the whole request; a short count leaves non-looping callers with
  uninitialised tail bytes). **This is a system-wide correctness fix: any TLS, key
  generation, ASLR or hash seeding on tobyOS was drawing zeros.**
- **no `/dev/urandom` or `/dev/random`** -- added as `FILE_KIND_DEVRANDOM` (reads from
  the CSPRNG, writes mix back in). NSS falls back to these when the syscall is absent, as
  do OpenSSL/Python/glibc. While there: `/dev/zero` was never reported readable by poll
  (it fell to the `default:` arm, POLLOUT only) -- fixed.
- **`chmod`/`fchmod`/`fchmodat` were ENOSYS** -- NSS creates its key DB then chmods it
  0600 and treats failure as "not secure", giving `SEC_ERROR_BAD_DATABASE` (-8174).
  Now a validated no-op (the VFS has no mode bits).

After all three, NSS initializes cleanly (zero nss_util errors) and the https run is
quiet -- but the DOM is still empty.

**DECISIVE NEGATIVE RESULT: `--ignore-certificate-errors` changes NOTHING.** The body is
still empty with identical timing. So the https blocker is **NOT certificate
verification** -- the leading hypothesis is eliminated. What is known:
- exactly ONE client TCP connection is made (`tcp[2]`), it reaches ESTABLISHED, and the
  peer sends **FIN at ~22.9 s**; chrome then exits at ~31 s with an empty document and
  NO "Page load timed out" (so it committed something, it did not hang).
- the same kernel fetches `http://example.com/` perfectly, so DNS/TCP/epoll are fine.

=> The failure is in the TLS exchange itself, above connect and below cert checking.

### MEASURED (same slice): the transport is CLEAN -- the stated suspect was WRONG

Added CHROMIUM_BOOT-gated per-connection wire accounting (`[tcp] WIRE`) + a TLS
record-type trace (`[tls] rx`). Result on `https://example.com/`:

```
[tls] rx tcp[2] seg=1440 type=0x16 ver=0303 reclen=1210     <- well-formed ServerHello
[tls] rx tcp[2] seg=512  type=0x17 ver=0303 reclen=445      <- TLS 1.3 application_data
[tcp] WIRE tcp[2] port=443 tx=1911 rx=4776 read=4776 ooo=0 retx=0
```

**This DISPROVES the out-of-order-reassembly hypothesis written above** (kept
deliberately, as the record of a wrong call): `ooo=0, retx=0`. Note also that
tcp.h's header comment claiming "no window scaling" is STALE -- tcp.c implements
RFC 7323 scaling and CUBIC. Established facts now:

- the client sends a complete padded ClientHello (tx=1911) and NOTHING afterwards;
- the server returns a complete, well-formed TLS 1.3 flight (rx=4776: a 0x16
  ServerHello then 0x17 application_data records);
- **chrome CONSUMES every byte (read=4776 == rx)**, so this is not a kernel
  read/readiness bug -- the bytes reach the application;
- chrome then sends **no TLS Finished AND no alert**. A client that REJECTS a
  handshake sends an alert first; total silence means it is STUCK, not refusing;
- ~0.5 s later chrome execve's a fresh renderer (pid 50) = the error-page commit,
  and dumps an empty body at ~31 s.

LEADING HYPOTHESIS (untested): the stall is in chrome's CERT VERIFICATION, which
runs off the TLS thread. Note `--ignore-certificate-errors` does NOT skip the
verifier -- it ignores its *result* -- so that flag failing to help is consistent
with a verifier that never RETURNS. Candidates: a verifier/thread-pool task that
never gets scheduled, or the verifier blocking on something absent (CRLSet/CT
data, an OCSP fetch). NEXT: dump the network-service thread stacks at the stall
with the existing `bt_dump_group` instrument (slice 33) and see what the verifier
thread is parked on; NSS init is now clean, so the DB is no longer the cause.

CAVEAT on the instrument: `[tls] rx` prints the first 5 bytes of each TCP
SEGMENT, but only the first segment starts on a record boundary -- middle lines
are mid-record continuation bytes and must NOT be read as record headers.
