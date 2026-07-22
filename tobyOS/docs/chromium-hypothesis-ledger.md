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
