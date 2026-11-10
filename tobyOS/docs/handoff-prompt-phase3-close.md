# Handoff prompt — closing out Linux-completeness Phase 3

Paste the block below to the next agent. Everything it references is in the tree.

---

You are continuing work on **tobyOS** (`c:\CustomOS`), a from-scratch OS. The
Linux-completeness arc (Phase 3, slices 8–16) is essentially done; your job is to
close out the four items it left open.

## Read these first, in this order

1. `tobyOS/docs/linux-arc-handoff-phase3.md` — the arc's full record. **§18, §19 and
   §20 are the ones you need**: slice 12 cut 2, slice 16's controllers, and slice 14.
   Two warnings about reading it:
   - **§16 ("Slice 14 — BLOCKED") is SUPERSEDED by §20.** Slice 14 is implemented.
     Do not act on §16's "needs a decision" framing; the decision was taken (gate it,
     default off) and the work is done.
   - §20 ends with two correction subsections that **disprove a diagnosis written
     earlier in §20 itself** ("THE SANDBOX RUN WAS MADE AFTER ALL…"). Read to the end
     of §20 before believing anything in its middle.
2. `~/.claude/plans/sunny-crafting-charm.md` — the 16-slice plan, for slice 16's
   capstone spec and the standing verification table.
3. The memory files for this arc (they are indexed in `MEMORY.md`):
   `linux-namespaces-slice8`, `linux-mount-namespace-slice9`,
   `linux-pid-namespace-slice10`, `linux-user-namespace-slice11`,
   `linux-net-namespace-slice12`, `linux-seccomp-slice13`, `linux-cgroup-v2-slice15`,
   `linux-cgroup-mem-io-slice16`, `linux-slice14-native-setuid`.
4. **Before touching anything Chromium, read `chromium-bringup` in memory.** That arc
   has its own hard laws and a closed Tier 3 (GPU) that must not be reopened.

## The governing thesis (standing directive)

tobyOS is ONE environment running BOTH Linux and Windows software. **Track C (Win32/PE)
is co-equal and deliberate.** Never break a personality boundary, and never advise
dropping Track C. `XPIPE` / `X2PIPE` / `THREEWORLDS` exist to enforce this.

## Non-negotiable working rules

- **Gates.** `bash logs/lxposix.sh --full` before you start and after every change —
  exit status is the result. Run the cross-personality gates (`XPIPE_BOOT`, `X2_BOOT`,
  `THREEWORLDS_BOOT`) and `defboot` (`logs/validate.sh`) after anything touching exec,
  paths, signals or the scheduler.
- **Build environment.** `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"`,
  and carry a Windows-native TMP on the compiler vars:
  `make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" ...`
- **`EXTRA_CFLAGS` reaches KERNEL objects only. User programs need
  `PROG_EXTRA_CFLAGS`.** The Makefile says so at `LIBTOBY_KABI_PROGRAM_RULES` and
  records that this exact mistake once made `-DCHROME_FULL` silently miss chromewin.
  I made it again in slice 14 and only the binary check caught it.
- **`EXTRA_CFLAGS` changes rebuild NOTHING** (no flag hash). `touch src/kernel.c`, or
  `rm -f src/*.o` on a flavour switch — objects ARE in `src/`. Slice 16 proved why: a
  stale `pmm.o` built with a since-dropped `-D` failed the link with
  `undefined symbol: cgmem_trace_alloc`. A link error is the lucky outcome.
- **`struct proc` grows ⇒ `make clean`.** There is no header dependency tracking.
- **Never hide a build behind a filter.** A grep swallowed `make: command not found`
  and I tested an ISO from an earlier build. Gate on the BINARY instead:
  `python -c "d=open('tobyos.bin','rb').read(); raise SystemExit(0 if (b'<new>' in d and b'<old>' not in d) else 1)"`
  — asserting the OLD marker is *gone* is what separates "rebuilt" from "the previous
  flavour happened to contain this string".
- **Bash heredocs eat backslashes.** `"\n"` arrives as a real newline and produces
  unterminated C literals. It cost me five separate failures. Use Write/Edit, or write
  the script to a file first.
- **Harnesses lie.** Never let a success path run through a command that can succeed on
  its own. When asserting "X's copy ran", compare something only X could produce. And
  **assert the VALUE, not merely that something succeeded** — the arc's three best
  findings were all a green test with a wrong number: ARP resolving to the host NIC's
  MAC, `chown` refusing with `ENOMEM`, `memory.current` matching by accident.
- **Measurement hygiene, learned the hard way.** `TaskStop` does **not** kill a
  runner's QEMU child — check `tasklist | grep qemu` and `taskkill //F` before
  believing a run. Never reuse a log filename across runs: two QEMUs writing one file
  looked exactly like a flaky kernel and I nearly documented it as one. Check log
  timestamps before reading a result; I twice nearly reported stale files as passes.
- **Report honestly.** Partial is fine, overclaiming is not. If a step fails, say which.

## Tree state

Branch `feat/audio-output`, **two commits ahead and not pushed** (remote is
`github.com/tobyish111/tobyOS.git`, and this branch has no upstream set):

- `3f8e670` — the whole arc (slices 8–16 controllers + slice 14). Also carries the
  in-progress audio work, which was interleaved in `Makefile`/`abi.h`/shared kernel
  files; splitting it would have produced a commit that does not build.
- `201fbb5` — `programs/linux-clonens/` plus two corrected comments.

Current gate state: `lxposix --full` GREEN 23/23 (zero ENOSYS gaps), `LXNS` **14/14**,
`linux-cgroup2` `0xff`, `nsetuid` `0x3f`, `linux-clonens` `0xff`, cross-personality all
PASS, `defboot` 3/3 alive, zero faults.

## Your four tasks, in priority order

### 1. The Chromium sandbox blocker (highest value, and the diagnosis is half done)

Slice 14 works: `chromewin` drops to uid 1000 before `execve`, Chromium **accepts a
non-root launch** (its root refusal never fires), and its sandbox engages. Then:

```text
FATAL:sandbox/linux/services/credentials.cc:309] Check failed: . : No child processes (10)
```

Its `clone(CLONE_NEWUSER|SIGCHLD)` child faults before executing a single syscall
(`cr2=0xffffffffffffffd8` = `-0x28`, `rip=0x0000000003716f29`, `cpu=0ms`,
`syscalls=0`), so `wait4` finds nothing to reap.

**Do not re-investigate the namespace path.** `programs/linux-clonens` already proves
`clone` with every `CLONE_NEW*` flag works — as root and as uid 1000, single- and
multi-threaded, with the child doing real work (deep recursion, libc formatting, TLS
reads). 8/8, zero faults. The Linux-arc surface is eliminated.

What is left is **CoW-forking chrome's own address space** (327 VMAs, libraries packed
from `0x100000000000`, V8 cages) through `vmm_cow_fork` — already known-fragile: the
`CHROMIUM_BOOT` path caps chrome CoW forks at 16 and notes that only "STW
tg_vm_quiesce + eager CoW make limited chrome CoW forks viable". Two probes:

- Map `rip=0x0000000003716f29` to a function in `chrome-headless-shell` (compute the
  offset from the ELF LOAD base — that arc's law).
- The `[fork] clone flags=` and `allow chrome CoW fork` instruments exist but are
  `CHROMIUM_BOOT`-gated, so they were compiled out of the `TKAPP_CHROMEWIN` build that
  failed. Enabling them for a chromewin build is the cheapest next step, and will show
  whether a *plain* CoW fork of chrome faults identically (which would make the
  namespace flag incidental).

Reproduce with: kernel `EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTKAPP_BOOT
-DTKAPP_CHROMEWIN"`, program `PROG_EXTRA_CFLAGS="-DCW_SANDBOX"`, force
`rm -f programs/chromewin/chromewin.o programs/chromewin/chromewin.elf` and
`rm -f build/initrd.tar build/base.iso tobyOS.iso`, boot `-m 4096 -cpu
qemu64,+smep,+smap`. **Gate on the staged `build/initrd/bin/chromewin` containing the
drop code and NOT `--no-sandbox`, and on the kernel referencing `/bin/chromewin`.**
My first attempt used `-DCHROMIUM_BOOT`, which has its own in-kernel launcher that
never goes through chromewin — a whole boot testing nothing.

### 2. Slice 16's capstone — the last unbuilt deliverable

An OCI runtime over the in-tree Alpine 3.19 rootfs plus an `LXCONTAINER_BOOT` gate.
The rootfs is already staged into the initrd as `/alpine.tar.gz`, and
`programs/linux-alpinerun/` (slice 7) already unpacks and chroots into it.

**Drive the namespaces directly, not via busybox `unshare -f`.** A pre-existing bug
blocks it: `sys_fork_share` gives the child a private stack, so busybox's
`xvfork(); xexecvp()` jumps through a NULL argv (`rip=0`, `err=0x14`). Do **not** fix
that as a side effect — share-until-exec is load-bearing for the Chromium arc.

All the pieces it needs exist and are verified: mount namespaces + `pivot_root`, pid
namespaces, user namespaces, net namespaces (veth pairs cross a boundary), seccomp,
and cgroup v2 with `pids`/`cpu`/`memory`/`io`.

### 3. The hardware validation the plan requires

`CW_SANDBOX` defaults **off** by the user's explicit decision, and the plan requires an
EliteDesk 800 G1 run before the default may flip. Every QEMU boot here auto-logs-in as
root. The five-step checklist is in **§20** of the handoff doc — including that
Chromium must survive past ~5 s, that a renderer process must actually appear, and
that `/data` must be writable as uid 1000 (slice 120 chmods it 0777 at mount, which is
**unverified** for a non-root chrome, and slice 120 is itself the bug that ate a
real-HW session). Slice 7 owes a non-root validation on the same machine.

### 4. Housekeeping (ask the user before any of it)

- The two commits are **unpushed**.
- `programs/alpine/alpine-minirootfs-3.19.0-x86_64.tar.gz` (3.2 MB) is untracked but
  the Makefile needs it for slice 7. The repo's convention for third-party binaries is
  fetch-not-commit (see the busybox entries in `.gitignore` and `programs/alsa/fetch.sh`),
  so an `alpine/fetch.sh` + `.gitignore` entry is probably right — but it is the user's
  call whether to vendor it instead.
- **`tobyOS/tobyOS/` is a stray 2.1 GB directory inside the repo** and looks
  accidental. `.glibc-tc/` (670 MB) and four disk images are also untracked. None of it
  is gitignored, so **never `git add -A`** in this tree.

## Things that are deliberate — do not "fix" them

- **No `io.max`.** This kernel cannot throttle block I/O, and a knob that accepts a
  limit and enforces nothing is the failure mode this arc kept finding.
- **`cow_copy_page` is deliberately uncharged.** Breaking CoW is charge-neutral;
  charging it leaks one page per break (caught only by the no-drift bit).
- **`cpu.weight` selects a priority band**, not a proportional share, and `PRIO_RT` is
  deliberately unreachable from it.
- **Net namespaces have no netlink control plane and no bridge**, and the ARP cache is
  still global. Stated in `§18`, not forgotten.
- **`copy_user_pages` in `fork.c` is dead code.** `sys_fork` uses `vmm_cow_fork`. Two
  slice-16 comments cited the dead function's "FULL copy" note as justification and
  have been corrected; don't re-derive the wrong conclusion from it.
