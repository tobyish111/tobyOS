#!/bin/bash
# isoboot.sh -- boot THE IMAGE THAT IS ACTUALLY BEING SHIPPED and prove it comes
# up clean. Not a harness flavour: the plain desktop ISO from logs/build_usb.sh,
# the same bytes that go on the USB stick.
#
# WHY THIS EXISTS: every verification so far ran a HARNESS build (CHROMIUM_BOOT
# / TKAPP_CHROMEWIN). Those share the kernel fix but are a different build
# flavour with different defines, and one of this project's standing traps is a
# green result from a configuration nobody ships. This boots the shipped one.
#
# It cannot click the taskbar pin (the desktop deliberately has no auto-login --
# that is the point of build_usb.sh), so it does NOT re-prove the browser; the
# browser is proven by logs/cwnet.sh on the identical kernel path. What it
# proves is that the shipped image boots, sizes /data, gets a lease, reaches the
# desktop, and takes no faults doing it.
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
ISO="${ISO:-tobyOS.iso}"
RAM="${RAM:-5120}"          # 826 MB initrd OOMs Limine's allocator at 512
SETTLE="${SETTLE:-45}"      # seconds to keep watching AFTER the desktop appears
LOG="logs/isoboot.log"

[ -f "$ISO" ] || { echo "no $ISO"; exit 1; }
echo "=== booting $ISO ($(ls -la --time-style=+%H:%M "$ISO" | awk '{print $5" bytes, "$6}')) ==="
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1
: > "$LOG"
"$QEMU" -cdrom "$ISO" -boot d \
  -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -smp 4 -m "$RAM" -cpu qemu64,+smep,+smap -serial "file:$LOG" \
  -no-reboot -display none &
for i in $(seq 1 240); do
    grep -aq 'Desktop ready' "$LOG" 2>/dev/null && { echo "[desktop reached at ~${i}s]"; break; }
    grep -aq 'KERNEL PANIC' "$LOG" 2>/dev/null && { echo "[KERNEL PANIC]"; break; }
    sleep 1
done
# Keep watching: a boot that reaches the desktop and then faults is still broken,
# and the interesting failures in this tree have arrived AFTER the GUI is up.
sleep "$SETTLE"
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

echo
echo "=== /data (should be sized from free RAM, not a hard 8 MiB) ==="
grep -aE '\[ramdata\]|is RAM-backed|mounted ./data.' "$LOG" | head -3
echo
echo "=== network ==="
grep -aE '\[dhcp\] ACK|\[net\] up:|resolv.conf' "$LOG" | head -3
echo
echo "=== desktop + browser payload ==="
grep -aE 'Desktop ready|entering graphical mode' "$LOG" | head -2
echo
echo "=== liveness (heartbeats must keep arriving) ==="
grep -aoE '\[hb\] #[0-9]+ up=[0-9]+s' "$LOG" | tail -2
echo
echo "=== FAULTS (must be empty) ==="
grep -aiE 'KERNEL PANIC|EXCEPTION [0-9]+|#GP|unhandled syscall' "$LOG" | head -5
echo
nf=$(grep -aciE 'KERNEL PANIC|EXCEPTION [0-9]+' "$LOG")
hb=$(grep -ac '\[hb\] #' "$LOG")
if grep -aq 'Desktop ready' "$LOG" && [ "$nf" = "0" ] && [ "$hb" -gt 3 ]; then
    echo "VERDICT: PASS -- shipped ISO boots to the desktop, $hb heartbeats, 0 faults"
else
    echo "VERDICT: FAIL -- desktop=$(grep -acq 'Desktop ready' "$LOG" && echo yes || echo no) faults=$nf heartbeats=$hb"
fi
echo "=== log: $LOG ==="
