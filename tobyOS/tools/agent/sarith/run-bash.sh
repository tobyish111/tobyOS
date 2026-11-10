#!/bin/bash
# run-bash.sh DIR -- run gen.py's scripts with bash 5.2.15, the output
# normalised to harness.c's: "==== N" per case, "NAME: line N: " removed.
B=${BASH_ORACLE:-$HOME/bash-5.2.15/bash}
cd "$1" || exit 1
for f in $(ls | grep '\.sh$' | sort); do
  echo "==== $((10#${f%.sh}))"
  LC_ALL=C "$B" "$f" 2>&1 | sed 's/^[^:]*: line [0-9]*: //'
done
