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

## Slice 5 — the remaining wall: a thread NULL-deref (open, needs deep diag)

Chrome now crashes on a genuine **NULL dereference** (`EXCEPTION 14`,
`cr2=0x0`, user-mode write) in one of its worker threads — *not* a missing
syscall. This needs the `TRACE=1` firehose + `rip` symbolization to localize.
Leading hypotheses, in order:
- **`SCM_RIGHTS`** — Mojo passes fds over the socketpair via `sendmsg`/`recvmsg`
  ancillary data; the current `lx_sendmsg`/`lx_recvmsg` don't handle
  `SOCK_KIND_UNIX` or ancillary, so an fd handoff could yield a NULL handle →
  deref. (First thing to check in the trace: `sendmsg`/`recvmsg` on the UNIX
  fds before the fault.)
- A syscall returning a shape chrome doesn't expect (e.g. one of the new fills'
  struct layouts), leaving a NULL where chrome assumes non-NULL.
- A genuinely missing platform surface (a `/proc`, `/sys`, or `/dev` node chrome
  opens and then derefs the result of).

This is the current burn-down front and the natural next slice.

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
