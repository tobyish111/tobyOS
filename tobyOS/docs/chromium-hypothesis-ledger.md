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

### Render tier re-tested in slice 35 -- STILL BROKEN (a hypothesis, refuted)

It looked plausible that slice-34's MAP_SHARED CoW fix had also cured the
SwiftShader "memory corruption" of slice 20, since the recorded symptoms (a
`link_map->l_versyms` wrong by a page-aligned `0x1495000`, a memcpy destination
landing in a guard page) are exactly what CoW-diverged shared pages look like,
and all of that analysis predated the fix. **Tested and FALSE.** Swapping
`--disable-gpu --disable-software-rasterizer` for
`--use-angle=swiftshader --use-gl=angle` (plus `--screenshot`/`--window-size`):

```
[4316 ms] [lopen] pid=15 fd=27 /opt/chrome/libvk_swiftshader.so
*** EXCEPTION 14: Page Fault  (in user mode) ***
[5048 ms] [isr] user-mode fault -- terminating pid=15 fault_count=344
                last_fault_rip=0x0000000007a4bb73
```

Dies ~5 s in, right after loading the SwiftShader ICD, and never reaches a DOM
or a screenshot. So the GL/Vulkan crash is INDEPENDENT of the CoW bug and
remains its own front. Harness reverted to the validated `--disable-gpu`
configuration. (Note the faulting rip differs from slice 20's `memcpy+0x35d`,
but so does the load layout -- correlate via `[libmap]` before concluding they
are different bugs.)

---

## SLICE 36 (2026-07-23) — https ROOT-CAUSED: chrome's post-quantum ML-KEM key exchange

**The https failure is chrome negotiating the post-quantum hybrid key exchange
`X25519MLKEM768` (TLS group 0x11EC = 4588), whose ML-KEM path fails on tobyOS.**
Found by pulling chrome's OWN NetLog off the guest and reading the negotiated
group + the exact error. NOT transport, NOT cert (date/trust/CT), NOT the AEAD
cipher, NOT the TLS version, NOT the crypto implementation (asm vs C).

### The decisive evidence chain (each experiment killed a hypothesis)

| Probe | Result | Kills |
|---|---|---|
| `[tcp] WIRE` byte accounting | tx=1847 rx=4776 **read=4776 ooo=0 retx=0** | transport / my socket code |
| native tobyOS TLS 1.3 to same URL | **SUCCESS 200** (validates the REAL cert vs the REAL 2026-07-23 clock via BearSSL: chain+hostname+notAfter) | cert date, cert trust, kernel, route, crypto-in-C |
| decode outbound TLS records | ClientHello -> CCS -> **19-byte encrypted record = 2-byte alert** | "hang" (it aborts, ~30 s not 120 s) |
| `--ignore-certificate-errors` | unchanged | cert policy (it's not a cert error) |
| force TLS 1.2 (`--ssl-version-max`) | fails identically | TLS-1.3-specific |
| force ChaCha20 (`--cipher-suite-denylist`) | fails identically | the AEAD cipher |
| `OPENSSL_ia32cap=0` (pure-C crypto) | fails identically | SIMD-asm crypto path |
| **chrome NetLog** (`--log-net-log`, kernel `vfs_read_all` + scan) | `net_error:-107` ERR_SSL_PROTOCOL_ERROR; `ssl_error:1` at `s3_pkt.cc`; **`exchange_group:4588`**; cipher 4867 (ChaCha) | names the group = X25519MLKEM768 |

### Why only ML-KEM fails while X25519 + the record cipher work

ML-KEM decapsulation ends in the Fujisaki-Okamoto re-encryption **equality
check**: decrypt -> re-encrypt -> compare to the received ciphertext; a mismatch
silently returns a *pseudo-random* shared secret (implicit rejection). So **any
single-bit error anywhere in the thousands of mod-3329 / NTT / Keccak operations
-> total key mismatch -> the record layer can't decrypt the server flight ->
`bad_record_mac` -> SSL_ERROR_SSL in s3_pkt.cc**. X25519 (one tolerant scalar
mult) and the AEAD (once keys are right) do not amplify a tiny error this way.
That is exactly why tobyOS's plain-X25519 native TLS succeeds against the same
server while chrome's ML-KEM path does not.

### Fix status — OPEN. The external disable-levers are NOT honored by headless-shell

- `--disable-features=X25519MLKEM768,PostQuantumKyber,X25519Kyber768Draft00`:
  no-op, group stays 4588. (The flag reaches the browser process; TLS runs in
  the network-service child, which only received the field-trial-forced
  `--disable-features=PaintHolding` -- my override did not propagate, and in a
  2026 build the feature is likely always-on / removed.)
- Managed policy `PostQuantumKeyAgreementEnabled:false` staged in all three
  candidate dirs (`/etc/opt/chrome`, `/etc/chromium`,
  `/etc/opt/chrome_for_testing` `/policies/managed/`): **also no-op** --
  chrome-headless-shell does not include the enterprise-policy machinery.
  (Left staged anyway: it is correct and a full chrome would honor it.)

So the fix requires making BoringSSL's ML-KEM produce a correct result on
tobyOS. RULED OUT as the cause (cheap checks): RNG (getrandom fixed, and a weak
seed can't cause a *wrong* KEM result -- keygen is self-consistent); unzeroed
memory (anonymous demand pages ARE memset-0 in page_fault.c:252); SIMD asm
(ia32cap=0 unchanged); stack overflow (would SIGSEGV; chrome exits 0). Remaining
candidates are a TCG instruction subtlety hit only by ML-KEM's code, or a
memory-corruption specific to its allocation pattern -- both need chrome-internal
instrumentation that an off-the-shelf binary does not allow. This is a genuinely
deep front.

### Permanent artifacts added this slice (all CHROMIUM_BOOT-gated)
- `[https-probe]`: kernel-side native TLS 1.3 GET of https://example.com before
  chrome spawns -- a standing regression guard that the tobyOS network+crypto
  stack is sound (SUCCESS 200 = kernel is not the differ).
- `[tcp] WIRE` / `[tls] rx` / `[tls] TX` / `[tcp] connect` wire+record tracing.
- `[netlog]`: after chrome exits, the kernel `vfs_read_all`s
  /data/netlog.json and scans it for the exact net_error + SSL reason context.
  Reusable for any future chrome network-layer wall.

### Standing
http:// navigation works and is the harness assertion. https:// to real
(post-quantum-enabled) servers -- i.e. Google/YouTube/Cloudflare/most of the web
in 2026 -- is blocked by the ML-KEM wall until BoringSSL's ML-KEM works on
tobyOS. tobyOS's own HTTPS stack is unaffected and fully functional.

---

## SLICE 37 (2026-07-23) — render tier: --screenshot attempted, blocked by the documented GL NULL-deref

Goal: get a rendered PNG out of chrome (`--screenshot`), the prerequisite for
any on-screen / windowed chrome. Two approaches tried, both blocked by the same
wall:

1. **CPU software raster** (`--disable-gpu`, KEEP software rasterizer,
   `--screenshot`): chrome STILL loads `libvk_swiftshader.so` -- `--screenshot`
   forces the GPU/viz compositor, which wants a GL context even under
   `--disable-gpu`. The gpu thread (pid 16) page-faults ~5 s in; chrome then
   hangs past the 400 s harness timeout (never produces a bitmap).
2. **Proper SwiftShader/ANGLE** (`--use-angle=swiftshader --use-gl=angle`,
   the slice 12-14 target config, WSI via the in-kernel fake X server): the
   **IDENTICAL** fault, same rip, same address. So it is NOT a misconfiguration
   -- it is a real, deterministic crash in SwiftShader GL init.

**The fault (localized):** `cr2=0x0000000000000308`, `err=0x4` (user READ of a
not-present page) = a **NULL-pointer dereference reading field +0x308**. The
faulting rip `0x100000b5270d` sits in **libc** (base `0x100000ac5000` from
`[libmap]`, so `libc+0x8d70d` -- a DIFFERENT libc function than slice 20's
`memcpy+0xab34d`). A NULL propagated from a failed SwiftShader/Vulkan init into a
libc call. (`last_fault_rip=0x7a4bb69` in the isr line is the STALE cumulative
latch, not this fault -- use the register-dump rip.)

This is the same class as the render-gl-bug-prompt.md wall (SwiftShader init
fails -> NULL GL-dispatch deref) and is **unchanged by the slice-34 MAP_SHARED
CoW fix** -- confirming (again) the GL crash is independent of the CoW bug.
Fixing it is the documented multi-slice render effort (SwiftShader Vulkan ICD
init / WSI), not a quick shim.

**Harness reverted** to the working `--disable-gpu` + `--dump-dom` +
`http://example.com/` config (no `--screenshot`). The `[screenshot]` PNG-verify
instrument is kept (reports "absent" until the GL wall falls). Pixels -- and
therefore the window/interactive tier -- remain blocked on this crash.

### Where the three "visit any page" fronts stand after this session
- **HTTP navigation:** WORKS (real live page, exit 0). Committed.
- **HTTPS:** root-caused to chrome's post-quantum X25519MLKEM768 ML-KEM (slice
  36). Deep; no headless-shell disable lever.
- **Pixels/render:** blocked by this deterministic SwiftShader GL NULL-deref.
  Deep.
- **Window + input:** not started; blocked on pixels.
All three are independent, deep, multi-slice fronts.

---

## SLICE 38 (2026-07-23) — the "GL NULL-deref" wall FALLS: it was a dlclose→dlopen
## use-after-free in ld.so's search scopes, poisoned by chrome's own allocator

**CORRECTION to the slice-37 diagnosis (and to the handoff doc):** the canonical
front-B crash signature `libc+0x8d70d reads NULL+0x308` was attributed to
`__internal_syscall_cancel` reading a zeroed TCB. Symbolizing that offset against
the sysroot libc's dynamic symbols places it in the **dlopen/dlerror wrapper
cluster** (`dlopen@GLIBC_2.2.5` = 0x8d400, `dlerror` = 0x8ccd0; 0x8d70d is a
static helper right after `__libc_alloca_cutoff`). Every front-B crash this
slice — and, in hindsight, the flaky messenger-walk #GPs of earlier slices —
is one bug family: **dlopen machinery reading memory freed during a prior
dlclose.**

### The capture (iterations 8-10, all in the browser process's GPU thread)

Getting a *raw* fault instead of chrome's own crash handler required
`--disable-in-process-stack-traces` (iter 6) plus two kernel instruments added
this slice in `src/isr.c`:
- full GP-register snapshot BEFORE `signal_deliver_fault` rewrites the
  trapframe (the old log printed the *handler entry* as rip);
- on the FATAL path: a 96-qword user-stack dump at rsp (return addresses
  symbolize offline against `[libmap]`) + a dump of the memcpy source bytes.

**Iteration 9 caught the scribbler in the act.** `EXCEPTION 14`, tid 15
(GPU thread), guest t=5.78s:
- rip = `libc+0xab380` = `__memcpy_sse2_unaligned_erms`, in the non-temporal
  4K-interleaved loop (`movntdq` to `rdi` AND `rdi+0x1000` per iteration);
- dst (rax) = a *stack local* at rsp+0x68 near the top of the thread's own
  8MiB stack; loop counters implied an original length of **~15.8MB**;
- the copy had already flooded [rax, stack_top) — including the first 0x280
  bytes of the thread's OWN TCB at `fs_base = stack_top - 0x940`, i.e. the
  `%fs:0x10` self-pointer — before the store crossed the top of the stack
  VMA (cr2 = stack_top + 0x940) and faulted. **In runs where the next VMA sat
  adjacent, this memcpy corrupted the NEIGHBOR thread's stack/TCB silently —
  producing exactly the historical "TCB self-pointer is NULL" downstream
  crashes.**

**Iteration 10 named the killer.** Same thread, same guest second, one fault
earlier in the cascade:
```
*** EXCEPTION 13: General Protection (in user mode) ***
rip=0x4000a237 (ld.so do_lookup_x+0x107)   rax=0xbadbad00badbad00
    a233: mov (%r14,%rax,8),%rax   ; scope->r_list[i]  (r14 = chrome heap)
    a237: mov 0x28(%rax),%rbx      ; link_map field -> #GP, rax = POISON
```
`0xbadbad00` (x2) is **chrome's poison constant** — a 16-byte SSE fill pattern
in chrome-headless-shell's rodata (4 hits at file offset 0x119f560; zero hits
in ld.so/libc/libvulkan/swiftshader). ld.so's `malloc` resolves through
chrome's allocator shim to PartitionAlloc, so ld.so's own scope arrays live in
— and are poisoned by — chrome's heap.

### The proven event chain (one [lopen]/[amap]/[libmap] trace, tid 15)
1. 4971ms  dlopen `/opt/chrome/libvulkan.so.1` #1 → mapped 0x102f0ac01000
2. 5003ms  dlopen `libvk_swiftshader.so` (the loader's ICD)
3. 5060-5097ms  `vk_swiftshader_icd.json` re-read 4x (enumerate/create cycle;
   first vkCreateInstance attempt fails headless)
4. **5146ms  dlclose(ICD)   → munmap 0x102f0b800000**
5. **5153ms  dlclose(vulkan #1) → munmap 0x102f0ac01000** — this free()s scope
   arrays through PartitionAlloc, which poisons them
6. 5652ms  chrome's second consumer calls dlopen("libvulkan.so.1") #2 → fresh
   mmaps (legitimate — refcount hit 0) → relocation → `do_lookup_x` walks a
   search-scope `r_list` that still references the freed array → #GP on the
   poisoned link_map pointer → fault cascade (the iter-9 runaway memcpy is the
   same walk consuming a poisoned string/size) → GPU thread dies → browser
   wedges.

### The fix (kernel.c, iter 10): pin the Vulkan libs
`LD_PRELOAD=/opt/chrome/libvulkan.so.1:/opt/chrome/libvk_swiftshader.so`
keeps both refcounts ≥ 1 for the process lifetime: dlclose becomes a pure
refcount drop (no unload, no scope frees), the retry dlopen is a name-cache
hit. **Result: the 5.7s GPU-thread death is GONE; the browser survived to
--dump-dom (63s) and wrote /data/shot.png.** envc 7 → 8.

Whether the stale scope reference is an upstream glibc bug (dlclose leaving a
dangling r_list in a surviving scope) or is induced by something tobyOS does
differently is UNRESOLVED — the preload sidesteps it either way. Do NOT remove
the preload without re-testing the dlclose/dlopen cycle.

### NEW wall exposed behind it: a 0xff000000 heap flood kills cert verification
With the GPU thread alive, the https attempt got further and died differently
(iter 10, tid 6 of the browser process, 200ms after the TLS server flight
arrived on tcp[2]):
- `EXCEPTION 13` rip = `libnspr4+0x240d0` — inside PR_Unlock's pending-notify
  walk (static fn after `PR_DestroyCondVar`; frames in libnss3's
  NSS_IsInitialized / trust-store neighborhood ⇒ this is chrome's
  cert-verifier path);
- the function copies the lock's heap-resident notify array to the stack via
  `movaps xmm1..xmm6` then walks it; both the stack copy AND the source heap
  region were flooded with `0xff000000ff000000` — **ARGB opaque-black pixel
  pairs** — and the walk dereferenced one as a link pointer (non-canonical →
  #GP);
- a teardown echo killed tid 19 at 63s (rip in chrome's vulkan-pointer text
  region, same 0xff000000 pattern in rax/rbx/rdi), which wedged browser exit.

So NSS/NSPR is a *victim*: something sprays a pixel clear over PartitionAlloc
heap. Suspect: SwiftShader's Vulkan JIT rasterizer clearing a surface through
a stale/wrong pointer (in-process GPU shares the address space). pmm alloc- and
free-side refcount tripwires stayed SILENT all run (no kernel-side premature
frees observed in the refcounted paths).

### Iteration 11 A/B (in flight when this entry was written)
`--use-angle=vulkan` → `--use-angle=swiftshader` (ANGLE on SwiftShader-GLES;
no Vulkan loader, no SwiftShader-Vulkan JIT). If the flood vanishes and https
completes: pixels AND https unblock together, and the Vulkan-path flood parks
as a follow-up.

### Instruments added this slice (all CHROMIUM_BOOT-gated, keep)
- `src/isr.c` fatal-path user-stack + memcpy-source dumps (`[ustk]`/`[usrc]`)
- pre-`signal_deliver_fault` register snapshot in the `[sigfault]` log
- `logs/sym38.sh` — offline symbolizer: [ustk] qwords → module+offset via the
  run's own [libmap] table

## SLICE 39 (2026-07-23) — ***PIXELS + HTTPS, TOGETHER.*** chrome renders
## https://example.com and writes a REAL screenshot; ML-KEM works natively

**VERDICT: fronts A (https) and B (pixels) are DONE, one root cause under
both.** `--dump-dom --screenshot` run: DOM of https://example.com printed to
stdout, `/data/shot.png` = 11573 bytes, PNG magic OK, and the reconstructed
image (hex-dumped over serial via [shotdump], rebuilt host-side) shows the
page RENDERED CORRECTLY: styled #eee background, laid-out heading, proper
glyph rasterization, blue link. chrome exit=0. NetLog: h2 negotiated,
cipher_suite=4867 (ChaCha20-Poly1305), key_exchange_group=4588 =
**X25519MLKEM768 — the post-quantum exchange chrome's BoringSSL computes now
WORKS on tobyOS; no fallback needed.** The slice-36 "ML-KEM math" theory is
dead: the math was always right (linux-mlkem proved it); its INPUTS were
being corrupted by the kernel bugs below.

### The final root cause: memfd MAP_PRIVATE treated as MAP_SHARED
SwiftShader's Reactor JIT (ExecutableMemory.cpp) keeps ONE memfd named
'swiftshader_jit'; for EVERY compiled routine it ftruncates that fd to the
routine size and mmaps it MAP_PRIVATE at OFFSET 0, writes the code, then
mprotects R-X. Linux gives each such mapping private CoW pages. tobyOS's
memfd_map gave every mapping the SAME physical frames (VMM_SHARED, the
mmap-coherent design added for Mojo) — so compiling routine N overwrote the
code of routines 1..N-1 in place.

**The kill chain, captured end-to-end in one run:** DrawCall holds three
routine slots {vertex,setup,pixel} at +0x40/+0x58/+0x70. All three JIT
mmaps aliased file pages 0..3, so the vertex slot's entry (0x102f13614060)
contained the LAST-compiled routine's bytes — the PIXEL routine. The caller
(libvk_swiftshader+0x1220f9, `call *0x40(%rbx)`) passed vertex-call args
(batch-index array on the stack in rdx) to code whose prologue consumes
pixel-call args `(device, Primitive*, int count, int cluster, int
clusterCount, DrawData*)` — it walked the Vertex output arena as a
Primitive[] with stride 0x8710 (138 strides = exactly the 4.7MB object) and
sailed into PartitionAlloc's PROT_NONE guard. The [pgj] page journal proved
the walked pages were NEVER written (single demand-zero map each): nothing
was "lost" — the walker itself was the wrong routine.

Fix (src/mmap.c memfd_map): honor !MAP_SHARED with an EAGER private copy —
fresh anon pages, file bytes copied at map time, plain private-anon VMA
(normal munmap free, normal fork CoW). Deviation from lazy CoW is invisible
to the JIT pattern (each mapping is touched by exactly one writer/reader).

### The supporting bug family fixed en route (all real, all latent killers)
1. **Cross-CPU editor-root race (vmm.c):** g_pml4/g_pml4_phys were GLOBALS;
   concurrent mmap/munmap/mprotect on different CPUs edited the WRONG address
   space (one process's munmap freed another's live frames → demand faults
   later returned zero pages where data had lived). Now per-CPU
   g_pml4_cpu[]/g_pml4_phys_cpu[] via vmm_ed_idx().
2. **No TLB shootdown (apic.c, vector 0xFD):** PTE downgrades (mprotect,
   munmap, madvise, brk-shrink, COW write-protect at fork) never IPI'd other
   CPUs; stale TLB entries let threads write through freed/remapped frames.
   tlb_shootdown_remote() now added at every downgrade site.
3. **vmm_protect stripped software PTE bits:** mprotect on a COW page granted
   RW in place (both fork sides then wrote one frame). Now preserves
   COW/DEMAND/SWAP bits and defers the write grant to the fault handler.
4. **fork left read-only pages non-COW:** later mprotect(RW) on either side
   made in-place sharing writable. Now ALL non-shared pages are COW-marked.
5. **munmap freed refcounted frames blindly** (slice-38 continuation): now
   refcount-aware on the anon path.

### Where front C starts (next slice)
Chrome runs to completion headless. The window front is now pure integration:
long-lived chrome on --remote-debugging-pipe, Page.captureScreenshot frames
into a TobyTK window, DevTools Input.* events back. One flake to watch: one
run died silently (no serial, qemu exit 1) ~4s AFTER chrome exit=0 during
process teardown — an SMP teardown race still lives somewhere; -d cpu_reset
harness (logs/run38fg.sh) is in place to catch it.

### Instruments added this slice (all CHROMIUM_BOOT-gated, keep)
- [pgj] page journal: every PTE mutation ring-logged (map/unmap/protect/COW/
  demand), dumped at fatal faults; proved the never-written-pages fact.
- [pfres] resolved-fault ring + fatal-path walk-probe.
- [mprot] mprotect/madvise/MAP_FIXED history ring.
- [memfd] now logs SHARED/priv + offset per mmap.
- [shotdump]/[shd] PNG-over-serial hex dump + logs/rebuild-shot.{sh,ps1}.

## SLICE 40 (2026-07-24) — ***FRONT C LANDS.*** Real Chromium in a TobyTK window:
## example.com rendered over the DevTools pipe, blitted live on the tobyOS desktop

**Deliverable proven by QMP screendump (logs/shot39_final.png):** the desktop
shows a native "Chromium" TobyTK window whose canvas is a live
Page.captureScreenshot stream from a LONG-LIVED chrome-headless-shell driven
over --remote-debugging-pipe (fds 3/4 wired by pipe+fork+dup2+execve in
programs/chromewin/main.c). example.com is fully rendered: heading, body,
link — https over X25519MLKEM768, SwiftShader-GLES pixels, fontconfig text.

### The wall this slice fell over (and the 2 kernel bugs under it)

Chrome froze at ~4s: BOTH threads state=RUNNING onq=0 forever, syscall ring
frozen, zero ring-3 profiler samples, no exceptions. Diagnosis needed a new
[hb-x] heartbeat line (BKL ticket next/serving, per-CPU cur/holds_bkl/ticks,
lx_do_poll iteration counter). It showed: cpu3 cur=1 holds_bkl=1 at 650k poll
iterations/s, BKL tickets frozen at next=932 serving=930, pid 3 parked at
bkl_lock on cpu0.

1. **sched_yield()'s Milestone-19 fast path kept the BKL (sched.c).** If the
   current proc is RUNNING and the local queue is empty it returned
   immediately — WITH the BKL. lx_do_poll(timeout=-1) waits by looping
   `check fds; sched_yield()` inside ONE syscall body, and enq_target_for
   sends all work to the BSP, so an AP's local queue is ALWAYS empty: chrome's
   pipe-reader thread (the FIRST thing a --remote-debugging-pipe chrome parks)
   spun the fast path forever holding the BKL and every other syscall on every
   CPU wedged at bkl_lock. FIX: the fast path now does
   `if (me->holds_bkl) { bkl_exit(); bkl_enter(); }` — the fair ticket queue
   hands the lock to any waiter. This bug predates chrome; any Linux proc
   doing an infinite poll/select on an AP could freeze the whole user plane.

2. **ABI_SYS_GUI_BLIT/_BLEND dereferenced the user pixel buffer raw
   (syscall.c).** First frame blit under -cpu +smap = kernel panic (P=1
   kernel-read fault, AC clear, "NO VMA (0 total)"). The GUI syscalls were
   never converted to the per-copy uaccess model (default QEMU CPU has no
   SMAP, so TK apps never tripped it). FIX: user_range_ok +
   uaccess_prepare_read + one uaccess_begin/end window around the blit.
   (GUI_GETPIXELS was already staged through a kernel row buffer.)

Also: **--no-sandbox was missing from chromewin's argv** — chrome exit(1)s at
~4s ("Running as root without --no-sandbox"), which burned a full 440s run
that looked like a hang (the repeating [3] exit_group in the hb ring was the
corpse, not activity). And **the first https handshake exceeds the origin's
timeout under TCG** (ServerHello to Finished took 15s; server FIN'd) — chromewin
now re-navigates every 45s while the frame PNG size never changes; retry 2
rode TLS session resumption and painted (2727 -> 11573 bytes, the exact
console-run screenshot size).

### chromewin host (programs/chromewin/main.c, /bin/chromewin, TKAPP_BOOT)
- pipe()+fork(), dup2 via out-of-range temp fds onto 3/4, raw ABI_SYS_EXECVE.
- CDP flat session: Target.createTarget(url) -> attachToTarget(flatten) ->
  Page.enable -> Page.captureScreenshot @~2.5fps -> b64_decode ->
  toby_image_load -> tk_draw_blit. 100+ frames/run, stable, no leaks observed.
- Input.dispatchMouseEvent/KeyEvent wired from TK_EV_* (implemented, untested).
- Nav-retry loop as above; ESC quits.

### Instruments added (CHROMIUM_BOOT-gated, keep)
- [hb-x]: BKL ticket state + per-CPU current/holds_bkl/timer_ticks +
  lx_do_poll iteration counter in the 3s heartbeat — THE line that cracked
  the freeze; cheap, leave it in.
- [devpipe]: LX_read/LX_write traces on fds 3/4.
- oncpu= column in the per-proc heartbeat line.

### Parked / next
- Input routing end-to-end test (QMP input-send-event -> TK -> CDP -> page).
- First-handshake latency: warm the TLS session (early throwaway fetch) or
  raise capture cadence only after first paint.
- The slice-38 teardown flake (silent death ~4s after chrome exit) unseen
  in ~8 slice-40 runs; keep -d cpu_reset in the harness.


## SLICE 41 (2026-07-24) — ***WHPX FRAME PRODUCTION.*** example.com renders live
## under hardware virtualization; the clock was a red herring, the BKL was the wall

Picking up the WHPX handoff (Phase 1: "fix frame production under WHPX"). Prior
probe: chrome bootstrapped in ~12s under WHPX but returned **ZERO** frames in
250s, with a `clock_gettime` storm + ~100s network gaps. Two named suspects, in
order: (1) `clock_gettime` calibration/resolution, (2) BKL contention. Handoff
said probe the clock first (cheap insurance) — do NOT fix it blind.

### Probe first: the clock is FINE under WHPX (suspect 1 DISPROVEN)
Two CHROMIUM_BOOT probes added:
- `[clkchk]` (syscall.c): flags any CLOCK_MONOTONIC read that goes backwards OR
  jumps >250ms vs the previous read, tagging cross-CPU transitions.
- per-CPU `g_cpu_mono_ns[]` sampled every timer tick, printed in `[hb-x]`.

Result under WHPX `-smp 4`: the per-CPU mono columns AGREE — cpu0=246660,
cpu1/2/3=246738, bsp-now=246750 ms; the ~80ms spread is pure sample-age (cpu0
takes fewer ticks). Every `[clkchk]` hit is FORWARD and monotonic — the 300-570ms
deltas are early-boot gaps where chrome simply didn't read the clock for that
long, and the two `[CPU-CHANGED]` hits are forward too. **perf_now_ns() is
already TSC-backed (nanosecond, calibrated vs the ACPI PM timer) and WHPX keeps
the per-vCPU TSCs synchronised** — no calibration bug, no desync. The slice-33
worry about unsynced per-CPU TSCs does NOT manifest under WHPX. Priority flipped
exactly as the handoff predicted: clock fine ⇒ the BKL is the sole lever.

### The fix: serve pure time reads WITHOUT the BKL
Every syscall takes the BKL for its whole body (syscall_dispatch). WHPX raises
the syscall rate ~100x, so chrome's message-pump clock storm (tens of
thousands/sec of clock_gettime/gettimeofday) turned the BKL into the system-wide
throughput cap and starved the compositor/network threads that must run to answer
`Page.captureScreenshot` — hence zero frames.

Those calls read only the monotonic TSC clock (no mutable kernel state) and write
a tiny result. New lock-free fast path in syscall_dispatch, BEFORE `bkl_enter()`:
- `linux_fast_time_syscall()` handles clock_gettime / gettimeofday / clock_getres:
  compute from perf_now_ns(), copy with `copy_to_user_nofault()` — a new
  write-side twin of copy_from_user_nofault built on `uaccess_probe_writable()`
  (pure page-table read: present+writable, no fault, no allocation, no vmm
  mutation ⇒ safe without the BKL and concurrently, same class as the demand
  faults that already run BKL-free across CPUs since slice 39).
- Taken ONLY for ABI_PERS_LINUX procs with no pending signal and an
  already-resident+writable buffer. Anything else (unknown clock id, non-resident
  buffer, native personality) returns 0 and falls through to the normal BKL path
  unchanged. Native OS procs never hit it (personality gate).

### Result: PHASE 1 MET
Same WHPX run (300s): **~470 continuous Page.captureScreenshot frames** (~2fps,
png=3174) vs the prior run's ZERO. QMP screendump (logs/shotwx_b.png) shows
example.com FULLY RENDERED in the native Chromium TobyTK window — "Example
Domain" heading, body copy, blue "Learn more" link on #eee — over HTTPS
(X25519MLKEM768; tcp[2] -> 104.20.23.154:443, full TLS 1.3 flight), SwiftShader
pixels, fontconfig text. First frame at guest ~12.5s (chrome start ~4.3s -> TLS
11.2s -> paint 12.5s). BKL healthy (next-serving=3, never wedged). Base OS
unregressed: clean-config build (no CHROMIUM_BOOT) + TCG defboot reaches
login/desktop, ZERO faults.

### Standing / next (Phase 2 = YouTube)
- `pollit=344M`: poll/select/epoll are still cooperative BUSY-WAIT loops
  (`scan fds; sched_yield()`), so idle waiters pin host cores at 100% under WHPX.
  Fine for example.com (frames flow); likely the next bottleneck for YouTube's
  much heavier V8/render load. Making poll/epoll BLOCK (sleep on fd readiness +
  a timer deadline) instead of spin is the obvious follow-up.
- Instruments kept (CHROMIUM_BOOT-gated): `[clkchk]`, per-CPU `mono=` in `[hb-x]`.
- Committed run39whpx.py / run39shot.py / build39.sh (were on-disk only), per the
  handoff, so this WHPX result is reproducible.


## SLICE 42 (2026-07-24) — Phase 2 (YouTube) root-caused: no true blocking wait ⇒ busy-spin CPU starvation

Flipped chromewin's START_URL to https://www.youtube.com/ and ran the fixed WHPX
build (logs/run41yt.py: -m 6144, 360s). Chrome bootstraps and starts loading —
but **produces ZERO frames** and the window stays on "connecting to chrome…".
Reverted START_URL to example.com afterward (keep the verified-green default);
the youtube runner (run41yt.py) is committed for the next session.

### What the run shows (logs/run41yt.log, logs/ytwx_b.png)
- CDP bootstrap completes fast (sessionId 8.5s, "bootstrap OK; polling
  screenshots" 10.4s), then `Page.captureScreenshot` is NEVER answered in 350s.
- First youtube TLS handshake is FAST, not slow: tcp[2] ClientHello 9.95s →
  ServerHello 9.98s → app-data (0x17) by 10.4s → a little more to 13.1s. Then
  **tcp[2] goes silent for ~96s** (next TX 109.7s). Same ~100s gaps recur
  (connect bursts at ~110s, ~160s, ~210s). The network is not blocked — it
  CRAWLS.
- System monitor: **CPU pegged 100%**, RAM 3.1/7.1 GiB (no OOM, no faults, no
  panic). `[hb-x] pollit` climbs 0 → **632,065,719** over the run (~2M poll
  iterations/sec). Wait-graph: ~20 threads futex/epoll-blocked for 60–211s.

### Root cause: tobyOS has NO true blocking wait — everything busy-spins
poll / select / epoll_wait (syscall.c lx_do_poll/lx_do_select/lx_epoll_wait),
nanosleep (sys_nanosleep), and timed-futex ALL implement "waiting" as a
`scan; sched_yield(); pause` busy-loop. sys_nanosleep's own comment says why
they refuse to `hlt`: "once every core is halted, headless QEMU/TCG stops
delivering the timer interrupt that would wake us → silent stall." So the design
trades true blocking for a spin that is merely SLOW under TCG.

Under WHPX at hardware speed this collapses: chrome parks ~20 idle worker
threads in epoll_wait(long/∞ timeout). Each stays `PROC_RUNNING` and busy-spins
instead of leaving the run queue, so `sched_yield` round-robins the CPU across
~25 runnable procs and the handful doing REAL work (V8 parsing YouTube's
multi-MB JS, the network service, the compositor) each get ~1/25 of a core.
Timers/delayed tasks therefore fire ~100s late (the thread that would run them
rarely gets scheduled), the page never finishes loading, and the renderer never
produces a frame for captureScreenshot. It is STARVATION, not deadlock — chrome
crawls forward (connections at 10s, 110s, 160s, 210s). example.com renders under
WHPX (slice 41) only because it is light enough that 1/25 of a core still
finishes; YouTube is not.

### The fix (next arc — deliberately NOT rushed here)
Give idle waiters a way to LEAVE the run queue: convert poll/epoll/select (and
ideally nanosleep + timed-futex) from busy-spin to true BLOCK — deschedule
(`PROC_BLOCKED`), wake on (a) an fd-readiness event (socket rx / eventfd write /
pipe write signalling a wait queue the poller registered on — a coarse global
"poll wakeup" generation is an acceptable first cut given the single BKL) or
(b) a timer deadline. The precondition the sys_nanosleep comment flags — a
halted core must still be woken — is satisfiable with a reliable PER-CPU LAPIC
periodic timer so every core wakes on its own tick even when all are idle;
verify that under WHPX before trusting `hlt`. This touches the wait path EVERY
Linux program uses (and the working example.com render + all linux-* tests), so
it needs its own session with a targeted before/after (a `/bin/linux-poll`-style
unit test: park a thread in epoll_wait, prove it wakes on write AND consumes ~0
CPU while parked) and a full re-validation (example.com still renders, defboot
clean, linux-futex/eventfd/mapshare green). Do NOT ship it without that.

### Standing
Phase 1 (WHPX frame production) is DONE and shipped (slice 41). Phase 2's wall is
now root-caused to a single, well-scoped kernel deficiency. The `[hb-x] pollit`
counter is the metric to watch: the blocking fix should drop it by orders of
magnitude and let YouTube's real threads run.


## SLICE 43 (2026-07-24) — ***YOUTUBE RENDERS.*** Blocking poll/epoll kills the busy-spin starvation; the futex-under-WHPX lost-wakeup is the next wall

Slice 42 root-caused YouTube's stall to CPU starvation: poll/select/epoll_wait
were cooperative BUSY-WAIT loops (`scan fds; sched_yield()`), so ~20 idle chrome
worker threads stayed PROC_RUNNING on the ready queue and starved the V8/network/
compositor threads to ~1/25 of a core. This slice makes those waiters truly
BLOCK, and YouTube's web app now loads and renders.

### The fix: true blocking poll/select/epoll_wait (thread.c)
New `poll_wait_block()` parks the caller (PROC_BLOCKED, off the ready queue),
mirroring pipe.c's wq_add: BKL-serialised, linked via `next_wait`, with
`wait_head` set so signal_send()'s wait_queue_unlink can splice a signalled
poller out (without that back-pointer it would dangle and a re-add would cycle
the list). A rate-limited `poll_tick()` (~1 ms) requeues every parked poller to
re-scan; each keeps its own deadline as a syscall local, so NO struct-proc field
was added. `poll_forget_proc()` unlinks a dying thread in teardown (both proc.c
sites, next to futex_forget_proc). lx_do_poll/lx_do_select/lx_epoll_wait now call
poll_wait_block() instead of sched_yield().

poll_tick() is driven from three BKL-holding contexts so a parked poller is
always re-scanned even when EVERY user thread is blocked:
  1. sched_yield's slow path (`if (me->holds_bkl)`) -- the under-load driver;
  2. pid 0's idle_loop (desktop / console harness);
  3. **the TKAPP_CHROMEWIN hold loop** -- added after a real freeze: that loop
     (not idle_loop) is what runs while chrome is parked, and its sched_yield()s
     do NOT hold the BKL, so the gated driver in (1) never fired. Result: the
     first example.com run FROZE (bkl tickets + pollit stopped dead, zero
     frames). A direct `bkl_enter(); poll_tick(); bkl_exit();` in the hold loop
     fixed it. LESSON: a BKL-gated periodic must have a driver in whatever loop
     the boot harness actually sits in.

### Validation (before trusting a core wait-path change)
- **EFDTEST PASS** (non-TKAPP CHROMIUM_BOOT under WHPX): epoll_wait parked on an
  eventfd is woken by a cross-thread eventfd write -- proves blocking epoll wakes
  correctly. (First time the linux-* tests ran under WHPX.)
- **example.com regression PASS**: renders under WHPX with ~480 continuous
  frames, and `pollit` fell from ~1.4M/s (slice-41 busy-spin) to ~3.4k/s -- a
  ~450x drop in spin rate; idle CPU now reads 0%.
- **Base OS PASS**: clean-config (no CHROMIUM_BOOT) build + TCG defboot reaches
  login/desktop, zero faults. poll_tick early-outs when no Linux poller is
  parked, so the base OS is unaffected.

### RESULT: YouTube's page loads and renders
`START_URL=https://www.youtube.com/` under WHPX (logs/run41yt.py, logs/ytwx_b.png
+ ytwx_c.png): the real YouTube **Kevlar desktop web app** loads
(`youtube.com/s/_/ytmainappweb/.../kevlar_base...` JS fetched + executed, its
console messages in the log) and RENDERS the homepage -- top nav (hamburger +
avatar buttons), the video-grid layout, and the thumbnail loading skeleton.
Before this slice it was a starved blank "connecting to chrome" at 100% CPU.
chrome SURVIVES (no browser exit; exactly one isolated renderer EXCEPTION 14 +
EXCEPTION 3 over the whole run, not a crash loop). RAM 3.7/7.1 GiB, no OOM.

### The next wall, precisely evidenced: futex lost-wakeup under WHPX
Content (thumbnails) does not fully populate, and ~100s network gaps persist
(tcp[2] at 14s, next batch at 121s). The wait-graph names the cause: chrome
threads stuck in `futex(0xc130ac0) for 197563 ms` and `146738 ms` -- a
FUTEX_WAKE that was LOST and salvaged only by the deadline/uaddr-change fallback.
This is the SAME failure the newly-WHPX-run **FUTEXTEST reports: FAIL exit=1
(lost wakeup)** -- while linux-futex has always PASSed under TCG. So it is a real
futex wake-DELIVERY race exposed by WHPX's true parallelism (TCG serialises vCPUs
and hides it), latent since slice 30 and only now dominant because the poll fix
let YouTube run far enough to stress futex hard. This is the #1 remaining wall
for full YouTube content and the clear next arc: diagnose why a FUTEX_WAKE misses
a FUTEX_WAIT_BITSET waiter under parallel execution (suspect the
add-to-list/release-lock/sched_yield window vs the waker's enqueue, or a
memory-ordering gap), fix it, and keep FUTEXTEST green under BOTH TCG and WHPX.
Note: poll was PROVABLY inert during the futex test (pollit=0), so the FUTEXTEST
FAIL is not caused by this slice.

### Standing
Phase 1 (WHPX frames, slice 41) and now Phase 2 (YouTube page renders, this
slice) are shipped. The dominant remaining bottleneck is a single, isolated,
reproducible core-primitive bug (futex-under-WHPX), with a permanent unit test
(linux-futex) that already flags it. Instruments kept: blocking poll via
poll_wait_block/poll_tick; `pollit` in `[hb-x]` is the spin-rate metric.


## SLICE 44 (2026-07-24) — the "futex-under-WHPX lost-wakeup" is a TEST ARTIFACT: kernel futex works; FUTEXTEST hardened; a slice-43 claim corrected

Took on the slice-43 "#1 wall": FUTEXTEST FAIL under WHPX, believed to be a
futex wake-DELIVERY race blocking YouTube content. Instrumented the futex
wait/wake path ([fx] trace) and reproduced under WHPX. The trace REFUTES the
lost-wakeup theory:

```
[3696] [futextest] start
[4901] [fx] pid=2 WAKE addr=2065f8 NO-WAITER        <- waker fires, NO waiter yet
[5563] [fx] pid=3 WAIT block addr=2065f8 dl=15554   <- waiter parks 662ms LATER
[7091] [futextest] FAIL ... lost wakeup
[10936] [fx] pid=2 WAIT block addr=7ffffffff1f8 dl=0
[10937] [fx] pid=3 WAKE addr=7ffffffff1f8 -> woke pid=2 on_cpu=0 on_rq=1 queued_cpu=0
[10939] [fx] pid=2 WAIT woke after=3ms timed_out=0  <- wake DELIVERS in 3ms
```

**The kernel futex is CORRECT.** When a wait precedes its wake, delivery is 3ms
(bottom). FUTEXTEST fails for a different reason: the waker's FUTEX_WAKE fired at
4901ms with NO waiter on the list, because the waiter did not park until 5563ms.
It is a **wake-before-wait** race: the waiter took ~862ms to get from setting its
userspace ready-flag (`g_waiter_ready=1`, just before the futex syscall) to
actually entering FUTEX_WAIT -- i.e. **scheduler latency** (preempted after the
flag, rescheduled ~1s later). The test's handshake (ready-flag + a fixed 200ms
sleep) is too weak for WHPX's true-parallel scheduling latency; TCG serialises
the two threads and hides it, which is why linux-futex has always PASSed on TCG.

### Correction to slice 43
The claim "futex lost-wakeup is the #1 YouTube wall" was **OVER-ATTRIBUTED**.
There is no kernel futex bug. The 147-200s YouTube futex waits are chrome's idle
thread-pool workers parked on purpose (the same pattern the DOM-front notes
flagged as normal), NOT lost wakeups. Fixing "futex" would not have unblocked
YouTube content.

### Fix: harden the test (programs/linux-futex/main.c)
Retry FUTEX_WAKE until it reports it woke someone (`nwoken >= 1`) instead of a
single fixed-delay wake. A WAKE returning 0 means the waiter is not on the list
yet -> retry past the park. Once a WAKE returns >= 1 the waiter WAS on the list
AND we woke it -- exactly what the DL2 test must prove. If the DL2 bug regressed
(timed waiter never on the list) every WAKE returns 0, the loop exhausts, and the
waiter escapes only at its 10s deadline -> ret != 0 -> FAIL. **Validated PASS
under BOTH WHPX (~183ms) and TCG** -- the guard is now valid on both
accelerators instead of red-for-the-wrong-reason under WHPX.

### The one real (but UNCONFIRMED-as-bottleneck) lever, parked
Scheduler latency: `enq_target_for()` returns 0, so every woken thread piles onto
the BSP's ready queue and APs must work-steal; a ~862ms park latency was observed
here. It COULD slow YouTube content-load, but it is not established as the
bottleneck (the ~100s network gaps are far larger and may be chrome-internal).
Not chased now. Next: Phase 3 (Page.startScreencast frame push, input routing,
video playback) -- the stated goal, and independent of this.


## SLICE 45 (2026-07-24) — Phase 3a: Page.startScreencast (push frames) replaces polled captureScreenshot; a non-blocking pipe read enables it

Phase 3 groundwork. Polled `Page.captureScreenshot` sends a blocking request that
queues behind a busy renderer -- YouTube returned only ~2 frames because the
renderer never idled to answer it. `Page.startScreencast` instead makes chrome
PUSH a `Page.screencastFrame` event (base64 JPEG + numeric sessionId) on every
damage; each is acked with `Page.screencastFrameAck{sessionId}`.

### The enabler: non-blocking pipe read (kernel)
chromewin is a NATIVE app driving chrome's --remote-debugging-pipe. A push-frame
event loop must drain that pipe WITHOUT a blocking read stalling TK input -- but
tobyOS native apps have no fcntl(O_NONBLOCK) and no working generic poll on pipe
fds (libtoby poll() falls back to sleep+mark-ready). Added a minimal primitive:
- `ABI_SYS_READ_NB` (185) + `sys_read_nb` (syscall.c): like READ but on a pipe
  returns -ABI_EAGAIN instead of parking when momentarily empty (writer alive).
- `pipe_tryread` (pipe.c): the non-blocking pipe drain; EOF=0, empty=-EAGAIN.
Purely additive -- pipe_read's blocking default is untouched, and only a caller
that opts in (chromewin) sees non-blocking behavior.

### chromewin: single-threaded event loop (programs/chromewin/main.c)
Bootstrap now ends with Page.startScreencast (jpeg, q60, 800x600, everyNthFrame
1). The main loop is event-driven: tk_pump forwards mouse/keys as Input.*
(FIRE-AND-FORGET -- a blocking ack would drop frames); then a non-blocking drain
(cdp_fill_nb + cdp_take_msg + cdp_dispatch) blits + acks every buffered
screencastFrame. cdp_wait (bootstrap only) also dispatches frames so one pushed
mid-bootstrap isn't lost. toby_image_load decodes the JPEG (stb_image). Nav-retry
is keyed on g_frames==0 (page NEVER painted) -- a static page renders once then
sends no more frames, so "no frame lately" must not re-navigate (an earlier
version mis-fired 7 re-navs on static example.com).

### Validated
example.com renders under WHPX via pushed JPEG frames (logs/shotwx_b.png), 0
spurious nav-retries, no faults. The full desktop + TobyTK boot and run the new
syscall path cleanly (base OS exercised). Static-page frame count is ~1 by design
(damage-driven). Next: flip to youtube + --autoplay-policy and test whether
startScreencast delivers the continuous frames a loading/animating page (and
video) produce -- the case polled captureScreenshot starved to ~2 frames.


## SLICE 46 (2026-07-24) — Phase 3: startScreencast pays off for YouTube (150+ frames, real homepage UI); video watch page crashes the browser

Exercised slice-45's startScreencast against YouTube + a video watch page.

### Win: startScreencast delivers for YouTube (the frame-delivery gap is closed)
- youtube.com homepage: renders its REAL signed-out UI now -- the Search bar,
  Sign in button, and the "Try searching to get started / Start watching videos
  to help us build a feed" empty-feed card (logs/ytwx_c.png from the homepage
  run). A proper Kevlar render, past the loading skeleton.
- watch?v=... page: startScreencast pushed **156 frames** (frame 30/60/.../150
  logged, ~7-8KB jpeg each) vs the **2** frames polled captureScreenshot got --
  the "screenshot request stalls behind a busy renderer" starvation is gone.

### chromewin fixes made here
- Removed the nav-retry entirely. It re-navigated on g_frames==0 every 45s, but
  YouTube legitimately takes >45s to first paint, so it just RESTARTED the heavy
  load (4 re-navs observed, never finishing). Under WHPX the first navigation's
  TLS handshake is already fast (slice 41); Target.createTarget(url) navigates
  and startScreencast paints when ready -- no retry needed.
- Added --autoplay-policy=no-user-gesture-required (for video autoplay; chrome
  uses a null audio sink, and VP9/AV1+Opus are present).

### Wall: video playback crashes the browser before the player paints
The watch page navigates (URL correct in the window) but the page area stays
WHITE and never shows the player. chrome's BROWSER process (pid 3) works hard --
27.0s CPU, 273,689 syscalls -- then **crashes: EXCEPTION 14 (user page fault),
pid 3 exit=-1 at ~247s**. chromewin handles it gracefully (sees the pipe EOF,
exits clean, 156 frames delivered). No OOM (7 GiB RAM). So the watch page is the
heaviest tier yet and chrome dies under its load (media pipeline + far more JS
than the homepage) before painting a visible player.

This is a fresh, deep frontier -- the same class as the earlier render/GL crash
arcs (slices 37-39): symbolize the EXCEPTION 14 (rip/cr2 -> which .so, which
chrome/tobyOS interaction), likely a tobyOS bug exposed only by the watch page's
heavier mmap/thread/GPU pressure, or a chrome fatal under the media stack.
Not chased this slice.

### Standing
Phase 3a (startScreencast) is DONE and clearly pays off for YouTube (156 vs 2
frames; homepage renders its real UI). Input routing (Phase 3b) is still untested
(the homepage's feed is empty -> no thumbnail to click; the watch page crashed).
Video playback is the open frontier, gated on the watch-page browser crash.
example.com remains the green default and renders without the nav-retry.


## SLICE 47 (2026-07-24) — the video-watch-page browser crash ROOT-CAUSED + FIXED: an unlocked VMA-table race under chrome's W^X mprotect storm

Chased the slice-46 wall (chrome's browser process crashes on a YouTube watch
page, EXCEPTION 14 err=0x6, ~247s). Symbolized it fully.

### Root cause: fault handler reads a VMA MID-SPLIT during a concurrent mprotect
The fault: `rip` in chrome's main .text, `err=0x6` (user WRITE, not-present),
`cr2=0x102408e02020` in chrome's mmap region. The handler REFUSED it:
```
[pfrej] pid=3 addr=0x102408e02020 err=0x6 WRITE to non-writable VMA
        [0x102401a02000, 0x102b01a04000) prot=0x0 flags=0xd
```
So `vma_find_internal` (first-match) found a GIANT ~27GB PROT_NONE VMA (V8's
pointer-cage reservation) covering the address. But the isr diagnostic, running
moments later on the SAME table, found a small 1-page RW VMA
`[0x102408e02000,0x102408e03000)` with NO overlap. Same address, same table,
different answer -> the table CHANGED between the two reads = a concurrent
mutation.

The `[mprot]` ring named it: chrome hammers a W^X pattern 27,498 times --
`mprotect(0x102408e00000, 2MB, PROT_NONE)` then `mprotect(0x102408e02000, 1page,
RW)`. mmap/munmap/mprotect run UNDER the BKL, but the #PF handler runs BKL-FREE
(fault concurrency, slice 39). So a write-fault on one CPU read the giant cage
VMA while a BKL-holding mprotect on another CPU was HALFWAY through carving the
RW sub-page (the giant's `->end` not yet shrunk past cr2), and `vma_find_internal`
returned the stale giant PROT_NONE -> "WRITE to non-writable VMA" -> SIGSEGV ->
browser dies mid-video. A textbook unlocked read/write race on `g_vma_tables`.

### Fix (src/mmap.c): re-verify a refusal under the BKL
The resolver is now `mmap_try_fault()`, run LOCKLESS first (the fast, common
path -- millions of demand-fills stay BKL-free). Only if it REFUSES does
`mmap_handle_page_fault` re-verify ONCE under the BKL, which serializes with
every VMA mutator, so the table is guaranteed split-free and consistent. A
genuine bad access still refuses; a transient-race refusal now resolves. IRQs
are enabled around `bkl_enter` so its spin still ACKs the TLB-shootdown IPI the
waited-on mprotect issues under the BKL (else deadlock). A copy_to/from_user
fault from a syscall already holds the BKL, so it skips the retry. Every
return-false path is side-effect-free, so re-running is safe. This is a GENERAL
fix -- any Linux program doing concurrent mprotect + touch on other threads
could hit it; it is not YouTube-specific.

### Validated
The racy refusal is GONE: **0** "WRITE to non-writable VMA" across a full watch-
page run (was the fatal one). chrome now survives ~30s FURTHER (247s -> 277s).
example.com still renders under WHPX (no regression); base-OS defboot reaches
desktop with zero faults.

### The wall one layer deeper (new, separate)
With the VMA race fixed, the watch page hits a DIFFERENT crash: a NULL-pointer
READ -- `cr2=0x6, err=0x4, rdi=0x0` (offset 6 off a NULL base) at rip in chrome
.text, in a chrome WORKER thread (pid 52 `+T`, exit=-1) ~277s -- and that run
rendered 0 frames (the watch page is non-deterministic; a prior run pushed 156).
Something returns NULL that chrome dereferences -- a separate, deeper bug (likely
a tobyOS resource/syscall returning NULL under the media pipeline, or a genuine
chrome fatal). Video playback remains blocked, now one layer in. Homepage render
+ startScreencast (slices 43/45/46) are unaffected and solid.


## SLICE 48 (2026-07-24) — Phase 3b: input routing VALIDATED end-to-end (keyboard + mouse -> chrome)

Verified chromewin forwards TobyTK input to chrome over CDP. QMP injects events
into tobyOS's emulated PS/2 devices (logs/run47input.py); a temporary
`[chromewin] input ...` probe in send_key/send_mouse confirmed the forward:

- **Keyboard:** injected "hello" -> `[chromewin] input key=0x68/0x65/0x6c/0x6c/
  0x6f -> chrome` -- each key went on_key -> send_key -> Input.dispatchKeyEvent.
  Keys need no cursor positioning (they go to the FOCUSED window), so this is the
  clean proof; it fires once g_session is set (~55s), independent of page paint.
- **Mouse:** a click landed `[chromewin] input mousePressed x=595 y=368 ->
  chrome` + mouseReleased -- on_event -> send_mouse -> Input.dispatchMouseEvent.

So the full path works: QMP -> tobyOS PS/2 -> GUI -> chromewin on_key/on_event ->
CDP Input.* -> chrome. The probe was removed after validation (chromewin is back
to its slice-46 state).

### Harness notes (for whoever automates input next)
- `usb-tablet` (absolute pointer) is NOT claimed by tobyOS's HID driver ("no boot
  HID driver claimed device") -- only boot-protocol PS/2 keyboard+mouse work. So
  there is no absolute pointer; QMP `abs` events do nothing.
- The PS/2 mouse applies ACCELERATION, so QMP `rel` deltas overshoot (a +290 unit
  step lands ~1100px). Homing to a corner then small steps works but is
  imprecise -- fine for "click somewhere on the page", not for hitting a small
  link reliably. Absolute clicking would need a tobyOS usb-tablet/HID report
  driver.
- example.com's render under WHPX is NON-DETERMINISTIC (~half the runs paint by
  ~60s, the rest sit on "connecting to chrome"); the keyboard proof sidesteps
  this since it does not need a painted page.

### Phase 3 standing
3a startScreencast (slice 45) + 3b input routing (this slice) are DONE. YouTube's
homepage renders its real UI (slice 46). Video playback remains blocked one layer
past the (now-fixed) VMA race, on a NULL-deref in a chrome worker (slice 47).


## SLICE 49 (2026-07-27) — ***VIDEO DECODES + PLAYS.*** A minimal local VP9 clip paints MOVING frames; the YouTube crash is therefore app/MSE-level, not core decode

Picked up the video handoff (`docs/chromium-video-handoff.md`). The prime move
(handoff §5.C) was a fast, deterministic **minimal `<video>` repro** to split
"does any video decode+paint" from "does YouTube's heavy app get there" — instead
of chasing the flaky 277s YouTube NULL-deref cold. It paid off decisively.

### Result: chrome decodes VP9 and paints MOVING video on tobyOS
Navigated chromewin directly at a raw `file:///opt/chrome/vid.webm` (a 14KB VP9
`testsrc2` clip, 128x96, generated with ffmpeg). Chrome renders it as a native
MediaDocument `<video>` player; a CDP `Runtime.evaluate` "user gesture" kick
(`document.querySelector('video').play()`, re-issued a few times) starts playback
(a MediaDocument does NOT autoplay even with `--autoplay-policy`). Under WHPX:
- **bootstrap OK, no crash** (0 EXCEPTION / exit=-1), chrome alive the whole run.
- **Continuous frame production**: screencast pushed frames 1→30→60→90→120→150
  over ~18s (~8 fps = the clip's rate, ~9 loops).
- **Motion is visual + measured**: the testsrc2 burned-in timecode advanced across
  screendumps (`vid_a`=00:00:00.500 frame 4; `vid_b`=00:00:00.125 frame 1) with the
  player controls auto-hidden = playing. `logs/vid_a.png` shows the decoded color
  bars + moving block through SwiftShader.

So the **media pipeline (VP9 decode -> SwiftShader composite -> paint) WORKS.**
The slice-47 YouTube renderer-worker NULL-deref (277s, ICU4X/interpreter Vec-walk)
is **not** a broken decoder — it is higher up: MSE streaming (YouTube uses Media
Source Extensions, not progressive `<video>`), YouTube's app JS, or i18n under
memory pressure. This retires the "media pipeline returns NULL" hypothesis
(handoff §5.B.1) as the direct cause.

### Harness artifacts found + worked around (NOT the video bug; documented so the
### next agent does not re-chase them)
- **`file://` serves `.html` as `text/plain`** on tobyOS chrome (the raw HTML
  rendered as literal text; `<video>` never parsed). A `.webm` file:// nav works
  (content-sniffed to video/webm -> MediaDocument), so file:// I/O is fine; only
  the HTML extension->MIME resolves wrong. chrome's `kPrimaryMappings` *should* map
  html->text/html unconditionally, so this is an unexplained tobyOS-specific gap in
  chrome's file:// MIME path. Sidestepped by the direct-webm nav. (A future clean
  local-HTML test would need this fixed, or a small `data:text/html;base64` URL.)
- **A large (~83KB) `data:text/html;base64` URL crashes the NetworkService.** To
  render local HTML I first had chromewin read the page + navigate to a data: URL
  (base64 encoder + big-URL path added to `programs/chromewin/main.c`). With the
  full 62KB page that reproduced a **NetworkService (`--type=utility
  network.mojom.NetworkService`, pid 26) CHECK-crash**: chrome logged
  `VALIDATION_ERROR_UNEXPECTED_STRUCT_HEADER` / "Received bad user message" then
  `int3;ud2` (base::ImmediateCrash). Small data: URLs (~20KB) did NOT crash the
  network service (they stalled at Page.enable instead — likely WHPX
  non-determinism). So a large data:-URL main-document navigation corrupts/oversizes
  the Mojo message the browser hands the NetworkService. Interesting as a *possibly
  real* shared-memory/Mojo-message issue (same error class as the memory file's
  old "ipcz VALIDATION_ERROR" phantom), but it is a test-harness trigger here, not
  the video path — PARKED, not chased.

### chromewin display-decoder follow-up (secondary, does NOT affect the finding)
After ~150 successfully-decoded frames the display FROZE while chrome kept
streaming: `toby_image_load` (stb_image, chromewin's screencast JPEG decoder)
began failing on the busier ~8KB frames (888 failures; the ~6KB frames decoded).
`toby_image_free` is leak-free, so the "works-for-N-then-all-fail" shape points to
**heap fragmentation from per-frame ~1.9MB (800x600x4) alloc/free churn** starving
a contiguous alloc, mis-reported as "JPEG decode failed". This caps sustained
high-frame-rate video DISPLAY (and could dim observation of any busy page), but is
orthogonal to decode/compositing (which chrome does; the frames arrive fine).
Follow-up options: reuse a fixed decode buffer, shrink the screencast size, or
switch screencast format to PNG (also enabled in `libtoby/src/image.c`).

### Repro / mechanics
`programs/chromewin/main.c` `START_URL` -> `file:///opt/chrome/vid.webm` (+ the new
`kick_play()` after startScreencast). Stage `vid.webm` into
`programs/chromium/chrome-headless-shell-linux64/` (gitignored, copied to
/opt/chrome). `bash logs/build_vid.sh` (chromewin + initrd + iso only, no kernel
touch), `python logs/run_vid.py` (WHPX, screendumps `logs/vid_{a,b,c,d}.png`).
Generate the clip: `ffmpeg -f lavfi -i testsrc2=size=128x96:rate=8:duration=2
-c:v libvpx-vp9 -b:v 60k -pix_fmt yuv420p -an vid.webm`.

### Next
The media pipeline is proven, so the remaining YouTube goal is the app/MSE tier:
either (a) a lighter real YouTube surface (`youtube.com/embed/<id>` — same MSE +
player JS, far lighter shell) to reproduce the 277s crash faster, or (b) a local
MSE `SourceBuffer.appendBuffer` test (needs the file:// HTML MIME fix or a small
data:text/html page) to isolate MSE, or (c) instrument the slice-47 NULL-deref
source directly (now knowing it is not decode).


## SLICE 50 (2026-07-27) — ***YOUTUBE CRASH ROOT-CAUSED = MEMORY CORRUPTION (not a NULL-return); a principled free-before-shootdown fix REDUCES but does NOT fully eliminate it.*** The slice-47 "NULL-deref" is a stale-TLB/freed-page corruption

**HONEST STATUS (corrected):** the ROOT CAUSE below (memory corruption, not a
syscall returning NULL) is SOLID — proven by ASCII text in a pointer register. The
FIX below (defer frees behind a TLB shootdown in munmap/madvise/brk) is a CORRECT
improvement, but it is **NOT yet demonstrated to eliminate the crash**: after the
fix, an embed run + a watch run were clean, but a SUBSEQUENT watch run **still
crashed at the same rip 0x208c13a (rbx=NULL) at 234s**. So there is at least one
RESIDUAL freed-page-reuse / stale-TLB window (or the shootdown-timeout path leaks),
and the crash rate is not yet measured over enough runs. Do not treat this as a
closed fix.

Took slice-49's "next" step (a) — `youtube.com/embed/<id>` — and it paid off twice:
it reproduced the crash **FASTER + more diagnostically**, and the diagnosis led
straight to a real kernel bug with a principled fix.

### The embed reproduced the crash at 65s with a decisive fingerprint
Navigated chromewin at `youtube.com/embed/aqz-KE-bpKQ?autoplay=1&mute=1`. The embed
player renders its REAL UI (bootstrap OK, frames flow), then a renderer WORKER
thread (pid 49, tgid 34) faults at **65s** — much faster + more reliable than the
watch page's 277s. The fault is at the **byte-identical instruction** as the
slice-47 watch-page crash: **rip=0x208c13a** (`movzbl 0x6(%rbx),%eax; cmp $0x8f,%al;
ja` — the ICU4X/Rust interpreter Vec-walk; `rbx` = a 24-byte Vec node's pointer
field). But this time it is **EXCEPTION 13 #GP, not #14 #PF**, because:
```
rbx=0x726f687475617b7b   (little-endian ASCII "{{author")
r14=0x646e615f = "_and"   r9=0x223a47482c22726f = 'or",HG:"'   stack: "network."
```
`rbx` holds **ASCII text where a pointer belongs** (non-canonical addr -> #GP). So
the slice-47 "chrome dereferences a NULL" was really **MEMORY CORRUPTION**: string
bytes overwrote a pointer in the parser's node array. NULL (watch page) and
"{{author" (embed) are two faces of the SAME root cause at the SAME rip.

### Root cause: freed physical frames reissued BEFORE the TLB shootdown
The faulting group's recent syscalls are an `mprotect`+`madvise(MADV_DONTNEED)`
storm. `sys_munmap` / `sys_madvise_dontneed` (src/mmap.c) did, per page:
`vmm_unmap(PTE)` -> `pmm_free_page(frame)` **inside the loop**, then ONE
`tlb_shootdown_remote()` **after** the loop. The window between freeing frame 1 and
the end-of-range shootdown is the bug:
- `pmm_free_page` returns a frame to the PMM immediately (own `g_pmm_lock`, no
  quarantine) — reusable at once.
- The **demand-fault** path `mmap_try_fault` calls `pmm_alloc_page` **BKL-FREE**
  (before its BKL retry). So while a BKL-holding munmap/madvise walks a multi-page
  range, a concurrent fault on another CPU can re-hand-out a just-freed frame,
  zero+refill it (JSON/config **string** bytes), while a THIRD CPU still has the OLD
  virtual->physical translation cached and reads that frame through its **stale TLB**
  -> reads "{{author" where its own pointer used to be. (apic.c's own shootdown
  comment already documented this class from slice 38's CoW ghost-frame corruption;
  the free path just never closed it.)

### Fix (src/mmap.c): shoot down BEFORE returning frames to the PMM
Added `struct mmap_free_batch` (bounded on-stack, 256 frames). munmap/madvise now
`vmm_unmap` + defer the frame into the batch; on a full batch (or at end)
`mmap_free_batch_flush` does `tlb_shootdown_remote()` **then** `pmm_free_page` for
each — so no frame is reissuable until a shootdown has followed its unmap. An empty
batch still forces the final shootdown (preserves the old guarantee for
refs>1/nofree unmaps). Amortized O(pages); correct regardless of any other
corruption source.

### Measured (partial — DO NOT overclaim)
- **Before fix:** the ONE pre-fix embed run crashed at 65s (rip 0x208c13a, #GP,
  rbx="{{author"). (n=1; the watch page was already non-deterministic pre-fix per
  the handoff, so "deterministic" is a weak claim.)
- **After fix:** embed run = clean 300s (0×0x208c13a); watch run #1 = clean 360s
  (0×0x208c13a, rendered the real player); watch run #2 = **CRASHED at 234s, rip
  0x208c13a, rbx=NULL** (6 occurrences). So the crash still happens. Sample too
  small to claim a rate change. 8 benign `[tlb] shootdown ack timeout` warnings per
  run (bounded-wait path; a parked-IRQ-off CPU takes the pending IPI before its next
  user access — but WORTH AUDITING whether that guarantee truly holds, since a
  timed-out shootdown that DID leave a stale writable TLB would reintroduce exactly
  this corruption).
- **Base OS clean:** `logs/defboot.sh` (stock non-CHROMIUM build, the fix is
  unconditional) reaches login/session with **ZERO faults**. No regression from the
  change itself.

### Residual leads (the corruption is multi-window / not closed)
1. Other user-frame free paths beyond munmap/madvise/brk (now all batched): **memfd
   teardown** (mmap.c ~1011, refcount->0 free) and process **exit/exec teardown**.
   fork.c:499/760 are error-cleanup of UNmapped just-alloc'd pages (safe). CoW
   copy-out DOES shoot down (page_fault.c:197) — safe.
2. The **shootdown-timeout** path (apic.c tlb_shootdown_remote bounded wait): if it
   returns having NOT flushed a CPU that then reads/writes user memory before taking
   the pending IPI, the freed frame is still reachable. Audit the IRET-to-user /
   bkl_enter re-entry ordering.
3. A stale-TLB **WRITE** (not just read): chrome writes a pointer to a VA whose old
   writable TLB (post-madvise-drop-then-refault, or post-CoW) points at the wrong
   frame -> the pointer lands in the wrong page -> a flushed CPU reads NULL. This is
   the NULL variant; the fix should cover it IF shootdowns never leak.
4. Re-verify the fix is actually reducing anything with a proper N-run crash-rate
   measurement before/after (this slice's N is too small).

### Note: YouTube embed "Error 153"
The embed player shows "Error 153 — Video player configuration error". This is
SEPARATE from the corruption crash (it persists after the fix, with chrome stable),
i.e. a YouTube-side embed/config restriction for this URL, not a tobyOS fault.

### Watch page (the handoff's real target) WITH the fix
On a CLEAN run the watch page now **renders YouTube's real Kevlar video player**:
the player rectangle with controls (a PAUSE glyph = playing state), a progress bar,
volume, and a buffering SPINNER, plus the metadata skeleton. On the one clean run it
survived the full 360s (was crashing ~277s). HOWEVER: in every observed run the
player stayed at the **buffering spinner** — the MSE video never reached VISIBLE
decoded frames (unlike the local .webm, which did). The ~30fps of damage chrome
streamed after the spinner was likely the spinner animation + Kevlar UI, NOT
confirmed video. And the corruption crash recurs (234s / 267s on two later runs),
killing the renderer before buffering could resolve. So "YouTube video plays" is
NOT achieved: the two open walls are (1) the residual corruption crash, and (2)
whatever keeps the MSE stream stuck buffering (network under WHPX? an MSE/decode
gap? or just the crash interrupting it — untestable until the crash is closed).
Also, chromewin's stb-image JPEG decoder could not keep up with 30fps 800x600 and
its `g_rxbuf` overflow-drop truncated frames -> the DISPLAY froze at ~frame 150
while chrome kept streaming (NOT the media pipeline). Fixed the flow-control:
startScreencast throttled to 640x480 / everyNthFrame=3 and the overflow-drop is now
message-boundary aligned (drop whole buffered messages up to the last NUL, never
mid-frame). With that, a clean run should SHOW the video actually playing — still to
be confirmed on a run that doesn't hit the residual corruption crash.

### Repro / mechanics
`programs/chromewin/main.c` `START_URL` -> the embed or watch URL. `bash
logs/build39.sh` (full kernel rebuild — always after a kernel change), then
`python logs/run_embed.py` (embed, 300s, `logs/emb_*.png`) or `logs/run_watch.py`
(watch, 360s, `logs/wat_*.png`). The fix is in `sys_munmap` + `sys_madvise_dontneed`
+ the `mmap_free_batch` helper in `src/mmap.c`.


## SLICE 50b (2026-07-27) — the RESIDUAL corruption window ROOT-CAUSED in code: map_4k silently REPLACES a present PTE, so two BKL-free demand faults on the same VA discard the winner's writes. Atomic install-if-absent + shootdown quarantine landed; crash-rate validation BLOCKED by a YouTube-side loading outage (proven environmental by A/B revert-test)

### The residual window, found by re-reading the crash-adjacent evidence
Slice 50 left "residual freed-page-reuse window open". Re-examining the ORIGINAL
277s watch-page crash log (run41yt.log): THREE `[vmm] WARN: map_4k: virt ...
already mapped` lines fired within 15ms of the fatal fault — one of them
(0x10240d3a5000) two pages from the crashing Vec buffer (rax=0x10240d3a3800).
`map_4k` (src/vmm.c) UNCONDITIONALLY overwrites a present PTE ("remapping is
legal" per its own comment; ~165k replaces per chrome boot). Combined with the
BKL-free fault path, that is a lost-write race needing NO free/TLB machinery at
all:

1. Threads A+B fault the same freshly-madvise-dropped VA concurrently (chrome
   PartitionAlloc recommit does this constantly under WHPX's real parallelism).
2. Both pass the `vmm_translate == absent` check (TOCTOU), both allocate + zero
   a frame.
3. A maps frame FA, returns; A's thread WRITES (e.g. a pointer) through it.
4. B maps frame FB (zeroed) — map_4k silently REPLACES FA. A's write is GONE.
5. A reader loads the pointer slot -> 0 -> `movzbl 0x6(%rbx)` with rbx=NULL ->
   the exact 0x208c13a signature. (And the replace does only a LOCAL invlpg, so
   A's CPU keeps a stale FA translation -> divergent views -> the ASCII variant.)

### Fix landed (src/vmm.c + src/mmap.c + include/tobyos/vmm.h)
- `vmm_map_page_if_absent(virt, phys, flags)` / `vmm_remap_page_if(virt,
  expected_phys, new_phys, flags)`: single-page installers that do the
  check+install as ONE critical section under `g_vmm_lock` (which all mappers
  already take). Returns 1 installed / 0 lost-race / -1 walk-OOM.
- `mmap_try_fault` demand path: install-if-absent; the LOSER frees its own
  never-visible frame and returns resolved (`[pfrace]` logs it, CHROMIUM_BOOT).
- `mmap_try_fault` CoW path: compare-and-remap (only install our copy if the
  frame is still the one we copied from); loser frees its stale copy.
  NOTE: old_phys is still LEAKED (pre-existing behavior kept) — a first attempt
  that also dec/freed it with munmap's refcount recipe was REVERTED (refcount
  provenance of VMA-CoW frames unverified; freeing a live frame is the exact
  bug class being fixed).
- Shootdown-timeout hole closed: `tlb_shootdown_remote_sync()` (apic.c) reports
  whether every peer ACKED within the bounded wait. `mmap_free_batch_flush` now
  QUARANTINES frames from un-acked flushes (`g_tlbq`, 8192 entries) and only
  releases them after a later fully-acked shootdown; ring-full deliberately
  LEAKS (a lost page beats a corrupted one). Under WHPX ack timeouts are
  routine (host deschedules vCPUs), so this path is exercised for real.
- isr.c fatal dump: `[isr] pointer-source page (rax=...)` — page journal +
  mprot history for the page the corrupted POINTER was loaded from (rax = the
  Vec buffer), not just cr2 (which is a useless tiny offset in these crashes).

### Validation status — honest
- **Local video (file:///opt/chrome/vid.webm): PLAYS on the new kernel** —
  continuous frames the whole 190s run (~300 frames; the slice-50 chromewin
  flow-control fix also holds: no more freeze-at-150). Fault paths + decode +
  render healthy.
- **defboot** (stock, non-CHROMIUM): CLEAN — login + session #1, ZERO faults,
  with the full final fix set (if_absent/remap_if/tlbq are unconditional code).
- **Corruption crash-rate on YouTube: BLOCKED, environmental.** After the fix,
  4/4 YouTube runs (watch x2, embed x2) failed to LOAD (bootstrap OK, ONE tcp
  connect, ~62KB HTML received, then chrome idle forever; historically 5/7
  loaded same-day). A/B REVERT-TEST settled it: the PRE-change kernel (stash +
  rebuild) shows the IDENTICAL no-load shape — so the outage is YouTube-side
  (bot throttling after ~20 hits from this IP today, or a served-page change),
  NOT the kernel. Guest boot milestones are byte-comparable pre/post (login
  3.4s vs 3.5s), killing the "boot got slower" scare (host-cache variance).
  The 0x208c13a crash-rate measurement therefore CANNOT run until YouTube
  loads again — retry after the throttle decays; the race itself is now
  structurally closed (check+install is atomic), and `[pfrace]` will count
  real occurrences when load returns.

### For the next session
1. Re-run `logs/run_watch.py` / `run_embed.py` a few times (later, fresh IP or
   after decay); watch for `[pfrace]` hits (the race firing + being absorbed)
   and for ANY 0x208c13a recurrence (would falsify this fix too).
2. The `[vmm] WARN map_4k already mapped` counter should now be near-ZERO from
   the demand path; remaining replaces come from MAP_FIXED eager-commit in
   sys_mmap (BKL-held, but replaces frames WITHOUT freeing the old frame or
   remote shootdown — a known leak + potential corruption for MAP_FIXED-heavy
   apps; unaudited, next candidate if 0x208c13a ever recurs).
3. page_fault.c native Case-2 demand path writes `*pte` with NO lock — same
   TOCTOU class for native multithreaded apps; unaudited.

### Addendum: permanent guard test DEMRACETEST (landed with 50b)
`programs/linux-demrace` + a `[DEMRACETEST]` boot verdict (kernel.c, next to
FUTEXTEST/MAPTEST, console-harness builds only: `CHROMIUM_BOOT` without
`TKAPP_BOOT`). 4 threads + main in lock-step rounds: madvise(DONTNEED) a page,
barrier, all threads first-touch the SAME page concurrently (each writes its
own slot), barrier, then each verifies its own AND a peer's slot (cross-check
matters: a same-CPU read-back can be satisfied by the thread's own stale TLB
entry for a replaced frame, masking the loss). PASS=3 / FAIL=1 / ERR=2.
Status: **PASS under WHPX** (1500 rounds, ~3s), FUTEXTEST+MAPTEST unregressed.
HONEST CAVEAT: `[pfrace]` = 0 across all runs — the ~1us
translate→alloc→zero→install window was never actually collided (WHPX barrier
-exit skew is ms-scale), so the test's power to DETECT the original bug is
unproven; it guards against gross regressions today. To strengthen: falsify
against the pre-50b kernel, and/or add a debug-define delay inside
mmap_try_fault's window to widen it deterministically. Reproduce:
`make iso EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DCHROMIUM_BOOT"` + a WHPX
boot; verdict prints ~10s in (logs/demrace_whpx.log).

### OPEN CONCERN (end of 50b session): one watch run crashed EARLY in ld.so + wedged
Final watch run of the session (post-50b kernel + DEMRACETEST build via
logs/build39.sh): chrome child pid 33 died at 15.4s guest -- EXCEPTION 14,
rip=0x400246da (ld.so range), cr2=0x0, fs_base=0 (before TLS setup = very early
dynamic-linker work), and the GUI then WEDGED (uniform-color screendumps, no
sessionId/bootstrap, remaining threads parked 290s+). Evidence:
logs/run_watch.log (this signature was never seen in any earlier run). Could be
(a) a genuine 50b regression in early process setup (if_absent path during
execve/ld.so demand-load -- audit whether the demand path's lost-race
return-true is safe for a FILE-BACKED VMA where winner+loser must see the SAME
eagerly-populated content, NOT a zero page), (b) another face of the known
watch-page/WHPX non-determinism, or (c) a stale-build artifact (three define
sets were built back-to-back; build39 does touch src/*.c, so unlikely).
NEXT SESSION: triage FIRST -- re-run 2-3x (any URL; the crash was pre-network),
and if it recurs symbolize rip-0x40000000 offset against ld-linux-x86-64.so.2.
Do NOT trust any YouTube crash-rate stats until this is resolved.

### Triage results (continuation session): ld.so crash 0/3 recurrence; a NEW third signature appeared; YouTube still throttled
- **ld.so strcmp-NULL crash: did NOT recur in 3 reruns** (same iso). Symbolized
  from the sysroot binary: rip 0x400246da = ld.so `strcmp`+0x1a faulting on the
  SECOND argument (rsi=NULL) -- very early (pre-TLS), i.e. a string pointer the
  fresh execve stack should carry (env/auxv, e.g. AT_PLATFORM) read as NULL.
  One occurrence total; PARKED as a rare-corruption data point. File-backed
  audit CLOSED: the demand path zero-fills ALL VMA types (pre-50b behavior too),
  and winner/loser frames are identical zero pages -- the 50b lost-race back-off
  adds no new file-backed semantics.
- **Run 3 hit a NEW, THIRD signature** (logs/run_watch_ebadf.log): renderer
  worker pid 45 (tgid 39), EXCEPTION 14 err=0x6 WRITE to cr2=0xfffffffffffffff7
  = rax = **-9 (-EBADF) used as a POINTER**, rip in a .so, rdi=0xf (fd 15),
  rdx=0x10, preceded by a tight epoll_wait spin. Shape: an fd closed mid-use by
  a sibling thread returned EBADF from a syscall chrome never expects to fail
  that way -> raw return used as an address. This is the FD-LIFETIME class
  (CLONE_FILES shared table), NOT the VM-corruption class. One occurrence;
  PARKED with log preserved.
- **0x208c13a: ZERO occurrences since the 50b fix** (5 post-fix watch runs) --
  but YouTube still is not loading pages (throttle persists, ~7 TLS TX per
  run), so the page-load-heavy crash path remains under-exercised. Crash-rate
  claim still deferred.
- Crash landscape reading: three DISTINCT one-off signatures in chrome workers
  across this arc suggests residual low-rate instability (memory corruption
  and/or fd races) that only heavy load exposes. The instruments to catch them
  are all armed ([pfrace], pointer-source-page autopsy, tlbq, DEMRACETEST).

## SLICE 51 (2026-07-27, continuation) — the "YouTube throttle" is DEAD: the load-stall is tobyOS-side, isolated to the Mojo response-BODY handoff; audits closed: exit-teardown, MAP_FIXED, channel shear (cosmetic)

### The stall is NOT YouTube throttling
example.com STALLS IDENTICALLY (logs/run_vid.log, START_URL=example.com): DNS ok,
TCP connect (104.20.23.154 -- example.com now fronts through Cloudflare!), TLS
handshake + request + full response DELIVERED (pid 40 rdchk rv=540 verify=OK
then clean EAGAIN), Mojo channel traffic flows for ~300ms more, then the
RENDERER (pid 49) sends one final 248-byte channel message at ~22.8s and NOTHING
ever answers -- every thread parks permanently (epoll_waits 25-68s+), zero
frames painted. 8 consecutive network-page stalls (watch x4, embed x2, example
x1 +1 prior) vs local-file pages painting 2/2. The ~17:00 onset correlates with
an EXTERNAL serving change (example.com moved behind Cloudflare = bigger cert
chains/metadata), i.e. bigger responses now walk into a tobyOS bug that small
early-day responses missed. Fresh chrome profile changed nothing (and /data is
RAM-backED each boot -- nothing guest-side persists; profile/disk theories dead).

### Audits CLOSED this slice
- **Process-exit teardown: SAFE.** The heir mechanism (proc.c ~1256) already
  guarantees vmm_destroy_user_pml4 runs only when the LAST member is reaped,
  and any other CPU's stale user-TLB entries die at its next CR3 write
  (context switch). No shootdown needed there.
- **Runtime MAP_FIXED: LOW-RISK.** All 56 fl=0x32 mmaps in a full run are
  ld.so load-phase .bss maps (single-threaded in that process, old frame
  leaked not freed). No multi-thread runtime MAP_FIXED observed.
- **Mojo channel "shear": COSMETIC.** A 9936-byte SEQPACKET message read as
  4096 then 5840 (= exact remainder) -- the transport PRESERVES the tail like
  a stream, chrome reassembles; the [chan] instrument merely mis-parses the
  second read's bytes as a header. NOT data loss. (unix_enqueue caps at 65535,
  ring-full = clean EAGAIN backpressure.)

### Where the next session starts: the response-BODY data pipe
The control channel works; the BODY travels a Mojo DATA PIPE (shared-memory
ring + signaling). The renderer's final unanswered 248-byte message is almost
certainly its body-read request. Prime suspects, in order: (1) the data-pipe
shm buffer (memfd) producer->consumer visibility or its signal (an eventfd/
channel message) being dropped/never sent for LARGER payloads; (2) a size-edge
in the shm cache/memfd path (bodies over some threshold); (3) chrome-side wait
on a certificate-verification or cache write that tobyOS leaves hanging. The
[efd]/[shm]/[chan]/[wait] instruments plus one new probe -- log memfd
write/read offsets per data-pipe -- should corner it in one run. Also NOTE the
4th crash signature this arc: EXCEPTION 12 #SS, rbp=0x10060c4108070c57 (byte
soup = data over a saved stack frame), pid 8 browser worker, 1x, parked --
random-victim corruption may still be alive at low rate.

## SLICES 52-53 (2026-07-27) — ***CORRECTION: the "network page stall" was a DISPLAY-PATH gap, not a kernel bug.*** Web pages render again; the YouTube wall is now pinned precisely at MSE

### SLICE 51 WAS WRONG — retract it
Slice 51 concluded the stall was "the Mojo response-BODY data pipe". It was not.
Slice 50's "YouTube throttling" was also wrong. Both were inferences from
ABSENCE (no frames) without asking the one entity that knew: chrome itself.

### What actually happened (measured, one probe, one run)
Added a CDP page-state probe (Runtime.evaluate every 10s) to chromewin. On the
"stalled" example.com run it reported, while frames=0:
```
tobyprobe rs=complete url=https://example.com/ title=Example Domain
          blen=207 bh=118 vis=visible
```
The page was FULLY loaded, parsed, laid out and VISIBLE the entire time.
Network, TLS, Mojo, the renderer and layout were all working perfectly.
**`Page.startScreencast` only pushes a frame when the page DAMAGES something.**
A static page finishes painting before the screencast is even started, so
nothing ever streams -- indistinguishable from a hang from the outside. This
also explains the "8/8 network-page stalls vs local files 2/2": the local .webm
is a VIDEO (continuous damage), so it always streamed.

### Fix (programs/chromewin/main.c, slice 52) -- VERIFIED
Hybrid display path: screencast still carries dynamic/video content at full
rate; when no pushed frame has arrived for 2s, chromewin PULLS one with
`Page.captureScreenshot` (one outstanding at a time; decode/install factored
into `install_b64_frame()` shared by both paths).
- **example.com now renders its real page** (headline, body text, "Learn more"
  link); screendumps 36KB -> 77KB.
- **The YouTube WATCH page renders its real player** (controls, progress bar,
  pause glyph, metadata skeleton), no crash, over a full 360s run.

### Kernel EXONERATED for this symptom (A/B)
Checked out the pre-slice-50 kernel (86bb0d8 src/+include/) and rebuilt:
example.com fails to paint IDENTICALLY. So slices 50/50b did not cause it.

### SLICE 53: the video wall is now pinned at MSE
Extended the probe to the <video> element. Repeatable on the watch page:
```
tobyprobe rs=loading title=Big Buck Bunny 60fps blen=108307
          vid=r0 n2 t0.0 d0 b- e- p0 src=blob:
```
Real page + player (108KB DOM, correct title), MediaSource ATTACHED (blob:),
networkState=2 (LOADING), NO media error, but readyState=HAVE_NOTHING and
**ZERO buffered ranges** -- MSE segments never deliver. `document.readyState`
also stays `loading`, so the main HTML is still streaming late in the run.
Combined with slice 49 (VP9 decode+paint PROVEN with a local .webm), the wall
is now isolated to: **the MSE segment fetch/append path never delivers bytes.**

### Also measured, unresolved (next leads, in order)
1. **Userspace spin convoy.** Late in a watch run the syscall mix is dominated
   by `sched_yield` (12218 vs 275 futex) from ~4 chrome threads = a
   base::SpinLock/PartitionAlloc spin. Network arrives in BURSTS with 20-80s
   gaps and the main document trickles (~4.6 KB/s), consistent with CPU
   starvation. NOTE a tempting-but-WRONG fix was tried and REVERTED: penalising
   yield-spinners in `sched_yield`'s interactivity credit did nothing, because
   spinners take the FAST PATH (`state==RUNNING && ready_head==0 -> return`)
   which never reaches the boost logic. Any real fix must act on the fast path
   (or on enqueue targeting -- everything piles on the BSP via enq_target_for).
2. **Per-connection throughput is FINE** (tcp[8]: 41KB in 0.2s), so this is not
   a slow TCP stack -- it is scheduling gaps between bursts.
3. Whether the segment requests are even ISSUED is still unknown: instrument
   which URLs/hosts the network service opens after the player attaches MSE
   (googlevideo.com connections did appear: 173.194.x, 74.125.x).

## SLICE 54 (2026-07-28) — ***MSE VIDEO PLAYBACK PROVEN ON tobyOS.*** MediaSource -> appendBuffer -> VP9 decode -> visible moving frames. The YouTube wall is therefore SEGMENT DELIVERY, not media

Slice 53 pinned YouTube at MSE (player renders, MediaSource attached, ZERO bytes
buffered) but could not say whether MSE itself works. Settled it with a
self-contained test.

### The test (permanent, `logs/gen_mse_test.py`)
Generates a ONE-LINE JS file (single quotes only, no backslashes -> embeds
straight into CDP JSON) containing a tiny VP9 clip as base64. chromewin
navigates to `about:blank` and injects it via a big `Runtime.evaluate`
(`MSE_TEST_JS` in programs/chromewin/main.c, g_bigcmd path). The script does the
whole MSE dance with NO NETWORK AT ALL: atob -> Uint8Array -> `new MediaSource`
-> `addSourceBuffer('video/webm;codecs=vp9')` -> `appendBuffer` -> `play()`,
publishing each step to `window.__mse`, which the slice-53 probe reports.

### RESULT: MSE WORKS, VIDEO PLAYS
```
tobyprobe rs=complete vid=r4 n2 t1.3 d2 b2.0 e- p0 src=blob: mse=updateend buf=2.00
```
`readyState=4` (HAVE_ENOUGH_DATA), **2.0s buffered**, no error, not paused, and
`currentTime` ADVANCING across probes (1.3 -> 1.2 ...). **`logs/vid_c.png` shows
the decoded VP9 frame painted at 640x480 with its burned-in timecode
(00:00:01.200).** So on tobyOS: MediaSource + SourceBuffer + appendBuffer + VP9
decode + composite + paint + playback clock ALL work end-to-end.
Verified across clip shapes: 32x32/2f (1227B), 32x32/10f (1803B),
96x64/2f (3788B), 96x64/10f (8333B) -- all buffer and play.

### A FALSE LEAD I CAUGHT (record it so nobody re-chases it)
A first run of the 96x64/10f test (12273-byte CDP message) left chrome ALIVE and
still READING our commands but never writing again -- which looked exactly like
"large IPC messages break". Two experiments killed that theory:
1. **IPC size ladder** (`IPC_SIZE_LADDER` in chromewin): Runtime.evaluate padded
   to 1K/2K/4K/6K/8K/10K/12K/16K/24K/32K/48K -- **every rung replied `ok`**,
   including 12445 bytes, the same size as the "killer".
2. **Control injection**: the identical 8333-byte payload in an identical-size
   message, doing atob+checksum but NOT touching MediaSource, ran fine
   (`ctrl-done len=8333 sum=65122`, 242 chrome replies after).
3. Re-running the ORIGINAL failing case then **worked** (buf=2.00, playing).
So it was a ONE-OFF FLAKE of the documented WHPX non-determinism, not a size
threshold. LESSON (third time this arc): never conclude from a single run here.

### Where YouTube actually stands now
Everything media-side is exonerated: decode (slice 49, progressive), display
(slice 52), MSE (this slice). The watch page renders its real player, and the
remaining failure is that its media SEGMENTS never arrive: networkState=LOADING,
no error, zero buffered, `document.readyState` stuck at `loading`, total RX only
~309KB for the whole run. The live lead is delivery/scheduling, not media:
network arrives in BURSTS with 20-80s gaps while ~4 chrome threads dominate the
CPU in a `sched_yield` spin (12218 vs 275 futex), even though per-connection
throughput is fine (41KB in 0.2s). Next: instrument which URLs the network
service actually opens after the player attaches MSE (googlevideo connections
DO appear), and attack the starvation at `sched_yield`'s FAST PATH or
`enq_target_for`'s BSP piling (NOT the io_boost credit -- that fix was tried and
reverted in slice 53; spinners never reach it).

## SLICE 55 (2026-07-28) — e1000 RX ring 32 -> 256 (measured packet DROPS eliminated); the YouTube stall is re-localised to a BIDIRECTIONAL chrome-side silence, not the network stack

Chased "why do YouTube's media segments never arrive" with chrome's own Network
domain plus new kernel RX instrumentation. Several theories died; one real bug
was found and fixed.

### chrome DOES request the segment -- everything is just ~50x too slow
`Network.enable` in chromewin now logs requestWillBeSent / responseReceived /
loadingFailed (media URLs individually, the rest counted). On a watch page:
```
[net] MEDIA REQ #2: https://rr5---sn-vgqsknde.googlevideo.com/videoplayback?expire=...
probe #10 at 100s: net{req=21 media=1 resp=1 fin=15 fail=0}
```
So the videoplayback request IS issued, a response IS received, and there are
**ZERO failures**. The problem is purely pace: ~21 requests in 120s, and the
media request is not even issued until ~100s. Nothing is broken; everything
crawls.

### REAL BUG FOUND + FIXED: the RX ring was overrunning (packets dropped)
New `[rxdbg]` instrumentation in src/e1000.c counts drains, packets, and the
biggest batch found in one drain (a big batch == we were LATE). Measured with
the old 32-descriptor ring: **302 "LATE drain" events with repeated batch=32**
-- i.e. the ring was found COMPLETELY FULL, which means the NIC had already
been dropping frames. RX is drained from poll/idle paths with the NIC's IRQs
masked (header note in e1000.c), so any burst arriving while nothing polls has
only the ring to land in.
FIX: `RX_DESC_COUNT` 32 -> 256. 256 x 16B = 4096 = still exactly ONE page for
the ring (no allocator change, RDLEN stays 128B-aligned as the chip requires);
cost is 256 RX buffers instead of 32. VERIFIED: max batch is now 51 (was pinned
at 32 = full ring), so the ring no longer overruns and those drops are gone.
defboot clean. This is a general fix -- any burst-heavy workload was losing
packets.

### But that was NOT the stall: the silence is BIDIRECTIONAL and chrome-side
With the bigger ring the stall persists, and the instrumentation now says
exactly what it is NOT:
- `[rxdbg]` shows `pkts` FROZEN at 151 for **53 seconds** while `drains` climbs
  5,801 -> 655,474 = **~14,000 drains/second**. We are polling the NIC
  furiously and NOTHING arrives. So RX servicing is not the problem.
- **We also transmit NOTHING in that window** (`[tls] TX` count = 0 over
  20-75s). The silence is BIDIRECTIONAL: chrome simply isn't writing.
- During it: 47 threads BLOCKED (mostly futex), exactly ONE RUNNING (pid 39, a
  network-service thread making only 119 syscalls in 60s), **nothing READY**,
  and only 1-4 ring-3 profiler samples per interval -- the box is IDLE.
So this is chrome waiting internally, not a tobyOS network fault.

### Theories KILLED this slice (do not re-chase)
- **Zero-window / RX-buffer-full stall**: the `[tcp] RX buffer FULL` diagnostic
  did NOT fire during the stall (it does appear LATER in a run on tcp[8] with
  `free=0`, which is a separate, real flow-control issue worth its own look).
- **Retransmit storm / RTO backoff**: zero retransmits, zero out-of-order, zero
  dup-acks during the stall; `[tcp] WIRE ... retx=0`.
- **Userspace spin convoy**: profiler shows the CPUs are NOT in ring 3 during
  the gap (1-4 samples/interval); the box is idle, not spinning.
- **"~50 ready threads serialized on the BSP"**: FALSIFIED by the heartbeat --
  the ready queues are EMPTY and only one thread is runnable. A fix built on
  that model (a rate-limited steal probe in sched_yield's fast path, since
  `enq_target_for()` does return 0 unconditionally) changed nothing measurable
  and was REVERTED. The fast-path/enqueue-targeting observation is still true
  and may matter under a different load -- but it is not this bug.

### Next
The question is now narrow: what is chrome WAITING for during a ~55s window in
which it neither sends nor receives, with every thread blocked and the machine
idle? Prime candidates: a timer/timeout whose deadline we compute wrong (the
timed-futex/clock path), or a wakeup that is delivered late (poll_tick is
1ms-rate-limited and pid 0 is its only driver when all threads are blocked -- if
pid 0's lane is delayed, every parked poller waits). Instrument the deadline and
wake path for the specific threads blocked at that moment: dump, for the longest
futex/epoll waiter, its requested timeout vs actual elapsed time.

## Slice 56 (2026-07-28): the ~50s silences ROOT-CAUSED AND FIXED -- a blocking
## recvmsg freezing the whole wake machinery; then two more serial walls peeled

### Method: the handoff's missing instrument, built first
Requested-vs-actual wait time for every blocked waiter, plus liveness counters
for the wake machinery itself:
- `[fxpark]/[fxlate]/[fxwake]` (thread.c): futex park/wake with requested vs
  actual ms and a `rdy=` split (time from wake-DELIVERY to running -- separates
  "wake came late" from "woken but not scheduled").
- `[wait] ... to=` (syscall.c): every blocked syscall now shows its REQUESTED
  timeout. `[cur]`: every non-blocked Linux thread's current syscall + time in
  it. `[tick]`: futex-sweep/poll_tick counters + `nextdue` (soonest parked
  timed-futex deadline; negative = overdue = the sweep is starving).
- `[eplate]/[polate]`: finite-timeout epoll/poll overshoot. `[rxdbg] tx=`:
  wire-level TX (the old "bidirectional silence" was inferred from [tls] TX
  only). `[fxrt]`: counts FUTEX_CLOCK_REALTIME waits. RX-FULL log made
  once-per-episode (the old 8-per-conn cap made absence unfalsifiable).

### RUN 1 (instruments only): the stall named itself in one run
During the silence window (129.5s -> 175s):
- `[tick]` fxsweep/polltick counters FROZEN for 45+s; `nextdue` sank to -26s
  (a timed futex 26 SECONDS overdue with the sweep simply not running).
- `[eplate] to=4549ms act=49799ms`, `[fxlate] req=1518ms act=47290ms rdy=0ms`:
  wake DELIVERY late by ~46s, scheduling instant once delivered.
- `[cur] pid=39 RUNNING in recvmsg for 47s` with `[hb-x] cpu0 cur=39`.

Mechanism (all links verified): `lx_recvmsg`'s UDP arm ignored O_NONBLOCK and
MSG_DONTWAIT, so chrome's speculative QUIC/DNS reads (chrome reads UDP with
recvmsg until EAGAIN) BLOCKED in sock_recvfrom_to's infinite drain+hlt loop --
on the BSP. pid 0 is is_idle (work-stealing skips it; the tick never preempts
kernel mode), so pid 0 sat READY behind the blocked thread for the whole wait:
no poll_tick (pid 0's loop is its all-blocked driver) and no futex sweep (BSP
yield slow path was its ONLY driver) => every timer and poller in the box
frozen until a stray datagram arrived ~50s later. This also resolves the
handoff's contradiction B: "RUNNING but no ring-3 samples" = inside ONE
syscall's in-kernel hlt loop (the ~620/s drains during silences were that loop
polling the NIC at IRQ rate; 119 syscalls/60s because each recvmsg lasted tens
of seconds).

### Fixes (defboot clean after each build)
1. `lx_recvmsg` UDP arm honours nonblock/MSG_DONTWAIT (root cause).
2. `sock_recvfrom_to` + `tcp_poll_until`: yield-if-ready instead of hlt when
   THIS CPU has READY work (never park pid 0 behind a blocking wait again).
3. futex sweep + poll_tick now run from ANY CPU's yield slow path (was
   BSP-only = single point of failure).
4. FUTEX_CLOCK_REALTIME absolute deadlines rebased realtime->monotonic
   (glibc default-clock condvars/sem_timedwait; they were compared against
   perf_now_ns and could never expire; chrome passes them 600+/run).
5. tcp_recv proactive window updates: `adv_free_last` stamped on every sent
   segment; ACK when reality exceeds the peer's known window by >=2 MSS.
   ([tcp] WIN-UPDATE fires with peer-knew=0 cases -- a peer that ran our
   window down was otherwise never told it reopened; pure ACKs are not
   retransmitted, and slirp probes weakly.)
6. `sock_close` -> `tcp_close_nowait`: POSIX close(2) on a socket must not
   block; the old path did a synchronous 5s FIN-wait + TIME_WAIT linger
   INSIDE close(2).

### RUN 2 (fixes 1-5): kernel wake machinery healthy; next wall exposed
- `[tick]` alive all run, `nextdue` ~-200ms, every parked wait within its
  requested `to=`. 33 requests in the first 20s (was 17 in 120s); the real
  `videoplayback` MEDIA REQ at ~26s (was ~100s). Zero faults.
- NEW WALL: the watch-page renderer called exit(0) at 43.0s, ~1.1s after
  AudioService spawn + `PcmOpen: default, No such file or directory` (tobyOS
  has no ALSA device) + SyncReader timeouts. No crash, no respawn; the page,
  its MSE fetches and all CDP evaluate replies died with it.

### RUN 3 (--mute-audio + --disable-audio-output, probe cap 12->36)
- Renderer SURVIVES the full 360s. The REAL watch page paints in the TobyTK
  window: player frame, controls, progress bar, buffering spinner.
- Media still absent. `[cur] pid=40 RUNNING in close for 4-7s` REPEATEDLY =
  chrome's network IO thread serially closing ~8 page-load sockets, each a
  synchronous 5s FIN-wait inside close(2) => event loop wedged 50-70s => no
  h2 WINDOW_UPDATEs => media conns frozen at ~50KB with exactly 129 bytes
  unread. Profiler: 1-3 ring-3 samples/interval -- the "renderer spinning at
  100% CPU" reading was kernel wait-loops, NOT user code. Wire totals: 3.4MB
  rx==read on the main youtube.com conn, retx=0 ooo=0 -- transport healthy.

### RUN 4 (fix 6): close wedge GONE; current wall = Mojo delivery stops ~35s
- Zero "RUNNING in close". All conns read==rx. Renderer survives. First frame
  at ~31s. But: CDP evaluate replies stop entirely ~35s in, frames stop at 3,
  net{} counters freeze, media still ~50KB.
- The browser RECEIVES every probe ([devpipe] pid-1 reads 626B every 10s all
  run) and browser MAIN (pid 3) pumps healthily (200-600ms futex/poll cycles,
  USER time) -- but nothing downstream runs: renderer main sits in ONE
  epoll_wait for 59+s (rescanned every ~1ms by poll_tick, never finds
  anything ready). So messages stop flowing browser->renderer (Mojo channel
  delivery), starving the renderer main AND the media data pipe.
- `[epset]` instrument added for the next run: for any epoll_wait blocked
  >10s, dump every watched fd's kind + file_poll_ready NOW. Splits "sender
  never sent" from "data pending but waiter never woken".

### Ruled OUT this slice (do not re-chase)
- TSC desync / clock jumps: clkchk clean in every run (no negative deltas).
- "Zero-window exonerated because RX-FULL didn't log": the cap was 8/conn AND
  an honest peer exhausts the advertised window WITHOUT triggering refusals;
  absence proved nothing. (The stale-window bug was real -- fix 5 -- though
  not the primary stall.)
- Renderer busy-spin: profiler shows ring 3 nearly idle during every stall.
- MSE/decode/display: untouched, still proven (slice 54).

### State at slice end
YouTube watch page renders its real player in the native window with the
kernel wake machinery verified healthy end-to-end. Remaining wall, precisely
bounded: browser->renderer Mojo message delivery stops ~35s in (evaluates
unanswered, media data pipe undrained, ~50KB of media fetched then nothing).
Next measurement is already in place: `[epset]`.

### RUN 5 (same build as run 4 + [epset]): RETRACTION + sharpened wall
- **RETRACT the "audio killed the renderer" conclusion.** With audio fully
  disabled, the watch-page renderer (pid 47, renderer-client-id=5 -- the same
  role as run 2's pid 52) again called exit(0), this time at 22.3s, 6s after
  execve. No crash, no respawn, page dead, frames=0 all run, fail=1. The run
  2 -> 3 A/B was one run per arm -- exactly the trap prime directive 2 warns
  about. Correct statement: the second (watch-page) renderer's clean
  self-exit shortly after taking over the page is FLAKY -- observed with
  audio (run 2, 43s) and without (run 5, 22.3s); absent in runs 3-4. The
  audio-off flags stay (tobyOS has no ALSA device; the ALSA failure spam and
  SyncReader churn are real), but they are NOT a renderer-death fix.
- `[epset]` (its purpose this run) answered its question for the surviving
  waiters: every >10s epoll waiter's watched fds show r0/r4-writable-only for
  hundreds of seconds -- NOTHING pending that a wake was missed for. "Sender
  never sent", not a kernel readiness/wake bug.
- Remaining walls, now two and precisely bounded:
  **R1 (flaky, fatal):** the watch-page renderer sometimes exits cleanly
  seconds after page handoff (runs 2, 5). A clean exit(0) means its main loop
  ended by request/channel-EOF -- suspect the browser->renderer channel
  dropping (AF_UNIX/SCM_RIGHTS churn) or a browser-side shutdown decision.
  Next instrument: log AF_UNIX peer-close/channel-EOF events with pid+fd, and
  dump [lx-recent] for a RENDERER exit_group (today only faults dump it).
  **R2 (consistent when R1 doesn't fire):** browser->renderer CDP/Mojo
  message flow stops ~35s in while renderer, browser main, and pipe reader
  all stay healthy (runs 3-4); media stops at ~50KB (h2 window starves when
  the data pipe is not drained). [chan] is capped at 500 by ~17s -- raise or
  time-window it to see the late channel traffic.

### RUNS 6-7 (R1 instruments: [rexit]+[lx-recent] on renderer exit_group,
### [uxclose] peer-EOF trace, [chan] raised/time-windowed): R1 is now DOMINANT
- R1 fired in runs 5, 6 AND 7 (three consecutive): the watch-page renderer
  (the ~27-29k-syscall one) calls exit_group(0) at 22-28s, shortly after the
  normal renderer-#1 SIGTERM swap. No successor renderer is ever execve'd; the
  page is dead for the rest of the run (probes unanswered, frames 0, media
  frozen, fail=1). Runs 3-4 (where it survived) now look like the lucky runs.
- What the new instruments established:
  - No kernel fault, no failed spawn, no proc-table exhaustion, browser
    healthy throughout. [uxclose] shows only the exiting process's OWN
    teardown closes -- no premature channel EOF against the renderer from
    outside (within the 120-line cap).
  - Renderer #1's last recorded channel traffic (run 6) is normal ipcz
    control chatter (64B messages, seq increasing) right up to its clean
    browser-instructed exit -- the SHAPE of an instructed shutdown, and
    renderer #2's exit looks the same from the outside.
  - In run 6 the browser wrote ~5.6KB+4KB to the DevTools pipe (frame-sized)
    BETWEEN the two exit_group calls -- the browser was mid-delivery when
    the renderer died; correlation with chromewin's kick_play cadence is
    suggestive in runs 6/7 but does NOT hold in run 2. Unproven.
- Instrument shortfalls that blocked the verdict (fix next):
  - [lx-recent] (global, 384 deep) was FLOODED by tid 20's sched_yield spam
    (a busy-yield thread, likely the in-process-GPU compositor wait) -- the
    renderer's actual prelude was pushed out. Make the exit dump per-process
    or filter sched_yield from the ring.
  - [chan] at ~200 msgs/s exhausts ANY fixed budget before the ~25s window
    (500 by 17s, 1500 by 21.5s, 18s-gated 1200 by 24.0s). The right design
    is a RING of the last ~200 channel ops per process, dumped at [rexit] --
    then the browser's final message TO the dying renderer (the suspected
    shutdown instruction, or the absence of one) is always on record.
- NEXT for R1: the channel-op ring + per-process exit dump; plus log
  RenderProcessHost-side intent from the browser if possible (chrome
  --vmodule=render_process_host_impl=1 may name the shutdown reason on
  stderr, cheaper than kernel work: try
  --enable-logging=stderr --v=0 --vmodule=render_process_host*=2).
- Separate note: tid 20 busy-yields (the lx-recent flood) -- the old yield
  convoy still has one resident; worth a look once R1/R2 are closed.

## Slice 56d (2026-07-28): R1 ROOT-CAUSED -- a ONE-BYTE SHEAR in AF_UNIX for
## >=64KB Mojo messages; renderer deaths explained mechanically

### The chring instrument delivered in one run
Run 9's [rexit] channel-ring dump for the dying watch-page renderer (tgid 50)
ends with EXACTLY: recvmsg(fd=5) -> 4096, -> 61439, -> 1608, -> 192, then
exit_group(0). 4096 + 61439 = 65535 = 0xFFFF -- a magic number, and the
slice-53 "large IPC" size ladder only tested up to 48KB, BELOW this cliff.

### The mechanism, found in code and matching the measurement bit-for-bit
1. lx_read_msghdr caps a sendmsg total at SYS_MAX_RW = 65536 and returns the
   short count -- HONEST (Mojo handles partial channel writes by queueing the
   remainder).
2. unix_enqueue_fds then did `if (len > 65535) len = 65535;` -- SILENT -- and
   sock_unix_send_fds returned the full n. Net effect: for any >=64KB channel
   message the receiver's byte stream is missing EXACTLY ONE BYTE (byte
   65535), while the sender continues from offset 65536.
3. One missing byte shears the Mojo/ipcz framing for every subsequent message
   on that channel; validation fails; chrome's channel-error path shuts the
   endpoint down. In the renderer that is a CLEAN exit_group(0) -- wall R1,
   killing the page. On a non-fatal endpoint the channel just goes silent --
   wall R2's exact shape (message flow stops while everyone looks healthy).
4. Flakiness explained: it fires only when some browser<->renderer message
   crosses 64KB, which happens (or not) around player start per run.

### Fix (slice 56d)
sock_dgram.len u16 -> u32, sock.tail_off u16 -> u32, enqueue the WHOLE
message (no truncation), loud 8MiB sanity refusal instead of any silent cap.
lx_read_msghdr's 64KB-per-call short read/write stays (honest, POSIX).
Verification: run 10 pending at time of writing -- criteria: no [rexit] of
the watch-page renderer, probes keep answering past 40s, media b>0.

### RUN 10 (shear fix in): R1's clean-exit mode is GONE; media pipeline
### UNLOCKED; next wall = the old wild-jump corruption class, further along
- The watch-page renderer NO LONGER cleanly exits. It ran 62,935 syscalls
  (2.3x the previous best), issued TEN media requests with TEN responses
  (was 2/2) across multiple videoplayback range fetches, painted frames --
  then died at 31.5s to a USER-MODE WILD JUMP (rip=0x258821d, unmapped low
  address, sane-looking stack, fault_count=58754 resolved demand-faults
  before it). A second chrome process (pid 44) crashed the same way at
  30.2s. exit code -1, not exit(0): a DIFFERENT wall than R1.
- Transport self-check: zero [sockchk] CORRUPT (the FNV dequeue check) --
  the AF_UNIX layer is byte-perfect post-fix; zero oversized refusals.
- Assessment: this is the 0x208c13a wild-jump class (slices 50/50b fixed two
  structural causes: free-before-shootdown and the PTE install-if-absent
  TOCTOU). The heavier faster IPC flow after slice 56's fixes now reaches a
  third instance ~30s in. NEXT ARC: the slice-50 toolset ([pgj] page
  journal, [pfres]/[pfrej] rings, g_tlbq quarantine) on rip 0x258821d /
  0x228cc12; check whether the faulting VA was recently munmap'd/remapped
  and whether an un-acked shootdown frame got reused.
- NOTE for the next agent: probes/net counters freeze after the crash
  because the page is gone -- that is R1-collateral shape, not R2. R2 as an
  independent wall is now DOUBTFUL: the one-byte shear on a non-fatal
  endpoint explains the runs-3/4 message-flow stops too. Re-test R2 only
  AFTER the corruption wall falls.

## Slice 57 (2026-07-28): the wild-jump corruption class = TWO stacked bugs --
## un-acked TLB shootdowns (proven by A/B) and VMA-table exhaustion

### Autopsy instruments (run 11)
The terse "rsp,rip both CPL=3" isr branch now dumps pgj/mprotect/pfres for
the stack page + rax/rbx/rdi pointer-source pages. Run 11 escalated to FOUR
crashes (browser included) at 21-23s: user-mode #GP with V8-poison pointers
(r15=0xf0f0f0f0f0f0f0f1, non-canonical r14), every crash's last_fault_rip in
one tight code range, an mprotect storm immediately prior, and a fork at
15.6s followed by five seconds of CoW-fault storm ([pfres] how=1 err=0x7
continuous, plus bursts of how=2 err=0x6 = live heap pages coming back as
fresh zero pages).

### Bug A: tlb_shootdown_remote dropped all_acked=false
apic.c's bounded ack-wait times out ROUTINELY under WHPX (8-warn cap
saturated by 5.4s of run 11) and the void wrapper discarded the result. The
slice-50 quarantine covers only the FREE path; fork's write-protect sweep,
CoW copy-out and mprotect all proceeded with laggard CPUs still holding
stale WRITABLE translations -- parent threads kept writing into frames
shared with fork children. FIX + PROOF: retry-until-all-acked => run 12 had
ZERO crashes (4 -> 0) but broadcast retries were catastrophic under WHPX
(bootstrap 267s: every round waits out host-descheduled vCPUs of OTHER
processes). Refined to CR3-TARGETED shootdown (only CPUs currently in the
target address space can hold its stale non-global user translations; a CPU
that switched away flushed at CR3 load; both race directions err toward
sending) => run 13: bootstrap back to 18.2s AND crashes 4 -> 1.

### Bug B: VMA-table exhaustion (the residual crash + run-10's "wild jumps")
Run 13's remaining crash at 29s printed "[mmap] WARN: VMA table FULL (4096
entries)" at the same instant -- and in hindsight EVERY run-10 fatal fault
line said "(4096 total)". Chrome's PartitionAlloc decommits + V8 W^X churn
split VMAs indefinitely and nothing merged them: at the cap, mmap fails and
mprotect SPLITS fail silently, leaving sub-ranges with NO VMA -- the next
touch is a fatal "NOT covered by any mmap-VMA" fault at a perfectly valid
address (the "wild jump to unmapped rip" illusion). FIX: cap 4096 -> 8192
plus merge-on-pressure compaction (sort + merge adjacent same-prot/flags
ANON fragments at >75% occupancy, hooked ONLY at the tops of
mmap/munmap/mprotect under the BKL, before any entry pointers exist).

### RUN 14 (both fixes + compaction): *** YOUTUBE VIDEO PLAYED ON TOBYOS ***
- ZERO crashes in 360s (was 4/run at worst). [mmap] VMA compact pid=52
  6144 -> 1196 entries -- the exhaustion class is dead (80% of the table was
  mergeable same-prot anon fragments).
- The probe delivered the handoff's definition-of-done line on the REAL
  watch page (blen=839576): `vid=r4 n2 t0.8 d635 b20.0 e- p0 src=blob:`
  -- HAVE_ENOUGH_DATA, 20.0s BUFFERED, currentTime advancing, duration 635s
  (full Big Buck Bunny), no error, PLAYING. 142 media requests / 141
  responses; ~200 screencast frames painted during the playing window.
- Remaining rough edge (next slice, app-level): after ~10-20s of playback
  the player RESET to r0/b- and idled (media counters freeze at ~80s;
  fail=29 requests -- the googlevideo 403/URL-expiry class, likely YouTube's
  n-parameter/ABR retry logic rather than a kernel fault; zero kernel
  crashes, zero VMA/TLB incidents in the same window). Screendump timing
  missed the playing window; add a frame-diff screenshot burst on r>=3 for
  visual proof next run.

## Slice 58 (2026-07-28): the player reset is URL CORRUPTION, not YouTube policy

### Two findings, one fixed, one newly named
**FIXED -- TCP conn-pool exhaustion.** Run 14's reset window was full of
`net::ERR_INSUFFICIENT_RESOURCES`: a watch page opens ~13 TLS conns before
media, and slice-56's non-blocking close leaves closed conns running out
FIN/TIME_WAIT in the background, so the 16-slot pool ran dry ~44s in and
chrome's socket() failed. Fix: TCP_MAX_CONNS 16 -> 64 plus recycle-on-full
in conn_alloc (prefer the TIME_WAIT closest to its deadline, else a detached
closing conn). Run 15: zero ERR_INSUFFICIENT_RESOURCES, zero pool-full
recycles needed, zero crashes.

**NEW WALL, precisely measured -- media URLs are being CORRUPTED.** Run 15's
media requests carry TWO different `expire=` values for the SAME video:
`expire=1785298981` (5 requests -- sane, ~now) and `expire=8922279409`
(6 requests -- year 2252, IMPOSSIBLE). The corrupt-URL requests are exactly
the ones YouTube answers **403**, which is why the player resets to r0/b-:
the segment fetches are rejected. Run 13 showed a DIFFERENT bogus value
(`expire=7639704651`), so this is random corruption, not a constant or a
policy artifact -- and it is silent (no crash, no fault) now that slice
57 fixed the crash sites.
- Read: the slice-57 corruption class is NOT fully closed. It no longer
  crashes, but it still mutates data in flight -- here, a URL query string
  somewhere between the player's JS building it and the network service
  issuing it.
- NEXT MEASUREMENT (do this first, do not theorise): the query string is
  plaintext in the TLS-layer send path. Hash/log the outgoing GET line for
  googlevideo requests at the point tobyOS hands it to TLS, and compare with
  chrome's own CDP requestWillBeSent URL for the same requestId. That splits
  "chrome built a bad URL" (renderer/V8-side memory corruption) from "tobyOS
  mangled the bytes in transit" (socket/TLS path). Both are in-house bugs;
  the split says which half to instrument next.
- Note the shape matches the ONE-BYTE-SHEAR family (slice 56d): a digit
  substitution / shifted byte in a string is exactly what a boundary bug in
  a copy path produces. Check the >=64KB paths and any remaining u16
  length/offset in the send direction.

### Slice 58b (run 16, rid-paired probe): PARTIAL RETRACTION of the
### "URL corruption" claim -- and the real remaining problem is VARIANCE
Same kernel as run 15, only chromewin rebuilt. Result:
- **No corrupt URLs and NO 403s.** Every videoplayback request/response
  logged expire=1785300094 (sane) and status=200. The two BOGUS-EXPIRE hits
  were my own probe's false positive: URLs with NO expire= param at all
  (generate_204 etc.) parse as exp=0. Fix the detector before reusing it.
- So the run-15 `expire=8922279409` observation is NOT reproducible on
  demand. It was real in that log, but it is INTERMITTENT and its mechanism
  is unproven -- possibly genuine memory corruption, possibly a fragment
  parsed as a whole message by cdp_fill_nb's "one giant partial: drop it
  all" path (identified but not yet instrumented). DOWNGRADE it from "the
  cause of the reset" to "an unexplained intermittent observation".
  The slice-58 commit message overstates it; this entry is the correction.
- **The honest headline: playback is FLAKY run-to-run, not blocked.**
  run 14 = r4/b20.0/playing; run 15 = r1/b4.0 then reset; run 16 = never
  past r1 (and one probe caught the element torn down: r0 n3 d0). Same
  kernel across 15/16. So what remains is not one deterministic wall but
  RUN-TO-RUN VARIANCE in whether the player sustains its segment loop.
- NEXT (do these in order, they are cheap):
  1. Fix the expire detector (skip URLs with no expire=), then run the SAME
     build 3x and tabulate: max buffered, #403s, #corrupt URLs per run.
     Variance data first -- one run cannot characterise a flaky failure.
  2. Instrument cdp_fill_nb's drop path (count drops + log when a message
     is parsed whose head is not '{'): proves or kills the fragment theory
     for the run-15 log, cheaply and independently of chrome.
  3. Only then chase kernel-side corruption again.

## Slice 58c (2026-07-28): 3x SAME-BUILD TABULATION -- playback is RELIABLE,
## and the URL corruption is REAL (fragment theory killed by drops=0)

Method: build once, run three times, keep each log, tabulate (prime
directive 2 applied properly instead of reasoning from single runs).

| run | maxbuf | best readyState | 403s | bogus URLs | crashes | cdp drops |
|-----|--------|-----------------|------|------------|---------|-----------|
| 1   | b20.0  | r4              | 5    | 6          | 0       | n=0 mid=0 |
| 2   | b24.4  | r4              | 0    | 0          | 0       | n=0 mid=0 |
| 3   | b20.0  | r4              | 0    | 0          | 0       | n=0 mid=0 |

Conclusions, each following directly from the table:
- **YouTube playback is RELIABLE, not flaky: 3/3 runs reached
  HAVE_ENOUGH_DATA with 20-24s buffered and zero crashes.** Runs 15/16 were
  the outliers, not the norm. `logs/x3_run3_c.png` is the visual proof that
  was missing: a DECODED VIDEO FRAME (the Big Buck Bunny title card) painted
  in the TobyTK window inside the real watch page (title, channel, 1.24M
  subscribers, like/share/save, Sign in).
- **The fragment theory is DEAD.** cdp_fill_nb dropped NOTHING in any run
  (n=0, mid=0), so chromewin's reassembly cannot explain any logged URL.
- **The URL corruption is REAL and reproducible at ~1 run in 3.** Run 1
  produced 6 bogus-expire URLs with the FIXED detector (which now only flags
  URLs that actually carry expire=) and YouTube 403'd 5 of them -- with zero
  CDP drops and zero crashes. So slice 58b's downgrade was too pessimistic:
  chrome really does emit URLs carrying impossible expire values. This
  RE-ESTABLISHES the finding on much better evidence than run 15 had.
- Note run 1 still reached b20.0 DESPITE the 403s: the corruption degrades
  the segment loop, it does not stop playback outright.

### Next (for the corruption, now the last known defect)
It is intermittent, silent, in-chrome, and survives every fix so far. It is
the same family as slice 56d's one-byte shear -- a string mutating in a copy
path. Cheapest discriminator left: log the SAME requestId's URL twice from
different chrome-side events (requestWillBeSent vs responseReceived carry
the URL independently). Identical-but-bogus = the URL was already wrong when
chrome built it (renderer memory); differing = it mutated between the two,
which localises the copy path. Both are one chromewin edit and one run.

## Slice 58d: FULL RETRACTION -- there is NO URL corruption. It is YouTube's
## own second (signature-gated) URL set, and playback works anyway.

Evidence, all from run 1's existing log (no new run needed):
- The "corrupt" URLs are NOT random: the same bogus expire (8054257608)
  repeats across SIX requests while the sane one (1785301507) covers five.
  Random memory corruption differs every time.
- Their CDP messages are SMALL (mlen ~2200) and well-formed (head '{') --
  no size boundary, no fragment.
- They carry a DIFFERENT `ei=` session token (pbA-TxoC0nQnX vs ozVpaoX-Orj72).
- **They target a DIFFERENT CDN HOST**: rr4---sn-5hne6nzk (6) vs
  rr2---sn-vgqsrn6y (5).
No bit-flip or copy-path bug can fabricate a coherent alternate hostname +
session token + a repeated far-future expire. These are two genuine URL sets
that YouTube itself served. Far-future `expire` values are normal for
SIGNATURE-GATED urls (access controlled by the sig/n transform rather than
by time); requests that do not carry the correctly transformed parameters
get 403. That is the long-standing YouTube signature-cipher wall already on
record as a documented non-goal -- NOT a tobyOS defect.

RETRACTED: slice 58's "media URLs are being corrupted" and slice 58c's
"URL corruption RE-ESTABLISHED". Both were pattern-matching on an impossible
expire without checking the simplest alternative (different host => different
URL set). The lesson for the ledger: before calling something corruption,
check whether the "corrupt" bytes are internally COHERENT -- coherence means
a different source, not a mutation.

STATE: nothing left to fix here. tobyOS reaches HAVE_ENOUGH_DATA with 20-24s
buffered in 3/3 runs and paints decoded frames; some segment fetches 403
because YouTube signature-gates a fallback URL set, and the player simply
uses the set that works. Kernel-side, this arc is DONE.

## Slice 59 (2026-07-28): desktop-UA + page-richness probe + scrolling.
## HONEST STATUS: metadata works; 403s NOT reliably fixed; thumbnails/
## comments still absent -- and full YouTube parity is its own arc.

Changes: chrome now runs with an ordinary desktop Chrome user-agent (was
"HeadlessChrome/151", which YouTube treats as a bot), --accept-lang, a
page-RICHNESS probe (imgs=decoded/total, tiles=, cmt=, meta=viewcount) and
user-like scrolling (YouTube lazy-loads sidebar thumbnails and does not even
request comments until scrolled).

Measured:
| run | UA      | 403s | imgs      | tiles | cmt | meta      |
|-----|---------|------|-----------|-------|-----|-----------|
| 17  | desktop | 0    | 2/65      | 2     | 0   | 23M views |
| 18  | desktop | 6    | 2/65      | 2     | 0   | 23M views |

- **METADATA WORKS**: the real view count (23M views) populates from the
  live page. Title/channel/subscriber/like chrome already rendered
  (logs/x3_run3_c.png).
- **403s are NOT eliminated.** Run 17 had none, run 18 had six on the same
  build. So the UA changes the odds at best; do NOT claim it as the fix
  (that would be the same single-run error this arc keeps making). Next:
  run the UA build 3x via logs/run_x3.sh and tabulate 403s per run against
  the pre-UA baseline (~1 run in 3) before drawing any conclusion.
- **Thumbnails did NOT improve after scrolling** (still 2/65 decoded, 65
  <img> tags present). So the earlier "it's just lazy-loading" explanation
  is NOT sufficient. Two candidates, untested: the scroll never took effect
  (YouTube may scroll an inner container, or the injected scrollBy raced the
  app's own handlers), or thumbnail fetches/decodes genuinely fail. CHECK
  FIRST, cheaply: probe window.scrollY after the scroll (proves the scroll
  happened at all) and count i.ytimg.com requests in the Network events
  (proves whether the fetches are even issued).
- **Comments never rendered** (cmt=0), consistent with the scroll not taking
  effect -- comments are fetched on scroll-to.

SCOPE NOTE: "YouTube like a normal browser" (populated thumbnails, sidebar
tiles, comments, click-through navigation) is a multi-slice arc, not a
follow-on to the playback work. The playback arc is DONE and verified
(3/3 runs HAVE_ENOUGH_DATA, 20-24s buffered, decoded frames painted).

## Slice 59b (2026-07-28): scroll FIXED (real wheel input), and that DISPROVES
## the lazy-loading explanation: thumbnails/tiles/comments still do not render

Run 19 discriminators (the split that was missing):
- **sy=0 after 8 window.scrollBy calls** -- the injected JS scroll NEVER took
  effect (YouTube's app owns the scroll). So slice 59's "scrolling did not
  help" was measuring a scroll that never happened.
- **thumb=18** -- i.ytimg.com thumbnail fetches ARE issued.
- **api=3 ok=3 bad=0** -- /youtubei/v1/next (which supplies related-video
  tiles AND comment data) SUCCEEDS every time.
FIX: replaced injected JS with a real Input.dispatchMouseEvent mouseWheel
(the same Input domain the window host already uses for user input), and the
probe now reads max(window.scrollY, scrollingElement.scrollTop).

Run 20 result -- the scroll WORKS (sy reached 1200 then 2357; the trailing
sy=0 is by design, scrolling back up for screenshots). But at FULL scroll
depth the page is still:
    imgs=2/78 decoded   tiles=2   cmt=0   meta=23M views
**So lazy-loading is NOT the explanation.** With fetches issued, the API
answering 200, and the viewport actually moved, 76 of 78 images still never
decode and the related/comment sections never render.

Where that points (NOT yet measured -- do not guess further):
1. Do the ytimg fetches COMPLETE? We count requests, not responses. Add
   ytimg response-status + loadingFinished counting -- one edit. If they
   never finish, it is a transport/connection-reuse problem; if they finish
   200 and still do not decode, it is chrome's image decode/raster path
   (SwiftShader software raster, --num-raster-threads=2) starving.
2. tiles=2 with a 200 OK from /next is a RENDER gap, not a data gap: the
   renderer is not turning the API response into DOM. Same suspect as (1):
   renderer main-thread/raster starvation under this workload.
Both point at the same next question -- is the renderer keeping up? -- which
is measurable with the ring-3 profiler already in the tree.

STATUS: video playback = DONE. YouTube UI parity = genuinely unfinished, now
with the first two false explanations (bot-UA gating, lazy-loading) removed
and the search narrowed to renderer throughput.

## Slice 59c: desktop viewport TRIED and REVERTED -- it made the page WORSE,
## which identifies the real constraint: RENDERER RASTER THROUGHPUT

Hypothesis: at 800x600 YouTube uses its narrow responsive layout (secondary
column collapsed), so tiles=2 / imgs=2-of-78 might be layout, not failure.
Test: Emulation.setDeviceMetricsOverride 1280x900 (screencast still scales
into the 800x600 window).

Result -- the override APPLIED (no error) and every richness metric got
WORSE:
| metric | 800x600 | 1280x900 |
|--------|---------|----------|
| blen   | 1050742 | 587715   |
| imgs   | 2/78    | 1/7      |
| tiles  | 2       | 0        |
| meta   | 23M views | -      |

2.4x the pixels under SwiftShader SOFTWARE raster => the renderer completes
LESS of the page. **The narrow-layout theory is dead, and the constraint is
renderer/raster THROUGHPUT.** Reverted; do not re-add a larger viewport
before rasterization gets faster.

Process note: run 21 produced NO data because the override was written as
`if (!cdp_wait(id)) return -1;` and killed the bootstrap. Enhancements in the
bootstrap path must be non-fatal -- fixed.

### The remaining work, stated plainly
Everything upstream of rendering is healthy: thumbnail fetches issued,
/youtubei/next 200 OK, metadata populating, scroll working, video playing.
What is left is that a modern YouTube watch page is more raster/layout work
than software SwiftShader can finish on this box. That is a PERFORMANCE arc,
not a correctness bug, and the honest options are:
1. Measure first: [prof] ring-3 sampling of the renderer + gpu processes
   during a watch page, to confirm raster/paint dominates (and which stack).
2. Reduce work: fewer raster threads is not the issue -- try
   --disable-features=CanvasOopRasterization, --disable-smooth-scrolling,
   --disable-2d-canvas-image-chromium, and YouTube's lighter surfaces
   (embed player, m.youtube.com) to quantify the gap.
3. Increase capacity: real GPU raster (i915-lite path) is the structural fix
   and a large arc of its own.

## Slice 59d: CORRECTION -- the renderer is NOT raster-bound. It is IDLE.

The [prof] ring-3 sampler was already running in run 22 and I committed the
throughput conclusion (99b43bb) WITHOUT reading it. It says the opposite:
    73 intervals, 568 total ring-3 samples => 7.8 samples per ~3s interval.
With 4 CPUs at ~1 kHz that is roughly 0.06% of ticks in userspace. A
raster-bound renderer would show HUNDREDS of samples per interval. **chrome
is not computing; it is waiting.** So "a modern watch page is more raster
work than SwiftShader can finish" is WRONG and is hereby retracted.

Why the bigger viewport still made things worse is then a different
mechanism (more/larger frames + shared-memory IPC per update => more waiting
per unit of progress), not more pixels of CPU raster.

This is the FOURTH theory to die in this arc (bot-UA gating, lazy-loading,
narrow layout, raster throughput). The pattern in every case: a plausible
story adopted from one signal without checking a cheap instrument that was
already in the tree. The instruments keep being right; the narratives keep
being wrong.

### What the evidence actually supports now
- Userspace idle + page half-built + data plane 200 OK => chrome's renderer
  is blocked waiting for something the browser/GPU process owes it, in the
  same FAMILY as the slice-56 wake bugs but NOT cured by them.
- Concretely to measure NEXT, in this order:
  1. Which thread is the page blocked behind? The [wait]/[cur] dump is
     already per-thread; filter it to the RENDERER pid only, at a moment
     when blen has stopped growing, and name the syscall + fd + duration.
  2. Is the compositor/GPU side starving it? --in-process-gpu means raster
     and GL live in the browser process; check whether the GPU thread is
     itself blocked (same dump, gpu pid).
  3. Only if both look healthy, revisit IPC latency (the Mojo channel
     round-trip time under load) -- measurable by timestamping a CDP
     Runtime.evaluate round trip while the page is stalled vs idle.

## Slice 59e: FUTEX_CMP_REQUEUE implemented (real ABI gap) but it was NOT the
## bug -- and the reframe that follows: chrome thinks the page is DONE

Measured: the renderer MAIN thread parked on futex(0xc130ac0, to=-1) for
645 -> 8168 ms while userspace ran at ~0.06%. I read that as a lost
broadcast (glibc pthread_cond_broadcast uses FUTEX_CMP_REQUEUE, which we
returned -EINVAL for) and implemented REQUEUE/CMP_REQUEUE as wake-all.

Run 23 verdict: **[fxrq] fired ZERO times and [fxop] logged no unsupported
op** -- chrome never issues those ops, exactly as the bring-up note said.
FIFTH dead theory. The requeue implementation is kept: it is a genuine Linux
ABI gap that any other glibc program can hit, and it now cannot hide (an
unsupported op logs [fxop] instead of silently returning EINVAL).

**The reframe this forces:** an idle main thread on an infinite futex is a
message loop with NOTHING TO DO. Combined with rs=complete, blen ~1.0 MB,
meta=23M views and (this run) vid=r4 b20.0 playing, the honest reading is
that chrome believes the page is FINISHED. tiles=2 / imgs=2-of-67 / cmt=0 is
then not a renderer failure at all -- it is what this client was SERVED.

NEXT (measure what the DOM actually contains, do not theorise):
1. Dump the top-level ytd-* element census + whether a consent/sign-in gate
   or "before you continue" interstitial is present in the DOM.
2. Count ytd-watch-next-secondary-results-renderer children specifically:
   0 children = YouTube sent no sidebar payload; >0 children with tiles=2 =
   a rendering gap. That single number splits served-vs-rendered.
3. Check the comments section's own state: ytd-comments + its continuation
   -- comments load via a SEPARATE continuation request that a client
   without the right context never issues.

## Slice 59f: DOM census -- real numbers, and a probe FALSE POSITIVE to fix

Census (stable across runs 24 + 25):
    sec=4  cmt2=73(0 threads)  gate=1  ytd=3637-3704  tiles=2  imgs=2/68
Consent cookies (CONSENT=YES+cb, SOCS=CAI on .youtube.com, set BEFORE
navigation) changed NOTHING -- every field identical. Two readings, and the
second is more likely:
1. The cookies were rejected/ineffective, or
2. **gate=1 is a FALSE POSITIVE of my own selector.** It matches
   `tp-yt-paper-dialog`, which YouTube ships HIDDEN on every page. Presence
   != displayed. This is the same mistake as the BOGUS-EXPIRE detector
   (flagging URLs with no expire= at all). MUST re-measure with visibility:
   offsetParent !== null / getComputedStyle(el).display !== 'none'.
Keep the consent cookies regardless: they are what a real session carries and
they cost nothing.

### The honest, stable picture (what any next agent should start from)
- ytd=3700 elements: the YouTube app DOES build itself fully.
- sec=4: the sidebar container has children, but a populated desktop page has
  ~20 tiles -- so content is PARTIALLY served/built, not absent.
- cmt2=73 with ZERO comment threads: the comments component exists and is
  empty -- comments are fetched by a separate continuation that never fires.
- imgs=2/68 with thumbnail fetches ISSUED and /youtubei/next 200 OK.
- Userspace ~0.06% busy: nothing is compute-starved.

### Next steps, in order, all cheap
1. Fix the gate detector (visibility, not presence) and re-run -- if gate
   becomes 0, the consent theory dies (theory 6) and the field was noise.
2. Ask chrome what it thinks is missing rather than inferring: dump
   `document.querySelector('ytd-watch-next-secondary-results-renderer')
   .innerText.slice(0,200)` and the ytd-comments innerText -- placeholder
   text ("Comments are turned off", a spinner, an error) names the state
   directly.
3. Check whether the comment continuation request is ever ISSUED: count
   /youtubei/v1/next continuations AFTER scroll (we count /next calls, but
   not which are continuations). Zero after scrolling => the app decided not
   to ask, which points back at client context (consent/session), not at us.

## Slice 59g: consent theory DEAD (gate=0), and the regions report EMPTY
## innerText -- i.e. present in the DOM but NOT LAID OUT

- **gate=0** once the detector tests VISIBILITY (offsetParent + client rects)
  instead of presence. The earlier gate=1 was my own selector matching
  `tp-yt-paper-dialog`, which YouTube ships hidden on every page. SIXTH dead
  theory; the consent cookies are kept (a real session carries them) but they
  fixed nothing.
- **secTxt=<-> cmtTxt=<->**: both regions return EMPTY innerText while
  sec=4 children and cmt2=73 descendants exist. innerText is LAYOUT-aware --
  it returns '' for subtrees that are not rendered. So the sidebar and the
  comments component are in the DOM but NOT LAID OUT.

### The one measurement that splits this (do it first)
`textContent` vs `innerText` on the same two elements:
- textContent NON-EMPTY + innerText empty => the DATA IS THERE and only
  layout/visibility is missing => a rendering/CSS/visibility problem on our
  side (or YouTube deliberately hiding a collapsed region at this viewport).
- BOTH empty => the containers are empty shells => the payload was never
  filled => back to the data/continuation path (cont=1 was observed, so at
  most one continuation ever fires).
Add both to the probe; it is a two-line change and it decides the direction.

### Process note for whoever continues
Six theories have now died here (bot-UA, lazy-load, narrow layout, raster
throughput, futex requeue, consent gate) and TWO of them died because MY OWN
PROBE was wrong (BOGUS-EXPIRE flagged URLs with no expire=; gate flagged
hidden dialogs). Before trusting any new probe field, sanity-check it against
a case where you KNOW the answer.

## Slice 59h: cdp_send buffer bug fixed; the layout-vs-payload split is BUILT
## but still UNMEASURED (run 29's page did not finish building)

- **REAL BUG FIXED: cdp_send's `static char buf[2048]`.** The probe JS grew
  past it, so snprintf TRUNCATED the command mid-string and chrome answered
  `-32700 JSON: invalid token`. Run 28 produced ZERO probes for this reason
  alone -- a tooling failure that looked like a page failure. Raised to
  16384. NOTE this would silently corrupt ANY long CDP command (large
  Runtime.evaluate, Input sequences, setCookie with big values), so it is a
  fix on its own merits, not just for the probe.
- The split probe (secIt / secTc / secH / cmtTc / cmtH) is now IN and
  correct, but run 29's page only reached ytd=137 elements (a full build is
  ~3700), so every sample shows the pre-build state. **No verdict yet.**

### Exactly where to resume
Run the current build until a run reaches ytd>3000, then read ONE line:
    secIt=<..> secTc=<..> secH=N cmtTc=<..> cmtH=N
- secTc non-empty + secIt empty + secH=0 => data present, NOT laid out
  => rendering/layout side.
- secTc empty + secH=0 => empty containers => payload never arrived
  => data side (and note only cont=1 continuation was ever observed).
Because the page build itself is variable, use logs/run_x3.sh (build once,
run 3x, auto-tabulate) rather than judging from a single run -- that harness
exists precisely for this.

## Slice 60: THE CONTROL EXPERIMENT (should have been run first). tobyOS is
## already AT PARITY with real headless Chrome. The gap is HEADLESS itself.

Ran host Chrome (Windows, real GPU, real network) against the SAME watch page
and censused the same elements:

| element                    | host --headless=old | host --headless=new | tobyOS |
|----------------------------|---------------------|---------------------|--------|
| ytd-comment-thread-renderer | **0**              | **0**               | 0      |
| ytd-compact-video-renderer  | **0**              | **0**               | 2      |
| ytd-comments (component)    | present (31)        | 0                   | present (73) |
| <img> tags                  | 89                  | 7                   | 68     |

**Headless Chrome does not render YouTube's comments or sidebar tiles on a
normal machine either.** tobyOS matches -- and on the sidebar/comments
components it actually does BETTER than the host's --headless=new run.
There is NO tobyOS defect here to fix. Six theories (bot-UA, lazy-load,
narrow layout, raster throughput, futex requeue, consent gate) all chased a
gap that does not exist as a tobyOS bug.

Caveat, stated honestly: the host runs used --virtual-time-budget, which can
starve real network I/O, so the host numbers are a LOWER bound. That does not
weaken the conclusion -- the burden of proof has shifted, and nothing now
suggests tobyOS is deficient on this page.

### What full UI parity would actually require
YouTube defers comments and sidebar tiles behind IntersectionObserver +
compositing/visibility signals that headless chromium does not generate.
chromewin runs **chrome-headless-shell**, which is the stripped old-headless
binary and can ONLY do headless. Getting a "normal browser" page therefore
needs HEADED chrome: the full chrome binary plus a real display surface
(Ozone backend against TobyTK/our framebuffer) -- a large bring-up arc of its
own, NOT a continuation of this one.

### Method lesson (the expensive one)
The control experiment cost 60 seconds and was decisive. It came AFTER ~8
six-minute guest runs and six dead theories. RULE: before attributing a
behaviour to your own system, reproduce it on a KNOWN-GOOD system. If the
reference does the same thing, there is no bug -- only a characteristic.

## Slice 61: RETRACT the slice-60 ceiling. Headless CAN render comments +
## sidebar -- the slice-60 control was wrong in TWO measurable ways.

Re-ran the host control the way §4.1 of the handoff demanded: NO
--virtual-time-budget, ~60-90s real wall clock, real CDP mouseWheel input
(the same Input.dispatchMouseEvent chromewin sends), probes via
Runtime.evaluate instead of --dump-dom. Full matrix, same watch page
(aqz-KE-bpKQ), host Chrome 15x on Windows:

| mode                        | vis     | foc  | threads | lockup | imgs   | ytd  |
|-----------------------------|---------|------|---------|--------|--------|------|
| headless=old + wheel scroll | visible | 0->1 | **20**  | 20     | 37/92  | 8291 |
| headless=new + wheel scroll | visible | 0->1 | **20**  | 20     | 34/83  | 7353 |
| headless=old, JS poke only  | visible | 0    | 0       | 20     | 1/66   | 3773 |
| headless=old, tobylike tour | visible | 0->1 | **20**  | 20     | 39/88  | 7378 |
| headed, window HIDDEN       | hidden  | 1    | 0       | 3      | 1/30   | 2466 |

Where slice 60 went wrong (both were MY measurement bugs, prime directive 3):
1. **--virtual-time-budget starved the scroll-driven phase entirely** (and
   slice 60 never scrolled). Comments are only REQUESTED after real scroll
   input reaches the continuation region; virtual time expires the budget
   before any of that can happen.
2. **Counted OBSOLETE element names.** Modern YouTube renders sidebar tiles
   as `yt-lockup-view-model` (host shows 20 while `ytd-compact-video-
   renderer` shows 0 on a FULLY-WORKING page), and comment threads render
   OUTSIDE the measured `ytd-comments` box (cmtH=0 with 20 threads visible).
   The slice-59g "sidebar EMPTY innerText" reading suffers the same problem.

What the matrix PROVES:
- **Route B (headed chrome + Ozone X11) is NOT required.** headless=old --
  the exact engine flavor tobyOS runs -- reaches full population. A headed
  chrome whose window is hidden/occluded is THROTTLED BELOW headless (ytd
  2466, wheel dispatches never even ack) -- headed would be a REGRESSION
  without a real visible display.
- **The §3A JS observer poke is dead**: scrollIntoView + synthetic scroll +
  forced reflow, with ytd-comments AT viewport top (cmTop=0), renders zero
  threads even on the host. Only REAL wheel input through the Input domain
  triggers the continuation machinery (consistent with slice 59b).
- **Focus emulation matters**: foc flips 0->1 on the first real wheel and
  continuations fire only at foc=1 (poke mode stayed foc=0, got nothing).
- **The r0 player reset reproduces ON THE HOST** after the -4200 up-scroll
  (post-scroll probes read vid=r0 t0.0 p1 in old, new AND tobylike modes).
  The slice-57 "player resets to r0" remainder is at least partly an
  artifact of our own scroll choreography, not a tobyOS defect.

Why tobyOS still showed cmt=0 (the actual remaining gap): run-27 probes show
the SPA finishes building at ~40s guest time (ytd 137 at 30s -> ~3700 by
40s), but the slice-59 scroll tour ran on a FIXED schedule at 25-53s -- it
toured a half-built page, ended with the -4200 return-to-top, and never
scrolled again; at sy=0 nothing below the fold can lazy-load, so the page
froze at imgs=10/65 cmt=0 with only cont=1 (the initial /next) for 300s.
The host 'tobylike' run does NOT freeze only because the host app is fully
built BEFORE 25s -- the tour's at-depth window lands on live observers.

### Slice 61 implementation (chromewin)
- Probe grown with the CORRECT metrics: th (comment threads), lk (lockup
  tiles), cti (continuation items), sh (scrollHeight), vis/foc, cmTop; CDP
  print cap 300 -> 800 (the old cap was silently truncating the probe tail).
- Probe replies are now PARSED (g_p_sy/ytd/sh/th) and drive a scroll state
  machine: WAIT-BUILD (ytd>=1500) -> DOWN (600px/4s until sy plateaus) ->
  DWELL 30s at bottom -> CRUISE (nudge 600px/8s as the page grows; one
  late §3A poke at 180s as belt-and-braces) -> TOP at 280s for screenshots.
- Emulation.setFocusEmulationEnabled at bootstrap.
- run_watch.py: two extra screenshots (240s, 330s runner) to capture the
  comments region while dwelling and the player after return-to-top;
  run_x3.sh tabulates th/lk/cti/sy.

## Slice 61b: the heartbeat experiment. Lifecycle starvation CONFIRMED, then
## traced to a DETERMINISTIC freeze -- raf stops at exactly 705, both runs.

Instrumented the rendering lifecycle directly (probe fields raf/io/hb) and
injected a PRESENTATION HEARTBEAT: a 2x2 fixed div whose background toggles
every 100ms via setInterval. Each toggle dirties style -> forces a real
BeginMainFrame -> IntersectionObserver delivery, lazy image decode and
YouTube's continuation trigger all get their delivery cycle. (First build of
the probe had a missing close-paren -- the whole expression compiled to a
SyntaxError, killing probe + state machine + heartbeat at once, and the run
produced NOTHING. logs/check_probe.py now extracts the JS out of the C
literals and node --checks it; run it after EVERY probe edit.)

Result, identical across two runs:
    raf = 0 -> 154 -> 440 -> 686 -> 705 -> FROZEN forever
    io  = fired1 (observer delivery works while the lifecycle runs)
    screencast frames freeze at the SAME probe as raf (14 in both runs)
    network keeps flowing all along (req 305->310+ after the freeze)
The lifecycle ran fine at ~25 rAF/s under the heartbeat, then the ENTIRE
frame pipeline (main frames + compositor + captureScreenshot) stopped at
raf=705 exactly, twice. Deterministic => resource exhaustion, not a race.

## Slice 61c: ROOT CAUSE -- /data was a 4 MiB, 256-inode tobyfs. Chrome
## filled it in ~20s; every create after that failed; the EACCES was a LIE.

The chain, each link verified:
1. disk.img was 16 MiB, but host mkfs_tobyfs stamps a FIXED 4 MiB / 256-inode
   tobyfs (TFS_TOTAL_BLOCKS constants) -- /data was 4 MiB all along.
2. chrome writes profile + HTTP cache (caching the very video segments it
   plays) + POSIX shared-memory files (--disable-dev-shm-usage sends them to
   TMPDIR=/data) into that 4 MiB. First failure at guest t=33s.
3. tobyfs create fails with VFS_ERR_NOSPC -- but the lx open path mapped ANY
   vfs_create failure to EACCES (syscall.c). Chrome logged 64,858 x
   "Creating shared memory in /data/... failed: Permission denied (13)".
   The errno was false: it was ENOSPC, not EACCES. Cost: the investigation
   started toward permissions/sysprot/caps (all clean, all loud-logging).
4. Frame transport allocates shm per CompositorFrame; once the pool drained
   (705 main frames), production stopped -- the raf=705 freeze. Comments
   never rendered because the continuation machinery needs lifecycle cycles
   AND the shm-starved compositor could not produce them after ~55s.

Fixes landed:
- logs/mkdata_gpt.py: generates a GPT image with ONE BLANK tobyOS-data-typed
  partition (1 GiB). The kernel's M23A provisioning path auto-formats it
  DEVICE-SIZED on first boot -- verified: "[tobyfs] format: 261883 blocks
  (1022 MiB), 4096 inodes" + "[boot] provisioned + mounted /data". disk.img
  is now that image (old 16 MiB one kept as disk.img.16m).
- syscall.c lx open: vfs_create failures now map VFS_ERR_NOSPC->ENOSPC and
  VFS_ERR_NAMETOOLONG->ENAMETOOLONG instead of blanket EACCES. Errno
  fidelity is diagnostic infrastructure.
- chromewin: --disk-cache-size=64MB bounds cache growth.
- run_watch.py: -drive snapshot=on -- every run boots a pristine full-size
  /data; no cross-run profile/cache contamination.
- logs/defboot.sh gate had TWO blind spots, both fixed: -m 512 OOM-panicked
  the moment the chromium payload (~392 MB initrd) was staged (before any
  boot marker), and the fault grep pattern 'KERNEL PANIC' missed the actual
  'PANIC:' prefix -- so the gate reported CLEAN on a panicked boot. It has
  been vacuous since the chromium payload arrived; -m 4096 + 'PANIC' fix it.

## Slice 61d: COMMENTS RENDERED ON TOBYOS -- th=20, matching the host control
## field-for-field. The unlock: PAUSE THE VIDEO (idle starvation, proven).

New probe field `ric` (a requestIdleCallback self-counter) + choreography
change: DWELL now pauses the video for 60s at the bottom (what a user
reading comments does), TOP resumes it. First batch was an environmental
wash (renderer startup death w/ [pfrej] NO-VMA writes -> clean exit_group;
one nav ERR_ABORTED with ZERO requests -- YouTube/day-volume variance, none
of it reaches the 61d code). Second batch, run 2, the smoking-gun timeline:

    110-130s  video playing: ric FROZEN at 1, raf ~0.1/s, page skeleton
              (ytd=54 -- the app build itself was STARVED, not just lazy
              loading)
    ~140s     DWELL pauses the video: ric 1->5, raf accelerates
    150-190s  the deferred build executes: lk 0->20, cti 0->15,
              th 0 -> 2 -> 14 -> 20, page sh 837 -> 5710, sy reaches 3928

Final probe, tobyOS vs host headless=old control:
    th=20 vs 20 | cmt=40 (20 threads + 20 #content-text bodies) vs 40
    lk=20 vs 20 | imgs 34/88 vs 37/92 | ytd 7198 vs 8291 | meta populated
That is UI parity on this run: comments with real text, populated sidebar,
thumbnails decoding. (cmtTc/cmtH stay empty/0 ON THE HOST TOO -- wrong box,
do not read them.)

Mechanism, stated precisely: a PLAYING video layer keeps SwiftShader raster
backpressuring the compositor; the renderer computes no idle periods; both
YouTube's app build AND its comments module are idle-scheduled, so the page
freezes as a skeleton (or, on built pages, comments never load). Pausing
collapses raster load; ric moves; everything deferred executes within ~50s.
The 61b heartbeat remains necessary (BeginMainFrames at all); the pause
supplies the IDLE side. Video playback proof is unaffected -- it is banked
early in the run (this batch: b53.5 buffered, r4, before any pause).

Run-1-vs-run-2 discriminator (both built fully under pause): depth. Run 1
cruised only to sy=1992 -- above the comments trigger -- and got th=0; run
2 reached sy=3928 and got th=20. Slice 61e: cruise 900px/6s (was 600/8) +
idempotent re-pause each cruise pass; WARM=1 mode in run_watch.py bakes a
persistent warm profile (cached player JS) into disk.img so snapshot
batches start warm -- cold-profile builds were the flakiness driver.

## Slice 61e: DEFINITION-OF-DONE BATCH -- 2/3 runs at full parity, zero
## crashes, zero 403s, buffering record b59.1.

Warm profile (WARM=1 bake, then snapshot batch) + cruise 900px/6s +
idempotent re-pause. Tabulation:
    run1: b39.7 r4 | th=0  lk=20 cti=3  sy=2303   (trigger never fired)
    run2: b41.2 r4 | th=20 lk=26 cti=15 sy=5139   FULL PARITY
    run3: b59.1 r4 | th=20 lk=20 cti=15 sy=5134   FULL PARITY
The warm profile eliminated the dud-build and crash classes outright (three
healthy videos, three built pages, no 403 churn). Remaining variance: ~1/3
of runs the comments trigger does not fire despite bottom + pause (run1 sat
at sy=2303 on an ungrown page) -- YouTube-side A/B / build-order variance,
not a reproducible tobyOS defect class. Screencast frames lag the DOM by
minutes at this raster rate: screenshots show the player UI (controls,
scrubber, duration) but comment-region visual capture needs the pipeline to
catch up while deep -- DOM census is the ground truth for this arc.

## Slice 61f: the polish pass -- PARK visual capture, stuck-bottom jiggle,
## and the VMA silent-drop bug that explains the startup flake.

1. **PARK (visual capture) works.** New probe field thTop (first rendered
   thread's rect.top -- ytd-comments has a zero rect even when 20 threads
   are visible, host included, so it cannot aim). When CRUISE sees th>0 it
   parks: aims the first thread toward the viewport, holds still, keeps the
   video paused (re-pause each pass; app resumes re-starve the idle
   scheduler). Validated live: `th=20 thTop=-2789 -> PARK`, re-aim fired,
   threads stayed rendered (th=20 cti=19). Measured wheel gain ~1.8x
   (asked -2909px, moved ~5281) overshot the single full-delta aim -- now a
   HALVING controller (dy=(thTop-120)/2 per probe), which converges under
   any gain < 2. First-ever DEEP-PAGE visual: related-video tiles rendered
   on screen with full metadata (titles, channels, "24M views - 5 years
   ago", duration badges) -- wat_f of the single validation run.
2. **Jiggle**: once pinned at the bottom with th=0 (sy unchanged across
   probes), CRUISE alternates -700/+900 wheels -- direction changes
   re-fire IntersectionObserver deliveries; same-direction wheels at a
   pinned bottom move nothing.
3. **VMA silent-drop FIXED (the [pfrej] NO-VMA flake).** sys_munmap's
   middle-split shrank v->end even when the tail vma_alloc FAILED --
   silently dropping VMA coverage of [addr+len, old_end) while its PTEs
   stayed present: exactly the "present PTE, NO VMA" wild-access signature
   (slice-57 class; the 61d startup flake). Now the VMA is kept spanning
   the hole (freed frames demand-fault back as zero pages -- harmless for
   ANON) and it logs loudly. Also fixed: mprotect's middle-split abandoned
   a half-allocated `mid` entry on tail-alloc failure, leaving an
   UNINITIALIZED (stale-range) VMA live in the table; now rolled back.
4. **Harness hardening after a stale-flavor incident**: defboot.sh
   recompiles kernel.c STOCK and rewrites tobyOS.iso; a lingering QEMU held
   the ISO lock, build_vid's make failed SILENTLY (tail-2'd output), and a
   full 3-run batch executed on the stock ISO (pid1=hello-boot, zero
   chromewin) before anything noticed. Now: build_vid fails loudly + checks
   the ISO exists, run_x3 aborts on build failure, defboot re-touches
   kernel.c and prints a STOCK-ISO warning.

Context for the evening batches: YouTube service to this IP degraded after
~15 runs (403s returned, buffered halved to b24, one run's ric frozen at 1
until 270s despite the pause -- build barely started before the clock ran
out). th-rate is currently gated by that variance, not by tobyOS: when the
page builds, sidebar+tiles populate every time, and the one th>0 run
parked+held exactly as designed. Re-measure fresh (different hour or
alternate watch URL) before drawing any rate conclusions.

## Slice 62: LIVE WINDOW RESIZE with real reflow -- the mechanism WORKS
## (vw 800x600 -> 1278x697, 4/4 runs); post-resize frame DISPLAY still open.

The ask: resize the browser window and have the page relayout like real
chrome. Delivered mechanism, validated end-to-end on example.com (isolated
from YouTube's evening flakiness):
  tk_maximize / WM drag -> kernel window_do_maximize (backbuffer realloc +
  GUI_EV_RESIZE) -> tk.c updates win.w/h -> chromewin's debounced resize
  watcher -> Page.stopScreencast + Emulation.setDeviceMetricsOverride +
  Page.startScreencast at the new size -> CHROME REFLOWS. Every run that
  reached the resize reported the page's own viewport flip: vw=800x600
  before, vw=1278x697 after (stable across 18-19 probes). The window
  visually maximizes; paint() draws from live geometry and clears stale
  margins; wheel/probe/PARK coordinates all track g_page_w/h.
Also fixed en route:
- Screencast is now 1:1 with the page (was maxWidth 640 on an 800px page:
  pushed frames displayed at 0.8x while polled screenshots were 1:1, and
  any click during a pushed frame was off by 25%).
- Probe made body-null-safe (document.body.innerText threw on blank/dying
  pages, blinding probes exactly when needed; 'Cannot read properties of
  null' x4 diagnosed).
- libtoby sbrk pool 16 -> 64 MiB (lazy mmap reservation): the 16 MiB pool
  fragmented under 1.92 MiB frame churn and every 3.56 MiB post-resize
  decode OOM'd (1006 silent "JPEG decode failed"s -- now the failure line
  carries byte size + stbi's reason via new toby_image_error() + a live
  malloc(4MB) test).

OPEN (one item): post-resize frames still failed to decode on the one
healthy-boot run WITH the 64 MiB pool (1035x, ~10-12KB JPEG files = the
maximized mostly-blank page; decoded size 3.56 MiB). The diagnostic build
(reason= + malloc4M=) never got a healthy boot to fire on -- three
consecutive runs drew the FROZEN-LIFECYCLE boot class (raf pinned at 1,
zero BeginFrames from t=0, zero frames, captureScreenshot never resolves so
the polled fallback wedges on its one outstanding request). That frozen
class predates slice 62 (it is the ric=1 shape from the 61d evening runs)
and boot-alternates with healthy runs on the SAME binary. NEXT SESSION: one
healthy diagnostic run answers allocator-vs-codec outright; also consider
clearing g_shot_id on a timeout so a hung captureScreenshot cannot wedge
the polled path forever.
Note: the [pfrej] NO-VMA startup flake recurred (same 0xc0da000/0xc0db000
addresses) WITHOUT the slice-61f munmap WARN firing -- a second mechanism
produces the same VMA-less-present-PTE state; still open.

## Slice 62 addendum: the post-resize display blocker is CONFIRMED heap
## exhaustion -- and the numbers implicate libtoby's allocator, not sizing.

A healthy-boot diagnostic run finally landed: post-resize decode failures
report `reason=outofmem malloc4M=FAIL` -- at failure time even a bare
malloc(4MB) fails while small allocations keep working. The pool is 64 MiB;
steady-state frame churn (two ~1.9 MiB blocks per frame, freed each cycle)
plus the resize transition should peak under ~10 MiB. ~180 successful
frames before exhaustion does not add up for a healthy first-fit +
forward-coalesce allocator: suspect chunks being LOST (freelist damage or
failed coalescing under interleaved large/small churn) in stdlib.c. NEXT:
instrument the heap (tip position + freelist total per N frames) and run
the example.com + RESIZE_TEST harness -- it reproduces in one ~7 min run.
toby_image_load/free audited leak-free (all rgba temporaries freed on
every path; img->pixels/struct owned and freed by toby_image_free).

## Slice 62b: the allocator bug FOUND AND FIXED -- resize now works fully,
## display included (750+ frames at 1278x697, zero decode failures).

Mechanism (confirmed by the fix): libtoby malloc was FIRST-fit over a LIFO
free list. Each frame free()d its ~1.9 MiB pixel block onto the list HEAD;
the next frame's SMALL stbi temporaries first-fit into that freshest big
chunk and SPLIT it; the following 1.9 MiB request no longer fit the nibbled
remainder, so heap_grow advanced the wilderness tip -- up to a frame-sized
leak per cycle, order-dependent (hence ~180-frame survival, then
exhaustion at 64 MiB with "outofmem" while small allocations still worked).
Forward-only coalescing could not undo it across the interleaved live
temporaries.

Fix (stdlib.c): (1) BEST-fit -- small requests take small chunks, recycled
big chunks stay whole for big requests; exact-fit short-circuits. (2)
merge-on-pressure: coalesce_all() merges every address-adjacent free pair
and the fit retries BEFORE heap_grow is ever considered (same pattern as
the slice-57 VMA compaction). Validation run: 750+ frames decoded (was
~180 then starvation), 0 "JPEG decode failed" (was 1006/1035), frames at
1278x697 post-maximize, page rendered edge-to-edge with visible reflow
(the example.com paragraph re-wraps at the wider measure). raf=13091.

Resize arc definition of done: MET. WM resize -> viewport override ->
chrome relayout -> full-size frames decoded -> displayed 1:1.

## Slice 62c (spot check): react.dev -- a modern React 19/Next.js site --
## renders FULLY on chromewin. title=React, blen=266KB hydrated DOM,
## imgs 43/43 (100%) decoded, 420+ frames at ~5fps sustained, zero decode
## failures, zero crashes; screenshot shows photos, header chrome and
## typography pixel-perfect with the page scrolled deep. Framework compat
## is inherent (real V8+Blink); raster throughput is the only axis where
## light sites (react.dev, 5fps) and heavy ones (YouTube, ~sub-1fps under
## video) differ.

## Slice 63 (perf tier 1): instrumentation + display-path cleanup + the
## serial firehose. RESULT: display path is now ~FREE; production is the
## bottleneck -- the profile that aims tier 2.

63a INSTRUMENT: per-stage frame timers in chromewin (b64 strip / decode+
swizzle / paint), aggregated per 30-frame window, printed with each frame
line. Every future perf claim reads from here.
63b DISPLAY PATH: RGBA->ARGB now swizzles IN PLACE in stbi's own buffer as
u32 ops (keep A+G, swap R<->B) -- clang vectorizes it under -msse2 -- and
the buffer is ADOPTED as img->pixels (stbi allocates via our malloc). Kills
a full-size second allocation + byte-wise convert pass per frame; halves
the big-chunk churn that provoked the 62b allocator bug.
63c SERIAL FIREHOSE: the CHROMIUM_BOOT watchdog dumped ~450 lines (proc
table + BKL/per-CPU + 384-entry syscall ring + prof + waitt) EVERY 3s --
~50k lines/360s run, and every guest serial byte is a WHPX VM EXIT, so the
watchdog dragged on the system it watched. Now: 1-line [hb] at 3s
(liveness + logdrop), deep dump at 60s. [devpipe] capped (first 64 then
1-in-128; was 5k+/run uncapped). Log volume: 3-14 MB -> 0.7 MB (5-20x).

MEASURED, post-fix:
- react.dev, 630 frames: dec=1ms b64=0ms paint=0-3ms per frame. The
  display path is ~free; frame rate (~2-5fps) is limited by PRODUCTION
  (chrome compositor/SwiftShader/BeginFrame under the BKL'd syscall
  layer). Tier 2 (BKL fast paths for futex/pipe/epoll) is the aimed lever.
- YouTube watch (b52.1 buffered, th=20 lk=20 full parity on the same run):
  the one video-frame sample decoded at ~90-120ms -- photographic content
  is where stb hurts; libjpeg-turbo (or half-res capture during playback)
  remains the video-case lever. More samples wanted.
- Whole-run health with the quiet kernel: full-parity YouTube run with
  comments + b52.1 on the first try of the evening.

## Slice 64 (perf tier 2, step 1): sched_yield was the BKL serializer.
## MEASURED, then FIXED: 25x fewer BKL acquisitions, 3-4x more syscall
## throughput. The next wall is now visible and workload-dependent.

INSTRUMENT FIRST (this is what made the fix obvious in one run): per-CPU
BKL acq/waits/wait-cycles + a per-syscall top-8 histogram, both in the 60s
deep dump ([bkl] / [lx-top]). The very first measurement named the culprit:

    sched_yield = 401,443 calls / 60s  -- 100x the next syscall (futex 2969)

and EVERY one took the BKL just to run sched_yield()'s fast path, whose
whole body was `bkl_exit(); bkl_enter()` -- a full ticket-lock round trip
(back of the fair queue, spin, re-acquire). cpu1/cpu2 each showed ~400-430k
acquisitions with ~20k contended waits per interval. That is the
system-wide serialization the tier-1 profile predicted, now named.

FIX (sched.c sched_yield_fast + syscall.c pre-BKL hook): a yield by a
still-RUNNING caller with an EMPTY local ready queue is semantically a
no-op -- nothing can displace it -- so it is served entirely before
bkl_enter(). Two things preserved from the old under-BKL path: the 10ms
futex timeout sweep runs in the fast path (it serialises on g_futex_lock,
never the BKL), and poll_tick (which needs the BKL) is kept alive by having
the fast path DECLINE every ~2ms so that yield takes the normal path.
Also landed: BKL-free futex fast paths (WAIT with stale value -> -EAGAIN;
WAKE with no waiter list -> 0), which is chrome's dominant futex traffic;
the no-lost-wakeup argument holds because a parking waiter re-checks *uaddr
under g_futex_lock, which our WAKE path also takes for its lookup.

MEASURED AFTER (same YouTube watch workload):
    BKL acquisitions cpu1/cpu2:  430k/389k  ->  15k/15k   (~25x fewer)
    syscall throughput (fast):   time 742k -> 2.9M, futex 6.4k -> 20.7k
                                 (3-4x more work done per interval)
    yields served lock-free:     2.16M per interval
    health: th=20 lk=20 full parity, b43.4 buffered, zero crashes
react.dev A/B (display-bound page): 630 -> 660 frames (+5%), and its BKL
wait is <1% of wall time -- on light pages the limiter remains chrome's own
frame production (SwiftShader raster), exactly as slice 63 said.

THE NEXT WALL, honestly stated: on the VIDEO workload the remaining BKL
holders now dominate -- some CPUs show 64-84% of wall time waiting on the
BKL (wait-cycles ROSE even as acquisitions fell 25x, i.e. fewer but much
longer holds). Yield churn used to "pump" the lock; with it gone, real work
saturates it. NEXT STEP: a per-syscall BKL HOLD-time histogram (same shape
as [lx-top], but timing bkl_enter->bkl_exit) to name which syscalls hold it
longest under video, then give those the same treatment. Suspects from the
slow-path top-8: epoll_wait, recvmsg, mmap/mprotect, sendto.

## Slice 64b/64c (perf tier 2, step 2): WHO holds the BKL. Answer:
## FILESYSTEM WRITES -- pwrite64 + openat + unlink are ~90% of all hold
## time, and the BKL is held ~94% of wall clock. Not IPC. Not scheduling.

Instrument, and TWO wrong versions of it before the right one (both
self-announced, which is the point of sanity-checking a probe against a
value you can bound):
 1. Timing the syscall BODY reported futex=7.9M Mcyc -- 17x a CPU's entire
    cycle budget for the interval. Wall time under a syscall is NOT hold
    time: the scheduler DROPS the BKL around blocking switches.
 2. Timing bkl_enter->bkl_exit and reading proc->cursys at exit reported
    "execve = 93% of hold time" on a run with ~20 execve calls.
    linux_syscall clears cursys to -1 BEFORE the lock is released, so every
    segment fell through to cursys_nat -- which for an exec'd process is
    permanently 142, because execve never returns to clear its own stamp.
 3. Correct: a per-CPU bucket (g_bkl_sysno) stamped right after bkl_enter,
    cleared at the syscall's final bkl_exit; buckets cover Linux syscalls
    0..511, NATIVE syscalls 512+n (chromewin's GUI/blit path was invisible
    before), idle(pid0) and kernel. Caveat: a segment after a blocking
    switch can attribute to the syscall that re-stamped the CPU.

MEASURED (YouTube watch page, three consecutive 60s intervals):
    pwrite64  44k-131k Mcyc      openat  62k-92k      unlink  26k-91k
    everything else              <= ~2k Mcyc (1000x smaller)
    BKL held ~94% of wall clock; cpu2/cpu3 spend 80%+ of wall time WAITING
    for it while holding almost none themselves (starved).
So the video-case ceiling is chrome's file I/O into the journalled tobyfs
volume, executed under the global lock: each pwrite64 costs milliseconds
(journal begin/commit + synchronous block writes), and while it runs, every
other CPU that needs a syscall stops.

TESTED AND REJECTED (a flag cannot fix it): --disk-cache-size=1 +
--media-cache-size=1 left pwrite64 at 131k Mcyc (vs 126k at 64MB). This is
not discretionary cache traffic; the write PATH is the cost. Reverted to
the sane 64MB cache.

NEXT ARC (tier 2 step 3, the real fix, kernel-side and well-scoped now):
 (a) take FS I/O out from under the BKL -- per-mount/per-inode locking for
     the tobyfs write path, so a multi-ms journalled write blocks one
     writer instead of the whole machine; and/or
 (b) a write-back page cache for tobyfs (batch journal commits, coalesce
     sector writes) so the common small-write case stops touching the disk
     synchronously at all.
Either one should show up immediately in [lx-hold]/[bkl]: pwrite64's share
collapses and cpu2/cpu3 wait time falls. The instrumentation to prove it is
now in the tree ([bkl] per-CPU acq/waits/wait/held + [lx-hold] top-10).

## Slice 65 (perf tier 2, step 3): /data was on ATA **PIO**. Moving it to
## virtio-blk took the BKL from 94% held to 1.3%, and react.dev from
## 630 -> 1050 frames. The kernel-refactor arc was NOT needed.

Slice 64c said FS writes owned ~90% of BKL hold time. Before refactoring
locking, ONE question: why does a single pwrite64 cost milliseconds? The
answer was in the driver, not the filesystem: /data was attached
`if=ide`, and blk_ata.c is a **PIO** driver -- `rep outsw`, 256 words per
sector, through an I/O port. Under WHPX every port access is a VM EXIT, so
a journalled write is thousands of exits with the global lock held.
tobyOS already registers virtio-blk-pci at boot, and the /data mount sweep
finds the tobyOS-data GPT partition on ANY block device, so the fix was a
transport swap in the harness (run_watch.py; IDE_DATA=1 restores the old
attachment for A/B):

                        IDE PIO            virtio-blk       delta
  BKL held (of wall)    ~94%               ~1.3%            ~70x less
  pwrite64 hold         44k-131k Mcyc      (off the top-10) --
  openat / unlink       62k-92k / 26k-91k  1156 / 954       ~60-90x less
  cpu2/cpu3 wait        175k-217k Mcyc     65-1680 Mcyc     ~100x less
  react.dev frames      630 (s63) / 660    **1050**         +67%
  YouTube raf (60s)     89-108             **6104** (ric 3408)  ~60x
  YouTube frames        ~12 by 240s        60+              ~5x

Mount proof: `[virtio-blk] vblk0: capacity=2097152 sectors (1024 MiB)` +
`[boot] mounted /data via GPT partition 'vblk0.p1'`. Zero code changes to
the FS or the lock: the "per-inode locking / write-back page cache" arc
that slice 64c scoped is DEFERRED, and may never be needed.

THE BOTTLENECK HAS MOVED, and the instrument says where. On the VIDEO
workload the BKL is contended again (~64% held) but the holders are now
real IPC/work:
    epoll_wait 50641 | recvmsg 17516 | openat 16507 | futex 14955 |
    unlink 14312 | write 13837 | munmap 7880 | sendto 7350
epoll_wait dominating means threads sleep-and-wake through the BKL'd wait
path. NEXT LEVERS, in order: (1) BKL-free epoll/poll fast path for the
common "already-ready" and short-timeout cases, mirroring slice 64's
futex/yield treatment; (2) the remaining openat/unlink churn (chrome's
cache still creating/removing files -- now cheap per call, but frequent);
(3) then tier 2.5 (zero-copy frames, MIT-SHM) and tier 3 (GPU).
NOTE: defboot.sh deliberately still boots /data over IDE -- it is the
stock-config gate, and keeping it on the legacy path means the PIO driver
stays covered.

## Slice 66 (perf tier 2, step 4): net_poll coalescing TRIED and REVERTED.
## The epoll cost is FUTILE RE-SCANS, not network pumping.

Theory: every poll/select/epoll wait iteration that watches a socket calls
net_poll() (the B14 RX pump) -- a full NIC drain + TCP timer service -- and
with chrome's ~40 threads that is thousands of stack pumps per second under
the BKL, which would explain epoll_wait becoming the top hold-time holder
(50641 Mcyc/60s) once the disk was fixed.

Test (instrument + fix in ONE build so a single run judges it): a 200us
global coalescer, counting ran vs skipped.
RESULT: ran=54987 skipped=57538 -- half the pumps eliminated -- and
epoll_wait hold moved 50641 -> 48781 Mcyc, i.e. NOT the cost. Theory dead;
change REVERTED (an unproven rate-limiter on network servicing is a latency
risk with no measured payoff).

What the numbers actually say. From [lx-top] epoll_wait ~= 14,785 calls per
60s interval holding ~48,800 Mcyc => **~3.3 Mcyc (~0.77 ms) of BKL hold per
epoll_wait call** -- far too much for scanning <=64 fds once. The wait loop
re-scans EVERY time it is woken, and poll_tick wakes ALL blocked pollers
periodically (~1ms) regardless of which fd became ready: N pollers x M fds x
every tick, nearly all of it futile, all under the BKL. That is a thundering
herd, and it is a DESIGN issue, not a tuning one.

NEXT ARC (well-scoped, the real fix): event-driven poll wakeups. Attach
waiters to the objects they watch (socket / pipe / eventfd wait queues) and
wake only those whose fd actually became ready, instead of poll_tick's
wake-everyone sweep. tobyOS already has per-object wait queues (pipe.c /
socket.c wq_wake_all) and poll_wake_all's own comment anticipates an
"event-hook caller" -- so the machinery to hang this on exists. Expected
effect: epoll_wait's hold collapses toward the cost of ONE scan per real
event, and with it the last big BKL consumer on the video workload.

## Slice 67 (perf tier 2, FINAL): event-driven poll wakeups. Per-call
## epoll_wait BKL hold 3.3 -> 0.25 Mcyc (13x cheaper); chrome responded by
## doing 12x more epoll_waits in the same wall time.

Slice 66 named the structural problem: poll_tick woke EVERY parked poller
every ~1 ms regardless of which fd became ready, and each one re-scanned all
its fds under the BKL -- a thundering herd on a timer.

Fix: producers announce readiness. poll_event_notify() (coalesced at 100us,
BKL-held like poll_wake_all) is called from the paths that actually make an
fd ready -- inside pipe.c's and socket.c's wq_wake_all (so every existing
wake site is covered by construction), and at the AF_UNIX enqueue / accept /
write-space / peer-close sites in unix_socket.c, which is where chrome's
entire Mojo IPC lives. The unix_socket hooks are placed AFTER the
spin_unlock (poll_wake_all takes run-queue locks). poll_tick survives as a
SAFETY NET at 20 ms (was 1 ms) for any readiness source not yet hooked: a
missing hook costs bounded latency, never a lost wakeup.

MEASURED (video workload, valid run -- the first attempt drew the known
frozen-lifecycle boot class and was discarded):
  wakes             89% event-driven (pollevent 103,043 vs polltick 12,674)
                    total wake rate ~1000/s -> ~385/s, and real events now
                    wake pollers IMMEDIATELY instead of up to 1 ms later
  epoll_wait calls  14,785 -> 176,929 per 60s interval   (12x more work)
  epoll_wait hold   48,781 -> 44,153 Mcyc  (flat in total...)
  per call          3.3 Mcyc (~0.77 ms) -> 0.25 Mcyc (~58 us)   **13x**
  BKL held          ~65% -> ~49% of wall clock (94% at session start)
  health            raf=2907 ric=849, frames 60+, b20.0, lk=20, no crashes
The lock is no longer being burned on futile re-scans; it is now spread
across an order of magnitude more real syscalls (recvmsg 173,923 and
getsockname 126,423 per interval are the next-biggest callers -- both are
cheap per call, so the remaining BKL cost is breadth, not depth).

TIER 2 IS DONE. Cumulative across slices 64-67: sched_yield no longer
serialises (25x fewer acquisitions), the disk left ATA PIO (94% -> 1.3%
held on the UI path), and poll wakeups are event-driven (13x cheaper per
epoll_wait). What remains on the BKL is many cheap syscalls rather than a
few expensive ones -- the next structural step would be finer-grained
locking per subsystem, which is NOT worth doing before the display and GPU
tiers, because frame production is once again the ceiling.

## Slice 68 (perf tier 2.5): SIZING the zero-copy frame path before
## building it. Frame rate is PAYLOAD-BOUND: a 6x cheaper frame is 2.3x
## more frames. Zero-copy is therefore worth ~2.3x -- and needs the X11 arc.

T1 proved OUR side of the display path is free (decode 1 ms, paint 0-3 ms).
That leaves chrome's own JPEG ENCODE plus the base64/CDP transport as the
per-frame cost. Measured it directly instead of assuming, by making frames
cheaper and counting them (react.dev, identical 360 s runs):

    quality 60, 1:1  (800x600)   ~21 KB/frame    1050 frames   (baseline)
    quality 10, 1:1  (800x600)    9.8 KB/frame   1440 frames   (+37%)
    quality 10, 1/2  (400x300)    3.5 KB/frame   2430 frames   (+131%)

Frame rate tracks payload size, not scene complexity -- so the pipeline is
bound by per-frame encode+transport, exactly what a shared-memory surface
eliminates. **Zero-copy is worth up to ~2.3x at full fidelity**, which is
now a measured justification rather than an assumption.

BUT the cheap-frame configs are NOT shippable as-is: half resolution makes
body text unreadable (a browser's whole job), and quality 10 visibly
destroys text edges at full res. Reverted to quality 60 at 1:1 (fidelity +
correct click mapping, slice 62).

WHAT T2.5 ACTUALLY REQUIRES (and why it is not finished here): CDP can only
hand over base64-encoded images -- there is no shared-memory frame API. A
true zero-copy path means chrome must composite into memory we own, i.e.
Ozone X11 + MIT-SHM against the in-kernel fake X server, which in turn needs
the FULL chrome binary (chrome-headless-shell is headless-only). That is the
Route-B arc -- refuted earlier as a FUNCTIONALITY requirement (slice 61
proved headless renders everything), but it is exactly the right vehicle for
PERFORMANCE. Scope: full chrome + its extra DSOs into the initrd, then grow
src/socket.c's fake X server from handshake-only to CreateWindow / MapWindow
/ GetGeometry / PutImage+MIT-SHM, then read pixels straight out of the
shared segment. Big, but now demonstrably worth ~2.3x.

INTERIM OPTION worth one experiment next session, cheaper than the X11 arc:
Emulation.setDeviceMetricsOverride with deviceScaleFactor 0.5 renders (and
encodes) a quarter of the pixels while keeping CSS geometry -- so input
coordinates stay correct, unlike the maxWidth scaling that caused the
slice-62 click skew. That trades sharpness for smoothness with correct
input, and is a one-flag test.

## Slice 69 (tier 2.5 continued): the dsf interim FAILED; Route B milestone
## 1 (FULL chrome on tobyOS) got two real kernel/build gaps fixed and now
## runs 10x further -- symlink(2) was missing, which is what killed it.

PART A -- interim half-raster: TESTED AND REJECTED. Theory:
Emulation.setDeviceMetricsOverride{deviceScaleFactor:0.5} would raster and
encode a quarter of the pixels while CSS geometry (hence click mapping)
stayed 1:1 -- the safe version of the resolution trick. Implemented with a
2x nearest-neighbour upscale band-blit on our side. RESULT: 990 frames vs
the 1050 baseline (WORSE), and frames still arrived 800x600 with a LARGER
payload (23 KB vs 21 KB): Page.startScreencast captures at CSS size and
ignores deviceScaleFactor<1. Reverted. So there is no cheap interim: the
only ways to shrink the per-frame payload are quality (destroys text) or a
real shared-memory surface.

PART B -- Route B milestone 1: does the FULL chrome binary run at all?
 - Fetched chrome 151.0.7922.71 (chrome-linux64, 389 MB extracted, 290 MB
   binary vs the 262 MB headless-shell tree).
 - DT_NEEDED diff vs headless-shell: only FOUR extra direct libs
   (libcairo, libcups, libpango, libsmime3) -- far less than feared.
 - Discovering the TRANSITIVE closure one guest boot at a time would cost
   ~7 min per library (chrome's loader reports only the first failure), so
   programs/chromium/closure.py walks DT_NEEDED across the binary + every
   staged .so, subtracts glibc-provided names, maps sonames to Debian
   packages, fetches and stages, and repeats until empty. CLOSED in 4
   rounds / seconds: cups pulled in the whole GSSAPI/Kerberos + GnuTLS
   chain (gssapi_krb5, krb5, k5crypto, com_err, krb5support, keyutils,
   gnutls, tasn1, nettle, hogweed, gmp, p11-kit, idn2, unistring, ssl3).
   186 libs staged; initrd 392 MB -> 550 MB, ISO 583 MB (RAM-loaded, needs
   the existing -m 6144).
 - Makefile: CHROME_FULL=1 stages chrome-linux64 as /opt/chrome instead of
   the shell (they cannot both fit); logs/build_full.sh drives it.
 - BUILD BUG FOUND: EXTRA_CFLAGS only reaches KERNEL objects, so
   -DCHROME_FULL silently missed chromewin, which kept exec'ing the (now
   unstaged) headless-shell path -- an exit-127 that looked exactly like a
   loader failure. Added PROG_EXTRA_CFLAGS for user programs.
 - KERNEL GAP FOUND AND FIXED: symlink(2)/symlinkat(2) were never wired to
   the Linux personality even though vfs_symlink() existed. Full chrome's
   ProcessSingleton ("one instance per profile" -- something
   chrome-headless-shell does not have) creates a SingletonLock SYMLINK and
   treats failure as FATAL (chrome_main_delegate.cc:520). With symlink
   wired, that error is gone. This benefits every Linux app, not just
   chrome.
 - Diagnosis tooling: the CHROMIUM_BOOT stdout hook logged only fd 1;
   chrome reports its fatal reasons on fd 2, so an exit_group(21) had no
   explanation anywhere. Now both fds are echoed (120-char window) -- which
   is how the ProcessSingleton message was found at all.

STATUS: full chrome LOADS, links its whole DSO closure, and now runs 1,269
syscalls (was 128) -- through getrandom/mkdir/stat/prctl/sendmsg (its Mojo
handshake) -- before exit_group(191) with NO fatal message logged. That
exit is the next thread to pull; it is an application-level decision, not a
loader or ABI failure. NOTE the default (headless-shell) flavour is
untouched and still the one build_vid.sh produces.

## Slice 70 (Route B m1 cont.): sigaltstack(2) implemented; full chrome's
## exit 191 narrowed to a post-handshake, self-relaunch path + a NO-VMA
## SIGSEGV in a child. Not yet closed -- state recorded for the next pass.

- sigaltstack(2) was UNHANDLED (-ENOSYS). Every Linux crash handler --
  crashpad here -- installs an alternate signal stack so it can still run
  when the faulting thread's own stack is the problem, and logs an ERROR
  when the call fails. Now recorded/reported faithfully (SS_DISABLE and the
  getter work; MINSIGSTKSZ enforced), while signal delivery still uses the
  normal stack -- identical behaviour to -ENOSYS, minus the error and the
  risk of a caller treating it as fatal. The crashpad ERROR is gone.
- Raised the fd1/fd2 echo to 600 lines x 200 chars and added --v=1 for
  CHROME_FULL builds, since chrome names its own failures when asked.

WHERE EXIT 191 STANDS (facts, no theory):
 - chrome links its full DSO closure, passes ProcessSingleton (slice 69's
   symlink fix), and reaches its Mojo handshake: 1,269 syscalls through
   getrandom/mkdir/stat/prctl/sendmsg.
 - With --v=1 it emits the crash-reporter consent line TWICE, from two
   processes -- i.e. the browser re-runs its own startup (a relaunch or a
   child of a type we have not identified). pid 1 exits 0; the other exits
   191 (0xBF), a code that matches no small RESULT_CODE_* enum, so it is
   probably not a plain chrome result code.
 - In one run a chrome child took SIGSEGV on an UNMAPPED address
   (`[pfrej] ... NO VMA`, 515 VMAs live, nearest mapping far away) -- the
   same no-VMA class whose FIRST mechanism was fixed in slice 61f
   (munmap middle-split) and whose SECOND mechanism is still open. Full
   chrome exercises far more of the memory ABI than the headless shell, so
   it may simply be a better reproducer for that known bug.
NEXT PASS, cheapest first: (1) log the argv of EVERY chrome exec (we log
only the first) to identify what the second process is -- zygote, relaunch,
or a --type= child; (2) instrument the faulting VA against the VMA table at
fault time to catch the second no-VMA mechanism with a live reproducer;
(3) only then judge whether exit 191 is a consequence of the SIGSEGV or an
independent decision.

## Slice 71 (Route B m1 cont.): exit 191 IDENTIFIED as chrome's own CHECK
## (INT3), deterministic and FLAG-INDEPENDENT. Symbolization is a dead end;
## the next move is a control, not more guessing.

Facts established this pass:
 - `[sigfault] pid=3 name=chrome vec=3 sig=5 rip=0x624a118` -- vector 3 is
   INT3, i.e. base::ImmediateCrash/CHECK. exit_group(191) is the AFTERMATH
   of chrome deliberately aborting itself, not a kernel kill and not an ABI
   rejection.
 - DETERMINISTIC: identical rip across runs.
 - FLAG-INDEPENDENT: replacing the whole GL flag set (--use-gl=angle
   --use-angle=swiftshader --enable-unsafe-swiftshader --in-process-gpu ->
   --disable-gpu, letting the full binary build its own stack) reproduced
   the SAME rip and the SAME exit. So the GL/Ozone flags are exonerated;
   reverted to one code path.
 - The earlier "chrome relaunches itself" reading is CORRECTED: both
   GetCollectStatsConsent lines carry the same `[3:3:` pid:tid, so one
   browser process logs it twice. What actually happens is pid 3 FORKS
   (no exec) a child that exits 0; the browser then CHECKs.
 - The child's `[pfrej] NO VMA` fault sits just past the end of a giant
   PROT_NONE reservation [0x102b00002000,0x102f00000000) -- the V8
   pointer-compression cage / PartitionAlloc pattern. mmap's MAP_FIXED path
   was audited and does honour the requested base, and the VMA table is
   correctly keyed by tgid for threads, so neither is the explanation yet.
 - Symbolization is impractical: chrome is a stripped PIE (only ChromeMain
   exported). Loaded at base 0x500000, so rip -> vaddr 0x5d4a118, and the
   INT3 byte is at rip-1 (the trap pushes the following address). Nearby
   .rodata strings are unrelated neighbours, not the CHECK's own message --
   and no CHECK text ever reached stderr even at --v=1, which is itself
   odd (a LOG(FATAL) should print file:line first).

NEXT MOVE -- run the CONTROL, per this project's own prime directive:
execute THIS EXACT binary with THESE EXACT flags somewhere known-good
(WSL/Linux box, or docker) and see whether it also CHECKs. If it does, the
flags/environment are wrong and tobyOS is exonerated (this is precisely the
mistake slices 59-60 made by not controlling first). If it runs there and
CHECKs here, THEN diff the environment: the fork-without-exec child, the
absent /proc entries chrome reads, or a syscall returning a plausible-but-
wrong value. Only after that does instrumenting the trap site pay.

## Slice 72: THE CONTROL EXISTS NOW (WSL2 Ubuntu 26.04). Verdict: the same
## binary with the same flags runs FINE on known-good Linux -- so the
## full-chrome CHECK is OURS. Two hypotheses already eliminated.

Installed WSL2 Ubuntu 26.04 (user-approved) purely as a reference machine,
and ran the EXACT chrome-linux64 binary with the EXACT flags chromewin
passes. Scripts (reusable, keep them): logs/control_fullchrome.sh,
logs/control_diff.sh, logs/control_policy.sh; output under logs/control/.

RESULT -- healthy on Linux, three flag variants, none crash:
    tobyflags (angle/swiftshader/in-process-gpu)  exit=124 (my 90s timeout)
    disablegpu                                     exit=124
    bare (no GL flags at all)                      exit=124
    stderr 850-905 lines of normal startup; ZERO CHECK/FATAL/signal lines.
On tobyOS the same binary emits 3 lines and INT3s. **So tobyOS is NOT
exonerated -- the difference is ours.** (Note --dump-dom does not terminate
the FULL browser the way it does chrome-headless-shell: it keeps running as
a browser session, hence the timeout. Fine for this question.)

DIFF AT THE DEATH POINT. tobyOS's last output is the 2nd
GetCollectStatsConsent line; a healthy startup continues immediately with:
config_dir_policy_loader -> policy_service_impl snapshot -> variations
FieldTrialTestingConfig -> scheduler_loop_quarantine -> **dbus GetNameOwner
org.freedesktop.login1** -> webrtc_event_log_manager -> proxy_config ->
cdm_registration (Widevine) -> component_crx_cache stat. tobyOS reaches
NONE of it, so the CHECK fires in that early window.

ELIMINATED SO FAR (each by experiment, not argument):
 1. GL/Ozone flags -- swapping the whole set changed nothing on either side.
 2. The staged POLICY FILE. The initrd installs
    etc/opt/chrome/policies/managed/tobyos.json (the ML-KEM workaround) and
    Ubuntu had none, making it the most visible difference at exactly the
    right moment. Reproduced it on the control: chrome logged "Found
    mandatory policy file", parsed it, and ran on with no CHECK. NOT it.

STILL OPEN, in the order worth testing (all now cheap, because the control
can be degraded toward tobyOS one property at a time):
 a. D-BUS: the healthy log shows chrome talking to a real bus
    (GetNameOwner org.freedesktop.login1) seconds after the death point;
    tobyOS has no bus at all. Test: point DBUS_SYSTEM/SESSION_BUS_ADDRESS
    at a nonexistent socket on the control and see if it CHECKs.
 b. /proc completeness -- chrome reads several entries during variations /
    field-trial setup; a plausible-but-wrong value is exactly the class of
    bug that produces a CHECK rather than an error return.
 c. The fork-without-exec child (browser forks, child exits 0) -- identify
    what that child is on the control (it will be there too) and compare.

## Slice 73: environment bisection on the control. Libraries EXONERATED,
## /proc/self/exe EXONERATED. The control's ICU crash was an artifact of the
## explicit-loader invocation -- and proving that took one command.

Bisected the environment chromewin hands chrome, one property at a time, on
the WSL control (exit 124 = ran the full timeout = healthy):
    baseline                                       124  healthy
    + LD_PRELOAD (bundled vulkan/swiftshader)      124  healthy
    + DISPLAY=:0                                   124  healthy
    + HOME/TMPDIR/LANG=C                           124  healthy
    + LD_LIBRARY_PATH=<chrome>:<our sysroot>       139  SIGSEGV, 0 stderr
Two harness bugs found and fixed first (worth remembering): exporting the
env into the subshell also applied it to wc/head/cut/mkdir, and `env VAR=..
timeout` applies it to timeout itself -- in both cases OUR bookworm sysroot
shadowed Ubuntu's glibc 2.43 and killed the harness, not chrome. Correct
form: `timeout N env VAR=... ./chrome`.

That sysroot result is NOT evidence against our libraries: on Ubuntu our
sysroot shadows the system libc/libm (2.36-era vs 2.43), a mixing that
cannot happen on tobyOS where our in-tree glibc is the only one. (The
sysroot does contain libc/libm/ld.so, dated 07-17 -- pre-existing, NOT added
by slice 69's closure.py, so the working headless flavour is unaffected.)

To test the library set faithfully, ran chrome under OUR loader with
--library-path (tobyOS's exact library world, real kernel):
    exit=133 (SIGTRAP) + "ERROR:base/i18n/icu_util.cc:232] Invalid file
    descriptor to ICU data received."
which LOOKED like the answer -- same trap shape as tobyOS. It is not.
Control for it: doing the same with UBUNTU's OWN loader fails identically.
So the ICU failure is caused by invoking chrome through an explicit loader
(/proc/self/exe then names ld.so, and chrome derives DIR_ASSETS from it),
not by our libraries. **Libraries exonerated.**

Then checked the same mechanism ON tobyOS with a new [procexe] probe:
    [procexe] pid=3 -> '/opt/chrome/chrome' (exe_path)   x3
Correct path, from exe_path, not the p->name fallback. **/proc/self/exe
exonerated too** -- and note this is the very bug an earlier slice fixed for
headless-shell (fork.c resolves symlinks so DIR_ASSETS is right), still
holding.

Also fixed: chromewin.o does not depend on the flavour define, so a
KERNEL-only edit left make reusing the object built for the OTHER flavour --
a CHROME_FULL initrd then shipped a chromewin that exec'd the absent
headless-shell path and died with exit 127 (the same stale-object class as
the hard rules). build_full.sh and build_vid.sh now delete it every build.

RUNNING TALLY of what is NOT the cause of full chrome's INT3: GL/Ozone
flags, the staged policy file, our library set, /proc/self/exe/DIR_ASSETS.
STILL OPEN: D-Bus absence (chrome talks to a real bus seconds after our
death point; tobyOS has none), /proc completeness beyond exe, and the
fork-without-exec child. The control makes each of these a one-command test
now -- degrade Ubuntu toward tobyOS rather than guessing across guest boots.

## Slice 74: STRACE ON THE CONTROL NAMES THE FAILING SUBSYSTEM -- crashpad's
## handler handshake (SCM_RIGHTS over a SOCK_SEQPACKET socketpair). tobyOS
## sends and never receives.

Strace of the healthy browser's MAIN THREAD on Linux, at exactly the point
tobyOS's [lx-recent] tail ends, decodes the sequence tobyOS could only show
as bare syscall names:

  socketpair(AF_UNIX, SOCK_SEQPACKET, 0, [4,5])       <- crashpad channel
  sendmsg(4, ..., msg_control=[{cmsg_len=28, ...}])   <- SCM_RIGHTS fd pass
  recvmsg(4, ..., msg_control=[{cmsg_len=28, ...}])   <- HANDLER REPLIES
  prctl(PR_SET_PTRACER, 541)
  mmap(16K PROT_NONE) + mprotect(8K RW) + sigaltstack({ss_sp=..,8192})
  rt_sigaction(SIGABRT/BUS/FPE/ILL/QUIT/SEGV/SYS/TRAP, SA_ONSTACK|SA_SIGINFO)

tobyOS's tail for the same thread:
  ... prctl, rt_sigprocmask, sendmsg, rt_sigprocmask, rt_sigaction,
      getpid, gettid, exit_group(191)
The sendmsg happens -- and then there is **no recvmsg at all**. Chrome goes
straight to signal setup and aborts. So the crashpad handler handshake is
where full chrome dies, which also explains the shape we already had: the
browser FORKS A CHILD WITHOUT EXEC (that child IS the crashpad handler --
crashpad forks it rather than exec'ing), the child exits 0 having failed its
side, and the browser CHECKs. It also explains why sigaltstack mattered
enough to implement in slice 70 (crashpad installs one immediately after
this handshake) and why --disable-crash-reporter does not help (chrome still
performs the handshake).

Confirmed present, so NOT the gap: SOCK_SEQPACKET (socketpair accepts it;
socket.c preserves message boundaries) and AF_UNIX pair creation (the run
logs `[unix] pair pid=3 a=1 b=2`).

Instrument note for the next pass: the [chan] ring only records fds whose
file kind is FILE_KIND_SOCKET, so AF_UNIX socketpairs never appear in it --
which is why ungating its 18s timer (done here for CHROME_FULL) still showed
nothing. The next probe must hook the AF_UNIX path itself: log sendmsg/
recvmsg on unix sockets with their RETURN VALUE and whether a cmsg was
attached/delivered. That single line will say whether our sendmsg rejected
the SCM_RIGHTS message, whether the handler child ever read it, or whether
the reply was sent but never delivered.

## Slice 75: THE FAILING CALL IS CAUGHT. crashpad's handshake sendmsg
## returns EPIPE -- the peer endpoint is already gone because the forked
## crashpad child dies before replying.

New [uxmsg] probe on the AF_UNIX sendmsg/recvmsg path (the [chan] ring
cannot see socketpairs -- it only records FILE_KIND_SOCKET fds, which is why
slice 74's ungating showed nothing). One line says it all:

    [uxmsg] pid=3 SEND fd=6 len=40 nscm=0 -> -32        (-32 = EPIPE)

40 bytes is exactly the iov_len strace showed for crashpad's handshake
message on Linux, so this IS that call. Two facts in one line: the send
FAILS with EPIPE (our socket believes the peer is gone), and we collected
nscm=0 SCM_RIGHTS descriptors where Linux's trace carries a cmsg -- though
the EPIPE comes first and is the fatal part. Chrome's crashpad client treats
that failure as unrecoverable -> INT3 -> exit_group(191). That is the whole
chain, end to end.

WHY the peer is gone (evidence, consistent across runs):
 - `[unix] pair pid=3 a=0 b=1` then `[unix] pair pid=3 a=1 b=2` -- slot 1 was
   FREED and reused, i.e. an endpoint was fully torn down between the two
   socketpair() calls.
 - `[uxclose] pid=3 closed unix sock slot=1 -> peer slot=0 sees EOF`.
 - The browser forks a child that exits 0 almost immediately, and in one run
   that child took `[pfrej] NO VMA` faults (addr 0x102f00c68xxx, just past
   the V8/PartitionAlloc PROT_NONE reservation) before dying.
Endpoint refcounting is NOT the bug: fork clones fds via file_clone, which
does sock_ref for FILE_KIND_SOCKET, and sock_close only tears down at the
last reference. The endpoint dies because the CHILD really does exit -- when
it goes, its references drop and the parent's next send is EPIPE.

So the remaining question is narrow and concrete: WHY does the forked
crashpad child die instead of replying? On Linux crashpad DOUBLE-FORKS (the
first child forks the grandchild that execs chrome_crashpad_handler, then
_exit(0)s immediately to reparent it) -- and our execve-argv log shows only
ONE exec for the whole run, so the grandchild's fork/exec never happened.
NEXT PASS: (1) log fork/exec from inside the crashpad child (we currently
only see `[fork] parent pid=3 -> child pid=1` and no second fork); (2) chase
the child's NO-VMA fault -- it is the same second mechanism left open since
slice 61f, and the crashpad child is now a REPRODUCER for it; (3) confirm
whether /opt/chrome/chrome_crashpad_handler is staged and branded
(EI_OSABI=Linux) in the CHROME_FULL initrd, since a failed exec of it would
produce exactly this shape.

## Slice 76: ROOT CAUSE FOUND AND FIXED -- clone() dispatched on CLONE_VM
## instead of CLONE_THREAD, so vfork/posix_spawn children became THREADS.
## The crashpad handshake now completes; chrome_crashpad_handler runs.

Followed the crashpad child's own syscalls and it ended: getdents64, mmap,
rt_sigprocmask, **clone3**, munmap, exit(0) -- with NO fork logged either way,
so clone3 never reached the fork path. Logged its flags:

    [clone3] pid=1 flags=0x100004100 -> (we said) THREAD   <- crashpad child
    [clone3] pid=3 flags=0x3d0f00    -> THREAD             <- a real pthread

Decoded: 0x100004100 = CLONE_VM|CLONE_VFORK|CLONE_INTO_CGROUP, with
**CLONE_THREAD CLEAR** -- vfork/posix_spawn semantics, i.e. a separate
PROCESS that shares the address space only until it execs. The real pthread
next to it is 0x3d0f00 = VM|FS|FILES|SIGHAND|**THREAD**|SYSVSEM|SETTLS|
PARENT_SETTID. Both LX_clone and LX_clone3 tested **CLONE_VM (0x100)** to
decide "thread", which is simply the wrong bit: CLONE_VM only means "share
the address space", and vfork sets it too.

THE CHAIN, end to end: crashpad double-forks (first child spawns the
grandchild that execs chrome_crashpad_handler, then _exit(0)s). We turned
that spawn into a thread, so the grandchild NEVER EXISTED; the first child
exited; its socketpair endpoint refs dropped; the browser's handshake
sendmsg hit a dead peer -> EPIPE (slice 75's [uxmsg] line) -> crashpad
treats it as fatal -> INT3 -> exit_group(191).

FIX: dispatch on CLONE_THREAD (0x10000) in both LX_clone and LX_clone3.
vfork-style children become plain forks, which is semantically safe because
such a child only execs or _exits. defboot stayed clean.

MEASURED AFTER (same run, all four previously-impossible things):
    [clone3] pid=1 flags=0x100004100 -> FORK      (and 0x3d0f00 -> THREAD)
    [fork]   parent pid=1 -> child pid=4          (the grandchild EXISTS)
    execve-argv pid=4: /opt/chrome/chrome_crashpad_handler   (it EXECS)
    [uxmsg]  pid=3 SEND fd=6 len=40 -> 40         (was -32 EPIPE)
    [uxmsg]  pid=4 RECV fd=7 want=40 -> 40        (the handler RECEIVES it)
This is a genuine Linux-ABI bug that affected far more than chrome: any
program using posix_spawn or vfork+exec got a thread instead of a process.

STILL exit 191, but now from a LATER stage: with the handler alive, the
remaining trace shows `[uxmsg] pid=1 RECV fd=4 want=40 -> 0` (the first
child reads 0 = EOF where Linux delivers the handler's reply carrying
SCM_RIGHTS), and note every [uxmsg] line so far reports nscm=0 -- we are not
carrying the ancillary fd. Next: make SCM_RIGHTS ride the socketpair reply.

## Slice 76b: the LAST missing piece is named exactly -- SCM_CREDENTIALS.

Decoded the handshake contract from the control's strace rather than
guessing at our nscm=0:

  browser -> handler:  sendmsg(4, iov_len=40, **msg_controllen=0**) = 40
  handler -> browser:  recvmsg(4, iov_len=8,
                         msg_control=[{cmsg_level=SOL_SOCKET,
                                       cmsg_type=**SCM_CREDENTIALS**,
                                       cmsg_data={pid=541,uid=0,gid=0}}]) = 8
  browser:             prctl(PR_SET_PTRACER, **541**)   <- the pid FROM the creds

Two consequences:
 1. `nscm=0` on our SEND is CORRECT -- that message genuinely carries no
    ancillary data. It was never the bug; do not "fix" it.
 2. What chrome actually needs back is an 8-byte reply carrying
    SCM_CREDENTIALS, and it uses the pid inside to set its ptracer. tobyOS
    has no SCM_CREDENTIALS/SO_PASSCRED, so even now that the handler runs
    and receives the request (slice 76), its reply cannot carry what the
    browser requires -- which is why the browser still CHECKs, and why the
    first child's read comes back 0.

NEXT (well-scoped, the likely last step of this arc): implement
SO_PASSCRED + SCM_CREDENTIALS on the AF_UNIX path -- synthesize the cmsg
from the SENDING proc's pid/uid/gid at recvmsg time (the kernel already
knows all three; nothing needs to be carried in the ring), and accept/ignore
SO_PASSCRED in setsockopt. Then re-run: chrome should get its reply, call
PR_SET_PTRACER, install the SA_ONSTACK handlers, and continue past crashpad
into normal browser startup.

## Slice 77: SO_PASSCRED + SCM_CREDENTIALS implemented. Chrome advances to a
## new, narrower failure: crashpad "incorrect payload size 0".

Implemented what slice 76b specified:
 - struct sock_dgram carries the SENDER's {pid,uid,gid}, stamped at ENQUEUE
   (by dequeue the sender may be gone -- crashpad's first child exits by
   design, which is exactly the case that matters here).
 - sock_unix_recv_fds publishes them for the message it just dequeued, on
   the same "first read of the message" rule the SCM_RIGHTS hand-off uses.
 - SO_PASSCRED (16) recorded in setsockopt; lx_scm_recv_emit emits a
   SOL_SOCKET/SCM_CREDENTIALS cmsg when the receiver asked for it and no
   SCM_RIGHTS was written, with CTRUNC if the control buffer is too small.
VERIFIED: `[scmcred] emitted pid=3 uid=0 gid=0 ctl_len=28` -- 28 bytes is
exactly what the control's strace reports (CMSG_LEN(sizeof(struct ucred))).
defboot clean.

Chrome now gets PAST the dead-peer failure and reports something new and
much more specific, from crashpad itself:
    third_party/crashpad/crashpad/util/linux/socket.cc:182]
        incorrect payload size 0
with `[uxmsg] pid=4 RECV fd=7 want=40 -> 40` (the handler DOES receive the
40-byte request) followed by reads returning 0. So the request now arrives
intact and the credentials ride correctly; what is still missing is the
handler's REPLY payload -- crashpad's UnixCredentialSocket expects a
specific number of bytes and is seeing none.

NEXT (narrow): trace the handler's own send after it receives the request --
does chrome_crashpad_handler call sendmsg at all (no [uxmsg] SEND from pid 4
appears in the captured window), and if so does our SEQPACKET path deliver
its payload? The [uxmsg] probe already covers both sides; extend its cap and
read the handler's half of the conversation.

## Slice 78: the "incorrect payload size 0" is a PREMATURELY SEVERED PEER
## LINK, not a missing reply. The handler's socket has peer=-1 before it
## ever reads.

Added peer/queue state to the [uxmsg] RECV probe, which settles it:

    [uxmsg] pid=3 SEND fd=6 len=40 nscm=0 -> 40
    [uxmsg] pid=4 RECV fd=7 want=40 flags=0x0 -> 40 nscm=0 **peer=-1** count=0
    [uxmsg] pid=4 RECV fd=7 want=40 flags=0x0 ->  0 nscm=0 **peer=-1** count=0

peer=-1 means self->peer_ip == 0, i.e. the endpoint's peer link is ALREADY
cleared on the read that succeeds. The 40 bytes arrive only because they
were queued before the severing; the next blocking read then finds "peer
gone AND ring drained" and returns 0 -- which is CORRECT behaviour for a
real EOF, and is exactly what crashpad reports as
`util/linux/socket.cc:182 incorrect payload size 0` before exiting. So the
handler is not failing to reply: it is being told the conversation is over.

Mechanism to chase next (narrow): sock_unix_peer_close() clears
`peer->peer_ip = 0` permanently when an endpoint hits its LAST reference.
That is right for a genuine close, but this channel passes through
crashpad's double-fork -- browser creates the pair, forks a middle child,
the middle child forks the handler and _exit(0)s immediately, and the
handler then execs and creates its OWN pair ([unix] pair pid=4 a=4 b=5).
Somewhere in that sequence an endpoint reaches refs==0 and severs the link
the handler still needs. The [uxref] trace shows every ref/close with its
outcome and [uxclose] names the severing side, so the next step is simply
to correlate them BY SLOT with the fd the handler reads (fd=7):

    which slot is fd=7, and which [uxclose] cleared its peer, and at that
    moment who still held the other end?

Prime suspects, in order: (1) the middle child's exit closing inherited
copies while the handler's exec is still in flight; (2) an endpoint being
torn down and its slot RECYCLED by the handler's own socketpair (the a=4
b=5 pair appears right after a teardown of slot 4 in the same run), which
would alias a live channel onto a fresh one -- note sock_unix_pair reuses
pool indices immediately, and peer_ip stores index+1 with no generation
counter, so a recycled slot is indistinguishable from the original.
Suspect (2) would also explain the run-to-run variability seen all along.

## Slice 79: CORRECTION -- the handler's EOF is a CONSEQUENCE, not the
## cause. The browser closes and exits 191 BEFORE the handler ever reads,
## and it never attempts to read the handshake reply at all.

Timestamped correlation of the whole conversation (one run, same log):

    7277  [unix] send pid=3 sock=0 -> peer=1 len=40 peer_count=1   (queued OK)
    7278  [uxmsg] pid=3 SEND fd=6 len=40 -> 40                     (success)
    7297  [uxclose] pid=3 closed unix sock slot=0 -> peer slot=1 sees EOF
    7301  [proc] pid=3 'chrome' exit code=191                      <-- ALREADY DEAD
    7468  [unix] recv pid=4 sock=1 peer=-1 queued=1 want=40        (handler reads)
    7470  [uxmsg] pid=4 RECV -> 40  peer=-1
    7521  [uxmsg] pid=4 RECV -> 0   peer=-1  -> "incorrect payload size 0"

So slice 78's reading was right about the mechanism (peer link cleared) but
WRONG about its significance: the browser itself severs the link at 7297 by
closing its own end on the way out, 170 ms BEFORE the handler's first read.
The handler's EOF and crashpad's payload complaint are downstream of a
browser that has already aborted. Chasing the handler further would have
been wasted effort -- exactly the kind of detour this project's control
discipline exists to prevent.

THE REAL GAP, now precisely stated: **the browser never issues the reply
read.** There is no recv of ANY kind from pid 3 on that socket -- not
recvmsg ([uxmsg] logs none), not read/recvfrom ([unix] recv logs none,
and it logs both the pid-4 and pid-6 reads, so the probe works). On Linux
the control's strace has recvmsg IMMEDIATELY after the sendmsg, on the same
fd, with no syscall between them:
    sendmsg(4, iov_len=40, controllen=0) = 40
    recvmsg(4, iov_len=8, [SCM_CREDENTIALS {pid,uid,gid}]) = 8
    prctl(PR_SET_PTRACER, <that pid>)
and tobyOS's own ring shows the browser going sendmsg -> rt_sigprocmask ->
rt_sigaction -> getpid -> gettid -> exit_group instead. So between "sendmsg
returned 40" and "decide to abort", crashpad takes an error path WITHOUT
reading -- meaning something about the send's OUTCOME (not the reply) is
unacceptable to it.

NEXT, in order:
 1. The send is a SOCK_SEQPACKET sendmsg whose msghdr carries msg_name=NULL,
    msg_controllen=0, flags=MSG_NOSIGNAL. Check what lx_sendmsg returns in
    the msghdr's OUT fields and whether MSG_NOSIGNAL (0x4000) is being
    misread as something else -- we ignore `flags` entirely in lx_sendmsg
    ((void)flags on the first line).
 2. Verify the socket is actually SOCK_SEQPACKET end-to-end: lx_socketpair
    accepts the type but stores nothing, so getsockopt(SO_TYPE) reports
    whatever the generic path says. crashpad checks socket properties
    before use.
 3. Only then look further afield.

## Slice 80: SO_TYPE now reports the real socket type (a genuine bug), but
## it is NOT the crashpad blocker -- same failure, unchanged.

getsockopt(SO_TYPE) answered from `kind` alone: TCP -> SOCK_STREAM,
everything else -> SOCK_DGRAM. So every AF_UNIX socket reported SOCK_DGRAM,
including the SOCK_SEQPACKET socketpairs Mojo and crashpad create and then
inspect. struct sock now records the type the caller actually asked for
(lx_socket and lx_socketpair both set it) and SO_TYPE reports it. Correct on
its own merits and worth keeping -- any program that validates its socket
type was being lied to.

TESTED: no change. Chrome still exits 191 with the same crashpad message.
Kept anyway (it is a real ABI correction, not a speculative change), and
recorded here so nobody re-tests it.

STATE OF THE HUNT, unchanged by this slice: the browser's handshake sendmsg
SUCCEEDS (returns 40, message queued on the peer), and then the browser
NEVER READS the reply -- no recvmsg, no read, no recvfrom -- it installs a
signal handler and exit_group(191)s, while on Linux recvmsg follows sendmsg
immediately on the same fd. Everything downstream (the handler's EOF, the
"incorrect payload size 0") is a consequence of the browser already being
gone (slice 79's timestamps).

REMAINING CANDIDATES for why crashpad bails right after a successful send,
now that socket type and credentials are both correct:
 a. lx_sendmsg IGNORES its `flags` argument entirely ((void)flags on the
    first line). crashpad sends with MSG_NOSIGNAL; other call sites use
    MSG_DONTWAIT/MSG_MORE. Nothing currently distinguishes them, and a
    blocking-vs-nonblocking mismatch on THIS send is the most plausible
    remaining trigger.
 b. The msghdr OUT fields: we write msg_controllen/msg_flags on the recv
    path but never touch them on send. Linux leaves them alone too, so this
    is lower probability -- verify rather than assume.
 c. crashpad may check the socket's peer credentials via getsockopt
    SO_PEERCRED (level SOL_SOCKET, option 17) BEFORE reading; we return 0
    with no data for unknown options, which would hand it zeroed creds.
    SO_PEERCRED is the natural companion to the SO_PASSCRED work already
    landed, and is cheap to implement -- try it first.

## Slice 81: SO_PEERCRED implemented (another real gap) -- and ELIMINATED as
## a candidate by evidence: chrome never asks for it.

SO_PEERCRED fell through getsockopt's `default: v = 0` and returned FOUR
zero bytes with a reported length of 4, where Linux returns a 12-byte
struct ucred -- structurally wrong, not merely empty. Implemented properly:
struct sock latches its creator's {pid,uid,gid} at sock_alloc (Linux answers
SO_PEERCRED from the values captured at socketpair()/connect() time, not
from whoever holds the fd now -- which matters precisely because these
endpoints are inherited across fork and exec), sock_peer_of() resolves the
AF_UNIX peer, and the option returns the peer's latched credentials.

RESULT: the [peercred] probe logged NOTHING -- chrome never queries
SO_PEERCRED at all. So this is eliminated as a cause by direct evidence
rather than by "the symptom didn't change", which is the stronger form.
Kept as a correctness fix; do not re-test it.

CANDIDATE LIST after this slice (unchanged failure, browser still never
reads the reply):
 (a) SO_PEERCRED -- ELIMINATED, never queried.
 (b) **lx_sendmsg ignores its `flags` argument entirely ((void)flags on the
     first line).** crashpad sends with MSG_NOSIGNAL; other callers use
     MSG_DONTWAIT/MSG_MORE, and nothing distinguishes them today. This is
     now the top remaining candidate -- and it is testable without chrome:
     write a tiny Linux test binary that socketpairs, sendmsg's with
     MSG_NOSIGNAL and recvmsg's, and run it under BOTH tobyOS and the WSL
     control. That isolates the ABI difference from the browser entirely,
     which is what should have been done for this whole class of question.
 (c) msghdr OUT fields on the send path (low probability).

## Slice 82: the ABI itself is EXONERATED. A standalone test of crashpad's
## exact handshake shape PASSES identically on tobyOS and on real Linux.

Stopped burning 7-minute chrome boots and wrote the ~30-line reproduction
instead (programs/linux-scmsg, freestanding static ELF, raw syscalls, same
recipe as linux-futex): socketpair(AF_UNIX, SOCK_SEQPACKET) ->
sendmsg(40 bytes, MSG_NOSIGNAL) -> recvmsg -> verify the bytes. Distinct
exit codes name each step (3=PASS, 10 socketpair, 11 MSG_NOSIGNAL send,
12 recv, 13 wrong size, 14 corrupt, 15 flagless send), and it retries
without flags on failure so "MSG_NOSIGNAL broke it" is distinguishable from
"sends are broken".

Ran on the CONTROL FIRST, per the prime directive -- if it failed there the
test would be wrong, not tobyOS:
    WSL Ubuntu (real 6.6 kernel): [scmsg] PASS ... exit=3
    tobyOS:                        [scmsg] PASS ... exit=3
IDENTICAL. So candidate (b) is dead: lx_sendmsg ignoring its `flags`
argument does NOT break this path, and the AF_UNIX socketpair transport --
SEQPACKET creation, MSG_NOSIGNAL sends, message-boundary receives, payload
integrity -- is CORRECT.

WHAT THIS MEANS: every mechanical explanation for the browser's behaviour is
now eliminated by direct test -- clone semantics (fixed, slice 76),
credentials (implemented, 77), socket type (fixed, 80), peer credentials
(implemented + never queried, 81), and now the send/receive path itself.
The browser's refusal to read its reply is therefore NOT a broken syscall;
it is chrome deciding, on some OTHER input, that crashpad setup already
failed. Candidate class shifts accordingly: look at what crashpad checks
BEFORE the read -- the handler process's liveness/status (it waitpid()s or
checks the pid it forked), /proc entries for that pid, or PR_SET_PTRACER
itself -- rather than at the socket.

The test is wired into the build permanently (Makefile + kernel spawn
alongside FUTEXTEST, [SCMSGTEST] VERDICT in every CHROMIUM_BOOT boot), so
this transport can never silently regress again.

## Slice 83: wait4 EXONERATED too. The browser's full syscall tail is now on
## the record and pins the divergence to one instruction boundary.

Added an [xexit] hook: any NON-ZERO exit_group from a Linux process dumps
the syscall ring. The browser aborts at ~7s, long before the 60s deep dump
that used to be the only way to see the ring, so its final calls had never
been recorded. Two things came out of it.

1. crashpad's DoubleForkAndExec waits for its intermediate child and
   REQUIRES waitpid(child)==child && WIFEXITED && WEXITSTATUS==0. Logged
   exactly what it gets:
       [wait4] pid=3 wait(1) -> 1 code=0 wstatus=0x0 (WIFEXITED=1 WEXITSTATUS=0)
       [wait4] pid=4 wait(5) -> 5 code=0 wstatus=0x0 (WIFEXITED=1 WEXITSTATUS=0)
   Correct on both levels of the double fork. **wait4 eliminated.** (Zombie
   semantics were checked too: proc_reap runs ONLY from the wait path, so a
   fast-exiting child cannot be auto-reaped out from under waitpid.)

2. The browser's OWN tail, filtered to pid 3, versus the control's strace at
   the same point:
       Linux : sendmsg -> recvmsg -> prctl(PR_SET_PTRACER) -> sigaltstack
               -> 8x rt_sigaction(SIGABRT/BUS/FPE/ILL/QUIT/SEGV/SYS/TRAP,
                  SA_ONSTACK|SA_SIGINFO)
       tobyOS: sendmsg -> rt_sigprocmask -> 1x rt_sigaction -> getpid
               -> gettid -> exit_group(191)
   So tobyOS's chrome skips recvmsg AND PR_SET_PTRACER AND the alt-stack
   install, installs a SINGLE handler, and aborts. getpid+gettid immediately
   before the exit is the shape of raise()/tgkill (chrome's abort path
   computes both), and we already know an INT3 fires (sigfault vec=3 sig=5).

RUNNING ELIMINATION LIST (all by direct test, not argument): clone
semantics, SCM_CREDENTIALS/SO_PASSCRED, SO_TYPE, SO_PEERCRED (never
queried), the whole AF_UNIX send/recv transport (slice 82's standalone test
passes identically on both kernels), and now wait4/zombie semantics.

WHERE THAT LEAVES IT: chrome makes its decision WITHOUT issuing the read --
the ring is authoritative, there is no recvmsg, recvfrom or read from pid 3
after the sendmsg. So the failing check consumes something chrome ALREADY
HAS, not a syscall result we can watch. The single rt_sigaction before the
abort is the strongest remaining thread: on Linux that block installs eight
handlers with SA_ONSTACK and is preceded by sigaltstack; ours installs one
and never calls sigaltstack (implemented in slice 70 -- verify chrome
actually calls it and what it returns). Next probe: log rt_sigaction's
signal number + flags and sigaltstack's arguments for the browser, and
compare against the control's strace line-for-line.

## Slice 84: CORRECTION -- crashpad setup COMPLETES. The browser installs
## all eight handlers with SA_ONSTACK after a real sigaltstack. The INT3 is
## a LATER, separate CHECK.

Logged rt_sigaction's signal+flags and sigaltstack's usage instead of
inferring from the ring's tail:

    [sigalt] pid=3 query=yes set=no      <- reads the current alt stack
    [sigalt] pid=3 query=no  set=yes     <- installs one
    [sigact] pid=3 sig=6,7,8,4,3,11,31,5,24 flags=0xc000804
             handler=0xfd71cc0 SA_ONSTACK SA_SIGINFO   (one line each)

That is SIGABRT/BUS/FPE/ILL/QUIT/SEGV/SYS/TRAP(+XFSZ) with SA_ONSTACK|
SA_SIGINFO -- the SAME set, flags and single shared handler the control's
strace shows. So crashpad's signal setup SUCCEEDS on tobyOS.

**This corrects slices 79-83.** "One rt_sigaction then abort" was an
artifact of reading only the last 32 entries of a global ring shared by four
processes; the full picture shows the browser getting substantially further
than believed. The crashpad handshake is therefore NOT the blocker either --
it is at worst a warning path -- and the INT3/exit 191 is a DIFFERENT,
LATER CHECK.

Note the ordering difference that misled me: the control's strace has
sendmsg -> recvmsg -> prctl -> sigaltstack -> handlers as one block, while
tobyOS reaches the sigaltstack+handlers block anyway. The missing recvmsg is
real but evidently non-fatal.

WHERE THIS LEAVES THE ARC: everything mechanical is eliminated (clone,
credentials, socket type, peercred, the AF_UNIX transport via the standalone
test, wait4/zombies) AND crashpad initialisation now demonstrably completes.
The remaining task is simply to identify the CHECK -- and the one asset not
yet used is chrome's own voice: no CHECK text ever reaches stderr even at
--v=1, which is itself anomalous (a LOG(FATAL) prints file:line first).
NEXT: find out why chrome's fatal logging produces nothing. Candidates: its
log file destination (it may be writing to a file under the profile dir we
never read back), stderr buffering lost at exit_group, or logging being
initialised late. Reading that message is now worth more than any further
syscall archaeology -- it names the CHECK outright.

## Slice 85: *** THE ABORT IS __stack_chk_fail *** -- a STACK CANARY
## MISMATCH, not a chrome CHECK. Disassembly settles what 10 slices of
## syscall archaeology could not.

Used real tooling on the control instead of guessing: chrome is a PIE loaded
at 0x500000, tobyOS reports rip=0x624a118, so the file address is 0x5d4a118
and the trapping byte is at rip-1. objdump around it:

    5d4a0ef:  mov  %fs:0x28,%rax        <- read the canary from TLS
    5d4a0f8:  cmp  -0x30(%rbp),%rax     <- compare with the on-stack copy
    5d4a0fc:  jne  5d4a112
    5d4a112:  call __stack_chk_fail@plt
    5d4a117:  int3                      <- THE TRAP WE HAVE BEEN CHASING
    5d4a118:  ud2

So the INT3 is glibc's stack-protector, and exit 191 is its aftermath.
**Chrome is not rejecting anything. Its stack canary does not match.**
Every "what is crashpad checking" theory is therefore moot -- including the
missing recvmsg, which is a SYMPTOM: the function never returns normally.

The enclosing function is also identified by its calls:
    ... bind@plt with length 0x6e (110 == sizeof(struct sockaddr_un))
    ... listen@plt
    ... four calls to a common destructor, then the canary epilogue
i.e. it creates, binds and listens on a UNIX-domain socket, and dies on the
way out of that frame.

TWO MECHANISMS, both ours, both testable:
 1. **We smash the caller's stack.** Something in bind/listen/getsockname
    writes past the user's buffer. Note lx_sockaddr_out writes its
    sockaddr WITHOUT consulting the caller's *addrlen (the netlink arm
    copies sizeof(nl) unconditionally; the AF_INET arm likewise) -- if a
    caller passes a buffer smaller than what we write, that is a textbook
    canary kill. getsockname is called ~126,000 times per interval by
    chrome, so this path is heavily exercised.
 2. **The canary READ is wrong**: %fs:0x28 resolves through the FS base, so
    any discrepancy in TLS setup between where the canary was stored (
    function entry) and where it is compared (exit) produces a spurious
    mismatch. Relevant because this process just went through our
    clone3 CLONE_VM|CLONE_VFORK path (slice 76), and real vfork SUSPENDS
    the parent until the child execs -- we do not, so parent and child run
    concurrently over memory glibc believes is exclusively the child's.

NEXT (in order, both cheap):
 a. Clamp every sockaddr writer to the caller's addrlen and re-run. This is
    correct regardless of the outcome and directly tests mechanism 1.
 b. If it survives, log fs_base plus the stored/expected canary at the
    faulting frame and check CLONE_VFORK parent-suspension semantics.
This is the closest the arc has been to a fix: the failure is now a
two-line C bug class rather than an opaque browser decision.

## Slice 86: *** FULL CHROME'S exit 191 IS FIXED *** -- mkdir(2) threw away
## the caller's mode, so ProcessSingleton's CHECK(mode == 0700) failed.
## Browser lifetime 7s -> full session.

Slice 85's reading was WRONG and is retracted here. The trap is not
__stack_chk_fail: dumping both sides of the canary at fault time showed
fs:0x28 == 0 and [rbp-0x30] == 0, i.e. they MATCH and that branch is never
taken. The `call __stack_chk_fail` seen next to the trap was a neighbouring
code path; clang pads with int3, so an int3 near a noreturn call proves
nothing. The real identification: the trap is `int3` followed by `ud2`,
which is exactly Chromium's IMMEDIATE_CRASH() macro -- a deliberate
CHECK/NOTREACHED abort, not memory corruption.

Adding RETURN VALUES to the syscall ring settled the rest. The browser's
last syscalls before the abort were all SUCCESSES:

    socket=14  fcntl=0  fcntl=0  readlink=-2  readlink=-2  close=0
    uname=0  symlink=0  getrandom=8  mkdir=0  stat=0   <-- then IMMEDIATE_CRASH

No failing syscall precedes the crash, which rules out the entire "what did
we return wrong" family the previous ten slices chased. That sequence is
ProcessSingleton::Create, which immediately after making its temp directory
does:

    CHECK(base::GetPosixFilePermissions(dir, &mode) &&
          mode == base::FILE_PERMISSION_USER_MASK)   // exactly 0700

ROOT CAUSE: vfs_mkdir() hardcoded `00755u | VFS_MODE_VALID` for every
directory ever created, and sys_mkdir explicitly discarded its mode
argument ("M25A: not honoured yet"). chrome asks for 0700, reads back 0755,
and CHECK-fails. chrome-headless-shell has no ProcessSingleton, which is
exactly why the headless flavour never hit this in ~40 slices.

FIXES
 * vfs_mkdir_mode(path, mode) carries the caller's mode; vfs_mkdir() keeps
   the 0755 default for the 8 in-kernel callers.
 * sys_mkdir passes the requested mode through.
 * chmod/fchmodat now really store the mode via vfs_chmod(), which existed
   and worked all along -- the syscall had been a deliberate no-op on the
   belief that "the VFS does not model permission bits". It does. This also
   removes a latent NSS failure (it chmods its key DB to 0600 and refuses
   the DB if that does not stick).

ALSO IN THIS SLICE (real ABI gaps found while hunting, all correct
independently of the above):
 * Named AF_UNIX sockets: bind/listen/accept were rejected outright (bind
   fell through to the sockaddr_in path -> EINVAL; listen -> EOPNOTSUPP).
   Registry lives in socket.c so struct sock does not change size. A bound
   listener is no longer reported POLLHUP by the "no peer == EOF" rule.
 * Every sockaddr write-back (getsockname/getpeername/accept/recvfrom x2)
   ignored the caller's *addrlen and wrote a fixed 16 bytes. Now clamped
   with Linux truncation semantics via lx_addr_writeback().
 * getsockname on an AF_UNIX socket claimed AF_INET; it now answers with a
   real sockaddr_un.
 * readlink took strlen() of an uninitialised kernel buffer (the per-mount
   readlink op need not terminate) -- a kernel-memory disclosure that could
   also return a length never written. Buffer zeroed, scan bounded.

RESULT: browser pid 3 no longer dies at ~7s; it runs the whole session and
gets as far as GPUPersistentCache / component_crx_cache / NativeMessagingHosts.
NEXT WALL (new, much later): a CHILD process, pid=10 'chrome+T', takes a
vec=13 GP fault and exits 191 at ~74s after 39.5s of real CPU. Registers
carry ASCII-looking bytes (r10=0x74736f01, r15=0x0605010702020301), which by
the standing rule means memory corruption rather than a bad return value.

## Slice 87: identified the ~74s fault as PartitionAlloc ThreadCache;
## shootdown IRQ-off deadlock FIXED; AT_RANDOM seeded; CoW-window still OPEN

### Identity of the ~74s crash (was mislabeled "child process")
pid=10 is a THREAD of the browser (`clone` tid=10 in tgid=3), not a forked
child. `--in-process-gpu` / `--no-zygote`. pid=29 exit 127 is benign:
`execve(xdg-settings ...)` missing helper.

Disassembly of rip file-addr `0x31c3fc4`: `#GP` on `mov (%r15),%rdx` with
non-canonical `r15=0x0605010702020301`. The surrounding code is Chromium
**PartitionAlloc ThreadCache** freelist lookup (`pthread_getspecific` +
`imul $0x5e0`, freelist `bswap` / `second == ~first` check, 2 MiB super-page
math). Same class as the YouTube ASCII-in-pointer crashes.

### Shootdown IRQ-off deadlock — CONFIRMED and fixed
Baseline run had `[tlb] ERROR: shootdown STILL un-acked after 16 rounds` in
a cluster at 8–12 s (crashpad forks). Two CPUs taking CoW faults with IRQs
masked deadlock on each other's shootdown IPIs, both time out, both proceed
with stale WRITABLE TLBs.

Fix (`apic.c`): while waiting for peer acks, periodically **self-flush CR3
and bump our own tlb gen** (exactly what the ISR would do). Peers waiting
on us observe progress; no `sti`, no BKL drop.
- Attempt that **sti'd** across the wait → kstack_top=0 SMEP panic on
  thread-group teardown. Retracted.
- Attempt that **dropped BKL** across the wait → chrome INT3/CHECK at ~12 s
  (VMA races). Retracted.
- Self-ack: `[tlb] WARN/ERROR` count **8+/run → 0** on validation runs.

### AT_RANDOM was all zeros — fixed
`AT_RANDOM` pointed at the 16-byte stack pad that `build_user_stack` zeroes
and never filled. Every process ran with `fs:0x28 == 0`. Now `rng_fill`s
those 16 bytes on exec (proc.c + fork.c). Confirmed live:
`fs:0x28=0x37b81f2d35988700` matching the stack slot when intact.

Also: brk shrink was free-THEN-shootdown; now batch + shootdown-before-free
(leak on un-acked). Quarantine ring got its own spinlock.

### Residual wall (OPEN) — CoW-after-fork stale-writable window
With shootdown timeouts gone, threads still #GP in PA with poison/ASCII,
and the page journal on the faulting pointer source shows:

    map4k → cow-fork-wp → cow-copy

i.e. the page was write-protected for fork, then CoW-copied, and the
pointer slot is already garbage. `vmm_cow_fork` write-protects the entire
user half, then shoots down once at the end — sibling threads keep running
with cached WRITABLE translations for that whole walk. Periodic
shootdowns mid-walk were tried and **made it worse** (mass simultaneous
#GP, browser exit 191 at 13 s); retracted. Next lever: stop-the-world the
thread group around the WP walk, or make the walk+shootdown atomic per
subtree under a hold that actually keeps siblings out of user mode.

Tier 2.5 (Ozone X11 + MIT-SHM) infra landed; **not DONE** — see handoff §4.

### Tier 2.5 close-out note (2026-07-31)
- STW `tg_vm_quiesce`, teardown `on_cpu` wait, kstack guard, fake X + SysV
  MIT-SHM + `xframe_poll`, PI futex `uaddr2` / UNLOCK handoff: in tree.
- Full-chrome **stability gate PASSED** on headless ozone + screencast
  (example.com, ~930 frames / session, tlb ERROR 0, no exit 191).
- Headed X11 still: Displays updated → only 10×10 CreateWindows → UI futex
  park → createTarget silence. SHM paint not default. Measure vs Slice 68
  recorded on JPEG path only (~930 vs ~1050 baseline; no 2.3× claim).

### Follow-ups observed late in the slice
- Richer auxv (AT_HWCAP/CLKTCK/UID/SECURE) appears to clear the flaky
  `GLib: getauxval () failed` path (0 hits on the clean rebuild run).
- A clean full-`.o` rebuild then showed browser `exit 191` at ~9 s via
  `#UD` (vec=6, likely `ud2` after CHECK) and a **sched panic**: switching
  to tid with `kstack_top=0` during thread-group teardown. That is the
  reaped-slot-still-runnable class resurfacing on the crash path — fix
  alongside the CoW window. tlb ERROR counts also returned on that run
  (post-panic / dying tree); treat as contaminated, re-measure after the
  kstack guard.

## Slice 88: headed Ozone X11 -- the recvmsg O_NONBLOCK bug (UI thread parked
## forever) + three scheduler-fairness fixes. Browser window + full WM
## property suite + navigation now happen; MapWindow still not reliable.

Method: WSL control rig upgraded with Xvfb + xtrace over TCP displays
(/tmp/.X11-unix is read-only under WSLg). control_x11trace.sh captures the
full X wire protocol of the SAME chrome + flag set on a real server --
the ground-truth request sequence for xserver.c. Two control experiments
eliminated whole theory families in minutes: --denyextensions (chrome maps
its window with ZERO extensions => our sparse extension surface is fine)
and dbus-dead env (chrome maps without any bus => dbus exonerated).

THE HEADED WALL (found via [uxstuck] + [xdbg] probes):
  lx_recvmsg's AF_UNIX branch honoured only MSG_DONTWAIT and IGNORED
  s->nonblock -- unlike lx_recv and the UDP branch right above it.
  Chromium's X socket is O_NONBLOCK and its UI pump reads until EAGAIN
  with flags=0: the first read on an empty ring became a FOREVER blocking
  wait ([uxstuck] pid=3 sock=4 xsrv=1 count=0 to=0; "READY in recvmsg" for
  173s). Mojo never tripped it because its channel reads pass MSG_DONTWAIT
  explicitly. Two-line fix.

FAIRNESS FIXES the unblock then exposed (all 1-vCPU):
  1. Futex wake handoff: FUTEX_WAKE enqueued the waiter and let the waker
     keep the CPU; the waker re-acquired the mutex within microseconds and
     the woken thread lost EVERY retry -- chrome's UI thread lost one mutex
     to a busy worker for 3+ minutes (130 wake/wait ping-pongs). Now a
     val==1 wake whose waiter already waited >5ms yields to it while the
     lock is still free (20ms first -- too high, the ping-pong re-waits
     every ~15ms and never qualified).
  2. sched_yield's early-return path (RUNNING + empty queue) skipped the
     futex-timeout sweep + poll_tick entirely; once the handoff calmed the
     system into that path, [tick] fxsweep froze for 2+ minutes and six
     workers sat 110s past their 60s futex deadlines. The early path now
     drives both at the same 10ms cadence (taking the BKL briefly for
     poll_tick when the yielder does not hold it).
  3. SCHED_QUANTUM_BASE 5 -> 1 under CHROMIUM_BOOT (50ms -> 10ms NORMAL
     slice): 50ms legal monopolies made every contended interaction a
     lottery. (polltick=<n> in [tick] counts WAKES PERFORMED, not runs --
     do not re-diagnose a frozen counter as a dead driver without checking
     which it counts. That mistake cost one build cycle here.)

RESULT: browser frame window created (799x599), FULL WM property suite set
(WM_PROTOCOLS/WM_CLASS/_NET_WM_PID/64KB _NET_WM_ICON burst), WM_NORMAL_HINTS
readback, AddKeepAlive(kBrowserWindow), navigation started ("URL to scan",
omnibox lines) -- none of which EVER happened before this slice. STILL OPEN:
MapWindow (op=8) not yet observed; the stall now WANDERS run to run
(dri3 warning / WM detection / post-props) -- the remaining shape is 1-vCPU
scheduling timing, not a single missing feature.

-smp 4 TRIED AND PARKED: froze at 7.6s with a dozen threads READY in
clone/clone3 for 232s and ZERO BKL acquisitions on cpu1-3 -- the slice-87
quiesce/deferred-requeue vs AP queues family. Separate arc; run_watch.py
documents it and stays at -smp 1.

New instruments that should outlive this slice: [uxstuck] (blocking UNIX
recv >5s names its socket + x_server flag), [xdbg] (x_conn gates dump),
[xpoll] (poll timeout with a readable-but-unreported X fd), poke MUTED
(wake flood guard visibility), control_x11trace/x11deny/x11nodbus/x11vmod.

## Slice 89: the wall is NOT scheduling and NOT X coverage -- it is AF_UNIX
## SLOT RECYCLING (the hazard slice 78 named and nobody fixed). -smp 4 did
## NOT reproduce the slice-88 freeze. MapWindow has now been OBSERVED.

RETRACTION FIRST: slice 88's `-smp 4 PARKED -- APs did ZERO syscall work`
is WRONG, or at least not reproducible. Four -smp 4 runs this slice: APs
took real work every time (`[bkl] cpu1 acq=1.2M cpu2 acq=407k cpu3 acq=313k`),
no clone3 pile-up, no freeze at 7.6s. Whatever the single 88-era trial saw,
"the AP/quiesce arc" is NOT the thing standing between us and tier 2.5. Do
not spend another session on it.

THINGS THAT WERE ACTUALLY BROKEN AND ARE NOW FIXED:

1. **`--vmodule` was silently disabled.** chromewin passed TWO `--vmodule=`
   flags; chrome keeps only the LAST, so the X11/ozone/views narration added
   for exactly this investigation never emitted a single line. Merged into
   one flag. (Check for duplicate flags before trusting missing output.)

2. **`[xsum]`, the diagnostic written to explain the stall, printed NOTHING
   for a whole 360s run** -- it hung off `xserver_tick`, which is called from
   pid 0's idle_loop, and under chrome load pid 0 NEVER RUNS. Any diagnostic
   on the idle path is invisible in precisely the situation it exists for.
   Moved onto the sched_tick heartbeat. The 100ms "wake (poll-idle)" lines
   that kept appearing come from file_poll_ready's separate xserver_on_poll
   caller -- which is what made the gap easy to miss.

3. **The wait-graph is BLIND to blocking recvmsg** (only epoll_wait/futex
   call waitt_enter). The browser UI thread showed up in NEITHER the blocked
   list NOR the running list -- it was parked in recvmsg on the X socket the
   whole time. [uxstuck] cannot catch it either, because our own 100ms
   idle-poke means no single wait ever reaches its 5s threshold: slice 88's
   fix blinded slice 88's instrument. `[xsum] lastreq=<ms>` is the
   replacement -- it answers "is chrome still talking to us at all".

4. **Two genuine SMP lost-wakeups** (release/acquire where StoreLoad
   ordering was needed -- on x86 BOTH sides can read stale and NEITHER acts):
   `sched_finish_switch` vs `tg_vm_resume` (thread parked BLOCKED forever,
   the shape slice 88 blamed on AP queues), and `sys_fork_share` vs
   `vfork_child_done` (launcher parked forever). SEQ_CST fences both sides.

5. **Group teardown ran `close_all_fds` with the BKL DROPPED** while
   file_close refcounts (vfs_refs/sock refs/pipe counts) are plain ints
   guarded ONLY by the BKL. Also `proc_wait_off_cpu` could hang the whole
   teardown forever on a quiesce-parked spinner. Both fixed (bounded wait +
   leak the slot rather than free a live thread's stack).

THE CURRENT WALL (repeatable signature; the EPIPE is a CONSEQUENCE):

    [31] 46(sendmsg) a1=7 = -32   <-- EPIPE
    ... rt_sigaction(11), getpid, gettid, exit_group(191)
    stderr: "Crashing due to FD ownership violation:"

Same divergence slice 83 recorded. What is NEW is why the peer is gone:

    [6952 ms] [execve] pid=1 now running 'chrome_crashpad_handler'
    [6954 ms] [proc]   pid=1 ... exit code=0 cpu=0 ms syscalls=4

**chrome_crashpad_handler execs and exits 2 ms later after FOUR syscalls.**
Slice 76 made the exec happen and nobody checked the handler SURVIVES. It
does not; its endpoint dies with it, so every later client sendmsg EPIPEs
and that client aborts 191. NEXT: dump those four syscalls. [xexit] will
not help -- it only fires on NON-ZERO exits and this one exits 0.

HYPOTHESIS TESTED AND **NOT CONFIRMED** -- AF_UNIX slot recycling.
Slice 78 named it ("sock_alloc recycles pool indices IMMEDIATELY ... NO
GENERATION COUNTER") and it fit the symptom exactly: a stale `peer_ip`
aliases whatever socket now owns the slot, and `sock_unix_peer_close`'s
`peer->peer_ip = 0` would then sever a LIVE stranger's channel. A full
generation counter was implemented to test it -- `struct sock.gen`/
`peer_gen`, all resolution via `sock_peer_checked()`, `struct file.sock_gen`
for descriptors, `[uxgen]` logging every stale link caught.
**`[uxgen]` fired ZERO times and the signature is byte-identical.** The
aliasing is real in principle but is NOT what breaks this run. The code is
KEPT (genuine hazard, no cost) but must NOT be credited with any behaviour
change, and must not be re-chased as the cause. Recording this the way the
arc's method demands: a fix that changes nothing is a disproof, not a win.

X EXTENSIONS -- the mixed state was wrong: we advertised MIT-SHM + RANDR +
XFIXES + BIG-REQUESTS. Slice 88's control proved chrome maps with ZERO
extensions; the ledger's "do not ADD extensions on a hunch" said nothing
about the half-way configuration we were actually in, where chrome takes
the extension path for what we advertise and then waits on companions our
hand-rolled replies never satisfy. Now MIT-SHM ONLY (tier 2.5 needs it),
`-DXSRV_EXT_ALL` restores the old set for A/B.

MapWindow (op=8) HAS now been observed on our server (`req c=4 op=8 seq=253`,
window resized to 799x599) -- the first time in this arc.

## Slice 91 CORRECTION: the -smp 4 freeze IS REAL. Slice 89's exoneration of
## Path A was WRONG, and this is what blocks tier 2.5.

**RETRACT slice 89's "Path A is dead: -smp 4 does NOT reproduce."** It
reproduces. Slice 89 ran four -smp 4 sessions, saw APs doing real work in
each, and concluded the arc was a dead end. That was sampling error: the
freeze is INTERMITTENT, and none of those four runs hit it inside the window
I inspected. The signature is unmistakable when it does:

    [cur] pid=19 READY in clone3 for 293492 ms
    [cur] pid=20 READY in clone3 for 293489 ms
    ... six threads, all in clone/clone3, all ~293 s
    [bkl] cpu0 acq=0 waits=0 wait=0Mcyc held=0Mcyc
    [bkl] cpu1 acq=0   [bkl] cpu2 acq=0   [bkl] cpu3 acq=0

**BKL acquisitions are ZERO on all four CPUs.** That is not chrome stalling;
that is the WHOLE KERNEL wedged -- no CPU is entering a syscall body at all.
This is slice 88's original report, verbatim, and the original handoff's
Path A recommendation was correct.

WHY IT LOOKED LIKE SOMETHING ELSE: the wedge presents downstream as the
"wandering stall" -- chrome stops at a different X request every run
(dri3 warning / WM detection / post-property-burst / after MapWindow),
because whichever thread happens to be mid-clone3 when the system freezes
determines which subsystem never finishes. Chasing the X protocol was
chasing shadows. Two supporting facts from this run, both of which say the
X server is INNOCENT:
  * `[xsrv] unhandled opcode` never fired -- every request chrome sent, we
    answered. It is not waiting on a reply we failed to send.
  * `[xsum] c=5 seq=195 lastreq=64810ms rlen=0 pend=0 rx=0 armed=1` -- our
    ring is EMPTY and chrome consumed every byte. It is not waiting on us.

WHERE TO LOOK (the slice-87 quiesce machinery, as originally flagged):
`cow_fork_lock_acquire` parks a forker that observes `vm_quiesce` by setting
`state=PROC_BLOCKED` and spinning `while (vm_quiesce) pause;`. If the STW
actor never reaches `tg_vm_resume` -- or reaches it while the sibling is
still `on_cpu`, deferring the requeue to `sched_finish_switch` -- nothing
ever puts that thread back on a run queue. Slice 89 added SEQ_CST fences to
BOTH sides of that handoff (the release/acquire pair could let each side
read the other's stale value on x86); those fences are correct and should
stay, but they are clearly NOT SUFFICIENT.

NEXT (concrete): the codebase already defends itself with timeouts in this
exact area -- `cow_fork_lock_acquire` steals the lock after 20M spins,
`tg_vm_quiesce` gives up waiting after 500k. **The deferred requeue has no
such backstop.** Add one: a periodic sweep (the sched_tick heartbeat is
already the right home -- it is the one thing still running when everything
else is wedged) that finds any proc with `vm_quiesced && !vm_quiesce &&
state==PROC_BLOCKED && !on_cpu` and requeues it, logging loudly. If that
alone unfreezes the guest, the deferred-requeue path is confirmed as the
bug and the real fix follows. Do NOT ship the sweep as the fix -- ship it as
the instrument that proves the mechanism.

### Slice 91 addendum: the no-WM change (07f297a) is UNVERIFIED — do not
### trust it until it is A/B'd against a run that does not freeze.

`_NET_SUPPORTING_WM_CHECK` now answers None (we are not a WM). The
reasoning is solid and control-backed: the control is Xvfb with NO window
manager, and there chrome SELF-MANAGES (ConfigureWindow(Above) ->
SetInputFocus -> QueryPointer -> paint) instead of delegating. Declaring a
WM demonstrably put chrome in an endless ChangeProperty / GetInputFocus /
SendEvent retry loop even after we started answering _NET_ACTIVE_WINDOW
properly.

BUT THE RUN THAT TESTED IT PROVES NOTHING. It stalled at seq=193 -- the
_NET_SUPPORTING_WM_CHECK GetProperty itself -- with **zero `[bkl]` lines in
the whole log**, i.e. it never reached the 60 s deep dump: the guest froze
at ~7.6 s. That is the intermittent `-smp 4` kernel freeze, not a verdict on
the X change. `[qstuck]` did NOT fire in that run either, so the backstop
has still never been exercised.

**This is the trap this arc keeps falling into: reading a frozen run as a
result.** Before judging any X-server change from here on, first confirm the
run got past the freeze -- `grep '\[bkl\] cpu' logs/run_watch.log` must be
NON-EMPTY (the 60 s deep dump ran). A log without it is a frozen guest and
says nothing about chrome.

ORDER OF WORK, revised: the freeze is upstream of everything. Fix it (or at
least make `[qstuck]` fire and prove the mechanism) BEFORE spending another
cycle on X protocol behaviour. Every X hypothesis tested against a frozen
run is wasted, and several already were.

## Slice 91 FINDING (the important one): TIER 2.5's PREMISE DOES NOT HOLD.
## Chromium never presents pixels through X — not on our server, and NOT ON
## A REAL ONE. Stop hardening xserver.c for frames.

Tier 2.5 is defined as: "chrome MapWindows on our fake X server, paints via
**ShmPutImage** into the shared segment, chromewin presents those pixels."
The whole zero-copy design, and the ~2.3x estimate from slice 68, rest on
chrome issuing ShmPutImage (or at minimum PutImage) on its X connection.

**It does not. Measured on the CONTROL — a full, real X server.**

Three configurations, Xvfb + xtrace, real page (https://example.com),
90–120 s each, chrome 151, the same binary the guest runs:

| flags | PutImage | ShmPutImage | MIT-SHM traffic |
|---|---|---|---|
| `--use-gl=disabled --disable-gpu --single-process` (our guest set) | 0 | 0 | QueryVersion x3, Attach x1, Detach x1 |
| `--disable-gpu-compositing --enable-unsafe-swiftshader --single-process` | 0 | 0 | QueryVersion x3, Attach x2, Detach x2 |
| same, **multi-process** (no `--single-process`) | 0 | 0 | QueryVersion x3, Attach x2, Detach x2 |

The third run had **66 extensions present=true** — MIT-SHM, DAMAGE, RENDER,
GLX, Composite, SYNC, XFIXES, RANDR, SHAPE, XKEYBOARD, XTEST — i.e. every
capability chrome could ask for, on a real server, with the page loaded
(4 navigation hits in stderr). Chrome attaches an SHM segment, DETACHES it,
issues GLX config queries (`glXGetFBConfigs`, `glXQueryServerString`,
`glXCreatePbuffer` / `glXDestroyPbuffer`) and renders **offscreen**. It
never pushes the result to the X window.

**CONSEQUENCES, stated plainly:**

1. **No amount of work on `src/xserver.c` can produce a tier-2.5 frame.**
   We spent slices 88–91 making our fake server more correct — MapWindow,
   VisibilityNotify/ConfigureNotify/FocusIn, EWMH SendEvent, the reply-vs-
   silence fix. All of it was real and worth keeping (chrome now reaches
   **bootstrap OK**, a live CDP session, which never happened before). None
   of it was ever going to yield a frame, because chrome does not send one.

2. **The slice-68 "~2.3x" figure was never validated against a working SHM
   path** — it was an estimate of what zero-copy *would* save, not a
   measurement of it working. Treat it as void.

3. **The CDP screencast is not a stopgap we are trying to escape; on this
   deployment it may be the only pixel source chrome offers.** The existing
   path (933 frames/example.com, ~1050/react.dev) is the product, not the
   fallback.

**WHAT TIER 2.5 SHOULD BECOME.** Do not reopen it as "make ShmPutImage
work". Either:
 * **Retire it**, and re-aim the perf work at frame PRODUCTION inside
   chrome, which is what tier 1 measured as the actual bottleneck (our
   display path is ~1 ms/frame, essentially free); or
 * **Re-scope it** to a mechanism chrome actually uses. That means finding
   out where the offscreen render goes and whether it can be intercepted —
   the GLX pbuffer path, or `viz`'s shared-memory buffers over Mojo (which
   we already carry), NOT the X core protocol.
Either way the next step is a measurement, not more X protocol.

**METHOD NOTE, and it is on me.** I used `logs/control/x11wire.txt` as
"ground truth for reaching MapWindow + ShmPutImage" for several slices. That
trace **never reached the paint stage either** — it ends at MapWindow and
teardown. I was treating a trace that stops before the goal as evidence
about the goal. When a control is your spec, check that the control actually
DOES the thing you are trying to reproduce before you trust it for that.

---

## SLICE 92 — tier A: the BKL-holding quiesce park is REAL and REMOVED; the "actor bkl_enter deadlock" theory is RETRACTED by its own A/B

**Context.** Post-slice-91 handoff, tier A: fix the intermittent `-smp 4`
freeze (six threads READY in clone3 ~293 s, `[bkl] acq=0` on all four CPUs,
`[qstuck]` silent, worst shape total serial silence).

**HYPOTHESIS (initially confirmed-looking, then half-refuted).** Audit found
the one park point in the CoW-fork STW machinery that waits while HOLDING
the BKL: a kernel-mode #PF on a WP'd user page — which `copy_to_user`
triggers **synchronously** via `uaccess_prepare_write -> page_fault_handler`
mid-syscall, BKL held — spun on `vm_quiesce` without dropping the lock
(`page_fault.c` + the twin copy in `mmap.c mmap_handle_page_fault`). Theory:
forker re-takes the BKL for `mmap_cow_clone` before `tg_vm_resume` -> hard
cycle on the FIFO ticket lock -> whole-kernel wedge matching every observed
fact.

**A/B RESULT — the theory's actor edge DOES NOT EXIST.** A new permanent
guard (`programs/linux-qfreeze`, QFREEZETEST in the console flavour: 3
hammer threads driving `clock_gettime` out-params into a 256 MB touched
region while main forks 12x) PASSES on BOTH kernels (3x fixed, 2x baseline,
WHPX `-smp 4`). Re-tracing showed why: `tg_vm_quiesce` returns with the BKL
already dropped, so `sys_fork`'s `held` flag is FALSE and the forker NEVER
re-takes the BKL between quiesce and resume — `mmap_cow_clone` runs
BKL-free. The old BKL-holding park therefore produces **sweep-length
whole-kernel BKL stalls** (every syscall queues behind the ticket lock until
`tg_vm_resume`), not a permanent deadlock. **The 293 s freeze mechanism
remains UNPROVEN.**

**KEPT (all real, all committed):**
 * `vm_quiesce_park_oncpu()` (fork.c): the shared park now drops/re-takes
   the BKL around the wait, clears the stale `vm_quiesced` on self-resume
   (it previously leaked =1 with no `sched_finish_switch` ever owed, arming
   `[qstuck]`/finish_switch to spuriously requeue a later unrelated BLOCKED
   state), reports `[qpark]` engagements (+`bkl=` count) and warns loudly on
   a >30 s park. Kernel rule enforced: no unbounded wait holds the BKL.
 * QFREEZETEST guard: runs with AP-run enabled (new
   `sched_disable_ap_run()` restores the boot invariant after) because the
   boot harness is otherwise BSP-only — **a first cut "passed" with the
   hammers at 0 iterations; the guard now hard-fails (exit 2 VACUOUS) unless
   every hammer proves live before the forks and advances during them.**
   Corollary worth knowing: every console-flavour boot guard (FUTEXTEST,
   DEMRACETEST...) has been running effectively single-CPU — DEMRACETEST's
   parallel verdicts were weaker than believed.
 * `run_freeze.py` upgrades: detects the WEDGED-ALIVE shape (heartbeats
   flowing, all-CPU `acq=0` in a deep dump) that the growth-only detector
   sailed past; archives every run's serial log (`freeze_run<N>.log`);
   `RUNS=` env knob.
 * `[fork] bkl re-take` timing instrument at the (normally skipped)
   `mmap_cow_clone` bkl_enter — if it ever prints, some park is holding the
   BKL again.

**METHOD NOTES.**
 * The A/B that was built to CONFIRM the mechanism REFUTED half of it
   instead — that is the test doing its job; the fix stands on the rule
   violation + measured stall shape, not on the retracted deadlock story.
 * A guard that passes with its hammers at 0 iterations is the frame
   counter that counts non-frames, again. Gate liveness INSIDE the test.

## SLICE 92b — THE FREEZE IS CAUGHT, SYMBOLIZED, AND ROOT-CAUSED: a CYCLIC futex waiter list walked forever under g_futex_lock with IRQs off

**The specimen.** Freeze-hunt run 5 (headed full chrome, WHPX `-smp 4`):
total serial silence at guest ~10.7 s, `run_freeze.py` captured
`info registers -a` twice, 3 s apart — the first successful capture since
the tool was built. The last serial line before silence:
`[fork] parent pid=13 clone-returning 25` — microseconds after a CoW fork
completed (chrome fork #2).

| CPU | RIP (both captures) | symbol | meaning |
|---|---|---|---|
| 0 | 0x...7501b → 0x...75021 | `futex_fast` | `spin_lock_irqsave(&g_futex_lock)` xchg loop, IRQs off |
| 1 | 0x...75021 (same) | `futex_fast` | same spin |
| 2 | 0x...dfb52, HLT=1, IF=1 | `sock_unix_recv_fds` | healthy cooperative hlt-wait |
| 3 | 0x...76353 (same both) | `futex_expire_timeouts` | **INSIDE the locked waiter-list walk** — `mov (%r14),%rdi; ... add $0x4d0,%rdi; jmp` |

CPU3 is the lock HOLDER, looping a linked-list traversal that never ends:
**the waiter list is cyclic.** IRQs off on the holder + both spinners =
no tick, no heartbeat, no serial, `[qstuck]` structurally blind. Every
signature fact of slices 88–91 falls out of this one state.

**How the cycle forms.** A parked futex waiter is made READY by machinery
that does NOT unlink it from the waiter list (only the futex code unlinks).
The waiter returns "woken", glibc re-checks its predicate, re-parks — and
the head-insert while its old link is still live closes a loop:
`pred -> T -> old_head -> ... -> pred`. The next `futex_expire_timeouts`
or wake walk then never terminates, holding the lock. Spurious-READY
sources found:
 1. **Stale `vm_quiesced` + `sched_finish_switch`'s deferred requeue** —
    apic.c's TLB-ack wait demotes a proc to BLOCKED+vm_quiesced=1 mid-wait
    (to satisfy a concurrent fork quiesce) and then KEEPS RUNNING; nothing
    consumed the flag. The proc's next futex park + yield hits the
    deferred requeue (BLOCKED + stale flag) and is spuriously requeued
    while still linked. This is why the freeze lands right after
    `cow_fork done` and needs `-smp 4`.
 2. **Any signal-path wake** (`signal.c` wakes BLOCKED procs without
    futex-unlink; crashpad's `rt_tgsigqueueinfo` to a futex-parked thread
    is a live example since slice 90).
 (The pre-92 pf-park stale flag was a third source, already removed.)

**FIXES (slice 92b):**
 * `futex_unlink_self()` after EVERY futex park's `sched_yield` returns
   (WAIT, LOCK_PI, WAIT_REQUEUE_PI): if still linked, the wake was
   spurious — unlink self under `g_futex_lock`, count it (`[fxspur]`,
   totals in the deep dump as `fxspur=`). Turns ANY spurious waker, present
   or future, into a POSIX-legal spurious wakeup instead of list
   corruption. This is the structural fix.
 * `futex_expire_timeouts`: the walk is now BOUNDED (PROC_MAX hops) and
   TRUNCATES a cyclic list with a loud `[fxcycle]` line — a residual
   cycle becomes a log line, not a dead kernel.
 * apic.c ack-wait: the BLOCKED+vm_quiesced demotion is UNDONE when the
   wait exits (it was a lie told to the quiesce wait by a proc that keeps
   executing) — removes stale-flag source #1 at its origin.

**VERIFICATION SIGNAL for future runs:** `[fxspur]` firing with NO freeze
is the mechanism confirming itself; `[fxcycle]` firing means a cycle
former still exists and must be hunted.

---

## SLICE 93 — TIER B DONE: the interframe time is MEASURED and decomposed; first tier-C win shipped (everyNthFrame 3->1 = 13->27 fps)

**Instrument** (`programs/chromewin`, all committed): per-30-frame `[cwif]`
line — arrival gap avg/max, chrome's own capture cadence (delta of
consecutive `screencastFrame metadata.timestamp`s), ack->arrival
turnaround, `captureScreenshot` round-trip, pushed/polled split — plus a
5 s `[cwping]` CDP `Runtime.evaluate` round-trip that doubles as a
JS-liveness probe (reports `location.href` + the anim counters), and a
deterministic damage generator (`/etc/anim.html`, 16 ms setInterval
full-viewport canvas repaint + an on-screen rAF counter; navigate to it
with `-DCW_URL=\"file:///etc/anim.html\"`).

**MEASURED, SMP=4, 360 s runs, all gate-clean:**

| regime | delivered | gap | chrome capture cadence | ack->arrival | ours |
|---|---|---|---|---|---|
| example.com (static) | 3.3 fps | ~310 ms | ~300 ms | ~296 ms | ~1 ms |
| anim page, nth=3 | 13 fps | 55 ms | 54 ms | 51 ms | ~1 ms |
| anim page, **nth=1** | **27 fps** | **24 ms** | **23 ms** | **21 ms** | ~1 ms |

Encode+transport is the small residual (gap minus capture cadence,
~2-10 ms); CDP pings round-trip in 32-86 ms under load. The renderer
composits at ~52 Hz (on-screen counter: `tick 52.5/s raf=13713`).

**FINDINGS, in order of consequence:**
 1. **The interframe time was never encode, transport, or our display
    path.** It was capture POLICY: `everyNthFrame:3` skipped 2 of every 3
    compositor commits (54 ms = 3 x ~18 ms commits), and on a static page
    the ~300 ms is viz's refresh duty-cycle re-serving an unchanged
    surface (irrelevant: nothing new to show).
 2. **nth=1 is a measured 2x and is now chromewin's default** (`CW_NTH`
    knob kept). Frame baselines recorded before slice 93 used nth=3 —
    do not compare across without noting this.
 3. The next ceiling is the single-in-flight ack loop at ~21 ms
    (capture+encode+2 pipe hops). Candidates, in order: ack-before-decode
    in chromewin (overlaps our ~2-5 ms with chrome's next capture);
    re-test the quality knob AT nth=1 (its slice-68 rejection predates
    tier 2 and this regime); kernel wake-latency work (the 32-86 ms ping
    RTT under load says IPC hops still cost real time on tobyOS).
 4. At SMP=1 chrome-side latency explodes (screenshot RTT ~3.5 s, pings
    up to 1.6 s) — all measurement stays at SMP=4.

**RETRACTION (mine, same-slice):** after the first blank-page run I
attributed the missing animation to "headless issues no BeginFrames so
rAF never ticks". WRONG — the page had simply failed to load
(`chrome-error://chromewebdata/`, the URL's file was missing from the
initrd tar list). With the file actually present, rAF ticks at ~50/s,
on-screen. The on-page counters (put there as the discriminator) are what
caught it. Verify the page LOADED before theorizing about the renderer.

**Gotchas that burned this slice** (both now on the record):
 * `initrd/etc` staging has BOTH a cp block AND an explicit tar file
   list; a file only in the former builds fine and silently does not
   exist in the guest (the linux-* /bin memory note, etc/ edition).
 * Exporting `TMP='C:\t'` (the clang integrated-as workaround) into the
   shell that later launches QEMU breaks `-snapshot` temp-file creation
   (`C:\/vl.XXX: Permission denied`) — QEMU exits before boot and the run
   silently produces no serial log. Scope the TMP override to the build
   (build_vid.sh already does) or prefix runs with a valid TMP.

**Post-change stock baseline (example.com, nth=1 default, SMP=4, 360 s,
gate-clean):** ~2820 frames (~7.8 fps at a ~100 ms cadence) vs 930 at
nth=3 — the static-refresh duty cycle follows consumer demand too, so the
arc's headline metric TRIPLED on the same page. Post-slice-93 baselines:
**example.com ~2820 / anim.html ~9840 frames per 360 s at SMP=4.**

---

## SLICE 94 — the ack-loop ceiling attacked from three sides: encode EXONERATED, wake->run latency MEASURED at 2-5 ms and CUT 2-4x by a wake-kick IPI; the TAIL is the next target

**New instrument, kernel:** `[wlat]` — perf_now_ns stamp on every enqueue
(`proc.enq_ns`), consumed at switch-in; the deep dump prints avg/max/n per
60 s. This is THE number under every IPC hop (futex wake, pipe write,
eventfd) on tobyOS.

**MEASURED (anim.html, nth=1, SMP=4, 360 s each, all gate-clean):**

| config | frames | typical gap | [wlat] avg |
|---|---|---|---|
| pre-94 (ack-after-decode, Q60) | 9840 | 23-25 ms | — |
| ack-early, Q60 | 7320 | 26-43 ms (noisy) | 1.6-14.6 ms |
| ack-early, Q30 | 8520 | 21-25 ms | 1.4-4.7 ms |
| ack-early, Q60, **wake-kick** | 9570 | **20-22 ms** | **0.8-1.4 ms** |

**Verdicts:**
 1. **Encode is not a lever.** Q30 (5.9 KB jpegs) vs Q60 (7.3 KB) moved the
    typical gap ~1-2 ms, inside run noise — slice 68's rejection
    re-validated in the nth=1 regime. Q60 stays (CW_Q knob kept).
 2. **Wake->run pickup latency was the real IPC cost: avg 2-5 ms per wake.**
    Mechanism: wakes enqueue on the BSP queue; a halted AP sleeps until its
    NEXT LAPIC tick before stealing (percpu.h even documented the design).
    **Fix: SCHED_WAKE_VECTOR (0xFC) wake-kick IPI** — sched_enqueue kicks
    ONE cpu advertising `idle_halted` (claimed before the IPI; the sti;hlt
    pair makes the pre-hlt race benign). ~15k kicks/60 s under chrome;
    avg wlat 2-4x better; best-ever steady-state gap. defboot clean.
 3. **Ack-before-decode: kept, verdict neutral.** Its theoretical 2-5 ms
    is invisible under hiccup noise; the best clean windows (20-22 ms) are
    with ack-early + kick.
 4. **The TAIL survives:** hiccup windows persist (one 2.5 s stall; 400 ms
    common), `[fxlate]` caught a futex wake sitting READY 363 ms, wlat max
    runs 1-17 s. NOTE the max is partly contaminated: a proc enqueued while
    still on_cpu (quiesce spinners) is unpickable by design and its stamp
    ages. Next slice: percentile histogram + straggler attribution (log
    pid/comm when a >100 ms-stamp proc is finally picked), then fix the
    mechanism it names.

**Session gotcha, now confirmed as law:** any Bash invocation that ran
`make CC=TMP='C:\t' ...` poisons a LATER `python run_watch.py` in the SAME
shell — QEMU's snapshot temp file lands on `C:\/vl.*` and it exits before
boot (silent no-log run). Builds and runs get SEPARATE shells; runs get
TMPDIR/TMP/TEMP prefixed to a real temp dir.

---

## SLICE 95 — THE TAIL HUNT, CLOSED: three convoy mechanisms named by attribution and fixed; worst stall 5.9 s -> 0.7 s, wlat >=1s events -> ZERO, frames +33%

**Instruments added** (all in the 60 s deep dump): `[wlat]` percentile
buckets (<100u/<1m/<10m/<100m/<1s/>=1s) + `[wtail]` straggler lines
(pid/comm/prio/io_boost/pickable-ms at pickup, 6/interval); a PICKABLE-time
re-stamp in sched_finish_switch (a proc queued while still on_cpu is
unpickable by design -- its stamp previously reported scheduler latency
that never existed); `[bklmax]` = longest single BKL hold + holder;
`[tlbto]` = shootdown ack-timeouts + giveups per interval.

**What attribution named, in the order the data peeled it:**
 1. **The 64-round shootdown storm (2.9 s class).** Every interval carried
    one ~12,000 Mcyc BKL hold; `[wtail]` showed clusters of threads
    pickable 5.9 s together; worst frame gaps 2.9-5.9 s. One
    `tlb_shootdown_remote` against a host-descheduled vCPU burned
    64 x ~45 ms retry rounds UNDER THE BKL (mprotect is the #1 caller:
    [lx-hold] mprotect=99.5k Mcyc/min). FIX: round cap 64 -> 2 + return on
    the pending-IPI argument (an awake CPU acks in us -- the cr3 filter's
    whole premise; a non-executing vCPU cannot consume a stale translation
    and takes its pending IPI at VM-entry before any user instruction).
 2. **Naive 4-level page walks (1 s class).** vmm_protect/vmm_unmap did a
    FULL descent per 4 KiB page -- including holes -- under g_vmm_lock
    with IRQS OFF (which also stopped that CPU acking OTHERS' shootdowns:
    the cascade fed itself). FIX: segmented walks (absent PML4/PDPT/PD
    subtrees skip 512G/1G/2M per iteration; leaf PT fetched once per 2 MiB)
    + sys_mprotect chunks the BKL every 32 MiB. mprotect 99.5k -> ~10k
    Mcyc/min. sys_mmap's eager-commit (alloc+memset+map, whole range) got
    the same 16 MiB chunking.
 3. **The iteration-bound ack round itself (the stubborn ~1.05 s
    constant).** One timed-out round = 2M pauses with a self-ack CR3
    reload every 256 iterations = ~7.8k VM EXITS under WHPX ~= 4.5k Mcyc.
    FIX: rounds are now TIME-bounded (10 ms deadline, self-ack every 4096)
    -- an awake CPU acks in microseconds, so 10 ms is two orders of margin.

**MEASURED, anim.html nth=1 SMP=4 360 s, all gate-clean:**

| metric | pre-95 | post-95 |
|---|---|---|
| worst frame gap | 5,920 ms | **715 ms** |
| [wtail] worst pickable | 8,737 ms | **576 ms** |
| [wlat] >=1s events/interval | 27-58 | **0 (all five intervals)** |
| [bklmax] single hold | ~2.9 s | **~230 ms** |
| frames/360 s | 9,840 | **13,050 (~36 fps, +33%)** |

**Safety verification:** the corruption-class guards all PASS on the new
kernel (QFREEZETEST x2 real, DEMRACETEST, MAPTEST incl. post-fork CoW
divergence -- exactly the classes an early shootdown give-up would break);
defboot clean; fxspur=0; [fxcycle] never fired.

**What remains, honestly:** occasional 200-700 ms events whose scale
matches HOST-imposed vCPU descheduling ([clkchk] records 1.5-1.9 s
mono-clock gaps; WHPX steals those slices regardless of what the guest
does). Not fixable in-guest; [tlbto]/[bklmax]/[wtail] keep it visible if
the profile shifts. A ~230 ms [bklmax] residue is still unattributed by
syscall -- next candidate if the tail ever matters again.

**Pre-existing failure surfaced (NOT this slice's): EFDTEST FAILs (lost
cross-thread eventfd wakeup) on committed HEAD too** -- A/B'd by stashing
slice 95 and rebuilding; verdict identical. Filed here as an open item:
chrome's message_pump_epoll rides eventfd, so this may matter for latency
or worse. Hunt it as its own slice.

---

## SLICE 96 — the eventfd lost-wakeup fixed (chrome jumps to ~40 fps); TIER 3 PHASE 0 RUN AND CLOSED: VirGL is blocked at the HOST layer — park tier 3 with evidence

**EFDTEST root cause.** `eventfd_write` never called `poll_event_notify()`
— eventfd was exactly the "readiness source with no event hook" the
slice-67 design warned about, silently relying on the `poll_tick` sweep.
Two consequences: in the console boot harness (pid 0 parked in proc_wait,
never in idle_loop) the sweep never runs and a cross-thread eventfd wake
was LOST FOREVER (the EFDTEST FAIL); under chrome it meant every
message-pump signal could ride sweep latency instead of waking the pump
immediately. FIX: `poll_event_notify()` from `eventfd_write` (and from
`eventfd_read`'s drain, for EPOLLOUT watchers/blocked writers).

**Verified:** EFDTEST FAIL -> PASS; all 7 console guards green (suite
itself runs ~2 s faster). On the real workload (anim, nth=1, SMP=4,
360 s, gate-clean): **13,050 -> ~14,610 frames (~40 fps, +12%)**;
`[wlat]` avg 680-850 us -> **~310 us** with wake volume DOUBLED
(~355k/interval, ~28k wake-kicks) — the message pump now flows
event-driven; >=1s events still ZERO; 10 windows >200 ms (worst 435 ms).
Cumulative since the tail hunt opened: 9,840 -> 14,610 = **+48%**.

**TIER 3 PHASE 0 (control-first, per the tier-2.5 law) — VERDICT: PARK.**
 * E1a: this host's QEMU 10.2.0 lists `virtio-gpu-gl-pci` and
   `egl-headless` — but `-display egl-headless` SEGFAULTS instantly, with
   or without the GPU device; `gtk,gl=on` also segfaults.
 * E1b: `-display sdl,gl=on -device virtio-gpu-gl-pci` boots tobyOS far
   enough to prove BOTH sides: our virtio_gpu.c negotiated **virgl=yes**
   (guest ready), then host virglrenderer died — "Unable to create OpenGL
   context >= 3.0 ... virgl could not be initialized: 22" (the QEMU
   process only reaches Windows' GDI OpenGL 1.1 fallback, no vendor ICD)
   — the device went inoperative and our driver correctly declined it.
 * E2: the WSL2 control rig has /usr/lib/wsl/lib (libd3d12/CUDA) but NO
   /dev/dri render node — no cheap place to measure GPU-raster value on
   real Linux either.
 * Perf case is weak regardless: CPU raster now delivers ~40 fps against
   a ~52 Hz compositor ceiling; the binding constraint is the ack cycle,
   not rasterization.

**Unblock conditions, recorded:** (a) a host GL path for QEMU (vendor ICD
visible to the qemu process, or a build whose ANGLE/EGL works — today's
segfaults suggest the shipped ANGLE is broken); (b) a demonstrated
raster-bound workload (heavy pages / WebGL / headed fidelity); (c) or a
different dev host. Until one of those exists, tier-3 kernel work
(/dev/dri + DRM ioctls + Mesa staging) would build toward a mechanism
this environment cannot run — exactly the mistake Phase 0 exists to
prevent.

## SLICE 96b — RETRACTION OF THE PARK, same session: TIER 3 PHASE 0 is PASSED. The host blocker was QEMU's missing ANGLE, and this machine already carries the cure

Slice 96 parked tier 3 on "host virglrenderer cannot initialize". That
conclusion was PREMATURE by one probe. The full chain, now measured:

 * The GDI-GL-1.1 fallback happens even on this RTX 5090 + Radeon iGPU
   box in a Console (non-RDP) session — because QEMU 10.2's Windows build
   ships NO EGL/GLES libraries at all (its dir has SDL2.dll and nothing
   ANGLE-shaped). egl-headless' instant segfault is the missing-library
   error path.
 * **Staging Chromium-flavour ANGLE DLLs (libEGL/libGLESv2/
   d3dcompiler_47, taken from the local Edge install) on PATH fixes it:**
   `-display egl-headless -device virtio-gpu-gl-pci` initializes
   virglrenderer (ANGLE -> D3D11 -> real GPU), and the guest completes
   bring-up: virgl=yes negotiated, GET_DISPLAY_INFO answered (scanout
   1280x800), RESOURCE_CREATE_2D ok, RESOURCE_ATTACH_BACKING ok.
   Reproduced twice, deterministic. `logs/setup_angle.sh` stages the DLLs
   into logs/angle/ (gitignored); prefix PATH with it for virgl runs.
 * Remaining deterministic quirk = **Phase 1 work item #1**: under
   virgl, SET_SCANOUT of a RESOURCE_CREATE_2D resource is rejected
   (resp 0x1203, invalid resource) and our driver declines the device
   (probe rc=-13). The 2D-compat scanout path differs in virgl mode;
   either mask the VIRGL ack until 3D is actually wanted (2D keeps
   working on a -gl device) or move scanout to the virgl/blob resource
   flow.

**Phase 0 status: PASSED, prerequisite documented.** Tier 3 is UNBLOCKED.
Phase 1 scope (unchanged from the design doc, now with a concrete first
step): (1) virgl-aware scanout in virtio_gpu.c (item above); (2)
/dev/dri/card0 + the DRM/virtio-gpu ioctl surface; (3) stage Mesa's
virgl driver DSOs into the chrome sysroot; (4) measure chrome GPU-raster
value against the CPU-raster ~40 fps baseline BEFORE committing to the
full surface. The perf-value caveat from slice 96 still stands: CPU
raster is at ~40 fps vs a ~52 Hz ceiling, so tier 3's near-term value is
fidelity/WebGL/headroom — hold Phase 1 to the same measure-first
discipline.

## SLICE 96c — the "un-ack VIRGL to keep 2D scanout" fix is DISPROVEN (same-slice retraction); the real Phase-1 shape is now measured

**Predicted in 96b:** SET_SCANOUT's 0x1203 rejection came from the guest
ACKING `VIRTIO_GPU_F_VIRGL`, which flips the device into virgl mode; not
acking it would keep the plain 2D contract on a `-gl` device.

**MEASURED: WRONG.** With the ack removed (`driver=0x00000001_00000000`,
log now prints `virgl=avail-not-acked`), the `-gl` device STILL rejects
SET_SCANOUT with 0x1203 and the driver still declines (rc=-13). QEMU's
stderr shows `virtio_gpu_virgl_process_cmd: ctrl 0x103, error 0x1203`
**in both runs** — i.e. **QEMU's `virtio-gpu-gl` routes every command
through virglrenderer regardless of what the guest negotiates.** There
is no 2D-compat path on a GL device to fall back to; the resource must
exist in virglrenderer's world for scanout to accept it.

**Kept anyway, relabelled honestly:** the guest no longer ACKS a feature
whose command flow it cannot speak (the slice-89 "unvalidated mixed
state" lesson). It is ack-honesty, NOT a fix — the decline is unchanged
and is Phase 1's job. Plain `virtio-gpu-pci` is unaffected (probe: binds,
SET_SCANOUT ok, cursor ready, device live) and defboot is clean.

**Phase 1, restated with evidence:**
 1. Scanout on a GL device needs the **virgl resource flow** (3D resource
    create / blob resources), not RESOURCE_CREATE_2D. This is the first
    real code item and it is bigger than a feature flag.
 2. **Cheap alternative worth evaluating first:** attach BOTH devices —
    plain `virtio-gpu-pci` keeps driving the display through the proven
    2D path, while the `-gl` device exists solely to back 3D contexts via
    `/dev/dri`. That sidesteps item 1 entirely for the bring-up.
 3. Then /dev/dri/card0 + DRM ioctls, Mesa virgl DSOs in the sysroot,
    and the measure-first gate: prove GPU raster beats the CPU-raster
    ~40 fps baseline before building the full surface.

**METHOD NOTE (third time this arc):** a mechanism guess about someone
else's implementation is a hypothesis, not a fix. This one cost a build
and two probes because it was cheap to test — which is exactly why it
was tested before being written down as done.

---

## SLICE 97 — TIER 3 PHASE 1a DONE: the GL device is no longer declined, it is BOUND in a 3D-only role with a VERIFIED virgl pipe

**The change.** A `-gl` device rejects 2D SET_SCANOUT by design (96c:
everything routes through virglrenderer), and the driver used to throw
the whole device away for it (rc=-13) — taking a live control queue with
it. Now, when SET_SCANOUT fails on a virgl-capable device, the probe
runs the **3D capability handshake** instead of declining:

```
[virtio-gpu] features: ... virgl=acked (3D role)
[virtio-gpu] SET_SCANOUT bad response type=0x1203
[virtio-gpu] 3D-ONLY mode: scanout is virgl-owned (2D SET_SCANOUT refused, expected).
             capset id=1 (VIRGL) maxver=1 maxsize=308 | CTX_CREATE ok
[virtio-gpu] 3D pipe VERIFIED -- device bound for tier 3
[pci] driver virtio-gpu bound to 00:04.0 (1af4:1050) strat=exact
```

**A real virglrenderer answered**: GET_CAPSET_INFO returned capset id 1
(VIRGL), max version 1, a 308-byte capability blob; CTX_CREATE opened a
rendering context and CTX_DESTROY closed it. That is the whole 3D
control path — host GL stack, virtqueue, command encoding, response
parsing — proven end to end from inside tobyOS.

**VIRGL is acked again** (96c removed the ack as honesty when we could
not speak the flow). The 3D role *wants* virgl mode; a 3D-only device
never claims a scanout, so display duty is unaffected.

**Regression-checked:** plain `virtio-gpu-pci` unchanged — binds,
SET_SCANOUT ok, backend installed, HW cursor active, `fb0 1280x800
backend=virtio-gpu` registered; defboot clean.

**What Phase 1a deliberately did NOT do:** no `/dev/dri`, no DRM ioctls,
no Mesa. The context is opened as proof and immediately destroyed. The
next slices are (b) a `/dev/dri/card0` node + the DRM ioctl surface
(DRM_IOCTL_VERSION / GEM / VIRTGPU_EXECBUFFER / GETPARAM) backed by this
device, (c) staging Mesa's virgl driver into the chrome sysroot, and
(d) the measure-first gate: GPU raster must beat the CPU-raster ~40 fps
baseline before the full surface is worth finishing.

**Two structural notes for whoever writes (b):** the file already
carried `virtio_gpu_ctx_create()` / `virtio_gpu_ctx_destroy()` /
`virtio_gpu_virgl_available()` from an old "Phase 3 foundation" — they
gate on `g_vgpu_active && virgl_enabled`, neither true during probe,
hence this slice's probe-time twins. And a 3D-only device sets
`three_d_only` and does NOT set `g_vgpu_bound`, so nothing in the
display path can mistake it for a scanout provider.

---

## SLICE 98 — TIER 3 PHASE 1b DONE (real 3D through /dev/dri, proven), PHASE 1c OPENED with a live gap list; 1d still gated

### Phase 1b: the DRM render node — DONE and PROVEN

`grep -r DRM_IOCTL src/` returned nothing for this entire arc. It now
returns a driver. New `src/linux_drm.c` (+ `include/tobyos/linux_drm.h`)
implements the Linux **render-node** contract over virtio-gpu:

| ioctl | behaviour |
|---|---|
| `DRM_IOCTL_VERSION` | reports driver name `virtio_gpu` (what Mesa matches on), with the two-pass length query Linux callers expect |
| `DRM_IOCTL_GET_CAP` / `SET_CLIENT_CAP` | answered / accepted |
| `VIRTGPU_GETPARAM` | 3D_FEATURES=1, CAPSET_QUERY_FIX, CONTEXT_INIT; BLOB/HOST_VISIBLE deliberately 0 (keeps Mesa on the classic path this layer serves); unknown ids log themselves |
| `VIRTGPU_GET_CAPS` | fetches the REAL virglrenderer capability blob |
| `VIRTGPU_CONTEXT_INIT` | lazily creates the 3D context |
| `VIRTGPU_RESOURCE_CREATE` | RESOURCE_CREATE_3D + contiguous backing + ATTACH_BACKING + CTX_ATTACH, returns GEM + resource handles |
| `VIRTGPU_MAP` + `mmap` | maps the BO's real DMA pages into the client (new `mmap_map_phys_user`, SHARED+NOFREE) — the mapping IS the buffer, not a shadow |
| `VIRTGPU_TRANSFER_TO/FROM_HOST` | TRANSFER_*_3D |
| `VIRTGPU_EXECBUFFER` | submits virgl command streams |
| `VIRTGPU_WAIT`, `GEM_CLOSE` | synchronous-complete / release |
| anything else | `[drm] UNHANDLED ioctl nr=0x..` — **the gap list**, once per number |

Supporting virtio-gpu work: RESOURCE_CREATE_3D / CTX_ATTACH+DETACH /
TRANSFER_*_3D / GET_CAPSET commands, a 3-descriptor `issue_cmd_payload`
for buffers too large for the scratch page, SUBMIT_3D ceiling 4 KiB ->
64 KiB (Mesa's streams are tens of KiB), and the 3D gate fixed:
`g_vgpu_active && virgl_enabled` required the DISPLAY backend, which a
slice-97 3D-only device never installs — now `vgpu_3d_ready()`.

**PROOF — the DRMTEST guard (`programs/linux-drmtest`), PASS:**

```
[drm] open dri/renderD128 (virgl capset=1 max=308)
[drmtest] driver name: virtio_gpu        [drmtest] 3D_FEATURES=1
[drm] GET_CAPS id=0 ver=0 -> 308 bytes   [drmtest] caps non-zero bytes=70
[drm] 3D context 2 created               [drmtest] bo_handle=1 res_handle=64
[drmtest] BO mapped + coherent           [drmtest] TRANSFER_TO_HOST ok
[drm] EXECBUFFER #1 size=8               [drmtest] EXECBUFFER ok
[DRMTEST] VERDICT: PASS exit=3
```

Every step Mesa's virgl driver performs at start-up, answered by a real
virglrenderer (70 non-zero bytes of genuine capability data). Zero
`UNHANDLED` entries on that path.

**Regression-checked:** plain `virtio-gpu-pci` unchanged (binds, scanout,
cursor, `fb0` registered); a boot with NO GPU device reports **SKIP**,
not FAIL (the verdict was fixed after the first run labelled the
expected configuration as a failure — a guard that cries wolf in the
common case trains everyone to ignore it); defboot clean.

### Phase 1c: Mesa staged, and the gap list is LIVE

`programs/chromium/mesa.sh` stages Debian bookworm's Mesa into the chrome
sysroot (own script, never run by the Makefile — a GPU experiment must
not destabilise the browser payload). Two traps worth recording: Debian
hardlinks all 13 gallium drivers to ONE 25 MB blob, so a naive extractor
staged exactly one driver and silently lost virgl (tarfile reports them
as LNKTYPE, not files); and materialising them all costs ~330 MB, so the
script prunes to virgl + swrast.

`programs/linux-egltest` then drives REAL Mesa and reports what breaks.
The list so far, in the order the runs produced it:

1. **`libGLdispatch.so.0` missing** -> staged libglvnd. CLOSED.
2. **glvnd returns EGL_BAD_PARAMETER for every call** -> it finds Mesa
   only through a vendor ICD json; staged `/etc/egl_vendor.json` +
   `__EGL_VENDOR_LIBRARY_FILENAMES`. CLOSED.
3. **`libxcb-randr.so.0` missing** -> staged the xcb closure. CLOSED.
4. **dlopening `libEGL_mesa.so.0` directly gives no `egl*` symbols** — a
   glvnd VENDOR library exports `__egl_Main`, not the public API. Not a
   bug: go through `libEGL.so.1`. CLOSED (understanding).
5. **OPEN — the real one.** With glvnd wired up, `eglGetPlatformDisplay`
   now succeeds (`display ok`) and `eglInitialize` fails 0x3001, and the
   log shows why: Mesa loaded **`dri/swrast_dri.so`, never
   `virtio_gpu_dri.so`, and never opened `/dev/dri` at all**.
   `MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu` did not change that, because
   the surfaceless platform first ENUMERATES devices
   (libdrm `drmGetDevices2`) — which walks `/sys/class/drm/renderD*/…`
   and lists `/dev/dri/`. We serve `open("/dev/dri/renderD128")` but
   provide neither the directory listing nor the sysfs tree, so Mesa
   concludes there is no GPU and falls back to software.

**Next step is therefore well-localized:** make the DRM device
DISCOVERABLE — `/dev/dri` as a listable directory plus the
`/sys/class/drm/renderD128/device/{vendor,device,…}` nodes libdrm reads
(the `/proc` + `/sys` machinery from Track B already exists) — then
re-run the same guard and read the next entry.

### Phase 1d: still gated, honestly

No frame measurement is possible until Mesa actually binds the virgl
driver. The slice-96 caveat stands: CPU raster already delivers ~40 fps
against a ~52 Hz compositor ceiling and the binding constraint is the
~20 ms ack cycle, so tier 3's near-term value is fidelity/WebGL/headroom
rather than fps — measure before finishing the surface.

### Method note

The layer is named `lxdrm_*` in `src/linux_drm.c` because `src/gpu_drm.c`
is an unrelated NATIVE display abstraction that also uses "drm"; two
things called drm in one kernel is exactly the confusion this arc has
paid for before.

---

## SLICE 99 — Phase 1c: Mesa now LOADS THE VIRGL DRIVER off our node; the remaining block is libdrm's sysfs validation, named exactly

Two measured gap-list entries closed, and the next one is no longer a
guess — it is a specific libdrm function with a specific file list.

**Entry #6 CLOSED — a DRM fd must stat as a CHARACTER DEVICE.**
`gbm_create_device()` opened `/dev/dri/renderD128` and returned NULL
having issued **zero ioctls**: it fstats the fd first and refuses
anything that is not `S_ISCHR`. Our generic stat emitter only knew
FILE/DIR (there is no `VFS_TYPE_CHR`), so every DRM fd looked like a
regular file. Added `linux_emit_chrdev_stat` — `S_IFCHR | 0666` with a
real `st_rdev` built from the Linux split encoding, DRM major 226, minor
128 for `renderD*` / 0 for `card*` (recorded on the file at open).

**Entry #7 CLOSED (understanding) — the platform choice mattered.**
EGL *surfaceless* makes Mesa enumerate devices via `drmGetDevices2()`
(list `/dev/dri`, walk `/sys/dev/char/...`), so it never opened our node
at all and silently loaded `swrast_dri.so`. **GBM takes an fd we open
ourselves**, needs no enumeration, and is the platform chrome's Ozone
path uses — so the test now tries GBM first and keeps surfaceless as a
fallback.

**RESULT: Mesa selects and loads OUR driver.** With those two fixed:

```
[drm] open dri/renderD128 (virgl capset=1 max=308)
[drm] ioctl #1 VERSION (nr=0x0 sz=64) -> 0
[drm] ioctl #2 VERSION (nr=0x0 sz=64) -> 0
[lopen] /opt/chrome/sysroot/dri/virtio_gpu_dri.so     <-- THE VIRGL DRIVER
```

Two VERSION calls is exactly right (libdrm's `drmGetVersion` is a
two-pass length-then-strings query), both answered 0, and Mesa used the
name we reported to dlopen **`virtio_gpu_dri.so`**. The guest's DRM
identity now drives Mesa's driver selection.

**Entry #8, OPEN — and precisely located.** After loading the driver,
Mesa fell back to `swrast_dri.so` **without issuing a single further
ioctl**. That rules out our ioctl implementations entirely: the failure
is upstream of the device, inside gallium's `pipe_loader_drm_probe_fd`,
which calls libdrm's **`drmGetDevice2(fd)`** — and that function
validates a DRM fd through **sysfs**, not ioctls:

| libdrm needs | what it does |
|---|---|
| `/sys/dev/char/226:128/device/drm` | `stat()` — `drmNodeIsDRM()`; absent = "not a DRM node" |
| `/sys/dev/char/226:128/device/subsystem` | `readlink()` — basename must be `pci` |
| `.../device/{vendor,device,revision,subsystem_vendor,subsystem_device}` or `config` | PCI identity |
| `.../device/uevent` | `PCI_SLOT_NAME=` for the bus address |

None of that exists in our `/sys`, so libdrm returns -EINVAL and gallium
gives up before ever touching the device. **This is the whole remaining
distance to "Mesa renders on the host GPU."**

Next slice, mechanically: extend `src/sysfs.c` (it already has
`sysfs_add_dir` / `sysfs_add_file(path, generator)`) with that tree,
teach it one symlink (`subsystem` -> `.../bus/pci`) since sysfs currently
serves only files and dirs, and re-run the same guard. The PCI values
are already known to the kernel — `virtio_gpu.c` logs `1af4:1050` at
probe.

**Also this slice: a per-ioctl trace** (`[drm] ioctl #N NAME (nr sz) ->
rc`, first 40 calls). The `UNHANDLED` list answers "what don't we
implement"; it could never answer "which implemented call did Mesa
dislike, and how far did it get" — which is exactly the question that
mattered here, and it settled it in one run.

**Unchanged and re-verified:** DRMTEST still PASS (the full
VERSION->GETPARAM->GET_CAPS->CONTEXT_INIT->RESOURCE_CREATE->MAP->
TRANSFER->EXECBUFFER->GEM_CLOSE walk, ioctls #3-11 above, all rc=0);
defboot clean.

**Phase 1d remains gated** on entry #8, for the same reason as before.

---

## SLICE 100 — sysfs grew a DRM device tree + symlink support (kept, correct); the "libdrm sysfs validation is the blocker" theory is RETRACTED — it was not

Slice 99 ended by naming libdrm's `drmGetDevice2(fd)` sysfs walk as the
remaining block. This slice built exactly that and **it did not change
the outcome**, so the theory is retracted rather than patched around.

**Built and kept (all correct, all needed eventually):**
 * `sysfs_add_link()` + a `readlink` op in sysfs — the tree served only
   files and directories before, and libdrm keys bus type on
   `readlink(".../device/subsystem")`.
 * `/sys/dev/char/226:128/device/` with `drm/`, `subsystem ->
   ../../../bus/pci`, `vendor`, `device`, `revision`,
   `subsystem_vendor`, `subsystem_device`, `uevent` (with
   `PCI_SLOT_NAME=0000:00:04.0`), and `config` — the 64-byte BINARY PCI
   header, which is what Linux libdrm actually reads
   (`drmParsePciDeviceInfo` opens `config`, not the text files).
 * Node cap 64 -> 96.

**A real bug found on the way, worth the slice by itself:** the first
`sysfs_readlink` returned the LENGTH, but `vfs_readlink`'s callers test
`rc != VFS_OK` and measure the buffer themselves. Every *successful*
readlink therefore reported failure. The in-guest probe caught it
immediately.

**VERIFIED BY IN-GUEST PROBE (not by inference):** the client now stats
and reads the tree itself and prints the results —

```
stat /sys/dev/char/226:128                      OK
stat /sys/dev/char/226:128/device               OK
stat /sys/dev/char/226:128/device/drm           OK
stat /sys/dev/char/226:128/device/vendor        OK
stat /sys/dev/char/226:128/device/uevent        OK
readlink subsystem -> ../../../bus/pci
vendor file: 0x1af4
```

Everything libdrm asks for is present and correct. **And Mesa still
falls back to swrast.**

**WHAT IS ACTUALLY KNOWN, stated without theory:**
 1. Mesa opens our node and issues exactly two `VERSION` ioctls, both
    rc=0 (libdrm's normal two-pass `drmGetVersion`).
 2. Mesa uses the name we return to dlopen **`virtio_gpu_dri.so`** — the
    real virgl driver.
 3. ~40 ms later it dlopens `swrast_dri.so`, having issued **zero**
    further ioctls on our fd.
 4. The sysfs tree is complete and readable (above).

So the rejection happens inside the virgl driver's screen/winsys
creation, before it touches the device, and it is NOT (or not only) the
sysfs validation this slice implemented.

**NEXT INSTRUMENT, and it is the obvious one: capture the child's
STDERR.** `LIBGL_DEBUG=verbose` is already set and Mesa explains these
refusals in detail on stderr (`EGL_LOG_LEVEL=debug` adds more), but the
boot harness only surfaces the client's stdout — Mesa has very probably
been telling us the answer for three runs and we have not been reading
it. Wire the spawned process's stderr into the serial log, re-run, and
let Mesa name its own reason instead of inferring it from ioctl
absence. (Every previous wall in this arc fell to an instrument, not to
a guess; this is the same shape.)

**Regressions:** DRMTEST still PASS (ioctls #3-11, all rc=0); defboot
clean; the new sysfs nodes are inert for everything else.

**METHOD NOTE.** Two slices in a row I named a mechanism from reading
someone else's source rather than from a measurement, and both times the
fix built on it did not move the result (96b's VIRGL un-ack, 99's sysfs
walk). The pattern is clear enough to write down: *a mechanism inferred
from third-party source is a hypothesis with a test attached, never a
diagnosis* — and the cheap test here was always "make the other program
tell you", which is what the next step finally does.

---

## SLICE 101 — Mesa's GBM device now CREATES on our DRM node; the last blocker is `/dev/dri` not being a listable directory, and Mesa says so itself

**The instrument I recommended in slice 100 already existed, and had been
answering all along.** Slice 100's handoff said "capture the client's
stderr, Mesa is probably explaining itself unread". The harness *already*
surfaces stderr as `[fd2]` — and it had been printing the reason since
the first Mesa run:

```
MESA-LOADER: failed to open virtio_gpu: libLLVM-15.so.1: cannot open shared object file
failed to load driver: virtio_gpu
```

Not sysfs at all — a missing DSO. Slices 99 and 100 built a sysfs device
tree on a theory while the actual cause sat in the log. **Correction to
the slice-100 method note: the failure was not "inferring from
third-party source" so much as not READING THE OUTPUT WE ALREADY HAD.
Grep the child's stderr before theorising about its behaviour.**

**Closing the DSO chain, then automating it.** libLLVM-15 -> libsensors5
-> libstdc++6 -> libz3-4/libpciaccess0, each costing a full ISO build +
boot to discover one name. So `mesa.sh` now parses the driver's own
`DT_NEEDED` **transitively** and prints the entire remaining gap at once:

```
[mesa] MISSING DEPENDENCIES -- add their packages to SEED:
    libpciaccess.so.0 (needed by libdrm_intel.so.1)
    libz3.so.4 (needed by libLLVM-15.so.1)
...
[mesa] dependency closure COMPLETE for the virgl driver
```

**RESULT: `gbm_create_device -> ok`.** Mesa loads `virtio_gpu_dri.so`,
initialises its GBM device against our render node, and no longer falls
back on load failure. The sysroot is 454 MB with the GL stack staged
(opt-in; `mesa.sh` is never run by the Makefile).

**The one remaining blocker, in Mesa's own words:**

```
MESA-LOADER: failed to retrieve device information
```

That is `loader_get_pci_id_for_fd()` -> libdrm `drmGetDevice2(fd)`.
Slices 99-100 gave it the sysfs tree it wants (verified in-guest: every
path stats, `readlink subsystem -> ../../../bus/pci`, binary `config`
present) — but `drmGetDevice2` **also `opendir("/dev/dri")` and iterates
the directory** to fill in the device's node names, and our `/dev/dri`
is not a directory at all: `open("/dev/dri/renderD128")` is special-cased
in the syscall layer (syscall.c, `strncmp(dev, "dri/", 4)`), with no
entry that `getdents64` can enumerate.

**NEXT, and it is now a single well-defined task:** make `/dev/dri`
listable — a FILE_KIND_DIR whose `getdents64` yields `card0` and
`renderD128` — then re-run the same guard. Everything else in the chain
is already proven working.

**Regressions:** DRMTEST still PASS (11 ioctls, all rc=0); defboot clean.
Phase 1d (the GPU-vs-CPU-raster measurement) remains gated on the above,
with the slice-96 caveat unchanged: CPU raster is ~40 fps against a
~52 Hz ceiling, so tier 3's near-term value is fidelity/WebGL/headroom.

---

## SLICE 102 — `/dev/dri` is now a real listable directory (kept); it did NOT close `drmGetDevice2`, so that guess is retracted too. STOP GUESSING: instrument the accesses.

**Built and kept:** `/dev/dri` is a `FILE_KIND_DIR` whose `getdents64`
yields `card0` and `renderD128` as `DT_CHR` entries. It existed only as a
special case in the open path before, which is wrong on its own terms —
a Linux `/dev/dri` is listable, and `drmGetDevices2()` (the enumerating
sibling libdrm uses on other paths) needs it. Correct regardless.

**It did not fix the failure.** Mesa still prints
`MESA-LOADER: failed to retrieve device information`, with a new
follow-on (`libEGL warning: did not find extension DRI_DRI2 version 2`).

**Scoreboard of guesses about libdrm's internals, this arc:**

| slice | guess | outcome |
|---|---|---|
| 96b | un-ack VIRGL restores 2D scanout | disproven by measurement |
| 99 | sysfs `/sys/dev/char/...` tree unblocks it | built; changed nothing |
| 100 | plus symlink + binary PCI `config` | built; changed nothing |
| 102 | plus a listable `/dev/dri` | built; changed nothing |

Four in a row. The common failure is that each was inferred from reading
third-party source and then *built* rather than *tested first*. Every one
produced a correct, keepable improvement — and none of them was the
cause, because none was measured before it was implemented.

**THE NEXT STEP IS AN INSTRUMENT, NOT A FIX.** `drmGetDevice2` fails
inside a small, finite set of filesystem accesses. We already log opens
(`[lopen]`); we do not log the `stat`/`readlink`/`openat` results that
this path depends on. Add a narrow trace — every `stat`, `readlink`,
`openat` and `getdents64` whose path starts with `/sys/dev/char`,
`/sys/class/drm`, `/sys/bus/pci` or `/dev/dri`, printing path and
return value — run the guard once, and the failing access will name
itself, exactly as `[fd2]` named the missing DSO in slice 101 after
three slices of theorising.

**What is genuinely working (unchanged, re-verified):** the whole lower
stack. `gbm_create_device -> ok`; Mesa loads `virtio_gpu_dri.so` off our
node's own VERSION answer; the DRM surface serves the full Mesa-shaped
walk (DRMTEST PASS, 11 ioctls, all rc=0); the Mesa dependency closure is
complete and self-checking. The remaining gap is one library function's
view of one device's metadata.

defboot clean.

---

## SLICE 103 — the instrument finally arrives: `[gpupath]` proves libdrm touches NOTHING, killing four slices of theory at once, and names a real gap by measurement

**The instrument.** `[gpupath]` logs every `stat` / `newfstatat` /
`readlink` whose path lies under `/sys/dev/char`, `/sys/class/drm`,
`/sys/bus/pci`, `/sys/devices/pci` or `/dev/dri`, with its return value.
Six lines of filter; it should have existed four slices ago.

**What it proved immediately.** During Mesa's device probe, libdrm makes
**ZERO accesses to any of those paths.** The only entries in the trace
are my own `probe_sysfs()` calls. So:

* `drmGetDevice2`'s sysfs walk never runs — it returns before touching
  the filesystem;
* therefore the sysfs tree (99), the symlink + binary PCI `config`
  (100), and the listable `/dev/dri` (102) could never have been the
  cause. **All three are now disproven by direct measurement rather than
  by "it didn't help".** They stay, because each is correct Linux
  behaviour we were missing, but none of them was the bug.

**A real gap, found by the same trace, not by reading source:**

```
[gpupath] stat /dev/dri/card0       -> -1
[gpupath] stat /dev/dri/renderD128  -> -1
```

**Path-based `stat()` of the DRM nodes FAILS.** `/dev/dri/*` exists only
as a special case in the `open()` handler; `vfs_stat` knows nothing about
it. Anything that stats a node by name before opening it — which is
exactly what libdrm's enumeration does for each directory entry — sees
ENOENT. Fix `vfs_stat` for those paths next; the trace will say whether
it moves libdrm.

**Also fixed this slice (measured, not guessed).** The slice-99
char-device stat only covered `LX_fstat`, but glibc's `fstat()` reaches
the kernel as `newfstatat(fd, "", AT_EMPTY_PATH)` or `statx` — both of
which flattened a DRM fd to `mode 0666`, i.e. a regular file. libdrm
checks `S_ISCHR` before anything else, so it would have bailed there
regardless. Both paths now emit `S_IFCHR` + `st_rdev` 226:minor.
(Consistent with the zero-access observation, though the trace shows
libdrm still not proceeding, so this was necessary and not sufficient.)

**Score so far, honestly.** `gbm_create_device -> ok`; Mesa loads
`virtio_gpu_dri.so` from our node's own VERSION answer; the DRM surface
serves the full Mesa-shaped walk (DRMTEST PASS, 11 ioctls, rc=0); the
Mesa dependency closure is complete and self-checking. Still no GL
context: `MESA-LOADER: failed to retrieve device information` persists,
and we now know it happens without any filesystem access at all.

**Next, in order:** (1) make `vfs_stat` answer for `/dev/dri/*`;
(2) re-run and read `[gpupath]` — if libdrm still touches nothing, the
remaining possibilities are narrow and mechanical (it is failing inside
`drmGetVersion`-derived bus detection on the fd itself, which the
existing `[drm] ioctl #N` trace will show). Both steps are now
instrument-first, which is the only thing that has actually worked on
this front.

defboot clean.
