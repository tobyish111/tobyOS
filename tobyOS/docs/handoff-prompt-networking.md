# Handoff prompt — the networking control plane, and what else is open

Paste the block below to the next agent. Everything it references is in the tree.

---

You are continuing work on **tobyOS** (`c:\CustomOS`), a from-scratch OS whose
Linux personality now runs a real Alpine userland, unmodified Chromium, and OCI
containers. Phase 3 (namespaces → containers) is **complete and gated**. Your
main job is the one subsystem it deliberately left unbuilt: **the networking
control plane**.

## Read these first, in this order

1. `tobyOS/docs/linux-arc-handoff-phase3.md` — the arc's full record.
   **§21 and §22 are the ones you need.** §22 is the most recent and states the
   networking order and why it matters. Two warnings about reading the rest:
   - **§16 is SUPERSEDED by §20**, and **§20's middle is RETRACTED by §21.**
     §20 concluded the Chromium sandbox blocker was CoW-forking chrome's address
     space. It was not — it was four ordinary `clone(2)` bugs. Read to the end
     of each section before believing anything in its middle.
   - §18's "no control plane" note is the thing you are now fixing.
2. `~/.claude/plans/sunny-crafting-charm.md` — the 16-slice plan, for the
   standing verification table.
3. Memory (indexed in `MEMORY.md`): `linux-net-namespace-slice12`,
   `linux-clone-childstack`, `linux-ptrace-slice`,
   `linux-devnode-stat-asymmetry`, `linux-oci-capstone-slice16`,
   `cross-personality-pipeline-x1` (the thesis), `tobyos-build-env`.
4. **Before touching anything Chromium, read `chromium-bringup` in memory.**
   That arc has its own hard laws and a closed Tier 3 (GPU) that must not be
   reopened.

## The governing thesis (standing directive)

tobyOS is ONE environment running BOTH Linux and Windows software. **Track C
(Win32/PE) is co-equal and deliberate.** Never break a personality boundary,
never advise dropping Track C. `XPIPE` / `X2PIPE` / `THREEWORLDS` enforce this.

## Non-negotiable working rules

- **Gates.** `bash logs/lxposix.sh --full` before you start and after every
  change — exit status IS the result. `bash logs/lxns.sh` for namespaces
  (asserts a subtest COUNT; raise `WANT_SUBTESTS` when you add one). Run the
  cross-personality gates (`XPIPE_BOOT`, `X2_BOOT`, `THREEWORLDS_BOOT`) and
  `defboot` (`bash logs/validate.sh 3 120 <prefix>` — the prefix must NOT
  include `logs/`) after anything touching exec, paths, signals or the
  scheduler.
- **Build environment.** `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"`,
  and carry a Windows-native TMP on the compiler vars:
  `make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" ...`
- **`EXTRA_CFLAGS` reaches KERNEL objects only. User programs need
  `PROG_EXTRA_CFLAGS`.**
- **`EXTRA_CFLAGS` changes rebuild NOTHING.** `touch src/kernel.c`, or
  `rm -f src/*.o` on a flavour switch — objects ARE in `src/`. A stale `vfs.o`
  carried `-DPATHFAIL_TRACE` into two unrelated gate runs this session.
- **`struct proc` grows ⇒ `make clean`.** No header dependency tracking.
- **Never hide a build behind a filter.** Gate on the BINARY, and assert the
  OLD marker is GONE — that is what separates "rebuilt" from "the previous
  flavour happened to contain this string".
- **USE Write/Edit FOR ANYTHING CONTAINING ESCAPES.** Bash heredocs eat
  backslashes. This cost me SIX separate compile failures in one session, twice
  producing NUL bytes inside C char literals and once a raw CR. The law was
  already written down and I kept reaching for heredocs anyway. Write the
  script to a file, or use the Edit tool.

## Method laws this session paid for

- **A GREP IS NOT A CENSUS.** Counting the syscall surface with a regex gave
  204 (too low: `case N:` followed by `{` was invisible) and then 218 (too high:
  it matched `lx_scname`'s ~90-entry NAME TABLE as if those were handlers).
  Both numbers were reported to the user before being caught. Scope any such
  scan to the dispatcher's own body by brace matching. **If a spot-check says a
  syscall is missing and you remember implementing it, suspect the spot-check.**
- **A GATE THAT CANNOT SEE A MISSING TEST IS NOT A GATE.** `logs/lxns.sh`
  asserted `VERDICT: PASS` only, so a subtest whose binary failed to stage
  simply did not run and the verdict stayed PASS — it reported GREEN 17/17,
  identical to before the test existed. It now checks the count. Apply the same
  suspicion to any gate you add: what does it look like when the thing under
  test is ABSENT rather than broken?
- **A REFUSAL CHECK MUST RUN WHILE THE THING BEING REFUSED IS POSSIBLE.** The
  ptrace test probed an unimplemented request AFTER the tracee had exited and
  got ESRCH instead of EINVAL — the kernel being right and the test asking at
  the wrong time.
- **CHECK A HYPOTHESIS BEFORE FIXING IT.** I parked ptrace with a note naming
  the BKL as prime suspect. One grep disproved it (`sched_yield` captures
  `had_bkl` and `do_switch` reacquires). "Fixing" it would have cost a boot and
  changed nothing.
- **INSTRUMENT, DO NOT GUESS.** Three 20-minute boots went into guessing what
  file Chromium was missing. `-DPATHFAIL_TRACE` (src/vfs.c) named it in one —
  the syscall ring records path ARGUMENTS, which are user pointers, so a missing
  file reads as `openat a1=29407312 = -2` and names nothing.
- **Measurement hygiene.** `TaskStop` does not kill a runner's QEMU: check
  `tasklist | grep qemu` and `taskkill //F` before believing a run. Never reuse
  a log filename across runs.

## Tree state

Branch `feat/audio-output`, **ten commits ahead and NOT pushed** (remote is
`github.com/tobyish111/tobyOS.git`; this branch has no upstream set). The user
has approved committing locally and NOT pushing — ask before that changes.

Gates as handed over: `lxposix --full` **23/23** (`enosys_gaps=0`), `LXNS`
**18/18**, `LXVETH` **6/6**, `LXCONTAINER` **PASS probe=0xff**, defboot 3/3,
cross-personality all PASS, zero faults. Syscall surface: **212** numbers
reachable (208 of the 0..334 core range).

**`tobyOS/tobyOS/` is a stray 2.1 GB directory inside the repo.** The user has
been asked and chose to leave it. **Never `git add -A` in this tree.**

## YOUR MAIN TASK: the networking control plane

`ip link add` cannot work today. rtnetlink here answers `RTM_GETLINK` /
`RTM_GETADDR` dumps with a **hardcoded lo + eth0** and accepts no commands. Two
prerequisites are already done and gated:

- **Namespaces hold a device LIST** (`net_ns_add_dev` / `net_ns_find_dev` /
  `net_ns_dev_at` / `net_ns_dev_count`). `net_ns_dev()` is still `devs[0]`, the
  primary, so all six pre-existing call sites are unchanged.
- **Addressing is per-DEVICE** (`net_ns_set_dev_addr` / `net_ns_dev_ip`). The
  per-namespace accessors answer for the primary.

`netns_veth_pair_named(ns_a, name_a, ip_a, ns_b, name_b, ip_b, mask)` creates a
pair with chosen names and appends both; duplicate names are refused.

### Do it in this order. The order is the point.

1. **Make the dumps report REALITY**, per namespace. `sock_netlink_send` in
   `src/socket.c` is the whole rtnetlink implementation (~200 lines, the
   builders `nlb_msg_begin` / `nlb_attr` / `nlb_msg_end` are already there).

   **THE INITIAL NAMESPACE'S REPLY MUST STAY BYTE-IDENTICAL.** Chromium's
   `net::AddressTrackerLinux` reads it, and a link that is online must report
   `IFF_UP|IFF_LOWER_UP|IFF_RUNNING` and must NOT be `IFF_LOOPBACK`, or chrome
   concludes the machine is offline. The comment above that function documents
   the exact sequence it depends on — read it before touching anything. The
   additive shape that has worked all through this arc: initial namespace keeps
   the existing hardcoded answer, non-initial namespaces enumerate their list.

2. **Then the command handlers**: `RTM_NEWLINK` (with nested `IFLA_LINKINFO` →
   `IFLA_INFO_KIND` = "veth" and `IFLA_INFO_DATA` → `VETH_INFO_PEER`),
   `RTM_NEWADDR`, `RTM_SETLINK`, `RTM_DELLINK`. You will need an attribute
   WALKER — only a builder exists today.

   **`ip` requires an `NLMSG_ERROR` ACK** (type 2, body = an int error, 0 on
   success) for any request with `NLM_F_ACK`. Without it, it hangs or reports a
   failure that did not happen.

   **busybox's real `ip` applet IS in the tree** (`iplink` is in the binary), so
   the acceptance test can be a third-party tool driving real netlink rather
   than a hand-rolled message. That is much stronger evidence — but read the
   busybox-witness pattern in the `LXNS_BOOT` table first: assert the VALUE, and
   never let the success path run through a command that can succeed on its own.

3. **Bridge.** A device whose `tx` floods to its attached ports. `struct
   net_dev`'s vtable already fits this — cut 2 found the same thing about veth
   and needed no refactor.

4. **Forwarding / NAT**, so a container can reach the internet. This is the
   largest piece and genuinely its own slice.

**A dump that still says "lo and eth0" after `ip link add` succeeded would be
worse than no control plane at all** — that is the trap §18 already named ("a
half-netlink nobody can drive would be worse than none"). If you get through (1)
and (2) only, that is a good slice; do not half-build (3) on top.

## Other open items, in rough priority

- **`ramfs` reports a FIXED 0444 mode for every file** (`ramfs_open` hardcodes
  it). No initrd file can carry real permissions or a setuid bit. This is a real
  latent defect AND the thing blocking Chromium's sandbox (below).
- **Chromium's sandbox is kernel-complete and blocked on payload plumbing.** Its
  zygote wants the setuid helper `chrome-sandbox`, which `chrome-headless-shell`
  does not ship; the full `chrome-linux64` distribution has it as
  `chrome_sandbox` (UNDERSCORE — Chromium looks for the HYPHEN name beside the
  executable, setuid root). Fix the ramfs mode issue or stage it on a tobyfs
  volume. `PROBE_SANDBOX=1 bash logs/cwsandbox.sh <fresh-tag>` reproduces.
  **Three hypotheses are already dead — do not re-run them:** `/dev/null`,
  `--disable-setuid-sandbox`, `statx`.
- **The DRM `statx` arm writes a `stat`-shaped record into a `statx` buffer**
  (different layouts). Left alone because that path is verified working
  end-to-end, but it is wrong. Do not fix it casually — the GPU arc is closed.
- **Small syscall tail**, each a forward onto existing machinery: `execveat`,
  `clock_adjtime`, `restart_syscall`, `close_range`, `preadv2`/`pwritev2`,
  `pidfd_*`, `vmsplice`. Leave `bpf`, `io_uring`, `landlock` absent — large, and
  an honest ENOSYS beats a stub.
- **No cgroup namespace**, so a container mounting `cgroup2` sees the whole
  hierarchy. The capstone bundle deliberately does not mount it.
- **REAL-HARDWARE VALIDATION IS OWED AND ONLY THE USER CAN RUN IT.** Everything
  since 2026-07 is unvalidated on the EliteDesk 800 G1, plus slice 7's non-root
  login and slice 14's checklist (§20). Every QEMU boot here auto-logs-in as
  root, so this environment is structurally blind to a whole class of bug. Raise
  it; do not let it keep slipping.

## Things that are deliberate — do not "fix" them

- **No `io.max`.** This kernel cannot throttle block I/O.
- **`cow_copy_page` is uncharged.** Breaking CoW is charge-neutral.
- **`cpu.weight` selects a priority band**, not a proportional share.
- **`sys_fork_share` gives a child with NO supplied stack a private one.** That
  is a documented deviation from vfork's contract and the Chromium launcher
  depends on it. A child with an EXPLICIT stack correctly gets that one.
- **No tmpfs `size=` fallback**: an unparseable option is refused, not defaulted.
- **`mkdirat` refuses a dirfd-relative path** rather than resolving it against
  the cwd. Creating a directory in the wrong place is worse than an error.
- **`copy_user_pages` in `fork.c` is dead code.** `sys_fork` uses
  `vmm_cow_fork`.
- **ptrace refuses SINGLESTEP / `PTRACE_EVENT_*` / GETSIGINFO / GETREGSET** with
  EINVAL rather than accepting them.

## Report honestly

Partial is fine, overclaiming is not. If a step fails, say which. If a gate goes
green because the test did not run, that is a red. If you retract an earlier
conclusion, say so plainly — this document exists because the last three
sessions each retracted something the one before it asserted.
