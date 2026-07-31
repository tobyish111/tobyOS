#!/bin/bash
# Slice 86: name the function containing the __stack_chk_fail trap.
# chrome is a PIE loaded at 0x500000 in the guest; rip 0x624a118 -> file
# 0x5d4a118. Find the greatest defined symbol <= that address.
set -u
CD=/mnt/c/CustomOS/tobyOS/programs/chromium/chrome-linux64
T=$((0x5d4a117))
echo "target file addr: $(printf 0x%x $T)"
for TBL in "-D" ""; do
  echo "=== nm $TBL ==="
  nm $TBL --defined-only "$CD/chrome" 2>/dev/null \
    | awk -v t=$T 'NF>=3 { a=strtonum("0x" $1); if (a<=t && a>best) {best=a; s=$3; ty=$2} }
                   END { if (s!="") printf "  %s  (%s)  start=0x%x  +0x%x\n", s, ty, best, t-best;
                         else print "  (no symbols)" }'
done
echo "=== function prologue search (backwards for push %rbp / endbr64) ==="
objdump -d --start-address=$((T-0x400)) --stop-address=$((T+8)) "$CD/chrome" 2>/dev/null \
  | grep -nE "endbr64|push +%rbp" | head -5
