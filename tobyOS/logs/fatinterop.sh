#!/bin/bash
# fatinterop.sh -- can a machine that is NOT tobyOS read what tobyOS wrote?
#
#   bash logs/fatinterop.sh
#
# logs/fsmatrix.sh already runs the CRUD matrix against a FAT volume, but
# it does it with src/fat32.c on both ends: the driver mounts what the
# driver formatted. That proves the two halves agree with each other and
# nothing else, and the whole point of formatting a USB stick as FAT32 is
# that OTHER systems can read it.
#
# So this boots tobyOS with a BLANK scratch disk attached, has the kernel
# format it and write real content, and then hands the resulting image to
# two readers that share no code with tobyOS:
#
#   - file(1)/libmagic, which has its own idea of what a FAT32 BPB is;
#   - logs/fatread.py, written from Microsoft's FAT specification, which
#     checks the structural invariants and compares the file bytes.
#
# The scratch image is created blank every run and the kernel refuses any
# disk that is not already blank, so this cannot touch real storage.
#
# Exit status is the result: 0 = green.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs

QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
IMG=fatscratch.img
LOG=logs/fatinterop.log
FLAGS="-DFAST_BOOT -DQUICK_BOOT -DFATDISK_SELFTEST"

echo "== building ($FLAGS)"
touch src/kernel.c
if ! make -j4 "CC=TMP='C:\t' TEMP='C:\t' clang" \
          "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
          EXTRA_CFLAGS="$FLAGS" iso > logs/fatinterop.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/fatinterop.build.log | head -20
    exit 1
fi

# A genuinely blank 40 MiB disk. Recreated every run: the kernel-side
# guard only formats a disk whose boot sector, GPT header and first data
# sector are all zero, so a leftover image would (correctly) be refused.
echo "== blank scratch disk"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=40 2>/dev/null || exit 1

echo "== running (timeout 200s)"
rm -f "$LOG"
timeout 200 "$QEMU" -cdrom tobyOS.iso \
    -drive file="$IMG",format=raw,if=ide,index=0,media=disk \
    -boot d -smp 4 -m 5120 -serial file:"$LOG" \
    -no-reboot -display none > /dev/null 2>&1
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

echo
grep -a "FATDISK" "$LOG" | sed 's/^\[[0-9 ]*ms\] //'
RC=0
grep -aq "FATDISK. VERDICT: WROTE" "$LOG" || {
    echo "the kernel did not finish writing the volume"; RC=1; }

echo
echo "== reader 1: file(1)/libmagic (not our code)"
DESC=$(file "$IMG")
echo "   $DESC"
case "$DESC" in
    *"FAT (32 bit)"*) echo "   ok   libmagic reads it as FAT32" ;;
    *) echo "   FAIL libmagic does not see a FAT32 volume"; RC=1 ;;
esac

echo
echo "== reader 2: logs/fatread.py (written from the FAT spec)"
python logs/fatread.py "$IMG" || RC=1

echo
if [ "$RC" = "0" ]; then echo "VERDICT: PASS"; else echo "VERDICT: FAIL"; fi
exit $RC
