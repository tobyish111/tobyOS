#!/bin/bash
# Slice 16 capstone gate: run a real OCI container over the Alpine rootfs.
#
#   bash logs/lxcontainer.sh [--clean]
#
# Exit status is the result: 0 = green, 1 = something failed.
#
# The verdict line is NOT the whole gate. It is backed by:
#   - the probe's 8-bit mask (every bit asserts a value; see
#     programs/linux-ctrprobe/main.c), and
#   - the runtime's own report of what it applied, because a container that
#     runs while quietly skipping half its config would otherwise look
#     identical to one that honoured all of it.
#
# The bundle's tmpfs /dev used to be UNSUPPORTED, and this gate asserted
# exactly one "NOT APPLIED: /dev" so the runtime's reporting stayed honest.
# tmpfs now exists (src/tmpfs.c), so that expectation INVERTED: /dev must be
# applied, and a "NOT APPLIED: /dev" is now a regression. The old comment said
# "if that line disappears, either tmpfs appeared (good, update this) or the
# runtime stopped reporting what it skipped (bad)" -- this is the first case,
# updated.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs

FLAGS="-DFAST_BOOT -DQUICK_BOOT -DLXCONTAINER_BOOT"
TAG="${TAG:-run}"
LOG="logs/lxcontainer_$TAG.log"
TIMEOUT="${TIMEOUT:-420}"

MK=(make "CC=TMP='C:\\t' TEMP='C:\\t' clang" "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc")

if [ -f "$LOG" ]; then
    echo "!! $LOG exists -- pass TAG=<something-new>. Two QEMUs writing one"
    echo "   filename is indistinguishable from a flaky kernel."
    exit 1
fi
LEFT=$(tasklist 2>/dev/null | grep -ci qemu-system)
if [ "${LEFT:-0}" != "0" ]; then
    echo "!! $LEFT qemu still running; taskkill //F //IM qemu-system-x86_64.exe"
    exit 1
fi

for p in linux-ocirun linux-ctrprobe; do
    if [ ! -f "programs/$p/$p.elf" ]; then
        echo "!! programs/$p/$p.elf missing -- run: bash programs/$p/build.sh"
        exit 1
    fi
done

if [ "$1" = "--clean" ]; then
    echo "== make clean"
    "${MK[@]}" clean > logs/lxcontainer.build.log 2>&1
fi

echo "== building ($FLAGS)"
touch src/kernel.c        # EXTRA_CFLAGS changes do NOT trigger a recompile
if ! "${MK[@]}" EXTRA_CFLAGS="$FLAGS" iso >> logs/lxcontainer.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/lxcontainer.build.log | head -30
    exit 1
fi

# Gate on the BINARY: the harness marker must be present, and the bundle and
# both opt-in programs must actually be inside the initrd. A build that merely
# succeeded proves none of that.
python -c "
import sys, tarfile
d = open('tobyos.bin','rb').read()
fails = []
if b'[LXCONTAINER]' not in d: fails.append('kernel has no LXCONTAINER harness')
try:
    names = set(tarfile.open('build/initrd.tar').getnames())
except Exception as e:
    names = set(); fails.append('cannot read build/initrd.tar: %s' % e)
for want in ('bin/linux-ocirun','bin/linux-ctrprobe','oci-bundle/config.json',
             'alpine.tar.gz'):
    if want not in names and './'+want not in names:
        fails.append('initrd is missing %s' % want)
for f in fails: print('   !!', f)
sys.exit(1 if fails else 0)
" || { echo "BINARY GATE FAILED"; exit 1; }
echo "   ok  harness + runtime + probe + bundle + rootfs all present"

echo "== running (timeout ${TIMEOUT}s)"
timeout "$TIMEOUT" "/c/Program Files/qemu/qemu-system-x86_64.exe" \
    -cdrom tobyOS.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -boot d -smp 4 -m 5120 \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial file:"$LOG" -no-reboot -display none > /dev/null 2>&1
taskkill //F //IM qemu-system-x86_64.exe > /dev/null 2>&1

echo
echo "== evidence"
grep -aE '\[LXCONTAINER\]|\[CTRSETUP\]|\[ocirun\]|\[CTR\]' "$LOG" \
    | sed 's/^\[[0-9 ]*ms\] //' | head -70

RC=0
grep -aq 'LXCONTAINER. VERDICT: PASS' "$LOG" || { echo "   !! no PASS verdict"; RC=1; }
grep -aq 'CTR. BITS=0xff' "$LOG"             || { echo "   !! probe mask is not 0xff"; RC=1; }

# The runtime must SAY what it applied and what it did not.
grep -aq 'ocirun.   root: pivot_root into' "$LOG" || \
    { echo "   !! no pivot_root line -- the root move did not happen"; RC=1; }
NA=$(grep -ac 'NOT APPLIED: /dev' "$LOG")
[ "$NA" = "0" ] || { echo "   !! /dev was NOT APPLIED ($NA) -- tmpfs exists now, so this is a regression"; RC=1; }
grep -aq 'ocirun.   mount: /dev type tmpfs' "$LOG" ||     { echo "   !! no tmpfs /dev mount line -- the container has no /dev"; RC=1; }

HB=$(grep -ac '\[hb\]' "$LOG")
FAULTS=$(grep -ac 'EXCEPTION\|user-mode fault\|PANIC' "$LOG")
echo
echo "== health: heartbeats=$HB faults=$FAULTS   log=$LOG"
[ "$HB" -lt 3 ] && { echo "   !! guest did not stay live"; RC=1; }
[ "$FAULTS" != "0" ] && { echo "   !! faults present"; RC=1; }

echo
[ "$RC" = "0" ] && echo "RESULT: GREEN" || echo "RESULT: RED"
exit $RC
