#!/bin/bash
# TobyEdit's Save button, end to end: click it, and the file must change AND
# the window must close.
#
#   bash logs/editsave.sh
#
# logs/edittrunc.sh covers load_file() on the HOST and says outright that
# gui_edit "cannot be driven headlessly". That is true of the parse, but not
# of the button: gui_post_mouse()/gui_post_key() feed the same event ring the
# real mouse and keyboard do, so a click can be delivered to the running
# editor and the result read back off the filesystem.
#
# The second case is the one worth having. Closing the window on a save that
# did NOT happen is how an editor eats a buffer -- the window disappears and
# the edits go with it. So the harness also launches an UNTITLED editor,
# where save_file() must refuse with "No filename", and asserts the window
# is STILL THERE afterwards.
#
# Runs its own build with -DEDITSAVE_SELFTEST. EXTRA_CFLAGS is not a tracked
# dependency, so src/kernel.o is removed first -- otherwise a stale object
# from another build is silently reused and the harness never runs at all.
cd /c/CustomOS/tobyOS || exit 1
LOG=logs/editsave.log

MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
sleep 2
rm -f "$LOG" || { echo "ABORT: log busy"; exit 1; }

# NEVER `make clean` here: programs/*/*.elf are gitignored and 55 of them have
# no in-tree build rule, so a clean is unrecoverable. Removing the one object
# whose flags changed is both sufficient and safe.
rm -f src/kernel.o
PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH" \
  make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
       EXTRA_CFLAGS=-DEDITSAVE_SELFTEST iso >/dev/null 2>&1
if [ ! -f tobyOS.iso ]; then echo "ABORT: build produced no ISO"; exit 1; fi

"/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
  -boot d -smp 4 -m 5120 -cpu max \
  -serial file:"$LOG" -no-reboot -display none 2>/dev/null &

for i in $(seq 1 90); do grep -aq "EDITSAVE. VERDICT" "$LOG" 2>/dev/null && break; sleep 2; done
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1

if ! grep -aq "EDITSAVE. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: harness never reported (did the build pick up the flag?)"
    grep -a "EDITSAVE" "$LOG" 2>/dev/null | head
    exit 1
fi

grep -a "EDITSAVE" "$LOG" | sed 's/^\[[0-9 ]*ms\] //'
echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'PANIC' "$LOG")"
grep -aq "EDITSAVE. VERDICT: PASS" "$LOG" || exit 1
