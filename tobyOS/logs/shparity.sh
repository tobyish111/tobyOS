#!/bin/bash
# shparity.sh -- THE BASH-PARITY GATE for the tobyOS shell.
#
# The tobyOS shell is meant to relate to bash the way TypeScript relates to
# JavaScript: a strict superset. Every bash script must run unchanged and
# produce the same bytes and the same exit status; extra features are welcome,
# divergence on shared syntax is a bug. That claim is only worth something if
# something checks it, so this gate checks it DIFFERENTIALLY against the real
# GNU bash 5.2 that tobyOS already ships -- not against a specification, and
# not against our own reading of one.
#
# Run it:      bash logs/shparity.sh
# Add a case:  drop a .sh file in initrd/etc/shparity/ and re-run. Cases must
#              be deterministic (no clock, pid, network or randomness) and
#              should stay inside shell builtins so the case measures the
#              SHELL and not whichever `ls` happens to be installed.
#
# Scratch lives on /data (tobyfs), so the run needs disk.img attached: the
# root filesystem is the read-only initrd ramfs and cannot create files at
# all. Without it the gate reports SKIP with that reason rather than passing.
cd /c/CustomOS/tobyOS || exit 1
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
LOG="logs/shparity.log"

# Toolchain, and the TMP workaround this box needs. A bare `export TMP=...`
# does NOT survive the make->sh hop (MSYS rewrites it); carrying a literal
# Windows path ON the compiler variable does, and fixes both the integrated
# assembler's temps and the gcc host tools in one go.
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t
MAKE_TMP=("CC=TMP='C:\\t' TEMP='C:\\t' clang" "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc")

echo "=== corpus ==="
ls -1 initrd/etc/shparity/*.sh 2>/dev/null | sed 's#.*/#  #'
CASES=$(ls -1 initrd/etc/shparity/*.sh 2>/dev/null | wc -l)
[ "$CASES" -gt 0 ] || { echo "NO CORPUS -- nothing to gate"; exit 1; }

echo "=== build gate ISO ($CASES cases) ==="
# kernel.c carries the SHPARITY_BOOT hook, so it must be recompiled whenever
# the flag changes -- make cannot see a -D change as a reason to rebuild.
touch src/kernel.c
make "${MAKE_TMP[@]}" iso \
    EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DSHPARITY_BOOT" \
    >logs/shparity.build.log 2>&1
[ -f tobyOS.iso ] || { echo "ISO BUILD FAIL"; tail -30 logs/shparity.build.log; exit 1; }

# GATE 0: the flags and the payload must actually be IN the artefacts. A build
# that quietly skipped the recompile, or an initrd missing the corpus, would
# otherwise produce a clean-looking SKIP that reads like "nothing to fix".
echo "=== gate 0: payload really shipped ==="
tar -tf build/initrd.tar 2>/dev/null | grep -qx 'bin/tsh' \
    && echo "  bin/tsh       PRESENT" || { echo "  bin/tsh       MISSING -- abort"; exit 1; }
tar -tf build/initrd.tar 2>/dev/null | grep -qx 'bin/shparity' \
    && echo "  bin/shparity  PRESENT" || { echo "  bin/shparity  MISSING -- abort"; exit 1; }
INTAR=$(tar -tf build/initrd.tar 2>/dev/null | grep -c '^etc/shparity/.*\.sh$')
echo "  corpus in tar: $INTAR / $CASES"
[ "$INTAR" -eq "$CASES" ] || { echo "  corpus SHORT -- abort"; exit 1; }
grep -qa 'SHPARITY' tobyos.bin \
    && echo "  SHPARITY_BOOT compiled into kernel" \
    || { echo "  SHPARITY_BOOT NOT in kernel -- abort"; exit 1; }

echo "=== boot ==="
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1; : > "$LOG"
"$QEMU" -cdrom tobyOS.iso \
  -drive file=disk.img,format=raw,if=ide,index=0,media=disk,cache=writethrough -boot d \
  -smp 4 -m 4096 -cpu qemu64,+smep,+smap -serial "file:$LOG" -no-reboot -display none &
# -m 4096, not the 512 the other harnesses use: with Chromium staged the
# initrd is ~825 MiB, and Limine loads it whole before the kernel starts. At
# 512 MiB the run dies in the BOOTLOADER ("High memory allocator: Out of
# memory") -- before a single line of gate output, which reads like the gate
# is broken rather than like the box is too small.
for i in $(seq 1 3600); do
    grep -aq 'SHPARITY\] VERDICT' "$LOG" 2>/dev/null && break
    sleep 1
done
sleep 2
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

echo "=== per-case results ==="
# NOT anchored with ^: the gate's lines share the serial wire with kernel
# logging, so a line can arrive with something already on it. Anchoring here
# silently printed nothing while the run underneath was perfectly fine.
grep -a '\[shparity\]' "$LOG"

echo "=== verdict ==="
if grep -aq 'SHPARITY\] VERDICT' "$LOG"; then
    grep -a 'SHPARITY\] VERDICT' "$LOG"
else
    echo "NO VERDICT -- the gate never reported. Last 20 serial lines:"
    tail -20 "$LOG"
fi

echo "=== faults (empty=clean) ==="
grep -aiE 'KERNEL PANIC|#GP|#PF|EXCEPTION [0-9]|unhandled syscall' "$LOG" \
    | sort | uniq -c | head -20
echo "(shparity end)"
