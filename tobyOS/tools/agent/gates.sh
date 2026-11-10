#!/bin/bash
# gates.sh TAG [gate...] -- run QEMU gates one after another (each builds its own
# ISO and boots it), keep verdicts in logs/gates-TAG.txt, and COUNT KERNEL PANICS
# AND [dsched] LINES in each gate's log: a dead AP is invisible to the verdicts.
# Run at most three gates per call from an agent (the tool's background limit is
# 2 h): "shparity ttyparity oilspec", then "textgate distrotail lxposix".
cd "$(dirname "$0")/../.." || exit 1
TAG=$1; shift
GATES=${*:-"shparity ttyparity oilspec textgate distrotail lxposix"}
OUT=logs/gates-$TAG.txt
: > "$OUT"
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1
for g in $GATES; do
  git checkout -- initrd/etc/oilspec/FILTER 2>/dev/null
  case $g in
    lxposix) bash logs/lxposix.sh --full > logs/gate-$TAG-$g.log 2>&1 ;;
    *)       bash logs/$g.sh           > logs/gate-$TAG-$g.log 2>&1 ;;
  esac
  rc=$?
  {
    echo "=== $g rc=$rc  panics=$(grep -ac 'KERNEL PANIC' logs/gate-$TAG-$g.log) dsched=$(grep -ac DUAL-SCHEDULE logs/gate-$TAG-$g.log)"
    grep -a -E "VERDICT|POSIX  |BASH-ONLY|TEXTGATE:|gained|LOST|^  [0-9]{4} " logs/gate-$TAG-$g.log | head -40
  } >> "$OUT"
  taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1
done
git checkout -- initrd/etc/oilspec/FILTER 2>/dev/null
echo "ALL GATES DONE" >> "$OUT"
cat "$OUT"
