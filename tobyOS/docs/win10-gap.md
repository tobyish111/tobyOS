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

**Last updated:** 2026-06-08 (priority scheduling + fair per-AP timeslicing)

---

## Bottom line

**~26% overall Win10 feature parity** (canvas baseline was ~16%). The 2026-05-30 work
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
| Scheduler / SMP | ~12% | ~23% | **APs run user code in parallel AND the dispatcher is now priority-aware + preemptively timesliced; the SMP desktop-freeze livelock is root-caused & fixed.** Per-CPU TSS + SYSCALL stack (GS base, no swapgs) + `current_proc` + AP CR0/CR4/EFER parity + per-CPU SYSCALL MSRs; a syscall-path **big-kernel-lock** serializes kernel entry (and pid 0's `gui_tick`/service work) so user code parallelizes while VFS/GUI/proc-table stay race-free; per-CPU **idle procs** + **work-stealing** distribute work; AP-run gated until boot completes. Measured **~2.6-2.7x on 4 CPU-bound workers**. **SMP desktop-freeze livelock fixed (2026-06-11):** the BKL is a fair ticket lock, pid 0's idle_loop holds it per-phase (not across the whole iteration), nanosleep/tcp waits drop it, and login no longer busy-polls; AP bringup retries missed SIPIs (`started`-flag-gated re-INIT). **Priority classes** (IDLE/LOW/NORMAL/HIGH/RT, `prio` HIGHER-runs-first, 0==NORMAL default; `SYS_SETPRIORITY`/`GETPRIORITY` + `toby_setprio` libc, uid/root policy) drive a highest-effective-priority dequeue with **FIFO-within-level** and **aging anti-starvation**. **Fair per-AP timeslicing:** the per-CPU LAPIC timer now preempts ring-3 user code whose class quantum is spent (only the BSP had the PIT before), so a CPU-bound proc stolen onto an AP no longer monopolises that core. Validated (`-DSCHEDPRIO_BOOT`): 3 HIGH vs 3 LOW workers on 4 cores → HIGH got **~18x** the CPU of LOW yet **every LOW proc still ran** (no starvation). GUI desktop stable (default and `+smap`). **Known issue:** an `argc>=1` first-run proc on an AP **under SMAP** SMEP-faults (`argc=0` clean) — real-HW only, needs HW debugging; non-SMAP unaffected. Still a coarse 5-class scheme, not full MLFQ / load-balancing. |
| Memory | ~18% | ~26% | `swap_init` wired; **real CoW fork** (`vmm_cow_fork` + `mmap_cow_clone`) — **actually functional as of 2026-06-12** (it had 4 latent bugs: child restarted at _start, no TLB flush after write-protect, refcounts off by one enabling shared writable pages, non-ref-aware teardown freeing live pages; see changelog); demand paging live; ASLR/NX/SMEP. **Memory compression (zram/zswap-style, 2026-06-13):** a dependency-free LZ4 codec (`src/lz4.c`) + a compressed in-RAM page store (`src/zram.c`) sit in front of swap — `swap_out` LZ4-compresses an evicted page and keeps it in RAM if it shrinks (so eviction needs no disk), only spilling incompressible pages to the disk swap area; proven by a crash-free self-test (zeros 157x / repeat 70x / mixed-batch 3.3x, 50 frames reclaimed from 72 pages, all loads bit-exact). **2 MiB large pages (2026-06-13):** `pmm_alloc_2m()` hands out 2 MiB-aligned huge frames, the VMM maps/unmaps them as single PD leaves (`vmm_leaf_size()` + huge-aware `vmm_unmap`), and the kernel heap transparently backs any allocation >= 2 MiB with huge pages (one PD leaf per 2 MiB vs 512 PTEs + a PT page), best-effort with 4 KiB fallback on fragmentation. Engages on every boot (the compositor backbuffer is a ~4 MiB huge arena); proven by self-test (huge-leaf map/translate/RW/unmap + huge-backed kmalloc). Still bitmap PMM (no buddy/slab) + first-fit heap; no demand-paged hugepages for user mmap yet. |
| Filesystem / Storage | ~20% | ~31% | TobyFS: **journaling** (replay on mount, now **crash-consistency + stress proven** — a power-loss harness injects a crash before/after the journal commit and proves remount is atomic rollback/replay, plus an ~80-op churn + double-indirect 4.5 MiB stress run with byte verification and the integrity checker throughout, 2026-06-13), **direct + single + double-indirect blocks** (max file now ~4 GiB, was ~4 MiB), **dynamic device-sized volumes** (format scales geometry + multi-block bitmaps to the disk, up to 1 GiB; legacy 4 MiB images still mount), **256-entry write-back buffer cache**. Fixed a latent journal intra-transaction read-after-write bug (was silently corrupting freshly-created inodes sharing a parent's inode-table block). VFS over ramfs/TobyFS/FAT32/ext2(rw)/ext4(rw)/proc/sys/cryptfs. **NVMe now handles 4K-LBA namespaces** (512-byte logical view + RMW for sub-sector access). **Block I/O is now async with real command queuing** — NVMe keeps multiple SQ commands in flight (CID-tagged) and AHCI uses NCQ (FPDMA QUEUED, up to 32 tags); a cooperative-yield wait lets concurrent submitters keep the hardware queue full. **ext4 is read-write WITH a JBD2 write-ahead journal** (2026-06-13): create/unlink/mkdir/write each run as a transaction logged as [descriptor][data][commit] then checkpointed, so writes are **crash-atomic**; mount now **recovers** (replays committed transactions) a needs-recovery image whose journal we can drive, instead of refusing it. Proven by a crash-injection self-test (power loss before/after commit / mid-checkpoint → rollback / replay / heal). Classic JBD2 format only (no metadata_csum/64bit/async-commit); not yet validated against a real Linux dirty image. Still no NTFS-class streams/ACLs. |
| Networking | ~22% | ~32% | **CUBIC** + window scaling, IPv6 link-local + **SLAAC global addressing** (RS/RA, prefix autoconf, default router) + **stateful DHCPv6** (RA M/O flags trigger a SOLICIT+rapid-commit / 4-message client; IA_NA/IAADDR address + DNS-server extraction; UDP-over-IPv6 demux on :546) + ICMPv6/ND, TLS 1.3, HTTP/2 (early). **DHCP/TCP working on real hardware over a VLAN-tagged LAN.** Small conn tables, no general UDP/TCP-over-IPv6 sockets yet (only the in-kernel DHCPv6 port), no offloads. Strongest area. |
| Device Drivers | ~12% | ~14% | **Loadable `ET_REL` kernel module loader** (foundational). 6 NIC drivers, AHCI/NVMe/IDE/virtio-blk, xHCI/EHCI/HID/MSC, HDA. No real GPU driver; no signed third-party ecosystem. |
| GUI / Desktop | ~10% | ~12% | GPU-accelerated compositor path exists **but only active with VirtIO-GPU**; on real Intel iGPU it falls back to the CPU/Limine compositor. Now **usable on hardware** (login no longer flaps; mouse/keyboard work). |
| Security | ~8% | ~19% | **SA_SIGINFO 3-arg handlers** (siginfo_t + ucontext on the user stack) and **TTY job-control keys** (Ctrl-Z->SIGTSTP / Ctrl-\->SIGQUIT, completing Ctrl-C->SIGINT). **Per-copy uaccess accessors** (Linux-style `copy_*_user`; the whole-syscall SMAP window is gone, so a stray kernel user-deref now faults under SMAP) -- validated end-to-end under hardware `+smap`. **SA_RESTART + job control (SIGSTOP/SIGCONT, PROC_STOPPED) now work**, signals reach CPU-bound procs on APs (LAPIC-tick delivery), and the SA_* flag ABI is Linux-aligned. Login auth now **salted Argon2id** (monocypher, 16-byte random salt, m=1 MiB/t=3, constant-time compare); legacy djb2 hashes self-upgrade on next login. **Login lockout** (5 fails → 30 s) blunts brute force/enumeration. **Real user-space signal delivery** works (kernel pushes a signal frame + sigreturn restores context; verified by `/bin/sigtest`). **SMAP re-enabled** behind a syscall-wide stac/clac uaccess window + wrapped loader/argv; validated under QEMU `+smap` (full desktop + sigtest, no #PF). ASLR/NX/SMEP on. Caps + sandbox + HMAC package signing. |
| Power / ACPI | ~6% | ~9% | **RTC driver** → real wall-clock time. ACPI shutdown + partial S3/S4 framework. No full AML power management. |
| Audio / Media | ~15% | ~15% | Intel HDA + software mixer + decode helpers. Unchanged this round. |
| App Compatibility | ~2% | ~53% | POSIX libc filled in (`signal.h`, `fork`, `symlink`/`readlink`, real `getuid/gid`, `wait`, `access`). **SSE/FP now enabled** (CR0/CR4 + FXSAVE/FXRSTOR FPU context switch), so floating-point programs run; **libc printf gained `%f`/`%e`/`%g`**; user stack 32 KiB→256 KiB. Marquee ports (lua/make/less/curl/tcc/as) **now actually ship in the initrd** (were built but omitted from the tar). **Lua interpreter runs real scripts** (`/bin/lua`, verified by `/etc/lua_selftest.lua`). **Foreign-binary compat (Track B, milestone B1, 2026-06-14): tobyOS now runs an UNMODIFIED Linux x86-64 binary.** A per-process ABI **personality** (`struct proc.personality`) is latched at ELF load from `e_ident[EI_OSABI]` — a binary branded `ELFOSABI_LINUX` (the standard FreeBSD-style `brandelf` tag; the binary's code is byte-for-byte untouched) runs under `ABI_PERS_LINUX`, and `syscall_dispatch` routes its `syscall`s through a new **Linux→tobyOS translation layer** (`linux_syscall()` in `src/syscall.c`). This leverages infra that was already Linux-shaped: the SYSCALL entry path already uses the exact Linux register ABI (`rax`=nr, args `rdi/rsi/rdx/r10/r8/r9`), the initial user stack is already Linux-auxv-shaped, and TLS is already via `MSR_FS_BASE`. Proven by `/bin/linux-hello` — a genuine Linux static ELF (raw Linux syscalls, no libc) that the kernel runs through libc-style startup: **`arch_prctl(ARCH_SET_FS)` TLS (verified via `%fs:0`), `write`+`writev` to stdout, `exit_group(42)`** — `[LXABI] VERDICT: PASS` clean under default boot AND hardware `+smep+smap` (0 faults). Translated set covers the common static-binary surface (read/write/writev/readv, open/close/lseek, mmap/munmap/mprotect/brk, arch_prctl, set_tid_address, ioctl→ENOTTY, nanosleep/clock_gettime, uname, getpid/uid/gid…); unimplemented numbers log `[linux] unhandled syscall N` + return `-ENOSYS` so the next gap is self-identifying. **Milestone B2 (2026-06-14): tobyOS runs REAL musl-libc binaries — a prebuilt, unmodified, statically-linked `busybox` (1.1 MB, musl 1.35.0).** A 7-applet battery (`echo/true/uname -a/pwd/cat/wc/stat` on `/etc/motd`) all exit 0 — exercising real-libc startup, stdio, file I/O (`open/read/fstat/close`), and the new **Linux `struct stat` translation** (`stat`/`lstat`/`fstat`/`newfstatat` → 144-byte Linux layout); `wc` prints correct `12 32 285`, `stat` prints the right size/blocks. Added **AT_RANDOM** to the auxv (libc canary), quiet `sendfile`/`fcntl` fallbacks. `[LXBB] VERDICT: PASS pass=7/7`, clean under default boot AND `+smep+smap` (0 faults), busybox is opt-in/not-committed (GPL; see `programs/busybox/README.md`). **Milestone B3 (2026-06-14): directory listing — `busybox ls`/`ls -la` work.** Added a `FILE_KIND_DIR` fd (`struct file` gains a kmalloc'd `dirpath` + resume offset); a Linux `open`/`openat` on a directory yields that fd, and **`getdents64`** re-opens via `vfs_opendir` and emits `linux_dirent64` records (8-aligned, with `d_type` from the VFS entry type) resuming after the stored offset — `fstat` on a dir fd reports `S_IFDIR`. `busybox ls -la /bin` prints a correct long listing (perms/links/uid/gid/size/name). 9-applet `[LXBB]` battery PASS, clean `+smep+smap`. **Milestone B4 (2026-06-14): Linux signal delivery + the cwd-`.` fix.** `rt_sigaction(13)`/`rt_sigprocmask(14)`/`rt_sigreturn(15)`/`kill(62)`/`tkill(200)`/`tgkill(234)` are translated onto the native signal layer — the Linux `struct sigaction` (handler/flags/restorer/mask order) is converted to tobyOS's, and **musl's own `sa_restorer` is recorded as the proc's sigreturn trampoline** (a Linux process never calls `SYS_SIGRESTORER`), so delivery enters the handler with `RDI=signo` and returns through the restorer → `rt_sigreturn`. Proven by `/bin/linux-sigtest` (a hand-rolled Linux ELF): installs a `SIGUSR1` handler, `kill`s itself, handler runs, `[LXSIG] VERDICT: PASS`. Also fixed `resolve_user_path` to strip a leading `.`/`./` so `busybox ls` with no path arg (`opendir(".")`) now works — battery grew to 10/10. Clean `+smep+smap` (0 faults; the signal-frame + sigaction uaccess is SMAP-safe). **Milestone B5 (2026-06-14): tobyOS runs DYNAMICALLY-linked Linux binaries via the real musl `ld.so`.** A PIE busybox (`PT_INTERP=/lib/ld-musl-x86_64.so.1`, `NEEDED libc.musl`) runs: the kernel's existing PT_INTERP path (built for native `ld-toby`) loads both the PIE program and the **genuine `ld-musl` interpreter** (which *is* libc in musl), hands off via auxv, and ld-musl self-relocates, relocates busybox, resolves symbols, sets up TLS, and runs — **`[LXDYN] VERDICT: PASS pass=7/7`** (echo/uname/pwd/cat/wc/stat/ls), clean `+smep+smap`, 0 faults, 0 unhandled syscalls. Required **zero new kernel code** — the Linux personality + syscall layer (B1–B4) + the existing dynamic-ELF loader already suffice, because busybox's only shared object is the kernel-loaded interpreter (so all of ld-musl's mmaps are anonymous). **Milestone B6 (2026-06-14): file-backed mmap.** Implemented Linux file-backed `mmap` — the primitive a loader needs to map a shared library beyond libc. `linux_syscall`'s mmap path now eager-reads the file's bytes into freshly-reserved pages and tightens to the requested protection (tobyOS's demand-paged `VMA_FILE` fault path was a stub, and musl closes the library fd the instant `mmap` returns, so lazy by-fd paging is impossible). The mmap **`offset` (Linux arg6)** — which the dispatch drops (only a1–a5 reach C) — is recovered from the saved `syscall_regs` block on the kstack, so **no hot-path `.S` change** was needed. Proven by `/bin/linux-mmaptest` (a hand-rolled Linux ELF): it `mmap`s a file at offset 0 *and* at a page offset (4096) and asserts the mapped bytes equal `read()`'s — **`[LXMMAP] VERDICT: PASS`**, clean `+smep+smap`. Proven directly by `/bin/linux-mmaptest`, and end-to-end by B7. **Milestone B7 (2026-06-14): end-to-end multi-DSO dynamic loading — tobyOS runs Alpine's real `file` binary, which loads a SEPARATE shared library (`libmagic.so.1`) via the real musl `ld.so`.** `file --version` → ld-musl opens `/lib/libmagic.so.1`, **file-backed-mmaps** its segments (one whole-span `MAP_PRIVATE` reservation, then a `MAP_FIXED` file map per segment at the lib's 16 TiB base + offset), relocates it, resolves its symbols, and the program prints `file-5.45` — **`[LXMULTI] VERDICT: PASS`**, clean `+smep+smap`, 0 faults, 0 unhandled. This **surfaced + fixed a real latent kernel bug**: `mmap.c`'s `PAGE_MASK` was `~(PAGE_SIZE-1)` where `PAGE_SIZE` is `4096u` (32-bit unsigned), so the mask was `0x00000000FFFFF000` and `page_align_down`/`up` **truncated any address ≥ 4 GiB** — and the mmap region lives at 16 TiB, so ld-musl's `MAP_FIXED` segment maps landed at the wrong (truncated) address → NULL-deref. (So B6's file-backed mmap was correct all along; this truncation, not the primitive, was the real blocker.) `file`/`libmagic.so.1` are opt-in/not-committed (see README). **Milestone B8 (2026-06-14): a real shell — `busybox sh` runs external commands via `fork`+`execve`+`wait4`.** `fork(57)`/`vfork(58)`/`clone(56)` (non-`CLONE_VM`) → tobyOS `sys_fork`; `execve(59)` → `sys_execve` (the Linux signature is identical, and execve re-latches the Linux personality from the new image); `wait4(61)` → `proc_wait` with the result re-encoded as a **Linux wait status** (`(code & 0xff) << 8`, so `WIFEXITED`/`WEXITSTATUS` work) + a `proc_any_child` helper for `wait4(-1)`; `setpgid`/`getpgid`/`setsid` accepted as no-ops/identity (tobyOS has a single global foreground pid, not POSIX pgroups). Proven by `busybox sh -c 'busybox echo A; busybox echo B'` — ash runs it as a command **list** so the first command can't be exec-optimized: `[fork] parent pid=2 -> child pid=3`, the child `execve`s busybox (`shell-forked-A`), the parent `wait4`s, then the trailing command exec-replaces in place (`shell-exec-B`) — **`[LXSH] VERDICT: PASS`**, clean default + `+smep+smap`, 0 unhandled. **Fixed another latent kernel bug:** `sys_execve`'s argv/envp/auxv packing wrote the new user stack **without a SMAP `stac` window**, so execve from CPL 3 #PF'd under `+smap` (native execve was silently broken, so this was never hit) — wrapped the pack in `uaccess_begin/end`. **Milestone B9 (2026-06-14): Linux threads — `clone(CLONE_VM)` runs a real thread in the shared address space.** `clone(56)` with `CLONE_VM` → new `sys_clone_thread` (fork.c): it mirrors `sys_fork`'s resume mechanism (copy the parent's trapframe, descend through `fork_child_entry` so the child returns from the syscall with rax=0) but installs the shared-VM thread fields `thread_create` uses — `is_thread`, `cr3 = leader->cr3` (no new PML4), `owns_pml4=false`, `tgid = leader` — and overrides the child's saved `user_rsp` to the caller-provided `stack` and its `tls_base` (CLONE_SETTLS), publishing the new tid via `CLONE_PARENT/CHILD_SETTID`. (`clone` *without* `CLONE_VM` is a fork-equivalent → `sys_fork`; added `sched_yield`.) Proven by `/bin/linux-thread` (a hand-rolled Linux ELF, with the clone in inline asm so the parent/child split doesn't touch the now-divided stack): it `clone`s a thread that runs in the **shared** address space, sets a shared flag, and `exit`s (the thread only) — the parent `sched_yield`s until it sees the flag, then exits 0. `[clone] thread tid=3 in tgid=2 (shared VM)`, the thread runs (`linux-thread+T`), **`[LXTHREAD] VERDICT: PASS`**, clean default + `+smep+smap`, 0 unhandled. **The discrete high-value Track B milestones are now complete** (static + real-libc binaries, dirs, signals, dynamic + multi-DSO loading, a shell, threads). **Milestone B10 (2026-06-26): unbranded Linux ELFs DROP-AND-RUN** — a shared `elf_is_linux_abi()` (used by both the spawn and `exec` paths) latches `ABI_PERS_LINUX` when the binary is branded `ELFOSABI_LINUX` **or** its `PT_INTERP` is a known Linux loader (`ld-linux*`/`ld-musl*`), so a vanilla `ELFOSABI_SYSV` *dynamic* Linux binary needs **no `brandelf` prep** (native `/lib/ld-toby.so` PIEs never match → never misclassified; static note-less SYSV binaries remain genuinely ambiguous and still need the brand). Proven by `[LXAUTO] PASS` running an `EI_OSABI=0`-forced musl busybox; full Linux track still green, `validate.sh` 3/3 ALIVE 0 faults. **Milestone B11 (2026-06-26): `pthread_join`** — a `clear_child_tid` on `struct proc` (set via `set_tid_address`/`clone(CLONE_CHILD_CLEARTID)`) is zeroed + `FUTEX_WAKE`d by `proc_exit` on thread exit, the exact rendezvous glibc/musl `pthread_join` blocks on; proven by `[LXJOIN] PASS` (a raw-syscall ELF spawns 4 joinable threads and joins each via `futex`). **Milestone B12 (2026-06-26): shell pipelines** — routed `dup2`/`dup3`/`pipe2` into the Linux layer (the only missing piece; the blocking pipe + fork/dup refcounts already worked), so `busybox sh -c 'a | b | c'` runs; proven by `[LXPIPE] PASS 3/3` (multi-stage pipelines whose final `grep -q` asserts the bytes flowed in order). **Milestone B13 (2026-06-26): readiness multiplexing** — `poll`/`ppoll`/`select`/`pselect6` + a real `epoll` (`epoll_create1`/`ctl`/`wait`, new `FILE_KIND_EPOLL`) over one `file_poll_ready()` predicate (pipes exact, UDP via datagram count, regular files always ready; TCP/tty input best-effort), all blocking cooperatively with timeouts; proven by `[LXPOLL] PASS` (a raw-syscall ELF's 7-check poll/select/epoll battery on a pipe). **Milestone B14 (2026-06-26): networking capstone — Linux BSD sockets + epoll, real event-driven TCP server.** The Linux personality had no socket syscalls; added `socket`/`bind`/`listen`/`accept`/`accept4`/`connect`/`setsockopt`/`getsockname`/`getpeername` (+`send`/`recv`) as real `FILE_KIND_SOCKET` fds, made a connected TCP socket a byte stream (`read`==`recv`, `write`==`send`), and wired **true TCP readiness** into `file_poll_ready` (listener `POLLIN` when a handshake completed via `tcp_can_accept`; connected `POLLIN`/`POLLOUT`/`POLLHUP`/`POLLERR` via `tcp_poll_flags`) — the cooperative poll/select/epoll loops now pump `net_poll()` whenever a watched fd is a socket so inbound segments progress while blocked. Proven by `[LXNETSRV] PASS exit=14`: a raw-syscall Linux ELF runs an epoll-driven TCP echo server and a **real external host client** (QEMU SLIRP `hostfwd`) gets its bytes echoed. **Milestone B15 (2026-06-26): signal completeness** — `SA_SIGINFO` 3-arg handlers now receive the byte-exact x86-64 *Linux* `siginfo_t`/`ucontext_t` layout, and synchronous CPU faults (`#PF`/`#GP`/`#DE`/`#UD`) are delivered to user handlers as `SIGSEGV`/`SIGFPE`/`SIGILL` with the right `si_addr` (`[LXSIGINFO] PASS exit=42`). **Milestone B16 (2026-06-26): networking CLIENT capstone** — the Linux socket layer gained UDP (`connect`/`sendto`/`recvfrom`/`send`/`recv` + a bounded `sock_recvfrom_to`) and ships `/etc/resolv.conf`, so a Linux binary RESOLVES a hostname over DNS and FETCHES it over a TCP active-open + HTTP GET; `[LXHTTP] PASS exit=55` is a live round-trip to the real internet over SLIRP (`example.com → HTTP/1.1 200 OK`). **Milestone B17 (2026-06-26): REAL off-the-shelf busybox does network I/O** — running unmodified musl-libc `busybox nslookup` + `wget http://example.com/` over the B16 path surfaced + fixed `bind(port=0)`→ephemeral, `socket(AF_INET6)`→`EAFNOSUPPORT` (so musl getaddrinfo falls back to IPv4), and `setitimer`/`ftruncate` stubs; `[LXNETBB] PASS 2/2`, a live internet fetch by genuine third-party software (busybox file battery still `[LXBB] PASS 10/10`). **Milestone B18 (2026-06-26): socket depth** — a real `getsockopt` (`SO_ERROR`/`SO_TYPE`/`SO_ACCEPTCONN`/timeout readback, was a no-op stub) + scatter-gather `sendmsg`/`recvmsg` (iovec gather/scatter, `msg_name` for UDP; syscalls 46/47 were `ENOSYS`); `[LXMSG] PASS exit=56` resolves+fetches over the live internet using `sendmsg`/`recvmsg` and asserts `getsockopt`. **Milestone B19 (2026-06-26): static, note-less SYSV Linux binaries DROP-AND-RUN** — `elf_is_linux_abi` gained rule 3: a *static* (`!has_interp`) binary carrying a `PT_GNU_STACK`/`RELRO`/`PROPERTY` phdr is Linux, since the `x86_64-*-linux-*` toolchain emits `PT_GNU_STACK` on every link (even `-nostdlib`) while the native bare-metal `x86_64-elf` toolchain emits none (verified: 0 native static ELFs carry it); the `!has_interp` guard keeps native *dynamic* `ld-toby` programs — which also carry `PT_GNU_STACK` — native. `[LXSAUTO] PASS exit=73` runs a genuinely unbranded/interp-less/note-less static Linux ELF auto-detected from the GNU phdr alone; full Linux track still green, `c_dynhello` still native. **Remaining is incremental breadth, not milestones:** UDP server-side (`recvfrom`-from-any needs host→guest hostfwd to prove), full glibc. **Track C (the Windows half), C1–C8 DONE 2026-06-15..17: tobyOS runs STOCK, off-the-shelf Windows x86-64 `.exe`s** — a PE32+ loader + IAT→kernel-shim binding via a user-mode marshalling gate (`ABI_SYS_WIN32_DISPATCH`/`ABI_PERS_WIN32`) makes tobyOS *be* the Win32 implementation (no real kernel32/user32 DLL). C1 freestanding `win-hello.exe`; C2 real ucrt `printf`; **C3 a plain `clang hello.c` through the FULL ucrt `mainCRTStartup`** (needed a real SWAPGS scheme for the PE's TEB via `gs:[0x30]` + a user heap + ~40 shims); C4 `clang++` incl. global constructors; C5 file I/O (`CreateFile`/`Read`/`Write`, HANDLE↔fd, `C:`→`/data`); C6 multithreading (`CreateThread`+`WaitForSingleObject`+ real `CRITICAL_SECTION`); C7 the user32/gdi32 GUI bridge (`RegisterClass`+`CreateWindowEx`+message loop, `WndProc` via a user-mode `DispatchMessage` trampoline); **C8 a VISIBLE + INTERACTIVE GUI window** — auto-login + desktop launch so it shows with chrome on the logged-in desktop, and `GetMessage` translates real mouse/keyboard/close `gui_event`s into `WM_*` so a real click recolours the window and the close button runs `WM_CLOSE`→`WM_DESTROY`→`PostQuitMessage`→clean exit; **C9 runtime API resolution** — `GetModuleHandle`/`GetProcAddress`/`LoadLibrary` resolve a Win32 API by NAME at runtime and return a callable pointer (the loader pre-generates a marshalling thunk per shim), the mechanism real software uses for runtime binding; **C10 MULTIPLE top-level windows** — a per-window state table + class registry; each window's paint/input/close routes independently and GetMessage stashes the per-message WndProc so the C7 trampoline calls the right one (a stock app opens two windows, a click recolours only the focused one); **C11 more GDI + more file ops + child/owned windows** — the marshalling gate was extended 8→10 args (for `hWndParent`); `CreatePen`/`SelectObject`/`MoveToEx`/`LineTo`/`Rectangle`/`Ellipse`/`SetPixel` map onto the compositor's draw ops, `SetFilePointer`/`GetFileSize`/`DeleteFile`/`CreateDirectory`/`GetFileAttributes`/`FindFirstFile`+`FindNextFile`+`FindClose` extend the file API, and `CreateWindowEx` records `hWndParent` so destroying a parent cascades WM_DESTROY to its children; **C12 bitmaps/BitBlt + real BUTTON/EDIT controls** — `CreateCompatibleDC`/`CreateBitmap`/`BitBlt` blit an in-memory image onto a window, and the predefined `BUTTON`/`EDIT` classes are real system controls (drawn + input-handled by tobyOS into the parent's client area; button click→`WM_COMMAND`, edit stores typed text via `GetWindowText`); **C13 more controls + modal dialogs + GDI fonts** — STATIC/CHECKBOX/LISTBOX/SCROLLBAR controls driven by `SendMessageA` (`LB_ADDSTRING`/`BM_GETCHECK`/`WM_VSCROLL`…), `MessageBoxA` as a real modal dialog (blocking poll loop → `IDOK`), and `CreateFont`/`SelectObject`/`GetTextExtentPoint32`/`SetTextColor`/`DrawText` scalable-font text + metrics; **C14a more controls + dialog templates** — `COMBOBOX`/`RADIOBUTTON`/`GROUPBOX`/`SysTabControl32` TAB controls (CB_*/TCM_*/WM_NOTIFY), and **`DialogBoxParamA` builds a real modal dialog by walking the PE's own `.rsrc` for the `RT_DIALOG` template** (DLGTEMPLATE/DLGTEMPLATEEX parse) and running its message loop via a fixed-VA shim-page trampoline that dispatches to the app's `DialogProc` (with `EndDialog`/`DefDlgProcA`/`CreateDialogParamA`/`GetDlgItem`/`CheckRadioButton`/`IsDlgButtonChecked`/`SendDlgItemMessageA`…); **C14b real TrueType fonts** — a kernel-side **stb_truetype** rasterizer (`src/kfont.c`, with an FXSAVE/FXRSTOR guard so its `-msse` float code never clobbers the calling app's live SSE state) renders genuine antialiased glyphs at arbitrary sizes from a bundled **Lato (OFL)** TTF in the initrd, so `CreateFontA`/`TextOutA`/`DrawTextA`/`GetTextExtentPoint32` produce real proportional text (8x8 bitmap fallback if no font ships); **C15 menus + timers + multi-button MessageBox** — a real menu bar (`CreateMenu`/`AppendMenuA`/`SetMenu` → kernel-drawn bar + dropdowns → menu clicks deliver `WM_COMMAND`), `SetTimer`/`KillTimer` → periodic `WM_TIMER`, and `MessageBoxA` honouring `MB_OKCANCEL`/`MB_YESNO`/… returning `IDOK`/`IDYES`/`IDNO`/`IDCANCEL`/…; **C16 registry + file dialogs + env + TTF-UI + per-thread TLS** — an in-kernel **registry** (advapi32 `Reg*`) persisted to `/data` (survives reboot), real **Open/Save file dialogs** (comdlg32) over `/data`, `GetCommandLine`/`GetEnvironmentVariable`/`SetEnvironmentVariable`/`ExpandEnvironmentStrings`, the whole **Win32 UI in TrueType**, and **per-thread TEBs** so `TlsAlloc`/`TlsGetValue`/`TlsSetValue` are per-thread. All Win32 milestones (`[WINPE]`=42 … C15=15, C16A/B/C/D/E) pass together under `+smep+smap`, 0 faults. **C17 winsock (ws2_32): TCP client + TCP server + UDP — a stock `.exe` does real network I/O over SLIRP.** **C18a: tobyOS runs the REAL off-the-shelf SQLite engine** (unmodified amalgamation, creates/queries an on-disk DB). **C18c: bold/italic TrueType faces** (CreateFontA lfWeight/lfItalic select among four Lato faces) **+ desktop-wide TrueType** (native apps' `gui_window_text` now renders proportional Lato). **C18b: compiler thread-local storage** — the PE loader walks the `.tls` directory and sets up per-thread template blocks + `_tls_index` + `gs:[0x58]` (`ThreadLocalStoragePointer`), so native `_Thread_local`/`__declspec(thread)` (built `-fno-emulated-tls`) works per-thread across `CreateThread`. **C18d: `shell32`** (CSIDL folder paths, `ShellExecuteA`, tray, drag-drop). **C19b: tobyOS runs the REAL off-the-shelf Lua 5.4.7 interpreter** (verbatim upstream, built as a PE; needed real `setjmp`/`longjmp` CPL3 leaf stubs + exposed a latent `fread` arg-swap bug). **C19c: a tightly-scoped `ole32`/COM ABI** — `CoInitializeEx`/`CoTaskMem*`/`CoCreateInstance` return a real vtable-backed object (one coclass/interface) whose `QueryInterface`/`AddRef`/`Release` + method are dispatched **through the vtable** (its slots are kernel gate-thunk VAs), proving COM-style kernel→CPL3 vtable dispatch (not a general COM runtime). **C20: float-return marshalling** — a second marshalling gate captures the FP arg registers `xmm0..xmm3` and lands a `double` result back in `xmm0` (`movq xmm0,rax`), backed by a new `-msse2` kernel libm (`kmath.c`, FXSAVE-guarded), so `double`-returning Win32/CRT functions work; proven both by a first-party libm harness (15 functions) and by the **real off-the-shelf Lua 5.4.7 interpreter doing `math.sqrt/sin/exp/log/pi`** (kernel re-reads the exact integer-scaled sentinel). **C21: float PRINTING** — the kernel Win32 `printf` engine (`win32_vformat`, behind ucrt's `__stdio_common_vfprintf`) now formats `double`s (`%f`/`%e`/`%g`), via a `kmath_fmt_double()` in the same `-msse2`/FXSAVE-guarded unit; proven by a first-party clang/ucrt `fprintf` battery (kernel re-reads the exact 10-conversion line) and by the **real Lua interpreter printing computed floats** (`1/4`, `0.1`, `10/3`, `sqrt(2)`). **C22: formatted/number INPUT** — the `scanf` family (`sscanf`/`fscanf` via a from-scratch `win32_vscanf` engine: `%d/%u/%x/%o/%c/%s/%f/%e/%g` + width/`*`/length) and real `strtod`/`atof` (the FP parse in `kmath_strtod`, the double returned through the C20 gate) now work — before, `vsscanf` was a 0-fields stub and `strtod` returned `0.0`; proven by a first-party clang/ucrt battery (sscanf ints/string/double, strtod with endptr, atof with exponent, `%x`, `%f`, fscanf from a file). All C1–C22 pass together under `+smep+smap`, 0 faults. Still no .NET/UWP. **Track X / X1 (2026-06-26): CROSS-PERSONALITY pipelines — the signature tobyOS feature.** Taught `sys_execve` to load a Windows PE (a new `execve_pe` arm; the ELF/Linux path is unchanged), so a forked busybox `sh` stage can `execve()` a genuine `.exe` and run it as a Win32 process. A single pipeline can now mix Windows and Linux binaries, with `WriteFile`/`ReadFile` bytes flowing through the same kernel pipes (`pipe2`/`dup2`) Linux stages use — proven by `[XPIPE] PASS 3/3`: `win-xprod.exe | grep` (Windows→Linux), `echo | win-xcons.exe` (Linux→Windows), and `echo | win-xfilter.exe | grep` (Linux→Windows→Linux, two personality flips in one pipeline). Neither Windows nor Linux can run the other's binaries in a pipeline; tobyOS does both on one kernel. |

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
2. **App compatibility (~46%)** — tobyOS now runs **unmodified Linux x86-64 binaries** broadly AND
   **stock, off-the-shelf Windows x86-64 `.exe`s — C and C++, console AND GUI** (a plain `clang`/`clang++`
   runs through its full ucrt CRT startup → main/WinMain → exit; real file I/O, multithreading with a real
   `CRITICAL_SECTION`, and a `user32`/`gdi32` GUI app that creates a window and draws through its WndProc
   message loop — and that GUI window is now **visible on the logged-in desktop and responds to real mouse/
   keyboard/close input** (C8)).
   **Track B (2026-06-14), milestones B1–B9 —
   the discrete Linux high-value set is complete:** a per-process ABI personality + a Linux→tobyOS
   syscall-translation layer run static ELFs (B1), **real musl-libc binaries** (B2 busybox), directory
   listing (B3 getdents64), **Linux signals** (B4), **dynamic linking via the real musl `ld.so`** (B5),
   **file-backed mmap** (B6) → **end-to-end multi-DSO** (B7, Alpine `file`+`libmagic`), **a real shell**
   (B8 `busybox sh` via fork/execve/wait4), and **threads** (B9 `clone(CLONE_VM)`). Surfaced + fixed
   **3 latent kernel bugs**. **Track C (2026-06-15), the Windows half, has its foundation — milestone C1:**
   a real **PE32+ loader** maps sections + applies base relocations, and the **IAT is bound to a kernel
   Win32 shim** via a user-mode marshalling gate (each import → a thunk that captures the Microsoft-x64
   args and issues one `ABI_SYS_WIN32_DISPATCH` syscall — solving the "CPL3 can't call kernel code"
   problem that made the old dead `pe_loader.c` unworkable). A genuine MinGW-built `win-hello.exe`
   (`kernel32!{GetStdHandle,WriteFile,ExitProcess}`) loads, prints to stdout, and exits 42 (`[WINPE]
   PASS`, clean `+smep+smap`). **C2 (2026-06-16) — the C runtime:** the marshalling gate now marshals
   **stack arguments** (not just the 4 register args), so variadic Win32 functions work; a new shimmable
   DLL namespace (`api-ms-win-crt-stdio`) implements `__acrt_iob_func` + `__stdio_common_vfprintf` + `puts`
   on top of a **full kernel `printf` engine** (flags/width/precision/length, `d/i/u/x/X/o/p/c/s/%`). A
   real ucrt-linked `win-crt.exe` renders `%s/%d/%x/%c`, width/precision/zero-pad/left-justify, `%lld`,
   `%p`, and >4 varargs correctly and exits 7 (`[WINPE2] PASS`, clean `+smep+smap`). **C3 (2026-06-16) —
   the full `mainCRTStartup`:** a STOCK `clang hello.c -o hello.exe` (no flags, no custom entry) now runs
   unmodified. This required giving a PE a real **TEB** reachable via `gs:[0x30]` — which forced a real
   **SWAPGS scheme** across the whole syscall + interrupt + context-switch surface (the kernel had used
   GS for per-CPU data without SWAPGS; PEs get a per-proc GS base = TEB, native/Linux procs get an
   identity swap so they're behaviourally unchanged) — plus a **user-memory heap** (`malloc`/`calloc` over
   `sys_mmap`), a **CRT data region** (`argc`/`argv`/`environ`/`_commode`/`_fmode`), and **~40 kernel32 +
   ucrt shims** (`_initterm`, `__p_*`, `__getmainargs` glue, `memcpy`/`strlen`/`strncmp`, `VirtualQuery`,
   critical sections, …). The off-the-shelf `.exe` prints `argc=1 argv0=win-hello3.exe` + its message and
   exits 3 (`[WINPE3] PASS`); C1/C2 still pass under the shared SWAPGS gate; clean `+smep+smap`, validate
   3/3 ALIVE, normal desktop boot unaffected. **C4 (2026-06-16) — C++:** a STOCK `clang++ hello.cpp` now
   runs, **global constructors and all** — mingw runs them via the GCC `__CTOR_LIST__` path (static
   `__main` → `__do_global_ctors`, all CPL3 user code), so **no kernel trampoline was needed**; C4 was
   just broadening the shim surface for the ~13 extra ucrt imports a C++ binary pulls in (`_errno`,
   `localeconv`, stdio FILE locking, wide/multibyte string helpers). A global ctor prints its line + sets
   a flag before `main`, which returns 5 (`[WINPE4] PASS`). **C5 (2026-06-16) — file I/O:** the Win32
   `CreateFileA`/`ReadFile`/`WriteFile`/`CloseHandle` API maps onto the VFS (`sys_open`/`read`/`write`/
   `close`) with a real HANDLE↔fd mapping, Windows access/disposition→`O_*` flag translation, and a path
   translation where the `C:` drive maps to tobyOS's writable `/data` (`\`→`/`). A stock `.exe`
   write→close→reopen→read round-trips `C:\wintest.txt` and verifies the bytes, exit 9 (`[WINPE5] PASS`).
   This also fixed a **latent entry-RSP alignment bug** — the mingw CRT entry needs `RSP%16==8`; I'd set
   `%16==0`, which C3/C4 survived by luck but C5's `movaps`-based buffer init exposed. **Linux: remaining
   is incremental syscall breadth** (pthread_join, poll/epoll, broader sockets, glibc). **C6 (2026-06-16) —
   multithreading:** `CreateThread` spawns a tobyOS thread that enters a CPL3 wrapper (shim page) which
   calls the thread function then exits; `WaitForSingleObject` joins it (thread HANDLEs are tagged so they
   don't collide with file fds); and `EnterCriticalSection`/`LeaveCriticalSection` are **real** mutual
   exclusion (lock state in the user `CRITICAL_SECTION`, serialised by the BKL, contention handled by
   yield-retry). A stock `.exe` with 4 threads each incrementing a shared counter under the lock yields
   exactly `4000` (no lost updates), exit 6 (`[WINPE6] PASS`). **C7 (2026-06-17) — the user32/gdi32 GUI
   bridge:** a stock Win32 GUI `.exe` (Windows subsystem, `WinMain`) does `RegisterClass` + `CreateWindowEx`
   (→ a real desktop window via `sys_gui_create`, correct title/size) + a `GetMessage`/`DispatchMessage`
   loop whose `WndProc` draws (`BeginPaint`→`FillRect`+`TextOut`→`EndPaint` → `sys_gui_fill`/`sys_gui_text`).
   The hard part — `DispatchMessage` calling the app's `WndProc` (a kernel shim can't call CPL3 code) — is a
   **user-mode trampoline** in the shim page (same idea as the C6 thread wrapper) that reads the `MSG`,
   loads the registered `WndProc`, and calls it. Proven by the serial log (window created + WndProc ran →
   exit 7, since the exit code is only set after the paint completes) + `[WINPE7] PASS`, clean `+smap`.
   (C7 ran the app at *boot* so its window was composited behind the login screen — that and input are
   resolved by C8.) **C8 (2026-06-17) — a VISIBLE + INTERACTIVE Win32 GUI window:** the harness now signs a
   session in programmatically (`session_login`, root's empty seed password), dismisses the login window,
   and launches the `.exe` onto the **logged-in desktop** so its window shows with full chrome on top of the
   wallpaper + taskbar; and `GetMessage` now translates real `struct gui_event`s into `WM_*` (mouse →
   `WM_MOUSEMOVE`/`WM_LBUTTONDOWN`/`WM_LBUTTONUP`, keyboard → `WM_KEYDOWN`, close → `WM_CLOSE`), with a
   `DestroyWindow`→`WM_DESTROY` synthesis so the idiomatic `WM_CLOSE`→`WM_DESTROY`→`PostQuitMessage`→clean-
   exit chain runs. A stock interactive `.exe` is driven by a **real PS/2 mouse click** (`mouse_inject_event`
   → driver → compositor hit-test → the window) whose `WndProc` recolours the window (blue→green via
   `CreateSolidBrush`, honoured by `FillRect`), then a deterministic close drives the teardown; it returns 8
   iff it painted, handled the click, AND ran the close chain (`[WINPE8] PASS`). The headline is finally a
   **screenshot of the real green Win32 window on the tobyOS desktop**, cursor on it, plus the serial log
   (`GetMessage -> WM_0201`…`WM_CLOSE`…`WM_DESTROY`) and exit 8. **C9 (2026-06-17) — runtime API resolution
   (`GetModuleHandle`/`GetProcAddress`/`LoadLibrary`):** every Win32 call until now was a *static* IAT import
   bound at load; C9 adds the mechanism a huge amount of real software uses — resolving an API by **name at
   runtime** and calling through the returned pointer. The loader now pre-generates a marshalling thunk for
   **every** kernel-side shim into a fixed region of the shim page, so `GetProcAddress(hModule, "Func")`
   resolves the name (`win32_shim_index`) and hands back that thunk's VA — a real, callable CPL3 function
   pointer that goes through the same gate as an IAT import. A module HANDLE is a tagged token carrying a
   shim index whose `.dll` names the module (so `GetProcAddress` recovers the DLL); `LoadLibraryA` returns
   the same (tobyOS *is* the implementation, nothing to load); unknown names/DLLs → NULL (honest). A stock
   `clang` `.exe` resolves `GetCurrentProcessId` at runtime and **calls it** (returns its real pid), checks
   the pointers are stable + distinct, that an unknown name is NULL, and that `LoadLibraryA("user32.dll")` +
   `GetProcAddress(…, "GetMessageA")` works cross-DLL — exit 9 (`[WINPE9] PASS`). **C10 (2026-06-19) —
   MULTIPLE top-level windows:** the GUI bridge's single-window state became a per-window table (keyed by
   `HWND == fd`) plus a class registry (class name → `WndProc`); every shim now routes by its `hwnd`/`hdc`
   arg, and `GetMessage` scans all the app's windows and -- the key trick -- stashes each message's target
   `WndProc` into the CRT slot the C7 `DispatchMessage` trampoline reads, so the right window's `WndProc`
   is called with no asm change (GetMessage/DispatchMessage run in lock-step per message). A stock `.exe`
   opens TWO windows sharing one `WndProc` (discriminating by `HWND`); a real mouse click into the focused
   window recolours **only** that one (per-window state), and the two windows close independently
   (`WM_CLOSE`→`WM_DESTROY` each, the window retired from the desktop on destroy) — exit 10 (`[WINPE10]
   PASS`) iff both painted, exactly one got the click, and both ran their destroy; proven by a **screenshot
   of two Win32 windows on the desktop, one green (clicked) and one blue**. Also fixed a cross-app state leak
   the refactor exposed (the process-global bridge is now reset per fresh GUI app, validated against the live
   fd table since pids are reused). **C11 (2026-06-19) — more GDI + more file ops + child/owned windows:**
   three additions on a widened foundation. The marshalling **gate was extended from 8 to 10 marshalled args**
   (re-assembled offline, 123 bytes, verified) so `CreateWindowExA`'s `hWndParent` (its 9th arg) — and any
   future >8-arg Win32 function — reaches the shim. **GDI:** `CreatePen`/`SelectObject` (per-DC current pen),
   `MoveToEx`/`LineTo` (per-DC current position), `Rectangle`, `Ellipse` (drawn as an inscribed circle —
   honest approximation), `SetPixel`, all mapping onto the compositor's `gui_window_line`/`_rect`/
   `_circle_outline`. **File ops:** `SetFilePointer`→lseek, `GetFileSize`→fstat, `DeleteFileA`→unlink,
   `CreateDirectoryA`→mkdir, `GetFileAttributesA`→vfs stat, and **`FindFirstFileA`/`FindNextFileA`/`FindClose`**
   directory enumeration via the kernel VFS (`vfs_opendir`/`readdir`) filling a `WIN32_FIND_DATAA`.
   **Child/owned windows:** `CreateWindowExA` now reads `hWndParent`, records the parent link, and destroying
   a parent **cascades** WM_DESTROY to its children (top-level rendering — the compositor has no nested
   client-area clipping — but faithful parent→child lifetime). Two proofs: a console `.exe` round-trips
   dir+create+write+size+seek/read+find+delete (exit 11), and a GUI `.exe` draws a pen line + Rectangle +
   Ellipse, opens a child window owned by the main, and destroys ONLY the main — both windows' WndProcs get
   WM_DESTROY (exit 11), proven by a screenshot of the GDI window + its child. **C12 (2026-06-19) — bitmaps/
   BitBlt + real BUTTON/EDIT controls:** (a) `CreateCompatibleDC`/`CreateBitmap`/`SelectObject`(bitmap)/
   `BitBlt`/`DeleteDC` let an app compose an off-screen image (a kmalloc'd 32-bpp pixel buffer; DWORDs are
   0x00RRGGBB which IS the framebuffer's XRGB) and blit it onto a window via `gui_window_blit`. (b) The
   predefined **`BUTTON`/`EDIT` window classes** are real system controls: `CreateWindowEx("BUTTON"/"EDIT",
   parent, …)` makes a **virtual child rendered into the parent's client area** (the compositor is
   top-level-only, so they're drawn in-line rather than as nested windows) — tobyOS draws each control and
   handles its input, the app never implements them. A BUTTON click synthesises `WM_COMMAND`(BN_CLICKED, id)
   to the parent; an EDIT stores typed chars (with backspace) readable via `GetWindowTextA`. Two proofs: a
   `.exe` BitBlts an in-memory gradient (exit 12, screenshot of the bitmap), and a `.exe` creates an EDIT +
   BUTTON that the harness types into ("tobyOS") and clicks — the app gets `WM_COMMAND`, reads the edit text
   back, exits 12 (screenshot of the edit + button). **C13 (2026-06-19) — more controls + modal dialogs +
   GDI fonts:** (a) the control system gained **STATIC, CHECKBOX, LISTBOX and SCROLLBAR**, driven by a new
   **`SendMessageA`** (the universal control channel: `BM_GETCHECK`/`SETCHECK`, `LB_ADDSTRING`/`GETCURSEL`/
   `SETCURSEL`/`GETTEXT`/`GETCOUNT`) — a checkbox toggles on click and notifies `WM_COMMAND`, a listbox row
   click sends `LBN_SELCHANGE`, a scrollbar's arrows/track send `WM_VSCROLL` (`SetScrollRange`/`Pos`/`Info`).
   (b) **`MessageBoxA`** is a real **modal dialog** — it creates its own window with the text + an OK button
   and blocks (a kernel poll loop, `sched_yield`-ing so the system keeps running) until OK is clicked, then
   returns `IDOK`. (c) **GDI fonts + text metrics:** `CreateFontA` maps a height onto tobyOS's scalable
   bitmap font, `SelectObject(font)` sets the DC scale, `SetTextColor`/`SetBkColor`/`SetBkMode` set text
   colours, `GetTextExtentPoint32A`/`GetTextMetricsA` report sizes, and `TextOutA`/`DrawTextA` (with
   `DT_CENTER`) render scaled, coloured text. Two proofs: a `.exe` with a STATIC/CHECKBOX/LISTBOX/SCROLLBAR +
   a button that pops a MessageBox — the harness checks the box, selects "Banana", nudges the scrollbar, and
   OKs the dialog (exit 13, screenshots of the controls + the modal box); and a `.exe` that draws text at 4
   sizes/colours + a centred `DrawText` (exit 13, screenshot of the fonts). **C14a (2026-06-20) — more
   controls + dialogs from resource templates:** (a) four new controls — **COMBOBOX** (a field + a click-to-
   open dropdown list; `CB_*`), **RADIOBUTTON** (a circular check, mutually exclusive within a `WS_GROUP`
   run), **GROUPBOX** (a labelled visual frame), and a **TAB** control (`SysTabControl32`; `TCM_*`, a tab
   click posts `WM_NOTIFY`/`TCN_SELCHANGE` and `ShowWindow(SW_HIDE/SW_SHOW)` switches pages). (b) **dialogs
   from resource templates** — `DialogBoxParamA(hInst, MAKEINTRESOURCE(id), …)` walks the PE's own `.rsrc`
   (exposed by the loader via `PE_DIR_RESOURCE`) for the `RT_DIALOG` resource, parses the `DLGTEMPLATE`/
   `DLGTEMPLATEEX` + its `DLGITEMTEMPLATE(EX)` controls, builds the dialog window + controls, and — since a
   modal dialog dispatches to the app's CPL3 `DialogProc` — runs the message loop via a fixed-VA shim-page
   **trampoline** (it PUMPs one message at a time from the kernel and DispatchMessages each to `DialogProc`
   until `EndDialog`), plus `CreateDialogParamA`/`DefDlgProcA`/`GetDlgItem`/`Set`/`GetDlgItemText`/
   `CheckDlgButton`/`IsDlgButtonChecked`/`CheckRadioButton`/`SendDlgItemMessageA`. Two proofs: a `.exe` drives
   the four controls (radio "Slow" + combo "Green" + tab "Two", exit 14, 2 screenshots), and a `.exe` whose
   whole UI is a modal dialog built from a `.rsrc` template — the harness types "tobyOS" into the EDIT, checks
   a box, picks "Premium", OKs, and the `DialogProc` reads them back → exit 14 (2 screenshots). **C14b
   (2026-06-20) — real TrueType fonts:** a kernel-side **stb_truetype** rasterizer (`src/kfont.c`) renders
   genuine antialiased glyphs at arbitrary pixel sizes from a bundled **Lato** (OFL) TTF shipped in the initrd
   (`/etc/Lato-Regular.ttf`, lazy-loaded via `vfs_read_all`). stb_truetype is float-heavy but the kernel is
   `-mno-sse`, so `kfont.c` is compiled `-msse` and brackets every float window in an FXSAVE/FXRSTOR guard
   that preserves the calling app's live SSE state (a syscall doesn't save user FP on entry). `CreateFontA`'s
   HFONT carries the pixel height, `TextOutA`/`DrawTextA` route through the rasterizer (alpha-blended into the
   window backbuffer, integer-only in gui.c), and `GetTextExtentPoint32A`/`GetTextMetricsA` report real TTF
   metrics; a glyph cache keeps the FP work off the hot path. Proof: a `.exe` draws text at 18/26/36/52 px in
   four colours + a centred `DrawText` and measures "WWWW" vs "iiii" → exit 14 iff proportional (TTF, not the
   monospace fallback); screenshot of real Lato glyphs. **C15 (2026-06-20) — menus + timers + multi-button
   MessageBox:** a real **menu bar** (`CreateMenu`/`CreatePopupMenu`/`AppendMenuA`/`SetMenu`) kernel-drawn
   into the top of the client area, with dropdowns whose item clicks deliver `WM_COMMAND`; **timers**
   (`SetTimer`/`KillTimer` → a periodic `WM_TIMER` synthesised by `GetMessage`); and `MessageBoxA` now honours
   the button set (`MB_OKCANCEL`/`MB_YESNO`/`MB_YESNOCANCEL`/`MB_ABORTRETRYIGNORE`/`MB_RETRYCANCEL`) and
   returns the clicked id (`IDYES`/`IDNO`/`IDCANCEL`/…). Proof: a `.exe` with a File/Help menu, a 500 ms
   counter, and a Yes/No About box → exit 15 (3 screenshots: menu bar + counter, dropdown, Yes/No box).
   **C16 (2026-06-20) — registry + file dialogs + env + TTF-UI + per-thread TLS** (five sub-milestones
   C16a-e): an in-kernel **registry** (advapi32 `Reg*`) persisted to `/data` so settings survive a reboot
   (proven by a 2-boot test); real modal **Open/Save file dialogs** (comdlg32) browsing `/data`;
   `GetCommandLine`/`GetEnvironmentVariable`/`SetEnvironmentVariable`/`ExpandEnvironmentStrings` over a seeded
   per-app environment; the entire **Win32 UI rendered in TrueType** (control labels, menus, dialogs — native
   apps + desktop stay 8x8); and **per-thread TEBs** giving each `CreateThread` thread its own
   `NtCurrentTeb`/TLS (`TlsAlloc`/`TlsGetValue`/`TlsSetValue`). **C17a (2026-06-21) — Winsock (ws2_32),
   the TCP client:** a stock Windows `.exe` linked against `ws2_32` does a real **HTTP GET over the network** —
   `WSAStartup`/`gethostbyname`/`socket`/`connect`/`send`/`recv`/`closesocket` mapped onto tobyOS's existing
   `ksock_*`/DNS/TCP stack (a tagged `SOCKET` handle; `gethostbyname` builds a real `struct hostent`).
   **C17b (2026-06-22) — Winsock TCP server:** a stock Windows `.exe` does `bind`/`listen`/`accept` and a REAL
   external client (the QEMU host, over SLIRP hostfwd) connects to it; the server reads the request and replies
   (caught + fixed a latent kernel bug: `tcp_close`'s TIME_WAIT linger held the BKL across `hlt`, deadlocking
   pid 0 — only the first *active* TCP closer, a server, hit it). **C17c (2026-06-22) — Winsock UDP:** a stock
   `.exe` does `sendto`/`recvfrom` (datagram echo server); a real host datagram (SLIRP hostfwd) round-trips
   through tobyOS's UDP stack (caught + fixed a second latent bug: `sock_recvfrom` never drained the NIC while
   waiting, so inbound UDP couldn't arrive unless another context happened to drain). **C18a (2026-06-24) — a
   REAL third-party binary:** the unmodified public-domain **SQLite** amalgamation, built as a Windows PE,
   runs on tobyOS and creates/queries a real on-disk database under `/data` (~110 new kernel32/ucrt shims —
   Win32 heap, the wide-file VFS, a C-runtime layer; +2 latent kernel-bug fixes: `GetLastError` and a POSIX
   `lseek`-past-EOF size bug). **C18b (2026-06-24) — `__declspec(thread)`/`_Thread_local` compiler TLS:** the PE
   loader walks the `.tls` directory and gives each thread a template-initialised TLS block via `gs:[0x58]`.
   **C18c (2026-06-24) — bold/italic + desktop-wide TrueType:** the kernel `stb_truetype` rasterizer holds four
   Lato faces (regular/bold/italic/bold-italic), `CreateFontA` selects by weight/italic, and the desktop's own
   text is now TrueType. **C18d (2026-06-24) — `shell32`:** `SHGetFolderPathA`/`SHGetSpecialFolderPathA` (CSIDL →
   `C:\` → `/data`), `ShellExecuteA`, `Shell_NotifyIcon`, `DragQueryFile`. **Windows: next** — broader runtime
   (more ucrt: e.g. `__stdio_common_vsprintf`), more of the system DLLs. Still no .NET/UWP.
3. **Real multi-core execution** — **DONE** (with one known SMAP caveat). APs run user
   code in parallel via a syscall-path big-kernel-lock + per-CPU idle procs + work-stealing,
   on top of the per-CPU TSS/GS/current_proc foundation. The pid 0 ↔ login interaction bit
   twice: first a deadlock (fixed by holding the BKL around pid 0's `gui_tick` work + the
   AP-run boot gate), then a **livelock** — pid 0's whole-iteration BKL hold vs login's
   busy-poll syscall stream — fixed 2026-06-11 (fair ticket BKL, per-phase idle_loop holds,
   truly-idle nanosleep, login sleeps between polls; see changelog). ~2.6-2.7x on 4 CPU-bound workers; desktop stable
   on default and `+smap`. **Priority classes + fair per-AP timeslicing now DONE**
   (2026-06-08): a HIGHER-runs-first `prio` (IDLE..RT, 0==NORMAL default) feeds a
   highest-effective-priority dequeue with FIFO-within-level + aging anti-starvation, and the
   per-CPU LAPIC timer preempts ring-3 user code whose class quantum is spent so CPU-bound
   procs timeslice on the APs (only the BSP had PIT preemption before). `SYS_SETPRIORITY`/
   `GETPRIORITY` + `toby_setprio` expose it (uid/root policy). Validated: 3 HIGH vs 3 LOW on
   4 cores → ~18x CPU split, no starvation. **Remaining:** an `argc>=1` first-run proc on an
   AP under SMAP SMEP-faults (kernel-executes-user-page; fault RIP tracks the packed
   user_rsp); needs a real-HW debug session. And the priority scheme is a coarse 5-class
   static model, not a full MLFQ with per-CPU load balancing.
4. **GPU-accelerated desktop on real hardware** — compositor accel only exists for VirtIO,
   not the Intel iGPU path used on the EliteDesk. **In progress (i915-lite):** the desktop
   today renders via Limine's firmware framebuffer (CPU memcpy); the Intel drivers
   (`gpu_intel_modeset.c`, the now-deleted duplicate `intel_gfx.c`, `compositor_accel.c`)
   were dead code, never called at boot. Staged bring-up that never re-modesets Limine's
   working pipe: **Stage 1 DONE** — read-only GT/display recon (`intel_gpu_gt_recon`) logs
   GT gen, GGTT/stolen config, BCS ring state, active scanout; writes nothing.
   **Stage 2 DONE + VALIDATED ON REAL HW (2026-06-04)** — `intel_gt_init`: forcewake
   (render+blitter), gen9 GGTT mapping (8-byte PTEs, top half of the 16 MiB BAR), BCS
   blitter ring bring-up, and an `MI_STORE_DWORD_IMM` hardware self-test. On the EliteDesk
   (HD 520, gen9): `self-test PASS -- blitter accel available` (`scratch=0x7ed5c0de`). The
   blitter executes our command stream end-to-end. Took 3 blind real-HW boots to land
   (BAR-size 4 MiB clamp → missing MI_USE_GLOBAL_GTT bit → PASS). Touches only GT/GGTT/ring
   + our own pages, never the display pipe, so it can't blackscreen the box; gated behind
   `intel_gt_selftest_ok()`. **Stage 3 (next)** — wire `XY_COLOR_BLT`/`XY_SRC_COPY_BLT` as a
   `gfx_backend` peer of virtio, presenting via Limine memcpy first (a broken blit shows
   visible garbage, never a black screen). **Stage 4** — vblank-synced `PLANE_SURF` page-flip
   with watchdog auto-revert. QEMU can't emulate the Intel GT, so 3-4 stay EliteDesk-validated.
5. **Security depth** — ~~salted/KDF auth~~ (Argon2id), ~~login rate-limiting~~ (lockout),
   ~~signal *delivery*~~ (frame push + sigreturn), ~~re-enable SMAP~~ (syscall-wide uaccess
   window), ~~SA_RESTART~~, ~~job control stop/cont~~ (both 2026-06-12). ~~per-copy
   uaccess accessors~~ (done 2026-06-12: Linux-style `copy_*_user`, whole-syscall window
   removed, validated under hardware `+smap`). ~~SA_SIGINFO~~ and ~~terminal job-control keys~~ (both 2026-06-13). The security-depth
   bundle is now done; the only job-control gap left is full POSIX process groups + fg/bg/jobs,
   which is a shell/controlling-tty feature that doesn't fit the kernel-builtin-shell model.

---

## Known shallow / "present but not robust" items
- SMP: APs run user code in parallel now (BKL-serialized kernel, work-stealing, ~2.6x on 4
  workers), with priority-aware preemptive timeslicing (5 static classes + aging, per-AP LAPIC
  preemption). Now has MLFQ-flavoured interactivity (I/O) boosting (yield-before-slice keeps a proc high,
  burn-a-slice demotes it -- proven to cut interactive wake latency under hog load) and
  load-balanced (busiest-queue) work stealing. Still not a full MLFQ: no per-CPU run-queue
  load tracking beyond steal-victim choice, single global quantum/aging tunables.
  Caveat: argc>=1 first-run procs on an AP under SMAP were reported to SMEP-fault on
  real HW (gap item #3). **2026-05-31: confirmed NOT reproducible in QEMU** across three
  escalating configs incl. multi-threaded TCG (`-accel tcg,thread=multi`, `Skylake-Client,
  +smep,+smap`) — 32/32 argc>=1 workers ran clean on APs, 0 faults. Instrumentation added
  (`-DMCARGV_BOOT` repro harness + a SMEP fault dump in isr.c) to capture the fault frame on
  the EliteDesk over the COM1 null-modem cable; see `docs/smep-ap-capture.md`. Root-cause now
  blocked on a real-HW serial capture, not on QEMU debugging.
  The 2026-06-11 freeze fix is validated mechanism-directed + no-regression only (the bug
  stopped reproducing on the dev host after a Windows update mid-investigation) — EliteDesk
  confirmation pending.
- IPv6: link-local + SLAAC global addressing (RS/RA, prefix autoconf, default router). Still no
  DHCPv6 (stateful), no address lifetimes/expiry, no Duplicate Address Detection.
- Passwords: now salted Argon2id (was djb2) with login lockout (5 fails → 30 s). Still no
  password policy and no PAM-style pluggable auth.
- GPU accel: VirtIO-only; real-HW desktop is CPU-composited.
- SMAP: enabled with per-copy accessors (Linux `copy_*_user` style, 2026-06-12) -- the
  whole-syscall stac window is gone, so a stray kernel user-pointer deref inside a syscall
  now faults instead of silently succeeding. Large I/O bounces through kernel buffers.
- Signals: user-handler delivery works (frame push + sigreturn, mask/pending
  honored), SA_RESTART restarts interrupted syscalls, SA_SIGINFO delivers
  siginfo_t+ucontext to 3-arg handlers, and SIGSTOP/SIGCONT job control works
  (PROC_STOPPED; stops delivered even to CPU-bound procs on APs via the LAPIC
  tick; Ctrl-Z/Ctrl-\ wired in the keyboard path). Still missing: full POSIX
  process groups + fg/bg/jobs, SIGCHLD-on-stop/WUNTRACED, and *caught-handler*
  delivery to a pure-CPU-bound process is deferred to its next syscall (tick
  paths only run default dispositions; they don't push handler frames).
- TobyFS journaling: now crash-consistency + stress tested (power-loss rollback/replay + churn/double-indirect harness, [TFST]). Swap/CoW are exercised by their own self-tests (zram [MEMCT], fork CoW) but not yet under sustained combined memory pressure.

---

## Changelog
- **2026-06-27** — **Track B: a REAL, off-the-shelf GNU *bash* runs INTERACTIVELY over the TTY — unmodified GNU bash 5.2.**
  Where B22 ran busybox's small `ash` interactively, this runs the canonical, full-featured Unix shell: **unmodified GNU bash
  5.2-015** (the public reproducible `robxu9/bash-static` build, statically linked against musl 1.2.3 — so it runs through the
  proven static-Linux loader path with no DSO version-matching; the milestone's new content is the *interactive bash read-eval
  loop over the TTY*, not the libc linkage). bash brings its own bundled **readline** line editor (raw-mode termios, `ESC[6n`
  cursor-position queries, `SIGWINCH`), `$((arithmetic))`, command substitution, and a much larger libc/syscall surface, so it
  exercises the B21/B22 console line discipline far harder than `ash` did. We "type" a 3-line session into the keyboard ring
  (`kbd_inject_raw`, like LXISH) — `X=$(busybox echo 35)` (command substitution → fork+exec), `echo BASH-OK
  ver=$BASH_VERSION sum=$((X+7))`, `exit $((X+7))` — and a PASS means bash read all three lines in its interactive loop,
  forked+exec'd busybox for the substitution, evaluated the arithmetic, printed its genuine **`ver=5.2.15(1)-release`**
  banner, and exited 42 (= 35 + 7). **Kernel gap filled:** `access` (syscall 21) was a blanket `-ENOENT` stub, and
  `faccessat` (269) / `faccessat2` (439) were unimplemented (`-ENOSYS`) — every shell PATH lookup / `test -e/-x` uses these.
  Replaced with a real `lx_faccess()` that resolves the path and `vfs_stat()`s it (existence-based; tobyOS doesn't model
  per-file Unix mode bits, and `/bin` is all-executable, so any existing path is accessible — exactly what PATH search needs).
  **Proof, clean under `+smep,+smap`, 0 faults / 0 unhandled syscalls:** `[REALBASH] VERDICT: PASS exit=42` (`logs/bashrun.sh`).
  **No regression** (touches core syscall dispatch + the access path): combined boot of the hand-rolled Linux battery
  (`LXABI/LXSIG/LXMMAP/LXTHREAD/LXJOIN/LXPOLL` all PASS), static musl busybox `[LXBB] PASS 10/10`, interactive busybox
  `[LXISH] PASS 42`, pty `[LXPTY] PASS 63`, `[LXPROCFS2] PASS 63`, static auto-detect `[LXSAUTO] PASS 73`, all in one ISO with
  0 faults (`logs/bashregress.sh`). *Opt-in:* the bash binary is gitignored (GPL); the initrd stages + brandelf's it only when
  present (`programs/realbash/build.sh`).
- **2026-06-27** — **Track B: a *BATTERY* of real, off-the-shelf, DYNAMIC glibc GNU Binutils runs — objdump/nm/size/c++filt 2.43.1.**
  The `realtool` milestone ran one tool (`readelf`); this runs five genuine, unmodified GNU Binutils 2.43.1 programs back-to-back,
  all **dynamically linked against glibc** (`ld-linux-x86-64.so.2` + `libc.so.6` + `libdl.so.2`, the Bootlin 2025.08 toolchain):
  `objdump --version` (identity → **`GNU objdump (GNU Binutils) 2.43.1`**), `objdump -d /bin/hello` (disassemble a native tobyOS
  ELF through libbfd), `nm -D /bin/objdump` (dump objdump's own dynamic symbols), `size /bin/objdump` (section sizes), and
  `c++filt _Z4testi` (Itanium-ABI demangle → **`test(int)`**). **Kernel gap fixed — the loader bug that blocked all multi-DSO
  glibc programs:** every file's `stat`/`fstat` reported `st_ino = 1`, but glibc's `ld.so` dedups already-loaded shared objects by
  `(st_dev, st_ino)`. So once `libdl.so.2` loaded (ino 1), opening `libc.so.6` (ino 1 too) looked like *the same file already
  mapped* → ld.so aliased libc's link-map to libdl's, and every `libc` symbol-version lookup (`GLIBC_2.7`, `GLIBC_2.36`, …) failed
  against libdl's tiny verdef table (`version 'GLIBC_2.7' not found`). Fixed by synthesizing a **stable, per-file inode** (FNV-1a
  over the path for path stats; over the backing-node pointer `f->vfs.priv` for `fstat`) so distinct files get distinct inodes and
  ld.so's dedup behaves — `libc.so.6` now generates its own link map and versions resolve. (Diagnosed with glibc's own
  `LD_DEBUG=libs,versions,files`; also bumped `VMA_MAX_PER_PROC` 64→256 and added mmap-failure logging while ruling out an OOM/VMA
  cause.) **Proof, clean under `+smep,+smap`, 0 faults / 0 unhandled syscalls:** `[REALTOOLS] VERDICT: PASS pass=5/5`
  (`logs/realtools.sh`). *Opt-in:* the binutils + glibc runtime are gitignored; the initrd stages them only when present
  (`programs/realtools/build.sh`).
- **2026-06-27** — **Track B: a REAL CPython interpreter runs a `.py` script — unmodified CPython 3.10.20.**
  tobyOS now runs a genuine, off-the-shelf **CPython 3.10.20** (the public, reproducible `astral-sh/python-build-standalone`
  build) — a dynamic musl PIE whose interpreter is `/lib/ld-musl-x86_64.so.1`, the *same* musl loader `busybox-dyn` already
  uses (musl has no symbol versioning, so no GLIBC-style DSO version-matching). The whole interpreter is **two files**: the
  18 MB `python3` (with libpython statically linked in) plus a 2.9 MB `python310.zip` holding the pure-python standard library —
  CPython's **built-in `zipimport`** reads modules straight out of that archive on the tobyOS VFS, so there's no need to stage
  thousands of stdlib files. We run an actual file, `python3 -S -B /hello.py`, that builds a list via a comprehension, sums it
  (`140`), **imports `json` from the zipimport archive** and serializes a dict, prints a real **`PYTHON-OK ver=3.10.20
  sum=140 json={"n": 7, "sum": 140}`** banner, and `sys.exit(140-98)` → 42, so a PASS proves the bytecode VM actually executed,
  not merely that the binary started. **Kernel gap filled:** `mremap` (syscall 25) was unimplemented — glibc/musl `realloc` and
  CPython's obmalloc arena resize use it; added a real `linux_mremap()` (shrink frees the tail in place; grow with
  `MREMAP_MAYMOVE` allocates a fresh anon region, copies the old bytes across, unmaps the old). This milestone also rode on the
  `realtools` inode fix — CPython opens its loader + the zip and would have aliased them under the old `st_ino = 1` dedup bug.
  **Proof, clean under `+smep,+smap`, 0 faults / 0 unhandled syscalls:** `[REALPYTHON] VERDICT: PASS exit=42`
  (`logs/realpython.sh`). **No regression** (`mremap` is additive to the syscall dispatch): the full Linux-track regression
  (`logs/bashregress.sh`) stays green. *Opt-in:* the interpreter + stdlib zip are gitignored; `programs/realpython/build.sh`
  fetches + zips them and the initrd stages them only when present (the `hello.py` proof script is committed).
- **2026-06-27** — **Track B: SELF-HOSTING with a REAL, off-the-shelf C compiler — unmodified TinyCC 0.9.27 compiles + runs C in-VM.**
  tobyOS already had its *own* homegrown compilers (`tobycc`, the bundled `tcc` port); this runs the **genuine, third-party
  Fabrice Bellard TinyCC 0.9.27** (the Alpine build — itself a dynamic musl program that loads `libtcc.so`, so it also exercises
  the multi-DSO musl loader). It **compiles a C source *inside the VM*** — `tcc -static -nostdlib -o /data/cc_out.elf
  /hello_cc.c` — and then **tobyOS loads and runs the freshly-emitted ELF**. The source is freestanding on purpose (its own
  `_start`, raw x86-64 syscalls, no libc/crt/headers), so the compile depends on nothing but the compiler itself, and the output
  is a tiny static ELF whose Linux personality tobyOS **auto-detects** (same path as `LXSAUTO`). A PASS means tcc turned C into
  machine code + a valid ELF *on tobyOS*, and that ELF ran and printed **`CC-OK built-by-real-tcc`** and exited 42 — end-to-end
  self-hosting with a real toolchain. **Proof, clean under `+smep,+smap`, 0 faults / 0 unhandled syscalls** (the `realtools`
  inode fix + the proven musl multi-DSO path meant *no new kernel gaps* were needed): `[REALCC] VERDICT: PASS exit=42`
  (`logs/realcc.sh`). **No regression:** this milestone makes *zero* kernel-core changes — the only `kernel.c` edit is the new
  harness inside `#ifdef REALCC_BOOT`, which is not defined in default/regression builds, so default boot logic is unchanged.
  *Opt-in:* the TinyCC binary + `libtcc.so` + `libtcc1.a` are gitignored; `programs/realcc/build.sh` fetches them from the
  Alpine package mirror and the initrd stages them only when present (the `hello_cc.c` proof source is committed).
- **2026-06-27** — **Track C (graphics): the virtio-gpu driver + GPU-accelerated present path proven HEADLESSLY — `[VIRTGPU] VERDICT: PASS`.**
  The `src/virtio_gpu.c` driver (modern non-transitional virtio, vid:did `1af4:1050`) was already implemented and wired, but its
  only proof was the **interactive** `make run-virtio-gpu` target — nothing automated/greppable. This adds a deterministic,
  headless proof. Booting `-vga none -device virtio-gpu-pci`, the driver binds during `pci_bind_drivers`, parses the modern virtio
  cap chain, negotiates features (virgl=no on this host), brings up scanout 0 (**GET_DISPLAY_INFO → RESOURCE_CREATE_2D →
  RESOURCE_ATTACH_BACKING → SET_SCANOUT**, 1280×800 BGRX backed by 1000 physically-contiguous PMM pages), and — once
  `gfx_layer_init()` runs — `virtio_gpu_install_backend()` hooks `gfx_flip()` so every present goes through **TRANSFER_TO_HOST_2D +
  RESOURCE_FLUSH** on the GPU instead of the Limine memcpy fallback (the HW cursor plane and per-window GPU resources come up too).
  The new `#ifdef VIRTGPU_BOOT` harness drives a burst of 24 frames through the compositor's `gfx_flip()` and **asserts the GPU
  present counters actually advanced** (a new `virtio_gpu_proof_stats()` accessor reports `full`/`partial` present counts + live
  backend state) — so a PASS proves real GPU commands flowed, not that the binary merely started. **Proof, clean under
  `+smep,+smap`, 0 faults / 0 unhandled syscalls:** `present=1 backend_active=1 scanout=1280x800`, `presents full=25 partial=0
  (delta=24)`, `[VIRTGPU] VERDICT: PASS` (`logs/virtgpu.sh`). **No regression:** the only always-compiled change is the additive
  read-only `virtio_gpu_proof_stats()` (never called in default builds); the harness lives inside `#ifdef VIRTGPU_BOOT`
  (undefined in default/regression builds), so default boot logic is unchanged.
- **2026-06-27** — **Track C (terminal): a REAL, interactive FULL-SCREEN ncurses app runs over the console — unmodified GNU `nano` 9.1 — `[REALTUI] VERDICT: PASS`.**
  Previous Track-B proofs (bash/binutils/CPython/TinyCC) were line-oriented; the kernel framebuffer console was a teletype, not a
  terminal emulator (it ignored CUP/ED/EL/SGR/alt-screen), so no curses app could render. This turns **`src/console.c` into a real
  VT100/ANSI emulator**: `console_putc` is now a state machine handling cursor addressing (CUP/CUU…/CHA/VPA), erase (ED/EL/ECH),
  insert/delete (ICH/DCH/IL/DL), scroll regions (DECSTBM + SU/SD + reverse-index), SGR (16-colour + bright + 256-colour + reverse +
  default), the alternate screen, cursor show/hide and deferred (LCF) auto-wrap — all implemented as **pixel-block moves on the
  framebuffer itself** (the FB *is* the cell store, so no separate logical grid is needed). On top of it we run **unmodified GNU nano
  9.1** (Alpine, dynamic musl: `nano → libncursesw.so.6 → libc.musl`, the same loader busybox-dyn/CPython/TinyCC use — so it also
  re-exercises the multi-DSO musl path), with Alpine's `ncurses-terminfo-base` staged at `/usr/share/terminfo` and `TERM=linux`.
  The `#ifdef REALTUI_BOOT` harness runs **before** `m14_init()` (no desktop/login competing for the framebuffer or keyboard),
  injects a few typed lines, and `proc_wait`s; nano queries `TIOCGWINSZ` (160×100), loads its terminfo and paints the title bar +
  edit body + two-row shortcut bar straight onto the framebuffer. The proof harness (`logs/realtui.sh`) screenshots the rendered UI
  via QMP, then injects **^X + 'n'** over the real keyboard (QMP `send-key`) so nano hits its "Save modified buffer?" prompt and
  exits cleanly. A new `console_vt_stats()` accessor lets the harness assert the emulator actually processed a full-screen redraw.
  **Proof, clean under `+smep,+smap`, 0 faults / 0 unhandled syscalls** (also added `setfsuid`/`setfsgid` stubs nano probes):
  `nano exit=0, vt CSI ops=113`, `[REALTUI] VERDICT: PASS` + a screenshot of the live nano UI. **No regression** (default build still
  boots to the desktop/login, 0 faults): the only always-compiled changes are the additive VT100 emulator (the NORMAL-state path is
  behaviourally identical for escape-free kernel logs) + the two ID stubs; the harness is gated by `#ifdef REALTUI_BOOT`.
- **2026-06-27** — **Track B: a REAL, off-the-shelf Linux *tool* runs end-to-end — GNU Binutils `readelf`, unmodified.**
  B25/B26 ran glibc proof binaries *we wrote*; this runs an actual third-party program we did **not** author: GNU Binutils
  **`readelf` 2.43.1** (~1.1 MB), lifted byte-for-byte from a Bootlin x86-64 glibc 2.41 toolchain. It is a dynamic PIE
  (`PT_INTERP=/lib64/ld-linux-x86-64.so.2`, `NEEDED libc.so.6`), so it re-drives the whole B26 glibc dynamic-loader path **and**
  a large real-world libc surface the toy proofs never touch: `getopt_long` option parsing, locale/gettext probing, the full
  `printf` family, and — the tool's actual job — `open()`/`mmap()`/parse of *another* ELF off the initrd, formatted to stdout.
  Run three ways, each `exit=0`: `readelf --version` → `GNU readelf (GNU Binutils) 2.43.1`; `readelf -h /bin/hello` →
  `Class: ELF64 / Type: EXEC / Machine: Advanced Micro Devices X86-64`; `readelf -dl /bin/readelf` → the `INTERP` segment +
  `NEEDED libc.so.6`. **Kernel gap fixed:** `pread64` (syscall **17**) — and its sibling `pwrite64` (18) — were unimplemented
  (`-ENOSYS`). glibc's loader (`dl-load.c open_verify`) `pread64()`s every DSO's ELF header/phdrs, and `readelf` `pread64()`s
  the file it inspects, so both died immediately. Added `sys_pread64`/`sys_pwrite64`: positioned I/O that saves/restores
  `file.vfs.pos` so the current offset is undisturbed, `ESPIPE` on non-seekable fds — matching Linux. **Proof, clean under
  `+smep,+smap`, 0 faults / 0 unhandled syscalls:** `[REALTOOL] VERDICT: PASS pass=3/3` (`logs/realtool.sh`). **No regression**
  (touches the core syscall dispatch): static musl busybox `[LXBB] PASS 10/10`, dynamic musl busybox `[LXDYN] PASS 7/7`, static
  glibc `[LXGLIBC] PASS exit=63`, dynamic glibc `[LXGLIBCDYN] PASS exit=127`, and a stock desktop boot reaches login with 0
  panics. *Opt-in:* the GNU binary + glibc runtime are gitignored (need the external toolchain); the initrd stages them under
  `/lib64` only when present (`programs/realtool/build.sh`).
- **2026-06-27** — **Track B milestone B26: a REAL glibc *DYNAMIC* binary runs — ld-linux.so.2 + shared libc.so.6.**
  B25 ran static glibc; B26 closes the single biggest remaining Linux-app gap by running an actual **glibc 2.41** *dynamically*
  linked PIE — the shape essentially every off-the-shelf Linux distro binary takes. The ELF is UNBRANDED (`EI_OSABI=SysV`) and
  declares `PT_INTERP=/lib/ld-linux-x86-64.so.2` + `DT_NEEDED libc.so.6/libm.so.6`; the kernel loads BOTH the PIE and glibc's
  ld.so and hands ld.so the auxv, and glibc's loader self-relocates, opens + file-backed-mmaps `libc.so.6` from the initrd
  `/lib`, relocates it, builds the DTV/TLS, and resolves PLT entries. A PASS therefore *also* proves the PT_INTERP-based
  Linux-personality auto-detect fires for a stock dynamic glibc binary. The proof goes one step past startup: a **runtime
  `dlopen("libm.so.6")` + `dlsym("cos")`** forces ld.so to open/map/relocate a *fresh* DSO long after startup — the hardest
  dynamic-loader path there is. **Root-cause kernel fix (general correctness):** glibc's ld.so minimal allocator
  (`__minimal_malloc`) places its first objects — including the main executable's `link_map` — in the tail of ld.so's own last
  BSS page (`&_end .. page-end`), trusting the OS to have zero-filled it (Linux guarantees this). tobyOS's ELF loader
  (`elf.c`) mapped fresh `pmm_alloc_page()` frames *without zeroing* and only zeroed `[filesz, memsz)`, leaving the page tail
  `[memsz, page-end)` as stale physical-page content (old code). That garbage landed in `link_map->l_info[]`, and `dl_main`'s
  dynamic-info adjust loop `#GP`'d on the non-canonical pointer. Fix: zero from end-of-file-data all the way to the
  page-rounded segment end, matching Linux's zero-fill-to-page-boundary — correct for *every* ELF, not just glibc. **Proof,
  clean under `+smep,+smap`, 0 faults:** `[LXGLIBCDYN] VERDICT: PASS exit=127` (`logs/b26.sh`) — all 7 checks: stdio, heap,
  float `sprintf`, `clock_gettime`, `getpid`, **pthread** (clone3+futex+TLS through the shared libc), and **runtime dlopen**.
  **No regression** (the loader change touches a core path, so the full dynamic battery was re-run): static musl busybox
  `[LXBB] PASS 10/10`, **dynamic musl busybox `[LXDYN] PASS 7/7` (ld-musl path unaffected)**, static glibc `[LXGLIBC] PASS
  exit=63`, and a stock desktop boot runs every native-ELF boot demo with 0 panics. *Opt-in:* the dynamic ELF + glibc
  `.so`s are gitignored (need the external sysroot); the initrd stages them only when present (`programs/linux-glibc-dyn/build.sh`).
- **2026-06-26** — **Track X capstone X2: one pipeline through ALL THREE personalities — Windows .exe -> Linux tool -> tobyOS GUI.**
  X1 proved Windows<->Linux pipelines over kernel pipes; X2 is the signature tobyOS trick taken to its conclusion: a single
  busybox `sh` pipeline whose bytes flow through a genuine Win32 PE, a real Linux/musl binary, AND a native tobyOS GUI app no
  other OS can run — on one kernel. The pipeline is `/bin/win-xprod.exe | busybox sed 's/XPIPE/X2/' | /bin/x2gui`: the Windows
  PE `WriteFile`s `XPIPE_TOKEN_OK`, busybox `sed` (musl Linux) transforms it to `X2_TOKEN_OK`, and **`x2gui`** — a new native
  app speaking the tobyOS `SYS_GUI_*` syscalls — reads the bytes off its stdin pipe, renders them into a real compositor
  window (`sys_gui_create`/`fill`/`text`/`flip`), and *proves the render* by reading its own window backbuffer pixels back
  (`SYS_GUI_GETPIXELS`) and counting glyph ink. `x2gui` is the pipeline tail, so its exit code becomes the pipeline's: a 3-bit
  mask, `7` = got input | Linux stage transformed the token | GUI render verified. **Kernel gap fixed:** the
  `ABI_SYS_GUI_GETPIXELS` syscall wrote its destination DIRECTLY, which faults under `CR4.SMAP`; it now stages each row through
  a kernel buffer and `copy_to_user`s it out (SMAP-safe), matching the rest of the uaccess surface. **Proof, 0 faults under
  `+smep,+smap`:** `[X2GUI] received='X2_TOKEN_OK'`, `[X2GUI] VERDICT: PASS token=yes rendered=yes inkpx=19`,
  `[X2PIPE] VERDICT: PASS exit=7` (`logs/x2.sh`). **No regression:** X1 cross-personality battery still `[XPIPE] PASS 3/3`;
  GUI desktop boots normally (login + app windows created). `x2gui` builds in-tree (freestanding clang), no external toolchain.
- **2026-06-26** — **Track B milestone B25: a REAL glibc static binary runs — not musl, not freestanding.**
  Until now every Linux proof was either freestanding (hand-rolled syscalls) or musl (busybox). B25 closes that by running an
  actual **glibc 2.41** statically-linked x86-64 ELF (linked against a Buildroot 2025.08 glibc sysroot with the in-tree
  clang/lld — see `programs/linux-glibc/build.sh`). glibc is the most demanding mainstream libc, so this exercises the whole
  C-runtime startup it does that musl skips: TLS setup via `arch_prctl`, `__libc_start_main`, `rseq`/robust-list registration,
  `brk`-backed malloc arena, and `clock_gettime`/`getrandom` probing. **Gap-fills** surfaced and implemented in `syscall.c`
  (glibc previously got `-ENOSYS` and limped along on fallbacks): `clone3(435)` (the modern `pthread_create` entry — translate
  its `clone_args` to the existing clone-thread path, accounting for clone3 passing stack *base+size* vs legacy clone's stack
  *top*), `prlimit64(302)` + legacy `getrlimit/setrlimit` (report sane `RLIMIT_STACK`=8 MiB / `RLIMIT_NOFILE`=1024, accept sets
  as no-ops — glibc reads `RLIMIT_STACK` to size thread stacks), `madvise(28)` (advisory no-op success), and `rseq(334)` named
  as an intentional `-ENOSYS` (the standard contract glibc handles by disabling rseq — no longer logged as "unhandled").
  **Proof, clean under `+smep,+smap`, 0 faults, 0 unhandled syscalls:** `[LXGLIBC] VERDICT: PASS exit=63` (`logs/b25.sh`) — the
  glibc binary passes all six checks: `stdio` (buffered `printf`), `heap` (malloc/strcmp), `sprintf` (float `%.3f`), `clock`
  (`clock_gettime`), `pid` (`getpid`), and **`pthread`** (create+join: clone3→clone + futex + TLS, returns the expected value).
  **No regression:** full Linux-track battery green (`LXABI/LXSIG/LXMMAP/LXTHREAD/LXJOIN/LXPOLL` all PASS — confirming the
  thread changes didn't break musl threads), busybox file battery `[LXBB] PASS 10/10`, unbranded auto-detect `[LXAUTO] PASS`.
  *Opt-in:* the glibc ELF is gitignored (needs the external sysroot); the initrd stages it only when present, so normal builds
  never require the toolchain.
- **2026-06-26** — **Track B milestone B24: `/proc` + `/sys` breadth — the introspection nodes real tools read.**
  Broadens B20's `/proc` and the existing `/sys` so memory/uptime/mount/fd-introspecting software works. **`/proc`** (in
  `procfs.c`): `meminfo` rewritten to the Linux labelled-kB format (`MemTotal`/`MemFree`/`MemAvailable`/… in kB with a `kB`
  unit, what `free`/`top`/sysconf parse); `uptime` now emits the two-float form (`uptime idle`); new `loadavg`
  (`0.00 0.00 0.00 running/total lastpid`), `stat` (aggregate + per-CPU `cpu` lines, `btime`, `processes`, `procs_running`),
  and `mounts` (one line per VFS mount via `vfs_iter_mounts`). New **`/proc/[pid]/fd/`** directory: lists a symlink per open
  descriptor and `readlink`s each to its target (`/dev/console`, `/dev/ptmx`, `/dev/pts/N`, `pipe:[..]`, `socket:[..]`,
  `anon_inode:[eventpoll]`, the dir path) — wired through `procfs_stat`/`opendir`/`readdir`/`readlink`. **`/sys`** (in the
  existing `sysfs.c`) gains the Linux-layout nodes startup code reads: `/sys/devices/system/cpu/{online,possible,present}`
  (the cpulist `0`/`0-N` that glibc `get_nprocs()`/`sysconf(_SC_NPROCESSORS_ONLN)` consults) and `/sys/kernel/{ostype,osrelease}`.
  **Proof, clean under `+smep,+smap`, 0 faults:** `[LXPROCFS2] VERDICT: PASS exit=63` (`logs/b24.sh`) — a raw-syscall Linux ELF
  reads `meminfo` (asserts `MemTotal:` + `kB`), `uptime` (two fields), `mounts` (finds `/proc`), `stat` (`cpu …` + `processes`),
  enumerates `/proc/self/fd` (sees fds 0/1/2 + `readlink`s fd/0) and reads `/sys/.../cpu/online`. **No regression:** B20 procfs
  proof `[LXPROCFS] PASS exit=15`, full Linux-track battery green, busybox file battery `[LXBB] PASS 10/10`.
- **2026-06-26** — **Track B milestone B23: pseudoterminals — `/dev/ptmx` + `/dev/pts/N`, the Linux `openpty()` pair.**
  B21/B22 gave the *console* a controlling TTY; B23 lets userspace mint its own terminals — the substrate every terminal
  emulator, `script`, `expect`, `tmux` and `sshd` is built on. New `src/pty.c` implements `struct pty` pairs: two byte rings
  wired back-to-back (master write → slave input, slave write → master output) plus a per-slave `ktermios` running the same
  cooked line discipline as the console (canonical line editing + VERASE/VKILL/VEOF, raw/VMIN, ECHO at receive time, `ICRNL`
  on input / `ONLCR` on output). `sys_open` intercepts the device nodes: **`/dev/ptmx`** allocates a fresh pair and returns
  the master; **`/dev/pts/N`** returns slave N — exactly the glibc/musl `openpty()` handshake
  (`open(ptmx)` → `TIOCSPTLCK` unlock → `TIOCGPTN` → `open(/dev/pts/N)`). Two new `FILE_KIND_PTY_MASTER/SLAVE` with
  refcounted ends (dup/fork via `file_clone`), wired through `file_read/write/close`, `LX_ioctl`
  (`TCGETS/TCSETS`, `TIOCGWINSZ/TIOCSWINSZ`, `TIOCGPGRP/TIOCSPGRP`, `TIOCGPTN`, `TIOCSPTLCK`, `TIOCSCTTY`, `FIONREAD`),
  and poll/select (`POLLIN`/`POLLHUP` off live ring + peer-closed state). Blocking reads use the cooperative wait-queue
  protocol (signals → `EINTR`).
  **Proof, clean under `+smep,+smap`, 0 faults, 0 unhandled syscalls:** `[LXPTY] VERDICT: PASS exit=63` (`logs/b23.sh`) — a
  raw-syscall Linux ELF runs the full openpty handshake, drives **master→slave** (write `hello\n` to master, cooked read on
  the slave returns exactly `hello\n`) and **slave→master** (write `world\n` on the slave, master reads `world\r\n` — the
  ldisc mapped NL→CRNL), round-trips the window size 40×120, and finally **forks a child whose stdio is the slave and
  `execve`'s a real `busybox sh -c 'echo PTY-SHELL'`, reading the output back off the master** — a genuine program running on
  a pseudoterminal. **No regression:** full Linux-track battery green (`LXABI…LXSAUTO`), busybox file battery `[LXBB] PASS 10/10`.
- **2026-06-26** — **Track B milestone B22: a REAL interactive Linux shell — `busybox sh -i` runs a read-eval loop over the B21 TTY.**
  B21 made the console a controlling TTY; B22 is the payoff: a genuine interactive shell. The blocker turned out to be the
  **terminal-query handshake** — busybox's line editor writes `ESC[6n` (Device Status Report → cursor position) and *blocks
  reading the reply*; a raw byte console never answers. Two fixes make it work: (1) `tty_console_output()` (now backing
  `FILE_KIND_CONSOLE` writes) scans output for `ESC[6n` and pushes a **cursor-position report** (`ESC[<row>;<col>R`, from the
  console cursor) into a **priority input-reply queue** that reads drain *before* the keyboard ring (served as its own burst so
  the editor parsing `…R` doesn't also swallow the user's typed line); (2) **poll/select now reports a console fd readable**
  when input is actually available (reply queue / pending cooked line / keyboard ring — via a new non-destructive `kbd_haschar()`
  + `tty_console_readable()`), where before a console fd only ever reported `POLLOUT`, so the editor's poll-before-read never woke.
  **Proof, clean under `+smep,+smap`, 0 faults, 0 unhandled syscalls:** `[LXISH] VERDICT: PASS exit=42` (`logs/b22.sh`) — the
  harness "types" a two-line session into the keyboard ring (`busybox true` ⏎ then `exit $(busybox echo 42)` ⏎) and spawns
  `busybox sh -i`; the shell answers the `ESC[6n` query, reads **both** lines from the TTY in its interactive loop, runs the
  command substitution (fork+exec `busybox echo`, captures `42`) and exits with it — proving an end-to-end interactive
  read-eval loop over the console, not just `sh -c`. **No regression:** full Linux-track battery green
  (`LXABI/LXSIG/LXMMAP/LXTHREAD/LXJOIN/LXPOLL/LXAUTO/LXSAUTO`, + `LXPROCFS`/`LXTTY` standalone), busybox file battery `[LXBB] PASS 10/10`.
- **2026-06-26** — **Track B milestone B21: the console is a real controlling TTY — Linux `termios` + a cooked line discipline.**
  Before B21 the console was a raw byte pipe and `ioctl` always returned `ENOTTY`, so a Linux libc decided stdin/stdout were
  *not* a terminal — `isatty()==0` → fully buffered, no line editing, no way to set raw mode. That blocks interactive Linux
  software (shells, editors, pagers, password prompts). B21 gives the kernel text console a single controlling TTY: (1) a real
  36-byte Linux `struct termios` (`src/tty.c`/`tty.h`) with sane cooked defaults (`ICANON|ECHO|ISIG`, standard `VINTR=^C`/
  `VERASE=DEL`/`VKILL=^U`/`VEOF=^D`/`VSUSP=^Z`), read/written by **`TCGETS`/`TCSETS`** — a working `TCGETS` is exactly what makes
  `isatty()` return true and lets a program flip raw mode; (2) a **cooked line discipline** in `tty_console_read()` (now backing
  `console_read`): canonical mode buffers a whole line with `VERASE`/`VKILL` editing + `ECHO` and `VEOF` for EOF, raw mode
  (ICANON cleared) delivers bytes immediately under `VMIN`; (3) **termios-driven signals** — the keyboard ISIG path
  (`tty_handle_isig`, replacing a hard-coded `^C/^\/^Z` block in `kbd_dispatch_char`) now fires `SIGINT`/`SIGQUIT`/`SIGTSTP`
  only while `ISIG` is set and only for the configured control chars, so a raw-mode app receives e.g. Ctrl-C as a literal data
  byte; (4) **`TIOCGWINSZ`** (terminal size from the console grid) + **`TIOCGPGRP`/`TIOCSPGRP`** (foreground group) + `TCFLSH`/
  `FIONREAD`; (5) wired Linux `ioctl(16)` on a `FILE_KIND_CONSOLE` fd to this TTY (non-console fds + unknown requests still get
  `ENOTTY`). **Proof, clean under `+smep,+smap`, 0 faults:** `[LXTTY] VERDICT: PASS exit=15` (`logs/b21.sh`) — `linux-tty`, a
  raw-syscall Linux ELF, calls `ioctl(0,TCGETS)` (isatty), flips raw mode via `TCSETS` and reads it back (verifying `ICANON`/
  `ECHO` actually cleared), queries `TIOCGWINSZ`, and does a **canonical read** that must cook the `hello\n` the harness injects
  into the keyboard ring into exactly one 6-byte line (proving the line discipline, not raw bytes). **No regression:** full
  Linux-track battery green (`LXABI/LXSIG/LXMMAP/LXTHREAD/LXJOIN/LXPOLL/LXAUTO/LXSAUTO/LXPROCFS/LXTTY` all PASS), real-musl
  busybox file battery `[LXBB] PASS 10/10`.
- **2026-06-26** — **Track B milestone B20: a minimal, dynamic `/proc` filesystem — `cpuinfo`, `self`, `[pid]/maps`, `[pid]/stat`, `[pid]/exe`.**
  Real Linux tooling probes `/proc` constantly (libc/runtime introspection, allocators reading `/proc/self/maps`, tools resolving
  `/proc/self/exe`); tobyOS had only a skeletal procfs. B20 makes `/proc` generate content from live kernel state. **`/proc/self`
  is now resolved dynamically** to the calling process's pid (a new `subst_self()` rewrites the path per-open against the current
  pid) instead of a stale boot-time static symlink. Added synthetic generators: **`/proc/cpuinfo`** (per-CPU stanzas from the SMP
  table), **`/proc/[pid]/maps`** (synthesized from the proc's image/heap/stack regions in canonical `addr-addr perms off dev ino path`
  form), and **`/proc/[pid]/stat`** (the space-separated field row). **`/proc/[pid]/exe` and `/proc/self/exe` are real symlinks**
  resolved on demand: `struct proc` gained an `exe_path[]` (the absolute path, latched at spawn in `proc.c` and at `execve`/`execve_pe`
  in `fork.c`), `struct vfs_ops` gained a `readlink` driver hook, `vfs_readlink` dispatches to it, and `procfs_readlink` answers
  `self`→pid and `[pid]/exe`→`exe_path`. Wired the Linux **`readlink(89)`/`readlinkat(267)`** syscalls (previously `-ENOSYS`) onto
  `vfs_readlink`. `procfs_opendir`/`readdir`/`stat`/`open` all run through `subst_self` and enumerate the new entries. **Proof, clean
  under `+smep,+smap`, 0 faults:** `[LXPROCFS] VERDICT: PASS exit=15` (`logs/b20.sh`) — `linux-procfs`, a raw-syscall Linux ELF, reads
  `/proc/cpuinfo`, `/proc/self/status`, `/proc/self/maps` and `readlink`s `/proc/self/exe` (→ `/bin/linux-procfs`); each of the four
  checks sets a bit, 15 = all four. **No regression:** combined Linux-track battery green
  (`[LXABI]/[LXSIG]/[LXMMAP]/[LXTHREAD]/[LXJOIN]/[LXPOLL]/[LXAUTO]/[LXSAUTO]/[LXPROCFS]` all PASS), and the real-musl busybox file
  battery is `[LXBB] PASS pass=10/10`.
- **2026-06-26** — **Track B milestone B19: static, note-less SYSV Linux binaries now DROP-AND-RUN — auto-detect the Linux ABI from `PT_GNU_STACK`.**
  This closes the last `elf_is_linux_abi` gap left open by B10. A genuine Linux binary that is *static* (no `PT_INTERP`),
  *note-less*, and *unbranded* (`e_ident[EI_OSABI]==0`, i.e. `ELFOSABI_SYSV`) has neither the `brandelf` tag (rule 1) nor a
  Linux-loader `PT_INTERP` (rule 2), so before B19 it fell to the native tobyOS personality and mis-dispatched its very first
  syscall. The insight: tobyOS's own native programs build for the bare-metal `x86_64-elf` target, whose linker emits **no**
  `PT_GNU_*` program headers, whereas every `x86_64-*-linux-*` link (even `-nostdlib`/`-static`) emits a **`PT_GNU_STACK`**
  header. So B19 adds rule 3: a **static** binary (`!has_interp`) carrying a GNU OS-specific phdr (`PT_GNU_STACK`/`RELRO`/`PROPERTY`)
  is treated as Linux. `do_elf_load` records `info->has_gnu_phdr` during its phdr scan; `elf_is_linux_abi()` gains a
  `has_gnu_phdr` parameter; both callers (`proc.c` spawn, `fork.c` execve) pass it. **The `!has_interp` guard is essential and
  was found the hard way:** native *dynamic* programs (linked against `/lib/libtoby.so` via `/lib/ld-toby.so`) *do* carry
  `PT_GNU_STACK`, and an ungated rule misclassified `c_dynhello` as Linux and page-faulted the kernel — gating to interp-less
  binaries keeps them native (they're already classified correctly by rule 2). Verified across the whole initrd: **0 native
  *static* ELFs carry `PT_GNU_STACK`**, so the rule never fires on a native binary. **Proof, clean under `+smep,+smap`, 0 faults:**
  `[LXSAUTO] VERDICT: PASS exit=73` (`logs/b19.sh`) — `linux-static-auto`, a genuine static raw-syscall Linux ELF, is first
  asserted by the harness to be genuinely ambiguous (`EI_OSABI=0`, no `PT_INTERP`, `PT_GNU_STACK` present), then spawned with
  no brand and no interp; it auto-detects Linux from the GNU phdr alone, runs its Linux syscalls (`arch_prctl`/`set_tid_address`/
  `write`/`writev`/`exit_group`) and exits 73. **No regression:** combined Linux-track battery green —
  `[LXABI]/[LXSIG]/[LXMMAP]/[LXTHREAD]/[LXJOIN]/[LXPOLL]/[LXAUTO]/[LXSAUTO]` all PASS, and `c_dynhello` runs native (exit 0).
- **2026-06-26** — **Track B milestone B18: socket depth — a real `getsockopt` + scatter-gather `sendmsg`/`recvmsg`.**
  The Linux socket layer had two stubs/gaps that real DNS/HTTP libraries lean on: `getsockopt` was an accept-and-ignore
  no-op (returned 0, left `*optval` untouched — so `SO_ERROR`/`SO_TYPE` readers got uninitialised junk), and
  `sendmsg`/`recvmsg` (syscalls 46/47) were unhandled (`-ENOSYS`). B18 adds (1) a real `lx_getsockopt(SOL_SOCKET,…)`
  answering `SO_ERROR` (0), `SO_TYPE` (`SOCK_STREAM`/`SOCK_DGRAM` from the socket kind), `SO_ACCEPTCONN` (1 iff
  `listen()`ing), `SO_RCVTIMEO`/`SO_SNDTIMEO` readback (as a `timeval`), and `SO_SNDBUF`/`SO_RCVBUF`/`SO_REUSEADDR`/…
  readbacks — honouring the caller's `*optlen`; and (2) `lx_sendmsg`/`lx_recvmsg` implementing the scatter-gather data
  path: a bounded iovec array (≤16, capped at `SYS_MAX_RW`) is gathered into one kernel buffer for send (UDP dest from
  `msg_name` or the connected peer; TCP via `tcp_send`) and the received bytes are scattered back across the iovecs for
  recv (UDP sender written into `msg_name`, `msg_namelen`/`msg_controllen`/`msg_flags` written back); ancillary data
  (`msg_control`/SCM_RIGHTS) is not supported and is reported cleared. **Proof, clean under `+smep,+smap`, 0 faults,
  0 unhandled syscalls:** `[LXMSG] VERDICT: PASS exit=56` (`logs/b18.sh`) — a genuine raw-syscall Linux ELF
  (`linux-msgsock`) asserts `getsockopt(SO_TYPE)==SOCK_DGRAM` and `SO_ERROR==0` on a fresh UDP socket, resolves
  `example.com` via a DNS query `sendmsg`'d across 2 iovecs + the reply `recvmsg`'d into 2 iovecs, then TCP-connects
  (asserting `SO_ERROR==0`) and fetches `/` with `sendmsg`/`recvmsg` (`HTTP/1.1 200 OK`) — a **live internet round-trip
  over SLIRP**. No-regression: real busybox `nslookup`+`wget` still `[LXNETBB] PASS 2/2` (busybox calls `getsockopt`,
  now answered properly), and `validate.sh 2 90` (default ISO) = 2/2 ALIVE, exc/panic=0.
- **2026-06-26** — **Track X milestone X1: CROSS-PERSONALITY pipelines — a genuine Windows `.exe` and Linux binaries exchange data in ONE pipeline on one kernel.**
  The signature tobyOS feature: a single busybox `sh` pipeline mixes a real Windows PE and Linux binaries, with bytes
  flowing through kernel pipes across the personality boundary — something neither Windows nor Linux can do natively.
  The enabling kernel change is **`execve()` → PE**: until now only `proc_spawn` auto-detected the `MZ`/`PE` magic; a
  Linux/native `sys_execve` went straight to the ELF loader, so a shell could not launch a `.exe`. A new `execve_pe`
  arm (`src/fork.c`) loads the image via `pe_load_user`, maps a 256 KiB Win32 stack, flips the process to
  `ABI_PERS_WIN32`, installs the PE's TEB/GS + resource/TLS metadata, and enters at RSP%16==8 — mirroring proc.c's
  spawn arm. The proven ELF/Linux execve path is left byte-for-byte unchanged (the PE branch is taken only for `MZ`
  images). One subtlety vs. spawn: execve `iretq`s straight to ring 3 via `proc_enter_user_asm` instead of going
  through `do_switch`, so the per-proc GS base never reaches the SWAPGS shadow MSR — `execve_pe` therefore
  `wrmsr(IA32_KERNEL_GS_BASE, teb)` itself before the jump, else the CRT's `gs:[0x30]` (NtCurrentTeb) reads stale
  kernel per-CPU data. Three **stock** Windows PEs (`-nostdlib`, kernel32-only, byte-for-byte normal `.exe`s) act as
  pipeline stages — `win-xprod` (WriteFile→stdout), `win-xcons` (ReadFile←stdin, asserts, exit 0/1), `win-xfilter`
  (ReadFile→transform→WriteFile). The Win32 `WriteFile`/`ReadFile` shims already forward to `sys_write`/`sys_read`, so
  a PE's std handles ride the same pipe fds busybox sets up with `pipe2`+`dup2` (B12). **Proof, clean under
  `+smep,+smap`, 0 faults, 0 unhandled syscalls:** `[XPIPE] VERDICT: PASS pass=3/3` (`logs/x1.sh`) —
  `win->linux` (`/bin/win-xprod.exe | busybox grep -q XPIPE_TOKEN_OK`), `linux->win`
  (`busybox echo … | /bin/win-xcons.exe`), and the showcase `linux->win->linux`
  (`busybox echo … | /bin/win-xfilter.exe | busybox grep -q WIN-SAW:…`, TWO personality flips in one pipeline). The
  serial log shows each forked shell stage `[execve] … now running Win32 PE …`. No-regression: the Linux execve path
  is exercised by the pure-Linux `echo`/`grep` stages inside these pipelines (all green), proc.c's PE-spawn path is
  untouched, and `validate.sh 2 90` (default ISO) = 2/2 ALIVE, exc/panic=0.
- **2026-06-26** — **Track B milestone B17: REAL off-the-shelf busybox does network I/O — `nslookup` + `wget` over the B16 socket path.**
  B16 proved the client path with a first-party ELF; B17 validates it with **genuine, unmodified musl-libc third-party
  software**. Running real busybox `nslookup example.com` and `wget http://example.com/` surfaced and fixed three exact
  gaps in how real libc uses sockets: (1) **`bind(port=0)`** — musl/busybox resolvers bind a UDP socket to port 0 ("any
  ephemeral"), but `sock_bind` rejected 0 as invalid (`nslookup: bind: Address in use`); now port 0 assigns an ephemeral
  port (BSD semantics). (2) **`socket(AF_INET6,…)` → `EAFNOSUPPORT`** — musl's `getaddrinfo` opens an IPv6 DNS socket
  first and only falls back to IPv4 on `EAFNOSUPPORT`; we returned `EINVAL`, so the fallback never happened (`wget: bad
  address`). (3) **`setitimer`/`getitimer`/`ftruncate`** — busybox arms an alarm-timeout around each network op and
  ftruncates the wget output file; these are now accept-and-ignore (the watched I/O has its own timeouts; wget's content
  is written by `write()` regardless), removing the `-ENOSYS` spam. **Proof, clean under `+smep,+smap`, 0 faults, 0
  unhandled syscalls:** `[LXNETBB] VERDICT: PASS pass=2/2` (`logs/b17.sh`) — real busybox resolves the name over UDP DNS
  and fetches the page over a TCP active-open + HTTP, a **live round-trip to the real internet over SLIRP**. No-regression:
  the busybox file battery is still `[LXBB] PASS 10/10`, Linux track `LXABI`/`LXSIG`/`LXMMAP` PASS, `validate.sh 3 80`
  (default) = 3/3 ALIVE, exc/panic=0.
- **2026-06-26** — **Track B milestone B16: networking CLIENT capstone — a Linux binary resolves DNS and fetches HTTP over real sockets.**
  B14 proved the *server* half of the Linux socket stack (passive open: bind/listen/accept/epoll). B16 completes the
  *client* half so a Linux binary can reach out to the network the way real tooling (curl/wget/getaddrinfo) does. (1)
  **UDP is now a first-class Linux socket.** `lx_connect`/`sendto`/`recvfrom`/`send`/`recv` (and the connected
  `read`/`write` byte forms) handle `SOCK_DGRAM`: `connect()` records the peer, `sendto` takes the destination from its
  5th arg (the 6th — addrlen — still isn't delivered, so it's assumed `sizeof(sockaddr_in)`) or the connected peer, and a
  new **bounded `sock_recvfrom_to()`** gives recv a real timeout (driven off `SO_RCVTIMEO`) instead of blocking forever.
  This is exactly what a DNS resolver needs. (2) **`/etc/resolv.conf` is published** so libc resolvers (musl
  `getaddrinfo`) find the nameserver; the initrd ships a static one (SLIRP's `10.0.2.3`, which DHCP also hands out) and
  `net_apply_lease` best-effort rewrites it from the live lease on any writable root (today the initrd root is read-only
  ramfs, so it's the static file). (3) **TCP active-open** (`connect` → `tcp_connect`) is exercised end-to-end. **Proof,
  clean under `+smep,+smap`, 0 faults:** `[LXHTTP] VERDICT: PASS exit=55` — a genuine raw-syscall Linux ELF
  (`/bin/linux-httpget`, `logs/b16.sh`) reads `/etc/resolv.conf`, sends a UDP DNS `A` query and parses the answer, then
  does a TCP `connect()` + `GET / HTTP/1.0` and checks the reply begins `HTTP/`. Over QEMU SLIRP this is a **live
  round-trip to the real internet** (observed: `example.com -> 104.20.23.154`, `HTTP/1.1 200 OK`). Honest scope: the
  proof targets a public host, so it needs the host to have working internet; the static resolv.conf nameserver is
  SLIRP-correct but not auto-derived on a real LAN until the root fs is writable. No-regression: full Linux track green
  (`LXABI`/`LXSIG`/`LXMMAP` PASS), the B14 TCP server still accepts/echoes, and `validate.sh 3 90` (default) = 3/3
  ALIVE, exc/panic=0. (Side note confirmed during B16: the B14 active-close *exit-code* verdict can lag past the harness
  window on this Windows host — reproduced identically on the committed pre-B16 kernel, so it's an environmental TIME_WAIT
  timing artifact, not a regression.)
- **2026-06-26** — **Track B milestone B15: signal completeness — Linux-layout `SA_SIGINFO` + catchable synchronous CPU faults.**
  Two gaps blocked real signal-using programs (crash handlers, JITs, GC/runtime preemption). (1) **`SA_SIGINFO` 3-arg
  handlers now get the x86-64 *Linux* `siginfo_t`/`ucontext_t` layout.** The native path already passed `&info`/`&uctx`
  in RSI/RDX, but always in *tobyOS-native* struct layout — a Linux binary read garbage at the Linux field offsets. A
  new personality-aware writer (`sig_emit_info_uctx`) emits the structures byte-for-byte per the Linux uapi for
  `ABI_PERS_LINUX` procs (siginfo `si_signo`/`si_errno`/`si_code` + the `_kill`/`_sigfault` union; `ucontext.uc_mcontext.gregs[]`
  in the fixed `REG_*` order incl. `REG_CSGSFS`=user `%cs` and `REG_CR2`), and keeps the native layout unchanged for
  toby/Win32 procs. (2) **Synchronous CPU faults are now delivered to user handlers.** Previously a ring-3 `#PF`/`#GP`/
  `#DE`/`#UD` that wasn't a demand-page/COW fault just `proc_exit`'d the process; now the exception dispatcher
  (`isr.c`) maps the vector to a signal (`SIGSEGV`/`SIGFPE`/`SIGILL`) and calls **`signal_deliver_fault()`**, which builds
  a signal frame from the exception trapframe and rewrites it so the trailing `iretq` enters the handler — with `si_addr`
  = the faulting `CR2` for `#PF`. If the process has no catchable handler it falls back to the exact prior fatal path
  (so default behavior is unchanged). `kill(getpid(), …)`/`raise()` now also report `si_pid`=sender (Linux-correct;
  `sys_kill` stamps the true sender, fixing the self-kill `si_pid=0` case). Limitation: tobyOS `sigreturn` restores from
  its own saved context, not the handler-visible `ucontext`, so report-and-exit / `longjmp` crash handlers work but
  in-place `ucontext`-driven resumption does not yet. **Proof, clean under `+smep,+smap`, 0 faults:** `[LXSIGINFO]
  VERDICT: PASS exit=42` — a genuine raw-syscall Linux ELF (`/bin/linux-siginfo`, `logs/b15.sh`) installs `SA_SIGINFO`
  handlers, checks an async `SIGUSR1`'s siginfo/ucontext at the exact Linux byte offsets (incl. `gregs[REG_CSGSFS]&0xffff
  ==0x33`), then dereferences an unmapped pointer and confirms the handler is entered with `si_addr`==the bad address —
  exit 42 only if both layouts are right *and* the synchronous fault was caught. No-regression: full Linux track green
  (`LXABI`/`LXSIG`/`LXMMAP`/`LXTHREAD` all PASS), and `validate.sh 3 90` (default) = 3/3 ALIVE, exc/panic=0.
- **2026-06-26** — **Track B milestone B14: networking capstone — Linux BSD sockets + epoll, and an event-driven TCP server.**
  B13 gave `poll`/`select`/`epoll`, but TCP-socket readiness was best-effort and the Linux personality had **no socket
  syscalls at all** — so a real network server couldn't be built. Two pieces close that: (1) **TCP readiness wired into
  the kernel poll predicate.** `file_poll_ready()` now consults the live TCP connection — a *listening* socket reports
  `POLLIN` when a handshake has completed (accept won't block; new `tcp_can_accept()`), and a *connected* socket reports
  `POLLIN`/`POLLOUT`/`POLLHUP`/`POLLERR` from data-buffered / established / peer-FIN / RST state (new `tcp_poll_flags()`).
  Crucially the cooperative wait loops now **pump `net_poll()` whenever a watched fd is a socket**, so inbound segments
  (the handshake completing on a listener, data arriving, a peer FIN) make progress while the process blocks in
  `epoll_wait` — the same RX pump `tcp_accept` already used. (2) **The Linux socket family is implemented** as real
  `FILE_KIND_SOCKET` fds (so `poll`/`epoll`/`read`/`write`/`close` all work uniformly): `socket`/`bind`/`listen`/
  `accept`/`accept4`/`connect`/`setsockopt`/`getsockname`/`getpeername`, plus `send`/`recv` (`sendto`/`recvfrom`) for the
  connected form, wrapping the kernel socket pool + `tcp.c` directly. A **connected TCP socket is now a byte stream** —
  `read()`==`recv()`, `write()`==`send()` (`file_read`/`file_write` route stream sockets to `tcp_recv`/`tcp_send`).
  **Proof, clean under `+smep,+smap`, 0 faults:** `[LXNETSRV] VERDICT: PASS exit=14` — a genuine raw-syscall Linux ELF
  (`/bin/linux-netserver`) runs an **epoll-driven TCP echo server** (`socket`→`bind :8080`→`listen`→`epoll_ctl(listen)`,
  then `epoll_wait` to accept and to recv), and a **REAL external client** (the QEMU host over SLIRP `hostfwd
  tcp::18082-:8080`, `logs/b14.sh`) connects and receives its bytes echoed back — reachable only if the handshake + recv
  readiness flowed through `epoll`. No-regression: full Linux track green (`LXABI`/`LXJOIN`/`LXPOLL` all PASS), and
  `validate.sh 3 90` (default) = 3/3 ALIVE, exc/panic=0.
- **2026-06-26** — **Track C milestone C23: wide-char / Unicode (UTF-16) — wide CRT strings, the `wprintf` family, and `%ls`.**
  Real Windows programs lean on `wchar_t`/UTF-16 (`wmain`, `wprintf`, `wcs*`, the `…W` APIs). The `…W` file APIs
  (`CreateFileW`, `WriteConsoleW`, `MultiByteToWideChar`, …) already worked, but the **wide CRT** was a near-total gap
  (only `wcslen`). Added, all operating on UTF-16 directly (down-converting U+0000–7F to ASCII, others `?`, matching the
  layer's existing policy): the **wide string family** `wcscmp`/`wcsncmp`/`wcscpy`/`wcsncpy`/`wcscat`/`wcschr`/`wcsrchr`;
  **wide stdio** `fputws`/`_putws`/`fputwc`/`putwchar`; and the **wide `printf` family** — `wprintf`/`fwprintf` route to
  ucrt's `__stdio_common_vfwprintf` and `swprintf`/`_snwprintf` to `__stdio_common_vswprintf`, both shimmed kernel-side.
  The wide formatter **reuses the narrow engine**: `win32_vformat` was refactored into a `win32_format_core` with a
  sink that targets either an fd or a capture buffer (so `sprintf`/`swprintf` capture), and the wide path **transcodes**
  the UTF-16 format string to a narrow one, rewriting the string/char conversions to their wide form (`%s`/`%c` →
  `%ls`/`%lc`; `%hs`/`%S` → narrow) so the core reads each vararg correctly. The narrow engine itself also gained
  `%ls`/`%S`/`%lc`/`%C` (wide string/char in a *narrow* `printf`, common in mixed-mode code). Also made shim resolution
  robust to ucrt **api-set forwarding**: an `api-ms-win-crt-*` import that misses an exact-DLL match now falls back to a
  name-only match against any `-crt-*` shim (e.g. `wcschr` imports from `-private-l1` in this build, `-string-l1` in
  another). **Proof, clean under `+smep,+smap`, 0 faults, no unresolved imports:** `[WINPE23] VERDICT: PASS exit=23` —
  first-party `win-wide23.exe` (built like `win-printf21`, `-D__USE_MINGW_ANSI_STDIO=0` so the wide stdio routes to the
  ucrt primitives → the kernel engine, confirmed via its import table) builds `L"Hello, wide"` with `wcscpy`/`wcscat`,
  validates it with `wcscmp`/`wcsncmp`/`wcschr`/`wcsrchr`/`wcslen`, `swprintf`s into a wide buffer and checks the code
  units, then emits via `wprintf` (`%ls`/`%d`/wide `%c`), narrow `printf("%ls")`, `fputws`, and `_putws` — exiting 23
  only if every wide-string result matched (else a distinct `9x`). The serial log shows the expected wide lines
  (`C23W|Hello, wide|len=11|X|`, `C23N|Hello, wide|`, …). The refactor is regression-checked: C21's float-printf proof
  still `PASS exit=21 match=1`, and `validate.sh 3 75` (default) = 3/3 ALIVE, exc/panic=0.
- **2026-06-26** — **Track B milestone B13: readiness multiplexing — `poll`/`ppoll`/`select`/`pselect6`/`epoll` work.**
  The event-loop primitive every server / interactive Linux program reaches for, and previously all `-ENOSYS`. Added one
  readiness predicate `file_poll_ready(struct file*)` (regular files always ready; pipes exact — POLLIN on buffered data,
  POLLHUP at EOF, POLLOUT on free space, POLLERR on a dead reader, all off the live ring + reader/writer counts; UDP
  sockets POLLIN off the queued-datagram count + always POLLOUT) and wired six syscalls on top of it: **`poll`(7)** and
  **`ppoll`(271)** (timespec timeout), **`select`(23)** and **`pselect6`(270)** (fd_set bitmaps, capped at `PROC_NFDS`),
  and a real **`epoll`** — `epoll_create`(213)/`epoll_create1`(291) mint a new `FILE_KIND_EPOLL` fd backed by a kmalloc'd
  interest list, `epoll_ctl`(233) does ADD/MOD/DEL, and `epoll_wait`(232)/`epoll_pwait`(281) fill the packed 12-byte
  `epoll_event[]`. All five block cooperatively (`sched_yield` between scans, honoring the timeout via `perf_now_ns` and
  bailing `-EINTR` on a pending signal); because the scheduler is cooperative, yielding lets the pipe's other end / a
  peer proc run and make the fd ready, observed on the next pass. **Honest scope:** TCP-socket POLLIN and tty/console
  *input* readiness are best-effort (reported writable; their input side isn't peekable here) — a documented limit, not
  hit by the proof. **Proof, clean under `+smep,+smap`, 0 faults, 0 unhandled syscalls:** `[LXPOLL] VERDICT: PASS exit=0`
  — a hand-rolled raw-syscall Linux ELF (`/bin/linux-poll`, no libc) runs a 7-check battery on a real pipe: `poll(POLLIN,
  0)` on an empty pipe → 0; `poll(POLLIN, 30ms)` → 0 (the timeout path); `write` then `poll` → `POLLIN`; `poll(POLLOUT)`
  on the write end → ready; `select` with the read fd set → ready; `epoll_create1`+`epoll_ctl(ADD,EPOLLIN)`+`epoll_wait`
  → 1 event with the registered `data` tag intact; then close the writer + `poll` → `POLLHUP` (EOF). It exits 0 only if
  every check matched. **No regressions:** full Linux track green incl. `LXPOLL`; `validate.sh 3 75` (default) = 3/3
  ALIVE, exc/panic=0 (these are Linux-personality-only syscalls; native unaffected).
- **2026-06-26** — **Track B milestone B12: shell PIPELINES — `busybox sh -c 'a | b | c'` works end-to-end.**
  B8 gave the shell `fork`/`execve`/`wait4`, but a *pipeline* additionally needs each stage to wire the pipe onto
  stdin/stdout with `dup2`, and the missing piece was exactly that: the Linux syscall layer had `pipe`(22) and `dup`(32)
  but **`dup2`(33), `dup3`(292), and `pipe2`(293) were unrouted** — so ash could create a pipe but couldn't connect it,
  and any pipeline failed. Added all three to `linux_syscall` (`dup2`/`dup3` → the native `SYS_DUP2`, with `dup3`'s
  `oldfd==newfd` → `EINVAL` and its `O_CLOEXEC` flag accepted-but-untracked since stages `execve` immediately; `pipe2`
  → `SYS_PIPE`, its `O_CLOEXEC`/`O_NONBLOCK` flags moot for the same reason). **No other change was needed:** the
  circular-buffer pipe (`pipe.c`) already blocks/wakes correctly and reports EOF/EPIPE off reader/writer refcounts, and
  `file_clone`/`file_close` already bump/drop those counts across `fork`+`dup`, so EOF fires only when the *last* writer
  closes — the whole multi-stage rendezvous falls out of existing primitives once the fds can be wired.
  `proc_any_child`+`proc_wait` already handle the shell's multi-child `wait4(-1)` loop (terminated children reaped
  first, else block on a live one; concurrent stages always make progress). **Proof, clean under `+smep,+smap`, 0
  faults, 0 unhandled syscalls:** `[LXPIPE] VERDICT: PASS pass=3/3` — the real static `busybox sh` runs three pipelines
  where the **last stage asserts the bytes that traversed every pipe**: `echo pipe-flows-ok | grep -q pipe-flows-ok`
  (2-stage), `echo alpha bravo charlie | cat | grep -q 'alpha bravo charlie'` (3-stage, content preserved through an
  intermediate), and `echo w x y z | wc -w | grep -q 4` (3-stage, a real word-count transform reaches the asserting
  stage) — each exits 0 only if the pipe/dup2 wiring works and the data flowed in order across the forked+exec'd stages.
  **No regressions:** full Linux track green incl. `LXSH`/`LXPIPE`; `validate.sh 3 75` (default) = 3/3 ALIVE,
  exc/panic=0 (these are Linux-personality-only syscalls; native unaffected).
- **2026-06-26** — **Track B milestone B11: `pthread_join` — the Linux thread-exit futex rendezvous now works.**
  Completes the threading story from B9 (`clone(CLONE_VM)` could *create* a thread, but nothing could *join* one): a
  glibc/musl `pthread_join` blocks in `futex(FUTEX_WAIT, &thread->tid, tid)` and relies on the kernel, at the joinee's
  exit, to write `0` to that word and `FUTEX_WAKE` it. tobyOS had `futex` (B-track) but the thread-exit half was a stub
  (`set_tid_address` returned the tid but stored nothing; `clone`'s `CLONE_CHILD_CLEARTID` was ignored). Added a single
  `clear_child_tid` field to `struct proc`, set by **`set_tid_address(2)`** (the caller's exit futex) and by
  **`clone(CLONE_CHILD_CLEARTID)`** (`ctid`, set explicitly in `sys_clone_thread` since the child is `memcpy`'d from the
  parent), reset on `execve` and fresh spawn; then **`proc_exit` honors it** — before disabling IRQs (so a demand fault
  on the word resolves cleanly) and while still in the exiting thread's CR3, it `copy_to_user`s a `0` and calls
  `futex(…, FUTEX_WAKE, 1)`. That's the exact contract every libc's `pthread_join` depends on, so it works for glibc and
  musl alike — no libc-specific code. **Proof, clean under `+smep,+smap`, 0 faults:** `[LXJOIN] VERDICT: PASS exit=0` —
  a hand-rolled raw-syscall Linux ELF (`/bin/linux-join`, no libc, so it exercises the *kernel* contract directly, not a
  libc shortcut) calls `set_tid_address`, spawns **4 joinable threads** with `clone(CLONE_VM|…|CLONE_PARENT_SETTID|`
  `CLONE_CHILD_CLEARTID)` each doing a `lock add` to a shared sum then `SYS_exit`, then **joins each via**
  `futex(FUTEX_WAIT, &tids[i], tid)`; it exits 0 only if the kernel cleared every `tid` slot to 0 *and* the shared sum
  equals `4×7` (all threads ran exactly once). Boot log shows the 4 `linux-join+T` threads exiting and the leader then
  exiting 0. **No regressions:** full Linux track green (`LXABI`/`LXSIG`/`LXMMAP`/`LXTHREAD`/`LXJOIN`/`LXBB`/`LXSH`/
  `LXDYN`/`LXMULTI`/`LXAUTO`); `validate.sh 3 75` (default build) = 3/3 ALIVE, exc/panic=0 (the `proc_exit` path is a
  no-op for the native userland, whose `clear_child_tid` is 0).
- **2026-06-26** — **Track B milestone B10: unbranded Linux ELFs now DROP-AND-RUN — auto-detect the Linux ABI from `PT_INTERP`.**
  Removes the last asymmetry between Windows and Linux foreign-binary support. A Windows `.exe` was always auto-detected
  by its `MZ`/`PE` magic (no prep), but a Linux ELF previously needed an explicit FreeBSD-style `brandelf` tag
  (`e_ident[EI_OSABI]=3`) — a vanilla off-the-shelf binary is `ELFOSABI_SYSV` (0), which fell to the *native* tobyOS
  personality and mis-dispatched its very first syscall. The fix: a single conservative helper, `elf_is_linux_abi()`
  (`elf.c`, declared in `elf.h`, called from BOTH spawn paths — `proc.c` and the `exec` path in `fork.c`), latches
  `ABI_PERS_LINUX` when **either** (1) the binary is branded `ELFOSABI_LINUX`/`GNU` (the original Track B mechanism,
  unchanged), **or** (2) its `PT_INTERP` basename is a known Linux dynamic loader (`ld-linux*` / `ld-musl*`). tobyOS's
  own dynamic loader is `/lib/ld-toby.so`, whose basename matches neither prefix, so native PIEs are **never**
  misclassified; the default stays native, and we only switch on positive evidence. This makes the realistic case —
  any **dynamic** glibc/musl Linux binary, which is virtually all real off-the-shelf software — truly "drop and run"
  with zero preparation. **Honest scope (later closed by B19, 2026-06-26):** a *static*, note-less `ELFOSABI_SYSV` binary
  carries no brand and no `PT_INTERP`, but it is *not* in fact indistinguishable from a native static binary — its
  GNU/Linux toolchain still emits a `PT_GNU_STACK` program header that the native bare-metal `x86_64-elf` toolchain never
  does (verified: 0 native static ELFs carry it). B19 adds that as rule 3 (gated to `!has_interp`), so static unbranded
  Linux binaries now drop-and-run too. **Proof, clean under
  `+smep,+smap`, 0 faults:** `[LXAUTO] VERDICT: PASS exit=0` — `/bin/busybox-auto` is a byte-for-byte copy of the musl
  busybox with `e_ident[EI_OSABI]` forced to **0** (genuinely unbranded). The boot harness first re-reads the file and
  asserts `EI_OSABI==0` (logs `on-disk EI_OSABI=0 (ELFOSABI_SYSV/unbranded)`), then spawns it; the loader logs
  `'busybox-auto' -> Linux ABI personality (EI_OSABI=0 interp=/lib/ld-musl-x86_64.so.1)` and the applet exits 0 — so a
  PASS can *only* mean it was auto-detected from the interp. **No regressions:** full Linux track all green —
  `LXABI`(branded static)=42, `LXSIG`/`LXMMAP`/`LXTHREAD`=0, `LXBB`=10/10, `LXSH`=0, `LXDYN`(branded dyn)=7/7,
  `LXMULTI`=0, plus the new `LXAUTO`; `validate.sh 3 75` (default build) = 3/3 ALIVE, exc/panic=0 (native userland
  unaffected, including `/lib/ld-toby.so` PIEs staying native).
- **2026-06-26** — **Track C milestone C22: formatted/number INPUT — the `scanf` family + `strtod`/`atof` now work.**
  The read-side complement to C20 (compute floats) and C21 (print floats), closing the float I/O round-trip. Before
  this, the kernel's `__stdio_common_vsscanf` shim was a **0-fields stub** and `strtod` always returned **`0.0`**
  (its own comment said "cannot return a double through the integer gate") — so no Windows program could parse a
  number from text. Added: (1) a from-scratch kernel **`scanf` engine** (`win32_vscanf` in `syscall.c`) supporting
  `%d/%i/%u/%x/%X/%o/%p/%c/%s/%f/%e/%g` + field width, `*` assignment-suppression, and `l`/`ll`/`h`/`L` length, reading
  one user pointer per conversion from the va_list and `copy_to_user`-ing each parsed value; wired behind both
  `__stdio_common_vsscanf` (input = a string) and `__stdio_common_vfscanf` (input = a `FILE*`; reads a bounded chunk
  via `file_read` then re-seeks the fd to just past the consumed bytes, so sequential `fscanf` on a file works).
  (2) **Real `strtod`/`atof`**: the decimal→double parse runs in the `-msse2` `kmath_strtod()` (same FP-accumulate
  algorithm as the native libc `strtod`, FXSAVE-guarded, result returned as bits); since `strtod`/`atof` are now in
  `win32_shim_is_double_ret()`, the **C20 float-return gate** does the `movq xmm0, rax` so the caller gets the double
  in `xmm0`, and the `endptr` is advanced past the consumed bytes. `scanf` itself returns an `int` field count and
  writes parsed doubles to user memory, so the engine needs no float gate — only `strtod`/`atof` (which *return* a
  double) do. Also added `strtoul`. **Proof, clean under `+smep,+smap`, 0 faults, 0 unresolved imports:** `[WINPE22]
  PASS exit=22` — first-party `win-scan22.exe` (built like `win-crt`, `-D__USE_MINGW_ANSI_STDIO=0` so the scanf/convert
  family routes to the kernel shims, not libmingwex in-process) runs an 11-conversion battery: `sscanf("42 -7 hello
  3.14159", "%d %d %s %lf")`, `strtod("2.71828rest", &end)` (checks `*end=='r'`), `atof("  -1.5e3")` → `-1500`,
  `%x`→255, `%f`→0.5, and an `fscanf` of `"100 2.5"` from a real file. It self-checks every field (returns `50+i` on
  the first failure) and the kernel re-reads `/data/c22.out` matching the exact text
  `C22|n=4|a=42|b=-7|w=hello|d=314159|e=271828|g=-1500|u=255|f=50|end=r|m=2|fi=100|fd2=25|` (floats scaled to integers
  via C21's `%d`). No regressions: full C1–C22 all-together = 39 PASS, 0 FAIL, 0 faults; `validate.sh 3 75` = 3/3
  ALIVE, exc/panic=0. App-compat ~52% → ~53%.
- **2026-06-26** — **Track C milestone C21: float PRINTING — the kernel Win32 `printf` engine now formats `double`s
  (`%f`/`%e`/`%g` + upper-case variants).** Closes the gap the C20 entry explicitly flagged as "still out of scope:
  float *printing*". The from-scratch kernel printf engine (`win32_vformat` in `syscall.c`, which backs ucrt's
  `__stdio_common_vfprintf` for any clang/ucrt PE) parsed flags/width/precision/length and handled `%d/%u/%x/%s/%c/%p`,
  but `%f/%e/%g` fell through to the "unknown conversion" path and were emitted verbatim. The double arrives as its
  8-byte IEEE-754 bit pattern in the next `va_list` slot (MS-x64 spills variadic doubles to the stack, which the C2
  gate already marshals), so no new marshalling was needed — only the decimal conversion, which needs FP. Since the
  engine is `-mno-sse`, the conversion lives in the existing `-msse2` `kmath.c` as `kmath_fmt_double()` (FXSAVE/FXRSTOR
  guarded like the rest of the unit, results materialised to memory/integers before `kmfpu_end`, exchanged as bit
  patterns); it renders the magnitude + reports the sign so the engine reuses its `emit_field` padding exactly like the
  integer conversions. The algorithm is ported from the native libc's `conv_float` (so the two formatters agree):
  `%g` picks `%e`/`%f` by decimal exponent and strips trailing zeros (unless `#`), `%f` of huge magnitudes falls back
  to `%e` to avoid u64 overflow, NaN/Inf render `nan`/`inf` (case-following the conversion). Also added `L` (long
  double == double on Windows) to the length-modifier parser. **Two proofs, both clean under `+smep,+smap`, 0 faults:**
  (1) `[WINPE21] PASS exit=21` — first-party `win-printf21.exe` (built like `win-crt`, `-D__USE_MINGW_ANSI_STDIO=0` so
  `fprintf` routes to `__stdio_common_vfprintf` → the kernel engine, NOT libmingwex in-process) writes a 10-conversion
  battery to `C:\c21.out`; the kernel re-reads `/data/c21.out` and matches the exact text
  `C21|3.141593|1.50|0.5|100000|1e+06|1.234568e+04|1.563e-02|-2.500|0.66666666666667|    3.14|` (default + explicit
  precision, width, the `-` sign, `%g` exponent selection + zero-strip, and a 14-significant-digit `%g` — what a real
  interpreter uses for `tostring(number)`). (2) `[WINPE21L] PASS exit=0` — the **real Lua 5.4.7 interpreter** runs
  `/etc/c21.lua`, computing + printing doubles (`1/4`, `0.1`, `10/3` at 14 sig digits, `sqrt(2)`); the kernel re-reads
  `/data/c21l.out` and finds `C21LUA=== quarter=0.250 tenth=0.1 third=3.3333333333333 root2=1.414214`. (Honest scope
  note: `win-lua` is a mingw build, so *its* float formatting is libmingwex in-process — that proof is the real-app
  end-to-end "compute a double, print it" capstone; the **kernel engine** itself is what `win-printf21` validates.)
  No regressions: full C1–C21 all-together run stays green. App-compat ~51% → ~52%.
- **2026-06-26** — **Track C milestone C20: float-return marshalling — `double`-returning Win32/CRT functions now
  work.** Closes the limit the C19b entry flagged ("libm `sin`/`cos`/`sqrt`/`pow`/… return a `double` which can't
  pass back through the integer gate"). The C2 marshalling gate captures only the four integer arg registers + stack
  and returns `rax`, so a function returning `double` (in `xmm0` per the MS-x64 ABI) got garbage. C20 adds a **second
  gate** (`WIN32_FGATE_VA = 0x30000180`, 74 bytes, assembled offline + objdump-verified) that *additionally* captures
  the FP arg registers `xmm0..xmm3` as bit patterns into the argument array (`a[0..3]`=int regs, `a[4..7]`=`xmm0..3`)
  and, after the dispatch syscall returns the result's bit pattern in `rax`, does `movq xmm0, rax` so the `double`
  lands in `xmm0`. A shim's IAT/procaddr thunk routes to this gate iff `win32_shim_is_double_ret()` (the loader's
  `pe_emit_gate_thunk` picks the gate per-shim), so existing integer shims are untouched. The real FP runs in a new
  `-msse2` kernel unit `kmath.c` (a from-scratch libm: `sqrt` via `sqrtsd`, range-reduced `sin`/`cos`/`exp`/`log`,
  `pow`=`exp·log`, `atan`/`asin`/`acos` via a `pi/6`-reduced series, `fmod`/`floor`/`ceil`/`frexp`/`ldexp`/…), each
  entry FXSAVE/FXRSTOR-guarded (`kmfpu_begin/end`) exactly like `kfont.c` so the caller's live SSE state is preserved;
  values cross the `-mno-sse`↔`-msse2` boundary as `uint64_t` bit patterns. **Latent-bug fix:** `fpu_restore`'s
  `fxrstor` asm has a `"memory"` clobber but does *not* mark the xmm registers clobbered, so the compiler kept the
  result live in an xmm reg across the guard's `fxrstor` (which overwrites it) and read garbage — fixed by forcing the
  result bits into an integer (`volatile uint64_t`) before `kmfpu_end`. ~26 `api-ms-win-crt-math` shims registered.
  **Two proofs, both clean under `-cpu qemu64,+smep,+smap`, 0 faults, 0 unresolved-and-called imports:** (1)
  `[WINPE20] PASS exit=20` — first-party `win-math20.exe` calls the **real** ucrt libm (built `-fno-builtin` so the
  calls are genuine imports, inputs `volatile` so nothing constant-folds) and checks 15 results (`sqrt`/`pow`/`sin`/
  `cos`/`tan`/`exp`/`log`/`log10`/`exp2`/`floor`/`ceil`/`fmod`/`fabs`/`atan2`/`ldexp`) against known values within
  tolerance using integer/relational ops only (exit `100+i` on the first failing check). (2) `[WINPE20L] PASS exit=0`
  — the **real off-the-shelf Lua 5.4.7 interpreter** (the C19b binary) runs `/etc/c20.lua` calling
  `math.sqrt/sin/exp/log/pi`; the kernel re-reads `/data/c20.out` and finds the exact sentinel
  `TOBYMATH=== sqrt2=1414214 sin30=500000 e=2718282 lne=1000000 pi=3141593` (integer-scaled to avoid the *separate*
  float-printing gap), turning C19b's "floats return garbage" caveat into a proven capability. (Running real Lua's
  float keys also surfaced + filled `frexp`, which returns a `double` *and* writes an `int*`.) **Still out of scope:**
  float *printing* (`printf`/`%g` formatting of a `double` arg) — a different path (the kernel printf engine), not the
  return gate. App-compat ~50% → ~51%.
- **2026-06-25** — **Track C milestone C19c: minimal COM / `ole32` (a deliberately narrow slice, NOT a real COM
  runtime).** Proves tobyOS implements the genuine COM ABI well enough to activate a coclass and dispatch its methods
  **through the object's vtable** — the hard part, since the object is kernel-provided but its methods must run as
  callable user code. tobyOS supports **exactly one** coclass (`CLSID_TobyAdder`) exposing **one** interface
  (`ITobyAdder : IUnknown`) with **one** real method (`Add`); every other CLSID/IID returns
  `REGDB_E_CLASSNOTREG`/`E_NOINTERFACE`. The `ole32` shims are `CoInitializeEx`/`CoUninitialize` (→ `S_OK`),
  `CoTaskMemAlloc`/`CoTaskMemFree` (→ the Win32 bump heap), and `CoCreateInstance(CLSID,…,IID,**ppv)`, which builds the
  object in user memory (`win32_heap_alloc`) as `{ lpVtbl, refcount, magic }` where **the vtable is an array of C9
  procaddr gate-thunk VAs** (`WIN32_PROCADDR_BASE_VA + idx*stride`, the same `mov eax,idx; jmp gate` thunk an IAT import
  gets). So `obj->lpVtbl->Method(this,…)` is a plain `call [mem]` that traps into the matching kernel shim with `this`
  as arg0 — the exact CPL3-callback mechanism the C6/C7/C14 trampolines use, reused as a vtable. The four method shims
  (`QueryInterface`/`AddRef`/`Release`/`Add`) are registered in the shim table under internal names that are never
  imported (only reached via the handed-back thunk VAs). Proof `[WINPE19C] PASS exit=19` — the first-party
  `win-com19c.exe` imports the **real** `ole32` entry points and exits 19 only if: a `CoTaskMemAlloc`/`Free` 64-byte
  round-trip succeeds, `CoCreateInstance` returns a live object, `Add(20,22)==42` **through the vtable**, `AddRef`
  takes the refcount 1→2, `QueryInterface(IID_IUnknown)` succeeds (ref→3, returns the same pointer), `QueryInterface`
  of an unknown IID returns `E_NOINTERFACE` with `*ppv` NULLed, and three `Release` calls walk the refcount back to 0.
  Clean under `-cpu qemu64,+smep,+smap`; all C1–C19c pass together (34 PASS, 0 faults); `validate.sh 3 75` 3/3 ALIVE.
  **Scope flag:** this is one CLSID/interface proving the ABI and the vtable-dispatch-from-kernel mechanism — not a
  general COM implementation (no real registry/marshalling/apartments/aggregation). App-compat stays ~50%.
- **2026-06-25** — **Track C milestone C19b: runs the real, off-the-shelf Lua 5.4.7 interpreter.** A second
  unmodified third-party program (after C18a SQLite) now runs as a Windows PE: the verbatim upstream Lua 5.4.7
  source (vendored at `third_party/lua/`, SHA-256 `9fbf5e…1e30`, all 61 `.c`/`.h` byte-identical to
  `lua.org`) built with the in-tree mingw clang into `win-lua.exe`, which lexes/parses/compiles and runs a real
  Lua program on its bytecode VM + standard library. Filling the import gaps surfaced one genuinely new piece of
  infrastructure and one **latent kernel bug**: (1) **`setjmp`/`longjmp` now work** — they save/restore the
  caller's live register context so they *cannot* be kernel shims, so the loader binds those IAT slots
  (`__intrinsic_setjmp`, `longjmp`) to two pure-CPL3 leaf stubs in the shim page (a self-consistent
  `{rbx,rbp,rsp,r12–r15,rsi,rdi,retaddr}` save/restore — no SEH unwind, which is exactly what Lua's protected-call
  error handling needs); this is the same fixed-VA user-stub mechanism as the C7/C14 trampolines. (2) **`w32_fread`
  had its arguments swapped** — `fread(ptr,size,nmemb,FILE*)` keeps the `FILE*` in the 4th slot like `fwrite`, but
  the shim read the fd from the buffer pointer and wrote file data into the `FILE*` token; SQLite never hit it (its
  VFS uses `ReadFile`, and the C18a test passed SQL via argv), but Lua's script loader reads through `fread`, so it
  exposed and fixed the bug. Plus a batch of leaf CRT shims real Lua reaches that SQLite did not (ctype
  `iscntrl/isgraph/islower/isupper/ispunct/isxdigit`, `strpbrk`, `strcoll`, stdio `getc/ungetc/feof/ferror/clearerr/
  freopen`, `setlocale`/`_configthreadlocale`, `remove`, `time`/`clock`, `GetModuleFileNameA`, `LoadLibraryExA`).
  Proof `[WINPE19B] PASS exit=0` — `win-lua.exe /etc/c19b.lua` runs a script of integer arithmetic
  (`fib(25)=75025`, `Σ i²=385`), the string library (`upper`/`rep`/`gsub` word-count), tables (`concat`) and `io`,
  writing its result to `C:\c19b.out`; the **kernel re-reads `/data/c19b.out` and finds the exact sentinel**
  `TOBYLUA=== sum=385 fib25=75025 words=4 …` — a value only producible by genuinely executing real Lua. Clean under
  `-cpu qemu64,+smep,+smap`; all C1–C19b pass together (33 PASS, 0 faults); `validate.sh 3 75` 3/3 ALIVE. Documented
  limit: the libm float functions (`sin`/`cos`/`sqrt`/`pow`/…) and `difftime` return a `double`, which can't pass
  back through the integer marshalling gate, so they're left unresolved-but-uncalled — float-printing scripts are
  out of scope (the gate would need XMM marshalling). App-compat ~49% → ~50%.
- **2026-06-25** — **Track C milestone C19a: hardened + shipped C1–C18.** Full-suite regression gate before
  opening C19. Built the all-flags ISO (every `-DWINPE*_BOOT` through `WINPE18D`) and ran the all-together boot
  under `-cpu qemu64,+smep,+smap` with `-netdev user` + tcp/udp hostfwd connectors and `cache=writethrough`:
  **all 32 verdicts PASS, 0 FAIL, 0 faults** — including the live C17 network round-trip (TCP client HTTP GET,
  TCP echo server with a real host client, UDP datagram echo) and C18a's on-disk SQLite db. Inspected the C18c
  GUI screenshots: the app window renders four distinct Lato faces (bold visibly heavier — +25% ink — and italic
  slanted), and the native `gui_about` window's body text is now smooth TrueType (desktop-wide `gui_window_text`
  → kfont). Default build (no `WINPE` flags) `validate.sh 3 75`: **3/3 ALIVE**, ~450 heartbeats each, `exc/panic=0`.
  Pushed the three previously-local merges (C18c/C18b/C18d) to `origin/main` (`b4b469d..5ef47d5`). Track C now
  spans C/C++, file I/O, threads + compiler TLS, runtime API resolution, multi-window, full GDI + multi-face
  TrueType (incl. the desktop), the full control/dialog/menu/timer set, registry, comdlg32, env, winsock, shell32,
  and a real off-the-shelf SQLite engine.
- **2026-06-24** — **Track C milestone C18d: `shell32` (folder paths, ShellExecute, tray, drag-drop).** Five new
  `shell32.dll` shims in `src/syscall.c`: **`SHGetFolderPathA`** and **`SHGetSpecialFolderPathA`** map a `CSIDL`
  folder id to a Windows path under `C:\` (e.g. `CSIDL_APPDATA` → `C:\Users\toby\AppData\Roaming`, `CSIDL_PERSONAL`
  → `…\Documents`, plus Desktop/Pictures/Windows/System32/Program Files/ProgramData/profile), honouring
  `CSIDL_FLAG_CREATE`/`fCreate` by `mkdir -p`-ing the tree on the writable `/data` mount (paths flow through the
  existing `C:\` → `/data` file layer); **`ShellExecuteA`** validates the verb/target and, for a `*.exe`, fires a
  real launch through the **safe pid-0 desktop launch queue** (`gui_launch_enqueue_arg`, never `proc_spawn` from a
  syscall context), returning `>32` per its fire-and-forget contract; **`Shell_NotifyIconA`** accepts add/modify/
  delete (tobyOS has no system tray) and **`DragQueryFileA`** reports zero files (no OLE drag source). Proof
  `[WINPE18D] PASS exit=88` — the stock console `win-shell18d.exe` (real `-lshell32` import) gets back well-formed
  `C:\…` paths from both folder shims, then writes a sentinel under the returned `CSIDL_APPDATA` path which the
  **kernel re-reads from `/data/Users/toby/AppData/Roaming/c18d.txt`** (proving the CSIDL mapping resolves
  end-to-end through `C:\`→`/data` and that `CSIDL_FLAG_CREATE` built the tree), with `ShellExecute>32`, tray
  accepted, and drag-count 0. Clean under `-cpu qemu64,+smep,+smap`, no unresolved imports; all C1–C18 pass
  together (32 PASS, 0 faults); validate 3/3 ALIVE. (OLE/COM deliberately avoided — out of scope.) App-compat
  ~48% → ~49%.
- **2026-06-24** — **Track C milestone C18b: compiler thread-local storage (`_Thread_local` / `__declspec(thread)`).**
  Extends C16e (which did only the explicit `TlsAlloc/TlsGetValue/TlsSetValue` API + per-thread TEBs) to the
  *compiler-emitted* TLS that real software uses. The PE loader (`src/pe_loader.c`) now walks the **`.tls` data
  directory** (`IMAGE_TLS_DIRECTORY64`, dir index 9): it lays out the TLS template block (raw bytes + zero-fill)
  at a fixed per-process region, writes `_tls_index` (= 0, the sole module) into the image, and points the main
  thread's **`TEB+0x58` (x64 `ThreadLocalStoragePointer`)** at a pointer array whose `[_tls_index]` slot holds
  the block — so the compiler's `mov eax,[_tls_index]; mov rcx,gs:[0x58]; mov rsi,[rcx+rax*8]; …[rsi+off]`
  sequence resolves. `CreateThread` (`src/syscall.c`) now gives **each thread its own template-initialised TLS
  block + array** (cloned from the leader's `.tls` template), so every `_Thread_local` is genuinely per-thread.
  The mingw `.tls` callbacks (`__dyn_tls_init`/`__dyn_tls_dtor`) are walked and **logged but not invoked**: they
  only set `_CRT_MT` and service *dynamic* TLS, and every C1–C18a mingw PE already ships them yet runs correctly
  with the loader never calling them — so static TLS works from the template copy alone, and calling CPL3
  callbacks from the kernel loader (process not yet running) would be needless and risky. **Note:** the test must
  be built `-fno-emulated-tls`, since clang's mingw target defaults to emulated TLS (`__emutls_get_address`),
  which never touches `gs:[0x58]`. Proof `[WINPE18B] PASS exit=42` — reachable only when 4 worker threads each
  observe the pristine template (`g_n==1000`) and main's private copy (`1005`) survives untouched (results
  `1007,1014,1021,1028`), i.e. real per-thread isolation, clean under `-cpu qemu64,+smep,+smap`. The new loader
  `.tls` path runs for **every** Windows PE in the suite (all carry a mingw `.tls` dir) with no regression: all
  C1–C18 pass together (31 PASS, 0 faults); validate 3/3 ALIVE. (Known gap surfaced: `__stdio_common_vsprintf`,
  the ucrt `snprintf` backend, is still unshimmed — out of TLS scope.) App-compat ~47% → ~48%.
- **2026-06-24** — **Track C milestone C18c: bold/italic TrueType faces + desktop-wide TrueType.** The kernel
  rasterizer (`src/kfont.c`) now holds **four Lato faces** (Regular/Bold/Italic/BoldItalic, all OFL, shipped
  in the initrd at `/etc/Lato-*.ttf`), each lazy-loaded independently with a transparent fallback to Regular
  when a weight/style is missing. `CreateFontA`'s `lfWeight` (≥700 = bold) and `lfItalic` now select the face:
  the `HFONT` token carries the face in bits 16-17 alongside the pixel height, `SelectObject` records it on the
  DC, and `TextOutA`/`DrawTextA`/`GetTextExtent`/`GetTextMetrics` render and measure with it (the glyph cache
  keys on face too). Separately, the **desktop's own `gui_window_text`** (`src/gui.c`) — the path every native
  tobyOS GUI app draws text through (`sys_gui_text`) — now routes through the rasterizer, so native apps render
  proportional Lato instead of the blocky 8×8 bitmap (with an 8×8 fallback when no TTF ships; the scaled-bitmap
  login path is untouched). **Latent-bug note:** Lato Bold shares Regular's advance widths, so a width check
  can't prove bold loaded — the harness instead reads the rendered pixels back (new `win32_gui_ink_stats`) and
  verifies the Bold line has measurably **more ink** than Regular (genuinely heavier), while the app proves
  Italic/BoldItalic loaded via their distinct advance widths. Proof `[WINPE18C] PASS exit=18` (bold +25% ink
  over regular) + two screenshots inspected (Regular/Bold/Italic/BoldItalic clearly distinct; a native
  `gui_about` window now in TrueType). All C1–C18c pass together under `-cpu qemu64,+smep,+smap` with 0 faults
  (30 PASS verdicts); validate 3/3 ALIVE; default desktop boot unaffected (kfont lazy-loads only when text is
  drawn — a PE-less/native-text-less boot never touches it). App-compat ~46% → ~47%.
- **2026-06-24** — **Track C milestone C18a: tobyOS runs a REAL off-the-shelf third-party database engine
  (SQLite).** The unmodified public-domain SQLite amalgamation (3.46.0, vendored verbatim at
  `third_party/sqlite/`) is built as a Windows PE (`win-sqlite.exe`) by the in-tree mingw clang and runs on
  tobyOS — code neither hand-written nor tobyOS-aware. It opens an **on-disk database** at `C:\c18.db`
  (→ `/data/c18.db`), runs `CREATE TABLE` / `INSERT` / `SELECT`, prints the rows (`TOBYSQL-OK`, `42`), and
  leaves a real **8192-byte 2-page SQLite database file on disk**; the harness reads that file back and finds
  the stored row value, so the proof spans the whole engine (SQL parser → VM → b-tree → the Win32 **wide-file
  VFS**). This needed ~110 new kernel32/ucrt shims in three groups: the **Win32 process heap** (`HeapAlloc`/
  `HeapReAlloc`/… over a new size-tracked allocator so `realloc`/`HeapSize`/`_msize` work — the C1–C17 bump
  allocator couldn't realloc); the **wide (W) file APIs** SQLite's WinNT VFS uses (`CreateFileW`/
  `GetFileAttributesExW`/`MultiByteToWideChar`/… converting UTF-16↔ASCII and reusing the A-path) plus
  time/console/locking/`GetFileType`; and a broad **C-runtime layer** (`fopen`/`fread`/`fwrite`/`fseek`,
  `strcmp`/`strcpy`/`memmove`/`strstr`, `isalnum`/`tolower`, `atoi`/`strtol`, `_stat64i32`/`_access`/`_mkdir`,
  `getenv`, `_localtime64`). The shim count crossed ~300, so the **PE shim region grew from 1 to 2 pages**
  (the GetProcAddress thunk table no longer fits one page). **Two latent kernel bugs caught + fixed:**
  (1) `GetLastError` was a constant-0 stub and the file-attribute shims didn't set `ERROR_FILE_NOT_FOUND`, so
  SQLite's `winAccess` read a missing rollback journal as a hard `SQLITE_IOERR_ACCESS` — now a real
  GetLastError/SetLastError + not-found signalling; (2) **`sys_lseek` grew the file when seeking past EOF**, a
  POSIX violation (size must only change on write) — SQLite's positioned reads of a not-yet-written page grew
  the empty db so it saw a short garbage header and returned `SQLITE_NOTADB`; now seek never changes size
  (writes still extend via `file_write`). `ReadFile`/`WriteFile` also honour an `OVERLAPPED` offset
  (positioned I/O). Build options are standard embed (`SQLITE_THREADSAFE=0`, `OMIT_LOAD_EXTENSION`,
  `MAX_MMAP_SIZE=0`); the engine source is unchanged. Proof `[WINPE18A] PASS exit=0 sentinel=1`. All C1–C18a
  pass together under `-cpu qemu64,+smep,+smap` with 0 faults (29 PASS verdicts); default desktop boot
  unaffected (steady heartbeats, max ~246ms gap, desktop ready, 0 faults, 0 PE activity, Linux-ABI boot tests
  still pass — the lseek fix is POSIX-correct). App-compat ~44% → ~46%.
- **2026-06-22** — **Track C milestone C17c: Winsock (ws2_32) — UDP datagrams.** A stock, off-the-shelf
  Windows `.exe` (`-lws2_32`) is a UDP echo server on tobyOS: `socket(SOCK_DGRAM)` -> `bind`(:9090) ->
  `recvfrom` -> `sendto` (echo). A REAL host datagram (SLIRP `hostfwd=udp::18081-:9090`, sent via bash
  `/dev/udp`) round-trips through tobyOS's UDP stack; the server checks for a magic token and exits 17 iff
  `recvfrom` delivered it and the echo `sendto` succeeded. Two new shims — `sendto`/`recvfrom` — wrap new
  by-fd kernel helpers `ksock_sendto`/`ksock_recvfrom` (socket.c) over `sock_sendto`/`sock_recvfrom`; user
  buffers go through `copy_from/to_user` and `recvfrom` fills the optional peer `sockaddr`. **Second latent
  kernel bug caught + fixed:** `sock_recvfrom` parked on a wait-queue (`PROC_BLOCKED`) and relied purely on
  the e1000 IRQ to wake it, but **never drained the NIC RX ring itself** — so an inbound datagram was never
  delivered when no other context happened to be draining (e.g. a boot-harness `proc_wait`); UDP recv simply
  hung. Fixed by making `sock_recvfrom` actively `rx_drain` + `sti;hlt` with the BKL dropped across the wait,
  mirroring `tcp_poll_until`/`dhcp_wait` (the proven pattern every other blocking receiver already uses).
  Proof `win-udp17.exe` (first-party): `[WINPE17C] PASS exit=17`, `recvfrom 12 bytes magic=1 from
  10.0.2.2:...`. All Win32 milestones C1..C17c pass together under `-cpu qemu64,+smep,+smap` with 0 faults
  (28 PASS verdicts); default desktop boot unaffected (steady heartbeats, max ~197ms gap, desktop ready, 0
  faults, 0 PE activity). **C17 (winsock) COMPLETE** — TCP client + TCP server + UDP. App-compat ~43% -> ~44%.
- **2026-06-22** — **Track C milestone C17b: Winsock (ws2_32) — the TCP server.** A stock, off-the-shelf
  Windows `.exe` (`-lws2_32`) acts as a TCP **server** on tobyOS: `socket` -> `bind`(INADDR_ANY:8080) ->
  `listen` -> `accept`, and a REAL external client — the QEMU host over SLIRP `hostfwd` (127.0.0.1:18080 ->
  guest:8080) — connects to it. The server `recv`s the request, checks for a magic token, `send`s an HTTP
  reply, and exits 17 IFF the token arrived (a path only reachable if a genuine inbound connection was
  delivered through tobyOS's TCP stack). Three new shims — `bind`/`listen`/`accept` — wrap the kernel
  `ksock_bind`/`ksock_listen`/`ksock_accept` (tcp.c already implements the full passive-open handshake +
  per-listener accept queue); `accept` returns a freshly-tagged `SOCKET`. Also added a `strstr` shim
  (`api-ms-win-crt-private`) the server's CRT imported. **Latent kernel bug caught + fixed:** `tcp_close`'s
  TIME_WAIT linger loop did `sti();hlt()` while **holding the BKL**, deadlocking pid 0 (which spins in
  `bkl_enter`) — the exact failure `tcp_poll_until` documents and avoids. It stayed latent because only the
  *active* closer reaches TIME_WAIT, and no prior test (all clients = passive closers) ever did; the C17b
  server is the first. Fixed by dropping/retaking the BKL across the wait, mirroring `tcp_poll_until`. Proof
  `win-srv17.exe` (first-party): `[WINPE17B] PASS exit=17`, the host received `HTTP/1.0 200 OK`. All Win32
  milestones C1..C17b pass together under `-cpu qemu64,+smep,+smap` with 0 faults (27 PASS verdicts); default
  desktop boot unaffected and shows 0 PE activity (the `tcp_close` fix is inert on a default boot — no TCP
  connection is opened — and the winsock shims only run for a PE process). App-compat ~42% -> ~43%.
- **2026-06-21** — **Track C milestone C17a: Winsock (ws2_32) — the TCP client.** A stock, off-the-shelf
  Windows `.exe` (built with the in-tree clang, linked against `ws2_32`) does a real **HTTP GET over the
  network** on tobyOS, with no real `ws2_32.dll`. Ten new shims — `WSAStartup`/`WSACleanup`/`WSAGetLastError`/
  `socket`/`connect`/`send`/`recv`/`closesocket`/`gethostbyname`/`htons` (+`ntohs`/`htonl`/`ntohl`/`inet_addr`)
  — map Win32 sockets onto tobyOS's existing kernel `ksock_*` API (`socket.c`, the same TCP stack the native
  HTTP/curl path uses) and the kernel DNS resolver. A Win32 `SOCKET` is a tagged handle (tag byte `0x68`,
  distinct from file fds / FILE* tokens) decoding to a `ksock` fd; `gethostbyname` resolves via `dns_resolve`
  and builds a real `struct hostent` (h_name/h_aliases/h_addrtype/h_length/h_addr_list) in the app's heap;
  all user buffers go through `copy_from/to_user` (SMAP-safe). The marshalling gate was **not** touched (all
  winsock fns are <=6 args, well within the 10-arg gate). Proof `win-net17.exe` (first-party, ordinary
  Winsock 2 code): `WSAStartup` -> `gethostbyname("example.com")` -> `socket`/`connect`(:80) -> `send` a
  `GET / HTTP/1.0` -> `recv` -> verifies the `HTTP/1` status line -> exit 17 (`[WINPE17A] PASS`, status line
  `HTTP/1.1 200 OK`, ~828 bytes). Needs networking (QEMU `-netdev user`/SLIRP, internet via host); DHCP binds
  10.0.2.15 at boot. All Win32 milestones (C1=42 … C16=…, C17A=17) pass together under `-cpu qemu64,+smep,+smap`
  with 0 faults (26 PASS verdicts); validate 3/3 ALIVE; default desktop boot unaffected and shows 0 PE
  activity (the winsock shims only run for a PE process). App-compat ~41% -> ~42%.
- **2026-06-20** — **Track C milestone C16 (a-e): registry, file dialogs, command line/env, TTF UI,
  per-thread TLS.** Five sub-milestones, each its own branch -> proof -> ff-merge. **C16a registry
  (advapi32):** a small in-kernel hive backs `RegCreateKeyExA`/`RegOpenKeyExA`/`RegCloseKey`/`RegSetValueExA`/
  `RegQueryValueExA`/`RegDeleteValueA`/`RegDeleteKeyA`/`RegEnumKeyExA`/`RegEnumValueA`; predefined roots
  (HKLM/HKCU/HKCR/HKU) are fixed keys, an opened HKEY is a tagged token, and writes flush the hive to
  `/data/toby_registry.dat` (chunked `vfs_write` + `bcache_sync`) so settings survive a reboot. Proof: a
  console `.exe` increments a persisted counter; **2-boot test** boot1 "fresh hive" seeds it, boot2 "loaded
  hive" reads it back -> persistence across reboot. (Fixed 3 bugs: sign-extended predefined HKEYs, an 80KB
  tobyfs write failing, vfs_create not truncating.) **C16b common file dialogs (comdlg32):**
  `GetOpenFileNameA`/`GetSaveFileNameA` are real modal file pickers over /data (list dirs, navigate, pick or
  type a name, write the `C:\...` path to `OPENFILENAMEA.lpstrFile`); proof opens the picked file + verifies
  its contents -> exit 16 + a screenshot of the file browser. **C16c command line + environment:**
  `GetCommandLineA`/`W`, and a seeded per-app env block behind `GetEnvironmentVariableA`/
  `SetEnvironmentVariableA`/`ExpandEnvironmentStringsA` (`%VAR%` expansion). **C16d TTF for the whole Win32
  UI:** all Win32 control labels, menus, MessageBox, and the file dialog now render real TrueType (via the
  C14b kfont rasterizer, `win32_ui_text`), falling back to 8x8; native apps' `sys_gui_text` stays bitmap so
  the tobyOS desktop keeps its nixie look. **C16e per-thread TEBs + TLS:** each `CreateThread` thread gets
  its own TEB (per-thread `NtCurrentTeb`/TLS), and `TlsAlloc`/`TlsFree`/`TlsGetValue`/`TlsSetValue` operate on
  the calling thread's TEB; proof is a multithreaded `.exe` where 3 workers each keep an isolated TLS value
  (and the main thread's survives) -> exit 16. All Win32 milestones (C1=42 … C15=15, C16A=16/17 (persisted),
  C16B=16, C16C=16, C16D=14, C16E=16) pass together under `-cpu qemu64,+smep,+smap` with 0 faults; validate
  3/3 ALIVE; default desktop boot unaffected (all new shims only run for a PE process; the registry/env/TLS
  state is lazy + per-PE-app). App-compat ~39% → ~41%.
- **2026-06-20** — **Track C milestone C15: menus + timers + multi-button MessageBox.** Three interactive
  desktop-app staples on the C12/C13 control + message infrastructure. (1) **Menus:** `CreateMenu`/
  `CreatePopupMenu`/`AppendMenuA` (`MF_POPUP`/`MF_SEPARATOR`/`MF_STRING`)/`DestroyMenu`/`SetMenu`/`GetMenu` —
  an `HMENU` is a tagged token into a kernel menu table; a bar attached via `SetMenu` (or `CreateWindowEx`'s
  `hMenu`) is kernel-drawn into the top strip of the window's client backbuffer, and a top-level item's
  `MF_POPUP` submenu is its dropdown. A menu click is routed in `GetMessage` **before** controls
  (`win32_route_menu_input`), toggles the dropdown, and a popup item delivers `WM_COMMAND(id, HIWORD=0)` to
  the window (drawn on top in `EndPaint`/refresh, like the C14 combobox dropdown). (2) **Timers:**
  `SetTimer(hwnd,id,ms,NULL)`/`KillTimer` → a periodic `WM_TIMER` synthesised by `GetMessage` (PIT-tick based)
  after input + before idle; a per-`(hwnd,id)` table, dead-window timers pruned on poll. (3) **Multi-button
  `MessageBoxA`:** honours `uType` `MB_OK`/`MB_OKCANCEL`/`MB_YESNO`/`MB_YESNOCANCEL`/`MB_ABORTRETRYIGNORE`/
  `MB_RETRYCANCEL` — draws the right 1–3 buttons, hit-tests each in the modal poll loop, and returns
  `IDOK`/`IDCANCEL`/`IDYES`/`IDNO`/`IDRETRY`/`IDABORT`/`IDIGNORE` (was hard-wired to a single OK→IDOK).
  Process-global menu + timer state is cleared on the fresh-app boundary (alongside the bridge memset) since
  `HMENU`/timer tokens don't survive a process exit. Proof: `programs/win-menu15/main.c` builds a File/Help
  menu bar (New/Open/Exit, About), a 500 ms timer updating a counter, and an `MB_YESNO` About box; the
  harness lets the timer tick, opens Help→About (Yes/No box), clicks Yes, then File→Exit → exit 15 iff the
  timer fired + a menu command arrived + the box returned `IDYES`. `[WINPE15] PASS exit=15` + 3 screenshots
  (the menu bar + "Timer ticks: N" in TTF; the Help dropdown; the Yes/No box). All Win32 milestones (C1=42 …
  C14B=14, C15=15) pass together under `-cpu qemu64,+smep,+smap` with 0 faults; validate 3/3 ALIVE; default
  desktop boot unaffected (menu/timer/message shims only run for a PE process). App-compat ~38% → ~39%.
- **2026-06-20** — **Track C milestone C14b: real TrueType fonts (kernel-side stb_truetype).** The Win32 GDI
  text path now renders genuine antialiased TrueType glyphs at arbitrary pixel sizes instead of the scaled
  8x8 bitmap font. A kernel font module (`src/kfont.c`) embeds the vendored `third_party/stb_truetype.h` with
  freestanding shims (kmalloc/kfree/memcpy/strlen + double libm) and a per-`(codepoint,px)` glyph cache
  (coverage bitmap + metrics); the bundled **Lato** (SIL OFL 1.1) TTF ships in the initrd at
  `/etc/Lato-Regular.ttf` and is lazy-loaded on first use via `vfs_read_all`. **Floating point in the kernel
  (the hard part):** stb_truetype is float-heavy but the kernel is normally `-mno-sse` (it never touches XMM);
  `kfont.c` is compiled `-msse` (a per-file Makefile rule) and brackets every float window in `kfpu_begin`/
  `kfpu_end`, which **FXSAVE the calling user process's live SSE/x87 state and FXRSTOR it afterwards** -- a
  syscall doesn't save user FP on entry (only a context switch does), so without the guard the rasterizer
  would corrupt the app's registers; the guarded windows are pure computation (no `sched_yield`) so they're
  atomic w.r.t. the user's FP state. Glyph blending is integer-only (`gui_window_blend_coverage` in the
  `-mno-sse` gui.c). The Win32 shims: `CreateFontA`'s HFONT now carries the pixel height, `SelectObject` sets
  the DC's `font_px`, `TextOutA`/`DrawTextA` route through the rasterizer, and `GetTextExtentPoint32A`/
  `GetTextMetricsA` report real TTF metrics (all fall back to the bitmap font if no TTF ships). Proof:
  `programs/win-font14b/main.c` `CreateFontA`'s 18/26/36/52 px and `TextOutA`'s real strings in four colours,
  `DrawTextA`-centres a line, and measures "WWWW" vs "iiii" with `GetTextExtentPoint32A` -> exit 14 iff the
  font measured **proportional** (TTF, not the monospace bitmap fallback); `[WINPE14B] PASS exit=14` +
  screenshot (real Lato glyphs at four sizes + a centred `DrawText`). All Win32 milestones (C1=42 … C14=14,
  C14B=14) pass together under `-cpu qemu64,+smep,+smap` with 0 faults (no `#XF`); validate 3/3 ALIVE; default
  desktop boot unaffected (kfont is lazy-loaded only on the first Win32 `TextOut`, so a PE-less boot never
  touches it). App-compat ~37% → ~38%.
- **2026-06-20** — **Track C milestone C14a: COMBOBOX/RADIO/GROUPBOX/TAB controls + dialogs from RT_DIALOG
  resource templates.** Two phases on the C12/C13 virtual-control foundation. (1) **Four new controls:**
  `COMBOBOX` (a closed field showing the selection + a drop arrow that opens a dropdown list — drawn in a
  second pass on top of its siblings + a click pre-pass so the open list captures clicks; `CB_ADDSTRING`/
  `GETCURSEL`/`SETCURSEL`/`GETLBTEXT`/`GETCOUNT`/`RESETCONTENT`), `RADIOBUTTON` (`BS_(AUTO)RADIOBUTTON` — a
  circular check, mutually exclusive within a `WS_GROUP` run via a per-control group id assigned at creation),
  `GROUPBOX` (`BS_GROUPBOX` — a labelled frame, visual only + click-transparent), and a `TAB` control
  (`SysTabControl32` — a tab strip + page frame; `TCM_INSERTITEM`/`GETCURSEL`/`SETCURSEL`/`GETITEMCOUNT`; a
  tab click switches the selection and posts `WM_NOTIFY` with a staged `NMHDR`(`TCN_SELCHANGE`), and
  `ShowWindow(SW_HIDE/SW_SHOW)` on child controls switches pages). Control creation was refactored into a
  shared `win32_ctrl_make()`; control labels now pick a contrast colour from the background luminance (dark
  text on a light dialog face, light text on a dark app window). (2) **Dialogs from resource templates:**
  `DialogBoxParamA(hInst, MAKEINTRESOURCE(id), parent, DlgProc, initParam)` builds a real modal dialog from
  the PE's own `.rsrc` — the loader now exposes the resource data directory (`PE_DIR_RESOURCE`) via
  `pe_load_info` → `struct proc`, and the kernel walks the resource tree (root → `RT_DIALOG` → id → lang →
  data entry), parses both `DLGTEMPLATE` and the extended `DLGTEMPLATEEX` (`dlgVer`/sig `0xFFFF`) header +
  each DWORD-aligned `DLGITEMTEMPLATE(EX)` item (class ordinal `0x80–0x85` or name, rect in dialog units, id,
  title), then builds the dialog window + its controls. Because a **modal** dialog dispatches to the app's
  CPL3 `DialogProc`, `DialogBoxParamA` is bound (loader) to a new shim-page **trampoline at a fixed VA**
  (`0x30000100`) that runs a GetMessage/DispatchMessage loop — PUMP-ing one message at a time from the kernel
  (`__toby_dlgsvc` at a fixed gate-thunk VA `0x300001F0`) and DispatchMessage-ing each to `DialogProc` via the
  C7 trampoline — until `EndDialog`, then returns the result (zero load-time patching of the blob; fixed VAs
  only; the import-thunk base moved `0x100`→`0x200` with `_Static_assert` guards). The kernel owns the
  dialog's painting (`win32_win.is_dialog`); `WM_INITDIALOG` is delivered first; `[X]`→`WM_COMMAND`/`IDCANCEL`.
  New shims: `EndDialog`, `CreateDialogParamA` (modeless), `DefDlgProcA`, `GetDlgItem`, `GetDlgCtrlID`,
  `Set`/`GetDlgItemTextA`, `CheckDlgButton`, `IsDlgButtonChecked`, `CheckRadioButton`, `SendDlgItemMessageA`,
  `lstrcmpA`. Two proofs (first-party; the `.exe`s are build artifacts) under `-DWINPE14_BOOT`:
  `programs/win-ctrl14/main.c` — the harness selects the 2nd RADIO (`RADIO id=402 group=1`), opens the
  COMBOBOX + picks "Green" (`COMBOBOX id=403 → sel=1`), switches to TAB "Two" (`TAB id=404 → sel=1`), clicks
  Finish; the WndProc reads radio/combo/tab state back → exit 14 (2 screenshots: combo dropdown open; final
  Slow/Green/page-Two). `programs/win-dlg14/main.c` — `WinMain` just returns `DialogBoxParamA` over a windres
  `RT_DIALOG` template (EDIT/AUTOCHECKBOX/GROUPBOX/2 AUTORADIOBUTTON/OK/Cancel); the harness types into the
  EDIT (`WM_INITDIALOG` seeded "to" → "tobyOS"), checks the box, picks "Premium", clicks OK; `DlgProc` reads
  the controls back and `EndDialog(14)` → exit 14 (2 screenshots: the template-built dialog; filled state).
  All FOURTEEN Win32 milestones (C1=42 … C13=13, C14=14, C14D=14) pass together under
  `-cpu qemu64,+smep,+smap` with 0 faults; validate 3/3 ALIVE; default desktop boot unaffected (the new shims
  + dialog trampoline only run for a PE process). App-compat ~36% → ~37%.
- **2026-06-19** — **Track C milestone C13: STATIC/CHECKBOX/LISTBOX/SCROLLBAR controls + MessageBoxA modal
  dialogs + GDI fonts/metrics.** Three areas on the C12 virtual-control foundation. (1) **More controls:**
  the control system gained `STATIC` (label), `CHECKBOX` (a BUTTON with a checkbox style — toggles `checked`
  on click + notifies `WM_COMMAND`/BN_CLICKED, with `BM_GETCHECK`/`BM_SETCHECK`), `LISTBOX` (an item store
  filled by `LB_ADDSTRING`; a row click sets the selection + sends `LBN_SELCHANGE`; `LB_GETCURSEL`/
  `SETCURSEL`/`GETTEXT`/`GETCOUNT`/`RESETCONTENT`), and `SCROLLBAR` (track + arrows + thumb; arrow/page
  clicks adjust the position + send `WM_VSCROLL`(SB_LINEUP/DOWN/PAGEUP/DOWN); `SetScrollRange`/`SetScrollPos`/
  `GetScrollPos`/`Set`/`GetScrollInfo`). The glue is a new **`SendMessageA`** that handles the control
  messages in-kernel (the kernel IS the control's WndProc). (2) **`MessageBoxA` — a real modal dialog:** it
  creates its own window with the text + an OK button (via the kernel-side `gui_window_create`, since
  `sys_gui_create` marshals its title from USER space) and **blocks in a poll loop, `sched_yield`-ing so the
  compositor + the rest of the system keep running**, until OK is clicked / Enter / closed (a 20 s safety
  cap), then tears the box down and returns `IDOK`. (3) **GDI fonts + text metrics:** `CreateFontA` maps the
  requested height onto tobyOS's scalable 8×8 bitmap font (an HFONT token carrying the integer scale),
  `SelectObject(font)` sets the DC's scale, `SetTextColor`/`SetBkColor`/`SetBkMode` set the text colours,
  `GetTextExtentPoint32A`/`GetTextMetricsA` report sizes from the scale, and `TextOutA` (now scaled+coloured)
  / `DrawTextA` (with `DT_CENTER`/`DT_VCENTER`) render text. Two proofs (first-party; the `.exe`s are build
  artifacts) under `-DWINPE13_BOOT`: `programs/win-ctrl13/main.c` lays out a STATIC + CHECKBOX + LISTBOX
  (Apple/Banana/Cherry) + SCROLLBAR + a "Show dialog" button — the harness checks the box (`CHECKBOX
  checked=1`), selects row 1 (`LISTBOX sel=1 ('Banana')`), nudges the scrollbar (`SCROLLBAR pos=1..3
  WM_VSCROLL`), then clicks the button → MessageBox → the harness clicks OK (`MessageBox: … waiting for OK` →
  `closed -> IDOK`), exit 13 (screenshots of the controls window + the modal dialog); and
  `programs/win-font13/main.c` draws four labels at sizes 8/16/24/32 in four colours + a centred `DrawText`,
  exit 13 (screenshot of the scaling fonts). **Fixed a real bug:** the modal box first failed silently
  because `sys_gui_create` marshals the title from USER space and MessageBoxA's title is a kernel string
  (`fd<0` → early `IDOK` with no visible box) — switched to the kernel-side window backend. All THIRTEEN
  Win32 milestones (C1=42 … C12=12, C13=13) pass together under `-cpu qemu64,+smep,+smap` with 0 faults;
  validate 3/3 ALIVE; default desktop boot unaffected (the new shims only run for a PE process). App-compat
  ~35% → ~36%.
- **2026-06-19** — **Track C milestone C12: bitmaps/BitBlt + real BUTTON/EDIT controls.** Two areas. (1)
  **Bitmaps:** `CreateCompatibleDC` (a memory HDC), `CreateBitmap`/`CreateCompatibleBitmap` (a kmalloc'd
  32-bpp pixel buffer; DWORDs 0x00RRGGBB == the tobyOS framebuffer's XRGB, no conversion), `SelectObject`
  (route a bitmap into a memory DC — extends the C11 pen path), `BitBlt` (copy a w×h sub-rect of the source
  bitmap to the destination window via `gui_window_blit`), `DeleteDC`/`DeleteObject` (free). An app composes
  an off-screen image and blits it onto its window. (2) **Real controls:** the predefined `BUTTON`/`EDIT`
  window classes. `CreateWindowExA("BUTTON"/"EDIT", …, hWndParent, hMenu=id, …)` creates a **virtual child**
  (a small table entry, not a tobyOS window) rendered INTO the parent's client backbuffer by `EndPaint` /
  on change — the compositor is top-level-only, so controls are drawn in-line rather than as nested windows.
  The kernel draws each control (button = 3-D gray face + label; edit = white box + text + focus border) and
  routes the parent's input to it: a click in a BUTTON's rect queues `WM_COMMAND`(BN_CLICKED, id) for the
  parent (delivered by `GetMessage`, which stashes the parent's WndProc); a keystroke with an EDIT focused
  appends/backspaces its text; `GetWindowTextA`/`SetWindowTextA` read/set a control's text. The app never
  implements the controls — exactly the "real control" model. Two proofs (first-party; the `.exe`s are build
  artifacts) under `-DWINPE12_BOOT`: `programs/win-bmp12/main.c` BitBlts an in-memory red/green gradient
  bitmap onto its window → exit **12** (`[WINPE12B] PASS`, screenshot of the gradient); and
  `programs/win-ctrl12/main.c` creates an EDIT + a BUTTON — the harness types "tobyOS" into the edit (serial:
  `control: EDIT id=201 text='t'…'tobyOS'`) and clicks the button (`control: BUTTON id=202 clicked ->
  WM_COMMAND` → `GetMessage hwnd=3 -> WM_COMMAND id=202`); the WndProc reads the edit text back and exits
  **12** (`[WINPE12] PASS`, screenshot of the edit showing "tobyOS" + the "Click Me" button). All TWELVE
  Win32 milestones (C1=42, C2=7, C3=3, C4=5, C5=9, C6=6, C7=7, C8=8, C9=9, C10=10, C11=11, C12=12) pass
  together under `-cpu qemu64,+smep,+smap` with 0 faults; validate 3/3 ALIVE; default desktop boot unaffected
  (the new shims only run for a PE process). App-compat ~34% → ~35%.
- **2026-06-19** — **Track C milestone C11: more GDI + more file ops + child/owned windows.** Three additions
  on a widened foundation. (1) **The marshalling gate was extended from 8 to 10 marshalled args** — the
  hand-assembled gate now does `sub rsp,0x50` and captures a4..a9 off the caller stack (123 bytes, re-
  assembled offline + verified by objdump, `WIN32_GATE_IMM_OFF` 0x51→0x6e, `win32_dispatch` reads 10 qwords).
  This unblocks `CreateWindowExA`'s `hWndParent` (its 9th arg = a8) and any future >8-arg Win32 function; all
  C1–C10 re-verified to pass with the wider gate (printf's va_list + CreateWindowEx's W/H exercise the moved
  a4–a7 offsets). (2) **GDI:** `CreatePen`/`SelectObject` (per-DC current pen, a tagged colour token),
  `MoveToEx`/`LineTo` (per-DC current position), `Rectangle`, `Ellipse` (drawn as an inscribed circle — the
  backend draws circles, an honest approximation), `SetPixel` — all routed by `hdc` to the compositor's
  `gui_window_line`/`gui_window_rect`/`gui_window_circle_outline`. (3) **File ops:** `SetFilePointer`→`lseek`,
  `GetFileSize`→fstat, `DeleteFileA`→`unlink`, `CreateDirectoryA`→`mkdir`, `GetFileAttributesA`→kernel-side
  `vfs_stat`, and **`FindFirstFileA`/`FindNextFileA`/`FindClose`** directory enumeration (a small find-handle
  table over `vfs_opendir`/`vfs_readdir`, filling a `WIN32_FIND_DATAA`); path translation (`C:`→`/data`,
  `\`→`/`) reused from C5. (4) **Child/owned windows:** `CreateWindowExA` reads `hWndParent`, records the
  parent link in the per-window table, and the WM_DESTROY retire path **cascades** the destroy to a parent's
  children (top-level rendering — the compositor has no nested client-area clipping — but faithful parent→
  child lifetime). Two proofs (first-party; the `.exe`s are build artifacts) under `-DWINPE11_BOOT`: a console
  `programs/win-c11file/main.c` round-trips dir+create+write+`GetFileSize`(16)+`SetFilePointer`+read(`ABCDEF`)+
  `FindFirstFile`(f.txt)+delete → exit **11** (`[WINPE11F] PASS`); and a GUI `programs/win-gui11/main.c` draws
  a pen line + Rectangle + Ellipse, opens a child window owned by the main, and calls `DestroyWindow(hMain)`
  ONLY — yet both windows' WndProcs receive WM_DESTROY (serial: `GetMessage hwnd=3 -> WM_DESTROY` then
  `hwnd=4`), exit **11** (`[WINPE11] PASS`), proven by a screenshot of the GDI window + its child on the
  desktop. All ELEVEN Win32 milestones (C1=42, C2=7, C3=3, C4=5, C5=9, C6=6, C7=7, C8=8, C9=9, C10=10, C11=11)
  pass together under `-cpu qemu64,+smep,+smap` with 0 faults; validate 3/3 ALIVE; default desktop boot
  unaffected (the new shims + wider gate only run for a PE process; none load in a default boot). App-compat
  ~33% → ~34%.
- **2026-06-19** — **Track C milestone C10: MULTIPLE top-level Win32 windows.** The GUI bridge's single
  global window state (`g_win32_gui`) became a **per-window table** keyed by `HWND == fd` (each holding its
  size / `WndProc` / paint+destroy flags / fill colour) plus a **class registry** mapping a class name to its
  `WndProc`. Every shim now routes by its `hwnd`/`hdc` argument (`ShowWindow`/`Update`/`InvalidateRect`/
  `DestroyWindow`/`DefWindowProc`/`BeginPaint`/`EndPaint`/`FillRect`/`TextOut`), `CreateWindowEx` binds a new
  window to its class's `WndProc`, and `GetMessage` scans **all** the process's windows for a pending
  WM_DESTROY (then retires the window — closing its desktop window + freeing the slot), WM_PAINT, or input
  event. The one subtlety -- DispatchMessage must call the **right** window's `WndProc` -- is solved with no
  asm change to the C7 trampoline: `GetMessage` stashes the message's target `WndProc` into the CRT slot the
  trampoline reads (GetMessage and DispatchMessage run in lock-step per message). Proof:
  `programs/win-gui10/main.c` (first-party; the `.exe` is a build artifact) under `-DWINPE10_BOOT` opens TWO
  windows sharing one `WndProc` (discriminating by `HWND`); the harness (reusing C8's auto-login + desktop
  launch + real-input infra) waits for both to paint, drives a **real PS/2 mouse click** into the focused
  window — recolouring **only** that one (blue→green) — holds for a screenshot, then closes BOTH windows in
  turn; the app returns **10** iff both painted, exactly ONE got the click (per-window routing, not a
  broadcast), and both ran their `WM_DESTROY` (`[WINPE10] VERDICT: PASS exit=10`). The headline is a
  **screenshot of two Win32 windows on the tobyOS desktop, one green (clicked) and one blue**, with the
  serial trace showing the click → `hwnd=4 WM_LBUTTONDOWN` and the two closes → `hwnd=4`/`hwnd=3 WM_CLOSE`.
  **Fixed a cross-app state leak the refactor exposed:** the process-global bridge is reset for each fresh GUI
  app — detected by "no live windows remain" validated against the live per-process fd table (pids are
  reused, so tgid is unreliable), which clears a previous app's leftover quit flag / class table / stale
  window slots. All TEN Win32 milestones (C1=42, C2=7, C3=3, C4=5, C5=9, C6=6, C7=7, C8=8, C9=9, C10=10) pass
  together under `-cpu qemu64,+smep,+smap` with 0 faults; validate 3/3 ALIVE; default desktop boot unaffected
  (the bridge only runs for a PE GUI process; none load in a default boot). App-compat ~32% → ~33%.
- **2026-06-17** — **Track C milestone C9: runtime API resolution — `GetModuleHandle` / `GetProcAddress` /
  `LoadLibrary`.** Until C9 every Win32 call was a *static* IAT import bound at load; C9 adds the mechanism a
  huge amount of real Windows software uses — resolve an API by **name at runtime** and call through the
  returned pointer (plugin systems, delay-loaded imports, optional-feature probes, much of the CRT). It's a
  clean generalization of the marshalling gate: at PE load the loader now pre-generates a 10-byte gate thunk
  (`mov eax,idx ; jmp gate`) for **every** kernel-side shim into a fixed region of the shim page
  (`WIN32_PROCADDR_BASE_VA`, bounded below by the per-import thunks and above by the 4 KiB page end), so
  `GetProcAddress(hModule, "Func")` resolves the name via `win32_shim_index` and returns that thunk's VA —
  a real, callable CPL3 function pointer that runs through the same gate → `ABI_SYS_WIN32_DISPATCH` → shim as
  an IAT import. A module HANDLE is a tagged token (`0x7D…`) carrying a shim-table index whose `.dll` names
  the module, so `GetProcAddress` recovers which DLL to search; `GetModuleHandleA(NULL)` returns the PE
  ImageBase; `LoadLibraryA` returns the same token (tobyOS *is* the implementation — nothing to load);
  `FreeLibrary` is a no-op TRUE; ordinal imports and unknown names/DLLs → NULL (self-identifying). New
  shims: `GetModuleHandleA`, `GetProcAddress`, `LoadLibraryA`, `FreeLibrary`, and `GetCurrentProcessId` (an
  observable target). Proof: `programs/win-c9/main.c` (first-party; the `.exe` is a build artifact) under
  `-DWINPE9_BOOT` resolves `GetCurrentProcessId` at runtime and **calls it** (returns its real pid=2),
  asserts the resolved pointers are stable + distinct, that an unknown name is NULL, and that
  `LoadLibraryA("user32.dll")` + `GetProcAddress(…, "GetMessageA")` resolves cross-DLL — exit **9**
  (`[WINPE9] VERDICT: PASS`). All NINE Win32 milestones (C1=42, C2=7, C3=3, C4=5, C5=9, C6=6, C7=7, C8=8,
  C9=9) pass together under `-cpu qemu64,+smep,+smap` with 0 faults; validate 3/3 ALIVE; default desktop boot
  unaffected (the new shims + loader thunk table only run for a PE process; none load in a default boot).
  App-compat ~31% → ~32%.
- **2026-06-17** — **Track C milestone C8: a VISIBLE + INTERACTIVE stock Win32 GUI window on the tobyOS
  desktop.** C7 proved the user32/gdi32 bridge but left two honest gaps — the window was composited *behind*
  the fullscreen login screen (boot-spawn, pre-desktop) and was inert (no input). C8 closes both. **Visible:**
  the harness signs a session in programmatically (`session_login("root","")` — root seeds with an empty
  password), dismisses the login window (`service_stop` + SIGKILL; it won't restart once a session is
  active), and launches the `.exe` onto the **logged-in desktop**, so the window shows with full chrome on
  top of the wallpaper + taskbar. **Interactive:** `GetMessageA` now drains the window's `struct gui_event`
  queue and translates it to `WM_*` — mouse → `WM_MOUSEMOVE`/`WM_LBUTTONDOWN`/`WM_LBUTTONUP` (with the
  `MK_*`/`lParam` packing), keyboard → `WM_KEYDOWN`, close → `WM_CLOSE` — and a new `DestroyWindow`→
  `WM_DESTROY` synthesis makes the idiomatic `WM_CLOSE`→`WM_DESTROY`→`PostQuitMessage`→`WM_QUIT`→clean-exit
  chain run; `FillRect` now honours a `CreateSolidBrush` colour so a click can visibly recolour the window.
  No new trampoline was needed (input flows through the normal `GetMessage` shim; only the C7 `DispatchMessage`
  stub calls CPL3). The interactive `.exe` (`programs/win-gui8/main.c`, first-party; the `.exe` is a build
  artifact) is driven under `-DWINPE8_BOOT` by a **real PS/2 mouse click** (`mouse_inject_event` → driver →
  compositor hit-test → the window, aimed at the window centre via a cursor-feedback loop) whose `WndProc`
  recolours the window blue→green; a deterministic close then drives the teardown. It returns **8** iff it
  painted, handled the click (`WM_LBUTTONDOWN`), AND ran the close chain (`WM_DESTROY`) — `[WINPE8] VERDICT:
  PASS exit=8`. **The headline is a screenshot of the real green Win32 window on the tobyOS desktop**, cursor
  on it, with the serial trace `GetMessage -> WM_0201 … WM_0202 … WM_0010(WM_CLOSE) … WM_DESTROY` and the
  "Logged in / Welcome, root" toast. All EIGHT Win32 milestones (C1=42, C2=7, C3=3, C4=5, C5=9, C6=6, C7=7,
  C8=8) pass together under `-cpu qemu64,+smep,+smap` with 0 faults; validate 3/3 ALIVE; default desktop boot
  unaffected (the input translation + new shims are additive, the harness/auto-login is `-DWINPE8_BOOT`-only).
  Kernel additions: `gui_post_mouse`/`gui_cursor_pos`/`gui_focused_window_client_center` (gui.c),
  `win32_gui_window_fd`/`win32_gui_fill_color`/`win32_gui_set_log` accessors (syscall.c). App-compat ~30% →
  ~31%.
- **2026-06-17** — **Track C milestone C7: tobyOS runs a stock Win32 GUI `.exe` (user32/gdi32 bridge).**
  A textbook Win32 GUI program (Windows subsystem, `WinMain`) -- `RegisterClassA` + `CreateWindowExA` +
  a `GetMessageA`/`DispatchMessageA` loop whose `WndProc` draws on `WM_PAINT` (`BeginPaint` → `FillRect`
  + `TextOutA` → `EndPaint`) -- now runs. It creates a **real tobyOS desktop window** (`sys_gui_create`,
  with the right title + 400×200 size) and the drawing maps onto `sys_gui_fill`/`sys_gui_text`. **The hard
  part** -- `DispatchMessage` calling the app's `WndProc`, which a kernel shim can't do (it can't call CPL3
  code) -- is solved with a **user-mode trampoline** in the shim page (same mechanism as the C6 thread
  wrapper): the loader binds `DispatchMessageA`'s IAT slot to a 46-byte stub that reads the `MSG`, loads
  the registered `WndProc` from a CRT-data slot, and `call`s it in CPL3 with the Microsoft-x64 args. The
  rest is ~16 shims (the 12 user32 funcs, gdi32 `TextOutA`, and the GUI-subsystem CRT extras
  `GetStartupInfoA`/`__p__acmdln`/`memset`/`IsDBCSLeadByte`); the message loop is synthesised kernel-side
  (`CreateWindow`/`ShowWindow` queue a `WM_PAINT`, `PostQuitMessage` ends it); `Sleep` was made real
  (`sys_nanosleep`) so the window persists. Proof: `programs/win-gui/main.c` (first-party; the `.exe` is a
  build artifact) under `-DWINPE7_BOOT`; the serial log shows `window_create … title='tobyOS Win32 GUI'`
  and the app exits **7** -- which is only returned after the WndProc's paint completes, so it proves the
  whole chain (window + message loop + WndProc trampoline + GDI drawing) ran. All SEVEN Win32 milestones
  (C1=42, C2=7, C3=3, C4=5, C5=9, C6=6, C7=7) pass together under `-cpu qemu64,+smep,+smap` with 0 faults;
  validate 3/3 ALIVE; default desktop boot unaffected. **Known limit (honest):** the harness runs the app
  at boot, so the window is composited *behind* the login screen and isn't crisply visible in a boot-time
  screenshot (a desktop-session launch would show it); single window; `DispatchMessage`-style callback
  only. App-compat ~29% → ~30%.
- **2026-06-16** — **Track C milestone C6: tobyOS runs a stock MULTITHREADED Windows `.exe`.** A plain
  `clang main.c -o win-thread.exe` using `CreateThread` + `WaitForSingleObject` + a real
  `CRITICAL_SECTION` now runs: 4 worker threads each increment a shared counter 1000× under the lock, main
  joins them all, and the total is exactly `4000` (no lost updates) — exit 6 (`[WINPE6] PASS`). Three
  pieces. **(1) CreateThread → a tobyOS thread.** The loader writes a tiny position-independent **thread
  wrapper** into the shim page (at a fixed offset; the gate/thunk layout moved to fixed offsets to make
  room). `CreateThread` allocates a `{func,param}` block + a thread stack from the per-proc heap and calls
  `thread_create(wrapper, block, stack_top, 0)`; the new thread enters the wrapper in CPL3, which sets the
  Microsoft-x64 first arg (`param`→`rcx`), calls the thread function, and exits via a raw
  `ABI_SYS_THREAD_EXIT`. The new thread is given the `ABI_PERS_WIN32` personality + the shared TEB before
  it can run (the BKL we hold blocks it until then). **(2) WaitForSingleObject → `thread_join`**; thread
  HANDLEs are tagged (`WIN32_THREAD_TAG|tid`) so `CloseHandle`/`WaitForSingleObject` tell them apart from
  file fds + ucrt `FILE*` tokens. **(3) Real critical sections.** `EnterCriticalSection`/`Leave` (no-ops
  since C3) now do real mutual exclusion: the lock state lives in the user `CRITICAL_SECTION` struct
  (`OwningThread`/`RecursionCount`), the BKL makes the read-modify-write atomic across threads, and
  contention is handled by `sched_yield`-retry. Proof: `programs/win-thread/main.c` (first-party; the
  `.exe` is a build artifact) under `-DWINPE6_BOOT`; all SIX Win32 milestones (C1=42, C2=7, C3=3, C4=5,
  C5=9, C6=6) pass together under `-cpu qemu64,+smep,+smap` with 0 faults; validate 3/3 ALIVE; default
  desktop boot unaffected (additive shims + shim-page layout only; no kernel-path change). App-compat
  ~28% → ~29%.
- **2026-06-16** — **Track C milestone C5: tobyOS runs a stock Windows `.exe` that does FILE I/O.** A
  plain `clang main.c -o win-fileio.exe` using the Win32 `CreateFileA`/`WriteFile`/`ReadFile`/`CloseHandle`
  API now round-trips a file: it opens `C:\wintest.txt` for write, writes a line, closes, re-opens for
  read, reads it back, and `memcmp`s — exit 9 (`[WINPE5] PASS`), `[c5] wrote 33 bytes, read 33 back: …`.
  The shims (`src/syscall.c`) map a Win32 **HANDLE directly onto a tobyOS fd** (distinct from the ucrt
  `FILE*` tokens the stdio shims use): `CreateFileA` translates the access (`GENERIC_READ/WRITE`) +
  disposition (`CREATE_ALWAYS`/`OPEN_EXISTING`/…) flags onto `O_*` and the path onto the VFS — a `C:`
  drive letter maps to tobyOS's **writable `/data`** mount (the root ramfs is a read-only initrd) and
  `\`→`/` — then calls `sys_open` (staging the translated path in a CRT-page scratch buffer so it can
  pass a user pointer); `ReadFile`→`sys_read`, `WriteFile` (from C1) →`sys_write`, `CloseHandle`→
  `sys_close` (leaving std handles + FILE* tokens alone). Also added `memcmp`. **Latent bug found + fixed:**
  the mingw CRT entry is entered with `RSP%16==8` (as if `CALL`-ed); the PE loader was giving it `%16==0`,
  which desyncs every CRT frame by 8 — C3/C4 survived by luck, but C5's `movaps`-based local-buffer init
  `#GP`'d on the misaligned `[rbp+x]`. Fixed the PE initial RSP in `proc.c` to `%16==8` (now all of
  C1–C5 are correctly aligned, not lucky). Proof: `programs/win-fileio/main.c` (first-party; the `.exe` is
  a build artifact) under `-DWINPE5_BOOT`; all FIVE Win32 milestones (C1=42, C2=7, C3=3, C4=5, C5=9) pass
  together under `-cpu qemu64,+smep,+smap` with 0 faults; validate 3/3 ALIVE; default desktop boot
  unaffected. App-compat ~28%.
- **2026-06-16** — **Track C milestone C4: tobyOS runs a STOCK C++ Windows `.exe` (global constructors
  run).** A plain `clang++ main.cpp -o win-cpp.exe` now runs unmodified: a C++ global constructor executes
  before `main` (prints its line + sets a flag) and `main` returns 5 (`[WINPE4] PASS`). KEY FINDING that
  kept this small: mingw runs global ctors via the GCC `__CTOR_LIST__` mechanism — static `__main` →
  `__do_global_ctors` walking the list and calling each ctor, **all in CPL3 user code** — NOT via the
  MSVC `_initterm(__xc_a,__xc_z)` path. So **no user-mode `_initterm` trampoline was needed** (my earlier
  C3-note assumption was MSVC-specific and didn't apply). C4 was therefore just broadening the shim
  surface for the ~13 extra imports a C++ binary pulls in across two new ucrt API sets (`convert`,
  `filesystem`): `_errno`/`localeconv`/`strerror` (return USER pointers into the CRT-data page — added an
  `errno` slot + a minimal "C"-locale `lconv` with `decimal_point="."`), `_lock_file`/`_unlock_file`
  (stdio FILE locking → no-op, single-threaded), `fputc`, `strnlen`/`wcslen`/`wcsnlen`, and
  `mbrtowc`/`wcrtomb` (minimal). Proof: `programs/win-cpp/main.cpp` (first-party source; the `.exe` is a
  build artifact) under `-DWINPE4_BOOT`; all FOUR Win32 milestones (C1=42, C2=7, C3=3, C4=5) pass together
  under `-cpu qemu64,+smep,+smap` with 0 faults (130 syscalls for the C++ startup vs C3's 26); validate
  3/3 ALIVE; default desktop boot unaffected (C4 is additive shims only — no kernel-path changes).
  App-compat ~27% → ~28%.
- **2026-06-16** — **Track C milestone C3: tobyOS runs a STOCK, off-the-shelf Windows `.exe`.** A plain
  `clang main.c -o win-hello3.exe` (no flags, no custom entry) now runs unmodified through the entire ucrt
  `mainCRTStartup` → `main` → `printf` → `exit`, printing `argc=1 argv0=win-hello3.exe` and exiting 3
  (`[WINPE3] PASS`). Four pieces. **(1) A real SWAPGS scheme.** The CRT reads `gs:[0x30]` (NtCurrentTeb),
  so a PE needs a TEB reachable via `gs:` — but the kernel had deliberately used the GS base for per-CPU
  data *without* SWAPGS. Introduced SWAPGS at every CPL3 boundary (syscall entry+exit in
  `syscall_entry.S`; ISR entry+exit conditional on `CS.RPL==3` in `isr_stubs.S`; first user-entry in
  `proc_switch.S`), backed by a per-proc `gs_base` loaded into the `IA32_KERNEL_GS_BASE` shadow in
  `do_switch`. A Win32 PE runs CPL3 with GS = its TEB; native/Linux procs get `gs_base=0` → shadow =
  `&percpu`, an *identity* swap that leaves them byte-for-byte unchanged (verified: normal desktop boot,
  user apps, C1/C2 all unaffected). **(2) A TEB + CRT-data page** mapped per PE (`pe_loader.c`): the TEB
  self-pointer at `+0x30` and stack bounds; the CRT-data page holds `argc`/`argv`/`environ`/`_commode`/
  `_fmode` (the ucrt `__p_*` accessors must return *user* pointers). **(3) A user-memory heap** — a
  per-proc bump allocator over `sys_mmap`'d anonymous memory backing `malloc`/`calloc` (never reclaims;
  `free` is a no-op — fine for a short-lived process; fresh blocks are zeroed so `calloc == malloc`).
  **(4) ~40 kernel32 + ucrt shims** (`src/syscall.c`): `_initterm`/`_initterm_e` (walk the init tables —
  empty for plain C, so nothing is called, which is necessary because a kernel shim can't call back into
  CPL3), the `__p_*` accessors, `exit`/`_exit`/`abort`, `memcpy`/`strlen`/`strncmp`,
  `VirtualQuery`/`VirtualProtect`, the critical-section no-ops, and a shared `w32_zero`/`w32_one` for the
  ~22 trivial ones. Proof: `programs/win-hello3/main.c` (first-party source; the `.exe` is a build
  artifact) under `-DWINPE3_BOOT`; all three Win32 milestones pass together under
  `-cpu qemu64,+smep,+smap` with 0 faults; validate 3/3 ALIVE; normal desktop boot unaffected. **Known
  limits (honest):** no C++ global ctors / TLS callbacks yet (they need a user-mode trampoline to call
  CPL3 init code); the SWAPGS path uses the standard CPL-conditional swap (the NMI-swapgs race that full
  kernels handle with IST is a known edge case, benign here since tobyOS ISRs don't read `gs:`).
  App-compat ~25% → ~27%.
- **2026-06-16** — **Track C milestone C2: tobyOS runs a Windows `.exe` that uses the C runtime
  (`printf`).** Two pieces on the C1 foundation. **(1) The marshalling gate now marshals stack
  arguments.** C1's gate captured only the 4 Microsoft-x64 register args (`rcx/rdx/r8/r9`); C2's reads
  `a4..a7` off the caller's stack into the arg array (bytes re-assembled offline + verified; the PE's
  initial RSP already sits at `USER_STACK_TOP_VA-0x400` so the upward reads stay inside the mapped
  stack). This is what makes variadic Win32 functions work — a real MinGW/ucrt `printf` inlines to
  `__stdio_common_vfprintf(options, FILE*, fmt, locale, va_list)` where **`va_list` is the 5th, stack-
  passed argument**. **(2) A new shimmable DLL namespace + a real printf engine.** `api-ms-win-crt-stdio-
  l1-1-0.dll` (one of the ucrt API sets) joins kernel32 in the shim table with `__acrt_iob_func` (returns
  an fd-encoding FILE* token), `__stdio_common_vfprintf`, and `puts`. `win32_vformat` (src/syscall.c) is
  a from-scratch `printf`: flags (`-`,`0`,`+`,space,`#`), width + precision (incl. `*`), length
  (`l`/`ll`/`h`/`z`/`I64`, respecting Windows' 32-bit `long`), and conversions `d/i/u/x/X/o/p/c/s/%`; it
  reads each conversion's argument from the user `va_list` (8-byte slots; `%s` is a user `char*` →
  `strncpy_from_user`) and writes via `file_write`. Since tobyOS has no real CRT DLL, these shims ARE the
  CRT for a PE — bypassing the full `mainCRTStartup` (SEH/`_initterm`/~9 ucrt API-set DLLs), which stays
  C3+. Proof: `programs/win-crt/main.c` (first-party; the `.exe` is a build artifact) calls the real
  ucrt `printf`/`puts` and prints `printf: world #42 hex=0xff char=Z pct=%`, a width/precision/flags line
  `[   42] [42   ] [00042] [+42] [tru] [0xabcd] [      rt]`, `six ints: 1 2 3 4 5 6` (stack-passed
  varargs), `%lld`/`%p`, and the exact `printf` return count (40), then exits 7 → `[WINPE2] VERDICT:
  PASS`. Clean under default boot AND `-cpu qemu64,+smep,+smap` (0 faults — the va_list/format/`%s`
  `copy_from_user` + stack-arg reads are SMAP-safe), validate 3/3 ALIVE, and C1's `win-hello.exe` still
  passes under the shared gate v2 (no regression). App-compat ~24% → ~25%.
- **2026-06-15** — **Track C milestone C1: tobyOS runs an unmodified Windows x86-64 `.exe`.** This is
  the foundation of the Windows half of gap #2 (app-compat), previously ZERO. A Windows PE does NOT
  make raw syscalls — it imports functions from DLLs through its Import Address Table — so C1 is three
  pieces, none of which the old `pe_loader.c` actually did (it was dead code with no callers, and its
  IAT-points-at-a-kernel-function design can't work: CPL3 can't call a CPL0 address). **(1) A real
  PE32+ loader** (`src/pe_loader.c`, rewritten): sniff `MZ`/`PE` in the exec path (`spawn_internal`,
  `src/proc.c`), map the headers + sections at the preferred ImageBase into user pages, zero BSS tails,
  apply `DIR64` base relocations (a no-op at the preferred base), all under SMAP uaccess windows.
  **(2) IAT → kernel-shim binding via a user-mode marshalling gate.** The loader maps one `R-X` user
  page holding a hand-verified position-independent gate plus a 10-byte thunk per import; each IAT slot
  is bound to its thunk. At runtime the PE does `call *[iat]` → thunk (`mov eax,<shim-index>; jmp gate`)
  → gate, which captures the Microsoft-x64 arg registers (`rcx/rdx/r8/r9`, callee-saved `rdi/rsi`
  preserved) into a user-stack array and issues exactly one new syscall, `ABI_SYS_WIN32_DISPATCH`.
  **(3) A kernel-side kernel32 subset** (`src/syscall.c`): a new `ABI_PERS_WIN32` personality routes
  that syscall to `win32_dispatch`, which `copy_from_user`s the args and calls the indexed shim.
  `GetStdHandle`/`WriteFile`/`ExitProcess` reuse tobyOS primitives (`sys_write`, `proc_exit`), so Win32
  console output lands on the same stdout path as native + Linux. Proof: a genuine MinGW-built
  `win-hello.exe` (`programs/win-hello/hello.c`, first-party source; the `.exe` is a build artifact)
  prints `hello from a Windows PE binary running on tobyOS` and exits 42 → `[WINPE] VERDICT: PASS`,
  clean under default boot AND `-cpu qemu64,+smep,+smap` (0 faults), validate 3/3 ALIVE. **Bug caught
  in the act:** `GetStdHandle(STD_OUTPUT_HANDLE)` compiles to `mov ecx,0xfffffff5` which zero-extends,
  so the shim must compare `nStdHandle` at DWORD width — the 64-bit sign-extended constant silently
  returned `INVALID_HANDLE` and ate the first WriteFile. App-compat ~23% → ~24%.
- **2026-06-14 (later 8)** — **Track B milestone B9 (finale): Linux threads — `clone(CLONE_VM)` runs a
  real thread in the shared address space.** Added `sys_clone_thread` (`src/fork.c`): a `clone(56)`
  with `CLONE_VM` set creates a thread that, like fork, resumes after the `clone` syscall with rax=0
  (it copies the parent's trapframe and descends through `fork_child_entry`) but installs the shared-VM
  thread fields `thread_create` uses — `is_thread=true`, `cr3 = leader->cr3` (no new PML4),
  `owns_pml4=false`, `tgid = leader` — then overrides the child's saved `user_rsp` to the
  caller-provided `stack` and sets `tls_base` (CLONE_SETTLS), publishing the new tid via
  CLONE_PARENT/CHILD_SETTID. `linux_syscall` routes `clone` with `CLONE_VM` → `sys_clone_thread`, and
  `clone` *without* it → `sys_fork` (the fork-equivalent); also mapped `sched_yield(24)`. A
  `proc_exit` for an `is_thread` proc already terminates just the thread (the leader-kills-group block
  is gated on `!is_thread`), and the reaper already spares the shared PML4 (`owns_pml4 && !is_thread`),
  so the existing thread teardown handled the new clone-threads unchanged. Proven by `/bin/linux-thread`
  (a hand-rolled Linux ELF; the clone + child path is inline asm so the parent/child split never
  depends on C touching the now-divided stack): it `clone`s a thread that runs in the **shared** address
  space, sets a shared global, and `exit`s the thread only; the parent `sched_yield`s until it observes
  the flag, then exits 0 — `[clone] thread tid=3 in tgid=2 (shared VM)`, the thread runs as
  `linux-thread+T`, **`[LXTHREAD] VERDICT: PASS exit=0`**, clean under default boot AND `-cpu
  qemu64,+smep,+smap` (0 faults, 0 unhandled), 3×100 s validate ALIVE. **This completes the discrete
  high-value Track B milestones** (B1–B9: static & real-libc binaries, dirs, signals, dynamic +
  multi-DSO loading, a shell, threads); what remains is incremental syscall breadth
  (`CLONE_CHILD_CLEARTID`+futex for `pthread_join`, poll/epoll, broader sockets, glibc) rather than
  discrete milestones. App-compat ~21% → ~23%.
- **2026-06-14 (later 7)** — **Track B milestone B8: a real shell — `busybox sh` runs external
  commands via fork/execve/wait4.** Mapped the process-control syscalls in `linux_syscall`:
  `fork(57)`/`vfork(58)`/`clone(56)` (non-`CLONE_VM`; threads are deferred) → `sys_fork`;
  `execve(59)` → `sys_execve` (the Linux arg layout `(path, argv, envp)` is byte-identical to
  tobyOS's, and execve already re-latches the Linux personality from the new image — B1);
  `wait4(61)` → `proc_wait`, re-encoding the result into a **Linux wait status** (`(code&0xff)<<8`
  so `WIFEXITED`/`WEXITSTATUS` work) with a new `proc_any_child(ppid)` helper (proc.c/proc.h) for
  `wait4(-1)`; `setpgid`/`getpgid`/`getpgrp`/`setsid` as no-ops/identity (tobyOS has one global
  foreground pid, not POSIX process groups). Proven by `busybox sh -c 'busybox echo A; busybox echo
  B'` — a command *list*, so ash can't exec-optimize the first command away: the shell **forks**
  (`parent pid=2 -> child pid=3`), the child **execve**s busybox and prints `shell-forked-A`, the
  parent **wait4**s, then the trailing command exec-replaces in place and prints `shell-exec-B` —
  **`[LXSH] VERDICT: PASS exit=0`**, clean under default boot AND `-cpu qemu64,+smep,+smap`
  (0 EXCEPTION/PANIC/#PF, 0 unhandled), 3×100 s validate ALIVE. **Found + fixed another latent kernel
  bug:** `sys_execve` (`src/fork.c`) packed argv/envp/auxv onto the new image's user stack with plain
  `memcpy`/pointer stores — **no SMAP `stac` window** — so an `execve` from CPL 3 page-faulted under
  `+smap` (cr2 in the user stack, kernel rip). Native execve was effectively never exercised (the
  desktop uses `toby_spawn`, and the terminal's fork+exec was broken), so this was latent; wrapped the
  pack in `uaccess_begin/end` (the same pattern elf.c uses). App-compat ~19% → ~21%.
- **2026-06-14 (later 6)** — **Track B milestone B7: end-to-end multi-DSO dynamic loading + a real
  kernel-bug fix.** tobyOS now runs Alpine's real `file` binary, which depends on a *separate* shared
  library (`libmagic.so.1`, beyond libc). `file --version` makes the real musl `ld.so` open
  `/lib/libmagic.so.1` and **file-backed-mmap** its segments — one whole-span `MAP_PRIVATE`
  reservation, then a `MAP_FIXED` file map per segment at the library's base + offset — relocate it,
  resolve its symbols, and print `file-5.45`: **`[LXMULTI] VERDICT: PASS`**, clean default +
  `-cpu qemu64,+smep,+smap` (0 faults, 0 unhandled), 3×100 s validate ALIVE. Getting here **found and
  fixed a genuine latent kernel bug**: `src/mmap.c`'s `PAGE_MASK` was `~(PAGE_SIZE - 1)`, and
  `PAGE_SIZE` comes from `pmm.h` as `4096u` (32-bit unsigned), so the mask was `0x00000000FFFFF000` —
  `page_align_down`/`page_align_up` **silently truncated any address ≥ 4 GiB**. tobyOS's mmap region
  is at 16 TiB (`MMAP_REGION_BASE`), so ld-musl's `MAP_FIXED` segment maps (e.g. `0x100000018000`)
  were aligned down to `0x18000` — the loader then wrote relocations to the wrong page and NULL-deref'd.
  Fixed to `~((uint64_t)PAGE_SIZE - 1)`. (So B6's file-backed mmap was correct all along; this
  truncation was the real blocker behind the earlier `dynmain`/`file` faults.) Diagnosed with a
  temporary `-DLXMMAP_TRACE` mmap tracer (kept, off by default). The multi-DSO `file`/`libmagic.so.1`
  are a real Alpine package, opt-in and not committed (README documents the fetch). App-compat ~17% → ~19%.
- **2026-06-14 (later 5)** — **Track B milestone B6: file-backed mmap.** A real `ld.so` maps a shared
  library beyond libc with `mmap(fd, MAP_PRIVATE, offset)`; tobyOS's demand-paged `VMA_FILE` fault
  path was a stub (`page_fault.c` mapped a zero page; the file read was a `TODO`), and musl closes the
  library fd the instant `mmap` returns — so lazy by-fd paging is impossible. Implemented file-backed
  mmap in `linux_syscall` as an **eager read**: reserve writable anon pages, read the file's bytes in
  at the requested offset (a short read at EOF leaves the tail zeroed = the file-hole/.bss semantics),
  then tighten to the requested protection. The mmap **`offset` (Linux's 6th arg = user `r9`)** is
  dropped by the dispatch (only a1–a5 reach C); rather than touch the hot `.S` path, it's read back
  from the saved `syscall_regs` block at the top of the kstack (the same block signal delivery uses).
  Proven by `/bin/linux-mmaptest` (a hand-rolled Linux ELF): mmaps `/etc/motd` at offset 0 and
  `/bin/c_hello` at a page offset (4096), asserting the mapped bytes equal `read()`'s — **`[LXMMAP]
  VERDICT: PASS`**, clean default + `-cpu qemu64,+smep,+smap` (0 faults), 3×100 s validate ALIVE; B1–B5
  harnesses unaffected (LXABI/LXSIG/LXDYN all still PASS). **Honest scope:** the file-backed-mmap
  *primitive* is proven, but a full "real `ld.so` loads a separate real `.so`" end-to-end demo is NOT —
  the Alpine minirootfs has no multi-DSO musl binary, and a hand-rolled non-musl PIE (`dynmain` +
  `libgreet.so`) makes ld-musl NULL-deref *internally* (`cr2=0` inside the loader — a ld.so/ABI-
  expectation mismatch, not a file-backed-mmap bug; the experiment was removed rather than ship a
  failing harness). App-compat ~16% → ~17%.
- **2026-06-14 (later 4)** — **Track B milestone B5: DYNAMICALLY-linked Linux binaries run via the
  real musl `ld.so`.** Fetched the Alpine minirootfs's PIE busybox (`PT_INTERP=/lib/ld-musl-x86_64.so.1`,
  `NEEDED libc.musl-x86_64.so.1`) + the `ld-musl` loader (which IS libc in musl), branded the busybox
  `EI_OSABI=Linux`, and bundled them opt-in into the initrd (`/bin/busybox-dyn`, `/lib/ld-musl-x86_64.so.1`).
  The kernel's existing PT_INTERP path (originally for native `ld-toby`) loads the PIE program + the
  genuine ld-musl interpreter at non-overlapping bases and hands off via auxv (AT_PHDR/AT_BASE/AT_ENTRY/
  AT_RANDOM); ld-musl then self-relocates, relocates busybox, resolves symbols (it recognises its own
  soname for the `NEEDED libc.musl`, so no extra file is opened), sets up TLS via `arch_prctl`, and runs
  the applet. A 7-applet dynamic battery (`-DLINUXDYN_BOOT`: echo/uname -a/pwd/cat/wc/stat/ls) is
  **`[LXDYN] VERDICT: PASS pass=7/7`**, clean under default boot AND `-cpu qemu64,+smep,+smap` (0
  EXCEPTION/PANIC/#PF, 0 unhandled syscalls). **This required zero new kernel code** — the B1–B4 Linux
  personality/syscall layer + the pre-existing dynamic-ELF loader already suffice, because busybox's only
  DSO is the kernel-loaded interpreter (so all of ld-musl's own mmaps are anonymous, offset 0). The commit
  is just the boot harness + opt-in initrd wiring + docs. **Deferred to B6:** a dynamic program that loads
  a *separate* shared library (beyond libc) needs working **file-backed mmap** — `page_fault.c`'s
  `VMA_FILE` fault path is a stub (maps a zero page; the file-read is a `TODO`), and the mmap `offset`
  (6th syscall arg) isn't plumbed through `syscall_dispatch`/`do_syscall` (only a1–a5 reach C). busybox+musl
  doesn't exercise that path. App-compat ~14% → ~16%.
- **2026-06-14 (later 3)** — **Track B milestone B4: Linux signal delivery + cwd-`.` fix.** Wired the
  Linux signal ABI onto tobyOS's native signal layer in `linux_syscall()`: `rt_sigaction(13)` converts
  the Linux `struct sigaction` (handler/flags/restorer/mask field order) into tobyOS's and registers
  it in `proc.sigstate`; crucially it records musl's supplied `sa_restorer` as the proc's sigreturn
  trampoline (`sigstate.restorer`) — a Linux binary never calls the tobyOS `SYS_SIGRESTORER`, so
  without this delivery had no return path. `rt_sigprocmask(14)` forwards to `sys_sigprocmask`
  (the 64-bit sigset layouts match), `rt_sigreturn(15)`→`sys_sigreturn`, and `kill(62)`/`tkill(200)`/
  `tgkill(234)`→`sys_kill`. Native delivery then enters the handler with `RDI=signo` and a frame
  whose return address is the restorer, so a musl 1-arg handler runs and returns correctly. Proven by
  `/bin/linux-sigtest` (a hand-rolled Linux x86-64 ELF, same brandelf shape as linux-hello): it
  installs a `SIGUSR1` handler with an `mov $15,%rax; syscall` restorer, `kill`s itself, and exits 0
  iff the handler ran — `[LXSIG] VERDICT: PASS`. Separately fixed `resolve_user_path` to strip a
  leading `.`/`./` (a bare `.` resolves to the cwd) so `busybox ls` with no path arg (which does
  `opendir(".")`) works — the busybox battery grew to **10/10 PASS** (`+ ls` no-path). All clean under
  default boot AND `-cpu qemu64,+smep,+smap` (0 EXCEPTION/PANIC/#PF — the signal-frame `copy_to_user`
  and sigaction marshalling are SMAP-safe), 3×100 s validate ALIVE. **Deferred:** SA_SIGINFO 3-arg
  handlers (they'd receive tobyOS-layout siginfo/ucontext) and the dynamic loader (`ld.so`).
  App-compat ~12% → ~14%.
- **2026-06-14 (later 2)** — **Track B milestone B3: directory listing — `busybox ls` works.** Added a
  `FILE_KIND_DIR` file kind (`struct file` gains a kmalloc'd `dirpath` + `dir_off` resume cursor;
  `file_close` frees it, `file_clone` deep-copies it). A Linux `open(2)`/`openat(257)` whose target
  is a directory now returns this fd (via `linux_open_dir`), and **`getdents64(217)`** re-opens the
  dir with `vfs_opendir` and streams `linux_dirent64` records — 8-byte-aligned `d_reclen`, `d_type`
  from the VFS entry type, `d_name` copied out — resuming after `dir_off` so a small user buffer is
  handled across calls (returns `-EINVAL` only if not even one entry fits, 0 at end). `fstat` on a
  dir fd now reports `S_IFDIR`. Verified: the busybox battery grew to 9 applets — added `ls /bin`
  (uses `d_type`, skips per-entry stat) and `ls -la /bin` (forces per-entry `lstat`); the latter
  prints a correct long listing (perms / link count / uid / gid / exact sizes — busybox 1131168,
  c_hello 48000 — / name). `[LXBB] VERDICT: PASS pass=9/9` + B1 `[LXABI] PASS`, clean under default
  boot AND `-cpu qemu64,+smep,+smap` (0 EXCEPTION/PANIC/#PF — getdents64's `copy_to_user` is
  SMAP-safe), 3×100 s validate ALIVE. **Known limitation:** cwd-relative `.` listing (`ls` with no
  path) still fails on a `.`-path resolution quirk — absolute paths work. **Deferred:** Linux signal
  delivery and the dynamic loader (`ld.so`) for non-static binaries. App-compat ~10% → ~12%.
- **2026-06-14 (later)** — **Track B milestone B2: tobyOS runs a REAL musl-libc binary (busybox).**
  Building on B1's personality + translation layer, fetched a prebuilt, unmodified, statically-linked
  **busybox 1.35.0 (musl)** (1.1 MB), branded it `EI_OSABI=Linux` at initrd-staging time (code
  untouched), and ran a 7-applet battery: `echo`, `true`, `uname -a`, `pwd`, `cat /etc/motd`,
  `wc /etc/motd`, `stat /etc/motd` — **all exit 0** (`[LXBB] VERDICT: PASS pass=7/7`). This exercises
  real-libc startup (arch_prctl TLS, set_tid_address), stdio (writev), real file I/O
  (`open`/`read`/`fstat`/`close` — `wc` prints the correct `12 32 285`), and the new work:
  (1) **Linux `struct stat` translation** — `stat`/`lstat`/`fstat`/`newfstatat` build the 144-byte
  Linux layout from tobyOS's `vfs_stat` (tobyOS's `S_IF*` bits already equal Linux's), so `busybox stat`
  prints the right size/blocks/type; (2) **AT_RANDOM** added to the auxv in both the spawn (`proc.c`)
  and execve (`fork.c`) paths (points at the zeroed 16-byte stack scratch pad — the libc stack canary;
  glibc requires it, musl falls back); (3) quiet `sendfile`/`fcntl` fallbacks so the log stays clean.
  Clean under default boot AND `-cpu qemu64,+smep,+smap` (0 EXCEPTION/PANIC/#PF — the new
  `copy_to_user`/`strncpy_from_user` paths are SMAP-safe), 3×100 s validate ALIVE. busybox is opt-in
  and **not committed** (GPLv2 + size; `programs/busybox/README.md` documents the one-line fetch +
  `.gitignore` excludes it). **Deferred to B3:** `getdents64`/`ls` (needs a directory-fd abstraction
  tobyOS lacks), Linux signal delivery, and the dynamic loader (`ld.so`) for non-static binaries.
  App-compat ~7% → ~10%.
- **2026-06-14** — **Foreign-binary compatibility, Track B milestone B1: run an unmodified Linux
  x86-64 binary.** Gap #2 (app-compat) is ecosystem-scale; after surveying the three tracks
  (A: broaden POSIX source / port musl; B: Linux ELF binary compat; C: Win32 PE) the user chose
  **B** (highest demonstrable value; the SYSCALL entry path, the auxv stack, and FS-base TLS were
  already Linux-shaped, so the headline win was also the most leveraged). Added a per-process **ABI
  personality** (`struct proc.personality`, `ABI_PERS_TOBY`/`ABI_PERS_LINUX` in `abi.h`) latched at
  ELF load from `e_ident[EI_OSABI]` (new `EI_OSABI`/`ELFOSABI_LINUX` in `elf.h`; `struct
  elf_load_info.osabi` populated in `elf.c`; set in `proc.c` spawn + `fork.c` execve). `syscall_dispatch`
  now branches: a Linux-personality process is routed through **`linux_syscall()`** (new, `src/syscall.c`)
  which translates Linux x86-64 syscall numbers onto tobyOS primitives — 1:1 calls forward to the
  native dispatcher, Linux-specifics (`arch_prctl` SET/GET_FS → `thread_set_tls`, `writev`/`readv` →
  fan-out over `sys_write`/`sys_read`, `set_tid_address`, `exit_group`, `clock_gettime`, `uname`,
  `nanosleep` timespec→ns, `mmap` Linux→VMA flag xlate, `brk` Linux return convention,
  `ioctl`→ENOTTY) handled inline; unknown numbers log + `-ENOSYS`. **Proof:** `/bin/linux-hello`
  (`programs/linux-hello/main.c`) — a genuine Linux x86-64 static ELF built with `clang
  --target=x86_64-linux-gnu -nostdlib` (raw syscalls, NO libtoby/headers) then `brandelf`'d
  (`EI_OSABI=3`, code untouched); it runs the libc-style startup (`arch_prctl` TLS verified via
  `%fs:0`, `write`+`writev`, `exit_group(42)`). `-DLINUXABI_BOOT` harness spawns it → **`[LXABI]
  VERDICT: PASS exit=42`** with its stdout in the serial log, clean under default boot AND `-cpu
  qemu64,+smep,+smap` (0 EXCEPTION/PANIC/#PF — the `copy_from_user`/`put_user_*` paths are
  SMAP-safe), 3×100 s validate ALIVE. Native binaries (OSABI 0) are untouched → zero regression risk.
  **B2 (next):** real musl/`busybox-static` (needs broader syscalls + AT_RANDOM + fstat struct xlate),
  Linux signals, and the dynamic loader. App-compat ~5% → ~7%.
- **2026-06-13 (later 7)** — **TobyFS crash-consistency + stress harness.** The write-ahead journal
  (replay-on-mount) + allocator/indirect-block/bitmap machinery were wired but never tested under
  crash or load. Added `bcache_discard()` (drops a device's cache entries WITHOUT flushing dirty
  blocks — models a power loss; `bcache_invalidate()` flushes first, wrong for crash modelling) and
  a crash-injection knob in `journal_commit()` (compiled in but inert by default — two always-false
  checks) that stops the commit before the marker or after it (before checkpoint).
  `tobyfs_crash_stress_test()` (`-DTOBYFS_STRESS_SELFTEST`, `[TFST]`): with a baseline file synced
  to a 16 MiB ramdev, it creates a file with a crash injected, `bcache_discard()`s (power loss), and
  remounts (`journal_recover`) — a **pre-commit crash rolls the create back** (file absent), a
  **post-commit crash replays it** (file present), the baseline survives, and the integrity checker
  stays non-FATAL in both; then a 4.5 MiB file exercises the double-indirect path and ~80 randomised
  mixed ops (create/rewrite/delete across direct/single/double-indirect sizes) verify every byte
  with the checker run throughout. All PASS, 0 failures, 0 panics (t1 rollback, t2 replay — "journal:
  replaying txn", t3 double-indirect, t4 churn 65 ops / 59 writes verified / 17 survivors / final
  checker clean). Default build healthy (3x100 s ALIVE, 0 panics, self-test absent, live /data tobyfs
  still mounts clean). **Completes the active queue (ext4 journaling, memory, DHCPv6, FS crash/stress).**
  Storage ~30% → ~31%.
- **2026-06-13 (later 6)** — **Stateful DHCPv6 client (RFC 8415).** IPv6 had SLAAC but not the
  stateful path. `src/dhcpv6.c` (+ `dhcpv6.h`): a DHCPv6 client that builds SOLICIT (+ Rapid
  Commit) / REQUEST / INFORMATION-REQUEST with the standard options (CLIENTID as a DUID-LL, IA_NA,
  ELAPSED_TIME, ORO for DNS), sends them over UDP 546->547 to ff02::1:2 with a proper IPv6 pseudo-
  header checksum, and on receive handles both the 2-message (rapid-commit) and 4-message
  (ADVERTISE->REQUEST->REPLY) exchanges — extracting the assigned address from IA_NA/IAADDR and the
  DNS server from option 23, matching the transaction-id per exchange. `icmpv6.c`: the RA handler
  now reads the **M/O flags** (byte 5) — M triggers a stateful SOLICIT, O-only an
  INFORMATION-REQUEST; SLAAC still runs alongside. `ipv6.c`: a minimal **UDP-over-IPv6 demux**
  (the `IPV6_NH_UDP` case was a stub) routes dst port 546 to the client, and
  `ipv6_dhcp6_configure()` installs the server-assigned address (now accepted as "for us", incl.
  its solicited-node multicast). Dormant in production — QEMU's SLIRP sends no RAs and has no DHCPv6
  server — exactly like SLAAC. **Validation:** `-DDHCPV6_SELFTEST` (`[DHCP6T]`) — t1 SOLICIT
  well-formed (50 B, all required options present); t2 synthetic rapid-commit REPLY binds the
  address (2001:db8::abcd) + captures DNS (2001:db8::53); t3 full ADVERTISE->REQUEST->REPLY advances
  state and binds. All PASS, 0 failures; default build healthy (3x100 s ALIVE, 0 panics, IPv6
  link-local up). Networking ~31% → ~32%.
- **2026-06-13 (later 5)** — **2 MiB large pages.** Completes the Memory queue item (large pages +
  compression). `pmm.c`: `pmm_alloc_2m()`/`pmm_free_2m()` allocate a 2 MiB-aligned huge frame (512
  contiguous pages on an aligned base, the requirement for an x86_64 PD leaf). `vmm.c`: `vmm_unmap()`
  is now huge-aware — it steps a whole 2 MiB leaf instead of failing 4 KiB-at-a-time (a latent bug:
  huge leaves couldn't be torn down before); new `vmm_leaf_size()` reports 0 / 4 KiB / 2 MiB.
  `heap.c`: `grow()` backs arenas >= 2 MiB with huge leaves (one PD entry per 2 MiB vs 512 PTEs + a
  PT page — less page-table memory + TLB pressure), rounding the brk up to 2 MiB first; **best-
  effort** — on fragmentation (no aligned 2 MiB run) it transparently falls back to the 4 KiB path,
  so nothing that would have allocated now fails. Arenas are never released, so no huge-teardown is
  needed; safe against `vmm_protect` (only ever called on user VAs, never the kernel heap).
  **Validation:** `-DHUGEPAGE_SELFTEST` (`[HUGEPT]`) — t1 direct (pmm_alloc_2m 2 MiB-aligned / −512
  free pages, vmm_map as one huge leaf, translate at base + mid-leaf, R/W across the full 2 MiB,
  huge-aware unmap, +512 frames returned) and t2 heap (`kmalloc(4 MiB)` lands in a huge-backed arena,
  data round-trips, 2 PT pages saved) — both PASS. Engages on **every** production boot (the
  compositor backbuffer is a 2×2 MiB huge arena — logged once at boot). Canonical default-build
  validation: 3×100 s boots all ALIVE at 627 heartbeats / ~64 s guest uptime, 0 panics, huge arena
  active each run. (Investigated a one-off corrupt-frame #BR during a batch's first-run cold-start
  window — reproduced as a freeze on `main` too, lands outside the huge arena, and never recurred
  across 10+ clean boots; it's the pre-existing cold-start harness artifact, not this change.)
  Memory ~25% → ~26%.
- **2026-06-13 (later 4)** — **Memory compression (zram/zswap-style, LZ4).** Anonymous pages are
  mostly zeros + repeated structure, so most of what swap would write is highly compressible. Added
  a compressed in-RAM page store in front of swap: `src/lz4.c` (a minimal dependency-free LZ4
  block-format codec — canonical overlap-safe decompressor + a greedy single-candidate compressor)
  and `src/zram.c` (the pool: `zram_store` compresses a page into a heap blob sized to the
  compressed length and returns a slot, `zram_load` decompresses+frees, incompressible pages
  rejected; stats for ratio + frames reclaimed). `swap_out` now tries zram first and only spills
  *incompressible* pages to the disk swap area — so eviction works even with no swap disk;
  `swap_in`/`swap_free` route by a per-slot `compressed` flag; `swap_init_dev()` lets a test target
  a ramdev. Eviction isn't wired to a caller yet, so the only boot-time change is one ~80 KiB pool
  allocation (the codec/pool are otherwise inert until something drives `swap_out`). **Validation:**
  `-DMEMCOMP_SELFTEST` harness (`[MEMCT]`) — LZ4 round-trip + ratios (zeros 157.5x, repeat 70.6x,
  text 1.13x, random correctly rejected), zram store/load on a 96-page mixed batch (stored 72,
  3.31x aggregate, **50 whole frames reclaimed**, every load bit-exact), and swap routing
  (compressible→zram with/without disk, incompressible→disk fallback, incompressible+no-disk→
  rejected). All PASS, 0 failures, 0 panics; default build healthy (3x100s boots ALIVE, self-test
  absent, zram comes up under the real `swap_init`). Memory ~24% → ~25%. **2 MiB large pages next.**
- **2026-06-13 (later 3)** — **ext4 JBD2 journalling: crash-atomic writes + recovery.** ext4 was
  read-write but applied changes in place (a crash mid-op could leave a half-allocated inode /
  dangling dir entry / torn data) and refused any needs-recovery image. Added a real write-ahead
  journal (`include/tobyos/jbd2.h`, `src/ext4.c`): every create/unlink/mkdir/write runs as a
  transaction that **stages** the blocks it touches, logs them to the journal as
  `[descriptor][data..][commit]`, **checkpoints** them to their homes, then marks the journal clean
  — so a crash is all-or-nothing. `jbd2_recover()` at mount scans from the journal superblock's
  `s_start`, replays every transaction with a matching commit block (idempotently, so a crash during
  the original checkpoint OR during replay heals next mount), and discards any uncommitted tail.
  Mount now **drives** a journal it understands (classic format: no metadata_csum/64bit/async-commit)
  and recovers a needs-recovery image instead of refusing it; a journal we can't drive is refused
  rather than corrupted by in-place writes. Non-journalled images keep the exact prior path (no
  behaviour change). Formatter gains `ext4_format_ex(dev, with_journal)` (JBD2 journal = inode 8,
  one contiguous extent). **Validation:** new `-DEXT4_JOURNAL_SELFTEST` crash-consistency harness
  (`[EXT4JT]`) injects a power loss at each transaction phase on a journalled ramdev — all 8 steps
  PASS, with recovery's replay counts proving the semantics (create crash<commit → replay 0 /
  rolled back; crash>commit → replay 5 / applied; mid-checkpoint → replay 5 / healed; data overwrite
  crash<commit → 0 / old kept; crash>commit → 3 / new applied; fs usable after). Original
  `-DEXT4_SELFTEST` still 6/6 (no regression). Default build healthy (branch vs main A/B at 100s
  wall both reach ~600 heartbeats / 61 s guest uptime, 0 panics; the ext4 code is provably inert at
  boot — "no ext4 partition discovered"). Limitation: validated via our own journal + crash
  injection, not against a real Linux metadata_csum journal (would need a host-created dirty image).
  Storage ~29% → ~30%.
- **2026-06-13 (later 2)** — **Scheduler maturity: interactivity boost + load-balanced stealing.**
  Two MLFQ-flavoured refinements on the existing priority/aging/timeslicing/stealing scheduler.
  (1) **Interactivity (I/O) boost**: a per-proc `io_boost` in `eff_prio` -- a proc that cedes the
  CPU with quantum left (blocking on pipe/socket/key, or voluntarily yielding) earns a one-class
  bonus so it out-ranks CPU-bound peers on wakeup; a proc that burns a full quantum has it cleared
  (`sched_tick`). Bounded + aging still applies, so nothing starves. (2) **Load-balanced
  stealing**: `steal_one` steals from the BUSIEST remote runqueue, not the first non-empty one.
  Proven with a pipe-blocking harness (mctest `pipeint` + `-DSCHEDINT_BOOT`): a reader blocked on
  a feeder-paced pipe under 6 CPU hogs oversubscribing 4 cores, all PRIO_NORMAL -- **io_boost on:
  max wake latency 0 ms / 112 wakeups; off: max 22 ms / 89** (+26% interactive throughput, stall
  eliminated). No priority-weighting regression (pure CPU hogs never earn the boost). Scheduler/SMP
  ~22% -> ~23%.
- **2026-06-13 (later)** — **IPv6 SLAAC: global address autoconfig + default router.** IPv6 was
  link-local only; added RFC 4862 stateless autoconfiguration. `ipv6_init` sends a Router
  Solicitation (ff02::2, with SLLAO); the RA handler parses Prefix Information options and, for a
  /64 with the autonomous flag, forms a global address (prefix | our link-local interface id) and
  records the RA source as the default router (caching its MAC from the RA's SLLAO). `ipv6_send`
  gained source selection (global src for global dst) and next-hop selection (on-link /64 direct,
  else via the default router); dst-for-us + solicited-node membership cover the global address.
  RS confirmed on the wire (QEMU pcap, ICMPv6 type 133); RA-parse path validated by an opt-in
  self-test (`-DSLAAC_SELFTEST`) feeding a synthetic 2001:db8::/64 RA -> global
  2001:db8::<iid> PASS (QEMU SLIRP doesn't emit RAs). Remaining v6 gap: DHCPv6 (stateful, M/O
  flags) and address lifetimes/DAD. Networking ~30% -> ~31%.
- **2026-06-13** — **SA_SIGINFO + TTY job-control keys (security-depth bundle 100% done).**
  SA_SIGINFO handlers now get the full `void(int, siginfo_t*, void*)` form: the kernel builds a
  `siginfo_t` (signo/code=SI_USER/sender pid+uid, recorded per pending signal at send time) and a
  `ucontext_t` (uc_sigmask + uc_mcontext from the interrupted trapframe) on the user stack above
  the sigreturn frame and passes them in RSI/RDX; all writes via `copy_to_user`. New
  siginfo_t/mcontext_t/ucontext_t mirrored in libtoby with sa_handler/sa_sigaction as a
  layout-compatible union. Keyboard TTY keys: Ctrl-C->SIGINT already existed; added
  Ctrl-Z->SIGTSTP (stops via PROC_STOPPED; `kill -CONT` resumes) and Ctrl-\->SIGQUIT to the
  foreground proc (no-op under the GUI where no console fg job is set). `/bin/sigtest` now 10/10
  incl. SA_SIGINFO (handler sees signo, si_pid=child, user-half ucontext RSP), PASS on default CPU
  and under `qemu64,+smep,+smap`, 0 exc. NOTE: full fg/bg/jobs + process groups remain out of
  scope -- they don't fit the kernel-builtin-shell + GUI-launch model (tobyOS has a single global
  foreground pid, not POSIX process groups). Security ~18% -> ~19%.
- **2026-06-12 (later)** — **Per-copy uaccess; the whole-syscall SMAP window is gone.** Replaced the
  coarse "stac for the entire syscall body" model with Linux-style per-copy accessors
  (`copy_from_user`/`copy_to_user`/`strncpy_from_user`/`clear_user`/`put_user_*`/`get_user_*` in
  `uaccess.h`), each range-checking the user pointer and bracketing only the actual copy in a
  stac/clac window; `syscall_entry.S` no longer opens a blanket window. ~80 syscall handlers
  converted: large I/O (read/write/send/recv/clip/gl-submit/tcp/tls/unix) **bounces** through a
  kmalloc'd kernel buffer at the boundary so the VFS/pipe/net stacks never see a user pointer;
  paths/strings via `strncpy_from_user`; struct out-params + staging arrays via `copy_to_user`;
  argv/envp pointer arrays slot-by-slot via `get_user_u64`. Also signal.c (sigaction/procmask/
  sigreturn + the signal frame written to the user stack via `copy_to_user`), fork.c (execve),
  thread.c (futex pre-touch + `thread_join` status), module.c. A stray kernel deref of a user
  pointer anywhere outside an accessor now **faults under SMAP** instead of silently succeeding.
  Validated under `qemu64,+smep,+smap` (hardware SMAP enforcing): desktop+login boot clean,
  `/bin/sigtest` 9/9 PASS (signal-frame copy-out, fork argv, SA_RESTART, pipe bounce all
  exercised), interactive login OK (keystrokes via `gui_poll_event` copy-out, username via
  `strncpy_from_user` -- 'tobyroot' rejected, 'toby' accepted), 0 exceptions; 9 default boots
  across configs all alive. Closes the last open item of gap #5 (security depth). Security ~17% -> ~18%.
- **2026-06-12** — **SA_RESTART + job control; fork/CoW was never actually functional — 4
  latent bugs fixed.** Security-depth bundle: (1) **SA_RESTART** — an EINTR'd blocking syscall
  whose caught handler has SA_RESTART is transparently re-executed (delivery rewinds the saved
  user RIP onto the `syscall` insn with RAX = the syscall number; args still live in the saved
  trapframe). (2) **Job control** — new `PROC_STOPPED` state; SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU
  park the proc, SIGCONT resumes at *send* time per POSIX (stops ↔ CONT cancel each other's
  pending bits), SIGKILL wakes a stopped proc to die. (3) The per-CPU LAPIC tick now delivers
  signals when it interrupts ring 3 — before, only the BSP's PIT path delivered, so a CPU-bound
  proc on an AP dodged fatal/stop signals indefinitely. (4) **ABI fix**: kernel SA_* flag values
  collided with libtoby's Linux-style values (user `SA_NOCLDSTOP`=0x1 read as kernel
  `SA_RESTART`) — kernel now uses Linux values; libtoby gains SA_NODEFER/SA_RESETHAND/
  SIGTTIN/SIGTTOU, errno.h gains EINTR. **The new fork-based sigtest cases then exposed that
  fork/CoW had never worked** (nothing in-tree forked and checked semantics; the terminal's
  fork+exec path was silently broken): (a) fork resumed the child at the spawn-time ELF entry —
  children re-ran `_start` over a CoW image; now the parent's saved syscall register block is
  copied onto the child kstack and the child descends through the normal syscall unwind with
  rax=0, all registers restored. (b) `vmm_cow_fork` write-protected the parent's PTEs but never
  flushed the TLB — the parent wrote through stale writable entries, silently corrupting shared
  pages. (c) CoW refcounts started at 1 (spawn-time pages predate the refcount system), so the
  sole-owner fast path made the *shared* page writable for both procs — shared writable stacks.
  (d) proc teardown freed leaf frames unconditionally, handing a parent's still-shared pages
  back to the PMM when a fork child exited. Plus: kernel-mode #PF on user-half addresses
  (kernel writing user memory under the SMAP window — syscall out-params, signal frames) now
  routes through the CoW/demand handler instead of dying. Verified via extended `/bin/sigtest`
  (`-DSIGTEST_BOOT`): 9/9 checks incl. SA_RESTART restart, EINTR without it, and
  SIGSTOP-park (LAPIC path, CPU-spinning child)/SIGCONT/SIGKILL observed via /proc; two full
  fork/wait cycles per run; 3×60 s default boots clean. Security ~15% → ~17%; Memory: CoW fork
  *actually* works now.
- **2026-06-11** — **SMP desktop-freeze root-caused & fixed (BKL livelock) + bringup/idle
  hardening.** The long-standing SMP-only hard desktop freeze (gui_tick heartbeat just stops,
  no fault; needed `-smp >= 2`) was root-caused via non-invasive QMP captures as a **BKL
  livelock between pid 0 and /bin/login**: pid 0's `idle_loop` held the BKL across its *entire*
  body (net + USB/input + gui_tick + shell as one long critical section) while login's main
  loop busy-polled `SYS_GUI_POLL_EVENT`/`SYS_YIELD` from an AP, taking the BKL on every
  syscall — at the freeze both live CPUs sampled ~100% inside `bkl_enter` and gui_tick never
  completed (no active TCP connections; an earlier TCP-suspect stack was pid 0's own
  non-blocking `net_service_tick`). Fix bundle: (1) the BKL is a **fair ticket lock** and
  `tcp_poll_until` drops it across its hlt-wait; (2) `idle_loop` now takes/releases the BKL
  **per phase** instead of one whole-iteration hold; (3) `sys_nanosleep` now **drops the BKL
  and genuinely idles** in sti;hlt (was a spin-yield that, on an AP with an empty queue, held
  the BKL for the entire sleep); (4) **login sleeps ~15 ms between event polls** instead of
  busy-yielding — its cost at the login screen fell to ~6 ms CPU / 4k syscalls per 24 s run
  (was a full spinning core); end-to-end sign-in re-verified via QMP sendkey. (5) AP bringup
  retries missed SIPIs (up to 3x INIT-SIPI-SIPI, gated on a `started` flag the AP sets before
  touching any lock, so a live AP is never re-INIT'd; `-DSMP_SIPI_FLAKE` fault-injector
  exercises the retry in QEMU) — targets the EliteDesk "cpu N: timed out waiting for online
  flag" race. Diagnostics from the chase are gated behind `-DSMP_DIAG`. Two hard-won
  methodology lessons recorded for posterity: per-acquire BKL event logging **masks** the
  freeze (Heisenbug — single-store counters + external QMP sampling only), and freeze
  detection must use **heartbeat-count stall while QEMU is still running** (a wall-clock-capped
  serial log ends mid-heartbeat and is indistinguishable from a freeze; a 25 s kill landing at
  ~13.2 s guest uptime masqueraded as a "deterministic 13.2 s freeze" twice). Validation
  caveat: the freeze reproduced 3/3 pre-fix but stopped reproducing on this host after a
  Windows update, so the fix is validated as mechanism-directed + no-regression (7/7 boots
  alive incl. under 8-way host load), pending EliteDesk confirmation. Scheduler/SMP ~21% → ~22%.
- **2026-06-08** — **Priority scheduling + fair per-AP timeslicing.** The scheduler was a pure
  per-CPU FIFO round-robin with one front-of-queue GUI boost; APs never preempted (only the BSP
  PIT did), so a CPU-bound proc stolen onto an AP monopolised that core until it blocked/exited.
  Added (1) a HIGHER-runs-first `prio` on `struct proc` (classes IDLE/-2..RT/+2, **0==NORMAL so a
  zero-initialised PCB defaults correctly with no per-site code**), feeding a highest-effective-
  priority dequeue/steal (`src/sched.c`) with **FIFO-within-level** (a tree of all-NORMAL procs
  behaves byte-identically to the old FIFO) and **aging anti-starvation** (effective priority
  climbs +1 per ~60 ms a proc waits READY, from a per-enqueue `enq_tick`, capped so a starved
  IDLE proc can out-rank a running RT proc); (2) **fair per-AP timeslicing** — `sched_tick(regs)`
  now preempts when the per-CPU LAPIC timer interrupts **ring-3** user code (where the proc holds
  no kernel lock/BKL, the same safety invariant the PIT preemption relies on) and the proc's
  class quantum (`sched_quantum_for`, reset on switch-in) is spent; `apic_timer_isr` EOIs before
  the tick so a preemptive switch never strands the LAPIC un-acked; (3) a userland surface —
  `SYS_SETPRIORITY`/`SYS_GETPRIORITY` (165/166) + `toby_setprio`/`toby_getprio` libc, with a
  uid/root policy (renice only your own procs; only root raises above NORMAL). Verified by an
  opt-in self-test (`-DSCHEDPRIO_BOOT`): 3 HIGH + 3 LOW timed CPU-bound workers on 4 cores →
  **HIGH ~18x the CPU of LOW (ratio 1792/100) yet every LOW proc still ran (no starvation)**, 0
  exceptions/panics. Production boot byte-neutral (desktop 292 heartbeats == baseline, all-NORMAL
  procs schedule exactly as before). Closes the open priority-classes/timeslicing bullet of gap
  item #3. Scheduler/SMP ~18% → ~21%; overall stays ~26%.
- **2026-06-08** — **ext4 read-write support.** `src/ext4.c` was read-only (create/write/unlink/
  mkdir returned ROFS). Added real write support on a clean (no-journal-replay) ext4/ext2:
  in-place overwrite, file growth (block-bitmap allocation + **depth-0 extent-tree extension** --
  the ext4-specific part: extend the last extent when contiguous so sequential writes stay one
  extent, else add a leaf up to eh_max=4; legacy-pointer inodes reuse the direct+single-indirect
  path), and create/unlink/mkdir with directory-entry insert (slot-split or grow-by-block) and
  remove. Block/inode bitmap + group-descriptor + superblock free-count bookkeeping was adapted
  from the already-read-write ext2 driver (`src/ext2.c`, shares the same on-disk structs). Mount is
  now rw; a needs-recovery (dirty journal) image is still refused, and there is **no journaling**
  (writes aren't crash-atomic) -- the depth-1 extent index tree and >4 fragmented runs return NOSPC
  (documented limit, not silent corruption). Verified self-contained (no host mke2fs): a new
  in-kernel `ext4_format` (single block group, 4 KiB blocks, extents, no journal) + `ext4_self_test`
  (`-DEXT4_SELFTEST`) format a ramdev, mount it via the real driver, and PASS create + multi-block
  extent-grow write + verify, in-place overwrite, mkdir + nested file, unlink, and remount-
  persistence; default IDE boot + production build stay clean (0 exceptions/panics). (Root-caused a
  nondeterministic heap corruption to a 1 KiB stack buffer in the mount path overflowing the ~16 KiB
  boot stack; moved it to the heap.) Storage stays ~29% (the last storage sub-task; rounds out
  filesystem write coverage rather than adding a new subsystem).
- **2026-06-07** — **Async block I/O + command queuing (NVMe multi-command + AHCI NCQ).**
  Both block drivers were single-outstanding-command (one NVMe SQ entry / AHCI slot 0, polled).
  Added an async block-I/O core (`struct blk_io` + `blk_io_complete` callable from the completion
  IRQ + `blk_io_wait`) and converted both drivers to keep multiple commands in flight. NVMe uses a
  per-controller CID bitmap + in-flight table (transient PRP-list/bounce pages so concurrent
  commands don't share DMA scratch); AHCI uses NCQ (FPDMA QUEUED, per-tag command tables, PxSACT
  set-to-issue / SDB-FIS-clears-to-complete, coarse task-file-error recovery), gated on CAP.SNCQ +
  the drive's IDENTIFY NCQ bit with a clean fall-back to the legacy single-command path. The wait
  cooperatively **yields** between completion polls when it's safe (scheduler up, IRQs enabled, a
  current proc) so other procs run and submit their own I/O — that overlap is what fills the queue;
  when unsafe (early boot, or a spinlock held like the bcache flush paths) it busy-polls the
  hardware, so it is never worse than before and needed no scheduler surgery (a true
  blocking/wake-from-IRQ variant was prototyped but stalled the scheduler on first-switch
  bookkeeping, so the cooperative-yield fallback was taken). Each completion is routed back to its
  request by CID/tag, so out-of-order completion is correct. Verified in QEMU via opt-in
  self-tests: NVMe (`-DNVME4K_BOOT`, logical_block_size 4096 and 512) and AHCI (`-DAHCIQ_BOOT`,
  `-device ahci` + SATA disk) each reach **peak 8 commands in flight** in a batched submit, with a
  concurrent write-distinct-patterns / read-back / per-buffer verify confirming correct per-tag
  routing; sequential and 4K-translation paths still PASS; default IDE boot + production builds
  unaffected (0 exceptions/panics, desktop healthy). Storage stays ~29% (correctness/throughput
  groundwork, not new feature breadth).
- **2026-06-07** — **NVMe 4K-sector (4096-byte LBA) support.** `src/blk_nvme.c` previously
  skipped any namespace whose native LBA size wasn't 512; real SSDs increasingly format at
  4096. The rest of the kernel (block registry, bcache, partition layer, filesystems) is fixed
  at 512-byte logical sectors, so the driver now keeps that invariant and translates: a 4K
  namespace of N native sectors is registered as 8N logical 512-byte sectors, and the read/write
  path emits whole-device-sector runs straight through (the FS does 4 KiB-aligned I/O, so this
  is the common case) while a sub-device-sector fragment (e.g. a single 512-byte GPT/MBR sector)
  is handled by a read-modify-write through a per-controller bounce page. Formats carrying inline
  metadata (PI/DIF) or non-power-of-two native sizes are still skipped. 512-byte namespaces take
  the identity fast path (sec_ratio==1) unchanged. Verified in QEMU with
  `-device nvme,...,logical_block_size=4096` (and =512) via an opt-in non-destructive self-test
  (`-DNVME4K_BOOT`, `blk_nvme_selftest`): aligned multi-sector rw, sub-sector write + neighbour
  preservation, and sub-sector read all PASS on both geometries; boot clean, 0 exceptions/panics.
  Storage row updated (still ~29%; this is breadth/correctness, not a parity jump). Remaining in
  storage: AHCI/NVMe single-outstanding-command (queuing).
- **2026-06-07** — **TobyFS: double-indirect blocks + dynamic device-sized volumes.** Max file
  size raised from ~4 MiB to ~4 GiB by adding a double-indirect block pointer (carved from the
  inode's pad bytes, on-disk inode size unchanged, legacy disks still mount). The filesystem is
  no longer hardcoded to a 4 MiB volume: `tobyfs_format` now computes geometry (inode count,
  multi-block inode/data bitmaps, journal start) from the backing device size (cap 1 GiB), and
  mount/fsck read the geometry from the superblock and heap-allocate multi-block bitmaps. The
  bitmap cache flushes only the single block containing a mutated bit. Backward compatible: a
  legacy 1024-block image derives the exact old layout and mounts clean (verified: real `/data`
  mounts fine). Also fixed a **latent journal bug**: in-transaction `journal_write` didn't
  update the block cache, so a read-modify-write of a block already buffered in the same txn
  (e.g. create updating a child inode then the parent-dir inode in the same inode-table block)
  re-read stale data and clobbered the earlier change — silently reverting freshly-created
  inodes to type=FREE. Verified in QEMU via an extended `tobyfs_self_test` that formats a 16 MiB
  volume, writes/reads/verifies a 5 MiB file through the double-indirect path (PASS), and the
  corruption-detection check (PASS), with the desktop booting clean and no panic. Storage ~27% →
  ~29%.
- **2026-05-31** — **AP/argc>=1/SMAP SMEP fault: confirmed NOT QEMU-reproducible.** Built an
  opt-in repro (`-DMCARGV_BOOT` in kernel.c: 8 rounds × 4 `argc=4`/`envc=3` `/bin/mctest`
  workers spawned together, with `/bin/mctest` now dereferencing `argv[]`) plus a greppable
  SMEP fault dump in `isr.c default_exception` (on `vector==14 && !from_user && err&0x10`:
  cli's the CPU, prints cpu/pid/user_entry/user_rsp/kstack_top/saved_rsp + fault-rip deltas,
  bracketed `===SMEP-FAULT-BEGIN/END===`). Ran `qemu64,+smep,+smap`, the same with argv
  deref, and `Skylake-Client,+smep,+smap` under `-accel tcg,thread=multi` — all clean (32/32
  workers, 0 SMEP, 0 #PF, desktop healthy; per-proc cpu times prove genuine parallel AP
  execution). No KVM on the Windows host, so MTTCG is the strongest local concurrency model.
  Wrote `docs/smep-ap-capture.md` for capturing the fault frame on the EliteDesk over COM1
  (38400 8N1 null-modem). The original MCTEST benchmark used `argc=0`, which is why it never
  exercised this path. No parity-number change; this is diagnosis, not a fix.
- **2026-05-31** — **Real multi-core landed.** On top of the per-CPU foundation, APs now
  run user code in parallel: a syscall-path big-kernel-lock (`sched.c` `bkl_enter/exit`,
  released/reacquired around blocking in `sched_yield`, around pid 0's `gui_tick`/service
  work in `idle_loop`, and in execve's direct-enter path) serializes kernel entry so user
  code parallelizes while VFS/GUI/proc-table stay race-free; per-CPU idle procs
  (`proc_ap_idle`, `is_idle`) + work-stealing (`steal_one`, `queue_steal_locked`) +
  `do_switch` run+migrate procs on APs; APs get per-CPU SYSCALL MSRs (`syscall_init_ap`)
  and CD/NW cleared; AP-run is gated until boot finishes (`sched_enable_ap_run`). Measured
  ~2.6-2.7x on 4 CPU-bound workers; GUI desktop stable on default and `+smap` across many
  boots. Known issue: `argc>=1` first-run procs on an AP under SMAP SMEP-fault. Earlier
  prototype that deadlocked the desktop was fixed by the idle_loop BKL wrap + AP-run gate.
  Scheduler/SMP ~15% → ~18%; overall ~25% → ~26%.
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
