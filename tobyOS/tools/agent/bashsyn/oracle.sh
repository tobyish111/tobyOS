#!/bin/bash
# bash 5.2.15's verdict on each FILE, as `bash -n` gives it, in the shape
# harness.c prints: NAME<TAB>STATUS<TAB>message|message|...
# Runs in WSL against the 5.2.15 build (~/bash-5.2.15/bash) -- the guest's
# exact version. Usage: oracle.sh [-O extglob] DIR FILE...   (names relative to DIR)
B=${BASH_ORACLE:-$HOME/bash-5.2.15/bash}
opts=()
if [ "$1" = -O ]; then opts=(-O "$2"); shift 2; fi
cd "$1" || exit 1
shift
for f in "$@"; do
  out=$(LC_ALL=C "$B" "${opts[@]}" -n "$f" 2>&1)
  st=$?
  printf '%s\t%s\t%s\n' "$f" "$st" "$(printf '%s' "$out" | tr '\n' '|')"
done
