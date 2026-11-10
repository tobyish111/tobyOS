#!/bin/bash
# usbsysfs.sh -- the gate for /sys/bus/usb/devices + native lsusb.
#
#   bash logs/usbsysfs.sh
#
# Why this exists as its OWN script: every other harness in logs/ boots
# QEMU with no USB devices at all (lxposix.sh, shparity.sh, ...), so a
# USB tree can be completely broken and every existing gate stays green.
# This one attaches a topology on purpose:
#
#   qemu-xhci
#     +- port 1: usb-hub
#     |    +- port 1: usb-kbd      -> tests the "1-1.1" hub-depth naming
#     |    +- port 2: usb-mouse
#     +- port 3: usb-storage       -> tests the "1-3" root-port naming
#
# and asserts VALUES, not exit codes. The checks below name the exact
# strings QEMU's devices report (0627:0001 with iProduct "QEMU USB
# Keyboard" / "QEMU USB Mouse", 46f4:0001 "QEMU USB HARDDRIVE"), so a
# regression that leaves the files present but empty -- which is what a
# dropped string-descriptor fetch looks like -- fails here instead of
# passing as "lsusb ran, exit 0".
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
STICK=/c/t/usbsysfs-stick.img
LOG=logs/usbsysfs.log
FLAGS="-DFAST_BOOT -DQUICK_BOOT -DPKGPROBE_BOOT"

taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1

echo "== build ($FLAGS)"
touch src/kernel.c            # EXTRA_CFLAGS changes rebuild NOTHING on their own
if ! make -j4 "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
             "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
             EXTRA_CFLAGS="$FLAGS" iso > logs/usbsysfs.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/usbsysfs.build.log | head -20
    exit 1
fi
# Gate on the BINARY, not on make's exit status: a failed build leaves the
# previous iso in place and every check below would test stale bits.
python -c "
import sys
d = open('tobyos.bin','rb').read()
missing = [m for m in (b'/bus/usb/devices', b'which lsusb') if m not in d]
sys.exit(1 if missing else 0)" || { echo "FAIL: kernel lacks the USB tree / probe"; exit 1; }
tar -tf build/initrd.tar | grep -qx bin/lsusb || { echo "FAIL: /bin/lsusb not in initrd"; exit 1; }

[ -f "$STICK" ] || "/c/Program Files/qemu/qemu-img.exe" create -f raw "$STICK" 64M >/dev/null

echo "== running"
rm -f "$LOG"
timeout 300 "$QEMU" -cdrom tobyOS.iso -boot d -smp 4 -m 5120 \
    -device qemu-xhci,id=usb0 \
    -device usb-hub,bus=usb0.0,port=1,id=hub1 \
    -device usb-kbd,bus=usb0.0,port=1.1 \
    -device usb-mouse,bus=usb0.0,port=1.2 \
    -drive if=none,id=stick,format=raw,file="$STICK" \
    -device usb-storage,bus=usb0.0,port=3,drive=stick \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial file:"$LOG" -no-reboot -display none > /dev/null 2>&1
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

echo
echo "== what the guest printed"
tr -d '\r' < "$LOG" | sed -n '/PKGPROBE. --- which lsusb/,/PKGPROBE. ==== done/p' \
    | sed 's/^\[[0-9 ]*ms\] //'

echo
echo "== checks (values, not exit codes)"
RC=0
check () {  # $1 = description, $2 = extended regex that must appear
    if tr -d '\r' < "$LOG" | grep -qaE "$2"; then
        echo "  ok   $1"
    else
        echo "  FAIL $1   (no match for: $2)"
        RC=1
    fi
}
check "kernel exposed >=3 usb devices" '\[sysfs\] /sys/bus/usb/devices: [3-9][0-9]* device\(s\) exposed'
# Do NOT hardcode the root-port numbers: qemu-xhci puts its four USB3
# ports at 1-4 and its four USB2 ports at 5-8, so `port=1` on the command
# line lands on xHCI root port 5 and the hub's children come out as
# "1-5.1"/"1-5.2". The claim worth gating is the SHAPE -- a root-port
# device and a hub-depth device both named the way Linux names them.
check "a root-port device dir"         'devices/1-[0-9]+/'
check "a hub-depth device dir"         '1-[0-9]+\.[0-9]+'
check "lsusb names the keyboard"       'Bus 001 Device [0-9]+: ID 0627:0001 QEMU QEMU USB Keyboard'
check "lsusb names the mouse"          'Bus 001 Device [0-9]+: ID 0627:0001 QEMU QEMU USB Mouse'
check "lsusb names the mass storage"   'Bus 001 Device [0-9]+: ID 46f4:0001 QEMU QEMU USB HARDDRIVE'
check "lsusb exited 0"                 'lsusb-rc=0'
check "uevent is Linux-shaped"         'DEVTYPE=usb_device'
check "speed reported (480 or 5000)"   'speed=(480|5000|12|1\.5)'
check "bcdUSB rendered"                'version= ?[0-9]\.[0-9][0-9]'
check "tree shows a hub child"         '\|__ Port [0-9]+: Dev [0-9]+, Class=(Hub|Human Interface Device)'
if tr -d '\r' < "$LOG" | grep -qaE 'KERNEL PANIC|PAGE FAULT|GENERAL PROTECTION'; then
    echo "  FAIL kernel faulted"; RC=1
fi

echo
[ "$RC" = 0 ] && echo "RESULT: GREEN" || echo "RESULT: RED"
exit $RC
