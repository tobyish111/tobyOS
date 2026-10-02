#!/bin/bash
# As oracle.sh, but RUNS each file (bash FILE), for cases whose header has
# to execute -- alias definitions, then `set -n` so the rest is only read.
B=${BASH_ORACLE:-$HOME/bash-5.2.15/bash}
cd "$1" || exit 1
shift
for f in "$@"; do
  out=$(LC_ALL=C "$B" "$f" 2>&1)
  st=$?
  printf '%s\t%s\t%s\n' "$f" "$st" "$(printf '%s' "$out" | tr '\n' '|')"
done
