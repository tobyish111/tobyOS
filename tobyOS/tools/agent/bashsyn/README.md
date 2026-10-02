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
