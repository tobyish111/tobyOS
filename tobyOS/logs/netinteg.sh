#!/bin/bash
# netinteg.sh -- do large HTTPS bodies arrive BYTE-EXACT, repeatedly?
#
# Chases two real-hardware symptoms that both look like something else:
#   * chrome: "Uncaught SyntaxError: Invalid or unexpected token" parsing
#     gstatic's recaptcha__en.js. V8 does not fail on Google's own minified JS
#     unless the bytes were wrong.
#   * a different run: 3x ERR_SSL_PROTOCOL_ERROR in the first navigation burst,
#     then none for 70 seconds.
# Both intermittent; a 667 KB Wikipedia article rendered clean in between. That
# is the shape that hides, and the shape a single run cannot settle.
#
# READ THIS BEFORE TRUSTING A GREEN RUN HERE: the corruption has ONLY ever been
# seen on real hardware (EliteDesk, e1000e/I217). QEMU with its e1000 has never
# shown it. So QEMU is the CONTROL -- it proves the harness fetches, hashes and
# compares correctly. It cannot prove the bug is absent. The run that matters
# is the same ISO on the EliteDesk.
#
# The ISO is the ORDINARY DESKTOP plus the probe, so the same stick is still a
# usable machine: these harnesses run after the GUI is up.
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t; export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'
RAM="${RAM:-5120}"
LOG="logs/netinteg.log"

echo "=== [1/3] build (desktop + NETINTEG_BOOT) ==="
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1
rm -f src/*.o programs/chromewin/chromewin.o programs/chromewin/chromewin.elf
rm -f build/initrd.tar build/base.iso tobyOS.iso
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" iso \
     EXTRA_CFLAGS="-DNETINTEG_BOOT" >logs/netinteg.build.log 2>&1; then
    echo "BUILD FAIL -- tail:"; tail -25 logs/netinteg.build.log; exit 1
fi
[ -f tobyOS.iso ] || { echo "BUILD FAIL (no ISO)"; exit 1; }
grep -qa "NETINTEG" tobyos.bin \
    && echo "  ok: [NETINTEG] compiled in" \
    || { echo "  FAIL: probe missing -- a silent boot would mean nothing"; exit 1; }
# Same destructive-harness rule build_usb.sh enforces: this probe only FETCHES,
# but a stray auto-provision flavour must never reach a USB stick.
for bad in PROVBOOT LXCONTAINER; do
    grep -qa -- "$bad" tobyos.bin && { echo "  FAIL: destructive harness [$bad] present"; exit 1; }
done
echo "  ok: no destructive harness"

echo "=== [2/3] QEMU control run (-m $RAM, no disk => fresh /data) ==="
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
: > "$LOG"
"$QEMU" -cdrom tobyOS.iso -boot d \
  -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -smp 4 -m "$RAM" -cpu qemu64,+smep,+smap -serial "file:$LOG" \
  -no-reboot -display none &
for i in $(seq 1 420); do
    grep -aq 'NETINTEG. VERDICT' "$LOG" 2>/dev/null && break
    grep -aq 'KERNEL PANIC' "$LOG" 2>/dev/null && break
    sleep 1
done
sleep 3
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

echo
echo "=== [3/3] per-fetch hashes ==="
grep -aE '\[NETINTEG\]' "$LOG" | head -14
echo
echo "--- TCP health during the run (ooo>0 or retx>0 is the mechanism) ---"
grep -aoE 'ooo=[0-9]+ retx=[0-9]+' "$LOG" | sort | uniq -c | head -5
echo "--- NIC counters (crc/miss indict the silicon) ---"
grep -aE 'e1000e. stats|\[e1000\] stats' "$LOG" | tail -2
echo
grep -a 'NETINTEG. VERDICT' "$LOG" | tail -1
echo "=== log: $LOG   |  ISO: tobyOS.iso (desktop + probe, safe to flash) ==="
