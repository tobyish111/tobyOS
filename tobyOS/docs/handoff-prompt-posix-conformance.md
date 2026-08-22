# Handoff: tobyOS shell — from "passes a corpus" to a real conformance claim

You are taking over an arc that has just finished its previous phase. Read
`tobyOS/docs/handoff-shell-posix.md` first — it is the deep reference for how
this shell is built, how it is measured, and the traps that have cost real
time. This file is the *task*, not the background.

Branch: `feat/posix-shell`. Head at handoff: `a863b0a`.

---

## 1. Where things actually stand

| gate | command | state | time |
|---|---|---|---|
| Oils spec suite (third-party) | `bash logs/oilspec.sh` | **POSIX 100.0%** (1278–1280 of same; the denominator moves ±2 with the background-race exclusions) | ~10 min |
| bash-parity (hand-written) | `bash logs/shparity.sh` | **92/92** | ~4 min |
| INTERACTIVE parity (pty) | `bash logs/ttyparity.sh` | **4/4** (+ bash-vs-bash selfcheck every run) | ~4 min |
| Linux ABI acceptance | `bash logs/lxposix.sh` | was RED at `linux-timers`, pre-existing — verify before blaming yourself | — |

`src/shell.c` is compiled **twice**: into the kernel, and into `/bin/tsh` with
`-DSHELL_HOSTED`. One language, two hosts.

**The 100% is a narrower claim than it sounds, and the whole point of this
handoff is to stop it being oversold.** It means: of the 2,776 cases extracted
from the Oils spec suite, the 1,278 where *real bash and real dash agree* all
match. That suite was written by another project, for another shell, and is
bash-oriented. "bash == dash" is a proxy for POSIX, not POSIX.

Proof that the proxy leaks: a four-minute targeted probe at the end of the last
session found four POSIX-required behaviours that 2,776 third-party cases never
touched. They are still broken. See §4.

---

## 2. Your primary task: score against The Open Group's VSC

This is what was asked for. **The availability check has been done
(2026-08-21). Do not redo it — act on it.** What was measured, with sources:

- The suite behind POSIX shell-and-utilities *certification* today is
  **VSC-PCTS2016** (IEEE Std 1003.1-2017, Shell and Utilities —
  opengroup.org/testing/testsuites/vscpcts2016.htm). It is not a public
  download, but it is not fee-gated for this project either: The Open Group
  grants a **twelve-month free license to open source projects**, requested by
  emailing `conformance@opengroup.org`. It is also free to organizations
  submitting for certification (get.posixcertified.ieee.org). VSU — named in
  an earlier draft of this file — is the UNIX-extensions suite for the UNIX
  brand, the wrong target for this arc; VSC-PCTS is the one.
- **The blocking step is human, not research: the user must send that email
  and accept the license.** You cannot fetch the suite, and you must not fill
  in license requests or sign anything on the user's behalf. Note that
  eligibility rides on the project being open source — surface that to the
  user, offer a draft email once, then get on with §3 while the answer is
  pending.
- **VSC 5.1.1L**, a lite subset (77 utilities, POSIX.2-1992), is advertised as
  a free Open-Source-licensed download at
  `pubs.opengroup.org/onlinepubs/064999999/` — but the tarball link is
  **dead**: `VSC511AL.t.Z` 404s live, has 404'd in the Wayback Machine since
  at least 2024-09 with no archived copy, and no mirror was found. The User's
  Guide, Release Notes, and licence on that page are live and worth reading
  for the suite's shape. If a copy ever surfaces it is a real Open Group
  suite, but a score on it is "VSC 5.1.1L, POSIX.2-1992 subset" — never bare
  "VSC", never certification.

If and when the real suite arrives: vendor it under `third_party/` the way
`oils-spec` is vendored, build a runner in the shape of
`programs/oilspec/main.c`, and score it. Expect it to assume a full hosted
UNIX: it will exercise utilities, not just the shell, and it will find kernel
and libc gaps as readily as shell gaps. Budget for that; do not silently
narrow the suite to the parts that pass.

Until then, §3 is the work. Do not describe any substitute as "VSC" or as
"certification". The user has been explicit that they want the real thing; the
honest answer that it is pending a license grant is worth more than a
lookalike.

**Never report a score for a suite you did not actually run.** This project has
a documented history of harnesses that measured the previous build, measured
nothing at all, or measured themselves — see the traps in
`handoff-shell-posix.md`. A fabricated conformance number would be worse than
all of them.

---

## 3. The path that is reachable today: walk the standard

While VSC-PCTS is pending a license grant — or if the grant never comes — the
defensible path to a conformance claim is to derive the cases from the
specification text rather than from someone else's test suite.

The source is the **Open Group Base Specifications (IEEE Std 1003.1)**, which
is publicly readable. The two parts that matter:

- **XCU section 2** — the Shell Command Language. Quoting, parameter
  expansion, word expansion order, field splitting, pathname expansion,
  redirection, exit status, the grammar, signals, special built-ins.
- **XCU section 4** — the utility descriptions, specifically the built-ins
  (`cd`, `read`, `getopts`, `umask`, `set`, `trap`, `wait`, `command`, `type`,
  `ulimit`, `fc`, `jobs`, `kill`, `alias`, `hash`, `times`, `newgrp`).

Method that works here, and is already proven in this repo:

- One `initrd/etc/shparity/NN-*.sh` case per **requirement**, not per feature.
  Each case runs under the real GNU bash 5.2 in the initrd and under
  `/bin/tsh`, and must produce byte-identical stdout and exit status.
- Keep one *shape* per file so a hang or an abort cannot hide the rest.
- 4 minutes a round. That cadence is why the last session moved as far as it
  did — the 10-minute corpus is for regression, not for iterating.

Cross-checks that are legitimate but are **not** conformance: the test suites
of `yash` (the most POSIX-pedantic free shell), `dash`, and `mksh`. Useful for
finding cases to steal. Not a certification claim.

Note for scope: the **Open POSIX Test Suite** (from the Linux Test Project) is
freely available but covers the *system interfaces* — pthreads, signals,
timers, mmap. That is the `lxposix` arc, not this one. Do not confuse it with
VSC and do not report it as shell conformance.

---

## 4. The first four gaps — FIXED (e792650), and what they proved

All four POSIX-required behaviours the corpus never touched are closed, each
with a parity case (`73-umask-symbolic`, `74-set-silent-options`,
`75-set-monitor`): `umask -S`/`-p` with bash's full flag/operand matrix,
`set -o ignoreeof` (wired at the hosted interactive EOF, the one place it is
observable), `set -o nolog` (accepted state; bash's manual itself says
"currently ignored"), and `set -o monitor`/`set -m`.

**Monitor is CLOSED (d6f3e40).** The kernel had been lying to every shell:
setpgid accept-and-return-0, kill(-pgid) a silent success. Now: `pgid` on
struct proc, one shared setpgid/getpgid body for both ABIs, kill(-pgid)
fan-out with ESRCH, tty signals delivered to the foreground GROUP, and
native SYS_SETPGID/GETPGID. Under `set -m` tsh's background jobs lead
their own group (pipeline stages join the first stage's); foreground jobs
deliberately stay in the shell's group — tsh cannot tcsetpgrp from
userspace. Case 91 pins the contract with single-command jobs only (bash
leads a pipeline's group with the FIRST stage while `$!` names the LAST —
a `-$!` probe would ask about the wrong group). tsh's `kill` learned `--`
along the way. Still open on this front: tcsetpgrp/terminal handover,
prompt-time job-status lines (interactive-only), pgid translation across
pid namespaces, and SIGTSTP-stop semantics for `fg`/`bg`.

The find was the proof that §3 works: a four-minute probe found four
mandated behaviours 2,776 third-party cases never exercised. Keep walking
XCU 2 and the XCU 4 built-ins the same way — one requirement, one case, read
bash's column.

**Walk progress (9533c92):** seven more probes (cases 76–82: read, getopts,
readonly/export, wait/kill, cd/CDPATH, trap, introspection) found five more
divergences, all closed: bare `read`→REPLY, `declare -r`/`-x` reinput forms,
kill's missing-pid status, the trap listing's SIG prefix, `set -h`/hash.
getopts' full silent-mode contract and CDPATH were already right.

**Leg three (cc176b8, cases 83–88):** alias reinput, shift/break/continue
counts, XCU 2.13.1 character classes (case AND glob), ulimit operands,
dot-script return, `fc -l` on empty history. Five of six were already
right; the one fix was ulimit's malformed-value status (1, not 2 — bash
keeps 2 for unknown flags). Host-oracle trap recorded there: MSYS bash
refuses `ulimit -f 100` where guest bash accepts it — only the gate's own
bash column decides ulimit lines.

**Leg four (35f113a, cases 89–90): newgrp, entirely.** The system now
ships `/etc/passwd` + `/etc/group`, a real `/bin/newgrp` (membership
check, setgid, exec of $SHELL; no setgroups in the native ABI so the
primary gid is the whole story), and `/bin/id` as observer. Shell finds
along the way: a tsh builtin newgrp stub was SHADOWING the utility (bash
has no newgrp builtin — dropped from the hosted allow-list);
`type`/`command -v` now classify through that allow-list; `command -v
external` answers the resolved path; **shipping /etc/passwd woke `~user`
up in the guest's bash** — tsh now resolves `~user` through /etc/passwd
in both tilde sites (no stat, unknown stays literal) and `compgen -A
user` reads it too. Case 89 documents why parity alone cannot gate an
external (both columns run the same binary — its expected column and
byte count are the content check) and why inner shells write to files
(fd offsets are not shared across the Linux→native exec seam — the
documented kernel gap, dodged not fixed).

Walked so far: set options, umask, read, getopts, readonly, export, wait,
kill, cd, trap, command, type, hash, times, alias/unalias, shift,
break/continue, pattern classes, dot/return, fc(-l), ulimit, newgrp, id,
tilde (~ and ~user).

**The interactive surface has its own gate now (6fa54b0):**
`bash logs/ttyparity.sh` runs both shells on real pty pairs and
byte-compares the terminal streams, paced by prompt sentinels; bash runs
`--noediting` so the line discipline echoes for both. **The instrument
validates itself every run** (bash-vs-bash must be byte-identical) and
its bring-up caught, in order: kill(0) broadcasting to the whole session
(stopped login the moment pgids were real), libtoby poll's lie + the
unbounded pty master read, `exit` at a tsh prompt NEVER exiting, the PS2
continuation gap (`if true` executed instead of prompting), the missing
"exit" announcements, and two chatter streams ([_exit] trace,
signal_set_foreground stub) that only a byte-compared tty could see.
Corpus v1: prompt framing, PS2 (quotes/backslash/compounds), ignoreeof's
^D contract, interactive defaults + alias. **Case-writing laws:** a
BROKEN session gets one LOGGED retry (silent-child flake, seen once);
job-notification cases need pid normalization the runner does not have
yet; nothing in a case may contain the sentinel bytes.

Still open interactively, now MEASURABLE with this gate: ^Z/SIGTSTP stop
+ fg/bg resume (needs tcsetpgrp handover + WUNTRACED reporting — the
kernel work sketched in the monitor leg), fc against a real history, and
the long tail of XCU 2 the corpus already covers piecemeal. Case-writing law: the runner gives bash
and tsh DIFFERENT scratch dirs (`<scratch>/a` vs `/b`), so a case may
never print an absolute path — strip `$PWD` prefixes the way
`80-cd-details.sh` does.

---

## 5. Laws of this codebase — violating these has cost whole sessions

**Build**
- `logs/oilspec.sh` **runs its own `make`** with `-DFAST_BOOT -DQUICK_BOOT
  -DOILSPEC_BOOT`. A plain `make` between edits is **not** what the gate
  measures. A fix can be in your binary and absent from the gate's.
- Check **make's exit status**, never `[ -f tobyOS.iso ]`. A stale ISO passes
  the existence check and the gate then measures the previous build.
- No header dependency tracking: change a struct layout, `make clean`.
- A new `/bin` program or `/etc` file needs the Makefile's explicit tar list.
- Never run two gates concurrently, and never `make` while one is running —
  QEMU holds the ISO open.

**Method**
- **Measure, do not reason.** Reading the source to decide *why* something
  failed has produced confident wrong answers repeatedly in this arc. Install
  a probe and run it.
- To get a diagnostic out of a gate that only compares stdout:
  `exec 2>/tmp/e ; <thing under test> ; exec 2>&1 ; cat /tmp/e`. The case's own
  stderr lands in the compared stdout diff. Put the *setup* before the
  `exec 2>` or the 24-line window fills instantly.
- A probe installed over a **BASH-ONLY** corpus slot prints no diff whether it
  passed or failed. "No detail in log" reads exactly like "it passed". Use
  POSIX-classified slots only.
- Predict what a change should gain, then read the **LOST** list first.
- Revert what you cannot explain. Several fixes in this arc were measured,
  found to cost more than they gained, and reverted with the reason written
  down. Do that rather than carrying a mystery.

**The oracle screen — do not widen it**
`programs/oilspec/main.c` re-runs bash up to 8 times on a POSIX-classified
mismatch and excludes the case if bash disagrees with **itself**. This exists
because the guest's bash is a measured coin flip on backgrounded output
(20 runs: `a & b` split 8/12, and `a & b &` produced *no output at all* twice).
It is deliberately narrow — already-mismatched, POSIX-only, every exclusion
printed with both bash outputs and named in the report.

If you touch it, **re-run the known-bad validation**: corrupt `echo`
deliberately, run a band, and confirm real failures are still reported (the
recorded result is 6/26 = 23.1% with twenty real failures caught, three
background races excluded). A gate verified only against a passing run has not
been verified — this project shipped exactly that mistake once, in
`logs/cwwebgl.sh`.

---

## 6. What honest completion looks like

Report the claim at the strength the evidence supports, and no higher:

- "Passes 100% of the POSIX-classified cases in the Oils spec suite, and N/N of
  a hand-written POSIX XCU mapping" — defensible today.
- "POSIX compliant" — needs the standard walked systematically, and even then
  is a statement about *tested* behaviour.
- "POSIX certified" — needs VSC-PCTS run under The Open Group's certification
  program. Do not imply it.

If the walk finds gaps you cannot close, list them. A short accurate list of
what fails is worth more than a percentage that hides it.
