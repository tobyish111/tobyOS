#!/bin/bash
# shparity_linux.sh -- the bash-parity gate, on Linux/QEMU.
#
# Same gate as logs/shparity.sh, which is written for MSYS2 on Windows (it
# hardcodes /c/CustomOS, the Windows QEMU path and taskkill). This runs the
# identical corpus and the identical oracle on a Linux box.
#
#   bash programs/realbash/build.sh     # fetch the GNU bash 5.2 oracle, once
#   bash logs/shparity_linux.sh
#
# -m 4096 is not optional: with Chromium staged the initrd is ~825 MiB and
# Limine loads it whole before the kernel starts, so a smaller box dies in the
# BOOTLOADER with no gate output at all.
set -u
cd /home/user/tobyOS/tobyOS
LOG=logs/shparity.log
CASES=$(ls -1 initrd/etc/shparity/*.sh 2>/dev/null | wc -l)
touch src/kernel.c
timeout 2400 make -j4 iso EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DSHPARITY_BOOT" \
    > logs/shparity.build.log 2>&1 || { echo "ISO BUILD FAIL"; tail -20 logs/shparity.build.log; exit 1; }
# GATE 0
tar -tf build/initrd.tar 2>/dev/null | grep -qx 'bin/tsh'      || { echo "bin/tsh MISSING"; exit 1; }
tar -tf build/initrd.tar 2>/dev/null | grep -qx 'bin/shparity' || { echo "bin/shparity MISSING"; exit 1; }
tar -tf build/initrd.tar 2>/dev/null | grep -qx 'bin/bash'     || { echo "bin/bash (oracle) MISSING"; exit 1; }
INTAR=$(tar -tf build/initrd.tar 2>/dev/null | grep -c '^etc/shparity/.*\.sh$')
[ "$INTAR" -eq "$CASES" ] || { echo "corpus SHORT $INTAR/$CASES"; exit 1; }
grep -qa 'SHPARITY' tobyos.bin || { echo "SHPARITY_BOOT not in kernel"; exit 1; }
echo "gate 0 ok: tsh+shparity+bash present, corpus $INTAR/$CASES"

pkill -9 -f qemu-system-x86_64 2>/dev/null; sleep 1; : > "$LOG"
setsid timeout -k 2 1200 qemu-system-x86_64 -cdrom tobyOS.iso \
  -drive file=disk.img,format=raw,if=ide,index=0,media=disk,cache=writethrough -boot d \
  -smp 4 -m 4096 -cpu qemu64,+smep,+smap -serial "file:$LOG" -no-reboot -display none >/dev/null 2>&1 &
for i in $(seq 1 1200); do
    grep -aq 'SHPARITY\] VERDICT' "$LOG" 2>/dev/null && break
    sleep 1
done
sleep 2; pkill -9 -f qemu-system-x86_64 2>/dev/null; sleep 1
echo "=== verdict ==="
grep -a 'SHPARITY\] VERDICT' "$LOG" || { echo "NO VERDICT"; tail -15 "$LOG"; }
