#!/bin/bash
# The ext2/3/4 inspection tools: dumpe2fs, e2fsck, debugfs, badblocks.
#
#   bash logs/e2fstools.sh
#
# These needed a new syscall. Userland could not see a disk's bytes at all --
# no raw read, no /dev/sda -- so none of these tools could exist.
# ABI_SYS_BLK_READ is root-only and READ-ONLY: a general raw write from
# userland would bypass the foreign-filesystem and boot-medium guards that
# provisioning carries, and those exist because swap once targeted a Windows
# EFI partition on this system.
#
# THE FIXTURES ARE HAND-BUILT. There is no mkfs available here (that would
# need the write access just ruled out), so the gate writes two 1 MiB images
# with superblocks whose every field is known, and attaches them as drives.
# That is stronger than testing against a real filesystem of unknown
# contents: every number the tools print can be checked against the number
# that was written, and one image carries a DELIBERATE inconsistency
# (free blocks > total blocks, state "has errors") so the checker has
# something real to catch. A checker that only ever says "clean" is
# indistinguishable from one that does not read the disk.
#
# The write modes are asserted to REFUSE rather than silently downgrade:
# e2fsck -y, debugfs -w and badblocks -w each report why. A caller who asks
# for a repair and gets a check, or asks for a write test and gets a read
# scan, would believe work happened that did not.
cd /c/CustomOS/tobyOS || exit 1
LOG=logs/e2fstools.log

MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
sleep 2
rm -f "$LOG" || { echo "ABORT: log busy"; exit 1; }

# Rebuild the fixtures every run so the expected values below cannot drift
# away from the bytes on the disk.
python - <<'PYEOF'
import struct
def sb(inodes, blocks, free_b, free_i, label, bad=False):
    b = bytearray(1024*1024); o = 1024
    def u32(off, v): struct.pack_into('<I', b, o+off, v)
    def u16(off, v): struct.pack_into('<H', b, o+off, v)
    u32(0x00, inodes); u32(0x04, blocks); u32(0x08, blocks//20)
    u32(0x0C, free_b); u32(0x10, free_i); u32(0x14, 1)
    u32(0x18, 0); u32(0x1C, 0); u32(0x20, 8192); u32(0x24, 8192)
    u32(0x28, 512); u16(0x34, 3); u16(0x36, 20)
    u16(0x38, 0xEF53); u16(0x3A, 1 if not bad else 2)
    u32(0x48, 0); u32(0x4C, 1); u32(0x54, 11); u16(0x58, 256)
    struct.pack_into('<16s', b, o+0x68, bytes(range(16)))
    struct.pack_into('<16s', b, o+0x78, label.encode())
    return b
open('logs/.ext4good.img','wb').write(sb(4096,16384,12000,4000,'TOBYGOOD'))
open('logs/.ext4bad.img','wb').write(sb(4096,16384,99999,4000,'TOBYBAD',bad=True))
PYEOF

rm -f src/kernel.o
PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH" \
  make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
       EXTRA_CFLAGS=-DCOREUTILS_SELFTEST iso >logs/.buildlog.txt 2>&1
BUILD_RC=$?
# A FAILED BUILD USED TO LOOK LIKE A PASSING GATE. make's output went to
# /dev/null and the only check was "does an ISO exist" -- which a PREVIOUS
# run's ISO satisfies. One compile error therefore aborted make, left the
# stale image in place, and this gate booted it and reported confident
# results about source that was never in the build. Seen for real: a broken
# char literal in head.c silently froze three gates on an hour-old ISO.
if [ $BUILD_RC -ne 0 ]; then
    echo "ABORT: the build FAILED -- the ISO on disk is stale, so any"
    echo "       verdict below would have described code that is not in it."
    tail -25 logs/.buildlog.txt
    exit 1
fi
[ -f tobyOS.iso ] || { echo "ABORT: build produced no ISO"; exit 1; }

# THE FIXTURES ATTACH OVER USB, NOT IDE. Adding ANY extra IDE drive makes
# this image fail to boot: Limine stops at "Loading module
# boot():/boot/initrd.tar" and never returns, with QEMU reporting nothing on
# stderr. Verified by control -- the same ISO boots fine with no extra drive,
# and fails with one. USB leaves the IDE bus untouched and the guest sees the
# fixtures as usb0/usb1.
"/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
  -device qemu-xhci,id=xhci \
  -drive if=none,id=fx1,format=raw,file=logs/.ext4good.img \
  -device usb-storage,bus=xhci.0,drive=fx1 \
  -drive if=none,id=fx2,format=raw,file=logs/.ext4bad.img \
  -device usb-storage,bus=xhci.0,drive=fx2 \
  -boot d -smp 4 -m 5120 -cpu max \
  -serial file:"$LOG" -no-reboot -display none 2>logs/.qemu_err.txt &

# The shared boot harness now runs six scripts (coreutils, extra,
# accounts, ssh, util, e2fs) and the ssh one alone takes ~40s of guest
# time. Waiting is cheap; a false ABORT costs a whole rebuild.
for i in $(seq 1 200); do grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null && break; sleep 2; done
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
if ! grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: the harness never reported"
    # -cdrom IS ide index 2. A -drive on the same index makes QEMU refuse
    # to start, and the serial log is then never created at all -- which
    # reads as a boot failure. Show what QEMU actually said.
    [ -s logs/.qemu_err.txt ] && { echo "qemu said:"; head -3 logs/.qemu_err.txt; }
    exit 1
fi

# The e2fs markers are prefixed e2_ because util_test.sh runs in the SAME
# boot and also emits @@lsblk. Sharing a marker made this gate parse the
# other test's output and report failures that were not about ext4 at all.
# A kernel diagnostic can interleave MID-LINE, not only at the start of
# one, so a filter that drops lines beginning with '[' leaves it behind and
# a perfectly correct tool looks broken. Cut an embedded diagnostic off
# where it starts, and swallow the blank line its own newline leaves.
kern_strip() {
    sed -E 's/^\[[0-9 ]+ms\] //' | grep -v '^\[' | awk '
        { if (match($0, /\[[0-9]+ ms\] \[[a-z_0-9]+\]/)) {
              frag = frag substr($0, 1, RSTART - 1); pending = 1; next }
          if (pending) {
              pending = 0
              if ($0 == "") { print frag; frag = ""; next }
              print frag $0; frag = ""; next }
          print }
        END { if (frag != "") print frag }' |
    sed -E 's/(.)(@@[a-z_])/\1\n\2/g'
}
OUT=$(sed -n '/@@e2_lsblk/,/@@e2_END/p' "$LOG" | kern_strip | tr -d '\r')
if ! echo "$OUT" | grep -q "@@e2_END"; then
    echo "ABORT: transcript incomplete"; echo "$OUT" | tail -5; exit 1
fi

# The marker match is /^@@[a-z_]/, not /^@@/: a UNIFIED DIFF HUNK
# HEADER is literally "@@ -1,3 +1,3 @@", and a looser pattern ended the
# diff section on its own first line and reported the output as empty.
section() { echo "$OUT" | awk -v k="@@$1" '$0==k{f=1;next} /^@@[a-z_]/{f=0} f'; }
PASS=0; FAIL=0
want() {
    local name="$1" label="$2" pat="$3" g
    g=$(section "$name" | tr '\n' '|')
    if echo "$g" | grep -qE "$pat"; then echo "  ok   $label"; PASS=$((PASS+1))
    else echo "  FAIL $label -- got '$g' (wanted /$pat/)"; FAIL=$((FAIL+1)); fi
}

echo "=== the fixtures are visible ==="
want e2_lsblk "lsblk sees the extra drives" 'NAME.*TYPE'
want e2_find  "both crafted filesystems are found" 'good=.*bad='

echo "=== dumpe2fs reads the values that were written ==="
# 16384 blocks, 4096 inodes, 1024-byte blocks: exactly what the fixture has.
want e2_dumpe2fs "reports the written geometry" 'TOBYGOOD.*4096.*16384.*1024'

echo "=== e2fsck actually checks ==="
want e2_e2fsck_clean "clean filesystem passes"        'clean.*rc=0'
want e2_e2fsck_bad   "catches free>total and state"   'exceeds total blocks.*rc=4'

echo "=== the write modes refuse, and say why ==="
want e2_e2fsck_refuses_repair  "e2fsck -y refuses"    'REPAIR.*rc=8'
want e2_debugfs_refuses_write  "debugfs -w refuses"   'read-only.*rc=1'
want e2_badblocks_refuses_write "badblocks -w refuses" 'WRITE test.*rc=1'

echo "=== read paths ==="
want e2_debugfs   "debugfs stats reads the superblock" 'TOBYGOOD.*Block count'
want e2_badblocks "badblocks scans without errors"     'rc=0'
want e2_notext4   "a missing device is reported"       'cannot read.*rc=1'

echo
echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
