#!/bin/bash
# probe2.sh OUTPREFIX FILE [SHELLS] -- upload FILE to the guest ($ED_HOST etc.) and
# run it under each shell (default "bash /bin/tsh") from /tmp, stdout and stderr
# merged; save each to OUTPREFIX.<shell basename> with the exit status as the last
# line ("exit=N"), and print a one-line IDENTICAL/DIFFERENT verdict.
cd "$(dirname "$0")/../.." || exit 1
OUT=$1; F=$2; SHELLS=${3:-"bash /bin/tsh"}
n=$(basename "$F")
bash logs/edremote.sh "mkdir -p /tmp/pr; cat > /tmp/pr/$n" < "$F" >/dev/null 2>&1
mkdir -p "$(dirname "$OUT")"
outs=()
for sh in $SHELLS; do
  b=$(basename "$sh")
  ED_TIMEOUT=${ED_TIMEOUT:-600} bash logs/edremote.sh "cd /tmp; $sh /tmp/pr/$n 2>&1; echo exit=\$?" 2>/dev/null | tr -d '\r' > "$OUT.$b"
  outs+=("$OUT.$b")
done
if [ ${#outs[@]} -eq 2 ]; then
  if cmp -s "${outs[0]}" "${outs[1]}"; then echo "$n IDENTICAL"; else echo "$n DIFFERENT ($(diff "${outs[0]}" "${outs[1]}" | grep -c '^[<>]') lines)"; fi
fi
