# Linux-completeness arc — handoff into Phase 3 (namespaces → containers)

**Written 2026-08-08, after slice 7. Updated 2026-08-08 after slices 8, 10, 9 and 11.**
Phases 1 and 2 are done and verified. This document is the entry point for
Phase 3.

**Slices 8, 9, 10, 11, 12-cut-1 and 13 are COMPLETE and verified — §10-§15.
All five namespaces AND seccomp are in.**

**SLICE 14 IS BLOCKED and is NOT the slice the plan describes — READ §16 FIRST.**
It needs a non-root chrome (Chromium refuses to sandbox as root), which needs a
native setuid this ABI does not have, plus the real-hardware validation the plan
mandates and this environment cannot produce.

---

## 0. The 30-second version

tobyOS's Linux personality now runs **a real Alpine Linux 3.19 userland** —
Alpine's own busybox and `apk-tools 2.14.0`, in a chroot, as **uid 65534**.
Slices 1–7 of `~/.claude/plans/sunny-crafting-charm.md` are complete.

Phase 3 is slices 8–16: namespaces, seccomp, cgroups, and finally a real OCI
container — with **Chromium dropping `--no-sandbox` (slice 14)** as the
integration proof.

The user's directive that governs everything here: **tobyOS is ONE environment
running BOTH Linux and Windows software.** Track C (Win32) is co-equal and
deliberate. Never break a personality boundary; never advise dropping Track C.

---

## 1. FIRST: how to know you haven't broken anything

```
bash logs/lxposix.sh           # POSIX gate only,  ~2 min
bash logs/lxposix.sh --full    # + full Linux/Win32 suite, ~5 min
```

Exit status **is** the result (0 green / 1 red), so it works as a pre-commit
check without reading the output. **Run it before you start and after every
change.** It asserts three independent things:

1. every sub-test passes,
2. the **ENOSYS census is empty** (this is what catches *coverage* regressions —
   proven by deliberately breaking `umask`: all 10 exit codes stayed green and
   only `enosys_gaps=1` caught it),
3. the guest stayed live and took zero faults.

Cross-personality gates are **not** in that script and must be run separately
after anything touching exec, paths, signals, or the scheduler:

```
touch src/kernel.c && make "CC=TMP='C:\t' TEMP='C:\t' clang" \
  "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
  EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTHREEWORLDS_BOOT -DXPIPE_BOOT -DX2_BOOT" iso
# expect: XPIPE 3/3, X2PIPE exit=7, [3W] three windows one desktop
```

Alpine milestone: `EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DLXALPINE_BOOT"`,
expect `[LXALPINE] VERDICT: PASS steps=6/6` (~90 s of guest time; the rootfs
extraction alone is ~80 s under TCG).

**Current baseline (after slices 8-11, all verified 2026-08-08):
10/10 POSIX gate, 23/23 full suite, `enosys_gaps=0`, **LXNS 8/8**, 0 faults,
XPIPE 3/3, X2PIPE exit=7, THREEWORLDS PASS, defboot `VALIDATE: GREEN 2/2`.**
Alpine (6/6) not re-run this session — it is a separate flavour.

The suite is 23 rather than slice 7's 22 because `-DLXNS_BOOT` now rides along
with `--full` (one extra verdict line — nothing that was passing changed).

All four namespace acceptance binaries report `0xff`:
`linux-ns` (8, UTS+IPC), `linux-pidns` (10), `linux-mntns` (9), `linux-userns`
(11) — plus four busybox witnesses (`unshare -u`, `-p`, `-m`).

**`logs/validate.sh` WAS BROKEN AND IS NOW FIXED.** It reported ALIVE for every
run no matter what, including boots that panicked in Limine before the kernel
started (four independent bugs, documented in the file). Any "defboot PASS"
recorded before 2026-08-08 meant nothing.

---

## 2. Build laws (each one cost a debugging session)

- **`struct proc` grew ⇒ `make clean`.** No header dependency tracking; stale
  objects keep old field offsets and corrupt memory silently. Phase 3 grows it
  repeatedly (slices 9–11).
- **`EXTRA_CFLAGS` changes do NOT trigger a recompile** ⇒ `touch src/kernel.c`.
- **Never hide the build behind `>/dev/null`.** A failed build leaves the
  PREVIOUS iso in place and you test stale bits — this happened, and produced a
  confidently wrong conclusion. `logs/lxposix.sh` refuses to run on a failed
  build for exactly this reason.
- **Bash heredocs EAT BACKSLASHES.** Writing C string literals through one
  turns `\t`/`\n` into literal tab/newline characters *inside* the literal.
  Use Write/Edit for anything containing escapes.
- Builds and QEMU in **separate shells**, with a Windows-native `TMP` carried
  ON the compiler variable. See [[tobyos-build-env]] memory.

---

## 3. The method trap that bit three times: harnesses that lie

Every one of these produced a **green verdict over a real failure**:

| What | Why it lied |
|---|---|
| `cmd \| sed 's/^/[TAG] /'` | pipeline exit status is **sed's**; "can't execute apk" counted as PASS |
| `cmd \|\| fallback` | the fallback ran **as root**, so a "non-root" milestone reported success at `uid=0` |
| `chroot /jail /bin/busybox echo hi` | the path exists on BOTH sides — it ran the **host** binary and looked like proof |

**Rules:** never let a test's success path run through a command that can
succeed on its own; prefer a proof that names each step over one that delegates
to a tool with fallbacks (see `programs/linux-alpinerun/`); and when asserting
"X's copy ran", compare something only X's copy could produce (version banners).

---

## 4. What Phase 3 inherits (seams already built for it)

These were built EARLY, on purpose, so Phase 3 doesn't have to redo work:

- **Mount-namespace seam (slice 5).** `vfs.c`'s table lives in a
  `struct mount_ns`; `proc->mnt_ns` is NULL == "initial namespace". **Slice 9
  only has to allocate a second `mount_ns` and point at it.** Two accessor
  macros keep ~29 call sites untouched. Namespacing is cheap because path
  resolution is longest-prefix over a small array — cloning is a struct copy.
- **Credentials are namespace-ready (slice 2).** real/effective/saved uid+gid,
  supplementary groups, and **Linux capability bits kept strictly separate**
  from tobyOS's `cap_profile` sandbox mask (`lcap_*` vs `caps`). The stored ids
  are HOST/initial-ns (Linux's `kuid_t`): **slice 11 must map at the REPORTING
  boundary** (getuid, stat, procfs status), NOT rewrite them.
- **Mount flags with teeth (slices 2+5).** `VFS_MNT_NOSUID` is enforced in
  `sys_execve`; `VFS_MNT_RDONLY` in all six path mutators. `mount(2)` already
  passes user flags through.
- **`chroot` uses its own `fs_root` field**, deliberately NOT the cap
  `sandbox_root` (a *check* vs a *translation* — folding them would turn an
  existing denial into a redirect). They compose.

---

## 5. Phase 3 plan (slices 8–16)

Ordered **easiest-mechanism-first** so the namespace plumbing is proven cheaply
before the expensive ones.

- **8 — UTS + IPC namespaces. ✅ DONE — see §10.** The mechanism is built and
  proven; slices 9–12 add a payload and a flag, not plumbing.
- **9 — Mount namespace. ✅ DONE — see §12.** (Briefly deferred in favour of 10,
  then implemented in full before slice 11.)
- **10 — PID namespace. ✅ DONE — see §11.**
- **11 — User namespace. ✅ DONE — see §13.** Unprivileged mount/chroot now work,
  scoped by namespace ownership.
- **12 — Network namespace. ✅ CUT 1 DONE — see §14** (it did NOT blow up: no
  loopback in this stack meant no refactor was needed; cut 2 = veth/bridge in 15).
  Original note kept for cut 2: **THE ONE THAT CAN BLOW UP.** ~93 static globals
  across `net.c`/`tcp.c`/`socket.c` must gather into a `struct net_ns`. **Do it
  in two cuts:** first `CLONE_NEWNET` yields an EMPTY ns (loopback only, no
  inter-ns routing) — which is exactly what a sandbox needs and defers
  veth/bridging entirely. Second cut (slice 15) adds veth+bridge. If cut 1
  proves worse than estimated, empty-ns-only still permanently unblocks 14.
- **13 — seccomp-bpf.** More tractable than it sounds: cBPF is a ~200-line
  interpreter and the hook point is the single clean dispatch site in
  `src/syscall.c` (`linux_syscall` vs `win32_syscall` vs native).
- **14 — MILESTONE: Chromium without `--no-sandbox`.** Remove
  `--no-sandbox --no-zygote` from `programs/chromewin/main.c`. Integration proof
  that user+pid+net ns and seccomp work together under the hardest real
  workload — and it closes a genuine security hole in the flagship demo.
- **15 — cgroup v2 core + `pids` + `cpu`. ✅ DONE — see §17.** NOTE: net-ns cut 2
  (veth/bridge) was bundled here by the plan and is NOT done — it is independent
  of cgroups and is the real `struct net_ns` refactor.
- **16 — cgroup `memory` + `io`, then run a real OCI container.**

Full plan with rationale: `~/.claude/plans/sunny-crafting-charm.md`.

---

## 6. Open items carried in (none block Phase 3)

- **`vfork` hands the child a PRIVATE STACK, which breaks vfork's contract.**
  *Found and characterised 2026-08-08 during slice 10.* `sys_fork_share()`
  allocates `vfork_stack_va` for the child, but vfork's whole point is that the
  child runs on the PARENT's stack until it execs. A child that reads any local
  set before the vfork therefore gets garbage. Measured with busybox
  `unshare -f`, which is `xvfork(); if (!pid) xexecvp(argv)`:

  ```text
  [fork] share pid=3 -> child pid=4 stack=0x7f0000800000
  *** EXCEPTION 14: Page Fault (in user mode) ***
    rip=0x0000000000000000  err=0x14      <- user-mode INSTRUCTION FETCH at NULL
  ```

  Attributed with a probe that used `-u` only (a namespace working since slice
  8, so no pid namespace was involved) and reproduced the fault identically —
  it is vfork, not namespaces. `LX_clone` already documents the same hazard for
  plain fork: *"glibc resumes on the parent's stack frame — a private RSP breaks
  it."*

  **This blocks `unshare -f` / `nsenter`-style tooling and therefore slice 16's
  container runtime**, which is the main reason to fix it. Not fixed here: it is
  pre-existing, out of slice-10 scope, and the share-until-exec path is
  load-bearing for the Chromium arc (see the CHROMIUM_BOOT fork caps), so it
  wants its own slice with the chromium gates run against it.

- **`ptrace` + `process_vm_readv/writev`** — scoped into slice 3, not built.
  `strace` wants them. Nothing downstream depends on them.
- **`pivot_root`** — deliberately `ENOSYS`, NOT aliased to chroot (it *moves*
  the root mount and detaches the old one; chroot only changes name resolution
  and leaves the old tree reachable through pre-existing fds). Wants slice 9
  underneath it. **Slice 16's container runtime will want it.**
- **`/proc/<pid>/environ`** — absent on purpose. The environment lives on the
  process's user stack, not the PCB; serving it needs the address recorded at
  exec plus a CR3 switch.
- **ramfs doesn't surface tar mtimes**, so initrd files report 1970. Cosmetic.
- **tobyfs partial-shrink `truncate`** leaves tail blocks allocated until
  unlink (wastes space, loses no data).
- **`MNT_NOSUID` is enforced but untested end-to-end** (needs a nosuid mount
  carrying a setuid binary).
- **Makefile stages the ~750 MB chrome headless payload UNCONDITIONALLY**
  (`Makefile` ~line 3106, an `elif` with no opt-in guard despite logging
  "OPT-IN"). Pre-existing; left alone to avoid disturbing the chromium arc.

## 7. Watch items (things Phase 3 could destabilise)

- **`vfs_open` is now on the symlink-resolution path for EVERY open.**
- **`sys_execve` now resolves through `resolve_user_path`** (cwd + chroot).
  It was the one path syscall that didn't, which is why chroot silently ran
  host binaries.
- **timerfd readiness depends on `poll_tick`'s ~20 ms sweep** — a timerfd
  becomes ready by the passage of time, so there is no event to hang
  `poll_event_notify()` on. Deleting that sweep as "redundant" silently stops
  timerfd waits from firing.
- **`signal_send` touches the run queue**, so alarm/timer work must NOT run in
  the raw PIT IRQ — it deadlocked the machine once. The handler's
  `(cs & 3) == 3` gate is a **safety condition**, not a style choice.

---

## 8. Tree state

**Everything is uncommitted**, on branch `feat/audio-output`. That includes the
user's **pre-existing audio work** (`audio_engine.c`, `audio_hda.c`,
`snd_pcm.c`, `chromewin/main.c`, `linux-sndtest`) which is NOT part of this arc
— do not revert or "clean up" those files. New untracked dirs from this arc:
`programs/linux-{cred,suid,timers,mount,alpinerun,ns}/`, `programs/alpine/`.

Nothing here has been committed or pushed. Ask the user before doing either.

## 9. Memory to read first

`linux-namespaces-slice8` (the mechanism slices 9–12 build on),
`linux-user-namespace-slice11`, `linux-mount-namespace-slice9`,
`linux-pid-namespace-slice10`,
`linux-alpine-slice7`, `linux-mount-chroot-slice5`, `linux-credentials-slice2`,
`linux-posix-gate-slice4`, `linux-timers-signals-slice3`,
`linux-proc-dev-times-slice6`, `cross-personality-pipeline-x1` (the thesis),
`tobyos-build-env`.

---

## 10. Slice 8 — the namespace mechanism (DONE 2026-08-08)

### How to run it

```text
bash logs/lxns.sh              # namespace gate only, fast focused loop
bash logs/lxns.sh --clean      # when struct proc changed (slices 9-11 will)
bash logs/lxposix.sh --full    # LXNS now rides along here too
```

`-DLXNS_BOOT` was added to `lxposix.sh --full`, so slices 9–16 cannot regress
namespaces while every other verdict stays green. **That is why `--full` now
reports 23 PASS rather than 22** — one extra verdict line, not a change to
anything that was already passing.

### What slices 9–12 inherit

New files: `include/tobyos/nsproxy.h`, `src/nsproxy.c`. **Read the header
first** — it states the two design decisions and why they are the right shape
for the namespaces that are still to come.

To add a namespace type, the work is:

1. define the payload struct + refcount **in the file that owns the payload**
   (`struct uts_ns` is in nsproxy.c, `struct ipc_ns` in shm.c next to the SysV
   tables, `struct mount_ns` already in vfs.c since slice 5);
2. add a `void *` to `struct proc` — **`struct proc` grows ⇒ `make clean`**;
3. add the kind to `ns_ptr_of` / `ns_get` / `ns_put` / `ns_inum_of` in
   nsproxy.c (four small switch arms);
4. add the flag to `CLONE_NEW_SUPPORTED` in nsproxy.h — **last**, and only once
   the namespace is real;
5. handle it in `ns_unshare` and `ns_setns`.

Everything else — clone/clone3 flag plumbing, `/proc/PID/ns` listing, readlink,
`FILE_KIND_NSFD`, fork inheritance, teardown — is type-generic already.

### Four things that are load-bearing, not incidental

- **NULL means "the initial namespace"**, for every type. Zero allocation at
  boot, and adding a type cannot change behaviour on a system where nobody
  called `unshare(2)`.
- **`CLONE_NEW_SUPPORTED` gates everything, and unsupported flags return
  `-EINVAL`** — which is exactly what Linux returns when the matching
  `CONFIG_*_NS` is off. Do not add a flag to that mask before the namespace
  works: accept-and-ignore is how a caller comes to believe it is isolated when
  it is not. The acceptance test asserts these refusals.
- **`clone(2)`'s namespace flags are STAGED on the parent
  (`proc->clone_ns_flags`) and applied INSIDE `sys_fork`**, before the child is
  enqueued. The obvious alternative — fork, then fix the child up by pid — is a
  race: `sys_fork` sets the child `PROC_READY` and `sched_enqueue`s it before
  returning, so on `-smp 4` another core runs the child first and it observes
  the parent's namespaces. That is the exact isolation failure the feature
  exists to prevent.
- **A new UTS ns COPIES the hostname; a new IPC ns starts EMPTY.** Both match
  Linux and the asymmetry is deliberate: a container with no hostname is broken,
  a container that can see the host's SysV segments is not isolated.

### Scope stated honestly

- **POSIX shm (`shm_open`, `g_shm` in shm.c) is deliberately NOT namespaced.**
  On Linux it is a tmpfs at `/dev/shm`, so its isolation belongs to the MOUNT
  namespace (slice 9), not the IPC namespace. Namespacing it here would have
  produced isolation attributed to the wrong namespace, which slice 9 would
  then contradict.
- **The mount namespace is still not refcounted.** `struct mount_ns` has a
  `refs` field and no get/put, because nothing has ever allocated a second one.
  `ns_get`/`ns_put` have a comment where slice 9's arms go.
- **`CLONE_NEW*` combined with `CLONE_THREAD` is refused (`EINVAL`)** rather
  than half-implemented. Verified this does not affect real pthreads: glibc's
  `pthread_create` uses `0x3d0f00` and its `fork` uses `0x01000011`, neither of
  which intersects `CLONE_NEW_ANY` (`0x7E020000`).
- **Namespaces are root-only** (`uid != 0` → `EPERM`), matching `mount(2)`'s
  existing rule. Slice 11 (user namespaces) is where unprivileged use arrives.
- **`ipc_ns_destroy` frees segment pages with `pmm_free_page` regardless of
  `page_ref`**, exactly as the pre-existing `shmctl(IPC_RMID)` path does. Not a
  new bug class, but if slice 16 does per-cgroup memory accounting, both want
  revisiting together.

### The method note worth carrying forward

The acceptance test (`programs/linux-ns/main.c`) is built around one rule, and
the header comment says so at length: **"the parent's hostname did not change"
is not evidence of isolation** — it is equally satisfied by a `sethostname(2)`
that does nothing, which is a failure mode this tree has shipped before
(`setitimer`, `utimensat`, `VFS_MNT_RDONLY`). So every claim is asserted from
both sides *plus a control*:

| Half | What it asserts |
| --- | --- |
| positive | the process that unshared sees its own change take effect |
| negative | the process that did not unshare does not see that change |
| control | the same operation WITHOUT unshare *does* propagate |

The control is what makes the result attributable to `unshare` rather than to
`fork`. Namespace identity is cross-checked independently through
`/proc/PID/ns/<kind>` inode numbers — a second witness that does not go through
the payload at all — and the whole thing is then re-proven through **real
busybox** (`unshare -u`, `hostname`, `readlink`), because a test that only
exercises its author's idea of the ABI can agree with a kernel that has the ABI
wrong.

Two smaller things that came out of the same discipline: the test **restores the
hostname it changed** (bit1/bit3 really do modify the initial namespace, so
leaving it altered would silently poison whatever the boot harness runs next),
and it calls `setvbuf(stdout, NULL, _IONBF, 0)` because its diagnostics come
from forked children leaving via `_exit()` — glibc's full buffering was
discarding exactly the output that says *which* sub-check failed.

---

## 11. Slice 10 — PID namespaces (DONE 2026-08-08)

New: `src/pid_ns.c`, `programs/linux-pidns/`. Extended: `nsproxy.{h,c}`,
`proc.h` (two more fields ⇒ **`make clean`**), `fork.c`, `proc.c`, `signal.c`,
`procfs.c`, `syscall.c`.

### The constraint that dictated the whole design

**In tobyOS a process's pid IS its index into `g_proc[PROC_MAX]`,** and
`proc_lookup()` is a direct array index. A pid cannot be reassigned, so the
Linux approach (a `struct pid` carrying a different number per namespace) was
not available.

So pids took the shape slice 2 chose for uids: **the stored `p->pid` is always
the host / initial-namespace number (the "kpid"); namespace-local numbers
("vpids") are a TRANSLATION at the syscall boundary.** Nothing inside the kernel
ever handles a vpid — not the ready queues, not the wait queues, not
`proc_lookup`. Rewriting `p->pid` would have meant auditing every internal user
of it, which is precisely the trap slice 2 recorded for credentials.

A process gets a vpid in its own namespace *and every ancestor*, which makes the
map itself the visibility test: `pid_vnr_in(ns, kpid) == 0` means "not visible
from `ns`", with no separate ancestry walk.

### Translation boundaries (the whole surface)

| Direction | Sites |
| --- | --- |
| out (report a vpid) | `getpid`, `getppid`, `gettid`, `wait4`/`waitpid` return, `siginfo.si_pid`, `clone`'s ptid/ctid writes, `/proc` listing, `/proc/<pid>/{status,stat}` pid+ppid, `subst_self` |
| in (accept a vpid) | `kill`/`tkill`/`tgkill`, `wait4`/`waitpid` argument, every numeric `/proc` path component |

Miss one and the failure is quiet: two numbers that both look plausible.

### Five things that are load-bearing

- **`pid_ns` vs `pid_ns_for_children` are NOT redundant.**
  `unshare(CLONE_NEWPID)` is the one namespace op that does **not** move the
  caller — its number in the new namespace would have to appear from nowhere
  while every existing holder of the old one carried on. So unshare and
  `setns(CLONE_NEWPID)` set only `pid_ns_for_children`, and the next fork's
  child becomes init there. Collapsing the two is the easiest way to get this
  quietly wrong; busybox's `unshare -f` exists *because* of this rule.
- **`pid_ns_place_child()` runs on EVERY fork, not only when clone flags were
  staged** — a plain `fork(2)` still has to land the child in
  `parent->pid_ns_for_children`.
- **`wait4` must capture the child's vpid BEFORE `proc_wait()`**, which reaps it
  and releases the mapping. Translating afterwards reads a dead mapping. (Caught
  while writing it, not by a test.)
- **`nsproxy_release()` calls `pid_ns_forget()` BEFORE dropping the namespace
  reference** — the last put frees the `pid_ns`, and forgetting afterwards would
  read freed memory.
- **The initial namespace is the IDENTITY mapping**, short-circuited at level 0,
  so every translation is `return kpid` on a system that never used
  CLONE_NEWPID. `proc->pid_ns == NULL` keeps slice 8's convention.

### Two regressions I introduced and caught before shipping

1. **`/proc/0` would have silently disappeared.** `pid_vnr()` reports 0 as "not
   visible", so routing the initial namespace through it dropped pid 0 from both
   the `/proc` listing and `procfs_kpid()`. This kernel has always exposed
   `/proc/0` (Linux has no pid 0); removing it is an unrelated behaviour change
   a namespace slice has no business making. Both sites now take the identity
   path verbatim when the reader is in the initial namespace.
2. **`subst_self()` wrote the kpid into the path**, which the new numeric
   translation would then have decoded a second time and resolved to the wrong
   process. It must write the vpid so the two agree.

### Deliberate scope, stated

- **`/proc` filters by the READER's namespace, not by whoever mounted it.** On
  Linux a container must remount `/proc` (`unshare --mount-proc`) to see its own
  pids; ours is correct per-reader without a mount namespace. A divergence, and
  a convenient one — it is why `ps` isolation works with slice 9 skipped — but
  it *is* a divergence.
- **Orphan reparenting to the namespace init happens only in NON-initial
  namespaces.** The initial namespace has never reparented orphans in this
  kernel; changing that as a side effect would be an unrequested behaviour
  change. (That absence is itself an open item.)
- **`getpid()` still returns the TID for a thread**, where Linux returns the
  TGID. Pre-existing; left alone because changing it alters every threaded
  program's view of itself. Open item.
- **A ns init's death SIGKILLs the namespace** (including nested ones, since the
  visibility map already encodes that relation). Unhandled-fatal-signal immunity
  for a ns init is *not* implemented.
- `CLONE_NEW*` with `CLONE_THREAD` is refused (`EINVAL`), as in slice 8.

### Method note

The sharpest check in `linux-pidns` is bit4: **one process reports two different
pids to two different readers at the same instant** — `Pid: 1` to itself,
`Pid: 3` to its parent — synchronised with a two-pipe handshake so there are no
sleeps and it cannot go flaky under TCG. No constant-returning stub can produce
that, which is the property "the child says its pid is 1" lacks on its own.

Slice 8's gate then earned its keep immediately: `linux-ns` dropped to `0x7f`
because it asserted `unshare(CLONE_NEWPID) == EINVAL`, which slice 10 made
false. That is the gate working — it had encoded "pid namespaces unsupported" as
a *requirement*. Fixed by moving the flag out of the refusal list **and adding a
positive assertion**, not by weakening the check. Slices 11/12 will hit the same
thing for `NEWUSER`/`NEWNET`; do the same.

---

## 12. Slice 9 — Mount namespaces (DONE 2026-08-08)

Implemented after slice 10 (it was briefly deferred), before slice 11.
New: `programs/linux-mntns/`. Extended: `vfs.{h,c}`, `nsproxy.{h,c}`,
`syscall.c`, `kernel.c`. **`struct proc` did NOT grow** — `mnt_ns` already
existed from slice 5 — so no `make clean` was needed for this slice.

### The seam paid off exactly as slice 5 predicted

Slice 5's claim was that slice 9 "only has to allocate a second `mount_ns` and
point at it". That held. Two things came free from it:

- **`/proc/mounts` is per-namespace with no work at all** — `vfs_iter_mounts`
  goes through the `g_mounts` accessor macro, which resolves to the *reader's*
  namespace.
- **Cloning a namespace is a struct copy**, because resolution is
  longest-prefix over a small array rather than a dentry/vfsmount walk.

`VFS_MAX_MOUNTS` went 8 → 16: the table is now per-namespace, and a container
that pivot_roots then mounts its own /proc, /sys, /dev, /dev/pts and /tmp needs
five slots on top of the three the boot namespace uses. At 8 that wall would
have been hit by slice 16 rather than by anything diagnosable.

### Propagation is real, not stored-and-ignored

This is the only part of the slice with genuine semantic content, and the part
that accepting `MS_SHARED` and doing nothing would have imitated perfectly. So:

- `struct vfs_mount` carries `prop` / `peer_id` / `master_id`; peers are mounts
  (in *any* namespace) sharing a `peer_id`.
- A **namespace registry** (`g_mnt_ns_list`) exists precisely so a mount call
  can reach peers in other namespaces. Linux walks its vfsmount tree; a list of
  namespaces is the equivalent for a flat per-ns table.
- `mount_ns_create()` **preserves `peer_id` for SHARED mounts** across the
  clone, which is what makes propagation survive an unshare. Clearing it would
  have turned every propagation test into a no-op that still looked correctly
  isolated — the most dangerous available bug in this slice.
- `MS_UNBINDABLE` is **enforced** (bind refused), not merely recorded.
- `mount --make-*` arrives as a `mount(2)` carrying only a propagation flag and
  no fstype, so it is handled *before* the fstype validation.

Verified in both directions plus a control: shared propagates child→parent,
private does not (so the first result is propagation and not leakage), and
**MS_SLAVE receives its master's mounts while sending none back** — both slave
directions, because testing one would pass for a "slave" that is really private
(no receive) or really shared (no containment).

### pivot_root is a real implementation, not a chroot alias

Slice 5 left it `ENOSYS` rather than aliasing it, correctly. In a flat
longest-prefix table, "move the root mount" is literally a rewrite of mount
points, so the old root ends up **somewhere else** rather than merely hidden —
the property chroot cannot provide. The test asserts all three halves: new root
live, old root reachable at `put_old`, and **no longer at `/`**.

**Requires a non-initial mount namespace.** Linux allows it in the initial one
(initramfs `switch_root` depends on that), but here it would rewrite the running
system's mount table with no way back, and every real caller is in its own
namespace anyway.

### Two bugs, one mine and one pre-existing

1. **`put_old` is relative to the PRE-pivot tree.** I first stored it verbatim,
   which parks the old root at `/newroot/oldroot` — a path that stops resolving
   the instant `/newroot` becomes `/`. The old root would have been silently
   unreachable, looking like a working pivot_root right up until someone tried
   to unmount it. Caught by the test (bit6 `rc=5`), which is why the test checks
   the old root's *new* location rather than only that the new root works.
2. **Pre-existing: `MS_BIND` put two table entries on one `(ops,data)`**, so
   unmounting either end called the driver's `umount` hook and left the other
   pointing at torn-down state. Propagation would have multiplied this, so
   `vfs_unmount` now calls the hook only when no mount in *any* namespace still
   references that `(ops,data)`.

### Scope stated

- `CLONE_NEWNS` **moves the caller** (contrast `CLONE_NEWPID`, which cannot).
  Not an inconsistency: a mount namespace is only ever consulted to resolve the
  process's own paths, whereas its pid is a name others already hold.
- Propagation is evaluated against the **parent mount** of the new mount point.
  There is no per-mount subtree redirection in this VFS, so `MS_MOVE` is still
  absent and `MS_BIND` remains "honest-or-refuse" as slice 5 left it.
- No `mount --bind` of a *subdirectory* (only of an existing mount point) — a
  slice-5 limitation this slice did not change.

---

## 13. Slice 11 — User namespaces (2026-08-08)

New: `src/user_ns.c`, `programs/linux-userns/`. Extended: `nsproxy.{h,c}`,
`proc.h` (**struct proc grew ⇒ `make clean`**), `vfs.c`, `procfs.c`,
`syscall.c`.

### Slice 2's advance decision paid off, exactly as slice 5's seam did for 9

Slice 2 recorded: *"The stored ids are HOST/initial-namespace (kuid_t). A user
namespace must NOT rewrite them; it interposes a uid_map at the REPORTING
boundary."* That held. `vfs_perm_check`, `cap_check_path`, oom scoring,
`SO_PEERCRED` and ~25 other consumers keep comparing host ids and were never
touched. Two slices in a row where a seam built early turned a refactor into an
afternoon — worth weighing next time the choice is "flat now, namespace later".

Translation surface — **report (k→v)**: `getuid`/`getgid`,
`getresuid`/`getresgid`, **all three** stat paths (native `abi_stat`, `lx_stat`,
`statx`), `/proc/*/status` Uid:/Gid:/Groups:. **accept (v→k)**: `setuid`/`setgid`,
`chown` family. An unmapped id reports as **overflow 65534** outbound and is
**EINVAL** inbound — never silently substituted.

### TWO SECURITY BUGS, both in code I had just written

1. **The uid_map privilege check authorised itself.** `unshare(CLONE_NEWUSER)`
   sets the creator's capabilities to FULL inside the new namespace — that is
   the feature. So gating the *arbitrary-map* write on
   `userns_capable(CAP_SETUID)` is **always true for the creator**, and any
   unprivileged process could have mapped any host uid and become genuinely
   root. Linux checks CAP_SETUID **in the PARENT namespace**; that answer stops
   existing the moment the caps are widened, so it is **frozen at creation**
   (`ns->creator_was_root`).

   *Generalise this:* when an operation grants privilege, a privilege check
   written after the grant may be checking the privilege it just handed out.

2. **A bare capability check would make user namespaces a root exploit** —
   `unshare(CLONE_NEWUSER)` then mount over the real `/`. So every privileged
   namespace operation also requires the caller's user namespace to **OWN** the
   namespace being operated on. `struct mount_ns` gained `owner_user_ns`;
   `mount`/`umount`/`pivot_root` check `userns_owns_mnt_ns()`, and
   `setns(CLONE_NEWNS)` carries the same guard or it would be the way around it.

### Order is the feature

`CLONE_NEWUSER` is created and INSTALLED FIRST in both `unshare` and `clone`, so
the capabilities it grants authorise the other namespaces requested in the same
call. That is what makes `unshare -Urm` work for a non-root user; the other order
leaves the combination useless to anyone but root. `CLONE_NEWUSER` itself needs
no privilege; everything else needs CAP_SYS_ADMIN in the caller's user ns.

### procfs had NO write path before this slice

`procfs_ops.write` was `0`. User namespaces are configured by WRITING
`/proc/PID/{uid_map,gid_map,setgroups}` — the file *is* the interface, there is
no syscall — so this slice added the first one, keyed on a `wkind` recorded at
open (`procfs_write` receives only the handle, not the path). Every other file
still returns `VFS_ERR_ROFS`.

Rules enforced: **write-once** (else a process could widen its own map after the
fact); an unprivileged writer may map exactly ONE id and only one it already
owns; `gid_map` additionally requires `setgroups` to have been denied first.

### The stale-refusal pattern bit a THIRD time — and this time it had side effects

`linux-ns` asserted `unshare(CLONE_NEWUSER) == EINVAL`. Unlike the NEWPID and
NEWNS instances, the call now **succeeded**, so besides reporting the wrong
verdict it left the test process inside a user namespace, which then broke its own
hostname restore later in the same run.

**A refusal assertion that runs in the PARENT becomes dangerous the moment the
feature lands.** The positive assertions all run in children for this reason.
Slice 12 (`NEWNET`) is the last flag that will do this — expect it, and put the
check in a child.

### A test flaw worth copying the fix from

bit7 first failed with the child writing `0 <TESTUID> 1` to **gid_map** — but the
child calls only `setuid()`, so its gid was still 0. The pre-deny write therefore
failed because the id was not the writer's, **not because setgroups was still
allowed**: that half would have passed with no setgroups rule implemented at all.
Fixed by using the real `getgid()` and asserting `errno == EPERM` specifically.
Same family as the arc's other harness lies — *a check that can fail for an
unrelated reason is as useless as one that can succeed on its own.*

### Known limitation, stated

`sethostname`'s ownership check is a **proxy**, not the exact Linux rule:
`struct uts_ns` has no owner field, so a caller in a non-initial user namespace
must also hold its own UTS namespace. It errs safe — the only UTS namespace such
a caller could otherwise reach is the shared one — and says so in the code. Give
`uts_ns` an owner field if this ever needs to be exact.

### Two more findings from getting bit7 green

**`EPERM` could not survive the VFS.** `/proc/PID/uid_map` refusals arrived in
userspace as `EACCES`, because the VFS error vocabulary had no code that maps to
`EPERM` — `VFS_ERR_PERM` becomes `EACCES` and nothing else fit. Linux reports
`EPERM` for every map refusal and container runtimes test for it. Added
`VFS_ERR_NOTPERM` (-14) → `-ABI_EPERM`.

This is the slice-6 bug in the other direction, and the general form is worth
keeping: **an error vocabulary that cannot express the right answer will silently
substitute a plausible one.** It was only visible because an earlier failure had
prompted tightening the test from "the write failed" to "the write failed *with
EPERM*" — the loose version would have accepted the wrong errno indefinitely.

**Inside a fresh user namespace, a process cannot learn its own host ids.**
`getgid()` after `unshare(CLONE_NEWUSER)` returns the overflow id 65534, because
no map is written yet — that is bit2 working, not a bug. Host ids must be
captured BEFORE unsharing, which is what real container runtimes do. Two
iterations of the gid_map test went into noticing this; the kernel was right both
times and the test was wrong both times.

---

## 14. Slice 12 cut 1 — Network namespaces (2026-08-08)

New: `src/net_ns.c`, `programs/linux-netns/`. Extended: `nsproxy.{h,c}`,
`proc.h` (**struct proc grew ⇒ `make clean`**), `socket.h`/`socket.c`,
`syscall.c`.

### The plan's estimate was wrong in a way that mattered — MEASURE FIRST

This was billed as "the one that can blow up": ~93 static globals to gather into
a `struct net_ns`. Two measurements taken *before* writing any code changed the
whole shape of the work:

1. **~38 static variables**, not ~93.
2. **THIS STACK HAS NO LOOPBACK.** Zero references to `127.0.0.1`,
   `0x7f000001` or `IN_LOOPBACK` in socket.c / tcp.c / ip.c / net.c.

(2) is the one that mattered. Cut 1 was specified as "an EMPTY namespace
(loopback only)"; with no loopback to replicate, an empty namespace just means
**no network** — exactly the sandbox property, reached without moving any of that
state. The scariest slice in Phase 3 needed no refactor at all, and only a grep
for loopback revealed it.

### Cut 1 isolates by DENIAL, not replication — report it that way

The ARP table, routes, interface list and TCP connection table all remain the
**initial** namespace's. A new namespace is empty because it has **no
interfaces**, so there is nothing to replicate, and every network operation from
inside is refused at the socket boundary.

This is honest — an interface-less namespace genuinely cannot reach anything —
and it permanently unblocks slice 14. It is **not** a network namespace in the
full sense. **Cut 2 (veth + bridge, slice 15) is still the refactor the plan
described** and will need those globals gathered.

### Four enforcement points, chosen so there is no hole

| Point | Answer inside an empty namespace |
|---|---|
| `connect()` | `ENETUNREACH` |
| `sendto()` | `ENETUNREACH` — **gated separately**: it carries its own destination and never goes through `connect`, so gating connect alone leaves a hole a whole DNS client fits through |
| `bind()` specific addr | `EADDRNOTAVAIL`; `INADDR_ANY` still binds (local operation, nothing arrives) |
| `sock_deliver()` inbound | dropped — else isolation is ONE-DIRECTIONAL and a sandbox can still RECEIVE |

### Two design decisions worth keeping

- **Enforcement is a property of the SOCKET, not the process.** `sock->net_ns` is
  latched in `sock_alloc` (right beside the existing SO_PEERCRED creds — a
  ready-made precedent). So `unshare(CLONE_NEWNET)` never retroactively unplugs
  an already-open socket, which is Linux's behaviour and what lets a sandboxed
  child keep an **inherited socketpair**. Test bit5 asserts both halves.
- **AF_UNIX is deliberately NOT scoped.** Linux scopes only ABSTRACT unix names
  by network namespace, and this kernel has none (verified: zero occurrences in
  unix_socket.c). Local IPC must survive inside a sandbox — it is exactly how a
  sandboxed renderer reaches its browser. Test bit4 asserts it still works; an
  "isolation" that severed AF_UNIX would be useless for slice 14. NETLINK is
  likewise left alone (chrome PCHECKs bind success; nothing unsolicited is
  delivered, so there is nothing to leak).

### Method: the assertion that carries this test is an ERRNO

"`connect()` failed" is worthless evidence here — connect to an address with
nothing listening also fails, and this VM's SLIRP has almost nothing listening. A
test accepting any failure would pass on a kernel with no namespace support.

So every check asserts **`ENETUNREACH` specifically**, and the control asserts the
outside world returns something *else*. Measured: `ENETUNREACH` inside,
`errno=111` (ECONNREFUSED) outside. That pairing separates "there is no network
here" from "the network is fine and the peer is absent", and needs **no reachable
peer**, so it stays robust headless.

### THE STALE-REFUSAL PATTERN, FOURTH OCCURRENCE — predicted and still shipped

§13 said verbatim: *"Slice 12 (`NEWNET`) is the last flag that will do this —
expect it."* Slice 12 then landed and **both** `linux-ns` and `linux-userns`
failed on `unshare(CLONE_NEWNET) == EINVAL`. Writing the prediction down is not
the same as acting on it.

**Standing rule:** when you add a flag to `CLONE_NEW_SUPPORTED`, grep every test
for that flag's name IN THE SAME EDIT. Both refusal lists now assert only
`CLONE_NEWCGROUP` — the one namespace still unimplemented — so the same grep
applies when cgroup namespaces land in slice 15/16.

### Slice 14 is unblocked

`linux-netns` bit7 demonstrates the sandbox recipe end to end: **uid 1000 runs
`unshare(CLONE_NEWUSER|CLONE_NEWNET)`, gets no network, and keeps working local
IPC.** Slice 14 now has user + pid + net + mount namespaces beneath it and waits
only on slice 13 (seccomp).

### Verified

`LXNS` GREEN (`linux-netns` 8/8 first run), `lxposix --full` 10/10 + 23/23
`enosys_gaps=0`, XPIPE 3/3 + X2PIPE exit=7 + THREEWORLDS PASS, defboot
`VALIDATE: GREEN 2/2`, 0 faults throughout — and **DHCP still completes in the
initial namespace** (`[dhcp] OFFER yiaddr=10.0.2.15`), which is the
no-regression check that matters most for this slice.

---

## 15. Slice 13 — seccomp-bpf (DONE 2026-08-08)

New: `include/tobyos/seccomp.h`, `src/seccomp.c`, `programs/linux-seccomp/`.
**`struct proc` grew ⇒ `make clean`.** Verified: `linux-seccomp` 8/8,
`[LXNS] PASS 10/10`, lxposix 10/10 + 23/23, XPIPE/X2/3W, defboot GREEN,
Alpine 6/6, 0 faults.

### Why it really is ~200 lines

seccomp uses **classic** BPF, not eBPF: fixed 8-byte instructions, two registers,
16 scratch words, **no backward jumps and no loops**. So filters terminate by
construction, the verifier is one forward pass (the CFG is a DAG by definition),
and the interpreter can touch nothing but its own registers and the read-only
64-byte `seccomp_data`. That is the whole security argument for running
attacker-supplied programs in ring 0.

### THE BUG THE TEST CAUGHT — the action constants need SIGNED comparison

Chain rule: every filter runs, most restrictive wins. The constants are ordered so
"least permissive sorts lowest" — but **only as int32**:

```text
KILL_PROCESS 0x80000000  KILL_THREAD 0x00000000  TRAP 0x00030000
ERRNO 0x00050000  USER_NOTIF 0x7fc00000  TRACE 0x7ff00000
LOG 0x7ffc0000  ALLOW 0x7fff0000
```

As **unsigned**, `KILL_PROCESS` (2147483648) is LARGER than `ALLOW` (2147418112),
so an unsigned `min()` silently selects ALLOW and **`SECCOMP_RET_KILL` can never
win**. I wrote it unsigned; bit6 caught it (the filtered child ran to completion
and exited 50 instead of dying). **A sandbox whose KILL degrades to ALLOW is the
worst available fail-open**, and every other test still passed.

### Do not let TRACE/USER_NOTIF fail open

Both need a supervisor this kernel lacks, so both return `-ENOSYS` (exactly what
Linux does when none is attached) and `SECCOMP_GET_ACTION_AVAIL` reports them
**unavailable**. Claiming them and treating them as ALLOW would be a filter that
fails open.

### `PR_SET_NO_NEW_PRIVS` was an accept-and-ignore stub

Returned 0, recorded nothing. seccomp's privilege rule depends on it, so a stub
meant either refusing every unprivileged filter or accepting them with the
promise unrecorded. Now real and irreversible; bit1 asserts it reads back.

### Deviation

The plan's table names a separate `LXSECCOMP_BOOT`; seccomp rides in `LXNS_BOOT`
(one fewer build cycle per regression). Split it if per-slice flavours matter.

---

## 16. Slice 14 — BLOCKED, and it is not the slice the plan describes

**READ THIS BEFORE ATTEMPTING SLICE 14.** The plan says "Remove
`--no-sandbox --no-zygote` from `programs/chromewin/main.c` and have it still
run." That is not sufficient, and the reason is written in the code itself:

```c
/* MANDATORY: we run as root; chrome exit(1)s at ~4s with
 * "Running as root without --no-sandbox is not supported"
 * otherwise (zygote_host_impl_linux.cc:101).  This exact
 * omission cost a full 440s run that looked like a hang. */
```

**The flags exist because chrome runs as ROOT, and Chromium categorically refuses
to enable its sandbox as root.** That is a Chromium policy check, not a kernel
capability gap. Removing the flags while running as root yields `exit(1)` at ~4 s
— a cost a previous session already paid 440 seconds to learn.

### What slice 14 actually requires

1. **A way for a native process to drop privileges.** `chromewin` is a NATIVE
   tobyOS app and **the native ABI has no setuid at all** — the setuid family
   exists only in the Linux personality (`LX_setuid`). Verified by grep: there is
   no `ABI_SYS_SETUID`. So either add one (slice 2's credential machinery is
   already there, so it is small) or drop privileges kernel-side in the execve
   path.
2. `chromewin` drops to a non-root uid before its `sc3(ABI_SYS_EXECVE, ...)`.
3. A profile directory writable by that uid. Slice 120 chmods `/data` 0777 at
   mount, which probably covers it — **unverified for a non-root chrome**, and
   slice 120 is precisely the bug that ate a real-HW session.
4. *Then* remove `--no-sandbox --no-zygote`.

### The kernel side is READY

Chrome's unprivileged sandbox needs user + pid + net namespaces and seccomp, and
all four are in and verified. `linux-netns` bit7 already demonstrates the exact
recipe: **uid 1000 runs `unshare(CLONE_NEWUSER|CLONE_NEWNET)`, gets no network,
and keeps working local IPC.** `linux-seccomp` bit5 proves a filter survives fork
AND execve. What is missing is the LAUNCHER, not the kernel.

### And the plan's own requirement cannot be met from here

> Real-hardware validation on the EliteDesk is **required** for slice 14, not
> optional — non-root and sandboxing are exactly the areas QEMU-as-root cannot
> exercise.

Every QEMU run here auto-logs-in as root (a standing blindness recorded in
memory), so the one configuration that would validate slice 14 is the one this
environment cannot produce. **Slice 14 needs a decision from the user**: run it on
the EliteDesk, explicitly waive the requirement, or accept a `CW_SANDBOX`-gated
version that leaves the flagship default untouched until validated.

Recommendation: gate it. Flipping `--no-sandbox` off unconditionally on a change
nobody can validate risks the taskbar-launched browser (slice 119), and turning
the default on afterwards is a one-line change.

---

## 17. Slice 15 — cgroup v2: core + pids + cpu (2026-08-09)

New: `include/tobyos/cgroup.h`, `src/cgroup.c`, `programs/linux-cgroup/`.
**`struct proc` grew (`int cgroup`) ⇒ `make clean`.** Verified: `linux-cgroup`
8/8, `[LXNS] PASS 11/11`, 0 faults.

**veth + bridge (network-namespace cut 2) is NOT in this slice.** The plan bundles
it here, but it is independent of cgroups and is the actual `struct net_ns`
refactor that slice 12 cut 1 avoided. Still outstanding, along with slice 16.

### Shape

v2 (unified), not v1: ONE hierarchy, one membership per process. The interface is
a filesystem at `/sys/fs/cgroup` — directories ARE cgroups, control files are
read/written. There is no cgroup syscall in Linux either; the filesystem is the
ABI. It nests under `/sys` natively because the mount table resolves by longest
prefix.

`proc->cgroup` is an INDEX, not a pointer: v2 has exactly one membership, and an
index makes fork's whole-PCB memcpy inherit it for free. 0 == root == the
zero-init default, so every pre-existing process is already in the root.

### The thing to get right: this interface is trivially fakeable

The control files are just text. `pids.max` can accept a write and read back
perfectly while limiting nothing; `cpu.weight` can round-trip while changing no
scheduling decision. **Both would pass a test that only read the files.** So
neither is tested through its file:

- **`pids.max`** — by whether `fork()` ACTUALLY fails, **with EAGAIN
  specifically** (any error would also come from an exhausted proc table), and
  succeeds again once raised (the control).
- **`cpu.weight`** — by MEASURING `/proc/<pid>/stat` CPU time under contention.
  Measured: **weight 10 → 3, weight 1000 → 153.**

### Implementation points

- The pids gate runs **before `proc_slot_claim()`** in all three creation paths,
  so a denied fork cannot leave a `PROC_EMBRYO` behind — the state slice 109 says
  must never leak.
- **Hierarchical**: walks to the root, so a child cgroup cannot escape its
  parent's cap by raising its own. bit5 asserts it.
- **Threads count** — Linux's pids controller limits TASKS, so `sys_clone_thread`
  is gated too; otherwise any threaded program evades `pids.max` trivially.
- Writing `cpu.weight` **re-derives the priority of processes already in the
  cgroup** — otherwise it would affect only future members, reading as "the file
  works" while changing nothing for the running workload.
- `cpu.stat` accumulates at the **same site** as `p->cpu_ns` (perf.c), so it
  cannot disagree with `/proc/<pid>/stat`.
- A cgroup name may not contain `.`, so it can never shadow a control file (the
  v2 rule, and what makes path resolution unambiguous). `rmdir` refuses a
  populated cgroup or one with children.

### Limits, stated rather than implied

- **`cpu.weight` selects a PRIORITY BAND, not a proportional share.** This is a
  class-based scheduler with aging, not virtual-runtime, so weight cannot divide
  CPU in the ratio of the numbers. The observed 3-vs-153 is a property of the
  priority bands under this particular contention, **not** proportional sharing —
  do not quote it as a ratio guarantee. The test asserts a measurable skew
  (>= 25%), which is the honest claim.
- **`PRIO_RT` is deliberately unreachable from `cpu.weight`**: letting an
  unprivileged container claim the latency-critical band by writing a number into
  a file would let it starve the desktop.
- Mapping: `<10` IDLE, `<50` LOW, `<400` NORMAL (default 100), `>=400` HIGH.
- **The cpu test needs MORE spinners than CPUs.** On `-smp 4` with two spinners
  each simply gets a core and the weights are invisible; the test runs 3+3.

---

## 18. Slice 12 cut 2 — veth pairs: real frames across a namespace boundary (2026-08-09)

Cut 1 gave network namespaces that isolate by **denial** (no device ⇒ `ENETUNREACH`).
Cut 2 gives a non-initial namespace an actual interface, so frames cross the boundary.

### It is small because `struct net_dev` was already the right abstraction

A veth is a `net_dev` whose `tx` hands the frame to its peer's *receive* path instead of
to hardware. The vtable (`tx` / `rx_drain` / `link_up` / `priv`) needed **no change**.
`src/veth.c` is ~150 lines for that reason, not because the feature was cut short.

The part that genuinely needed work is not the device, it is the **context**. An incoming
frame must be processed *as the namespace that owns the receiving end*, because
`ip_dst_is_for_us()` and ARP compare against "my IP" and that address is now
per-namespace. Hence `net_ns_rx_enter()/leave()` bracketing `eth_recv()`, and
`net_my_ip()` / `net_my_netmask()` / `net_my_mac()` replacing three globals at the points
that matter. Depth-1 save/restore is sufficient **because veth delivery is inline on the
sender's stack** — if that ever becomes queued, this assumption breaks.

`eth_recv()` is the stack's single ethertype demux, so veth reuses it: a veth frame takes
byte-for-byte the same path through arp/ip/ipv6 that a wire frame does.

### No refactor of the globals — deliberately

The initial namespace still uses `g_net_devs` / `g_my_ip` / `g_my_mac` untouched. Only
*non-initial* namespaces carry their own device and addresses. e1000, DHCP and the whole
browser/Chromium arc are therefore unaffected by construction, which is why the DHCP ACK
in the same boot is a meaningful check rather than a formality.

### THE FINDING: a PASS that was wrong, and a test too weak to see it

The first green run reported `ok ARP answered by the peer ns (52:54:00:12:34:56)`.
That is **the host e1000's MAC**. The peer had cached "10.99.0.2 is at *the host NIC*" —
which resolves perfectly and is completely wrong.

Cause: two layers carry a source MAC and I had only fixed one.
- `eth_send()` sets the **ethernet header** src from the sending device — fixed in cut 2.
- ARP's **payload** carries a *sender hardware address* of its own, and
  `arp.c` still filled all three of those from `g_my_mac`.

Fix: `net_my_mac()` (namespace-relative, same rx-context-then-current_proc lookup as
`net_my_ip`), consumed by all three `arp.c` sites. `g_my_mac` count in `arp.c` is now 0.

The test bug is the more important half: bit4 asserted only that `arp_resolve()`
**succeeded**. It now asserts the **value** equals the peer end's `02:00:00:00:00:01`.
Same lesson as the errno assertion in cut 1 — *a resolution that succeeds is not evidence
that it resolved to the right thing.*

### Two process failures in this cut, both self-inflicted, both worth recording

1. **I mis-diagnosed the object location, and the original law was right.** I claimed
   "`rm -f src/*.o` deletes nothing — objects live in `build/`", because `ls src/kernel.o`
   failed. It failed because the build I had just "run" never ran at all (see item 2), not
   because the path was wrong. The link line is `ld.lld ... src/kernel.o src/klibc.o ...`:
   **objects are in `src/`, and `rm -f src/*.o` on a flavour switch is correct.**
   `make clean` does `rm -rf $(OBJS) $(KERNEL) iso build`.

   Proof it matters: slice 16 later hit the exact trap the law exists for. `pmm.o` had been
   compiled with `-DCGMEM_TRACE`; the next build dropped the flag, `pmm.c` was unchanged so
   it was not recompiled, and the link failed with
   `undefined symbol: cgmem_trace_alloc, referenced by pmm.c`. A link error is the lucky
   case — the same staleness usually produces a silently wrong kernel.
2. **I hid a build behind a filter and tested stale bits.** `make ... 2>&1 | grep -iE
   "error|warning:..."` printed nothing — because `make` was **not on PATH at all** and the
   "success" I read was an ISO timestamp from an *earlier* background build. The run that
   followed exercised a binary without my edit. Both of the project's own laws
   ("never hide a build", "verify the marker is in the binary") existed to catch exactly
   this and I bypassed them.

   The gate that now catches it in one step, and should be used for every harness flavour:
   ```
   python -c "d=open('tobyos.bin','rb').read(); \
              raise SystemExit(0 if (b'<new marker>' in d and b'<old marker>' not in d) else 1)"
   ```
   Asserting the **old** string is *gone* is what distinguishes "rebuilt" from "happens to
   contain the marker because the previous flavour did too".

### Verified (`-DLXVETH_BOOT -DLXNS_BOOT`, on a binary gated as above)

```
[veth] pair veth0a(10.99.0.1, net:[4026535936]) <-> veth0b(10.99.0.2, net:[4026535937])
[LXVETH]   ok   pair created, both ends in namespaces
[LXVETH]   ok   per-namespace addressing (host unchanged)
[LXVETH]   frames: a tx=1 rx=1 | b tx=1 rx=1
[LXVETH]   ok   frames crossed BOTH ways
[LXVETH]   ok   ARP answered BY THE PEER'S OWN MAC (02:00:00:00:00:01)
[LXVETH] VERDICT: PASS subtests=4/4
[dhcp] ACK bound: ip=10.0.2.15 ...        <- host network unaffected
netns: BITS=0xff   [LXNS] VERDICT: PASS subtests=11/11    <- cut 1 intact
hb=116 faults=0
```

Both ends are in **non-initial** namespaces in this harness, deliberately: that removes
`net_default()` from the picture, so the pass cannot be an artifact of the host's NIC
carrying the frame.

### What cut 2 does NOT deliver — stated, not implied

- **No control plane.** Pairs are created by kernel call (`netns_veth_pair`), not by
  `ip link add ... type veth`. netlink here only ever *builds reply messages* for
  interface dumps; it does not accept commands. `RTM_NEWLINK`/`RTM_NEWADDR` is a slice of
  its own, and a half-netlink nobody can drive would be worse than none.
- **No bridge.** A pair proves frames cross a boundary; a bridge is what lets many
  namespaces share one uplink.
- **No NAT/forwarding.** A container can reach its peer end. It cannot reach the internet.
- **The ARP cache is still global**, shared across namespaces. Correct only because
  addresses do not currently collide between namespaces; two namespaces reusing
  `10.99.0.1` would alias. Known, not fixed.

So cut 2 means *real frames cross a namespace boundary with per-namespace addressing and
a working ARP round trip*. It does not yet mean "a container can reach the network".

---

## 19. Slice 16 (part 1) — cgroup v2 `memory` + `io` controllers (2026-08-09)

New: `programs/linux-cgroup2/` (8-bit acceptance test; a second program because slice
15's exit-status bitmask is already full at `0xff`). Extended: `cgroup.{c,h}`,
`mmap.c`, `proc.c`, `page_fault.c`, `fork.c`, `oom.c`, `blk.h`, `pmm.c` (flag-gated
instrument), Makefile, the `LXNS_BOOT` harness table.

### THE PRE-EXISTING LIE THIS SLICE HAD TO FIX FIRST

`memory.current` needs a per-process page count. One already existed —
`p->user_pages`, which `/proc/PID/stat` reports as RSS — so the first design summed it
per cgroup for free. Reading it showed it was **assigned once at spawn**:

```c
/* Milestone 19: approximate RSS ... */
p->user_pages = p->user_stack_pages + 1 /* rough elf overhead */;
```

and never updated again. So `ps` reported the **same constant RSS for every process no
matter what it mapped**, and `oom.c`'s victim score — `(p->user_pages * 1000) / total` —
was ranking processes by a fabricated number. Building `memory.current` on it would have
produced a precise-looking figure with nothing behind it, which is the exact failure mode
slice 15's notes warned about for `pids.max`.

So `user_pages` is now **maintained**, and both the controller and `ps`/`oom` get real
numbers out of it. That was not in the plan; it was in the way.

### One funnel, because eight scattered counters is one missed site from drift

```c
uint64_t mm_user_page_alloc(struct proc *p);      /* charge, then allocate */
void     mm_user_page_free(struct proc *p, uint64_t phys);
void     mm_user_pages_release_all(struct proc *p);   /* bulk, at teardown */
```

Every user page in the system now enters and leaves through these. The property that
matters is that it is **auditable by grep**: `pmm_alloc_page()` must not appear in a
user-page context anywhere else. Returning 0 on refusal reuses the OOM path callers
already handle, so no failure path needed rewriting.

Uncharge at teardown works from the process's **own** counter, not by counting frames
during the page-table walk — correct however the pages were released, including
`vmm_destroy_user_pml4`'s bulk walk, and it cannot double-count pages a thread shares
with its leader.

### THE MISS THAT COST THE MOST: `mmap_try_fault` is not the fault handler

I charged `mmap.c` (brk, CoW, demand, eager-commit), `proc.c` (stack, `proc_brk`) and
`fork.c` — then watched a child allocate **64 MiB inside a 6 MiB `memory.max`** with its
own RSS unchanged at 98 pages.

`src/page_fault.c` has ten allocation sites and is the **real** `#PF` handler;
`mmap_try_fault` is only a sub-path of it. Charging the sub-path left every ordinary
demand fault uncharged. Four sites there needed the funnel (PTE_DEMAND promotion,
file-backed VMA, anonymous VMA, stack growth) plus the stack-growth rollback free.

The lesson is the audit method, not the file: **enumerate every caller of the underlying
primitive before assuming you know where the work happens.** One `grep -rn pmm_alloc_page
src/*.c | sort | uniq -c` at the start would have shown `page_fault.c` at ten and saved
several build/boot cycles. I ran that grep only after the test failed.

### The bug bit7 caught: CoW must be charge-NEUTRAL

Charging `cow_copy_page()` looked obviously right and is wrong. A CoW break **exchanges**
one page for another: the process was already charged for the shared page by the fork
bulk-charge, and the old reference is dropped in the same operation, via
`page_ref_dec`/`pmm_free_page` — a path with no uncharge. Net effect: **+1 leaked charge
per CoW break**, showing up as a steady 2-pages-per-fork-cycle drift in
`memory.current`. Every other bit stayed green through it.

`cow_copy_page` is therefore deliberately uncharged, with the reasoning written at the
call site so nobody "fixes" it back.

### The hot-path cost the plan warned about — measured, then removed by design

The plan's risk note: *"cgroup `memory` touches the allocator hot path. If accounting
costs measurable throughput, ship `pids` + `cpu` and defer."*

Two things kept it cheap:

1. **The charge is at the USER-PAGE level, not inside the PMM.** `pmm_alloc_page` itself
   is untouched, so kernel allocations (page tables, slab, driver DMA) pay nothing, and a
   user page's charge sits next to a 4 KiB `memset` that dwarfs it.
2. **The first version still had a real cost, and it was mine.** `memory.peak` was
   refreshed by calling `cgroup_mem_pages()` per ancestor, and that summed the subtree by
   scanning all `CGROUP_MAX` slots and walking each one's parent chain — **O(32 x depth)
   per page fault, paid even on a machine with no limits at all.** Replaced with an
   incrementally-maintained `mem_subtree` updated by one `cg_subtree_add()` walk:
   O(depth), no inner loop, and `memory.current` becomes an O(1) read.

With `mem_max_pages == UNLIMITED` short-circuiting the limit check, a system that sets no
limits now pays one add per ancestor per user page. `memory` and `io` both ship.

### What is charged, and what is not — stated, not implied

| | |
|---|---|
| **Charged** | anonymous user address-space pages: brk growth, demand faults, eager commit, exec/stack setup, fork's inherited pages |
| **NOT charged** | page cache, memfd/shm backing pages, kernel allocations incl. page tables. **Linux charges all three.** |
| **Over-counted** | pages shared via `sys_fork_share`/vfork and mmap CoW clone are counted in both cgroups until copied. `sys_fork` FULL-copies, so its bulk charge is exact. |

The over-count is deliberate: it buys the invariant `mem_pages == sum of member
user_pages`, which the test can actually check. A counter nobody can audit is how drift
ships unnoticed.

### `io`: accounting only, and NO `io.max`

Charged inside `blk_read`/`blk_write` — `static inline` in `blk.h`, so they are the one
place every block operation in the kernel passes through; a driver cannot bypass the
accounting without bypassing the API. Charged on success only, so `io.stat` cannot
disagree with the device after an unplug. Cost is one call per request against a DMA
round trip.

**`io.max` does not exist**, and its absence is the honest signal: throttling needs
somewhere to sleep inside a synchronous block path called under filesystem locks. A knob
that accepted a limit and throttled nothing is precisely what this arc already caught in
`VFS_MNT_RDONLY` and `PR_SET_NO_NEW_PRIVS`. `cgroup.controllers` advertises
`cpu io memory pids` — every one of which enforces or reports something real.

### The test: bit6 is the one that makes the others mean anything

`memory.current` must agree **exactly** with the RSS `/proc/PID/stat` reports for the
same single-member cgroup. Two independent code paths over one counter: a fabricated
number, or one that had drifted from a missed uncharge, cannot survive it. No tolerance
is allowed, because slack would hide exactly the drift being looked for. bit7 then
requires **zero** drift across four fork/allocate/exit cycles.

Measured: `memory.current 401408 -> 8790016 (+8388608 for 8 MiB touched)` and back down
by the same 8388608 on `munmap` — exact in both directions, not approximate.

### Method failures in this slice, all mine, all previously documented laws

1. **Bash heredocs ate backslashes FOUR times.** `"\n"` inside a quoted heredoc reaches
   Python as a real newline, so `'\n'` became an unterminated char literal in generated C
   (a 20-error cascade in `cgroup.c`), and later broke the test program — whose build
   then failed while `&&` skipped the ISO rebuild, so **I read output from a stale ELF and
   drew a wrong conclusion from it.** The law already existed: use Write/Edit for anything
   with escapes. Scripts written to a file with the Write tool are immune.
2. **A grep-filtered build hid `make: command not found`.** The "ISO built" I believed was
   a timestamp from an earlier background build, and the run that followed exercised a
   kernel without my edit. PATH needs
   `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"`.
3. **I mis-corrected the flavour-switch law** (see section 18, item 1): objects are in
   `src/`, `rm -f src/*.o` is right, and slice 16 proved it by linking a stale `pmm.o`
   against a dropped `-DCGMEM_TRACE` — `undefined symbol: cgmem_trace_alloc`.
4. **Concluding from an incomplete check.** "Zero `[cgtrace]` lines, so it never fires" was
   wrong twice: once because the trace cap (10) had been consumed by an earlier subtest,
   once because the test binary in the ISO was stale. Both times the fix was to check
   whether the *instrument* could have fired before trusting its silence.

**The gate that catches (2) and (3) in one step, and is now used before every harness
boot** — note it also asserts the OLD marker is GONE, which is what separates "rebuilt"
from "the previous flavour happened to contain this string too":

```
python -c "d=open('tobyos.bin','rb').read(); \
  raise SystemExit(0 if (b'<new>' in d and b'<old>' not in d) else 1)"
```

### THE MEASUREMENT TRAP: two QEMUs, one log filename

Partway through bring-up the *same kernel binary* appeared to give different results run
to run — enforcement working, then not, then working. I was one step from writing
"`memory.max` is racy" into the handoff.

It was not the kernel. A batch runner I had stopped with `TaskStop` left **its QEMU child
alive**, and that QEMU was still writing the *same* `logs/cg2s$i.log` paths the new runner
used. Two writers on one filename, interleaved, from two different ISOs. There is no way
to tell that apart from a flaky kernel by reading the log.

Three rules came out of it, and they apply to every harness in this tree:

- **Stopping a runner does not stop its QEMU.** `TaskStop` kills the shell; check
  `tasklist | grep qemu` and `taskkill //F //IM qemu-system-x86_64.exe`, then verify the
  count is zero before believing any new run.
- **Never reuse log filenames across runs.** Unique names per run make double-writing
  impossible to mistake for behaviour.
- **Never let two QEMUs run at once when measuring.** Beyond contamination, they compete
  for host CPU, and this arc already has a law that a slow capture is not a freeze.

Both of the earlier "flaky enforcement" observations are consistent with this and with
object staleness from my repeated `-DCGMEM_TRACE` toggling. I am recording it as an
unresolved-then-explained measurement artifact rather than a kernel defect, because the
final binary's numbers are clean and reproducible — but the honest statement is that the
intermediate runs were never trustworthy evidence in the first place.

---

## 20. Slice 14 — native privilege drop, sandbox GATED (2026-08-09)

Decision taken: **implement it behind `CW_SANDBOX`, default OFF.** The flagship
taskbar browser (slice 119) keeps its current, validated behaviour; flipping the
default is a one-line change once the hardware run below passes.

New: `programs/nsetuid/` (6-bit native acceptance test). Extended:
`abi.h` (+`ABI_SYS_SETUID` 189, `ABI_SYS_SETGID` 190), `syscall.h`, `syscall.c`,
`libtoby/{include/unistd.h,src/unistd.c}`, `programs/chromewin/main.c`, Makefile,
the `LXNS_BOOT` table.

### What was actually missing: a native setuid

The plan framed slice 14 as "remove `--no-sandbox --no-zygote`". That is not the
blocker. Chromium refuses to enable its sandbox **as root** — a policy check in
`zygote_host_impl_linux.cc:101`, not a kernel gap — and `chromewin` is a NATIVE app,
while the setuid family existed only in the Linux personality. So the browser had no
way to stop being root before `execve`.

`ABI_SYS_SETUID`/`SETGID` fill that gap, and they **share one implementation** with
the Linux personality (`lx_do_setid`, two entry points). Two credential models in one
kernel drift, and one of them ends up the lenient one — which for a privilege-drop
primitive is a security bug, not an inconsistency. Slice 11's hardening (namespace
translation, capability clearing on a drop from root) therefore applies to both.

### Order matters in the launcher, and a failure must be FATAL

`chromewin` drops **setgid before setuid** — after dropping uid there is no authority
left to change gid, so the reverse order silently leaves the process in group 0. And a
failed drop **returns 1 rather than continuing**: carrying on would exec a *root*
Chromium with the sandbox flags now removed, which is exactly the `exit(1)`-at-4s trap
that once cost a 440-second run and reads as a hang rather than a refusal.

### TWO REAL BUGS THE TEST FOUND

**1. `setgid` authorised itself off the effective GID.** The code read
`eff = is_uid ? cp->uid : cp->gid` and tested `eff == 0`. So a process that had dropped
to uid 1000 while still in group 0 — *exactly the state after a bare `setuid`* — took
the privileged branch and could switch to **any** gid. Linux gates `setgid` on
CAP_SETGID / euid 0, never on the current gid. The `setreuid`/`setregid` case a few
lines below already had it right (`bool priv = (cp->uid == 0)`), so the two calls
disagreed about what "privileged" means and `setgid` was the lenient one.

Caught by `nsetuid` bit4, which asserts a non-root process CANNOT setgid to an
arbitrary group.

**2. `sys_chmod`/`sys_chown` leaked raw VFS error codes to userspace.** Both returned
`vfs_chmod`/`vfs_chown` directly. `VFS_ERR_PERM` is `-12`, so a permission denial
arrived in userspace as **`ENOMEM`**. Same bug class as the one already fixed with
`vfs_err_to_abi()` — these two sites were missed. Their `cap_check` failures also
returned a bare `-1` (= EPERM by accident) instead of `-ABI_EPERM`.

Surfaced because bit3 checks that a root-only `chown` starts failing after the drop.
It did — with `errno=12`. **A test that only asserted "chown failed" would have
accepted that forever**, so bit3 now asserts the errno is `EACCES` or `EPERM`. Same
lesson as slice 12's ARP MAC and cut 1's ENETUNREACH: the *value* is the assertion.

### Why bit3 is the check that carries this test

`getuid()` returning 1000 proves a field changed. A "drop" that moved `p->uid` while
leaving root authority intact would pass a naive test and be **worse than no drop**,
because it would look safe. bit3 therefore asserts a **root-only syscall now fails**,
with the right errno.

### Verified in QEMU

```
nsetuid: start uid=0 (root)
nsetuid: setuid(1000) OK, getuid()=1000
nsetuid: climb-back REFUSED (errno=1) -- drop is one-way
nsetuid: root-only chown now REFUSED with errno=13 (EACCES/EPERM) -- authority
         really dropped, not just the uid field
nsetuid: non-root setgid REFUSED (errno=1)
nsetuid: absurd uid rejected (errno=1)
nsetuid: BITS=0x3f (want 0x3f)
[LXNS]   ok   slice 14: native privilege drop (C) exit=63 (want 63)
[LXNS] VERDICT: PASS subtests=13/13 skipped=0     faults=0
```

Both build configurations are gated on the BINARY, not on the build succeeding:

| build | `--no-sandbox` in the ELF | drop code in the ELF |
|---|---|---|
| default | present | compiled out |
| `PROG_EXTRA_CFLAGS=-DCW_SANDBOX` | **absent** | present (incl. fatal-refusal path) |

**`EXTRA_CFLAGS` does NOT reach user programs — use `PROG_EXTRA_CFLAGS`.** The
Makefile says so at `LIBTOBY_KABI_PROGRAM_RULES` and records that this exact mistake
once made `-DCHROME_FULL` silently miss chromewin, producing an exit-127 that looked
like a loader failure. I made it again; the binary check caught it, a successful build
would not have.

### THE HARDWARE RUN THIS SLICE OWES

Every QEMU boot in this tree auto-logs-in as root, so the sandboxed path **cannot** be
validated here — that is a structural blindness, not an oversight. Before
`CW_SANDBOX` may become the default, on the EliteDesk 800 G1 (COM4, Rufus DD image):

1. Build `PROG_EXTRA_CFLAGS="-DCW_SANDBOX"` and confirm on the serial log:
   `[chromewin] dropped to uid=1000 gid=1000 before exec (sandbox build)`.
2. Confirm Chromium **survives past ~5 s**. Dying at ~4 s means it still saw root, or
   the drop failed and the launcher's fatal path did not fire.
3. Confirm the profile directory is writable as uid 1000. Slice 120 chmods `/data`
   0777 at mount, which *probably* covers it — **unverified for a non-root chrome**,
   and slice 120 is itself the bug that ate a real-HW session. Look for
   `Failed to create data directory` / a chrome cache error, not just a running
   process.
4. Confirm a renderer process actually appears (the sandbox is the *zygote* path;
   `--no-zygote` is also gone). One process alone means the sandbox did not come up.
5. Then load a page and confirm rendering — the sandbox denying a syscall Chromium
   needs would show as a blank or dead tab rather than a crash.

If any step fails, the fix belongs in the launcher or `/data` permissions, not in the
kernel: the namespace + seccomp prerequisites are verified independently
(`linux-netns` bit7, `linux-seccomp` bit5).

### THE SANDBOX RUN WAS MADE AFTER ALL, AND IT FOUND THE NEXT BLOCKER

I had written that this environment "structurally cannot" validate the sandboxed
path because QEMU auto-logs-in as root. **That reasoning was wrong for this slice:**
the privilege drop is performed by `chromewin` itself, so it happens whatever the
session's uid is. The run was made.

**Take 1 was wasted on the wrong launcher.** `-DCHROMIUM_BOOT` has its own in-kernel
launcher that execs `/opt/chrome` directly and never goes through `chromewin`, so
none of slice 14's code was on the path. The tell was an absent `[chromewin]` line
and a bare `chrome` pid. The chromewin path is **`-DTKAPP_BOOT -DTKAPP_CHROMEWIN`**
(Slice 39 FRONT C, 420 s hold). The gate now asserts the KERNEL references
`/bin/chromewin` as well as that the staged binary is a sandbox build — which would
have caught take 1 before spending a boot on it.

**Take 2, the result that matters:**

```
[chromewin] dropped to uid=1000 gid=1000 before exec (sandbox build)
"Running as root without --no-sandbox is not supported"   -> 0 occurrences
[ns] clone: pid=4 new namespaces user
[fork] parent pid=3 -> child pid=4
[isr] VMA at cr2=0xffffffffffffffd8:  rip=0x0000000003716f29
[isr] user-mode fault -- terminating user process pid=4
[proc] pid=4 'chrome-headless-shell' exit code=-1  cpu=0 ms  syscalls=0
[3] 56(clone)  a1=268435473 = 4        <- CLONE_NEWUSER|SIGCHLD, child pid 4
[3] 61(wait4)  a1=4        = -10       <- ECHILD
FATAL:sandbox/linux/services/credentials.cc:309] Check failed: . : No child processes (10)
```

**So slice 14's deliverable works.** The privilege drop takes effect, Chromium
accepts a non-root launch (the root refusal never fires), and its sandbox proceeds
far enough to start building namespaces. That is the whole point of the slice.

**And the next blocker is now precisely located, in the kernel, not the launcher:**

`clone(CLONE_NEWUSER|SIGCHLD)` (flags `0x10000011`; the low byte is the exit signal,
SIGCHLD 17) creates the child — `[ns] clone: pid=4 new namespaces user` confirms the
namespace is applied — and the child then **faults before executing a single
syscall** (`cpu=0 ms syscalls=0`). `cr2 = 0xffffffffffffffd8` is `-0x28`: a
dereference at a small negative offset from a base register holding **zero**, which
is the same shape as the already-fixed `proc_enter_user_asm` GP-register bug
([[linux-exit-path-fault]]) — a register the child should have inherited is zero on
its first return to user mode. Chromium's `wait4` then finds no child (`ECHILD`) and
its sandbox `CHECK`s out.

**A COVERAGE GAP IN MY OWN TESTS EXPLAINS WHY THE ARC'S 8/8 MISSED THIS.**
`linux-userns` and `linux-netns` exercise `unshare(CLONE_NEWUSER)` — changing the
CALLER's namespace. Nothing tested `clone(CLONE_NEWUSER)` — creating a CHILD directly
in a new namespace. Those are different code paths, and only the second is what
Chromium's sandbox uses. The lesson generalises: for every namespace flag, both
entry points need a test, and the arc only ever tested one.

Next step for slice 14 is therefore a kernel fix, not launcher work: make a
clone-with-namespace-flags child survive its first return to user mode, then add a
`clone(CLONE_NEWUSER)` case to `linux-userns` so it stays fixed.

### CORRECTION: the clone-child diagnosis above is WRONG, and here is the disproof

The section above concluded that `clone(CLONE_NEWUSER)` produces a child that faults
before its first syscall. **That is not true, and a new test proves it.**

`programs/linux-clonens/` was written to pin the bug before touching the fork path.
It issues the clone exactly as Chromium does -- `syscall(SYS_clone, flags, 0,0,0,0)`
with a NULL child stack -- and covers the matrix:

| case | result |
|---|---|
| root + plain clone (control) | ok |
| root + NEWUSER / NEWUTS / NEWIPC / NEWNS / NEWPID / NEWNET | ok |
| NEWUSER child really unmapped (`getuid()==65534`) | ok -- flag honoured, not ignored |
| root + NEWUSER with a live sibling thread | ok |
| **uid 1000** + plain clone | ok |
| **uid 1000** + NEWUSER | ok |
| **uid 1000** + NEWUSER + live sibling thread (closest to Chromium) | ok |

`BITS=0xff`, zero faults. Non-root does not break it. A live sibling thread does not
break it. Both together do not break it.

**The first version of this test was too weak, and saying how it was weak matters
more than the result.** Its child called `_exit()` immediately -- which reads almost
no state, so a child whose callee-saved registers, frame pointer or TLS base had been
mangled would have passed. Chromium's child ran real code and faulted at
`cr2=-0x28`, a dereference through a base register holding zero, which is exactly the
damage `_exit()` cannot see. The child now walks a deep recursive call chain, formats
through libc, and reads `errno` (a TLS access). Still 8/8.

**So the Linux-arc namespace surface is eliminated as the cause.** What remains is
what is specific to Chromium at that moment: **forking chrome's own address space**
-- 327 VMAs, shared libraries packed from `0x100000000000`, V8 cages -- through
`vmm_cow_fork`. The tree already treats that as fragile: the `CHROMIUM_BOOT` path
carries a hard cap of 16 chrome CoW forks and a comment noting that only
"STW tg_vm_quiesce + eager CoW make limited chrome CoW forks viable".

**That makes the sandbox blocker a Chromium-arc problem, not a Linux-arc one.** The
next probe belongs to that arc's tooling, not to namespaces:

1. Map the faulting `rip=0x0000000003716f29` to a function in
   `chrome-headless-shell` (compute the offset from the ELF LOAD base, per that
   arc's law) -- it identifies what the child was doing when its base register was
   zero.
2. Determine whether a *plain* CoW fork of chrome faults the same way, which would
   confirm the namespace flag is incidental. The `[fork] clone flags=` and
   `allow chrome CoW fork` instruments already exist but are `CHROMIUM_BOOT`-gated,
   so they were compiled out of the `TKAPP_CHROMEWIN` build that produced this
   failure -- enabling them for a chromewin build is the cheapest next step.

**What slice 14 delivered stands unchanged:** the privilege drop works, Chromium
accepts a non-root launch, and its sandbox engages. What it uncovered is a
pre-existing weakness in CoW-forking a very large address space, which was always
going to surface the first time anything asked chrome to fork.

### Also corrected: `sys_fork` does NOT full-copy

Two slice-16 comments (in `cgroup.h` and `fork.c`) justified bulk-charging a forked
child by citing `copy_user_pages`'s "intentionally a FULL copy (no COW)" note.
**`copy_user_pages` is dead code** -- the compiler says so (`unused function`), and
`sys_fork` actually calls `vmm_cow_fork`. No behaviour changes (the over-count note
already covered CoW paths, and the child still gets its own `cr3`, so it is its own
mm owner and the bulk charge still lands correctly), but the stated reason was wrong
and is now fixed.

---

## 21. Slice 14 CLOSED OUT + slice 16 CAPSTONE (2026-08-10)

**Read this section before acting on §20's middle.** §20 concludes that the
sandbox blocker was "CoW-forking chrome's own address space -- 327 VMAs, V8
cages -- through `vmm_cow_fork`", and hands the next step to the Chromium arc's
tooling. **That conclusion is wrong.** Nothing about chrome's size, its VMAs or
CoW was involved. The cause was four ordinary `clone(2)` bugs, all fixed here,
and the first was visible in the very log §20 quotes.

### THE ROOT CAUSE, and the one step that found it

§20's own first probe -- "map `rip=0x0000000003716f29` to a function in
`chrome-headless-shell`" -- answers the whole question in about a minute.
`base=0x500000` is in the same log, so the ELF vaddr is `0x3216f29`:

```
3216f1b:  e8 60 a5 31 08    call   b531480 <clone@plt>
3216f20:  64 48 8b 0c 25... mov    %fs:0x28,%rcx        <- reload the stack canary
3216f29:  48 3b 4d d8       cmp    -0x28(%rbp),%rcx     <- FAULTS. cr2 = -0x28 => rbp == 0
```

`cr2 = 0xffffffffffffffd8` is not "a base register holding zero" in general --
it is **`%rbp` exactly**, in a stack-canary check. And the call above it is
`clone@plt`: **Chromium goes through glibc's clone() LIBRARY function, not
`syscall(SYS_clone, ...)`.**

That distinction is the bug. glibc's `__clone` stores `fn`/`arg` at the top of
the caller-supplied child stack, and its child side is

```
xor %ebp,%ebp ; pop %rax ; pop %rdi ; call *%rax
```

**`LX_clone` dropped `a2` (child_stack) on the floor.** The child therefore ran
those pops against the PARENT's stack: `%rax` got the return address of the
caller's own `call clone@plt`, the child "called" back into the middle of its
parent's caller with `%rbp == 0`, and died on the first frame-relative access --
zero syscalls, so `wait4` found nothing and the sandbox `CHECK`ed out.

**Why the previous session's 8/8 disproof was itself wrong.**
`programs/linux-clonens` issues `syscall(SYS_clone, flags, NULL, ...)`, and for
a NULL child stack "resume where the parent was" is the CORRECT behaviour -- so
that test passes on a kernel with this bug and always would have. Its header
claimed the raw form "is precisely what Chromium's ForkWithFlags does"; that
sentence is corrected in the file. The lesson is not "test both entry points"
(§20 said that, and the test written from it still missed) -- it is **test the
entry point the real caller uses, and read the disassembly to find out which
one that is.**

### FOUR KERNEL BUGS, in the order they surfaced

Each was uncovered by fixing the one before it, so they are a chain, not a list.

1. **`clone(2)` ignored `child_stack` on the CoW arm** (`LX_clone`, `LX_clone3`).
   Fixed with `proc->clone_child_stack`, staged by the clone arm and consumed
   inside the fork -- the same pattern and the same reason as slice 8's
   `clone_ns_flags`: the child is enqueued before the syscall returns, so a
   post-fork fixup is an SMP race, not a simplification.

2. **`nsproxy_apply_clone_flags` contradicted its own comment.** It read

   ```c
   if ((want & ~CLONE_NEWUSER) && !userns_capable(LCAP_SYS_ADMIN)) return -EPERM;
   ```

   under a comment saying "asking for NEWUSER together with others is authorised
   by the NEWUSER itself". Masking a bit out of the set being CHECKED grants
   nothing. So unprivileged `clone(CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNET)` --
   flags `0x70000011`, the only combination unprivileged containers ever use,
   and Chromium's second clone -- was measured against the PARENT's user
   namespace and refused with EPERM. `ns_unshare()` had always been right,
   because it INSTALLS the user namespace first and only then tests the
   capability. Two entry points to one feature, one of them wrong -- again.

3. **`sys_fork_share` ignored `child_stack` too.** With bug 1 fixed, the sandbox
   advanced from `credentials.cc:309` to `credentials.cc:118` --
   `ChrootToSafeEmptyDir`, which clones with
   `CLONE_VM|CLONE_VFORK|CLONE_FS|SIGCHLD|CLONE_UNTRACED` (`0x84311`) **and a
   stack**. That reaches the share arm, which maps the child a fresh zeroed
   private stack, so `pop %rax` gave 0 and `call *0` produced `rip=0, err=0x14`
   -- the signature already recorded against busybox `unshare -f` in §6.

   **The private stack is UNCHANGED for the caller that supplies none** (plain
   `vfork`, the load-bearing Chromium launcher path §6 warns against touching).
   An explicitly supplied stack simply takes precedence. The two callers are
   distinguishable, so this is not the fix §6 forbids.

4. **`fork`/`clone` returned the HOST pid to a caller inside a pid namespace.**
   Slice 10 translated `getpid`, `getppid`, procfs and `wait4`'s ARGUMENT -- but
   not the number fork HANDS BACK, which is precisely the number every caller
   then feeds to `waitpid`. Inside a namespace `pid_knr()` finds no mapping for
   it, so a process could not reap its own children (ECHILD). Fixed by
   `lx_child_pid_ret()` on all four fork/clone/clone3 return paths, threads
   included (a tid is namespace-local too).

Plus one bug introduced by fix 1 and caught by the capstone: **the staging
fields are copied into the child by the whole-PCB `memcpy`.** A child that keeps
`clone_child_stack` applies it to ITS next fork, pointing a grandchild's RSP at
an address belonging to a program that had already been replaced by execve.
`clone_ns_flags` had the same latent hole. Both are now explicitly zeroed in all
three fork paths.

### Where Chromium's sandbox stands now -- NOT "done"

```
[chromewin] dropped to uid=1000 ...
[ns] clone: pid=4 new namespaces user            <- CanCreateProcessInNewUserNS
[proc] pid=4 ... exit code=0  syscalls=19        <- that probe child RUNS and exits 0
[ns] clone: pid=5 new namespaces pid user net    <- 0x70000011 now ACCEPTED
[fork] share pid=5 -> child pid=6 ... stack=0x0  <- the caller's stack is honoured
Check failed: sys_chroot("/proc/self/fdinfo/") == 0
```

**The next blocker is a named, ordinary feature gap: `/proc/PID/fdinfo` does not
exist.** `ChrootToSafeEmptyDir` chroots into it as a guaranteed-empty directory.
It is a bounded procfs addition -- a directory plus one file per open fd,
mirroring the existing `fd/` handling in `procfs_stat` / `procfs_opendir` /
`procfs_readdir` -- and a genuine Linux-completeness item regardless of
Chromium. **Expect more blockers after it**; the sandbox does a great deal past
this point. What changed is the KIND of failure: from "the child dies before
executing one instruction, cause unknown" to "the child runs and prints what it
is missing".

Reproduce with `bash logs/cwsandbox.sh <fresh-tag>`, which carries the build
laws (right launcher, `PROG_EXTRA_CFLAGS`, forced object deletion) and gates on
the BINARIES before spending a boot.

### Slice 16's capstone: a real OCI container, `LXCONTAINER_BOOT`

```
TAG=<fresh> bash logs/lxcontainer.sh      # never reuse a log name
```

`programs/linux-ocirun/` is a minimal OCI runtime: it parses a real
`config.json` and applies namespaces, id mappings, cgroup v2 limits, mounts, a
seccomp profile compiled from that profile's syscall NAMES, `pivot_root`, and a
privilege drop -- then execs the configured process and reports its status.

**The rule it is built around:** a container runtime's failure mode is not
crashing, it is STARTING ANYWAY. Anything affecting isolation that cannot be
honoured is FATAL (unknown namespace type, `namespaces[].path`, an unknown
seccomp action or syscall name, seccomp argument filters, a failed
map/pivot/drop). Anything else not applied is REPORTED BY NAME and counted. The
bundle asks for a `tmpfs /dev` on purpose so every run exercises that reporting
-- and `logs/lxcontainer.sh` asserts **exactly one** `NOT APPLIED: /dev`.

The verdict is carried by `programs/linux-ctrprobe/`, which runs INSIDE and
returns an 8-bit mask in which every bit asserts a value:

```
[CTR] ok pid ns: getpid=1, /proc Pid:1        [CTR] ok uts ns: hostname 'toby-container'
[CTR] ok root: host root GONE and / is Alpine 3.19.0 (both halves)
[CTR] ok creds: uid=gid=1000, root-only chown refused (errno=1)
[CTR] ok net ns: connect(10.0.2.2:80) -> ENETUNREACH exactly
[CTR] ok alpine: the container's OWN busybox -- 'BusyBox v1.36.1 ...'
[CTR] ok seccomp: chmod -> errno=1 as configured, others unaffected
[CTR] ok cgroup pids.max: fork returned EAGAIN after 7 children (limit 8)
[CTR] BITS=0xff      [LXCONTAINER] VERDICT: PASS setup=3/3 probe=0xff
```

**The rootfs needs its own MOUNT.** `pivot_root` requires the new root to BE a
mount point, and this VFS cannot bind a subtree to make one (`MS_BIND`
re-registers an existing *mount's* ops at a second path, so binding
`/data/alp` onto itself is not expressible). `LXCONTAINER_BOOT` therefore
creates a RAM-backed tobyfs volume at `/oci`. The runtime **refuses to fall back
to chroot** when the pivot is impossible: a silent downgrade from "the old root
is gone" to "the old root is merely hard to name" is exactly the quiet weakening
this program exists not to do.

**Two things the capstone taught, both worth more than the pass:**

- **SIZE A tobyfs VOLUME BY INODES, NOT BY CONTENT.** The rootfs unpacks to
  ~9 MiB, so the volume was made 32 MiB. `tobyfs_format` derives
  `inodes = total_blocks / 64` (floor 256), and the Alpine rootfs is **528
  entries, 335 of them symlinks**. The extraction died halfway with
  `tar: can't create symlink './usr/bin/readlink'` -- a message that points at
  symlinks, and not at the volume. 192 MiB = 768 inodes; a `_Static_assert`
  guards it now and the harness prints the budget before extracting.
- **THE PROBE COMMITTED THIS ARC'S OLDEST MISTAKE.** Two of its checks forked
  and then wrote `waitpid(p, &st, 0);`, discarding the result. Both reported
  `ok` while bug 4 above was live and every one of those waitpids was returning
  ECHILD. `linux-clonestk` caught what the container missed, and only because it
  asserted waitpid's RETURN VALUE. The probe now checks the reap and the kill.

### New tests, and what each is for

- `programs/linux-clonestk/` -- the LIBRARY `clone()` shape. bit1 is the check
  that carries it: not "the child ran" but "the child's own frame address is
  INSIDE the buffer we supplied", because a child pointed at some other valid
  stack would pass the weaker version and still be wrong. Its child also does a
  plain `fork()` of its own, which is what catches staging leaking through the
  PCB copy. **Negative control run:** with the fix disabled the mask is `0x00`
  and every case fails with `errno=10` -- ECHILD, the literal
  `credentials.cc:309` signature.
- `programs/linux-ctrprobe/` -- the in-container payload described above.
- `logs/cwsandbox.sh`, `logs/lxcontainer.sh` -- one-command gates, each
  refusing to reuse a log filename and each gating on binaries rather than on
  the build's exit status.

### Also added, because an OCI config asks for them

`mount(2)` now accepts `-t proc`, `-t sysfs` and `-t cgroup2`. Each mounts an
existing stateless singleton at a second point (content is rendered per-caller,
so a second mount point is a second VIEW, not a second instance). This also
closes a pre-existing mismatch: `/proc/filesystems` had always advertised
`nodev sysfs` while `mount(2)` answered ENODEV for it; `cgroup2` is now listed
as well. **No `tmpfs`** -- this kernel has no mountable in-memory filesystem
(ramfs is the initrd singleton), and accepting `-t tmpfs` to mount something
else would be precisely the lie this arc keeps catching.

Stated limit: there is no cgroup namespace, so a container that mounts
`cgroup2` sees the whole hierarchy rather than a subtree rooted at its own
cgroup. The capstone bundle deliberately does not mount it.

### Gate state after all of the above

`lxposix --full` **GREEN 23/23** with `enosys_gaps=0`, `LXNS` **15/15**,
`LXCONTAINER` **PASS probe=0xff**, zero faults in every run.

---

## 22. Post-capstone completeness push (2026-08-11)

Six commits after §21, all gated: `4109c03` (syscall name table), `5988d75`
(fdinfo + syscall batch), `3f721b6` (/dev stat), `f18a146` (tmpfs), `5566908`
(ptrace), `6219344` (net_ns device list). Plus `57f4335`, which is a GATE fix
and is the first thing below because it changes how much the others' greens are
worth.

### THE GATE COULD NOT SEE A TEST THAT NEVER RAN

`logs/lxns.sh` asserted `VERDICT: PASS` and nothing else. A subtest whose binary
failed to stage simply did not run, the count dropped, and the verdict stayed
PASS. Not hypothetical: `linux-ptrace` was added, its Makefile and harness-table
edits silently did not apply, and the gate reported **GREEN 17/17 — exactly
what it reported before the test existed.**

A missing test reads as a pass, which is the same failure shape as a green test
over a real bug and arguably worse, because there is nothing to notice. The
gate now asserts the subtest COUNT (`WANT_SUBTESTS`, currently 18); raise it
when you add one.

Its first version did not work either — the `sed` backref was written through a
heredoc and arrived as a raw `\x01` byte, so the extracted count was always
empty and the guard fired on every run. Replaced with `grep -o | cut`, which
has no escapes to lose, and **verified against a real log before committing**.

### MEASURING THE SYSCALL SURFACE: two wrong numbers before a right one

The count is now **212** reachable syscall numbers (208 of the 0..334 core
range). Getting there took three attempts and the wrong ones were reported
before they were caught:

- `case\s+(\d+)\s*:\s*(/\*|\n)` missed every raw-number case followed by `{`,
  so `clock_nanosleep`, `memfd_create` and `sigaltstack` read as ABSENT. **204,
  too low** — and that list was stated to the user before it was checked.
- Dropping the suffix then matched `lx_scname`'s ~90-entry NAME TABLE as though
  those labels were handlers. **218, too high.**
- Scoping the scan to the dispatcher's own body by brace matching: correct.

The census lives in a script now, not a grep. **Anything claiming a syscall is
absent must be checked against the dispatcher body**, and a spot-check that
disagrees with a memory of what was implemented is probably the spot-check.

A real defect fell out of it: `lx_scname` named **305 "syncfs"**. syncfs is 306
and was already named correctly a few lines above. That table is what the ENOSYS
census and the `[lx-recent]` ring print, so an unhandled `clock_adjtime` would
have been reported under the wrong name and sent the next reader to implement
the wrong thing.

### `/proc/PID/fdinfo`, and the batch

fdinfo is a directory plus one file per open fd, mirroring `fd/`. Chromium's
`ChrootToSafeEmptyDir()` chroots into it precisely because it holds exactly the
caller's own descriptors and empties when they close — so the listing walks the
LIVE fd table. `mnt_id` is OMITTED rather than reported as a plausible constant:
Linux already varies fdinfo's field set by descriptor kind, so key-based readers
cope, and inventing a value is the fabrication this arc keeps undoing.

Also `mkdirat`, `getcpu`, `mlock2`, `fadvise64`, `openat2`,
`process_vm_readv/writev`, `membarrier`. Two are honest no-ops and the
distinction matters: `fadvise64` is a HINT the spec permits ignoring and
`mlock2` cannot page out what never pages out, so no caller can observe a
difference — unlike a mount flag or a seccomp action, where ignoring the
argument removes a guarantee. `openat2` REFUSES any `RESOLVE_*` bit with EINVAL,
because those are path-resolution RESTRICTIONS and honouring the open while
dropping them hands back a sandbox that does not exist.

`membarrier` is backed by the existing TLB-shootdown IPI, which IS a real
barrier (taking an interrupt serializes the receiving core); QUERY advertises
only what that covers.

`process_vm_readv/writev` copy through the editor-root borrow — point the
page-table walker at the target while the kernel stays on its own PML4 — PER
PAGE, because a remote range is contiguous virtually and arbitrary physically.

### YOU COULD `open("/dev/null")` BUT NOT `stat` IT

The synthetic device nodes were built in the OPEN path only, so every other
path syscall asked the VFS, which had never heard of them:

```
open("/dev/null")  -> a working descriptor
stat("/dev/null")  -> ENOENT
```

That silently broke `test -e /dev/null`, `access()`, `ls -l`, and
`base::PathExists`. One table (`g_dev_synth`) now answers "what exists in /dev"
for stat, newfstatat, statx and access.

**THE `statx` ARM IS THE ONE THAT MATTERS**: glibc's `stat()` compiles to statx
on modern builds, so fixing `LX_stat` alone would have left every C program
still seeing ENOENT. That is exactly how slice 104's DRM blind spot worked —
**and its warning comment is three lines above the code that repeated the
mistake.**

statx gets the STATX layout with char-device bits stamped on, NOT
`linux_emit_chrdev_stat`, which writes a `struct lx_stat` — a different shape.
**NOTE: the existing DRM statx arm does exactly that** (fills a 256-byte statx
buffer with a stat-sized record). Left alone because that path is verified
working end-to-end and it was not this slice, but it is a real latent defect.

### tmpfs

`src/tmpfs.c`: writable, mountable anywhere, flat path-keyed table like ramfs
(the VFS resolves by longest-prefix match and has no dentry graph, so a tree of
inodes is a structure nothing else here speaks).

**SIZE-CAPPED, and that is the load-bearing decision.** An unbounded in-memory
filesystem lets any process consume all of RAM — `cat /dev/zero > /tmp/x` is
enough. Per mount, 16 MiB default, `size=` PARSED rather than accepted and
ignored, an unparseable value REFUSED rather than falling back to the default.

`linux-tmpfs` 0xff. bit5/bit6 carry it: ENOSPC at 16711680 bytes on the default
mount and at 65536 on a `size=64k` one — the requested cap, not the default.
Reading a number back from a config file only proves the file remembers it.

Follow-through: the OCI bundle's `/dev` is now APPLIED, and
`logs/lxcontainer.sh`'s assertion was INVERTED (it required exactly one
`NOT APPLIED: /dev`; it now requires zero plus a real mount line). Leaving the
old assertion would have turned a fix into a red gate.

### ptrace WORKS -- and the cause was a RACE, not a mechanism

`programs/linux-ptrace` 0xff. TRACEME, ATTACH/SEIZE, DETACH, PEEK/POKE,
GETREGS/SETREGS, SETOPTIONS, CONT, SYSCALL, KILL, syscall entry/exit stops.
SINGLESTEP, the `PTRACE_EVENT_*` stops, GETSIGINFO and GETREGSET are REFUSED
with EINVAL — a tracer promised a stop that never fires waits forever.

**The bug, which took four attempts:**

```
[fork] parent pid=2 -> child pid=3
[ptrace] pid=3 TRACEME (tracer=2)
[signal] pid=3 stopped by signal 19
<nothing, ever>
```

A tracer forks and calls waitpid IMMEDIATELY, before the child has run
PTRACE_TRACEME. At that instant the child is not traced, so a "is this child
traced?" test is false and the caller commits to a wait that only wakes on
TERMINATION — while the child traces itself and parks. **The condition has to be
re-evaluated AFTER blocking, not before.** `proc_wait_or_ptrace()` does that.

Three dead ends, each a real disproof:

1. wait4 looked only for a ptrace-stop, but strace's opening handshake is
   TRACEME + `raise(SIGSTOP)` — a JOB-CONTROL stop. signal.c's stop site now
   marks a traced process, which is also why ptrace-stops reuse PROC_STOPPED.
2. That fix woke the tracer with a hand-rolled `sched_enqueue()` from the signal
   path and the guest wedged to a dead stop (**heartbeats=0**) — exactly §7's
   warning. The wake now goes through proc.c's own `wakeup_waiters`, the one
   `proc_exit` already uses.
3. Polling instead left the guest live but the tracer still never saw the stop,
   because polling cannot fix a waiter that has already committed to a block.

**A hypothesis CHECKED AND DISPROVED rather than "fixed":** that parking with
`sched_yield()` while holding the BKL was the cause. `sched_yield` captures
`had_bkl` and hands it to `do_switch`, which reacquires on resume — parking
with the BKL held is supported. It had been written into the parked handoff note
as the prime suspect, and it was wrong.

The test is a MINIATURE STRACE, not a list of ptrace() calls: "every request
returned 0" would pass on a kernel where each stop is fabricated and each
register read is zeroes. Measured: `write(fd=4, .., 19)` by number and argument,
`rax=19` at the exit stop, PEEKDATA finding the child's pattern, POKEDATA
landing a value the CHILD reads back.

The test also probed the refusals AFTER `PTRACE_CONT` ran the tracee to exit and
got ESRCH instead of EINVAL — the kernel being right and the test asking at the
wrong time. **A refusal check must run while the thing being refused is
otherwise possible.**

### CHROMIUM'S SANDBOX: SETTLED, and it is NOT a kernel gap

Staging a dummy file at `/opt/chrome/chrome-sandbox` changed the zygote host's
CHECK from `No such file or directory (2)` to `Invalid argument (22)` — which
identifies the missing file exactly. Chromium wants the **setuid sandbox helper
binary**; `chrome-headless-shell` ships none. The full `chrome-linux64`
distribution carries one as `chrome_sandbox` (UNDERSCORE); Chromium looks for
`chrome-sandbox` (HYPHEN) beside the executable, conventionally setuid root.

Three hypotheses were tested and failed first, all recorded so nobody repeats
them: `/dev/null` (fixed, not it), `--disable-setuid-sandbox` (verified present
in the staged binary by grep, not it), `statx` (fixed, not it).

**BLOCKER FOR THE FIX: `ramfs` reports a FIXED 0444 mode for every file**
(`ramfs_open` hardcodes it), so no initrd file can carry a setuid bit or its
real permissions. The helper must live on a tobyfs volume, or be named via
`CHROME_DEVEL_SANDBOX`. That mode limitation is a latent surprise elsewhere too.

`PROBE_SANDBOX=1` in `logs/cwsandbox.sh` stages and removes the dummy. It is a
probe, not a fix.

`logs/cwsandbox.sh` also LIED and is fixed: its checks were "drop happened, no
root refusal, no credentials.cc, no clone refused" — all four true on a run
where the browser never came up, which it duly reported as GREEN. The bootstrap
check is now the deciding one.

`-DPATHFAIL_TRACE` (src/vfs.c) is what made any of this diagnosable: the syscall
ring records path ARGUMENTS, which are user pointers, so a missing file shows up
as `openat a1=29407312 = -2` and names nothing.

### Networking: the device list is in, the control plane is NOT

A namespace held EXACTLY ONE device, which is what makes `ip link add`
unexpressible. Now a list; `net_ns_dev()` still returns `devs[0]` so all six
pre-existing call sites are untouched. `netns_veth_pair_named()` names both ends
and appends them; duplicate names refused. LXVETH 5/5.

**Blast radius measured before refactoring: SIX call sites**, not the sweeping
change "the one slice that can blow up" implies — the same lesson cut 1 already
paid for.

**NEXT, IN THIS ORDER, and the order matters:**

1. **Per-device addressing.** A namespace carries one ip/netmask/gateway, so
   `RTM_NEWADDR` on a second interface has nowhere to put the address. Doing
   handlers first means writing one that lies.
2. **Make the dumps report the REAL device list**, per namespace. `RTM_GETLINK`
   is hardcoded `lo` + `eth0` today. **The initial namespace's reply must stay
   byte-identical** — Chromium's AddressTrackerLinux depends on it, and a link
   that is online must report IFF_UP|IFF_LOWER_UP|IFF_RUNNING and not
   IFF_LOOPBACK or chrome concludes the machine is offline.
3. **Command handlers**: RTM_NEWLINK/NEWADDR/SETLINK/DELLINK + `NLMSG_ERROR`
   ACKs, which `ip` requires. busybox's real `ip` applet IS present in the tree,
   so the test can be a third-party tool rather than hand-rolled netlink.
4. Bridge, then forwarding/NAT.

A dump that still says "lo and eth0" after `ip link add` succeeded would be
worse than no control plane — the trap the cut-2 note already named.

### Method note, recorded because it cost real time

**Bash heredocs ate backslashes SIX times this session**, twice producing NUL
bytes inside C char literals and once a raw CR inside one. Every occurrence was
caught by the compiler, and every one was avoidable: the law is Write/Edit for
anything containing escapes, and it was already written down.

### Gate state

`lxposix --full` **23/23** with `enosys_gaps=0`, `LXNS` **18/18**, `LXVETH`
**5/5**, `LXCONTAINER` **PASS probe=0xff**, defboot 3/3, cross-personality all
PASS, zero faults everywhere.

---

## 23. Slice 12 cut 4 — THE NETWORKING CONTROL PLANE (2026-08-12)

`ip link add` works. rtnetlink parses and executes commands, and the dumps
report what is actually there, per namespace. New: `src/rtnetlink.c`,
`include/tobyos/rtnetlink.h`, `programs/linux-netlink/`, `logs/lxnet.sh`.
Extended: `net_ns.c`, `veth.c`, `socket.c`, `syscall.c`, `net.h`, `nsproxy.h`,
the LXNS harness table.

Steps 1 and 2 of the handoff order are done. **Step 3 (bridge) and step 4
(forwarding/NAT) are NOT**, deliberately — the handoff said a good slice stops
there rather than half-building a bridge on an unproven control plane.

### What it does

- **RTM_GETLINK / RTM_GETADDR report REALITY, per namespace.** A fresh
  `unshare -n` now dumps nothing; it used to answer with the host's hardcoded
  `lo` + `eth0`, which reads perfectly and is a complete fabrication for a
  namespace that has no interface and cannot send a byte.
- **RTM_NEWLINK** (`type veth`, with or without a nested `VETH_INFO_PEER`),
  **RTM_DELLINK** (removes BOTH ends, across namespaces), **RTM_NEWADDR /
  RTM_DELADDR**, **RTM_SETLINK** (admin up/down, and `IFLA_NET_NS_PID` to move
  an interface into another namespace), all with `NLMSG_ERROR` ACKs.
- **An attribute WALKER** (`nla_find`), nested-aware. Only a builder existed.
- **IFF_UP is an ENFORCED admin state**, not a stored flag: `veth_tx` and
  `veth_deliver` both consult it, so a down interface stops carrying frames in
  either direction. **Deviation from Linux, stated rather than hidden: a device
  here is created UP.** Linux creates one DOWN. This kernel has no admin state
  for its own NIC and every pre-existing veth passes frames the instant it
  exists; defaulting to DOWN would have failed LXVETH for a semantic nothing
  else here honours.
- **The SIOC* ioctls** — see below, they are not optional.

### THE INITIAL NAMESPACE'S REPLY, and how "byte-identical" is actually checked

Chromium's `AddressTrackerLinux` and glibc's `check_pf` both read this reply and
both fail silently and catastrophically if it changes. The initial namespace
emits exactly the messages it always did and only then appends anything the
control plane created in it — which is nothing until someone runs `ip link add`.

There was no way to capture a golden dump from the previous build once the
edit was made, so **byte-identity is asserted as exact BYTE LENGTH plus every
field parsed and compared**: 116 bytes / 3 messages for GETLINK (lo index 1 with
`IFF_LOOPBACK`, eth0 index 2 without it, then DONE), 72 bytes / 2 messages for
GETADDR. Those numbers are arithmetic, not observation — 16-byte header, aligned
body, 4-byte attribute headers. The eth0 carrier bits are compared against
`net_is_up()` rather than hardcoded, so a boot where DHCP has not finished
reports honestly instead of failing.

**The initial namespace can now hold control-plane devices at all**, which cut 3
refused. MEASURED BEFORE CHANGING IT: every packet-path caller of
`net_ns_dev()` / `net_ns_ip()` / `net_ns_netmask()` (eth.c:51, net.c:99,
`net_my_ip`, `net_my_mac`, `net_my_netmask`) guards on the RAW namespace pointer
being non-NULL, and the initial namespace IS the NULL pointer — so none of them
ever consults the list for it, and e1000/DHCP/ARP are unaffected by
construction. Without this, the standard container recipe (create the pair on
the host, move one end in) would have been unexpressible.

### THE FINDING: A PERFECT NETLINK SURFACE IS STILL NOT DRIVABLE

busybox's `ip link add name bb0 type veth` worked over netlink on the first
try — real device, real pair, `[veth] created bb0 <-> veth0`. The very next
command died:

```
ip: can't find device 'bb0'
```

`ip addr add ... dev NAME` resolves the name to an INDEX through
`ll_name_to_index()`, which consults a cache only the LISTING commands populate
and otherwise falls straight through to libc's `if_nametoindex()` — **and that
is an ioctl (SIOCGIFINDEX), not netlink at all.**

So a control plane can have a complete, correct netlink surface and still be
unusable by the standard tools. `rtnl_sock_ioctl` now answers SIOCGIFINDEX,
SIOCGIFFLAGS/SIOCSIFFLAGS, SIOCGIFNAME, SIOCGIFADDR, SIOCGIFNETMASK,
SIOCGIFMTU, SIOCGIFHWADDR, SIOCGIFTXQLEN — every value read from the same state
the dumps read, so the two surfaces cannot drift. Unknown requests still get
ENOTTY, so the change is purely additive (an `isatty()` on a socket answers
exactly as before).

**A hand-rolled test would never have found this** — it resolves indices from
its own dump. This is precisely what the busybox-witness pattern is for, and it
is the strongest single argument in this arc for keeping it.

Note `if_nametoindex` opens an **AF_UNIX** socket for that ioctl, so the handler
must not be restricted to AF_INET fds.

### TWO MORE BUGS THE SAME WORK EXPOSED

1. **`sock_deliver` dropped every netlink reply in an empty namespace.** Cut 1's
   inbound gate (`if (!net_ns_has_network(s->net_ns)) return;`) exists to stop a
   frame that arrived on the HOST's NIC reaching a socket in an empty namespace.
   A netlink reply is not an arriving frame — it is the kernel's synchronous
   answer to a request that very socket just made, built strictly from
   `s->net_ns`. Dropping it made an empty namespace unable to observe that it is
   empty, which is how the emptiness is *supposed* to be seen. Netlink is now
   exempt; every real-traffic path is still refused and linux-netns still proves
   it.
2. **The capability check was in the wrong place.** `userns_capable()` reads
   `current_proc()`, and the kernel-side harness calls `rtnl_process` from a
   boot task with an empty capability set — so every command came back EPERM for
   a caller that has no credentials to check. The decision now belongs to the
   syscall boundary (`privileged` argument), which is where the caller's
   credentials exist. The selftest exercises BOTH values, so the refusal is
   tested on a request that would otherwise succeed.

### LIMITS — stated, not implied

- **No bridge, no forwarding, no NAT.** A container reaches its peer end. It
  still cannot reach the internet.
- **A namespace holding TWO interfaces answers ARP for the PRIMARY's address
  only.** `arp_recv` replies when the target IP equals `net_my_ip()`, and that
  is the receiving namespace's `devs[0]` address. Cut 3b made the STORED address
  per-device; the RECEIVE path still has no per-device context because
  `eth_recv` does not carry the device. This is the receive-side counterpart of
  cut 3b and it is the next thing to fix if a bridge lands. It is why the
  kernel selftest moves the peer end into its own namespace before the frame
  test — one end per namespace is also the real container topology.
- **`ip link add ... type X` for any X other than `veth` is EOPNOTSUPP**, by
  name. An empty shell that forwards nothing would be the half-answer this file
  exists to avoid.
- **AF_INET only** for addresses; AF_INET6 is refused by family rather than
  accepted and dropped.
- **A NON-initial namespace reports no `lo`.** Linux always creates one. This
  stack has no loopback anywhere (cut 1's founding measurement), so reporting
  one would be an interface that cannot carry a packet.
- **One reply DATAGRAM per request.** Linux splits large dumps; with at most 8
  devices per namespace the whole answer is under 1 KiB against a 3 KiB buffer,
  and the builder refuses to emit a partial message.
- **`ip addr add ... dev eth0` on the host is EOPNOTSUPP**, not ENODEV: lo and
  eth0 are net.c's, they exist and cannot be commanded from here, and "no such
  device" about an interface `ip link show` just listed sends the reader hunting
  the wrong bug.
- **No `ip link set ... mtu` / `address` / rename**: refused with EOPNOTSUPP
  rather than accepted and ignored.
- **The ARP cache is still global**, as cut 2 said.
- Veth ends ARE now reclaimed when a namespace dies (`netns_veth_release`,
  called from `net_ns_put`); the peer is unpaired rather than deleted, because
  it may live in a namespace that is still running — which is what Linux
  reports for the surviving half. Pool raised 8 -> 16 pairs.

### Method notes this slice paid for

- **A SKIP MESSAGE THAT NAMED THE WRONG CAUSE COST A FULL GATE RUN.** The LXNS
  harness printed `SKIP busybox ip link add (no /bin/busybox)` for a busybox
  that was plainly present and had just run five other cases. `proc_spawn` also
  refuses any argv entry over **ABI_ARG_MAX (512 bytes, proc.c:436)**, and the
  witness script had grown past it. The message now reports the rc, the actual
  length and the cap. **A diagnostic that can only say one thing will say it
  when it is wrong.**
- **The DHCP health check matched nothing on a boot where DHCP had plainly
  succeeded.** The log line is column-aligned — `[dhcp] ACK    bound:` — and the
  pattern assumed one space. It reported a network regression that had not
  happened. Gate patterns must be checked against a real log, not written from
  memory of the format.
- **The build-marker gate fired correctly and for a reason I did not expect**:
  not a stale build, but a marker string I had edited in the source without
  updating the gate. It now prints WHICH marker is missing and says both causes
  out loud. The check earned its place either way — it stopped a run that would
  have tested the previous binary.
- **Heredocs ate backslashes again**, this time in a throwaway python one-liner
  whose regex character class arrived broken. The law is Write/Edit for anything
  containing escapes, and it applies to scratch scripts too.
- **Editing sources while a gate build is running invalidates the run.** One run
  was killed for this rather than trusted. `make` also rebuilds essentially
  everything when the Makefile itself is touched, which is easy to forget when
  adding a source file.

### Gate state

`bash logs/lxnet.sh` — one command, three independent layers in one boot:
**LXNETLINK 7/7** (kernel-side: the initial namespace's exact reply, and whether
a netlink-created pair really carries a frame), **LXVETH 6/6** (regression: the
enforced admin state sits in every frame path now), **LXNS 20/20 skipped=0**
(userspace: `/bin/linux-netlink` BITS=0xff over a real socket, and a busybox
`ip` witness measuring `n=2 a=1 e=0 z=1 d=1 u=2`).

---

## 24. Slice 12 cut 5 — the IP LAYER becomes namespace-aware (2026-08-12)

Cut 5 is what had to happen before forwarding or NAT could mean anything, and
it turned out to be mostly BUG FIXES rather than new machinery. Extended:
`net_ns.c`, `net.c`, `eth.c`, `ip.c`, `udp.c`, `tcp.c`, `socket.c`, `veth.c`,
`nsproxy.h`, `net.h`, `eth.h`, plus one new subtest in `rtnl_selftest`.

### THE HEADLINE: a container's IP packets carried the HOST's source address

```c
h->src_ip = g_my_ip;            /* ip_send_frame, unconditionally */
```

Cut 2 made the ethernet header's source MAC namespace-relative, then had to fix
the ARP payload's sender MAC after the first fix "looked complete and was not"
(§18's own words). **This is the same finding one layer further up**, and it
went unnoticed for the same reason the ARP one nearly did: nothing had ever
sent IP — as opposed to ARP — from inside a namespace, so no test could see it.
LXVETH does an ARP round trip; linux-netns asserts ENETUNREACH; neither puts a
datagram on the wire.

The rule cut 2 wrote down was *"when making a stack namespace-aware, grep for
the global at EVERY layer."* The census that starts this cut found five more:
`ip_send_frame`'s source address, `ip_send`'s next-hop decision, the UDP
pseudo-header (send AND verify), and the TCP pseudo-header (send AND verify).
All five are now `net_my_ip()`.

### THE SECOND BUG: NULL WAS AMBIGUOUS, SO THE HOST GUESSED

Cut 2 stored the receive context as a bare `void *ns` where NULL meant "no
context" — and NULL is ALSO the initial namespace. So there was no way to say
"this frame is the host's", and the accessors fell through to
`current_proc()->net_ns`.

That fallback is a GUESS, and on the host receive path it is a wrong one:
**e1000's rx drain runs from its IRQ handler and from `net_poll()`, which
`poll`/`select`/`recvmsg` call.** A process inside a container calling `poll()`
therefore drained the HOST's NIC while `net_my_ip()` answered with the
CONTAINER's address — and `ip_dst_is_for_us()` dropped the host's own packets.
Latent since cut 2; nothing had a namespaced process polling under host traffic
until containers arrived.

The context now carries an explicit `active` flag and the receiving DEVICE.
`eth_recv()` — which all five hardware drivers call — brackets it as "the
initial namespace, said out loud", so **the fix is one function and no driver
was touched.**

### THE THIRD BUG: RETRANSMITS BELONG TO THE CONNECTION, NOT THE POLLER

`tcp_tick_all()` runs from `tcp_poll_until()`, so a container process sitting in
`poll()` drives retransmits for EVERY connection including the host's. Reading
`current_proc()` at send time would have stamped the container's address on the
host's segments and computed the checksum over it — i.e. moving the send path
onto `net_my_ip()` would have INTRODUCED a bug where none existed.

`struct tcp_conn` (local to tcp.c, so the field costs nothing) now latches
`net_ns` at `conn_alloc`, and `tcp_emit` — the ONE place a connection puts a
segment on the wire — brackets with it. One bracket covers the source address,
the pseudo-header and the route decision together. UDP has no connection object,
so `sock_sendto` brackets with the SOCKET's namespace instead: the same rule cut
1 chose for the ENETUNREACH gate, and for the same reason.

### THE COMPILER CAUGHT AN UNFILLED FIELD, AND IT WOULD HAVE FAILED SILENTLY

`tcp_conn.net_ns` was added and `tcp_ctx_ns()` was written to fill it, and the
assignment in `conn_alloc` was never made. **0 is the initial namespace**, so
the missing assignment would have read as "every connection belongs to the
host" — no crash, no error, exactly the previous behaviour, and the container
half silently doing nothing. What surfaced it was
`-Wunused-function: 'tcp_ctx_ns'`. A field whose "unset" value is a legitimate
value cannot be caught by testing the common path.

### The resolver, and why the order is the design

```
1. an ACTIVE context wins        -- something that KNOWS said so
   (within it, the DEVICE's address beats the namespace's primary)
2. else the calling process's namespace   -- right for a syscall-driven send
3. else the host's globals
```

(1) existing at all is the cut-5 fix. It also removed the limitation §23
recorded as "the next thing to fix": a namespace with two interfaces answers
ARP for its PRIMARY's address only. The context carries the device now, so each
interface answers for its own — and `eth_send` replies out the interface the
frame arrived on rather than the namespace's primary.

### Limits — stated, not implied

- **There is still NO ROUTE TABLE.** Routing is "my subnet, else my gateway",
  now asked per-namespace. Forwarding and NAT need more than this and the next
  reader should not mistake it for a route lookup. An off-subnet send with no
  gateway configured now returns false instead of ARPing for 0.0.0.0.
- **The UDP/TCP port lookup is GLOBAL** (`sock_lookup_by_port`), so two
  namespaces cannot both bind the same port. Not fixed, and it is the next
  thing a real container workload will hit.
- **The ARP cache is still global** (cut 2's note, unchanged).
- **No bridge, no forwarding, no NAT.** A container reaches its peer end with
  real IP now. It still cannot reach the internet.
- A container that makes the kernel resolve a name (the in-kernel DNS/HTTP path,
  not sockets) now does so from its OWN namespace and therefore fails, where
  before it silently used the host's network. That is correct-but-failing
  replacing incorrect-but-working, and it is a deliberate consequence.

### Method note: a gate that reported RED, and the RED was mine

`validate.sh 3 120` came back **RED — run 2 DEAD, heartbeats=0**, with runs 1
and 3 both alive at 31 heartbeats on the same ISO. Run 2's log stopped at 12 ms
mid-boot, at a clean line boundary, with no fault and no panic — the shape of a
process that was KILLED, not one that hung.

Cause: `logs/realcurl.sh` ends with `taskkill //IM qemu-system-x86_64.exe //F`,
and I had started the defboot run while realcurl was still finishing. Its
cleanup killed my run 2.

**A gate script that kills QEMU BY IMAGE NAME cannot be overlapped with any
other QEMU run.** The existing law ("check `tasklist | grep qemu` and
`taskkill` before believing a run") is about stale processes *before* a run;
this is its mirror image — another script's cleanup arriving *during* one.

It was re-run with nothing else in flight and came back **GREEN 3/3, 32/32/33
heartbeats**. The hypothesis was checked rather than assumed, which is the only
reason it is recorded as measurement error instead of shipped as a known-flaky
boot.

### Gate state

`bash logs/lxnet.sh` GREEN: **LXNETLINK 8/8**, LXVETH 6/6, LXNS 20/20
skipped=0. `lxposix --full` **23/23** `enosys_gaps=0`. defboot **3/3 alive, 0
faults**. **REALCURL PASS** —
unmodified static curl 8.20.0 fetching `http://example.com/` over e1000+SLIRP,
`http_code=200 size=559`, which is the regression check that matters most here:
it exercises DNS (UDP checksums both ways), the TCP handshake, `tcp_emit`'s new
bracket, and the off-subnet gateway path through `net_my_gateway()` — all of
them for the HOST, on code that every packet now passes through.

The new subtest is the one that carries this cut —
`UDP crossed the boundary and carries the SENDER's address (10.55.0.1), not the
host's`. It fails in two independent ways on a partial fix: with only the
source address fixed the checksum still mismatches and the datagram is dropped,
so "nothing arrived" and "arrived with the wrong source" are distinguishable.

---

## 25. Slice 12 cut 6 — ports, ROUTES, BRIDGE, and NAT (2026-08-12)

**A container can reach the internet.** New: `src/bridge.c`, `src/nat.c`.
Extended: `net_ns.c`, `net.c`, `ip.c`, `socket.c`, `tcp.c`, `veth.c`,
`rtnetlink.c`, `nsproxy.h`, `net.h`, `ip.h`, the LXNS table. Four pieces, each
gated before the next was started.

### 1. THE PORT SPACE WAS GLOBAL — a cross-namespace data leak

`sock_lookup_by_port` scanned the whole pool by port alone, so **a datagram
arriving in one namespace was delivered to a socket bound in another** (first
port match wins), and two namespaces could not both bind a port. The same was
true of tcp.c's `conn_lookup`, `listen_lookup` and `port_in_use`.

Delivery asks `net_current_ns()` (the receive context); a BIND asks the
SOCKET's own namespace, which is what lets two namespaces hold one port and is
also the namespace the later delivery will match against.

The subtest asserts BOTH halves: both binds succeed AND the count on the wrong
socket is zero. Asserting only the first would pass on a kernel that stopped
checking collisions and then delivered to whichever socket it found first.

### 2. A ROUTE TABLE, consulted first, old logic as the fallback

Routing was "my subnet, else my gateway" — no way to say "10.99.0.0/24 is
through THIS interface", which is exactly what a reply travelling back down to a
container needs. Per-namespace table, longest prefix wins, and **a matching
route names the OUTPUT INTERFACE** — the thing this stack could not previously
say at all.

Additive by construction: an empty table takes the identical old path, so eth0
and DHCP are untouched. A connected route appears automatically from
`ip addr add`, as on Linux. `RTM_NEWROUTE/DELROUTE/GETROUTE` are wired, and
`ip route add default via X` (no `dev`) resolves its interface from the route to
the GATEWAY, which is the form every real tool sends.

Two details that would each have produced a silently wrong table: a DEFAULT
route must carry **no RTA_DST** (or `ip` prints `0.0.0.0/0` and some parsers
reject it), and `rtm_family` arrives as **AF_UNSPEC** from busybox, so refusing
anything but AF_INET would fail the most ordinary route command there is.

### 3. A BRIDGE that LEARNS

`struct net_dev`'s vtable fit unchanged for the third time in this slice — a
bridge is a device whose `tx` goes to its ports. Enslavement lives in the
namespace's interface record (`master`), not in the device, because a device can
move between namespaces and its master cannot follow it.

**It learns source MACs.** Flooding everything is functionally correct — the
wrong container discards it — and is also a way to leak every container's
traffic to every other container. The subtest checks the unicast counter for
exactly that reason: it is what separates a bridge from a hub, and the ARP
assertion alone would pass on a hub.

**The loop guard is not optional.** A port's `tx` is a veth end that may deliver
to another port. Attaching both ends of one pair to one bridge — two ordinary
`ip link set` commands — would recurse until the stack is gone. There is no
spanning tree here, so a depth counter is the honest defence.

### 4. FORWARDING + MASQUERADE

The mapping table is the whole mechanism: on the way out rewrite the source
address to the uplink's and substitute a port; on the way back look the port up
and put the original address and port back.

**The ORDER in ip_recv is the load-bearing part.** A reply to a masqueraded
connection is addressed to the HOST'S OWN address — that is what masquerading
means — so `ip_dst_is_for_us` is true for it, and a local-delivery-first stack
hands a container's segment to the host's socket layer. The NAT table is
therefore consulted BEFORE local delivery, and only a packet matching a live
mapping is taken.

The reverse lookup matches on the peer address as well as the translated port,
so a host on the internet cannot inject into a container by guessing a port.

Limits, stated: **masquerade only** (no DNAT, no inbound port forwarding, no
filtering — there is no iptables here); **enabling it is a kernel call**,
because this kernel has no `/proc/sys` at all, so a userspace runtime cannot
turn forwarding on; **fragments are dropped rather than forwarded untranslated**
(only the first carries the ports a mapping is keyed on, and forwarding the rest
with the container's source address would leak an unroutable address onto the
wire); TCP state is not tracked, so a mapping expires on a timer.

### The test that carries this cut, and why it checks both ends

"The internet" is a third namespace behind an uplink veth, so the round trip is
deterministic and needs no real network:

```
world saw 192.168.50.1:50000 (NOT the container's 10.77.0.2),
and the reply came back to the container from 192.168.50.2  [out=1 in=1 maps=1]
```

Asserting only "it arrived" would pass on a stack that forwarded the
container's unroutable source address straight onto the wire — which works
perfectly on this fake wire and vanishes on a real one. Asserting only the
outbound half would miss the mapping table entirely, which is the half that
exists so the reply is not eaten by the host's own socket layer.

The ARP caches are warmed first, deliberately: a forwarded packet has no sender
to retry on its behalf, so the first datagram to a cold next hop is legitimately
dropped. Testing that as a failure would be testing ARP, not NAT.

### A TEST CAUGHT ITS OWN OBSOLESCENCE, which is the system working

`/bin/linux-netlink` bit5 asserted `ip link add type bridge` -> EOPNOTSUPP. The
moment bridges landed the gate went RED with `type-bridge=0` against a wanted
95. That is not a false alarm — it is an assertion noticing that a documented
refusal had become a supported feature. It now checks the opposite (bridge
SUCCEEDS and is listed) and uses `vlan` for the kind this kernel genuinely does
not have. **Keeping both in one bit is deliberate:** a kernel that ignored
IFLA_INFO_KIND entirely fails one of them whichever way it guessed.

### Method note

The `until grep ... logs/lxnet.log` loop I used to wait for a verdict read the
PREVIOUS run's log — the gate deletes it only just before starting QEMU, so for
the first minute the old one is still there and still says PASS. The handoff's
"never reuse a log filename across runs" law is about exactly this, and the
reuse is inside the gate script itself. Wait on the RUNNER, not on the log.

### A GATE THAT FAILED SILENTLY BY PRODUCING NO OUTPUT AT ALL

`logs/realcurl.sh` came back with its REALCURL section EMPTY -- no verdict line,
no error, nothing between the header and the fault check. The log showed why:

```
limine: Loading module `boot():/boot/initrd.tar`...
PANIC: High memory allocator: Out of memory
```

A **BOOTLOADER** panic loading an ~826 MB initrd into a guest the script starts
with `-m 512`. The kernel never ran, so nothing it could have printed appeared,
and the script's "grep for the verdict" check reported that as an absence rather
than a failure.

`validate.sh` already carries this exact fix and a comment explaining it (`-m
512` "was too small to boot the current ISO at all... which is how a bootloader
panic came to be reported as a healthy boot"). realcurl.sh never got it. Raised
to 5120 to match every other gate; it then passed with
`http_code=200 size=559`.

**Note honestly: this same script passed at `-m 512` earlier the same day, and
that is unexplained.** 826 MB cannot fit in 512 MB, so the earlier pass is the
anomaly, not this failure. It is recorded as unexplained rather than given a
tidy story.

### Gate state

`bash logs/lxnet.sh` GREEN: **LXNETLINK 12/12**, LXVETH 6/6, **LXNS 21/21
skipped=0** (`/bin/linux-netlink` 0xff, `[NSIP] n=2 a=1 e=0 z=1 d=1 u=2`,
`[NSRT] conn=1 def=1`). `lxposix --full` **23/23** `enosys_gaps=0`. **REALCURL
PASS**. defboot **3/3**.
