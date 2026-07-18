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

## Slice 2 — the next measured wall: **V8's virtual-memory cage** (open)

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
