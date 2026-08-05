#!/bin/bash
# Slice 119: does CLICKING the taskbar's Chromium pin actually launch it?
#
# The pin's geometry can be checked from a screendump and its dispatch by
# reading three lines of gui.c, but "a user clicks it and Chromium comes up"
# needs the click to really happen. Host-side injection does NOT work here:
# QEMU accepts relative input-send-event (returns success) yet nothing ever
# reaches the guest's IRQ12 and the cursor never moves -- measured twice with
# the QMP replies printed. So the click is synthesised INSIDE the guest, via
# gui_post_shell_click(), which drives the same on_mouse_event() handler the
# PS/2 callback does: real hit-testing, real pin dispatch.
#
# Coordinates mirror gui.c:
#   x0 = START_BTN_W(62) + 4 + TASKBAR_SEARCH_W(160) + 8 = 234
#   pin i centre = x0 + i*TASKBAR_PIN_W(44) + (44-6)/2
#   Chromium is pin 8  -> x = 605 ;  y = screen_h(800) - 44/2 = 778
set -u
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t
export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'
cd /c/CustomOS/tobyOS || exit 1
PY=/c/Users/tdude/AppData/Local/Programs/Python/Python311/python

CX=${CX:-605}
CY=${CY:-778}
HOLD=${HOLD:-240000}
echo "=== [1/3] build: desktop + synthetic click at ($CX,$CY) ==="
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1
sleep 1
rm -f build/initrd.tar build/base.iso tobyOS.iso
touch src/kernel.c src/gui.c
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" iso \
     EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTKAPP_BOOT -DTKAPP_HOLD_MS=$HOLD -DTKAPP_CLICK_X=$CX -DTKAPP_CLICK_Y=$CY" \
     > logs/pintest.build.log 2>&1; then
    echo "BUILD FAILED -- tail:"; tail -20 logs/pintest.build.log; exit 1
fi
[ -f tobyOS.iso ] || { echo "BUILD FAILED (no ISO)"; exit 1; }
ls -l --time-style=full-iso tobyOS.iso

echo "=== [2/3] boot; the guest clicks its own taskbar ==="
$PY logs/pinclick.py

echo "=== [3/3] verdict ==="
L=logs/pintest.log
echo "--- the click ---"
grep -a "synthetic shell click\|hit=taskbar-pin" "$L" | head -4
echo "--- did chromewin launch? ---"
grep -aE "launched /bin/chromewin|created pid=[0-9]+ .*chromewin|chromewin.*chrome pid=|sessionId=" "$L" | head -8
echo "--- chrome bootstrap ---"
grep -acE "\[execve-argv\]|chrome pid=" "$L" | sed 's/^/chrome exec events: /'
echo "--- faults (empty=clean) ---"
grep -aiE "KERNEL PANIC|EXCEPTION [0-9]+|terminating user process" "$L" | head -5
