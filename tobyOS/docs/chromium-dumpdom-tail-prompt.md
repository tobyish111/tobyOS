# Task: get real headless Chromium to `--dump-dom` `<h1>tobyOS</h1>` on tobyOS

You are picking up a **deep, live, successful Chromium bring-up**. Real,
unmodified `chrome-headless-shell` 151.0.7922.34 (V8 + Blink + SwiftShader)
runs on tobyOS's Linux ABI as a **full multi-process tree** — browser, GPU,
network/utility and renderer children — with **zero FATALs, zero crashes, and
every process exiting cleanly**.

**It still does not print the DOM.** That is the unmet goal. Two concrete,
named blockers remain (below). Everything here was *measured*; where I guessed
and was wrong, I have said so, because those are the traps.

---

## THE PRIME DIRECTIVE: measure, then READ CHROME'S SOURCE. Do not infer.

Every wall this arc fell to the same move, and every wasted cycle came from
skipping it:

1. Reproduce, and use the **in-tree instruments** (below) to see what the
   kernel sees.
2. When chrome complains, **fetch the actual chromium source line**:
   ```bash
   curl -sS "https://chromium.googlesource.com/chromium/src/+/refs/tags/151.0.7922.34/<path>?format=TEXT" \
     | base64 -d > /c/t/f.cc
   ```
   Exact line numbers. **Do not** use the WebFetch summarizer for line
   attribution — it mis-attributes. This single command cracked *five* walls.
3. Fix the smallest real thing. Re-measure.

**AND: verify the instrument before trusting what it says.** Three times this
session an instrument nearly produced a false finding (see LESSONS). This is
the single most expensive failure mode on this codebase.

---

## CURRENT STATE (end of slice 22, 2026-07-20)

Commits: `5b4e530` (slice 21), `f088295` + `931f4b1` (slice 22), on branch
`slice21-fork-inherits-sockets`. Nine kernel bugs fixed — see "What was fixed".

A clean run now looks like:

```
FATAL:                 0
crashes (exit -1):     0
process exits:         all 0 (85 = the unrelated hello-boot harness task)
KERNEL PANIC:          0
[pmm] double free:     0
"no connection":       1-2      <-- BLOCKER A (count varies by RUN LENGTH, see below)
"Corruption detected": 0        <-- BLOCKER B, CLOSED in slice 23
DOM:                   0        <-- the goal
```

**Read the run length before comparing counts.** `with no connection` is 1 or 2
purely as a function of how long the run lasted: children invited at ~7.5 s hit
their 15 s deadline only once the run passes ~22.5 s. Check the last kernel
`[N ms]` timestamp (`grep -ao '^\[[0-9]* ms\]' LOG | tail -1`) before reading
anything into a difference.

### BLOCKER A — RESOLVED (slice 24, commit `a59d3c7`). Read this before the history below.

**Cause: `sendto()` on AF_UNIX was never implemented.** `lx_send` handled only
UDP and TCP, so an AF_UNIX socket fell through to the TCP check and returned
`-ENOTSOCK` (-88). `recvfrom` and `sendmsg` BOTH had an AF_UNIX arm; `sendto`
did not, and that asymmetry is what hid it for three slices. The browser sends
its invitation with `sendmsg` (it needs SCM_RIGHTS), so that path always worked
— which is exactly why every trace showed the invitation arriving intact. The
child answers with a plain 80-byte `sendto`, the one path with the hole.

So the framing "browser never sends vs child never receives" was wrong on both
counts: **the child replied to every invitation and the kernel rejected the
call.** Both sides then waited on each other until the 15 s deadline.

Found by tracing socket-fd syscall **args AND return values** — the recent-
syscall ring logs names only and structurally cannot show a failing return.
That instrument is committed as `[chan]` (see the instrument list).

After the fix: every -88 gone, and the channel round-trips —
`pid=37 sendto fd=5 232 -> 232` immediately followed by
`pid=8 recvmsg fd=53 -> 232`, repeatedly, in both directions. Channel ops
11 -> 234. `with no connection` is 0 *by mechanism* (a conversing child cannot
time out), not merely absent.

**The GPU-process SIGKILL is NOT a tobyOS bug — chrome does it to itself.**
Identified with the `[sig]` instrument (signal_send is the only way a signal
becomes pending, so every fatal signal passes through it):

```
[sig] sig=9 -> pid=16 'chrome-headless-shell' FROM pid=2 'chrome'
```

pid 2 is the BROWSER. `GpuProcessHost` decides the GPU child is hung and kills
it, then respawns — the tail shows `reaped pid=16` immediately followed by
`fork parent pid=15 -> child pid=16`, i.e. a **respawn loop**. Under TCG the
guest runs at ~3-4% real-time, so SwiftShader init cannot meet chrome's timeout.
This is chrome's designed recovery path (it logs WARNING "has crashed 1 time(s)"
and continues), so treat it as an emulation-speed artifact, NOT a correctness
bug to chase. Note the harness DOES pass `--disable-gpu` +
`--disable-gpu-compositing` (kernel.c:7286) and chrome spawns a GPU process
anyway; the swiftshader flags visible in `[execve-argv]` belong to a UTILITY
child, not the browser. Don't misread those as the browser's flags.

Throughout the respawn loop the Mojo channel stays healthy: 230 `[chan]` ops,
zero -88, zero "no connection". So the transport is fine and the GPU churn is
not blocking IPC.

**THE FRONT IS NOW EXACTLY WHAT THIS DOC PREDICTED IT WOULD BE.** Blockers A and
B have both fallen and there is STILL NO DOM. Per the scope-honesty section, that
is the strong signal: the renderer's failure to complete a document load was
never about the process model. Attack "renderer completes a document load"
directly — Blink loader/parser — using the profiler and wait-graph, and re-run
the `--timeout=5000` probe (if neither the DOM nor "Page load timed out." appears,
`executeCommands()` still never runs).

Caution on run length changed meaning: chrome now does so much more work that
emulation runs at ~4% real-time, so guest time advances slowly. Do not read a
0 count for anything that fires on a deadline without checking the last
`[N ms]`.

### (historical) BLOCKER A — Mojo: children get the invitation and never reply

Measured, with the `[unix]` and `[scm]` traces:

```
[unix] pair pid=16 a=4 b=5
[unix] send pid=16 sock=4 -> peer=5 len=184 nfds=1 peer_count=1  <- browser sends ONCE
[unix] recv pid=27 sock=5 peer=4 queued=1 want=4096              <- child consumes it
[scm]  recvmsg pid=27 nf=1: kind=2                               <- and gets the fd
```

Every child receives the 184-byte Mojo invitation **and** its SCM_RIGHTS fd
(`ctl_len=20 flags=0x0`, clean). Then it goes into `epoll_wait` and **never
sends anything and never reads again**. The browser sends once and waits. It is
a mutual stall. After ~15 s the child logs
`child_thread_impl.cc:908 Terminating current process after 15 seconds with no
connection` and exits 0.

**Ruled out** (don't re-chase):
- The socket layer. Send and receive both demonstrably work cross-process.
- fd inheritance. The child has the channel at the RIGHT fd:
  `kMojoIPCChannel(2) + kBaseDescriptor(3) = fd 5`, confirmed `FILE_KIND_SOCKET`
  in `[execve-fds]`. Same math checks out for `kFieldTrialDescriptor(3)→fd 6`
  and `kPseudonymizationSaltDescriptor(7)→fd 10`.
- SCM_RIGHTS cross-process. Verified live, both directions.
- The shared-memory eventfd upgrade: `mojo/core/embedder/features.cc` shows
  `kMojoUseEventFd` is `FEATURE_DISABLED_BY_DEFAULT` on Linux. This is the
  PLAIN SOCKET channel. (The handoff's earlier slices describe chrome taking
  the shared-memory path — that must have been a different flag combination.)
- `fd_alloc_into` — already routes through `proc_fds()` correctly.
- MAP_SHARED sharing itself — **now genuinely verified** (slice 23). Note the
  pre-slice-23 version of this bullet was WRONG in the way that matters: the
  trace said "attached", but those attaches were *aliasing* onto recycled inode
  numbers. It is correct now, and the stall did not move.

**Note `kMojoIpcz` is `FEATURE_ENABLED_BY_DEFAULT`** on this build. ipcz is the
newer transport and may expect more than the legacy node channel. That is an
unexplored lead.

**Next measurements:**
1. Instrument syscall **ARGS** for `read`/`write`/`sendmsg`/`recvmsg` on the
   child (the ring logs names only). Does the child ever `write()` to fd 5 and
   get an error? Does it read a *partial* message and stall waiting for the
   rest? tobyOS's AF_UNIX is message-queued with a `tail_off` partial-read
   path — Mojo's channel framing may want byte-stream semantics it isn't
   getting.
2. Check what the child does with the received fd. It is `kind=2` (a FILE) —
   chrome's shared-memory region. Does it `mmap` it? (Use the `[shm]` trace —
   **cap now 200, but check you aren't hitting it**.)
3. Read `mojo/core/node_channel.cc` / the ipcz driver for what the child is
   supposed to send back after `ACCEPT_INVITEE`, then look for that syscall.

### BLOCKER B — CLOSED (slice 23, commit `30a2d21`). Do not re-chase.

Was: `Corruption detected in shared-memory segment` ×3
(`persistent_memory_allocator.cc:890`). **Cause: the slice-22 shm page cache
aliased unrelated regions onto one page set**, because it was keyed on the raw
inode NUMBER — and a number is only an identity while the file is LINKED.
tobyfs reissues the lowest free number on the very next create, and chrome
creates every shared-memory region as a temp file it unlinks IMMEDIATELY.
Measured: **inode 9 allocated 6× in one boot**, six processes "attaching" to it
at four different sizes. Fixed by keying on **(inode, incarnation)** plus a
`struct file`-pinned region that rides fork/dup/SCM_RIGHTS. Corruption is now 0
and children verifiably attach to the exact incarnations the browser created.
Full write-up: `docs/chromium-bringup-m1.md` slice 23.

**What this rules out for Blocker A: shared memory itself.** It is now correct
and verified cross-process, and the Mojo stall survived the fix completely
unchanged. So the stall is NOT about the browser and child seeing different
bytes. Attack the channel protocol, not the memory.

Two notes worth keeping from the hunt:
- `SetCorrupt()` at :890 is only the **reporter**; the real predicates are its
  ~10 call sites. The constructor has two — cookie mismatch on a non-kReadWrite
  attach (:383), and an inconsistent header on an existing segment (:443).
- The `[shm]` trace printing **"attached"** reads like success but actually meant
  **aliased**. The tell was inconsistent `np` on a single key.

---

## What was fixed (do NOT redo) — nine bugs

### Slice 21 (`5b4e530`) — multi-process
1. **`fork()` copied the WRONG fd table.** `fork.c` read `parent->fds` directly,
   but a CLONE_FILES thread's own `fds[]` is deliberately EMPTY (the real table
   is the thread-group leader's, behind `proc_fds()`), and **chrome launches
   children from a dedicated launcher THREAD** — so every child got ZERO
   descriptors and died at its first `dup2` with `_exit(127)`. The handoff's
   then-recommended front ("make fork inherit sockets") was a correct but
   *insufficient* diagnosis; this was the real blocker.
2. **`file_clone` refused `FILE_KIND_SOCKET`.** Added a refcount to
   `struct sock` (`in_use` stays a pure liveness flag — the pool scans test
   THAT), and moved the AF_UNIX peer-EOF wake into `sock_close`'s LAST-reference
   branch, so an inheritor closing its copy no longer signals EOF to the peer.
3. **`execve` recorded the LITERAL path**, so `/proc/self/exe` was
   self-referential → `DIR_ASSETS` became `/proc/self` → every child died on
   `Invalid file descriptor to ICU data received`. Resolve symlinks up front.
   Also `vfs_read_all` now follows symlinks (`vfs_follow_link`).
4. **User stack was 8 eager pages (32 KiB) with NO grow-down** → demand
   grow-down to an 8 MiB floor (Linux's default `RLIMIT_STACK`).
5. **`copy_from_user` did only a RANGE check** → any process could PANIC the
   kernel with a bad pointer to any read-taking syscall. Added
   `uaccess_prepare_read`, plus `copy_from_user_nofault`/`uaccess_probe_resident`
   for exception context (isr.c's crash diagnostic was panicking the kernel
   from *inside* fault handling).

### Slice 22 (`f088295`) — Mojo + sandbox
6. **`MAP_SHARED` file-backed mmap did not SHARE.** `linux_mmap_file` reserved
   ANONYMOUS pages and filled them by reading the file, so two processes
   mapping one file each got a PRIVATE COPY. `mmap.c` had documented this gap
   for ages; it turned out to be the blocker.
   `base/memory/platform_shared_memory_region_posix.cc:209` calls
   `CreateAndOpenFdForTemporaryFileInDir` **unconditionally** — there is NO
   memfd path in this build, so no flag routes around it
   (`--disable-dev-shm-usage` only picks the directory). Added a **per-INODE**
   page cache (`struct shm_cache`), keyed by `vfs_file.ino` not path so it
   survives the `unlink()` chrome does immediately. `MAP_PRIVATE` still copies.
7. **`/proc/<pid>/task/` did not exist** — chrome's sandbox asserts
   mono-threadedness by stat'ing it and testing `st_nlink == 3`
   (`thread_helpers.cc:41`). Added it plus per-thread `task/<tid>`.
   **TERMINATED-but-unreaped threads must read as GONE**, or
   `thread_helpers.cc:104` ("Stopped thread does not disappear in /proc") fails
   after 30 polls. `nlink` and directory presence must agree on "live" or the
   FATAL just alternates between :41 and :104.
8. **`st_nlink` was hardcoded to 1** in both stat emitters.
9. **`*at()` syscalls ignored a real dirfd for RELATIVE paths.** chrome opens
   `/proc` as a dirfd and reaches through it for `"self/task/"` (:41) and
   `"self/fd/"` (`proc_util.cc:79`). Added `resolve_user_path_at()`, used by
   `newfstatat` + `openat`.

---

## Instruments in-tree — ALL `CHROMIUM_BOOT`-gated, behaviour-neutral

**Use in this order.** Getting the order wrong cost me several wrong turns.

1. **Ring-3 sampling profiler** (`sched.c`, `prof_dump_and_reset`) — separates
   "spinning in user code" from "blocked in kernel" in ONE run.
2. **Wait-graph tracker** (`syscall.c`, `waitt_*`) — who is blocked in
   `futex`/`epoll_wait`, on what, for how long. This is what showed the
   14-process tree sitting idle rather than livelocking.
3. **Recent-syscall ring** (`LX_RECENT = 384`) — dumped at the heartbeat and on
   any fatal user fault. Names only, **no args** (the main gap — see Blocker A).
4. **`[execve-argv]` / `[execve-fds]`** (`fork.c`) — the child's full argv AND
   its actual open fd table at exec, with `FILE_KIND` per fd. This pair is what
   identified the ICU failure. Read them side by side.
5. **`[unix] pair/send/recv`** (`socket.c`) — AF_UNIX socketpair creation and
   every enqueue/dequeue, with **pool indices** (NOT fds) and peer. Reads as a
   conversation. Cap 40.
6. **`[scm]`** (`syscall.c`) — SCM_RIGHTS send/recv, logging the **KIND** of
   each passed fd (and for sockets, whether its peer still exists). Emitted
   BEFORE the install/CTRUNC paths, which may `file_close()` those pointers —
   logging after would be a use-after-free.
7. **`[shm] MAP_SHARED`** (`syscall.c`) — inode, page range, and
   CREATED/attached. This is how you prove cross-process sharing. **Cap 200.**
8. **`[libmap]`** — `.so` load map; correlate a `.so`-region rip → base+offset
   → `objdump`.
9. **`[path]`** — traces `headless` / `.pak` / `self/exe` lookups.
10. **uaccess PTE dump** — present-RO vs unmapped on a `copy_to_user` EFAULT.
11. **`vmm_remap_count()`** — `map_4k` "already mapped" is rate-limited to 16
    (one run emitted ~165k, drowning the log); the counter keeps the signal.

Symbolize: main PIE @ `0x500000`, ld.so @ `0x40000000`, `.so`s @
`0x100000000000+` (via `[libmap]`). Tools:
`C:/msys64/ucrt64/bin/{objdump,nm}.exe`. Note `nm` + `awk strtonum` gave wrong
bracketing symbols for me — grep the address neighbourhood instead.

Useful one-liners:
```bash
grep -ac "tobyOS</h1>" logs/chromium-m0.log        # the goal
grep -aoE "exit code=[-0-9]+" logs/chromium-m0.log | sort | uniq -c
grep -a "FATAL\|Check failed" logs/chromium-m0.log
grep -a "\[unix\]\|\[scm\]\|\[shm\]" logs/chromium-m0.log
grep -ac "double free\|KERNEL PANIC\|with no connection\|Corruption detected" logs/chromium-m0.log
```

---

## LESSONS — every one of these cost real time

- **VERIFY THE INSTRUMENT BEFORE TRUSTING IT.** Three near-misses:
  - The `[shm]` trace capped at 16 and the run produced **exactly 16**, making
    it look like children never mapped shared memory. Pure artifact. I caught it
    only by counting lines. **Check whether a trace hit its cap.**
  - The harness prints `/opt/chrome/chrome-headless-shell not present --
    SKIPPED` for **ANY** `proc_spawn` failure. It said "no binary" while the two
    lines above showed the binary's ELF loading fine. Real cause: **`.argc = 15`
    is HARDCODED** next to a NULL-terminated `argv[]` I had shortened to 14.
    **Re-count `.argc` whenever you touch that argv** (`kernel.c`).
  - `fault_count` / `last_fault_rip` in the "terminating user process" line are
    **cumulative page-fault stats, NOT the cause of death**. The real exception
    is printed separately (`EXCEPTION 3: Breakpoint` = chrome's own CHECK;
    `EXCEPTION 14` = a real #PF). I disassembled `last_fault_rip` for an int3
    death — a dead end.
- **The errno in chrome's `Check failed: . : <errno>` is STALE/incidental.** It
  does not name the failing predicate (which is stripped from official builds).
  Fetch the source line.
- **Chrome probes kernel features with deliberately-INVALID flags expecting
  `-EINVAL`.** tobyOS syscalls must VALIDATE unknown flag bits. There are
  probably more latent instances.
- **A "verified working" entry is only verified for the PATH that exercised
  it.** `/proc/self/exe` was on the do-not-re-chase list — true for the browser
  (spawned with a real path), false the moment a process `execve`s the symlink.
  Same for the `.pak` lookup; both hang off `DIR_ASSETS`. Scope such claims.
- **`resolve_user_path()` prepends the cwd**, so an *at()* relativity test must
  be made on the RAW user string. Testing after it is dead code — exactly what
  my first attempt did.
- **Anything reading user memory inside exception handling must use
  `copy_from_user_nofault`.** The normal accessors RESOLVE faults, which is
  re-entrant if you are already handling one — that turned the crash diagnostic
  into a kernel panic.
- **Shared physical pages need `page_ref_inc` per address space.**
  `free_subtree` (`vmm.c`) frees a leaf only at `page_ref <= 1`. Miss it and
  every teardown frees the frame — `[pmm] WARN: double free`, frames recycled
  under live processes.
- **The stale-log trap.** The boot truncates `logs/chromium-m0.log`, so while a
  build runs you are reading the PREVIOUS run's log. Always check the log's
  mtime is NEWER than `tobyOS.iso` before believing anything.
- **A short run can fake a fix.** I reported "no connection → 0" as a win; the
  browser had crashed at 16.8 s, before children starting at ~7 s could reach
  their 15 s deadline. Check the run actually lasted long enough.
- **VLOG is compiled OUT** of official chrome (`--vmodule`/`-v=1` = nothing).
- fds 0/1/2 → serial, so `--dump-dom`'s stdout **would** appear if produced.
- `clang -c`, not `-fsyntax-only`. **klibc has no `strstr`.**
- Don't `2>&1` native exes in PowerShell; `grep -c` returning 0 exits 1 and
  will abort a `&&` chain.

---

## Known gaps left OPEN (deliberate, documented in-code)

- **`memfd_map` has the IDENTICAL double-free bug** as the shm cache did — it
  maps `mf->pages[]` with no `page_ref_inc`. Not fixed because `memfd_unref`
  frees pages directly and reconciling the two ownership models needs its own
  pass. **If double-frees reappear, this is the source.**
- `strncpy_from_user` probes only its FIRST page; a string crossing into an
  unmapped page can still panic. Needs per-page probing inside the copy loop.
- procfs `/proc/<pid>/fd/<n>` reads `p->fds` directly — the same
  thread-empty-table bug as fixed in `fork`. Diagnostic-only.
- `openat` only opens by resolved kernel path in its DIRECTORY arm; a
  dirfd-relative open of a non-directory is still cwd-relative.
- The shm cache has **no writeback and no reclaim** (now 256 entries; slice 23
  needs more because regions no longer wrongly collapse onto one key). The cache
  IS the file's contents for mappers, but a later `read()` sees on-disk bytes.
  Entries are permanent, which is also why a file with no inode identity
  (`ino_gen == 0`, ramfs) deliberately falls back to copying — see the call site
  in `linux_mmap_file`.
- ~~NOT re-validated against the non-chrome boot/test harness.~~ **PAID in slice
  23**: `bash logs/defboot.sh` (stock ISO, no chrome harness — the heaviest
  native-ELF path) boots to login + GUI, tobyfs formats/journals/mounts clean,
  0 hard faults. That covers slice 21's `uaccess` change and slice 23's
  `vfs_file`/`struct file` layout growth. Re-run it after any struct change.

---

## Where the DOM actually comes from (READ THIS — it is not what it looks like)

`--dump-dom` is **not** "navigate and print". From
`components/headless/command_handler/headless_command_handler.cc`: chrome
navigates to an INTERNAL WebUI page `chrome://headless/headless_command.html`
(served from `headless_command_resources.pak` beside the binary, found via
`DIR_ASSETS` → `/proc/self/exe`), and only when THAT page finishes loading does
`DocumentOnLoadCompletedInPrimaryMainFrame()` fire →
`Target.exposeDevToolsProtocol` → `Runtime.evaluate("executeCommands(...)")`.
That injected JS creates a SECOND target, navigates it to your real URL, dumps
the DOM, and `OnCommandsResult` prints it with `std::cout`.

Extract the JS from the local .pak (v5 resource pack, gzip blobs):
```python
import struct, gzip
d=open('programs/chromium/chrome-headless-shell-linux64/headless_command_resources.pak','rb').read()
cnt,_=struct.unpack_from('<HH',d,8)
e=[struct.unpack_from('<HI',d,12+i*6) for i in range(cnt+1)]
for i in range(cnt):
    print(gzip.decompress(d[e[i][1]:e[i+1][1]]).decode('utf-8','replace'))
```

**`--timeout=5000` is a decisive probe:** it makes the JS race the load and call
`handleCommands()` anyway, so the DOM is dumped even if the load never
completes, plus a "Page load timed out." log. If NEITHER appears,
`executeCommands()` never ran.

---

## Build / verify

- `bash logs/chromium-m0.sh` (incremental) ·
  `bash logs/chromium-m0-clean.sh` (**required after any struct-layout change**).
- `BOOTSECS=90` keeps a run short; the browser does not exit on its own, so the
  default 1200 s just idles after chrome stalls.
- Fresh `disk.img` per run: `dd if=/dev/zero of=disk.img bs=1M count=16`.
- Do **not** put `&` inside a `run_in_background` call.
- Verify: `--dump-dom` must print `<h1>tobyOS</h1>` (self-verifying in the
  serial log). Then switch to `--screenshot` and host-diff the PNG.
- Never `taskkill msedge` (the user's real browser).
- One coherent slice per branch off `main`; extend `docs/chromium-bringup-m1.md`
  + a `MEMORY.md` line each slice; trailer
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`; merge `--no-ff`.

---

## Non-issues / secondary

- **Slice 19 was a MISDIAGNOSIS.** The "read-only page" is chrome's own
  `protected_memory` section, which chrome mprotects RO and then *verifies* with
  a syscall write expecting `-EFAULT`. Returning `-EFAULT` is CORRECT.
- **Flaky Vulkan crash** (`memcpy+0x35d` during SwiftShader extension
  enumeration; same corruption family as ld.so `check_match` reading a garbage
  `link_map->l_versyms`). Only gates `--screenshot`; `--disable-gpu` avoids it.
  Not on the `--dump-dom` path.
- Ruled out for the renderer stall: CPU spin (profiler), futex deadlock (wait
  graph).

---

## Scope honesty

The hard, uncertain tiers — GL, V8 memory, signals, storage, Mojo transport,
multi-process launch, the sandbox `/proc` probes — are **done**. Chrome runs
clean. What remains is two named blockers with concrete next measurements, a
working method (measure → read chrome's source → smallest fix), and instruments
that answer "spinning or blocked?", "who talks to whom?" and "is this memory
actually shared?" in a single run.

But be clear-eyed: **the multi-process theory has never been proven.** It has
cleared nine real blockers without yet producing a document load. If Blockers A
and B fall and there is STILL no DOM, that is the strong signal — it would mean
the renderer's failure to complete a document load was never about the process
model, and the front becomes "renderer completes a document load" attacked
directly with the profiler and wait-graph.
