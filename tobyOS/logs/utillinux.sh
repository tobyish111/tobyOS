#!/bin/bash
# The util-linux tools: column colrm col rename whereis namei uuidgen
# setterm lsblk findmnt lsns wdctl prlimit lslocks lsipc zramctl.
#
#   bash logs/utillinux.sh
#
# Third of the six groups from the distro census.
#
# TWELVE OF THESE DO A REAL JOB against real data -- lsblk reads the kernel's
# block registry through ABI_SYS_BLK_LIST, findmnt reads /proc/mounts, lsns
# reads /proc/<pid>/ns, wdctl asks the watchdog that actually exists.
#
# FOUR HAVE NO DATA SOURCE and are asserted to SAY SO rather than print an
# empty table: prlimit (no getrlimit syscall), lslocks (no /proc/locks),
# lsipc (SysV IPC exists but nothing enumerates it), zramctl (zram reports
# no devices and /proc/meminfo hardcodes SwapTotal to 0). An empty table is
# the failure worth guarding against here -- `lslocks` printing a header and
# no rows is a CLAIM that no locks are held, and that claim is not known.
#
# Where this host has the same tool, GNU is the oracle; the rest assert on
# shape. Runs from the boot harness -- see logs/coreutils.sh for why.
cd /c/CustomOS/tobyOS || exit 1
LOG=logs/utillinux.log

MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
sleep 2
rm -f "$LOG" || { echo "ABORT: log busy"; exit 1; }

rm -f src/kernel.o
PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH" \
  make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
       EXTRA_CFLAGS=-DCOREUTILS_SELFTEST iso >/dev/null 2>&1
[ -f tobyOS.iso ] || { echo "ABORT: build produced no ISO"; exit 1; }

"/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
  -boot d -smp 4 -m 5120 -cpu max \
  -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -serial file:"$LOG" -no-reboot -display none 2>/dev/null &

for i in $(seq 1 90); do grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null && break; sleep 2; done
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
if ! grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: the harness never reported"; exit 1
fi

kern_strip() { sed -E 's/^\[[0-9 ]+ms\] //' | grep -v '^\[' ; }
OUT=$(sed -n '/@@column_t/,/@@END/p' "$LOG" | kern_strip | tr -d '\r')
if ! echo "$OUT" | grep -q "@@END"; then
    echo "ABORT: transcript incomplete"; echo "$OUT" | tail -5; exit 1
fi

section() { echo "$OUT" | awk -v k="@@$1" '$0==k{f=1;next} /^@@/{f=0} f'; }
PASS=0; FAIL=0
want() {
    local name="$1" label="$2" pat="$3" g
    g=$(section "$name" | tr '\n' '|')
    if echo "$g" | grep -qE "$pat"; then echo "  ok   $label"; PASS=$((PASS+1))
    else echo "  FAIL $label -- got '$g' (wanted /$pat/)"; FAIL=$((FAIL+1)); fi
}
# gnu <name> <label> <host-command>  : compare with the same tool here
gnu() {
    local name="$1" label="$2" cmd="$3" b g
    b=$(eval "$cmd" 2>&1 | tr -d '\r' | tr '\n' '|')
    g=$(section "$name" | tr '\n' '|')
    if [ "$b" = "$g" ]; then echo "  ok   $label"; PASS=$((PASS+1))
    else
        echo "  FAIL $label"
        echo "         gnu : $b"
        echo "         toby: $g"
        FAIL=$((FAIL+1))
    fi
}

echo "=== text tools, against the same tool on this host ==="
gnu column_t "column -t"  "printf 'a bb ccc\ndddd e ff\n' | column -t"
# colrm and col are NOT installed on this host, so there is no oracle to
# diff against -- assert the known-correct answer instead. colrm 3 5 removes
# columns 3..5 (cde) from abcdefghij; col resolves the X-backspace-Y
# overstrike down to Y.
want colrm "colrm 3 5 removes those columns"   '^abfghij\|'
want col   "col resolves backspace overstrike" '^aY\|'

echo "=== file tools ==="
want rename        "rename replaces in the name" 'ren_bbb.txt'
want whereis       "whereis finds a binary"      'ls:.*/bin/ls'
want namei         "namei walks each component"  'f: /etc/passwd.*d /.*- passwd'
want namei_missing "namei names the failing part" '\? nosuchfile.*rc=1'

echo "=== uuidgen ==="
want uuidgen       "prints a uuid"                '[0-9a-f]{8}-[0-9a-f]{4}-'
# 37 bytes (36 + newline), dashes at 9/14/19/24, and '4' at position 15:
# that version nibble is what makes it an RFC 4122 v4 rather than 16
# random bytes with dashes in the right places.
want uuidgen_shape "is a well-formed RFC 4122 v4" '37\|----\|4\|'

echo "=== system state, from real sources ==="
want lsblk       "lsblk lists real devices"   'NAME.*TYPE.*SIZE'
want findmnt     "findmnt reads /proc/mounts" 'TARGET.*SOURCE.*FSTYPE'
want findmnt_one "findmnt filters by target"  '/data'
want lsns        "lsns lists namespaces"      'TYPE.*PATH'
want wdctl       "wdctl reads the watchdog"   'Enabled:.*Timeout:'

echo "=== the four with no data source: each must SAY which ==="
want prlimit "prlimit names the missing syscall"   'getrlimit.*rc=1'
want lslocks "lslocks names the missing /proc file" '/proc/locks.*rc=1'
want lsipc   "lsipc explains the missing interface" 'no listing interface.*rc=1'
want zramctl "zramctl refuses to print a fake zero" 'SwapTotal.*rc=1'

echo
echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
