#!/bin/bash
# The file-type registry, both halves.
#
#   bash logs/filetypes.sh
#
# HOST half (logs/filetypes_test.c): compiles <tobyos/file_types.h> and checks
# the table's own invariants -- duplicate extensions, rows whose action claims
# a handler that is not there, binaries marked editable, images routed at
# something that cannot decode them. None of that is a compile error, and all
# of it is invisible until someone double-clicks that one file.
#
# BOOT half: the same table, linked into the kernel, answering at runtime --
# plus a real 4x4 BMP written to /data and opened in gui_viewer, so the image
# association is proven end to end rather than assumed.
#
# Why both: the point of this work was that the tree had THREE file-type
# tables and two of them were dead -- src/file_assoc.c and
# libtoby/src/fileassoc.c each had a private list and no callers at all. A
# host test alone would pass just as happily on a table nothing links.
# TYPECOUNT is printed by both halves and compared: if they ever differ, the
# kernel and userland have drifted apart again, which is exactly how the
# original three copies came to disagree.
cd /c/CustomOS/tobyOS || exit 1
LOG=logs/filetypes.log
CC_HOST=gcc

echo "=== host: the table itself ==="
PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH" \
  $CC_HOST -std=c11 -Wall -Wextra -I include \
     -o logs/filetypes_test.exe logs/filetypes_test.c || {
    echo "ABORT: host test did not compile"; exit 1; }
./logs/filetypes_test.exe
HOST_RC=$?
HOST_N=$(./logs/filetypes_test.exe | sed -n 's/^TYPECOUNT=//p')

echo
echo "=== boot: the same table, in the running system ==="
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
sleep 2
rm -f "$LOG" || { echo "ABORT: log busy"; exit 1; }

# EXTRA_CFLAGS is not a tracked dependency, so the object whose flags changed
# must go. NEVER `make clean`: programs/*/*.elf are gitignored and most have
# no in-tree build rule, so a clean is unrecoverable.
rm -f src/kernel.o
PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH" \
  make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
       EXTRA_CFLAGS=-DFILETYPES_SELFTEST iso >/dev/null 2>&1
[ -f tobyOS.iso ] || { echo "ABORT: build produced no ISO"; exit 1; }

"/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
  -boot d -smp 4 -m 5120 -cpu max \
  -serial file:"$LOG" -no-reboot -display none 2>/dev/null &

for i in $(seq 1 90); do grep -aq "FILETYPES. VERDICT" "$LOG" 2>/dev/null && break; sleep 2; done
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1

if ! grep -aq "FILETYPES. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: boot harness never reported"
    grep -a "FILETYPES\|file_assoc" "$LOG" 2>/dev/null | head
    exit 1
fi

grep -a "FILETYPES\|\[file_assoc\]" "$LOG" | sed 's/^\[[0-9 ]*ms\] //'

# The viewer logs its decode BEFORE opening a window, so this line is the
# only proof the image path ran at all -- a window on screen proves nothing
# a gate can read.
if grep -aq "decoded image 4x4" "$LOG"; then
    echo "  ok   gui_viewer decoded the test bitmap"
    IMG_OK=1
else
    echo "  FAIL gui_viewer did not report a decode"
    grep -a "viewer" "$LOG" | head -3
    IMG_OK=0
fi

BOOT_N=$(grep -a "FILETYPES. TYPECOUNT=" "$LOG" | sed -n 's/.*TYPECOUNT=\([0-9]*\).*/\1/p' | head -1)
if [ -n "$HOST_N" ] && [ "$HOST_N" = "$BOOT_N" ]; then
    echo "  ok   host and kernel see the same $HOST_N types"
    SAME=1
else
    echo "  FAIL host=$HOST_N kernel=$BOOT_N -- the two worlds have drifted"
    SAME=0
fi

echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'PANIC' "$LOG")"
grep -aq "FILETYPES. VERDICT: PASS" "$LOG" || exit 1
[ "$HOST_RC" = "0" ] && [ "$IMG_OK" = "1" ] && [ "$SAME" = "1" ] || exit 1
echo "VERDICT: PASS"
