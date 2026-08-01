#!/bin/bash
# Slice 85: identify the CHECK that fires the INT3, using real tooling on the
# control instead of guessing from the guest.
#
# tobyOS reports the fault as rip=0x624a118 with chrome (a PIE) loaded at
# base 0x500000, so the file/vaddr is 0x624a118-0x500000 = 0x5d4a118, and the
# INT3 byte itself is at rip-1 (the trap pushes the FOLLOWING address).
# Disassemble the window around it and pull out (a) the instruction stream,
# (b) any RIP-relative string references, which is how chrome's CHECK sites
# name their file/condition.
set -u
CD=/mnt/c/CustomOS/tobyOS/programs/chromium/chrome-linux64
OUT=/mnt/c/CustomOS/tobyOS/logs/control
mkdir -p "$OUT"
command -v objdump >/dev/null || { export DEBIAN_FRONTEND=noninteractive
    apt-get install -y -qq binutils >/dev/null 2>&1; }

TRAP=0x31c3fc3
FROM=$((TRAP - 0x120))
TO=$((TRAP + 0x10))

echo "=== bytes at the trap (expect cc = int3) ==="
objdump -s -j .text --start-address=$TRAP --stop-address=$((TRAP+8)) \
        "$CD/chrome" 2>/dev/null | tail -2

echo
echo "=== disassembly leading into it ==="
objdump -d --start-address=$FROM --stop-address=$TO "$CD/chrome" 2>/dev/null \
    | tail -40 > "$OUT/disasm.txt"
cat "$OUT/disasm.txt"
