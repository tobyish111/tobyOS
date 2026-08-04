#!/bin/bash
# Default-config boot smoke: rebuild stock ISO (no test harnesses) and confirm
# the desktop/compositor comes up with no panic -- the heaviest native-ELF path.
cd /c/CustomOS/tobyOS || exit 1
# Slice 61f: same toolchain PATH the build scripts export -- defboot ran
# `make` from the ambient PATH and silently depended on the caller's shell.
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
touch src/kernel.c
make iso >logs/defboot.build.log 2>&1
[ -f tobyOS.iso ] || { echo "ISO BUILD FAIL"; tail -20 logs/defboot.build.log; exit 1; }
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
LOG=logs/defboot.log
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1; : > "$LOG"
# Slice 61c: -m 512 -> 4096. When the chromium payload is staged (build/initrd
# carries /opt/chrome, ~392 MB) the initrd alone overflows 512 MB and the
# kernel panics "High memory allocator: Out of memory" BEFORE any marker --
# and the old fault grep (KERNEL PANIC|EXCEPTION) missed the plain "PANIC:"
# prefix, so the gate reported CLEAN on a panicked boot. Both fixed.
"$QEMU" -cdrom tobyOS.iso \
  -drive file=disk.img,format=raw,if=ide,index=0,media=disk,cache=writethrough -boot d \
  -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -smp 4 -m 4096 -cpu qemu64,+smep,+smap -serial "file:$LOG" -no-reboot -display none &
# Slice 107: 40s is NOT enough once the initrd carries chrome. MEASURED: an
# 849 MB chrome-flavour ISO spends ~72s in limine loading the initrd before
# the kernel prints its first byte -- the guest itself then reaches login in
# 1.3s. The old window expired mid-load and printed an EMPTY marker list with
# an empty fault list, which reads exactly like a clean boot that reached
# nothing: a false alarm that costs a regression hunt. Wait long enough for
# the worst case, but still break the moment userspace appears.
for i in $(seq 1 180); do grep -aqE 'window_create|compositor|desktop|login' "$LOG" 2>/dev/null && break; sleep 1; done
sleep 3
if ! grep -aqE 'window_create|compositor|desktop|login' "$LOG" 2>/dev/null; then
    echo "WARNING: no userspace marker after ${i}s -- log is $(stat -c%s "$LOG" 2>/dev/null || echo 0) bytes."
    echo "         If it stops at limine's 'Loading module', the initrd load is"
    echo "         still running and this is a TIMEOUT, not a boot failure."
fi
echo "=== late-boot markers ==="
grep -aE 'window_create|compositor|session|login|reached desktop|GUI' "$LOG" | tail -8
echo "=== faults (empty=clean) ==="
grep -aiE 'PANIC|EXCEPTION [0-9]+:|#GP|#PF|unhandled' "$LOG" | grep -viE 'SetUnhandledException' | head
echo "(end)"
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1
# Slice 61f: this script rebuilt kernel.c STOCK (no CHROMIUM/TKAPP defines)
# and rewrote tobyOS.iso. Re-touch kernel.c so the NEXT flavored build
# (build_vid/build39) recompiles it with its own flags instead of silently
# reusing the stock kernel.o -- and remember the ISO on disk is now STOCK.
touch src/kernel.c
echo "NOTE: tobyOS.iso is now the STOCK flavor; run build39.sh or build_vid.sh before any chromium batch."
