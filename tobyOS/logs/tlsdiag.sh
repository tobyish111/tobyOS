#!/bin/bash
# tlsdiag.sh -- WHY DOES CHROME'S https FAIL? Get the EXACT error, from the two
# instruments this arc already built, in ONE boot.
#
# Real hardware (EliteDesk, 2026-08-14) fails every remote page load with
# net::ERR_FILE_PATH_TOO_LONG. That text is a red herring twice over: it is
# chrome's MapSystemError(ENAMETOOLONG) wording, but the -17 is emitted by
# ssl_client_socket_impl.cc INSIDE the TLS handshake, and a -DPATHFAIL_TRACE
# boot proved resolve_user_path never returns ENAMETOOLONG. So it is a TLS
# handshake failure wearing a filesystem error's name.
#
# The arc's own recorded lesson (slice 36 traps): "the fix was always to get
# the EXACT net_error, not reason from symptoms." Both instruments exist:
#
#   [https-probe]  tobyOS's OWN TLS 1.3 GET of https://example.com, sharing the
#                  kernel/socket/TCP path with chrome but using tobyOS's own
#                  BearSSL/ChaCha20 code. SUCCESS => kernel path sound, chrome
#                  is the differ. FAIL => the fault is shared.
#   [netlog]       chrome's OWN NetLog (--log-net-log), read back by the kernel
#                  and scanned for ssl_error/alert/cipher/exchange_group. This
#                  is what named X25519MLKEM768 in slice 36.
#
# Both live under CHROMIUM_BOOT (&& !TKAPP_BOOT) -- a one-shot BOOT harness, so
# a real-hardware run needs no clicking, just a serial capture.
#
# RUN THIS IN QEMU FIRST. https over X25519MLKEM768 has WORKED under TCG since
# slice 39, so a QEMU failure here means the recent networking cuts (route
# table, src_ip, NAT-before-local-delivery, per-ns ports) regressed it and it
# is fixable locally. A QEMU pass makes the same ISO the real-hardware probe.
#
# CAVEAT, stated because it matters: a CHROMIUM_BOOT flavour auto-logs-in as
# root. That is fine for a TLS question and wrong for a uid question.
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t; export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'

RAM="${RAM:-5120}"        # 826 MB initrd OOMs Limine's allocator at 512
BOOTSECS="${BOOTSECS:-1500}"
LOG="logs/tlsdiag.log"
BUILDLOG="logs/tlsdiag.build.log"

echo "=== [1/4] payload present? ==="
[ -f programs/chromium/chrome-headless-shell-linux64/chrome-headless-shell ] \
    || { echo "MISSING chrome payload -- run programs/chromium/build.sh"; exit 1; }
echo "  ok: chrome-headless-shell staged"

# FLAVOUR SWITCH. make keys off mtime, NOT -D flags, so objects from the
# previous (desktop + PATHFAIL_TRACE) build would be silently reused and the
# harness would be compiled OUT while the ISO looked fresh. Objects live in
# src/, so this is the correct incantation -- see the net-namespace memory,
# where a stale pmm.o linked against a dropped -D flag proved it.
echo "=== [2/4] dropping previous flavour's objects ==="
rm -f src/*.o
rm -f programs/chromewin/chromewin.o programs/chromewin/chromewin.elf
rm -f build/initrd.tar build/base.iso tobyOS.iso

CF="-DFAST_BOOT -DQUICK_BOOT -DCHROMIUM_BOOT -DPATHFAIL_TRACE"
echo "  EXTRA_CFLAGS=$CF"
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          iso EXTRA_CFLAGS="$CF" >"$BUILDLOG" 2>&1; then
    echo "BUILD FAIL -- tail:"; tail -40 "$BUILDLOG"; exit 1
fi
[ -f tobyOS.iso ] || { echo "BUILD FAIL (no ISO)"; tail -40 "$BUILDLOG"; exit 1; }
[ tobyOS.iso -nt tobyos.bin ] || { echo "BUILD FAIL: ISO older than tobyos.bin"; exit 1; }

# MARKER GATE. A grep-filtered build can hide "make: command not found", and a
# silently-absent harness reads exactly like a harness that found nothing.
# Assert every instrument this run depends on is actually IN the binary.
echo "=== [3/4] marker gate (the instrument must be compiled IN) ==="
gate_fail=0
for m in "https-probe" "netlog" "netlogdump" "pathfail"; do
    if grep -qa -- "$m" tobyos.bin; then echo "  ok: [$m] present"
    else echo "  FAIL: [$m] MISSING from tobyos.bin"; gate_fail=1; fi
done
[ "$gate_fail" = "0" ] || { echo "ABORT: instruments missing -- a null result would be meaningless"; exit 1; }
ls -la tobyOS.iso

echo "=== [4/4] boot in QEMU (TCG, -m $RAM, up to ${BOOTSECS}s) ==="
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1
: > "$LOG"
# NO -drive, DELIBERATELY. disk.img carries a tobyfs /data that SURVIVES
# reboots, and that wrecked the first verification attempt two ways at once:
# chrome inherited the previous run's profile (so it took a different startup
# path and died in SwiftShader), and when it died before writing a new
# netlog.json the kernel happily read and reported the PREVIOUS run's file --
# byte-identical output that looks like a reproduction but is an echo.
# Without a disk, /data is the RAM-backed fallback: fresh every boot by
# construction, and the same configuration the EliteDesk actually runs (its
# sweep finds no tobyfs volume either).
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
grep -aE '\[ramdata\]|is RAM-backed|mounted ./data.' "$LOG" | head -4
echo
echo "=== ANSWER 1: tobyOS's OWN TLS 1.3 (kernel path, no chrome) ==="
grep -aE '\[https-probe\]' "$LOG" | head
echo
echo "=== ANSWER 2: chrome's OWN NetLog -- the exact SSL error ==="
grep -aE '\[netlog\]' "$LOG" | head -40
echo
echo "=== ENAMETOOLONG trace (empty = the -17 is NOT errno-derived) ==="
grep -aE '\[pathfail\]' "$LOG" | head
echo
echo "=== THE DELIVERABLE: did the page actually load? ==="
if grep -aq 'Example Domain' "$LOG"; then
    echo "  PASS: chrome parsed the real https://example.com DOM"
    grep -aiE 'Example Domain|<h1>' "$LOG" | head -3
else
    echo "  NOT SEEN: no 'Example Domain' in the DOM dump"
fi
echo
echo "=== chrome outcome ==="
grep -aiE 'CHROMIUM\] VERDICT|chrome exit|DOCTYPE' "$LOG" | head -10
echo
echo "=== NIC counters for the chrome run (crc/miss > 0 indicts the silicon) ==="
grep -aE 'stats\(post-chrome\)' "$LOG" | head -2
echo
echo "=== faults (empty = clean) ==="
grep -aiE 'KERNEL PANIC|#GP|EXCEPTION [0-9]|unhandled syscall' "$LOG" | head
echo "=== full log: $LOG ==="
