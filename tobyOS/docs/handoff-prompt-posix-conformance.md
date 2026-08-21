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
| Oils spec suite (third-party) | `bash logs/oilspec.sh` | **POSIX 1278/1278 = 100.0%** | ~10 min |
| bash-parity (hand-written) | `bash logs/shparity.sh` | **73/73** | ~4 min |
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

## 2. Your primary task: score against The Open Group's VSC/VSU

This is what was asked for. **Do the availability check FIRST and report before
spending a session on it.**

What is believed, and what you must verify rather than assume:

- The Open Group's verification suites — **VSC** (Verification Suite for
  Commands), **VSU**, VSX, VSTH — are the suites behind UNIX/POSIX
  *certification*. They are understood to be **licensed products, not public
  downloads**: historically a signed agreement and a fee, delivered as source
  to the licensee.
- If that is still true, you **cannot** obtain them autonomously, and you must
  say so plainly rather than substituting something else and calling it VSC.

So, in order:

1. **Establish obtainability.** Check The Open Group's current distribution
   terms. Report: obtainable / licensed-only / superseded-by-something. One
   short answer with a source.
2. **If obtainable** — vendor it under `third_party/` the way `oils-spec` is
   vendored, build a runner in the shape of `programs/oilspec/main.c`, and
   score it. Expect it to assume a full hosted UNIX: it will exercise
   utilities, not just the shell, and it will find kernel and libc gaps as
   readily as shell gaps. Budget for that; do not silently narrow the suite to
   the parts that pass.
3. **If licensed-only** — say so, do not fake it, and fall back to §3. Do not
   describe any substitute as "VSC" or as "certification". The user has been
   explicit that they want the real thing; the honest answer that it is not
   reachable is worth more than a lookalike.

**Never report a score for a suite you did not actually run.** This project has
a documented history of harnesses that measured the previous build, measured
nothing at all, or measured themselves — see the traps in
`handoff-shell-posix.md`. A fabricated conformance number would be worse than
all of them.

---

## 3. The fallback that is actually reachable: walk the standard

If VSC/VSU is out of reach, the defensible path to a conformance claim is to
derive the cases from the specification text rather than from someone else's
test suite.

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

## 4. Known-broken right now — start here, they are cheap

Measured in the guest, not inferred. All four are POSIX-required and all four
were invisible to the third-party corpus:

| requirement | tsh today |
|---|---|
| `set -o ignoreeof` | rejected |
| `set -o monitor` | rejected |
| `set -o nolog` | rejected |
| `umask -S` | `umask: '-S': invalid mode` |

`umask -S` is the sharpest: symbolic output is mandated, it is trivially
testable, and nothing in 2,776 cases exercised it. `set -o monitor` is the
substantial one — it implies job control, so scope it before starting.

These are the proof that §3 is worth doing. Fix them with a regression case
each, then keep walking.

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
- "POSIX certified" — needs VSC/VSU and The Open Group. Do not imply it.

If the walk finds gaps you cannot close, list them. A short accurate list of
what fails is worth more than a percentage that hides it.
