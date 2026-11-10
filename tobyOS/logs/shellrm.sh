#!/bin/bash
# shellrm.sh -- can the OS delete a directory TREE? Gate for slice 127's rm -r.
#
# WHY: `rm` could only remove a file or an EMPTY directory, so an ordinary
# tree could not be deleted from tobyOS at all. Found the practical way -- a
# corrupt /data/cr2 (a chrome profile, thousands of files) needed clearing
# and no command could do it.
#
# Drives the KERNEL SHELL over the serial console via a -DSHELL_SCRIPT_BOOT
# style command stream is not available here, so instead this builds a boot
# hook that runs the sequence and prints assertions. Keeping it in-kernel
# avoids depending on a userspace shell that may not be staged.
#
#   PASS -> tree created, `rm` alone REFUSES it with the -r hint, `rm -r`
#           removes it, and the path is gone afterwards
#   FAIL -> any step reports otherwise
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t; export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
LOG=logs/shellrm.log

echo "=== [1/3] build with -DRMTREE_SELFTEST ==="
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1; sleep 1
rm -f src/*.o build/initrd.tar build/base.iso tobyOS.iso
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" iso \
     EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DRMTREE_SELFTEST" \
     >logs/shellrm.build.log 2>&1; then
    echo "BUILD FAILED -- tail:"; tail -20 logs/shellrm.build.log; exit 1
fi
grep -qa "RMTREE" tobyos.bin || { echo "FAIL: selftest not compiled in"; exit 1; }
echo "  ok: selftest marker in tobyos.bin"

echo "=== [2/3] boot ==="
: > "$LOG"
"$QEMU" -cdrom tobyOS.iso -boot d \
  -drive file=disk.img,format=raw,if=ide,index=0,media=disk,snapshot=on \
  -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -accel whpx,kernel-irqchip=off -smp 4 -m 4096 \
  -serial "file:$LOG" -no-reboot -display none &
for i in $(seq 1 200); do
  grep -aq 'RMTREE. VERDICT\|KERNEL PANIC' "$LOG" 2>/dev/null && break
  sleep 1
done
sleep 2
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1

echo "=== [3/3] VERDICT ==="
tr -d '\000' < "$LOG" | tr '\r' '\n' | grep -a "RMTREE" | head -20
echo
if tr -d '\000' < "$LOG" | tr '\r' '\n' | grep -aq "\[RMTREE\] VERDICT: PASS"; then
    echo "PASS: rm -r removes a tree; plain rm refuses one"
else
    echo "FAIL -- see $LOG"
fi
