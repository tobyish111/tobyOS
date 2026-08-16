#!/bin/bash
# Run-only half of tlsdiag.sh: boot the ALREADY-BUILT tobyOS.iso and report.
# Split out because rebuilding to re-run a boot wastes ~4 minutes, and the
# interesting variable here is the boot, not the build.
#
# NO -drive: see tlsdiag.sh for why (persistent /data on disk.img gave a stale
# netlog that read exactly like a fresh reproduction).
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
RAM="${RAM:-5120}"
BOOTSECS="${BOOTSECS:-1500}"
LOG="${LOG:-logs/tlsdiag.log}"

[ -f tobyOS.iso ] || { echo "no tobyOS.iso -- run logs/tlsdiag.sh first"; exit 1; }
echo "=== booting $(ls -la tobyOS.iso | awk '{print $6,$7,$8}') (-m $RAM, no disk) ==="
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1
: > "$LOG"
"$QEMU" -cdrom tobyOS.iso -boot d \
  -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -smp 4 -m "$RAM" -cpu qemu64,+smep,+smap -serial "file:$LOG" \
  -no-reboot -display none &
for i in $(seq 1 "$BOOTSECS"); do
    grep -aq 'netlogdump. END' "$LOG" 2>/dev/null && { echo "[harness complete]"; break; }
    grep -aq 'netlog. /data/netlog.json not present' "$LOG" 2>/dev/null && { echo "[no netlog]"; break; }
    grep -aq 'KERNEL PANIC' "$LOG" 2>/dev/null && { echo "[KERNEL PANIC]"; break; }
    sleep 1
done
sleep 2
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

echo
echo "=== /data provenance (RAM-backed => any netlog below IS from this run) ==="
grep -aE '\[ramdata\]|is RAM-backed' "$LOG" | head -3
echo
echo "=== ANSWER 1: tobyOS's OWN TLS 1.3 (kernel path, no chrome) ==="
grep -aE '\[https-probe\]' "$LOG" | head
echo
echo "=== ANSWER 2: chrome's OWN NetLog -- was net_error:-17 before the fix ==="
grep -aE '\[netlog\] ' "$LOG" | head -20
echo
echo "=== THE DELIVERABLE: did the page actually load? ==="
if grep -aq 'Example Domain' "$LOG"; then
    echo "  PASS: chrome parsed the real https://example.com DOM"
    grep -aiE 'Example Domain' "$LOG" | head -2
else
    echo "  NOT SEEN: no 'Example Domain' in the DOM dump"
fi
echo
echo "=== chrome outcome ==="
grep -aiE 'CHROMIUM\] VERDICT|M0 gap-list run complete|chrome exit' "$LOG" | head -5
echo
echo "=== NIC counters for the chrome run ==="
grep -aE 'stats\(post-chrome\)' "$LOG" | head -2
echo
echo "=== faults ==="
grep -aiE 'KERNEL PANIC|EXCEPTION [0-9]' "$LOG" | head -5
echo "=== full log: $LOG ==="
