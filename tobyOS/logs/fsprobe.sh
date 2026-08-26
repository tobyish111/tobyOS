#!/bin/bash
# The "usable machine" gate: filesystem CRUD, and /sys as a NON-ROOT user.
#
#   bash logs/fsprobe.sh
#
# WHY IT EXISTS. Every other harness in this tree is spawned from pid 0 and
# therefore runs as root. On 2026-08-24 the EliteDesk booted, logged in as
# `toby` (uid 1000), and lspci exited 1 -- on a tree whose PKGPROBE run had
# just printed a full, correct PCI listing. Root could read /sys; a user
# could not. No gate could see that, because no gate had ever dropped
# privileges.
#
# /bin/fsprobe runs twice inside one boot: as root, then as uid 1000. It
# asserts VALUES (byte counts, file sizes, modes, entry counts) rather than
# exit codes, because an editor/tool exiting 0 has never been evidence here.
#
# Exit status is the result: 0 = green.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs

FLAGS="-DFAST_BOOT -DQUICK_BOOT -DFSPROBE_BOOT -DGUITEXT_SELFTEST -DSCHEDGUARD_SELFTEST"
LOG=logs/fsprobe.log

echo "== building ($FLAGS)"
touch src/kernel.c
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          EXTRA_CFLAGS="$FLAGS" iso > logs/fsprobe.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/fsprobe.build.log | head -20
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
echo "== results"
grep -a 'FSPROBE' "$LOG" | sed 's/^\[[0-9 ]*ms\] //'

echo
echo "== do_switch context guard (the wake-after-death net) =="
grep -a 'SCHEDGUARD' "$LOG" | sed 's/^\[[0-9 ]*ms\] //'

echo "== fixed-cell text renderer (GUI grids: terminal, editor, viewer) =="
grep -a 'GUITEXT' "$LOG" | sed 's/^\[[0-9 ]*ms\] //'

echo
echo "== gate"
P=$(grep -ac '\[FSPROBE\]   ok ' "$LOG")
F=$(grep -ac '\[FSPROBE\]  FAIL ' "$LOG")
echo "   checks: pass=$P fail=$F"
RC=0
grep -aq 'FSPROBE. VERDICT: PASS' "$LOG" || RC=1
grep -aq 'GUITEXT. VERDICT: PASS' "$LOG" || { echo "   fixed-cell text renderer FAILED"; RC=1; }
grep -aq 'SCHEDGUARD. VERDICT: PASS' "$LOG" || { echo "   do_switch context guard FAILED"; RC=1; }
[ "$F" != "0" ] && RC=1
# A floor on the CHECK COUNT, not just on the verdict: a harness that dies
# half way still prints "fail=0" for everything it managed to reach.
[ "$P" -lt 140 ] && { echo "   too few checks ran ($P, expected ~150) -- the harness did not complete"; RC=1; }

if [ "$RC" = "0" ]; then echo "VERDICT: PASS"; else echo "VERDICT: FAIL"; fi
exit $RC
