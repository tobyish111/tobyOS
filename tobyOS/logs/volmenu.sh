#!/bin/bash
# volmenu.sh -- does the file explorer's Volumes sidebar actually render?
#
#   bash logs/volmenu.sh
#
# The Volumes section is new machinery: a GUI program calling sys_blk_list
# and flattening Places+Volumes into one listbox. "It compiles" says nothing
# about whether real devices appear, so this launches the explorer under the
# TKAPP harness and screenshots it.
#
# Two traps already hit here, both recorded so they are not hit again:
#   - MSYS bash's /dev/tcp cannot read the QMP greeting ("Invalid argument"),
#     so the socket work lives in logs/qmpshot.py instead.
#   - a poll loop with no sleep burns its iterations instantly; the first
#     run screenshotted a machine still loading its 897 MB initrd.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
OUT="$PWD/logs/volmenu.png"
LOG=logs/volmenu.log
PORT=45999

echo "== building"
touch src/kernel.c
if ! make -j4 "CC=TMP='C:\t' TEMP='C:\t' clang" \
          "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
          EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTKAPP_BOOT -DTKAPP_FILES -DTKAPP_RCLICK_X=40 -DTKAPP_RCLICK_Y=311" \
          iso > logs/volmenu.build.log 2>&1; then
    echo "BUILD FAILED:"; grep -E ' error:' logs/volmenu.build.log | head -20; exit 1
fi

rm -f "$OUT" "$LOG"
echo "== running"
"$QEMU" -cdrom tobyOS.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -device qemu-xhci,id=xhci \
    -drive if=none,id=stick,format=raw,file="$PWD/logs/volstick.img" \
    -device usb-storage,bus=xhci.0,drive=stick \
    -boot d -smp 4 -m 5120 -serial file:"$LOG" \
    -qmp tcp:127.0.0.1:$PORT,server,nowait -no-reboot -display none &
QPID=$!

# Wait for the EVENT, not for a guess at how long it takes. A fixed sleep
# shot the frame at 12 s of guest time while the injected click does not
# fire until ~19 s, so the menu was simply not open yet.
WANT="synthetic RIGHT-click"
grep -q "RCLICK" logs/volmenu.build.log 2>/dev/null || true
echo "== waiting for: $WANT (up to 300 s)"
ready=0
for i in $(seq 1 150); do
    sleep 2
    kill -0 $QPID 2>/dev/null || break
    if grep -aqF "$WANT" "$LOG" 2>/dev/null; then ready=1; break; fi
done
[ "$ready" = "1" ] || echo "   (marker never appeared -- shooting anyway)"
sleep 6                                   # let the menu paint

python logs/qmpshot.py $PORT "$OUT" || echo "QMP screendump failed"
sleep 2
kill $QPID 2>/dev/null; taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

echo
grep -a "TKAPP\|gui_files" "$LOG" | sed 's/^\[[0-9 ]*ms\] //' | head -8
echo
if [ -s "$OUT" ]; then echo "screenshot: $OUT ($(stat -c %s "$OUT") bytes)"; else echo "NO SCREENSHOT"; exit 1; fi
