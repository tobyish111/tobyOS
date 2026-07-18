#!/bin/bash
# chromium-m0.sh -- Track B M0: run REAL headless Chromium and collect the
# first gap list. See docs/chromium-bringup-m0.md.
#
# The deliverable is the gap list, not a pass/fail. Chromium self-identifies
# every wall in the serial log: missing shared libraries (ld.so) and missing
# Linux syscalls ("[linux] UNHANDLED syscall N ...").
#
# Prereq: bash programs/chromium/build.sh  (fetches the ~262 MB payload)
# Run under MSYS2 bash (the cross toolchain + make live on the MSYS2 PATH).
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
# Windows-native TMP so clang integrated-as + host tools can make temp files
# (works from both git-bash and msys2 bash -- see tobyos-build-env memory).
mkdir -p /c/t; export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'

RAM="${RAM:-4096}"
BOOTSECS="${BOOTSECS:-1200}"
LOG="logs/chromium-m0.log"
BUILDLOG="logs/chromium-m0.build.log"

echo "=== [1/4] verify payload present ==="
if [ ! -f programs/chromium/chrome-headless-shell-linux64/chrome-headless-shell ]; then
    echo "MISSING payload -- run: bash programs/chromium/build.sh"; exit 1
fi
du -sh programs/chromium/chrome-headless-shell-linux64

echo "=== [2/4] build ISO with CHROMIUM_BOOT (forcing initrd rebuild) ==="
# I edited src/syscall.c (trace) + src/kernel.c (harness); force both to rebuild.
# rm initrd.tar so the recipe re-tars WITH /opt/chrome; rm ISOs so they re-pack.
touch src/kernel.c src/syscall.c
rm -f build/initrd.tar build/base.iso tobyOS.iso
# Carry a Windows-native TMP prefix ON $(CC)/$(HOST_CC): exporting TMP gets
# mangled crossing MSYS make->sh, which breaks the .S integrated-assembler
# temp file (tobyos-build-env memory). This inline form survives.
# TRACE=1 adds the -DLINUX_SYSCALL_TRACE firehose (every Linux syscall entry) --
# used to see what chrome was doing right before a fault. Verbose + slower.
CF="-DFAST_BOOT -DQUICK_BOOT -DCHROMIUM_BOOT"
[ "${TRACE:-0}" = "1" ] && CF="$CF -DLINUX_SYSCALL_TRACE"
make "CC=TMP='C:\\t' TEMP='C:\\t' clang" "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
     iso EXTRA_CFLAGS="$CF" >"$BUILDLOG" 2>&1
if [ ! -f tobyOS.iso ]; then echo "ISO BUILD FAILED -- tail:"; tail -50 "$BUILDLOG"; exit 1; fi
tail -3 "$BUILDLOG"
grep -aE 'chromium:|initrd' "$BUILDLOG" | head
ls -la tobyOS.iso

echo "=== [3/4] boot in QEMU (-m $RAM, headless, up to ${BOOTSECS}s) ==="
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1
: > "$LOG"
"$QEMU" -cdrom tobyOS.iso \
  -drive file=disk.img,format=raw,if=ide,index=0,media=disk,cache=writethrough -boot d \
  -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -smp 4 -m "$RAM" -cpu qemu64,+smep,+smap -serial "file:$LOG" \
  -no-reboot -display none &
QPID=$!
for i in $(seq 1 "$BOOTSECS"); do
    grep -aq 'CHROMIUM\] M0 gap-list run complete' "$LOG" 2>/dev/null && { echo "[chrome exited]"; break; }
    grep -aq 'CHROMIUM] VERDICT: SKIP' "$LOG" 2>/dev/null && { echo "[SKIPPED -- no binary staged?]"; break; }
    grep -aq 'KERNEL PANIC' "$LOG" 2>/dev/null && { echo "[KERNEL PANIC]"; break; }
    sleep 1
done
sleep 2
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

echo "=== [4/4] GAP LIST ==="
echo "--- DSO tier: missing shared libraries (ld.so) ---"
grep -aiE 'cannot open shared object|error while loading shared|not found' "$LOG" | sort -u | head -60
echo "--- syscall tier: unhandled Linux syscalls (ranked, deduped) ---"
grep -aE '\[linux\] UNHANDLED syscall' "$LOG" | sed -E 's/ a=.*//' | sort -u | head -80
echo "--- chrome/ld.so stderr (how far it got) ---"
grep -aiE 'chrome|blink|v8|FATAL|ERROR:|Check failed|DOCTYPE|<h1>|tobyOS</h1>' "$LOG" | head -40
echo "--- faults ---"
grep -aiE 'KERNEL PANIC|#GP|EXCEPTION [0-9]|missing from VFS|page fault' "$LOG" | head -20
echo "=== log: $LOG  (grep -a for detail) ==="
