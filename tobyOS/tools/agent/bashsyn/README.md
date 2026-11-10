# bashsyn — the bash-grammar check, tested against bash itself

`src/shell_bashsyn.inc` is a recognizer of bash 5.2's grammar that tsh runs
on every command before executing it (see the comment at its top). This
directory tests it on the HOST against the real bash 5.2.15 — the guest's
exact version — built in WSL at `~/bash-5.2.15/bash`.

| file | what |
|---|---|
| `harness.c` | compiles the very `.inc` with `malloc`; reads files the way `bash -n FILE` does and prints `NAME<TAB>STATUS<TAB>msg\|msg...` |
| `oracle.sh` | the same line from bash: `bash -n` on each file, `LC_ALL=C` (the guest's locale) |
| `oracle-run.sh` | the same, RUNNING each file — for cases whose header must execute (alias definitions, then `set -n`) |
| `fuzz.py` | damaged copies of a corpus: cut short, spans deleted, shell tokens inserted, metacharacters swapped in, lines duplicated or swapped; deterministic per seed |

Build: `gcc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-function -o harness.exe harness.c`
(MSYS2 ucrt64, `TMP='C:\t'`).

Run (the corpus — every Oils spec case plus the shparity fixtures, one script
each — lives outside the repo, in `C:\Users\tdude\src\bash-ref\corpus`):

    # oracle, in WSL
    cd corpus; ls | grep '\.sh$' | xargs -n 400 bash .../oracle.sh . > ../corpus-plain-oracle.txt
    ls | grep '\.sh$' | xargs -n 400 bash .../oracle.sh -O extglob . > ../corpus-eg-oracle.txt
    # the check
    ls | grep '\.sh$' | xargs -n 400 harness.exe | tr -d '\r' | sort > ../corpus-plain-harness.txt
    # fuzz
    python fuzz.py corpus fz1 1 3

Then `diff` the sorted files. Aliases: `harness.exe -a NAME=VALUE ...` against
`oracle-run.sh` on files that define the same aliases and run `set -n`.

Results are in `logs/b9/bashsyn-*`.

## Known difference

When the token bash names in a message is a WORD containing `$( )`, `<( )` or
`>( )`, bash prints that substitution as its command printer re-formats it
(`<( sh)` → `<(sh)`, `$( a|b )` → `$(a | b)`); the check prints it as
written. Verdict, line and status are the same. 4 of the 35,532 fuzz cases.

## The command printer (batch 12)

The same file also BUILDS bash's command tree when asked (`bsyn_env.build`)
and PRINTS it with the rules of bash's print_cmd.c (`bsp_*`, tsh's own code):
what `declare -f`, `type`, `set`, an exported function's environment string,
`$BASH_COMMAND`, a job's name and the text a `$( )` runs are made of.

| file | what |
|---|---|
| `prharness.c` | the tree + printer on the host. `prharness OUTDIR FILE...` parses each file's first command and prints it as `declare -f f` would; `PR_ALL=1` prints every command (make_command_string), `PR_ALL=1 PR_DECL=1` every function definition |
| `mkwrap.py SRC DST` | wraps each corpus script as `f() { ... }; declare -f f` |
| `oracle-wrap.sh DIR OUTDIR` | runs bash 5.2.15 (WSL) on the wrapped scripts, keeping its `declare -f` output: the oracle |
| `cmp.sh WRAPDIR ORACLEDIR OUTDIR` | prharness over the wrapped scripts, compared with the oracle |
| `asanrun.sh REF WRAPDIR` | in WSL, prharness built with ASan+UBSan (`prharness_asan`) over the corpus and the fuzz sets, every command printed |

Build: `gcc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-function -o prharness.exe prharness.c`
(MSYS2 ucrt64, `TMP='C:\t'`); the ASan build is the same with
`-fsanitize=address,undefined -o prharness_asan`, in WSL. prharness sets
`c_locale` (the oracle runs under `LC_ALL=C`, glibc: `$'\u03bc'` prints back
as `\u03BC`); tsh passes its own decoder (`ansi_u`), which follows the box's
musl bash (`*` in the C locale).

Results (2026-10-03, `logs/b12/printer/`): 2845 of the 2849 corpus scripts
bash prints as a function come out byte-identical; the other 4 (1509 1529
1918 1930) are scripts whose text closes `f`'s body early (`try {...}`,
`{ls; }`, an array literal error), so bash never reaches `declare -f` -- not
printer differences. The check's verdicts are unchanged by the tree code:
corpus plain and extglob 0 differences, fz1/fz2/fz3 the same 2 each as
before (the `<( sh)` spelling above). ASan/UBSan/LeakSanitizer: 0 reports
over ~32,000 inputs.
