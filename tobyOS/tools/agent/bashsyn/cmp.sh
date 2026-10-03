#!/bin/bash
# cmp.sh WRAPDIR ORACLEDIR OUTDIR -- prharness over WRAPDIR (mkwrap.py's
# output), each result compared with bash's (oracle-wrap.sh). Names the
# differing files in OUTDIR/diffs.txt.
H=$(cd "$(dirname "$0")" && pwd)/prharness.exe
W=$1; O=$2; G=$3
rm -rf "$G"; mkdir -p "$G"
ls "$W"/*.sh | xargs -n 300 "$H" "$G"
same=0; diff=0; : > "$G/diffs.txt"
for f in "$O"/*.out; do
  n=${f##*/}
  [ -s "$f" ] || continue
  if cmp -s "$f" "$G/$n"; then same=$((same+1)); else diff=$((diff+1)); echo "$n" >> "$G/diffs.txt"; fi
done
echo "same=$same diff=$diff"
