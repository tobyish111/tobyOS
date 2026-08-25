#!/bin/bash
# The STORAGE PROVISIONING GUARD gate: what the kernel refuses to overwrite.
#
#   bash logs/provguard.sh
#
# This is the one subsystem in tobyOS allowed to write a partition table, so
# the assertions that matter are the REFUSALS. A permission model is only
# proven by what it says no to.
#
# NO DATA DISK IS ATTACHED, and that is load-bearing. src/provision.c's
# self-test says so in its own header ("Run it in a boot WITHOUT real data
# disks attached -- it temporarily takes /data over from the RAM fallback and
# restores it afterwards"), and three of its assertions encode that
# precondition: t1 expects the fake volume to become the LIVE /data, and t2
# then expects it to classify as MOUNTED. Boot with -drive disk.img and those
# three fail for a reason that has nothing to do with the guard -- which is
# exactly what happened when this self-test was first wired into the fsprobe
# gate, whose QEMU line does attach one.
#
# Exit status is the result: 0 = green.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs

FLAGS="-DFAST_BOOT -DQUICK_BOOT -DPROVISION_SELFTEST"
LOG=logs/provguard.log

echo "== building ($FLAGS)"
touch src/kernel.c
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          EXTRA_CFLAGS="$FLAGS" iso > logs/provguard.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/provguard.build.log | head -20
    exit 1
fi

echo "== running (timeout 240s, NO data disk by design)"
rm -f "$LOG"
timeout 240 "/c/Program Files/qemu/qemu-system-x86_64.exe" \
    -cdrom tobyOS.iso \
    -boot d -smp 4 -m 5120 \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial file:"$LOG" -no-reboot -display none > /dev/null 2>&1

echo
echo "== guard verdicts"
grep -a 'PROV\]' "$LOG" | sed 's/^\[[0-9 ]*ms\] //'

echo
echo "== gate"
P=$(grep -ac 'PROV\] PASS' "$LOG")
F=$(grep -ac 'PROV\] FAIL' "$LOG")
echo "   assertions: pass=$P fail=$F"
RC=0
grep -aq 'PROV. self-test complete: ALL PASS' "$LOG" || RC=1
[ "$F" != "0" ] && RC=1
# A floor, because a self-test that dies early still reports no failures.
[ "$P" -lt 18 ] && { echo "   too few assertions ran ($P, expected ~20)"; RC=1; }

if [ "$RC" = "0" ]; then echo "VERDICT: PASS"; else echo "VERDICT: FAIL"; fi
exit $RC
