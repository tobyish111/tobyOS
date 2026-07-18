# Chromium bring-up — M1 (in progress): closing the gap list

Continues [`chromium-bringup-m0.md`](chromium-bringup-m0.md). Same
instrument-first loop: run real chrome-headless-shell, read the measured wall,
close it, re-run, read the next wall.

## Slice 1 — `int3` → `SIGTRAP` (DONE, verified)

**Gap (from M0):** chrome's `IMMEDIATE_CRASH()`/`CHECK`/`DCHECK`/`NOTREACHED`
macros emit `asm("int3")`; tobyOS wired all IDT vectors DPL 0, so a user-mode
`int3` raised `#GP` (`err=0x1a` = IDT descriptor 3) instead of `#BP`.

**Fix (4 files, +32/-9):**
- `src/idt.c` — after the DPL-0 loop, raise vectors 3 (`#BP`) and 4 (`#OF`) to
  DPL 3 (`0xEE`) so user `int3`/`into` trap instead of `#GP`.
- `src/isr.c` `default_exception` — kernel-mode `#BP` logs + resumes (was a
  separately-registered `breakpoint_handler`); user-mode `#BP` → `SIGTRAP`
  (vector 4 → `SIGSEGV`) via the existing B15 `signal_deliver_fault()`.
- `src/kernel.c` — dropped the redundant `breakpoint_handler` + its
  `isr_register(3,…)`; the boot int3 smoke-test now exercises the real path.
- `include/tobyos/signal.h` — `TRAP_BRKPT`/`TRAP_TRACE` si_codes.

**Verified:** kernel int3 smoke-test still round-trips
(`[isr] breakpoint hit … / int3 returned cleanly`); chrome's `EXCEPTION 13
(#GP)` is now a clean `EXCEPTION 3 (#BP, user mode)` delivered as SIGTRAP
(chrome installs no SIGTRAP handler this early, so the default action terminates
it — correct POSIX behaviour). No boot regression, no panic.

## Slice 2 — V8's virtual-memory cage (DONE, verified)

**Two fixes in `src/mmap.c` (+ `prctl` in syscall.c):**

1. **`sys_mmap` skips the eager-commit loop for `PROT_NONE` anon** — a PROT_NONE
   mapping is an address-space *reservation*, not usable memory. V8's cage
   reserves e.g. 32 GiB `PROT_NONE`; eager-committing = 8.3M `pmm_alloc_page` →
   instant OOM. Now it records the VMA and commits nothing; `mmap_handle_page_
   fault` demand-zero-fills after an `mprotect` raises the perm.
2. **`mmap_handle_page_fault` now sets the editor root to `p->cr3`** before its
   `vmm_map`/`vmm_unmap`/`vmm_translate`. `vmm_map` edits the *global*
   `g_pml4_phys`, which between syscalls is the **kernel** PML4 — so the demand
   page for a cage slice landed in the wrong address space and the process
   re-faulted forever → SIGSEGV. `page_fault.c`'s COW/demand path already did
   this; `mmap_handle_page_fault` didn't. **Latent kernel bug**, never hit until
   a Linux process first demand-paged through this path (chrome's cage:
   reserve huge PROT_NONE → mprotect a 4 KB slice RW → touch it).
3. `prctl(2)` — accept-and-ignore the common SET options (incl. `PR_SET_VMA`
   0x53564d41, which V8 uses to name every cage/arena region for `/proc/maps`);
   unknown options still self-identify.

**Verified:** the cage `EXCEPTION 14` is gone (count 0); chrome now runs **753
syscalls** (was 557), through V8's entire `VirtualMemoryCage`/PartitionAlloc
setup (reserve 64 GiB → trim to align → 16 GiB cage → mprotect slices RW →
touch). The trace shows the reserve/mprotect/touch dance now succeeding.

## Slice 3 — AF_UNIX socketpair / Mojo IPC (DONE, verified)

Implemented AF_UNIX `socketpair(2)` as a new socket kind (`SOCK_KIND_UNIX`): a
bidirectional in-memory **message** channel (SEQPACKET). Two socks are
cross-linked (each reuses `peer_ip` as the peer's pool index + 1); `write`
enqueues one message into the peer's datagram ring, `read` dequeues one from its
own; `poll` reports POLLIN/POLLOUT/POLLHUP; close wakes the peer to EOF.
Reuses the existing `struct sock` datagram ring + `wq_recv` (no struct-layout
change; bumped `SOCK_MAX` 16→128 for chrome's many pairs). Files:
`socket.h`/`socket.c` (channel), `file.c` (read/write/close), `syscall.c`
(`file_poll_ready` + `lx_socketpair` + `gettimeofday` + `prctl` reads). No
`SCM_RIGHTS` (fd passing) yet — deferred until measured.

**Verified — a huge leap:** chrome went **753 → 5,193 syscalls**, created the
socketpairs, and **spawned 9+ threads** (shared-VM tgid=2), running deep into
multi-threaded Mojo/`ThreadPool` bring-up.

## Slice 4 — thread-pool/init syscall fills (DONE, verified)

Filled the batch chrome reaches once its `ThreadPool` is up:
`sched_getaffinity(204)` (reports all online CPUs so the pool sizes itself) +
`sched_setaffinity(203)`, `sysinfo(99)` (RAM from the PMM), `getpriority(140)`/
`setpriority(141)`, `getresuid(118)`/`getresgid(120)` (root), `clock_getres(229)`,
`statfs(137)`/`fstatfs(138)` (a tmpfs-shaped answer), `prctl PR_GET_NAME(16)`.
`landlock_*(444)` intentionally stays `-ENOSYS` (optional sandbox; chrome falls
back).

**Verified — another leap:** chrome went **5,193 → 20,014 syscalls**. The
syscall surface is now essentially complete for chrome startup — the *only*
remaining unhandled syscall is `landlock(444)`, correctly rejected.

## Slice 5 — thread fd-table sharing (`CLONE_FILES`) + fd limit (DONE, verified)

The thread NULL-call was **not** a missing syscall — it was a real kernel bug.
Diagnosed with a new cheap instrument: a **recent-syscall ring** (`src/syscall.c`)
dumped by `src/isr.c` on any fatal user fault (far cheaper than the full trace at
20k+ calls). It showed the crash was **timing-dependent** across freshly-spawned
threads (one victim spinning on `futex`, another right after `epoll_create1`) →
a threading race, not a deterministic gap.

**Root cause:** `struct proc` holds an inline `fds[PROC_NFDS]`, and
`sys_clone_thread` **copied** it (`file_clone` per fd) — that's *fork* semantics,
not thread (`CLONE_FILES`). So an epoll/socket fd created on one thread was
invisible to the others → cross-thread `fd_lookup` returned NULL → NULL handle →
the process **called a NULL function pointer** (`rip=0`, `cr2=0`, instruction
fetch). Breaks *every* multi-threaded Linux program.

**Fix:** a `proc_fds(p)` accessor returns the thread-group **leader's** fd table
for threads (own table for non-threads); all runtime fd accessors
(`fd_lookup`/`fd_alloc_into`/close/dup2/pipe/spawn-inherit) route through it, and
`sys_clone_thread` leaves the child's own `fds[]` **empty** (so its exit never
closes the shared descriptions). Then the *next* wall: chrome hit **`EMFILE`**
(`message_pump_epoll` `CHECK`) because `PROC_NFDS` was **16** — with threads now
correctly sharing one table, chrome+Mojo exhaust it. Raised `PROC_NFDS` 16→**1024**
(matching the reported `RLIMIT_NOFILE`).

**Verified — a giant leap:** `EXCEPTION 14` gone, EMFILE gone, chrome runs
**20,014 → 270,253 syscalls** — into real engine/rendering territory (fontconfig,
SwANGLE GL init, dozens of threads).

## Slice 6 — thread/proc table limit + init fills (DONE, verified)

- `PROC_MAX` 64→**256**: chrome spawns ~17+ threads (each a proc slot); 64 minus
  the boot procs ran out → `clone` ENOMEM → `pthread_create` EAGAIN → NULL thread
  handle. Raising it cleared the EAGAIN completely.
- Fills: `statx(332)` (real, 256-byte struct from `vfs_stat` — glibc prefers it
  over `newfstatat`), `time(201)`, `fallocate(285)` (accept), `inotify_add_watch
  (253)`/`rm_watch(254)` (fake watch descriptors so callers proceed).

**Verified:** `pthread_create` EAGAIN gone (0), the only remaining unhandled
syscall is `landlock(444)` (correctly `-ENOSYS`). Chrome runs its full early
platform bring-up — Mojo, `ThreadPool` (17 threads), fontconfig, and *gracefully*
degrades the platform bits tobyOS lacks (all non-fatal `ERROR`s, chrome falls
back): D-Bus connect (no named/`connect`-by-path AF_UNIX), `AF_NETLINK` socket,
`/proc/sys/fs/inotify/max_user_watches`, SwANGLE `eglInitialize` (no GL).

## Slice 7 — futex timeouts (`FUTEX_WAIT_BITSET`) (DONE, verified)

The int3 was symbolized (chrome's main PIE loads at `0x500000`; `objdump` at
`rip-0x500000` showed `int3; ud2` = `base::ImmediateCrash()`). It was **not** the
futex — but symbolizing led to the real futex bug: the kernel `futex()` handled
only `FUTEX_WAIT(0)`/`FUTEX_WAKE(1)` and **dropped the timeout arg (`a4`)**.
glibc's `pthread_cond_timedwait`/`mutex_timedlock` use **`FUTEX_WAIT_BITSET(9)`
with an absolute timeout** — which the old code returned `-EINVAL` for, so
chrome's timed waits **busy-looped on EINVAL** (34k syscalls in 7.5 s).

**Fix (`src/thread.c` + `proc.h` + call sites):** `futex()` now takes the
timeout pointer and handles `FUTEX_WAIT`/`FUTEX_WAIT_BITSET` (+ `WAKE`/
`WAKE_BITSET`). Untimed waits keep the efficient block+wake; **timed** waits
poll-with-idle (`hlt`) against a deadline (relative for `WAIT`, absolute for
`WAIT_BITSET` — and tobyOS's clocks are all `perf_now_ns`-based, so a chrome
absolute deadline is directly comparable). Returns `0`/`-EAGAIN`/`-ETIMEDOUT`/
`-EINTR`.

**Verified:** chrome's timed waits now block correctly instead of busy-looping;
it runs its **full concurrency stack** and now legitimately *waits* (1135
syscalls across **85 s** — mostly idle on real timeouts) rather than spinning.

## Slice 8 — the render/navigation wall: `IMMEDIATE_CRASH` (open, deep)

With concurrency correct, chrome waits ~85 s (a navigation/render timeout) then
hits `base::ImmediateCrash()` on the main thread — a message-less
`NOTREACHED`/`CHECK` deep in a stripped function (`posix_memalign+0x3d8xx`,
allocator/base). No DOM was dumped, so **navigation never completes**: the
in-process renderer can't finish a frame — SwANGLE `eglInitialize` failed (no
GL), so the compositor/paint path likely stalls, and a watchdog fires. Getting
past this is the **rendering-pipeline** tier (software compositing / GL fallback
/ whatever blocks the renderer) — genuinely M2-class work, not another syscall
fill. Next diagnostic step: stack-walk the main thread at the crash + trace what
the renderer thread is blocked on.

## Cumulative arc (this session, 8 slices)

Chrome went from *can't load `libpthread`* → **runs its full multi-threaded
engine**: glibc dynamic + V8 memory cage + Mojo IPC (AF_UNIX socketpair) + a
17-thread ThreadPool with correct fd sharing, fd/proc limits, and **working
futex timeouts** + fontconfig + graceful degradation of the platform bits tobyOS
lacks. It now reaches — and stalls in — the actual **render pipeline**. Several
fixes were real kernel bugs beyond chrome (int3→SIGTRAP, the demand-paging
editor-root, `CLONE_FILES` fd sharing, futex timeouts).

## Slice 2 — V8's virtual-memory cage (DONE, verified)

Chrome now int3s right after `socketpair(AF_UNIX, SOCK_SEQPACKET)` (syscall 53)
returns `-ENOSYS`. Chromium's **Mojo IPC** builds its message pipes on AF_UNIX
`socketpair`s; the `CHECK` on that failing is the current fatal wall. Also
non-fatal-but-worth-filling now that chrome reaches them: `gettimeofday(96)`,
`statfs(137)`/`fstatfs(138)`, `prctl` reads `PR_CAPBSET_READ(23)`/
`PR_GET_DUMPABLE(3)`.

The meat is **AF_UNIX domain sockets**: a bidirectional in-process channel (two
ring buffers, crossed) as a new `FILE_KIND`, wired into read/write/close/poll.
Watch for whether Mojo then needs **SCM_RIGHTS** (fd passing over the socket) —
`--single-process` may avoid most of it; measure before building it.

## Original analysis (superseded by slices above)

With `int3` fixed, chrome runs **557 syscalls / ~2.5 s CPU**, then the
`-DLINUX_SYSCALL_TRACE` firehose shows the last syscalls before the trap:

```
mmap(0x10000, 0x1000f0000 ≈ 4.0 GB, PROT_NONE, MAP_PRIVATE|ANON, -1)   ×2
getrandom ×2
mmap(0,       0x800000000 = 32 GB,  PROT_NONE, MAP_PRIVATE|ANON, -1)   ×2
→ int3  (CHECK failed: the reservation didn't succeed)
```

These are **V8's `VirtualMemoryCage` / PartitionAlloc address-space
reservations** — reserve a huge *virtual* region `PROT_NONE`, commit nothing,
then commit sub-ranges on demand. tobyOS fails them:

**Root cause (measured):** `src/mmap.c sys_mmap` **eager-commits every page** of
an anonymous mapping (`pmm_alloc_page()` per page, lines ~189-206). A 32 GB
`PROT_NONE` request = 8.3M page allocs → instant OOM (~3.5 GB RAM) → mmap fails
→ chrome `CHECK`-fails. `PROT_NONE` should reserve address space and commit
nothing.

**The fix is a real VM-subsystem slice, not a one-liner:**
1. Route the Linux **anonymous** mmap to the demand-paged `sys_mmap2` (it already
   reserves the VMA + registers it with the page-fault `vm_space` and commits
   nothing; the fault handler zero-fills on first touch and honours VMA write
   perms). Start with `prot == PROT_NONE` / large reservations to stay surgical.
2. Then handle how V8 *commits* sub-ranges of the reservation — and here the
   second gap bites: `sys_mprotect` updates only the legacy `g_vma_tables`, **not
   the `vm_space` VMAs the fault handler reads**, and does **no sub-range
   splitting**. So after V8 mprotects a slice of the cage to RW, a first-touch
   fault still sees `PROT_NONE` → SIGSEGV. Needs `vm_space` mprotect with VMA
   split. (If V8 instead commits via `MAP_FIXED` anon RW mmap, that path — small
   eager commit at a fixed addr — already works; the trace after fix 1 will show
   which.)
3. Possible follow-on: V8 may require the cage **base alignment** (e.g. aligned
   to its size for pointer-compression base masking); `find_free_region` returns
   only 4 KB-aligned bases. Re-measure after fix 1.

This is the "V8 is demanding on the VM subsystem" reality the roadmap flagged —
a bounded but multi-cycle slice. It is the current burn-down front.

**Reproduce the trace:**
`TRACE=1 bash logs/chromium-m0.sh` then
`grep -aE 'lxtrace\] chrome|EXCEPTION 3' logs/chromium-m0.log | tail`.

## Slice 9 — the "render wall" was SCHEDULER STARVATION, not GL (DONE, verified)

The slice-8 wall (chrome waits ~85s then `base::ImmediateCrash`, no DOM) was
handed down as a GL/SwANGLE problem. **Instrument-first disproved that and four
more hypotheses, each by measuring:**

- **GL is not the gate.** Added software-render boot flags
  (`--disable-gpu-compositing --enable-unsafe-swiftshader
  --run-all-compositor-stages-before-draw --virtual-time-budget=10000` in the
  `CHROMIUM_BOOT` harness) — the `eglInitialize SwANGLE failed` error vanished
  yet the crash was byte-identical. GL selection does not gate it.
- **Not demand-paging.** Surfaced the kernel's per-proc `fault_count` at the
  fatal-fault dump (`isr.c`): only ~170 faults over the whole run.
- **Not `sendmsg`/`recvmsg`-on-AF_UNIX, not AF_UNIX ring drops.** Instrumented
  both; zero hits — Mojo uses plain read/write on the socketpair.

**Real cause (measured with a `sched_tick` heartbeat that dumps every Linux
proc's scheduler state):** the browser **main thread sits `READY` at max
effective priority for the whole ~60s but is never scheduled**, while a pool
thread stays `RUNNING` continuously. That pool thread is in a **timed futex
wait** (`FUTEX_WAIT_BITSET` with a deadline — glibc `pthread_cond_timedwait` /
`sem_timedwait`) whose kernel loop (`src/thread.c`) did `hlt` **without ever
calling the scheduler**: it idles its CPU but never services the ready queue,
keeps `state == RUNNING` (never `PROC_BLOCKED`), and — since kernel-mode is not
preempted by `sched_tick` — monopolises "current", starving every runnable
peer. **A real kernel bug affecting any heavily-multithreaded Linux program.**

**Fix (`src/thread.c`):** add `sched_yield()` to the timed-futex poll loop so
`READY` peers run while it waits toward the deadline (mirrors the untimed path
above it, which already blocks correctly). **Verified: 77–85s hang → chrome
runs to ~4.3s** (an ~18x collapse); the heartbeat shows the main thread
scheduling normally.

**New permanent instruments (kept):** `isr.c` user-stack dump (raw-scans the
faulting stack, flags main-PIE-range qwords `[0x500000,0x0d000000)` as candidate
return addresses to symbolize with `objdump --start-address=<val-0x500000>`) +
`fault_count`/`last_fault_rip` in the terminate dump; the `#ifdef CHROMIUM_BOOT`
`sched.c` heartbeat (per-proc state/prio/io_boost/enq/eff/onq every ~3s).

## Slice 10 — VMA cap + ABI fills → chrome reaches the render pipeline (DONE)

With the freeze gone, chrome crashed fast (~4.3s) on an *invariant*
`ImmediateCrash` that three trial fixes (ftruncate, fcntl) did NOT move — the
tell was in the raw log: **`[mmap] WARN: VMA table FULL (256 entries)`**. The
per-proc VMA cap (`VMA_MAX_PER_PROC` in `src/mmap.c`) was **256**; chrome maps
~60 shared libraries (each a whole-file `MAP_PRIVATE` reservation + several
`MAP_FIXED` segment maps) + V8's cage + ~17 thread stacks + shm — well over 256.
`vma_alloc` failed → `mmap` `-ENOMEM` → chrome CHECK-crashed. **The entire
"shared-memory" investigation was a red herring; the invariant crash was always
an mmap failing on a full VMA table.**

- **`src/mmap.c`: `VMA_MAX_PER_PROC` 256 → 4096** (file-local struct, ~40 B/entry
  x PROC_MAX ≈ 42 MiB BSS; Linux default `vm.max_map_count` is 65530). Follow-up
  noted: `MAP_FIXED` should *replace* overlapped VMAs (ld.so's segment maps each
  add an entry today) + the table could be per-proc heap-allocated.
- **ABI fills landed alongside** (real bugs regardless): `fcntl(F_GETFL)` returns
  the fd's access mode (new `struct file.o_accmode`, set in `sys_open`, copied by
  `file_clone`) instead of a blanket 0 — chrome's shm `CheckPlatformHandle...`
  reads it; real `ftruncate` (records `f->vfs.size`, was a no-op); `clock_nanosleep`
  (routes to `nanosleep`, honours `TIMER_ABSTIME`).

**Result: chrome runs 4.3s → ~7–9s, into the render/GL pipeline** (SwANGLE
`eglInitialize` fails: SwiftShader Vulkan "requested extension not supported",
non-fatal). New gaps reached: `rename(82)` still `-ENOSYS`.

## Slice 11 — render pipeline: NULL GL-dispatch call (OPEN, current front)

Chrome now dies at ~7–9s with **`EXCEPTION 14` at `rip=0` (err=0x14, user
instruction-fetch)** — a **call through a NULL function pointer** on the main
thread, from inside a shared library (return addr `~0x100000b02a80` in the .so
map region), ~3–5s *after* the non-fatal SwANGLE GL-init errors. Almost certainly
a GL/EGL dispatch pointer left NULL by the failed `eglInitialize`. This is the
render-pipeline / GL tier (M2-class). Next: identify the .so (correlate the
caller addr with ld.so library load addresses) + what pointer is NULL → decide
shim vs. making SwANGLE actually initialize. `rename` also wants filling.

### Slice 11 GL deep-dive (findings; NOT cracked — the hard tier)

Multiple instrument-first experiments pinned the render wall precisely; the fix
is genuine ANGLE-internals work, not config. Findings (all reverted to committed
flags — none dumped the DOM):

- **ANGLE (`libGLESv2.so`) dlopens SwiftShader's ICD (`libvk_swiftshader.so`)
  DIRECTLY** (its DT_NEEDED is only libc/libpthread/libgcc_s/ld — Vulkan is
  dlopen'd at runtime), **bypassing `libvulkan.so.1` — the loader that implements
  the WSI instance extensions.** So `VK_KHR_surface` + `VK_KHR_xcb_surface` (which
  ANGLE's `vk_renderer.cpp` `VerifyExtensionsPresent` REQUIRES) are absent →
  `eglInitialize` fails → a NULL GL dispatch is called downstream.
- **`--use-angle=vulkan` + env `VK_ICD_FILENAMES=/opt/chrome/vk_swiftshader_icd.json`
  routes ANGLE through the real loader → `VK_KHR_surface` RESOLVES** (progress!).
  Only `VK_KHR_xcb_surface` then remains.
- The bundled `libvulkan.so.1` IS built with xcb WSI and `libxcb.so.1`
  (+ its deps `libXau.so.6`, `libXdmcp.so.6`) are in the sysroot, **but the loader
  correctly FILTERS OUT `VK_KHR_xcb_surface` because there is no functional X
  server** (`xcb_connect` has no `DISPLAY`). ANGLE hard-requires it anyway.
- **`VK_LOADER_DISABLE_INST_EXT_FILTER=1` REGRESSED it** (both surface extensions
  failed again) — not the override it appeared to be.

**Conclusion:** the real fix is making ANGLE use a SURFACELESS/headless Vulkan
path (no X11 window surface) — not reachable via the chrome flags / loader env
tried (`--ozone-platform=headless`, `--disable-features=Vulkan`,
`--use-angle=vulkan`, the loader filter override). Realistic next options, all
substantial: (a) a minimal headless X server / `xcb_connect` shim so the loader
advertises `VK_KHR_xcb_surface`; (b) patch/configure ANGLE for
`VK_EXT_headless_surface` / surfaceless; (c) provide a headless Vulkan ICD that
advertises what ANGLE demands. **Track B kernel ABI is proven sufficient — this
is entirely chrome's own GL stack in a headless environment.**

## Slice 12 — the extension wall was a tobyOS path bug; wall moves to `xcb_connect` (DONE)

The slice-11 deep-dive concluded `VK_KHR_xcb_surface` was "filtered out because
there is no X server" and that the fix needed a surfaceless ANGLE / headless
Vulkan ICD. **Two instrument-first measurements disproved that and found the real
cause, which is much smaller.** (Both were free — static analysis of the bundled
`.so`s + a `VK_LOADER_DEBUG=all` boot.)

- **`--use-angle=null` does NOT dodge GL** (two takes). With `--disable-gpu` the
  null backend is ignored (chrome forces SwANGLE, "all (1) EGL display types
  failed"); dropping `--disable-gpu` gets null accepted but as "0 EGL display
  types" — either way the **same NULL GL-dispatch crash** fires. Chrome calls a
  NULL dispatch whenever GL init yields no usable display, independent of *why*.
- **The loader does NOT probe an X server.** `libvulkan.so.1`'s `DT_NEEDED` is
  only libc/libdl/libgcc_s/libpthread — it never links or calls libxcb; it *does*
  implement `vkCreateXcbSurfaceKHR` + `vkGetPhysicalDeviceXcbPresentationSupportKHR`
  (built with xcb WSI). So its `VK_KHR_xcb_surface` advertising is compile-time and
  unconditional — the "filtered because no DISPLAY" theory was wrong. This chrome's
  ANGLE compiles only `DisplayVkXcb` + `DisplayVkWayland` (no surfaceless class),
  so ANGLE genuinely requires an X11 (or wayland) WSI — there is no headless ANGLE
  path to configure.
- **`VK_LOADER_DEBUG=all` caught the real bug.** Routed through the loader
  (`--use-angle=vulkan` + `VK_ICD_FILENAMES`), the loader found the ICD manifest
  but then: `Searching for ICD drivers named ./libvk_swiftshader.so` →
  `/opt/chrome/./libvk_swiftshader.so: cannot open shared object file: No such
  file` → `vkCreateInstance: Found no drivers!`. The file **exists** (ANGLE loads
  it directly in the default run); the interior `.` component in the loader-built
  path was the problem. **`src/syscall.c resolve_user_path` copied absolute paths
  verbatim and only collapsed *leading* dot-components of relative paths** —
  interior `/./`, `/../`, and `//` leaked straight to the fs layer, whose lookups
  are a `strcmp` against normalized entry names (ramfs) or reject `.`/`..` outright
  (tobyfs). A real ABI gap any Linux program can hit.

**Fix (`src/syscall.c`): a `path_lexical_clean()` pass** (collapse `//`, drop
`.`, resolve `..` by popping a component, never above root) applied to the final
absolute path for every syscall that takes a path. Host-unit-tested across the
tricky cases (`/a/b/../../../c`→`/c`, `/..`→`/`, dotted filenames like `/....`
and `/a/..b/c` preserved). It subsumes the old leading-`./` special case.

**Result (measured, `bash logs/chromium-m0.sh`):** the ICD now loads, the loader
enumerates **"SwiftShader Device (Subzero)"**, `vkCreateInstance` succeeds, and
**every `VerifyExtensionsPresent` / `VK_KHR_xcb_surface` error is GONE (0 hits)** —
the entire Vulkan instance + extension wall is passed. Chrome now advances ~1s
further and dies at the *next*, precisely-identified wall:

```
DisplayVkXcb.cpp:62 (initialize): xcb_connect() failed, error 1
Display.cpp:1128 (initialize): ANGLE Display::initialize error 0: Not initialized.
eglInitialize Vulkan failed with error EGL_NOT_INITIALIZED
```

`xcb_connect()` (XCB_CONN_ERROR) fails because there is no X server at `DISPLAY=:0`
and tobyOS AF_UNIX is **socketpair-only** (no named/abstract bind/connect). The
same gap makes D-Bus fail earlier (`Failed to open socket: Invalid argument`).
The harness now commits the loader route (`--use-gl=angle --use-angle=vulkan
--enable-unsafe-swiftshader` + `VK_ICD_FILENAMES` + `DISPLAY=:0`).

## Slice 13 — minimal fake X server over named AF_UNIX (OPEN, current front)

The remaining blocker is now concrete and NOT the "surfaceless ANGLE / headless
ICD" the slice-11 deep-dive imagined: ANGLE's `DisplayVkXcb::initialize` needs a
live `xcb_connect()`. Plan: (1) named/abstract AF_UNIX (`bind`/`listen`/`accept`/
`connect` on `sockaddr_un`, reusing `src/socket.c`'s `SOCK_KIND_UNIX` rings) —
this also unblocks D-Bus; (2) a minimal fake X server answering the X11
connection-setup handshake (`xConnClientPrefix` → `xConnSetupSuccess` with ≥1
screen/depth/visual) at `/tmp/.X11-unix/X0` so `xcb_connect` succeeds. Chrome is
headless (`--dump-dom`), so it never opens a real window — the handshake + a
couple of local setup queries should suffice. Then `--dump-dom` should print
`<h1>tobyOS</h1>`; then switch to `--screenshot` and host-diff.
