#!/bin/bash
# Page-reclaim acceptance gate.
#
#   bash logs/reclaim.sh
#
# Proves that evicting anonymous pages to swap and faulting them back in
# round-trips the data byte-for-byte. /bin/reclaimtest fills 256 pages with a
# pattern keyed on BOTH page index and byte offset, sleeps (parking itself in
# PROC_BLOCKED, the only state reclaim evicts from), then verifies every byte.
# Three rounds, re-dirtied between each.
#
# Exit status is the result: 0 = green, 1 = something failed.
#
# WHY THE CHECKS BELOW ARE WHAT THEY ARE:
#   - The child's exit code is the verdict. Reclaim's failure mode is silent
#     data corruption, so "the guest stayed alive" would pass over exactly the
#     bug this exists to find.
#   - A pass ALSO requires evicted>0 and faulted_back>0. A run where nothing
#     was ever evicted would exit 0 having tested nothing -- the green-test-
#     that-never-ran trap this tree keeps hitting.
#   - Faults and heartbeats are gated too: a verdict line can predate damage.
#
# -smp 1 IS DELIBERATE. Reclaim only evicts from a process that is provably not
# executing (see reclaim_candidate: the copy-then-unmap order loses any write
# the victim makes during the copy). This kernel's sys_nanosleep yield/pause-
# spins rather than blocking, so on -smp 4 the sleeping child sits RUNNING on
# an AP essentially always and is never an eligible victim -- measured: the
# whole run evicted 0 pages. On one CPU, pid 0 running the reclaim loop implies
# the child is descheduled, which is exactly the domain reclaim supports.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs

FLAGS="-DFAST_BOOT -DQUICK_BOOT -DRECLAIM_BOOT"
LOG=logs/reclaim.log

echo "== building ($FLAGS)"
touch src/kernel.c
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          EXTRA_CFLAGS="$FLAGS" iso > logs/reclaim.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/reclaim.build.log | head -20
    exit 1
fi

# Gate on the BINARY, not on make's exit code alone: assert the harness string
# is actually in the kernel we are about to boot.
python -c "
import sys
d = open('tobyos.bin','rb').read()
sys.exit(0 if b'/bin/reclaimtest' in d else 1)
" || { echo "FAIL: tobyos.bin does not reference /bin/reclaimtest"; exit 1; }

echo "== running (timeout 240s)"
rm -f "$LOG"
timeout 240 "/c/Program Files/qemu/qemu-system-x86_64.exe" \
    -cdrom tobyOS.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -boot d -smp 1 -m 2048 \
    -serial file:"$LOG" -no-reboot -display none > /dev/null 2>&1

echo
echo "== reclaim"
grep -a 'RECLAIM\]' "$LOG" | sed 's/^\[[0-9 ]*ms\] //'

RC=0
grep -aq 'RECLAIM\] VERDICT: PASS' "$LOG" || RC=1
grep -aq 'RECLAIM\] VERDICT: FAIL' "$LOG" && RC=1

EVICT=$(grep -ac '\[reclaim\] evicted' "$LOG")
echo
echo "== eviction batches logged: $EVICT"
[ "$EVICT" = "0" ] && { echo "   !! nothing was ever evicted"; RC=1; }

HB=$(grep -ac '\[hb\]' "$LOG")
FAULTS=$(grep -ac 'EXCEPTION\|user-mode fault\|PANIC' "$LOG")
echo "== health: heartbeats=$HB faults=$FAULTS"
[ "$HB" -lt 3 ] && { echo "   !! guest did not stay live"; RC=1; }
[ "$FAULTS" != "0" ] && { echo "   !! faults present"; RC=1; }

echo
[ "$RC" = "0" ] && echo "RESULT: GREEN" || echo "RESULT: RED"
exit $RC
