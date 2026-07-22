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
