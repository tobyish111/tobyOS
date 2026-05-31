# tobyOS vs Windows 10 — Living Gap Analysis

**Purpose:** a repo-tracked, updatable assessment of how far tobyOS is from Windows 10
functionality. Originally a Cursor "canvas" (outside the git tree); moved here so it
survives clones and any agent can read/update it.

**How to use / update this file**
- Numbers are *feature-coverage* parity estimates (not benchmarks), per subsystem, vs Win10.
- "Wired" means the feature initializes at boot and is reachable — verified against
  `src/kernel.c` call sites and boot logs — **not** that it is Windows-10-robust under load.
- When you change a subsystem, update its row, bump the date, and add a line to the Changelog.
- Be honest: "code present" ≠ "wired" ≠ "robust". Distinguish them.

**Last updated:** 2026-05-30

---

## Bottom line

**~25% overall Win10 feature parity** (canvas baseline was ~16%). The 2026-05-30 work
added the security-depth bundle (Argon2id auth, login lockout, real signal delivery, SMAP
re-enabled behind a uaccess window), an app-compatibility bump (SSE/FP enabled with
FXSAVE context switching, `%f`/`%e`/`%g` printf, a 256 KiB user stack, and the marquee
ports — including a working Lua interpreter — actually shipping in the initrd), and the
per-CPU multi-core foundation (per-CPU TSS / GS-base SYSCALL stack / current_proc; the
round-robin flip is deferred).

The percentage understates the real milestone: tobyOS now **boots to the desktop and gets
on the network on real hardware** (HP EliteDesk 800, Intel CPU). The canvas scored a thing
that only ran in QEMU. It is now a real, bootable, networked OS — a qualitative jump even
though the parity bar barely moves.

A prior estimate of "30–35%" was optimistic: it counted code that is present but either
doesn't help real hardware (GPU compositing), is shallow (djb2 passwords, link-local-only
IPv6), or adds no capability (SMP that still won't run apps on other cores). ~22% is the
honest number.

---

## Parity by subsystem

| Subsystem | Canvas | Now | Notes |
|---|---|---|---|
| Scheduler / SMP | ~12% | ~15% | **Per-CPU multi-core foundation now built & tested**: per-CPU TSS, per-CPU SYSCALL stack via GS base (no swapgs), per-CPU `current_proc`, AP CR0/CR4/EFER (SSE/SMEP/SMAP/NX) parity. APs still idle — the round-robin "flip" is deliberately deferred: it needs a syscall-path big-kernel-lock (or full per-subsystem locking) + per-CPU idle contexts + AP pop-and-switch, which can't be safely validated headlessly. heap+PMM+sched-queues are already spinlocked; VFS/GUI/proc-table are not. Preemptive via PIT 1 kHz. No priority classes/MLFQ. |
| Memory | ~18% | ~24% | `swap_init` wired; **real CoW fork** (`vmm_cow_fork` + `mmap_cow_clone`, no longer eager-copy); demand paging live; ASLR/NX/SMEP. Still bitmap PMM, first-fit heap, no memory compression / large pages. |
| Filesystem / Storage | ~20% | ~27% | TobyFS: **journaling** (replay on mount), **indirect blocks** (max file 64 KiB → ~4 MiB), **256-entry write-back buffer cache**. VFS over ramfs/TobyFS/FAT32/ext2(rw)/ext4(ro)/proc/sys/cryptfs. Still no NTFS-class streams/ACLs; 4 MiB ≪ 16 TB. |
| Networking | ~22% | ~30% | **CUBIC** + window scaling, IPv6 link-local + ICMPv6/ND, TLS 1.3, HTTP/2 (early). **DHCP/TCP working on real hardware over a VLAN-tagged LAN.** Small conn tables, link-local-only v6, no offloads. Strongest area. |
| Device Drivers | ~12% | ~14% | **Loadable `ET_REL` kernel module loader** (foundational). 6 NIC drivers, AHCI/NVMe/IDE/virtio-blk, xHCI/EHCI/HID/MSC, HDA. No real GPU driver; no signed third-party ecosystem. |
| GUI / Desktop | ~10% | ~12% | GPU-accelerated compositor path exists **but only active with VirtIO-GPU**; on real Intel iGPU it falls back to the CPU/Limine compositor. Now **usable on hardware** (login no longer flaps; mouse/keyboard work). |
| Security | ~8% | ~15% | Login auth now **salted Argon2id** (monocypher, 16-byte random salt, m=1 MiB/t=3, constant-time compare); legacy djb2 hashes self-upgrade on next login. **Login lockout** (5 fails → 30 s) blunts brute force/enumeration. **Real user-space signal delivery** works (kernel pushes a signal frame + sigreturn restores context; verified by `/bin/sigtest`). **SMAP re-enabled** behind a syscall-wide stac/clac uaccess window + wrapped loader/argv; validated under QEMU `+smap` (full desktop + sigtest, no #PF). ASLR/NX/SMEP on. Caps + sandbox + HMAC package signing. |
| Power / ACPI | ~6% | ~9% | **RTC driver** → real wall-clock time. ACPI shutdown + partial S3/S4 framework. No full AML power management. |
| Audio / Media | ~15% | ~15% | Intel HDA + software mixer + decode helpers. Unchanged this round. |
| App Compatibility | ~2% | ~5% | POSIX libc filled in (`signal.h`, `fork`, `symlink`/`readlink`, real `getuid/gid`, `wait`, `access`). **SSE/FP now enabled** (CR0/CR4 + FXSAVE/FXRSTOR FPU context switch), so floating-point programs run; **libc printf gained `%f`/`%e`/`%g`**; user stack 32 KiB→256 KiB. Marquee ports (lua/make/less/curl/tcc/as) **now actually ship in the initrd** (were built but omitted from the tar). **Lua interpreter runs real scripts** (`/bin/lua`, verified by `/etc/lua_selftest.lua`). Still own-ELF-only; **zero** Win32/.NET/UWP. |

---

## What's been done

**Tier 1 (wired & verified live):** swap init; CoW fork; SMP AP work-migration; TobyFS
indirect blocks; password auth; RTC driver.

**Tier 2 (wired & verified live):** block buffer cache; CUBIC + window scaling; IPv6
dual-stack (link-local); TobyFS journaling; GPU compositor (VirtIO only); loadable kernel
driver model; POSIX libc surface.

**Real-hardware bring-up (this is what made it actually run):**
- **SMAP** — was disabled because it was enabled with no `stac`/`clac` uaccess wrappers,
  so the first kernel access to user memory (`CR2=0x400000`, the ELF load base) #PF'd on
  real Skylake while QEMU (no SMAP) booted fine (see `memory/realhw-smap-divergence`).
  **Now re-enabled** (2026-05-30): the SYSCALL trampoline brackets the whole syscall body
  in stac/clac, and the out-of-syscall user writers (ELF loader, argv/envp packer) use
  `uaccess_begin/end`; both gated on `g_smap_on` so non-SMAP CPUs are unaffected. Verified
  by booting the full desktop and `/bin/sigtest` under `qemu64,+smep,+smap`.
- **GUI event-type ABI fix** — kernel `GUI_EV_MOUSE_MOVE=1` collided with a stale
  `EV_CLOSE=1` in `user_login` + 8 other apps, so moving the mouse "closed" the window;
  the login service then flapped login↔desktop until disabled. `/bin/login` is built from
  `programs/user_login/`, not `programs/login/`.
- **VLAN networking** — the router answers DHCP on 802.1Q VID 591; `eth_recv` de-tags
  correctly and DHCP now completes on the real LAN.

---

## Remaining gap to Win10 (impact order)

1. **Drivers** — no real GPU/WiFi/BT/print; storage/NIC breadth limited. The bulk of Win10
   and the hardest. Loadable-module infra exists but no driver ecosystem.
2. **App compatibility (~3%)** — no Win32/POSIX-compat runtime → no software ecosystem.
3. **Real multi-core execution** — the per-CPU *foundation* is built and committed
   (per-CPU TSS, GS-base SYSCALL stack, per-CPU current_proc, AP hardening parity).
   A full flip (syscall-path BKL + per-CPU idle procs + AP pop-and-switch + work-steal)
   was **prototyped and proved 2.62x on 4 CPU-bound workers** — APs genuinely run user
   code in parallel — but it was **reverted** because it **deadlocks the desktop**: the
   GUI relies on the cooperative model where pid 0 continuously runs `gui_tick`, and the
   new scheduling parks the BSP on a blocked login proc so input/compositor events never
   get generated (pid 0 ↔ login deadlock). It also exposed a non-deterministic
   `current_proc()` inconsistency for the pid-0 kernel thread (proc_spawn/proc_wait racing
   the scheduler → `ppid` wrong → proc_wait hangs). To land it: give pid 0 a proper
   per-CPU idle separate from the GUI driver (or drive `gui_tick` from a dedicated kernel
   thread/IRQ), and make kernel-thread proc management hold the BKL. Needs real-HW-validated
   testing — deadlock/race modes don't surface reliably in short headless boots.
4. **GPU-accelerated desktop on real hardware** — compositor accel only exists for VirtIO,
   not the Intel iGPU path used on the EliteDesk.
5. **Security depth** — ~~salted/KDF auth~~ (Argon2id), ~~login rate-limiting~~ (lockout),
   ~~signal *delivery*~~ (frame push + sigreturn), ~~re-enable SMAP~~ (syscall-wide uaccess
   window). This bundle is now largely done. Remaining: per-copy uaccess accessors (the
   window is coarser than Linux's per-`copy_*_user`), SA_RESTART, job control/stop-cont.

---

## Known shallow / "present but not robust" items
- SMP: cores boot, run the kernel idle loop + take LAPIC timer IRQs, and now have the full
  per-CPU plumbing to run user code — but the enqueue is still BSP-pinned, so user code
  does not yet run on APs (the concurrency flip is deferred; see gap item #3).
- IPv6: link-local only — no SLAAC/DHCPv6/global addressing.
- Passwords: now salted Argon2id (was djb2). Still no account lockout / rate-limiting,
  no password policy, and no PAM-style pluggable auth.
- GPU accel: VirtIO-only; real-HW desktop is CPU-composited.
- SMAP: enabled, but via a coarse syscall-wide stac/clac window rather than per-copy
  accessors, so a stray user-pointer deref *inside* a syscall isn't caught (only outside).
- Signals: user-handler delivery now works (frame push + sigreturn, mask/pending
  honored). Still missing: SA_RESTART syscall restart, job-control stop/cont,
  SA_SIGINFO/siginfo_t, and handler delivery to a pure-CPU-bound process is
  deferred to its next syscall (timer-IRQ path doesn't push frames).
- TobyFS journaling/swap/CoW: wired but not stress-tested under crash/pressure/load.

---

## Changelog
- **2026-05-30** — Initial repo-tracked version. Re-scored after Tier 1/2 wiring + real-HW
  bring-up (SMAP, GUI event ABI, VLAN DHCP). Overall ~16% → ~22%.
- **2026-05-30** — Login password auth moved from unsalted djb2 to salted **Argon2id**
  (`src/users.c`, via the already-linked monocypher; random salt from `rng_fill`,
  constant-time `crypto_verify32`). Legacy djb2 digests still verify and self-upgrade on
  next login. On-disk credential format is now
  `$argon2id$m=<kib>,t=<passes>$<salt_hex>$<hash_hex>`; `struct user.password_hash`
  widened 65→128. Builds + boots clean (login still reached, no #PF). Security ~9% → ~11%.
- **2026-05-30** — **Login lockout** (`src/session.c`): per-username failed-attempt
  throttle, 5 fails → 30 s lockout, unknown users counted too (blunts enumeration),
  monotonic PIT clock, LRU-evicted fixed table.
- **2026-05-30** — **Real signal delivery** (`src/signal.c` + `syscall_entry.S` layout
  mirror): on the syscall-return path the kernel now pushes a signal frame onto the user
  stack (saving the full GP context + mask), redirects the SYSRETQ into the handler with
  RDI=signum, and `sys_sigreturn` restores the context. Added `SYS_SIGRESTORER` (164) so
  libc registers a sigreturn trampoline; fixed the sigaction/sigprocmask ABI marshalling
  (libtoby's 64-bit `sigset_t` vs the kernel's 32-bit struct meant sa_flags was read from
  the wrong offset); `fork` now inherits dispositions + clears pending (POSIX). Verified by
  `/bin/sigtest` (build `EXTRA_CFLAGS+=-DSIGTEST_BOOT`): handler runs, context preserved,
  blocked-signal pending/unblock all PASS, no #PF. Security ~11% → ~13%.
- **2026-05-30** — **SMAP re-enabled** (`include/tobyos/uaccess.h`, `hardening.c`,
  `syscall_entry.S`, `elf.c`, `proc.c`). The SYSCALL trampoline opens a stac/clac uaccess
  window around the whole syscall body (survives blocking — proc_switch preserves RFLAGS);
  the ELF loader and argv/envp packer use `uaccess_begin/end` (save+restore AC, so they
  nest inside execve's window); the COW copier already uses HHDM (kernel) addresses so
  needs none. All stac/clac gated on `g_smap_on`, set only after CR4.SMAP goes live, so
  non-SMAP CPUs (default QEMU) run the identical old path — confirmed booting clean with
  `[hardening] SMAP not available`. Under `qemu64,+smep,+smap` the full GUI desktop runs
  19 s with no #PF and `/bin/sigtest` passes. Security ~13% → ~15%. This is the fix the
  real-Skylake `CR2=0x400000` #PF was waiting on.
- **2026-05-30** — **Multi-core foundation** (`smp`/`tss`/`sched`/`proc`/`syscall_entry.S`):
  per-CPU TSS (one TSS/CPU, GDT slot reused at LTR), per-CPU SYSCALL stack via GS base
  (gs:[0]/gs:[8], no swapgs — proc_switch no longer reloads the GS selector), per-CPU
  `current_proc()`, and AP CR0/CR4/EFER parity (SSE+SMEP+SMAP+NX) in `hardening_init_ap`.
  Enqueue still BSP-pinned, so behaviour is unchanged (APs idle) — verified: -smp 4 boots
  clean, 4 CPUs online, per-CPU TSS installed, desktop runs, no faults. The round-robin
  flip (needs a syscall BKL + per-CPU idle contexts + AP pop-switch) is deferred for
  dedicated, real-HW-validated work. Scheduler/SMP ~13% → ~15%.
- **2026-05-30** — App-compat: **SSE/floating-point enabled** (`hardening.c` sets CR0.EM=0/
  MP, CR4.OSFXSR|OSXMMEXCPT) with per-process **FXSAVE/FXRSTOR** context switching
  (`cpu.h`, `proc` `fpu_state[512]`, `sched.c`, first-entry restores in proc.c/fork.c/
  thread.c) so FP programs run safely alongside others. **libc printf gained `%f`/`%e`/`%g`**
  (`libtoby/src/stdio.c`, built `-msse`). Default **user stack 32 KiB→256 KiB** (proc.c).
  The B3 marquee ports (lua/make/less/curl/tcc/as) were built+staged but **missing from the
  initrd tar** — now shipped. Fixed three latent bugs in `/bin/lua` (never ran before: it
  wasn't shipped): a ~3 MB `compiler` stack local (→ static), an unbacked `vm.protos` NULL
  pointer, and unsupported `t[k]=v` indexed assignment; added `string.format/rep/upper/
  lower`, `math.max/min/ceil`, `io.write`. Verified by `/etc/lua_selftest.lua` via
  `/bin/lua` (EXTRA_CFLAGS+=-DLUATEST_BOOT): ALL OK on default and `+smap` CPUs; desktop
  still boots clean. App Compatibility ~3% → ~5%; overall ~23% → ~24%.
