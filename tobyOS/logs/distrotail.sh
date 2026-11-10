#!/usr/bin/env bash
# distrotail.sh -- the last names a distro has that this tree did not:
# strip, dig, host, mkfs, domainname and cc.
#
#   bash logs/distrotail.sh
#
# What carries the weight here:
#
#   * strip is checked by RUNNING the binary it shrank. A stripped file
#     that no longer loads is the only interesting way for this tool to be
#     wrong, and its size says nothing about that. (The byte-level
#     comparison against GNU strip -- program headers unmoved, every
#     allocated section identical -- runs on the host, where readelf is.)
#
#   * dig and host ask the REAL resolver. DHCP writes /etc/resolv.conf at
#     ~6.7 s of boot and these run well after, so a "SERVER: 10.0.2.3"
#     line proves the tool read the file rather than falling back to its
#     compiled-in default -- which is the failure that would otherwise
#     look exactly like success.
#
#   * mkfs is a dispatcher, so what matters is that an unknown type is
#     reported as an unknown type, not as a missing file.

set -u
cd "$(dirname "$0")/.."

LOG=logs/distrotail.log
rm -f "$LOG" || { echo "ABORT: log busy"; exit 1; }

# FORCE A KERNEL REBUILD. EXTRA_CFLAGS is not a file, so make cannot see
# that it changed: src/kernel.o built by the PREVIOUS gate -- with that
# gate's -D flags -- is newer than kernel.c and gets reused. Running
# logs/fsprobe.sh (which builds -DFSPROBE_BOOT, no -DCOREUTILS_SELFTEST)
# and then this gate booted a kernel with NO selftest in it at all: the
# harness never ran, and the only symptom was a boot that looked healthy
# and said nothing. Five other gates already did this; these did not.
rm -f src/kernel.o
PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH" \
  make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
       EXTRA_CFLAGS=-DCOREUTILS_SELFTEST iso >logs/.buildlog.txt 2>&1
BUILD_RC=$?
# A FAILED BUILD USED TO LOOK LIKE A PASSING GATE -- see logs/coreutils.sh.
if [ $BUILD_RC -ne 0 ]; then
    echo "ABORT: the build FAILED -- the ISO on disk is stale, so any"
    echo "       verdict below would have described code that is not in it."
    tail -25 logs/.buildlog.txt
    exit 1
fi
[ -f tobyOS.iso ] || { echo "ABORT: build produced no ISO"; exit 1; }
# A fresh 32 MiB AHCI scratch disk every run: the mkfs/fsck/tune2fs/blkid
# fixtures format it, so it must start blank -- and it must NOT be disk.img
# (/data). The kernel's BLK_WRITE refuses a mounted device; the gate proves
# that too.
rm -f logs/scratch.img; truncate -s 32M logs/scratch.img

"/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
  -device ahci,id=ahci0 -drive if=none,id=scratch,file=logs/scratch.img,format=raw \
  -device ide-hd,drive=scratch,bus=ahci0.0 \
  -boot d -smp 4 -m 5120 -cpu max \
  -serial file:"$LOG" -no-reboot -display none 2>/dev/null &

# 800 s, not 440: the shared boot now runs eleven test scripts and the
# distro one alone spends ~40 s in ping, tcpdump, an FTP session and a
# debugger. The machine was alive and working at 438 s when this gate
# gave up on it.
for i in $(seq 1 400); do
    grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null && break
    sleep 2
done
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
if ! grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: the harness never reported"
    exit 1
fi

# A kernel diagnostic can interleave MID-LINE, and the halves may be several
# diagnostics apart, so the prefix is held until the next surviving line
# says whether it was a whole line or the start of one. The trailing sed
# then puts each @@marker back on its own line -- a command that printed no
# final newline leaves the next `echo @@...` glued to its output.
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
OUT=$(sed -n '/@@dt_BEGIN/,/@@dt_END/p' "$LOG" | kern_strip | tr -d '\r')
if ! echo "$OUT" | grep -q "@@dt_END"; then
    echo "ABORT: the guest transcript is incomplete"
    echo "$OUT" | tail -5
    exit 1
fi

# The checks live in logs/distrochecks.sh so logs/hwgate.sh can apply the
# SAME ones to a transcript from the real EliteDesk.
. logs/distrochecks.sh
