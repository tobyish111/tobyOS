#!/bin/bash
# usbprov.sh -- prove the WHOLE persistent-USB path before it costs a
# real-hardware round trip: xHCI sees the stick -> usb_msc registers it as a
# block device -> the provisioning guard accepts it -> tobyfs is written ->
# AND IT COMES BACK AFTER A REBOOT.
#
# This is the flow the user is about to attempt on the EliteDesk, where a USB
# stick is the ONLY route to persistent /data: the internal disk is a Windows
# disk (FOREIGN, refused by design) with 3.0 MiB unallocated, and provisioning
# is whole-disk anyway.
#
# QEMU attaches the stick to qemu-xhci exactly as real SuperSpeed hardware
# does, so this exercises the same driver path. What it CANNOT prove is that
# the EliteDesk's firmware routes a given port to xHCI at all -- on Intel PCH
# the USB2 pairs are routed by XUSB2PR, which tobyOS deliberately leaves alone
# so the BIOS keeps the legacy-emulated keyboard. Hence the port census: it
# separates "no device was connected" from "we failed to enumerate one".
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t; export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
RAM="${RAM:-5120}"
STICK=/c/t/usbstick.img

echo "=== [1/4] build (PROVISION_BLANK_BOOT: auto-provision the blank stick) ==="
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1
rm -f src/*.o build/initrd.tar build/base.iso tobyOS.iso
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" iso \
     EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DPROVISION_BLANK_BOOT" \
     >logs/usbprov.build.log 2>&1; then
    echo "BUILD FAIL -- tail:"; tail -25 logs/usbprov.build.log; exit 1
fi
[ -f tobyOS.iso ] || { echo "BUILD FAIL (no ISO)"; exit 1; }
grep -qa "port census" tobyos.bin && echo "  ok: [xhci] port census compiled in" \
    || { echo "  FAIL: census missing"; exit 1; }

echo "=== [2/4] fresh blank 2 GiB stick ==="
rm -f "$STICK"
"/c/Program Files/qemu/qemu-img.exe" create -f raw "$STICK" 2G >/dev/null || exit 1

run_boot () {   # $1 = log
    : > "$1"
    "$QEMU" -cdrom tobyOS.iso -boot d \
      -device qemu-xhci,id=xhci \
      -drive if=none,id=stick,format=raw,file="$STICK" \
      -device usb-storage,bus=xhci.0,drive=stick \
      -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
      -smp 4 -m "$RAM" -cpu qemu64,+smep,+smap -serial "file:$1" \
      -no-reboot -display none &
    for i in $(seq 1 240); do
        grep -aq 'Desktop ready' "$1" 2>/dev/null && break
        grep -aq 'KERNEL PANIC' "$1" 2>/dev/null && break
        sleep 1
    done
    sleep 12
    taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1
    sleep 2
}

echo "=== [3/4] boot 1: does the stick appear, and can it be provisioned? ==="
run_boot logs/usbprov1.log
echo "--- xHCI saw: ---"
tr '\r' '\n' < logs/usbprov1.log | grep -aE "port census|\[xhci\].*(slot|Address)" | head -4
echo "--- registered as a block device? ---"
tr '\r' '\n' < logs/usbprov1.log | grep -aE "usb_msc|\[blk\] registered" | head -5
echo "--- provisioned? ---"
tr '\r' '\n' < logs/usbprov1.log | grep -aE "\[PROVBOOT\]|\[prov\]" | head -4

echo "=== [4/4] boot 2: SAME stick -- does /data come back from it? ==="
run_boot logs/usbprov2.log
tr '\r' '\n' < logs/usbprov2.log | grep -aE "mounted ./data.|is RAM-backed|tobyOS-data" | head -4

echo
echo "=== VERDICT ==="
saw=$(tr '\r' '\n' < logs/usbprov1.log | grep -ac "port census: [1-9]")
prov=$(tr '\r' '\n' < logs/usbprov1.log | grep -ac "PROVBOOT] provision")
pers=$(tr '\r' '\n' < logs/usbprov2.log | grep -acE "mounted ./data. on usb|mounted ./data.*\.p1")
if [ "$saw" = "0" ]; then      echo "FAIL: xHCI never saw the stick"
elif [ "$prov" = "0" ]; then   echo "FAIL: stick seen but not provisioned (guard refused? not a blk dev?)"
elif [ "$pers" = "0" ]; then   echo "FAIL: provisioned but /data did NOT come back on boot 2"
else                           echo "PASS: USB stick enumerated, provisioned, and /data PERSISTED across reboot"
fi
echo "logs: logs/usbprov1.log logs/usbprov2.log"
