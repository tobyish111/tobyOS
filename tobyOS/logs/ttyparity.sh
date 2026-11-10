#!/bin/bash
# ttyparity.sh -- THE INTERACTIVE BASH-PARITY GATE for the tobyOS shell.
#
# shparity.sh measures the script surface; this one runs both shells on a
# real pseudoterminal (via /bin/ttyparity in the guest) and compares the
# terminal byte streams: prompts, PS2 continuation, ignoreeof, interactive
# option defaults. The runner validates ITSELF first -- bash vs bash must
# produce identical transcripts -- and reports INSTRUMENT-BROKEN otherwise.
#
# Run it:      bash logs/ttyparity.sh
# Add a case:  drop a .txt file in initrd/etc/ttyparity/ and re-run. One
#              interactive SHAPE per file; every case must end in a line
#              that exits the shell (or a final %EOF%).
cd /c/CustomOS/tobyOS || exit 1
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
LOG="logs/ttyparity.log"

export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t
MAKE_TMP=("CC=TMP='C:\\t' TEMP='C:\\t' clang" "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc")

echo "=== corpus ==="
ls -1 initrd/etc/ttyparity/*.txt 2>/dev/null | sed 's#.*/#  #'
CASES=$(ls -1 initrd/etc/ttyparity/*.txt 2>/dev/null | wc -l)
[ "$CASES" -gt 0 ] || { echo "NO CORPUS -- nothing to gate"; exit 1; }

echo "=== build gate ISO ($CASES cases) ==="
touch src/kernel.c
make "${MAKE_TMP[@]}" iso \
    EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTTYPARITY_BOOT" \
    >logs/ttyparity.build.log 2>&1
RC=$?
[ "$RC" = "0" ] || { echo "BUILD FAIL rc=$RC"; tail -30 logs/ttyparity.build.log; exit 1; }

echo "=== gate 0: payload really shipped ==="
tar -tf build/initrd.tar 2>/dev/null | grep -qx 'bin/ttyparity' \
    && echo "  bin/ttyparity PRESENT" || { echo "  bin/ttyparity MISSING -- abort"; exit 1; }
INTAR=$(tar -tf build/initrd.tar 2>/dev/null | grep -c '^etc/ttyparity/.*\.txt$')
echo "  corpus in tar: $INTAR / $CASES"
[ "$INTAR" -eq "$CASES" ] || { echo "  corpus SHORT -- abort"; exit 1; }
grep -qa 'TTYPARITY' tobyos.bin \
    && echo "  TTYPARITY_BOOT compiled into kernel" \
    || { echo "  TTYPARITY_BOOT NOT in kernel -- abort"; exit 1; }

echo "=== boot ==="
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1; : > "$LOG"
"$QEMU" -cdrom tobyOS.iso \
  -drive file=disk.img,format=raw,if=ide,index=0,media=disk,cache=writethrough -boot d \
  -smp 4 -m 4096 -cpu qemu64,+smep,+smap -serial "file:$LOG" -no-reboot -display none &
for i in $(seq 1 3600); do
    grep -aq 'TTYPARITY\] VERDICT' "$LOG" 2>/dev/null && break
    sleep 1
done
sleep 2
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

echo "=== per-case results ==="
grep -a '\[ttyparity\]' "$LOG"

echo "=== verdict ==="
if grep -aq 'TTYPARITY\] VERDICT' "$LOG"; then
    grep -a 'TTYPARITY\] VERDICT' "$LOG"
else
    echo "NO VERDICT -- the gate never reported. Last 20 serial lines:"
    tail -20 "$LOG"
fi

echo "=== faults (empty=clean) ==="
grep -aiE 'KERNEL PANIC|#GP|#PF|EXCEPTION [0-9]|unhandled syscall' "$LOG" \
    | sort | uniq -c | head -20
echo "(ttyparity end)"
