#!/bin/bash
# Linux slice 4: the POSIX acceptance gate, as ONE command.
#
#   bash logs/lxposix.sh            # gate only  (fast: ~2 min)
#   bash logs/lxposix.sh --full     # gate + the whole Linux/Win32/cross suite
#
# Why this exists: slices 1-3 each added their own -D harness, so a regression
# run had become "remember 18 flags and eyeball 24 verdict lines". Anything
# that tedious gets skipped, and a gate that gets skipped is not a gate.
#
# Exit status is the result: 0 = green, 1 = something failed. So this can be
# used as a pre-commit check without reading the output.
#
# METHOD NOTES baked in (each one cost a debugging session at some point):
#   - EXTRA_CFLAGS changes do NOT trigger a recompile; touch src/kernel.c.
#   - Build and QEMU want a real Windows-native TMP; MSYS mangles it unless the
#     value rides ON the compiler variable.
#   - Never hide the build behind >/dev/null: a failed build silently leaves
#     the PREVIOUS iso in place and you test stale bits.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs

FULL=0
[ "$1" = "--full" ] && FULL=1

FLAGS="-DFAST_BOOT -DQUICK_BOOT -DLXPOSIX_BOOT"
LOG=logs/lxposix.log
TIMEOUT=240
if [ "$FULL" = "1" ]; then
    FLAGS="$FLAGS -DLINUXABI_BOOT -DLINUXBB_BOOT -DLINUXDYN_BOOT -DLINUXSH_BOOT"
    FLAGS="$FLAGS -DLXGLIBC_BOOT -DLXGLIBCDYN_BOOT -DLXPTY_BOOT -DLXTTY_BOOT"
    FLAGS="$FLAGS -DLXPROCFS_BOOT -DLXPROCFS2_BOOT -DREALBASH_BOOT -DREALPYTHON_BOOT"
    FLAGS="$FLAGS -DWINPE_BOOT -DWINPE5_BOOT"
    # Phase 3 slice 8: the namespace gate rides along with --full, so slices
    # 9-16 cannot silently regress namespaces while every other verdict stays
    # green. logs/lxns.sh remains the fast focused loop for working ON them.
    # (This is why --full now reports 23 PASS rather than 22 -- one more
    # verdict line, not a change in what was already passing.)
    FLAGS="$FLAGS -DLXNS_BOOT"
    LOG=logs/lxposix-full.log
    TIMEOUT=360
fi

echo "== building ($FLAGS)"
touch src/kernel.c
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          EXTRA_CFLAGS="$FLAGS" iso > logs/lxposix.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/lxposix.build.log | head -20
    exit 1
fi

echo "== running (timeout ${TIMEOUT}s)"
rm -f "$LOG"
timeout "$TIMEOUT" "/c/Program Files/qemu/qemu-system-x86_64.exe" \
    -cdrom tobyOS.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -boot d -smp 4 -m 5120 \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial file:"$LOG" -no-reboot -display none > /dev/null 2>&1

echo
echo "== gate"
grep -a 'LXPOSIX' "$LOG" | sed 's/^\[[0-9 ]*ms\] //'

RC=0
grep -aq 'LXPOSIX. VERDICT: PASS' "$LOG" || RC=1

if [ "$FULL" = "1" ]; then
    echo
    echo "== full suite"
    P=$(grep -ac 'VERDICT: PASS' "$LOG")
    F=$(grep -ac 'VERDICT: FAIL' "$LOG")
    echo "   PASS=$P FAIL=$F"
    grep -a 'VERDICT: FAIL' "$LOG" | sed 's/^\[[0-9 ]*ms\] //' | cut -c1-90
    [ "$F" != "0" ] && RC=1
fi

# Liveness + fault gates. A run that wedged or faulted is not a pass even if
# the verdict line looks green -- the verdict may simply predate the damage.
HB=$(grep -ac '\[hb\]' "$LOG")
FAULTS=$(grep -ac 'EXCEPTION\|user-mode fault\|PANIC' "$LOG")
echo
echo "== health: heartbeats=$HB faults=$FAULTS"
[ "$HB" -lt 3 ] && { echo "   !! guest did not stay live"; RC=1; }
[ "$FAULTS" != "0" ] && { echo "   !! faults present"; RC=1; }

echo
[ "$RC" = "0" ] && echo "RESULT: GREEN" || echo "RESULT: RED"
exit $RC
