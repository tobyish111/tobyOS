# Task: get real headless Chromium to `--dump-dom` `<h1>tobyOS</h1>` on tobyOS

You are picking up a **deep, live, successful Chromium bring-up**. Real, unmodified
`chrome-headless-shell` 151.0.7922.34 (V8 + Blink + SwiftShader) runs on tobyOS's
Linux ABI. Its GL init, storage, signals, V8 memory **and its whole Mojo IPC fast
path** now work. It still does not print the DOM. **The blocker is now localized
to one sentence, and the recommended next step is a concrete, bounded kernel fix.**

Everything below was *measured*. Where I guessed instead of measuring, I have said
so — those are the traps to avoid.

---

## THE PRIME DIRECTIVE: measure, then READ CHROME'S SOURCE. Do not infer.

Two fixes were wasted this arc inferring from a symptom that turned out to be
meaningless (see LESSONS). The winning move every time was:

1. Reproduce, and use the **in-tree instruments** (below) to see what the kernel sees.
2. When chrome complains, **fetch the actual chromium source line**:
   ```bash
   curl -sS "https://chromium.googlesource.com/chromium/src/+/refs/tags/151.0.7922.34/<path>?format=TEXT" \
     | base64 -d > /tmp/f.cc
   ```
   Exact line numbers. **Do not** use the WebFetch summarizer for line attribution —
   it mis-attributes lines. That one command cracked two walls in a row.
3. Fix the smallest real thing. Re-measure.

---

## Where the DOM actually comes from (READ THIS FIRST — it is not what it looks like)

`--dump-dom` is **not** "navigate to the URL and print". From
`components/headless/command_handler/headless_command_handler.cc`:

- chrome navigates the web contents to an **internal WebUI page**,
  `chrome://headless/headless_command.html`, served from
  `headless_command_resources.pak` beside the binary (found via `DIR_ASSETS` →
  `/proc/self/exe`).
- **Only when THAT page finishes loading** does
  `DocumentOnLoadCompletedInPrimaryMainFrame()` fire →
  `Target.exposeDevToolsProtocol` → `Runtime.evaluate("executeCommands(...)")`.
- That injected JS (extract it from the .pak, snippet below) creates a **second**
  target, navigates *it* to your real URL, dumps the DOM, and `OnCommandsResult`
  prints it with `std::cout`.

**Measured: `executeCommands()` never runs.** Decisive proof: with `--timeout=5000`
the JS *races* the load and **still calls `handleCommands()` afterwards**, so it
would have dumped the DOM anyway *and* logged `"Page load timed out."` — neither
appeared. So `DocumentOnLoadCompletedInPrimaryMainFrame` never fires: **the
renderer completes no document load, not even chrome's own 229-byte internal page.**

Already verified working, so do **not** re-chase: `/proc/self/exe`, the `.pak`
lookup, DIR_ASSETS, the JS itself, virtual time, the `data:` URL, the target page,
the Mojo channel, GL.

---

## CURRENT FRONT (slice 21, 2026-07-20): cross-process Mojo never connects

`--single-process` is **GONE**. Chrome runs a real multi-process tree: browser +
gpu + network/utility children, ~14 processes alive at once, all healthy in
`epoll_wait`/`futex`. Five kernel bugs were fixed to get here (commit `5b4e530`,
detail below). **Children no longer die** — instead each one starts, waits, and
self-terminates:

```
[INFO:content/child/child_thread_impl.cc:908]
Terminating current process after 15 seconds with no connection.
```

**Verified working, do NOT re-chase:** the child inherits the Mojo socketpair at
the RIGHT fd — `kMojoIPCChannel(2) + kBaseDescriptor(3) = fd 5`, confirmed
`FILE_KIND_SOCKET` in the `[execve-fds]` dump — and every other descriptor lines
up too (`kFieldTrialDescriptor(3)→fd 6`, `kPseudonymizationSalt(7)→fd 10`).
SCM_RIGHTS works **cross-process** (`[scm] sendmsg passing 1 fd(s)` in one
process, `recvmsg got 1 fd(s)` in another). So the plumbing is right and the
failure is in the handshake TRAFFIC over that socket.

**Next step:** instrument syscall ARGS (the ring logs names only). Log
`sendmsg`/`recvmsg`/`read`/`write` fd + byte counts per pid, gated on
`CHROMIUM_BOOT`, and answer: does the browser ever write the Mojo invitation to
its end, and does the child ever read fd 5? That splits the problem in one run
— browser-never-sends vs child-never-receives vs delivered-but-malformed.
Suspect the AF_UNIX stream semantics under cross-process use (partial reads,
`tail_off`, EOF-vs-EAGAIN) before suspecting chrome.

## SUPERSEDED FRONT (kept for context): make `fork()` inherit sockets

`--single-process` is a legacy mode chrome barely supports, and the page that must
load first is a **WebUI** page (strict process requirements). That is the leading
explanation for the silent non-commit — and it also explains why `about:blank`
behaved identically (the WebUI page is always first, whatever your target URL).

**Dropping `--single-process` made chrome use its real multi-process model and
actually spawn children.** They currently all `_exit(127)`
(`exit_code=32512 == 127<<8`, chrome's launcher's "child setup failed"), ending in
`FATAL … GPU process isn't usable. Goodbye.`

Two gaps on that path are already fixed (merged `d644e6c`), each of which pushed the
child further — children went from **6 → 25 syscalls**:

- **`/dev/null` + `/dev/zero` did not exist.** `base::LaunchProcess` opens
  `/dev/null` to remap a child's stdio before `execve`. Added
  `FILE_KIND_DEVNULL`/`DEVZERO`.
- **`open("/proc/<pid>/exe")` returned ENOENT** — `stat` said symlink and `readlink`
  worked, but `procfs_open` had no case. It now follows the symlink to the real
  executable.

### → YOUR NEXT FIX (measured, not guessed)

Children still `_exit(127)`, now after a **second `dup2`**. Cause:

> `fork()` clones the fd table via `file_clone()` (`src/file.c`), which
> **deliberately refuses `FILE_KIND_SOCKET`** ("a child shouldn't silently share a
> parent socket"). The child therefore gets `NULL` for every socket fd, and the
> launcher's `dup2` of the **inherited Mojo socketpair fd** fails.

Linux `fork()` inherits **all** fds. Fix: let `file_clone` share the `struct sock`
across fork, which needs a **refcount on `struct sock`** (a struct change ⇒ **clean
build**), with `sock_close`/`file_close` only tearing down at zero. Mind the
existing `sock_unix_peer_close()` semantics.

After that, expect further multi-process walls (cross-process Mojo, fd passing).
Note: the **SCM_RIGHTS implementation already handles real processes** — it
`file_clone`s on send and `fd_alloc_into`s into `current_proc()` at recvmsg time,
which is the receiver either way.

**Why this path:** multi-process is chrome's real model, and every step so far has
been a genuine POSIX-correctness fix that outlives chrome. Fighting legacy
`--single-process` may not be winnable at all.

---

## Instruments already in-tree — USE THEM IN THIS ORDER

All `CHROMIUM_BOOT`-gated and behaviour-neutral. Using them out of order cost me
several wrong turns.

1. **Ring-3 sampling profiler** (`src/sched.c`, `prof_dump_and_reset`) — samples the
   user `rip` on every ring-3 timer tick. **Run this FIRST**: it separates "spinning
   in user code" from "blocked in kernel" in one run. (It immediately disproved my
   "busy livelock" reading of thread states.)
2. **Wait-graph tracker** (`src/syscall.c`, `waitt_*`) — which thread is blocked in
   `futex`/`epoll_wait`, on what address/fd, and for how long. A true wait graph
   (entries live exactly as long as the block).
3. **Recent-syscall ring** (`LX_RECENT = 384`) — dumped at the heartbeat and on any
   fatal user fault. 48 was too small; it hid the spin that cracked the last bug.
4. **`[libmap]`** (`linux_mmap_file`) — `.so` load map; correlate a `.so`-region rip
   → base+offset → `objdump`.
5. **`[path]`** — traces `headless` / `.pak` / `self/exe` lookups.
6. **`[memfd]` / `[scm]`** — memfd create/ftruncate/seals/mmap and SCM_RIGHTS
   send/recv.
7. **uaccess PTE dump** — present-RO vs unmapped on a `copy_to_user` EFAULT.

Symbolize: main PIE @ `0x500000`, ld.so @ `0x40000000`, `.so`s @ `0x100000000000+`
(via `[libmap]`). Tools: `C:/msys64/ucrt64/bin/{objdump,nm}.exe`.

Extract the headless JS from the local .pak (v5 resource pack, gzip blobs):
```python
import struct, gzip
d=open('programs/chromium/chrome-headless-shell-linux64/headless_command_resources.pak','rb').read()
cnt,_=struct.unpack_from('<HH',d,8)
e=[struct.unpack_from('<HI',d,12+i*6) for i in range(cnt+1)]
for i in range(cnt):
    print(gzip.decompress(d[e[i][1]:e[i+1][1]]).decode('utf-8','replace'))
```

---

## LESSONS — do not repeat these

- **Chrome's `Check failed: . : <errno>` — the errno is STALE/incidental.** It is
  whatever `errno` happened to be left over. It flipped `Success (0)` →
  `Invalid argument (22)` purely because an unrelated probe changed. It does **not**
  name the failing predicate (which is stripped from official builds). **Fetch the
  source line.** Two fixes were wasted before I did.
- **Chrome probes kernel features with deliberately-INVALID flags, expecting
  `-EINVAL`.** `memfd_create("", ~0)` and `eventfd2(0, ~0)` are both
  `PCHECK(ret < 0 && (errno == EINVAL || ENOSYS || EPERM))`. tobyOS accepted every
  flag and handed back valid fds ⇒ FATAL. **tobyOS syscalls must VALIDATE unknown
  flag bits, not ignore them — there are probably more latent instances.**
- **Pre-check kernel sources with `clang -c`, not `-fsyntax-only`** — the latter
  missed an implicit-declaration error that failed the real build. **`klibc` has no
  `strstr`.**
- **VLOG is compiled OUT of official chrome** — `--vmodule` / `-v=1` produce zero
  output. Don't plan around chrome's own logging.
- **The stale-log trap.** The boot truncates `logs/chromium-m0.log`, so *while a
  build runs you are reading the PREVIOUS run's log*. Wait for `boot in QEMU` in the
  run's `.out` **and** a fresh low timestamp before believing anything. This burned
  me twice.
- fds 0/1/2 are all console → serial, so `--dump-dom`'s stdout **would** appear if
  produced.
- **A "verified working" entry is only verified for the path that exercised it.**
  `/proc/self/exe` was on this doc's do-not-re-chase list — true for the BROWSER
  (spawned with a real path), false the moment a process `execve`s the symlink
  itself. Same for the `.pak` lookup: both hang off `DIR_ASSETS`. Scope every
  such claim to the code path that proved it.
- **A failure message can name a cause it never checked.** The harness prints
  `/opt/chrome/chrome-headless-shell not present -- SKIPPED` whenever
  `proc_spawn` returns < 0, for ANY reason. It said "no binary" while the two
  lines above showed the binary's ELF and interpreter loading fine. The real
  cause was `.argc = 15` hardcoded next to a NULL-terminated `argv[]` I had
  shortened to 14. **Re-count `.argc` whenever you touch that argv.**
- **`fault_count`/`last_fault_rip` in the terminate line are CUMULATIVE page-fault
  stats, not the cause of death.** The actual exception is printed separately
  (`EXCEPTION 3: Breakpoint` = chrome's own CHECK; `EXCEPTION 14` = a real #PF).
  Disassembling `last_fault_rip` for an int3 death is a dead end.
- **Anything reading user memory inside exception handling must use
  `copy_from_user_nofault`.** The normal accessors now RESOLVE faults (demand/COW),
  which is re-entrant if you are already handling a fault — that turned the
  crash diagnostic itself into a kernel panic.

---

## Build / verify

- `bash logs/chromium-m0.sh` (incremental) · `bash logs/chromium-m0-clean.sh`
  (**required after any struct-layout change**).
- Fresh `disk.img` per run: `dd if=/dev/zero of=disk.img bs=1M count=16`.
- Do **not** put `&` inside a `run_in_background` call — it tracks only the launch.
- Verify: `--dump-dom` must print `<h1>tobyOS</h1>` (self-verifying in the serial
  log). Then switch to `--screenshot` and host-diff the PNG.
- Never `taskkill msedge` (the user's real browser).
- One coherent slice per branch off `main`; extend `docs/chromium-bringup-m1.md` +
  a `MEMORY.md` line each slice; trailer
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`; merge `--no-ff`.

---

## Done already (do NOT redo) — slices 12–20, all merged

interior-dot path normalization · in-kernel fake X server + named AF_UNIX ·
stable per-file inodes · tobyfs `rename`/`fsync`/`unlinkat`/`flock` · SA_RESETHAND ·
`mprotect` VMA splitting · `copy_to_user` `-EFAULT` · **memfd_create** (page-backed,
mmap-coherent shared memory) · **SCM_RIGHTS** fd-passing over AF_UNIX ·
**memfd_create + eventfd2 flag validation** (cleared the
`mojo/core/channel_linux.cc` FATAL; the Mojo shared-memory channel now negotiates
end-to-end, SCM_RIGHTS verified live) · **AF_UNIX epoll/recvmsg EOF-vs-EAGAIN spin
fix** · **/dev/null + /dev/zero** · **open(/proc/<pid>/exe) follows the symlink**.

**Slice 21 (commit `5b4e530`)** — the five fixes that unlocked multi-process:
`fork()` sourcing its fd table from `proc_fds()` (a thread's own `fds[]` is
EMPTY by design, and chrome forks from a launcher THREAD — this, not the socket
refusal, was why children had no descriptors) · socket refcount so fd
inheritance shares the endpoint, with peer-EOF moved to the LAST reference ·
`execve` resolving symlinks so `/proc/self/exe` and therefore `DIR_ASSETS` are
real (fixed the ICU death) · `vfs_follow_link` + symlink-following
`vfs_read_all` · demand grow-down user stack to 8 MiB (was 8 eager pages =
32 KiB, no growth) · `uaccess_prepare_read` + `copy_from_user_nofault` (a bad
pointer to any read-taking syscall could PANIC the kernel).

Known gaps left open: `strncpy_from_user` probes only its first page; procfs
`/proc/<pid>/fd/<n>` still reads `p->fds` directly (same thread-empty-table bug,
diagnostic-only); and slice 21 has NOT been re-validated against the non-chrome
boot/test harness, which the uaccess change touches globally.

Known non-issues / secondary:
- **Slice 19 was a MISDIAGNOSIS.** The "read-only page" is chrome's own
  `protected_memory` section, which chrome mprotects RO and then *verifies* with a
  syscall write expecting `-EFAULT`. Returning `-EFAULT` is CORRECT. Not a bug.
- **Flaky Vulkan crash** (deterministically `memcpy+0x35d` in libc during SwiftShader
  extension enumeration; same corruption family as ld.so `check_match` reading a
  garbage `link_map->l_versyms`). Only gates `--screenshot`; `--disable-gpu` avoids
  it entirely. Not on the `--dump-dom` path.
- Ruled out for the renderer stall: CPU spin (profiler), futex deadlock (wait graph —
  the browser main thread cycles healthily at 124–268 ms).

---

## Scope honesty

The hard, uncertain tiers (GL, V8 memory, signals, storage, Mojo IPC) are **done**.
What remains is a layer-by-layer tail, but it is no longer mysterious: you have one
named blocker (`fork()` must inherit sockets), a working method (measure → read
chrome's source → smallest fix), and instruments that answer "spinning or blocked?"
and "who waits on whom?" in a single run. Expect each fix to reveal the next — every
one so far has been a real, bounded kernel bug, and most outlive chrome entirely.
