#!/bin/bash
# oracle-wrap.sh DIR OUTDIR -- run bash 5.2.15 on each DIR/*.sh (a corpus
# script wrapped as f() { ... }; declare -f f), keep stdout as OUTDIR/NAME.out
# and the exit status as OUTDIR/NAME.rc. Nothing in the body runs.
B=/home/toby/bash-5.2.15/bash
mkdir -p "$2"
for f in "$1"/*.sh; do
  n=${f##*/}
  ( cd /tmp && LC_ALL=C timeout 10 "$B" --norc --noprofile "$f" > "$2/$n.out" 2>/dev/null < /dev/null; echo $? > "$2/$n.rc" )
done
echo done
