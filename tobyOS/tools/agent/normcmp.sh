#!/bin/bash
# normcmp.sh DIR -- for each X.bash/X.tsh pair in DIR: SAME (byte-identical),
# NUMS (identical once every digit run is N -- look at which numbers), or
# DIFF (differs in words).
cd "$1" || exit 1
for b in *.bash; do
  n=${b%.bash}
  [ -f "$n.tsh" ] || continue
  if cmp -s "$b" "$n.tsh"; then echo "SAME $n"; continue; fi
  if cmp -s <(sed -E 's/ *[0-9]+/ N/g' "$b") <(sed -E 's/ *[0-9]+/ N/g' "$n.tsh"); then
    # the numbers that differ, bash->tsh, pairwise by position
    nums=$(paste -d'|' <(grep -oE '[0-9]+' "$b") <(grep -oE '[0-9]+' "$n.tsh") | awk -F'|' '$1!=$2' | head -6 | tr '\n' ' ')
    echo "NUMS $n  $nums"
  else
    echo "DIFF $n ($(diff <(sed -E 's/ *[0-9]+/ N/g' "$b") <(sed -E 's/ *[0-9]+/ N/g' "$n.tsh") | grep -c '^[<>]') lines)"
  fi
done
