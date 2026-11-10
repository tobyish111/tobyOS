#!/bin/bash
# TobyFS crash-consistency + stress harness, as a gate.
#
#   bash logs/tfsstress.sh
#
# fsmatrix.sh proves the CRUD contract. This proves the JOURNAL: rollback on
# a power loss before the commit marker, replay on one after it, the
# double-indirect path at 4.5 MiB, and a churn run whose survivors are
# re-verified byte for byte. Worth its own gate whenever anything changes how
# transactions are opened, committed, or how blocks are freed.
#
# Exit status is the result: 0 = green.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs

FLAGS="-DFAST_BOOT -DQUICK_BOOT -DTOBYFS_STRESS_SELFTEST"
LOG=logs/tfsstress.log

echo "== building ($FLAGS)"
touch src/kernel.c
if ! make "CC=TMP='C:\t' TEMP='C:\t' clang" \
          "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
          EXTRA_CFLAGS="$FLAGS" iso > logs/tfsstress.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/tfsstress.build.log | head -20
    exit 1
fi

echo "== running (timeout 240s)"
rm -f "$LOG"
timeout 240 "/c/Program Files/qemu/qemu-system-x86_64.exe" \
    -cdrom tobyOS.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -boot d -smp 4 -m 5120 \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial file:"$LOG" -no-reboot -display none > /dev/null 2>&1

echo
echo "== the harness"
grep -a 'TFST' "$LOG" | sed 's/^\[[0-9 ]*ms\] //'

echo
echo "== gate"
# The harness prints its own verdict; trust that, but also refuse a run that
# never reached it (a boot that died early must not read as green).
if ! grep -aq 'TFST\] PASS' "$LOG"; then
    if grep -aq 'TFST\]' "$LOG"; then
        echo "VERDICT: FAIL"
    else
        echo "VERDICT: FAIL -- harness never ran (boot died before it?)"
    fi
    exit 1
fi
echo "VERDICT: PASS"
exit 0
