#!/bin/bash
# Slice 91: turn logs/freeze_regs.txt (QMP `info registers -a` capture from
# run_freeze.py) into named kernel lines. Kernel RIPs symbolize against
# tobyos.bin (full symbols); ring-3 RIPs are reported as-is (user code --
# symbolize those against chrome/its .so map instead).
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
cd /c/CustomOS/tobyOS || exit 1
F=${1:-logs/freeze_regs.txt}
[ -f "$F" ] || { echo "no $F"; exit 1; }
echo "=== RIPs per capture ==="
grep -oE "RIP=[0-9a-f]+" "$F" | nl
echo
echo "=== kernel symbolization (tobyos.bin) ==="
grep -oE "RIP=[0-9a-f]+" "$F" | cut -d= -f2 | while read -r rip; do
    case "$rip" in
    ffffffff8*)
        sym=$(addr2line -f -e tobyos.bin "0x$rip" 2>/dev/null | tr '\n' ' ')
        echo "0x$rip  $sym";;
    *)
        echo "0x$rip  (ring-3 / non-kernel)";;
    esac
done
