# The bash-parity gate

## The contract

The tobyOS shell is meant to relate to bash the way TypeScript relates to
JavaScript: **a strict superset.** Every bash script runs unchanged and
produces the same bytes and the same exit status. Extra features — types, an
object pipeline, a static checker — are welcome on top. Divergence on shared
syntax is a bug, not a dialect.

PowerShell is the counter-example worth keeping in mind: it is a rewrite
rather than a superset, which is why adopting it meant abandoning everything
already written. A superset has no adoption cost, so that is the shape to
hold onto.

"Superset" is a marketing claim until something measures it. This gate
measures it.

## What it does

`/bin/shparity` runs every case in `/etc/shparity/*.sh` twice:

    /bin/bash --norc --noprofile CASE     # the oracle
    /bin/tsh                     CASE     # the subject

with identical argv, identical environment, and a freshly wiped working
directory each, then requires **byte-identical stdout and an identical exit
status.**

The oracle is not a specification we interpret — it is the unmodified GNU
bash 5.2 already shipped in the initrd (`programs/realbash`). That matters:
it is the same binary users compare us against, so there is no gap between
"passes our reading of POSIX" and "behaves like bash".

**stderr is captured and reported but never fails a case.** Diagnostics name
the shell that produced them, so `bash: line 3: ...` can never equal a tsh
message; requiring equality there would bake in a failure we would then be
tempted to paper over. Its first line *is* printed, because it usually names
the actual cause — `sh: ]: command not found` explains a whole file of wrong
branches at a glance.

## Running it

    bash logs/shparity.sh

Adding a case: drop a `.sh` file in `initrd/etc/shparity/` and re-run. Cases
must be **deterministic** (no clock, pid, network, randomness) and should stay
inside shell builtins wherever possible, so a case measures the *shell* and
not whichever `ls` happens to be installed. Validate a new case against real
bash on the host first; the gate is only as honest as its corpus.

Two environment facts the gate depends on, both of which cost a debugging
session to learn:

- **Scratch lives on `/tmp` (tmpfs)**, falling back to `/data`. It cannot live
  on `/`: the root filesystem is the read-only initrd ramfs, whose
  `ramfs_create()` returns `ROFS` for any path not already in the tar. Scratch
  used to be on `/data`, which meant every run wrote to the real disk — and
  hard-killing QEMU on timeout eventually corrupted the volume (`ino=96
  allocated but type=FREE (orphan slot)`), after which the scratch directory
  could be neither opened nor recreated. A test harness has no business
  persisting anything.
- **QEMU needs `-m 4096`, not the 512 the other harnesses use.** With Chromium
  staged the initrd is ~825 MiB and Limine loads it whole before the kernel
  starts. At 512 MiB the run dies in the *bootloader* (`High memory allocator:
  Out of memory`) before a single line of gate output — which reads like a
  broken gate rather than a too-small box.

Do not run two gates at once. The driver kills QEMU by image name on the way
out, so an overlapping run murders the other one's VM mid-boot and the log
stops at `Loading module` — indistinguishable from a boot failure.

`GATE 0` in the driver script checks that `bin/tsh`, `bin/shparity` and all
corpus cases are really **in `build/initrd.tar`**, and that `SHPARITY_BOOT` is
really in `tobyos.bin`, before booting anything. A payload that silently
failed to ship would otherwise produce a clean-looking `SKIP` that reads like
"nothing to fix".

## Where it stands

    [SHPARITY] VERDICT: 21/23 pass

| Run | Score | What changed |
|-----|-------|--------------|
| Baseline | 0/14 | `programs/sh`: a big parser with almost no expansion layer |
| Lift | 3/14 | `/bin/tsh` rebuilt from `src/shell.c` (the kernel shell's language) |
| Separators | 4/14 | multi-line compounds; whitespace-tolerant `; do` |
| File layer + `**` | 5/14 | two GP faults fixed; exponentiation added |
| **Nine-gap sweep** | **21/23** | corpus grown to 23; all nine remaining gaps closed |

The two failures are cases where **tsh is correct and the oracle is not** — see
"When the oracle is the broken one" below.

## POSIX measurement, 2026-08-17

    33/53 (62%)  ->  49/53 (92%)

The corpus was grown from 23 cases to 53, mapped section by section onto the
POSIX Shell Command Language (XCU 2): quoting, special parameters, parameter
expansion, tilde, command substitution, arithmetic, field splitting, pathname
expansion, expansion ORDER, redirection, here-documents, exit status,
pipelines, AND-OR lists, groups and subshells, for/case/while/until,
functions, test, set options, special builtins, getopts, read, printf and
traps. Every case was validated against real bash on the host before shipping.

The first measurement said **33/53** and settled the question the 23-case
corpus had made look easy: the shell was not POSIX compliant, and 18 of the 30
newly added cases failed. The old 21/23 had meant "the things we thought to
test pass".

### Fixed since

Kernel/VFS (see the section below): fork and dup2 now share one file offset,
and tmpfs defers freeing an unlinked node until its last handle closes. That
alone turned four cases green -- ones in which tsh was already correct and only
the oracle misbehaved.

Shell, in order of how much they unblocked:

| Gap | Cause |
|-----|-------|
| Here-doc detection blind to `$(( ))` | `shell_line_find_heredoc` tracked quotes but not substitutions, so `$((1<<4))` had its `<<` read as a here-doc operator -- taking the entire shift family with it |
| Logical lines | A trailing backslash and an unclosed quote did not continue onto the next line; the reader stitched compounds and here-docs but not these |
| Backslash in double quotes | Swallowed unconditionally; POSIX keeps it literal except before `$`, a backtick, `"`, another backslash, or newline |
| Tilde on an assignment RHS | `x=~` stored a literal tilde; also expands after each `:`, which is what makes `PATH=~/bin:~/tools` work |
| Compound as a pipeline stage | Split before tokenizing, because by token time `while` is just a word. Stages run sequentially, bounded by the 64 KiB pipe buffer |
| `VAR=x fn` | The assignment words were threaded into the builtin path but not the function path, so a prefix assignment to a function vanished |
| `for a` with no `in` | Flattened the positionals into a string and re-split, so `set -- "one two"` became two iterations |
| Bare `(` in `$( )` | The scanner counted `$(` but not a plain `(`, while every `)` decremented -- so a subshell inside a substitution closed it early |
| Nested `case` | Both the closing `esac` and the clause-ending `;;` were found by first match, so an inner case stole them and truncated the outer body |

### Still open (4 of 53)

| Case | Cause |
|------|-------|
| 37-redir-fd | **Descriptors above 2 are not supported.** `exec 3> f` and `echo x >&3` need fds 3..9; `shell_fd_state`, `shell_io_frame` and the subshell frame all carry exactly three slots, and `shell_apply_redirs` rejects anything outside 0..2. Widening those arrays is mechanical but touches every redirection site. The earlier failures in this case -- `1>&2`, then stdout/stderr ordering -- are both fixed. |
| 43-for-variants | `for ... done > file` reaches `shell_parse_compound_redirs` with an EMPTY tail, so it returns success with no redirection and the loop writes to the terminal. The tail is lost between `shell_find_matching_done` and the parse; the line itself survives the splitter intact. |
| 50-getopts | `$#` is correct and `$1` is empty after `shift $((OPTIND-1))`, so the fault is in `shift` / positional rewriting, not option parsing. |
| 51-read-builtin | `IFS=: read a b c < f` does not execute -- the variables keep their previous values. An assignment prefix on this builtin in this shape. |

### Design decision taken

`/bin/tsh` no longer registers the kernel shell's convenience builtins. The
hosted build answers "is this a builtin?" from an allow-list of exactly the
names bash implements internally, and everything else goes through PATH. The
kernel shell keeps all of them -- it may run before there is any `/bin` to
exec from. One `#ifdef SHELL_HOSTED` in `shell_cmd_lookup()`, switched on by a
define on the tsh compile rule.

That closed the `echo x | cat` case: the builtin `cat` demanded a path where
the real utility reads stdin. A builtin sharing a utility's name but not its
behaviour is precisely what a superset shell must not do.

## The architecture: one language, two hosts

`src/shell.c` is compiled **twice**. In the kernel the keyboard poll loop
drives it over the real subsystems; in `/bin/tsh` `programs/tsh/host.c` drives
it over libtoby syscalls. The expansion family, the recursive-descent
arithmetic evaluator, globbing, functions, traps and here-documents exist
exactly once and cannot drift between the two.

This replaced the obvious plan — port features into `programs/sh` — because
that shell was itself the argument against it: a full parser with
if/for/while/case that still scored 0/14, because the hard part of a shell is
not the grammar, it is everything the grammar delegates to.

`src/shell.c` was **not** carved up to make this work. It gained three
additive entry points (`shell_init_hosted`, `shell_run_script_hosted`,
`shell_run_line_hosted`); nothing existing was removed or compiled out, so the
kernel shell gains every fix below rather than diverging from them. The ~120
kernel symbols only its kernel-only builtins need are satisfied by
`programs/tsh/kstubs.c`, where each one says out loud that it is unavailable
rather than returning a quiet zero — an `ifconfig` that prints a confident
fictional network configuration is the failure mode this project keeps finding
elsewhere.

## What the gate found, and what fixed it

Every one of these was latent in the interactive kernel shell. None was
introduced by the port; the gate simply asked questions nobody had asked.

### Parsing

1. **`if c; then x; fi` did not parse.** `shell_find_if_fi` required the
   character *before* the `;` to be a space, so `echo eq; fi` was rejected
   while `echo eq ; fi` was accepted.
2. **Separators had to be exactly one space** — `for w in $v;   do` failed.
   Now normalised in place before parsing.
3. **Compound commands had to be on one line.** Correct for an interactive
   shell, wrong for every script ever written. The script reader now
   accumulates lines until the construct closes.
4. **Anything after a compound was silently dropped.**
   `shell_try_if_command` computed `fi_skip` and threw it away, so
   `if a; then b; fi; echo c` never echoed. Harmless while compounds were
   one-liners; fatal once multi-line compounds got joined into one line.
5. **`(subshell)` was rejected unless it was the entire line.**

### Expansion

6. **`$?` was a line ahead of itself.** `shell_tokenize` expands as it
   tokenizes and was handed whole lines, so `false; echo $?` expanded `$?`
   before `false` ran. The same ordering bug ran a `$( )` in the second half
   of a line before the first half, side effects and all. Lines are now split
   into `;` / `&&` / `||` segments *before* tokenizing — which also fixed 4
   and 5, since a compound parser now only ever receives its own text.
7. **Field splitting knew only whitespace.** IFS whitespace collapses, but
   each non-whitespace IFS character delimits on its own, so `a::b` with
   `IFS=:` is three fields, the middle one empty. An unquoted expansion that
   comes out empty now contributes no word at all.
8. **`"$@"` collapsed to one word.** It now plants a `0x01` word-boundary
   marker before each parameter, and `shell_add_arg` splits on it even for
   quoted words. The marker is a prefix rather than a separator, so zero
   parameters yields zero words instead of one empty one.
9. **`$( )` and `${ }` inside `$(( ))`** reached the evaluator as literal
   text. The expression is expanded before it is evaluated.
10. **`**` did not exist** — it consumed one star and failed on the second.

### Words and patterns

11. **`for` iterated raw tokens**, so `for w in $v` ran once with `a b c` and
    `for f in *.txt` iterated the literal pattern. The list now goes through
    the same word pipeline as command arguments.
12. **Glob results were unsorted.** bash guarantees sorted order and scripts
    rely on it; readdir returns whatever order the filesystem stores.
13. **Quoted case patterns were compared literally**, so `"")` — the
    idiomatic empty-string arm — never matched, and `"*"` matched everything
    instead of one asterisk. Patterns are unquoted before matching, with
    quoted metacharacters escaped so they stay literal.

### Redirection

14. **A redirection on a compound was ignored**, so
    `while read l; do ...; done < file` read from the terminal and the loop
    body never ran. Loops now parse their tail as redirections and wrap the
    whole loop in one io frame, with a single exit path so it always unwinds.

### Bugs in the port itself

15. `shell_open_vfs_file()` builds a **real** `struct file`, so the host's
    wrapper-struct cast read past the allocation — two GP faults.
16. The host's `vfs_open` opened `O_RDONLY`, making every `>` target
    unwritable.
17. A ~5.5 KB `struct shell_pipeline` on the stack of `shell_try_for_command`
    — a recursively entered function already holding ~5 KB — overflowed the
    stack on nested loops and corrupted memory a byte at a time. The symptom
    was single characters missing at random positions, in cases with no loops
    in them at all. Now heap-allocated, along with two other frames.

## Kernel bugs the gate turned up

Neither is a shell bug. Both surfaced because a differential gate measures
*both* sides, and the oracle started misbehaving.

- **`O_APPEND` was parsed and discarded.** `sys_open` read the flag and threw
  it away (`(void)want_append; /* honoured at write-time once we plumb seek */`),
  so every `>>` in every program wrote from offset 0 and overwrote what it was
  meant to append to. Invisible in casual use, because `echo two >> f`
  replacing `one` leaves a plausible file of the same length. Fixed by
  positioning at end-of-file on open. Full atomic append (two processes
  appending to one file) still wants the shared-offset work below.
- **There was no writable `/tmp`.** Root is the read-only initrd ramfs, whose
  create hook returns ROFS for anything not already in the tar, so nothing in
  the system could make a temp file — `tmpfile()`, `mkstemp()` and bash's
  here-documents all silently produced garbage. tmpfs had been in the tree
  since slice 5, reachable only through `mount(2)`, mounted by nobody. Now
  mounted at boot.

## When the oracle is the broken one

Two cases fail with tsh producing the **correct** output and real GNU bash
producing the wrong one. They are kept failing rather than quietly excluded,
because each marks a real kernel gap:

| Case | Symptom | Root cause |
|------|---------|-----------|
| 12-heredoc | bash emits nothing for its here-documents | **No unlink-while-open.** bash writes a here-doc to a temp file and unlinks it while holding the fd. `tmpfs_unlink()` does `kfree(nd->data)` and zeroes the node immediately — there is no open-handle refcount anywhere in the VFS — so the fd reads back nothing. |
| 20-subshell | bash loses a subshell's output | **fd offsets are not shared.** `file_clone()` byte-copies `struct vfs_file`, cursor included, so `fork()` and `dup2()` hand out independent offsets instead of one shared open file description. The subshell child writes at offset N, the parent's own offset is still N, and the parent's next write overwrites the child's bytes. |

Both want the same missing layer: an open-file description holding the cursor
and a link count, rather than a cursor embedded per descriptor. That is a VFS
slice, not a shell one, and it is worth doing — `dup2` is how every shell
implements `2>&1`, and fork-with-shared-offset is what makes `(subshell) >> log`
behave.

A layout-safe route exists. `struct file` already carries a side-allocated
`int *vfs_refs`; widening that allocation to `struct { int refs; size_t pos; }`
shares the cursor without changing `sizeof(struct file)` — which matters,
because this build has no header dependency tracking and a real layout change
would need a full clean rebuild to avoid silent corruption. 26 call sites read
the cursor today.

## Files

| Path | Role |
|------|------|
| `programs/shparity/main.c` | the differential harness |
| `initrd/etc/shparity/*.sh` | the corpus (23 cases, bash-validated) |
| `logs/shparity.sh` | host driver: build, GATE 0, boot, report |
| `programs/tsh/host.c` | userspace host: printk sink, file layer, vfs, spawn |
| `programs/tsh/kstubs.c` | kernel-only symbols, each loudly unavailable |
| `src/kernel.c` (`SHPARITY_BOOT`) | spawns the gate at boot |
| `Makefile` (`SHPARITY_CASES`) | corpus staging; the one globbed initrd entry |
