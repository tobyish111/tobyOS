#!/bin/bash
# probe3.sh OUTPREFIX FILE -- like probe2.sh, for probes that start shells of
# their own: runs FILE as `SH=<shell> <shell> FILE` for bash and /bin/tsh,
# output to a file ON THE GUEST, fetched back 40 lines at a time (a long
# edremote reply gets cut short). Saves OUTPREFIX.bash / OUTPREFIX.tsh with
# "exit=N" last, and prints IDENTICAL / DIFFERENT.
cd "$(dirname "$0")/../.." || exit 1
OUT=$1; F=$2
n=$(basename "$F")
bash logs/edremote.sh "mkdir -p /tmp/pr; cat > /tmp/pr/$n" < "$F" >/dev/null 2>&1
mkdir -p "$(dirname "$OUT")"
for sh in bash tsh; do
  b=$(basename "$sh")
  lines=$(ED_TIMEOUT=${ED_TIMEOUT:-600} bash logs/edremote.sh "cd /tmp; SH=$sh $sh /tmp/pr/$n > /tmp/pr/$n.$b 2>&1; echo exit=\$? >> /tmp/pr/$n.$b; wc -l < /tmp/pr/$n.$b" 2>/dev/null | tr -d '\r ')
  : > "$OUT.$b"
  start=1
  while [ "$start" -le "${lines:-0}" ]; do
    bash logs/edremote.sh "sed -n '${start},$((start+39))p' /tmp/pr/$n.$b" 2>/dev/null | tr -d '\r' >> "$OUT.$b"
    start=$((start+40))
  done
done
# A run ends with its "exit=N" line. Without it the fetch failed -- an
# upload or session flake, often the first run after installing a new
# /bin/tsh -- and two EMPTY files would compare IDENTICAL: four probes were
# once reported identical that way (logs/b12 p85 p86 fn1 p38, batch 12).
for b in bash tsh; do
  if ! tail -1 "$OUT.$b" 2>/dev/null | grep -q '^exit='; then
    echo "$n NORESULT ($b side has no exit= line -- rerun)"; exit 3
  fi
done
if cmp -s "$OUT.bash" "$OUT.tsh"; then echo "$n IDENTICAL"; else echo "$n DIFFERENT ($(diff "$OUT.bash" "$OUT.tsh" | grep -c '^[<>]') lines)"; fi
