#!/bin/bash
# The CRUD contract, asked of EVERY filesystem driver -- not just the three
# that userspace happens to have mounted.
#
#   bash logs/fsmatrix.sh
#
# logs/fsprobe.sh runs a full create/read/update/rename/truncate/chmod/delete
# matrix, but only over /, /etc, /tmp and /data -- ramfs, tmpfs and tobyfs.
# ext4/ext2/FAT metadata were never exercised by anything, and a NULL vfs op is
# not a stub: the VFS turns it into EROFS, so the gap is silent until some
# program needs it. `make`, `tar` and `cp -p` all care about timestamps.
#
# This formats a RAM-backed ext4 volume in the kernel and runs the same matrix
# against it, asserting VALUES (sizes, modes, uid/gid, the timestamp read back)
# rather than exit codes.
#
# Exit status is the result: 0 = green.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs

FLAGS="-DFAST_BOOT -DQUICK_BOOT -DFSMATRIX_SELFTEST"
LOG=logs/fsmatrix.log

echo "== building ($FLAGS)"
touch src/kernel.c
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          EXTRA_CFLAGS="$FLAGS" iso > logs/fsmatrix.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/fsmatrix.build.log | head -20
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
echo "== the matrix"
grep -a 'FSMATRIX' "$LOG" | sed 's/^\[[0-9 ]*ms\] //'

echo
echo "== gate"
P=$(grep -ac 'FSMATRIX\]   ok ' "$LOG")
F=$(grep -ac 'FSMATRIX\]  FAIL ' "$LOG")
echo "   checks: pass=$P fail=$F"
RC=0
grep -aq 'FSMATRIX. VERDICT: PASS' "$LOG" || RC=1
[ "$F" != "0" ] && RC=1
# A floor: a self-test that dies half way still reports no failures.
# Count every check that RAN -- passes alone would make the floor fire a
# second time for a run that is merely red, hiding the real reason.
T=$((P + F))
[ "$T" -lt 72 ] && { echo "   too few checks ran ($T, expected >=72)"; RC=1; }

if [ "$RC" = "0" ]; then echo "VERDICT: PASS"; else echo "VERDICT: FAIL"; fi
exit $RC
