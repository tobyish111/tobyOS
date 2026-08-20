# Handoff: tobyOS shell (`tsh`) POSIX conformance

You are picking up an in-progress arc: raising `/bin/tsh` against a **third-party**
POSIX conformance suite. Read this whole file before touching anything. Most of
it is not "how the code works" — it is **how to find out what is actually true**,
because in this arc reading the source has produced confident wrong answers over
and over, and measuring has not.

Current state: **POSIX 1196/1280 = 93.4%** (from 711 = 55.5% at the start of the
arc, 1041 = 81.3% at the start of this session). `broken=0`, timeouts 1.
Bash-parity gate steady at **52/54**.

---

## 1. What the gate is

`third_party/oils-spec` is the **Oils spec suite** — 222 files, 3,964 cases,
written by someone else, for a different shell project. That is the whole point:
our own 54-case corpus (`logs/shparity.sh`) can only contain bugs we already
suspected.

Four pieces:

| file | job |
|---|---|
| `logs/oilspec_extract.py` | parses the `#### case` format into `initrd/etc/oilspec/NNNN.sh` + `manifest.json`. **2,776 cases** (YSH/Oil-only files dropped). |
| `logs/oilspec_host.py` | runs every case under **real bash 5.2 AND real dash on the build box** and classifies it. ~48s. |
| `programs/oilspec/main.c` | the in-guest differential runner. Prints a compact **MAP bitmap** (64 cases/line) because 2,776 prose lines would swamp the serial wire. |
| `logs/oilspec_report.py` | decodes the MAP, joins manifest + classification, writes the census, the failure list, and the **run-to-run diff**. |

Two more, added this session, that make a run readable:

| file | job |
|---|---|
| `logs/oilspec_show.py` | `python logs/oilspec_show.py 1765 1766` — the case text, its class, and the host oracle's expectation, together. |
| `logs/oilspec_detail.py` | pulls the per-case `L1 bash:` / `L1 tsh :` diffs out of the 5 MB serial log and reprints them **grouped by spec file** next to the case text. `python logs/oilspec_detail.py redirect here-doc`, or with no argument for all of them. |

### The two oracles — do not confuse them

- **`logs/oilspec_host.json` only CLASSIFIES.** Cases where host bash and host
  dash agree exactly are **POSIX** (1280 — the compliance denominator). Where
  they differ, **BASH-ONLY** (1481). Nondeterministic under two bash runs,
  **UNUSABLE** (15, excluded both ways).
- **The comparison is against the bash 5.2 binary in the initrd**, in-guest,
  same scratch dir, stdout + exit status must match **exactly**. stderr is
  reported but never compared (a diagnostic names the shell that produced it,
  so requiring equality would bake in a permanent failure).

**They can disagree, and eleven of the remaining failures are that.** Case 2098
(`read line <&6`) and 2118 (`exec 3>&1; exec 4>&1`) are cases where the guest
bash FAILS on a kernel gap and tsh gets the right answer; matching the oracle
there would mean breaking tsh. A stubborn "POSIX" case is not automatically a
tsh defect — check what the guest's bash actually does before chasing it.

**The corpus's own recorded `## STDOUT:` blocks are NOT the oracle.** They were
recorded years ago on another machine. Trusting them manufactures phantom bugs.

---

## 2. Running it

Everything below runs from **`c:\CustomOS\tobyOS`** and needs this PATH:

```bash
export PATH="/c/msys64/usr/bin:/c/msys64/mingw64/bin:/c/Program Files/LLVM/bin:$PATH"
```

```bash
# compile-check only (fast, do this before every gate run)
make "CC=TMP='C:\t' TEMP='C:\t' clang" tobyOS.iso 2>&1 | grep -E ' error|undefined'

# full run, ~9 min. Run it in the BACKGROUND and wait for the notification.
bash logs/oilspec.sh > /path/to/scratch/run.txt 2>&1

# one band with per-case detail, ~4 min. The probe loop (see below) uses this.
bash logs/oilspec.sh 1730-1765 > /path/to/scratch/band.txt 2>&1

# the bash-parity gate. MUST stay 52/54. Run before every commit -- it has
# caught three regressions the 2,776-case corpus did not.
bash logs/shparity.sh
```

After a full run, read **in this order**:

```bash
sed -n '/score by class/,/^$/p' /path/to/scratch/run.txt   # the number
cat logs/oilspec_diff.txt                                  # WHAT MOVED  <- the important one
python logs/oilspec_detail.py                              # every POSIX diff, by feature
```

Hard operational rules:

- **Never run two gates concurrently.** They fight over QEMU and the log.
- **Never edit source while a gate is BUILDING.** It will compile a half-edited
  file and the result is attributed to the wrong change.
- The full run needs `-m 4096` (825 MiB initrd) — already in the script.
- **Never use a bash heredoc for anything containing backslashes.** Even
  quoted `<<'EOF'` eats a layer: `\n` becomes a real newline and `\"` becomes
  `"`. This cost time five times before this session and three more times
  during it. Use the Write or Edit tool.

---

## 3. The method that works

1. **Rank.** `python logs/oilspec_detail.py` groups every POSIX failure by
   spec file with its actual diff. A file is a feature area. Pick the biggest
   cluster with a single mechanism — not the biggest number.
2. **Read the diff, not the source.** The `L1 bash:` / `L1 tsh :` lines say
   what happened. Source-reading in this arc has produced confident wrong
   answers repeatedly.
3. **If the diff is not enough, install a PROBE as a corpus case.** Overwrite
   `initrd/etc/oilspec/NNNN.sh` with the lines you want to test, run that band,
   read the diff, then `git checkout initrd/etc/oilspec/`. bash and tsh run
   *the same text* side by side. This is still the single highest-value
   technique in the arc: it settled four questions this session in one
   four-minute run each, including two where the source reading had been
   wrong.
4. **Predict what should move, in writing, before you run.** Then check the
   diff against the prediction. A change that gains something other than what
   you predicted means you do not understand it yet.
5. **Full run → read the diff → shparity → commit.** Only commit measured work.

### Why this discipline exists — new evidence from this session

- **The gate could not see itself.** The whole-corpus run had produced ZERO
  per-case diffs for as long as the detail code existed, because
  `/etc/oilspec/POSIX` is written by Windows Python in text mode and every id
  read as `"0002\r"`. EXCLUDE survived the same CRLF by luck — its lines are
  `NNNN reason`, and cutting at the space took the CR with it. The score
  looked healthy and the one thing the run existed to produce was missing.
  **When a diagnostic produces nothing, suspect the diagnostic.**
- **shparity found what 2,776 cases did not.** Two real bugs — the assignment
  shape test reading the new no-split markers as text, and a keyword finder
  returning the position OF a closing `)` as the end of the list before it —
  showed up as three shparity failures while oilspec was climbing. Run both.
- **A batch that regressed.** Batch 5 added five grammar checks, gained 20 and
  lost 34, because a single `|` inside a case pattern (`a|b)`) was being read
  as a pipe. Predicting per-case and reading the LOST list is what caught it
  in one cycle instead of carrying it.
- **The ISO can hold a different initrd than `build/initrd.tar`.** One run
  reported `SKIP reason=no-corpus` with gate 0 green: the tar had 3,877
  entries and the ISO's copy stopped after 817. The gate now prints what the
  guest MOUNTED next to what the host packed. Fix is
  `rm -f build/initrd.tar build/base.iso tobyOS.iso` and re-run.

---

## 4. How the parser is put together now

`src/shell.c` is a line-oriented shell: it reads a logical line, joins
continuations and multi-line compounds into ONE line, and hands that to
text-matching parsers. The single most important thing to understand is
**`struct shell_scan`** (search for "the structural scanner").

A RESERVED WORD IS ONLY RESERVED WHERE A COMMAND MAY START. The scanner walks
tokens tracking a command position, `( )` / `{ }` / compound nesting, unclosed
`$( )` / `${ }` / `$(( ))` / backticks, and a handful of syntax judgements. Six
things run on it and must keep running on it, or they will drift apart again
the way they had:

- the multi-line accumulator (`shell_run_script_text`)
- the top-level list splitter (`shell_find_list_sep`)
- the function-body finder (`shell_try_function_definition`)
- the join separator between two physical lines (`st->no_sep`)
- the "may a `COMPOUND &` be backgrounded" test
- `shell_line_syntax_ok`, which rejects what bash rejects

Two rules inside it are easy to get wrong and were:

- `no_sep` is NOT `cmd_pos`. After an assignment a command may still start
  (`x=1 cmd`), but a newline ends the command, so joining the next line needs
  a `;`. Using cmd_pos turned `x=$((x+1))` / `echo $x` into a prefix
  assignment on echo.
- a single `|` inside a case pattern is ALTERNATION. Clearing the
  pattern position on it makes every `a|b)` a syntax error.

The keyword finders (`shell_find_if_fi`, `shell_find_elif_else`,
`shell_find_loop_do`, `shell_find_matching_done`, `shell_find_kw_sep`) share
`shell_sep_keyword`: a reserved word counts only after a command terminator,
and `shell_sep_cut` decides whether that terminator belongs to the preceding
list (`}` and `)` do, `;` does not).

`src/shell.c` is compiled TWICE — into the kernel, and into `/bin/tsh` with
`-DSHELL_HOSTED`. A kernel-only symbol needs a shim in `programs/tsh/host.c`
or the hosted link fails.

---

## 5. What is left: 84 POSIX failures

Run `python logs/oilspec_detail.py` for all of them with diffs. By kind:

| kind | n | note |
|---|---|---|
| implementable shell work | 61 | see below |
| the guest's bash is the one that is wrong | 11 | 2098 2118 2202 2268 2343 2410 2647 1565 2260 1478 1822 — matching these would mean breaking tsh |
| needs a kernel feature | 5 | symlinks (`ln -s`), mkfifo, `<>` on a fifo, globstar |
| NUL bytes inside a variable | 4 | see below |
| known flaky | 3 | 2271 2272 2273 — background output racing the exit |

**100% is not reachable.** 18 of the 84 are the oracle, the kernel, or timing;
they are not shell defects and "fixing" them would mean making tsh worse. The
reachable ceiling is around **1262/1280 = 98.6%**, and the last stretch of that
is expensive.

The 61 that ARE shell work, by mechanism, largest first:

- **`$@` semantics** (0224 2374 2530 2531 2546 2741). Unquoted `$@` must
  produce one field per parameter including empty ones, and `"[$@]"` in an
  assignment joins with spaces. The field markers (`SHELL_ARG_MARK`) already
  exist; what is missing is that an empty parameter survives them.
- **temporary environment frames** (0416 0421 1165). `FOO=a BAR=[$FOO] cmd`
  must apply the assignments left to right, each RHS seeing the earlier ones;
  tsh expands them all first.
- **traps** (1245 1246 1209) and **errexit corners** (1557 1564 1518 1525).
- **here-documents in odd places** (0533 0534 1754 1760) — a here-doc inside
  backticks is not collected at all.
- **command substitution corners** (1379 1394 2383) — including the
  `case` inside `$( )` that is documented below as attempted and reverted.
- singles: 0158 (`a /= -3`), 0422 (escaped `=` in a command name), 0424,
  0741 (CDPATH), 1010/1037/1038 (printf), 1135 (read's last field), 1693/1694
  (glob bracket expressions), 1953, 1970 (bare `;`), 2040, 2058 (a compound's
  redirection is applied BEFORE its word list expands), 2071 (a redirect on a
  function definition applies to every call), 2080, 2354/2359 (tilde inside
  `${ }`), 2623 (`$''` in a VarSub argument), 2629, 2646, 2665 (`$LINENO`
  after the accumulator joins lines), 2692.

### Structural items — status

- **NUL bytes — NOT WORTH IT, and the reason matters.** The cases need a NUL
  *inside a variable* (`s=$(printf '\000.\000')`, `${#s}`). C strings cannot
  hold that. A length-carrying **output path** — which is what it looks like
  from the outside — moves almost none of them; it needs a length-carrying
  **value** representation through the variable table, argv and every
  expansion. Also `src/tcp_shell.c` installs the string sink with no byte
  sink, so the obvious surgical fix misroutes the TCP shell.
- **`case` inside `$( )` — ATTEMPTED AND REVERTED (previous session).** Making
  the substitution scanner case-aware gained none of its targets and cost five
  arith cases. The mechanism was never explained. If you retry it, find out
  why first.
- **`ulimit` reports "unlimited" on purpose.** The kernel discards setrlimit
  and answers getrlimit with infinity, which is why the real bash in the
  initrd prints `unlimited` after `ulimit -f 4294967296`. tsh keeps its table
  for validation and for the hard-limit rule, but reports what is in force.
  Remove the override in `shell_rlimit_print` the day the kernel grows real
  per-process limits.
- **`[[ ]]`** is skipped by the structural scanner (it has its own grammar)
  but is still not a builtin in the hosted shell, so a malformed one spawns
  `/bin/[[`. Three cases (0556 2148 2150) want specific statuses there.

---

## 6. Rules to work by

1. **Measure, don't reason.** Every confident source-reading diagnosis in this
   arc that was not checked against a probe or a diff turned out wrong.
2. **Predict, then verify.** Write down what should move before you run.
3. **Revert what you cannot explain**, however plausible it looks.
4. **A correct change that gains nothing is not evidence of anything.** Go
   back to the case.
5. **shparity must stay 52/54.** Its two failures are kernel-side issues where
   *tsh is correct and the oracle is wrong* (no unlink-while-open; fork/dup2
   do not share fd offsets). If it drops to 51, you broke something — and it
   will be something the big corpus does not cover.
6. **Commit messages carry the retraction.** When something is reverted or a
   previous claim was wrong, say so in the message.

Relevant memory files: `shell-oilspec-third-party-gate.md`,
`spawn-fd-refcount-leak.md`, `shell-bash-parity-gate.md`.
