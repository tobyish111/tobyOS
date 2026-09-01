#!/usr/bin/env bash
# procnet.sh -- vmstat, tload, ss, skill, and the three with no data source.
#
#   bash logs/procnet.sh
#
# Four of these read real kernel state and are checked against the file the
# number came from, not against a plausible shape. The other three MUST
# refuse: slabtop has no /proc/slabinfo, bridge has no ABI to the kernel's
# bridge tables, tracepath has no setsockopt to set a TTL with. A tool that
# printed zeros there would look like a working tool reporting an idle
# machine, which is the failure this gate exists to catch.

set -u
cd "$(dirname "$0")/.."

LOG=logs/procnet.log
rm -f "$LOG"

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

"/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  -boot d -smp 4 -m 5120 -cpu max \
  -serial file:"$LOG" -no-reboot -display none 2>/dev/null &

for i in $(seq 1 220); do
    grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null && break
    sleep 2
done
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
if ! grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: the harness never reported"
    exit 1
fi

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
OUT=$(sed -n '/@@pn_vmstat_header/,/@@procnet_END/p' "$LOG" | kern_strip | tr -d '\r')
if ! echo "$OUT" | grep -q "@@procnet_END"; then
    echo "ABORT: the guest transcript is incomplete"
    echo "$OUT" | tail -5
    exit 1
fi

section() { echo "$OUT" | awk -v k="@@$1" '$0==k{f=1;next} /^@@[a-z_]/{f=0} f'; }

PASS=0; FAIL=0
ck() {
    local g; g=$(section "$1" | tr '\n' '|')
    if echo "$g" | grep -Eq "$3"; then echo "  ok   $2"; PASS=$((PASS+1))
    else echo "  FAIL $2 -- got '$g' (wanted /$3/)"; FAIL=$((FAIL+1)); fi
}

echo "=== vmstat, against the file the numbers came from ==="
ck pn_vmstat_header    "prints the procps header"  'procs.*memory.*swap.*io.*system.*cpu'
# The guest prints raw lines; the arithmetic happens HERE, with the host's
# own tools, so a broken pipeline in the test script cannot masquerade as a
# broken tool.
VMROW=$(section pn_vmstat_row | head -1)
VMFREE=$(echo "$VMROW" | awk '{print $4}')
MFREE=$(section pn_meminfo | head -1 | awk '{print $2}')
if echo "$VMFREE" | grep -Eq '^[0-9]+$'; then
    echo "  ok   the free column is a number ($VMFREE)"; PASS=$((PASS+1))
else
    echo "  FAIL the free column is a number -- row was '$VMROW'"; FAIL=$((FAIL+1))
fi
if echo "$MFREE" | grep -Eq '^[0-9]+$'; then
    echo "  ok   /proc/meminfo agrees it is a number ($MFREE)"; PASS=$((PASS+1))
else
    echo "  FAIL /proc/meminfo free -- got '$(section pn_meminfo | head -1)'"; FAIL=$((FAIL+1))
fi
# vmstat reports kB and reads the same file, so the two must be close. They
# are not sampled at the same instant, so allow a little drift.
if [ -n "$VMFREE" ] && [ -n "$MFREE" ] &&
   echo "$VMFREE" | grep -Eq '^[0-9]+$' && echo "$MFREE" | grep -Eq '^[0-9]+$'; then
    D=$(( VMFREE > MFREE ? VMFREE - MFREE : MFREE - VMFREE ))
    LIM=$(( MFREE / 10 + 1024 ))
    if [ "$D" -le "$LIM" ]; then
        echo "  ok   vmstat's free tracks /proc/meminfo (${VMFREE} vs ${MFREE} kB)"; PASS=$((PASS+1))
    else
        echo "  FAIL vmstat free ${VMFREE} vs meminfo ${MFREE} kB"; FAIL=$((FAIL+1))
    fi
else
    echo "  FAIL could not compare vmstat and meminfo"; FAIL=$((FAIL+1))
fi
# The columns it CANNOT fill must be named, not filled with zeros.
ck pn_vmstat_says_why  "says which counters are missing" 'cpu/io/system columns are unavailable'

echo "=== tload ==="
TL=$(section pn_tload | head -1)
if echo "$TL" | grep -Eq '^[0-9]+\.[0-9][0-9]( |$)'; then
    echo "  ok   prints a real load average ($(echo "$TL" | awk '{print $1}'))"; PASS=$((PASS+1))
else
    echo "  FAIL tload printed '$TL'"; FAIL=$((FAIL+1))
fi

echo "=== ss, from /proc/net ==="
ck pn_ss_header "prints a header"      'Netid.*State.*Local Address'
ck pn_ss_listen "-lt exits cleanly"    'rc=0\|'

echo "=== skill signals a real process ==="
ck pn_skill         "kills a running sleep"  'skill rc=0\|'
ck pn_skill         "and it is gone"         'still running: 0\|'
ck pn_skill_nomatch "no match is a non-zero exit" 'rc=1\|'

echo "=== the three with no data source: each must SAY which ==="
ck pn_slabtop   "slabtop names /proc/slabinfo"   'slabinfo does not exist.*rc=[1-9]'
ck pn_bridge    "bridge names the missing ABI"   'no syscall or /proc file publishes.*rc=[1-9]'
ck pn_tracepath "tracepath names setsockopt"     'no setsockopt.*rc=[1-9]'
ck pn_snice     "snice names setpriority"        'no setpriority.*rc=[1-9]'

echo
echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
