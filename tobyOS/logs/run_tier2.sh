#!/bin/bash
# Tier-2 (thread-group semantics) validation chain:
#   1. make clean       -- struct proc + signal_state GREW (SIG_MAX 32->64)
#   2. lxposix gate     -- 14 subtests incl. linux-nptl
#   3. cross-personality gates (XPIPE/X2/3W) -- owed: signals + sched touched
#   4. defboot (logs/validate.sh)
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

echo "===== [1/4] make clean (signal_state/proc layout changed)"
make clean > /dev/null 2>&1

echo "===== [2/4] lxposix gate"
bash logs/lxposix.sh || { echo "TIER2: lxposix RED"; exit 1; }

echo "===== [3/4] cross-personality gates"
touch src/kernel.c
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTHREEWORLDS_BOOT -DXPIPE_BOOT -DX2_BOOT" iso \
          > logs/tier2x.build.log 2>&1; then
    echo "TIER2: XGATE BUILD FAILED"; grep -E ' error:' logs/tier2x.build.log | head -10
    exit 1
fi
rm -f logs/tier2x.log
timeout 240 "/c/Program Files/qemu/qemu-system-x86_64.exe" \
    -cdrom tobyOS.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk -boot d \
    -smp 4 -m 5120 \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial file:logs/tier2x.log -no-reboot -display none > /dev/null 2>&1
echo "== cross-personality verdicts:"
grep -aE 'XPIPE|X2PIPE|\[3W\]' logs/tier2x.log | sed 's/^\[[0-9 ]*ms\] //' | head -8
XR=0
grep -aq '\[XPIPE\] VERDICT: PASS' logs/tier2x.log || { echo "!! XPIPE not PASS"; XR=1; }
grep -aq '\[X2PIPE\] VERDICT: PASS' logs/tier2x.log || { echo "!! X2PIPE not PASS"; XR=1; }
grep -aq '\[3W\] VERDICT: PASS' logs/tier2x.log || { echo "!! THREEWORLDS not PASS"; XR=1; }
XF=$(grep -ac 'EXCEPTION\|user-mode fault\|PANIC' logs/tier2x.log)
[ "$XF" != "0" ] && { echo "!! faults=$XF"; XR=1; }
[ "$XR" != "0" ] && { echo "TIER2: cross-personality RED"; exit 1; }
echo "cross-personality GREEN"

echo "===== [4/4] defboot (3 runs x 90s, DEFAULT flavour)"
touch src/kernel.c
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" iso \
          > logs/tier2def.build.log 2>&1; then
    echo "TIER2: DEFAULT BUILD FAILED"; grep -E ' error:' logs/tier2def.build.log | head -10
    exit 1
fi
bash logs/validate.sh 3 90 tier2def || { echo "TIER2: defboot RED"; exit 1; }
echo "TIER2: ALL GREEN"
