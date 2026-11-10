#!/bin/bash
# hwupstress.sh N [FILE] -- upload FILE (default logs/b27/bigexit, 841 KB) N
# times over SSH (logs/edremote.sh; ED_HOST/ED_PORT as set) and check each
# copy's md5. Prints each failure and the tally; exits 1 on any. Finding
# 166: before its fix 4 of 40 failed on the EliteDesk (none in QEMU).
cd "$(dirname "$0")/../.." || exit 1
n=${1:-40}
f=${2:-logs/b27/bigexit}
want=$(md5sum "$f" | cut -c1-32)
ok=0 bad=0
for i in $(seq 1 "$n"); do
  s=$(date +%s)
  r=$(ED_TIMEOUT=${UP_TIMEOUT:-60} bash logs/edremote.sh "cat > /tmp/up.$((i % 3)); md5sum /tmp/up.$((i % 3))" < "$f" 2>&1 | tr -d '\r' | tail -1)
  case $r in
    "$want"*) ok=$((ok + 1)) ;;
    *) bad=$((bad + 1)); echo "try $i: $(( $(date +%s) - s ))s FAILED: ${r:0:90}" ;;
  esac
done
# the box's /tmp is a 16 MB tmpfs: leave it as it was (utilgate's du cases
# failed for want of room behind these copies once)
bash logs/edremote.sh 'rm -f /tmp/up.0 /tmp/up.1 /tmp/up.2' > /dev/null 2>&1
echo "UPSTRESS: ok=$ok bad=$bad (host ${ED_HOST}:${ED_PORT:-22})"
[ "$bad" = 0 ]
