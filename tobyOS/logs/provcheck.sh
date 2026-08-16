#!/bin/bash
# provcheck.sh -- re-validate persistent-storage PROVISIONING against the
# CURRENT tree, plus prove a provisioned volume actually survives a reboot.
#
# WHY NOW: the provisioning story was proven 2026-07-03 and its matrix has not
# been re-run since. Two things have changed under it:
#   * today's tobyfs inode-density change edits tfs_compute_geom, and
#     PROVISIONING CALLS tobyfs_format -- so provisioning runs straight through
#     the code I just touched.
#   * everything in the Linux/networking arc since July.
# "It passed six weeks ago" is not evidence about today's binary.
#
# TWO PHASES, because the guard battery and the real thing are different claims:
#   [PROV]     15-assertion guard battery on RAM fake disks (FOREIGN refused
#              even with FORCE, MOUNTED refused, BLANK allowed, e2e provision
#              + file round-trip).
#   [PROVBOOT] provision a REAL blank disk at boot, then REBOOT the same image
#              against the same disk and confirm /data comes back from the GPT
#              partition -- the claim that actually matters for "chrome keeps
#              its cookies".
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t; export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
RAM="${RAM:-5120}"
DISK=/c/t/provdisk.img

echo "=== [1/4] build (PROVISION_SELFTEST + PROVISION_BLANK_BOOT) ==="
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1
rm -f src/*.o build/initrd.tar build/base.iso tobyOS.iso
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" iso \
     EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DPROVISION_SELFTEST -DPROVISION_BLANK_BOOT" \
     >logs/provcheck.build.log 2>&1; then
    echo "BUILD FAIL -- tail:"; tail -25 logs/provcheck.build.log; exit 1
fi
[ -f tobyOS.iso ] || { echo "BUILD FAIL (no ISO)"; exit 1; }
for m in "PROV\]" "PROVBOOT"; do
    grep -qa -- "$m" tobyos.bin && echo "  ok: [$m] compiled in" \
        || { echo "  FAIL: [$m] missing -- a null result would be meaningless"; exit 1; }
done

# A genuinely BLANK disk: all zeroes, no MBR, no GPT. The guard classifies this
# BLANK (allowed without FORCE); anything with a partition table would be
# TOBYOS-or-FOREIGN and take a different path.
echo "=== [2/4] fresh blank 2 GiB disk ==="
rm -f "$DISK"
"/c/Program Files/qemu/qemu-img.exe" create -f raw "$DISK" 2G >/dev/null || exit 1
ls -la "$DISK"

run_boot () {   # $1 = log
    : > "$1"
    "$QEMU" -cdrom tobyOS.iso -boot d \
      -drive file="$DISK",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
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

echo "=== [3/4] boot 1: guard battery + provision the blank disk ==="
run_boot logs/provcheck1.log
echo "--- guard battery ---"
grep -aE '\[PROV\] (PASS|FAIL)' logs/provcheck1.log | tail -20
np=$(grep -ac '\[PROV\] PASS' logs/provcheck1.log)
nf=$(grep -ac '\[PROV\] FAIL' logs/provcheck1.log)
echo "  PASS=$np FAIL=$nf"
echo "--- provisioning a real disk ---"
grep -aE '\[PROVBOOT\]|tobyfs\] format|mounted ./data.' logs/provcheck1.log | head -6

echo "=== [4/4] boot 2: SAME disk, does /data come back? ==="
run_boot logs/provcheck2.log
grep -aE "mounted ./data.|is RAM-backed|tobyOS-data|\[part\] .*p1" logs/provcheck2.log | head -6

echo
echo "=== VERDICT ==="
persist=0
grep -aq "mounted '/data' on virtio0:p0.p1\|mounted '/data' on .*\.p1" logs/provcheck2.log && persist=1
if [ "$nf" != "0" ]; then
    echo "FAIL: $nf guard assertion(s) failed -- see logs/provcheck1.log"
elif [ "$np" -lt 10 ]; then
    echo "FAIL: only $np guard assertions ran (expected ~15) -- harness did not complete"
elif [ "$persist" != "1" ]; then
    echo "FAIL: boot 2 did NOT mount /data from the provisioned partition"
    grep -aE "RAM-backed" logs/provcheck2.log | head -2
else
    echo "PASS: guard $np/$np, disk provisioned, and /data PERSISTED across reboot"
fi
echo "logs: logs/provcheck1.log logs/provcheck2.log"
