#!/usr/bin/env bash
# power.sh -- shutdown / poweroff / halt / reboot.
#
#   bash logs/power.sh
#
# Two halves, and the second is the one that matters.
#
#  1. The SAFE cases, from /etc/power_test.sh: -k does everything except
#     pull the trigger, `shutdown` with no time is a usage error (which is
#     exactly why every distro requires one), a bad time is rejected.
#
#  2. A REAL power-off. The image is built with -DPOWER_REALTEST, which
#     spawns /bin/poweroff after the harness has printed its verdict, and
#     this gate then checks that QEMU EXITED ON ITS OWN -- no taskkill.
#     Nothing else proves ABI_SYS_POWER reaches the hardware rather than
#     just returning a plausible number.

set -u
cd "$(dirname "$0")/.."

LOG=logs/power.log
rm -f "$LOG"

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
       "EXTRA_CFLAGS=-DCOREUTILS_SELFTEST -DPOWER_REALTEST" iso \
       >logs/.buildlog.txt 2>&1
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
  -boot d -smp 4 -m 5120 -cpu max \
  -serial file:"$LOG" -no-reboot -display none 2>/dev/null &
QPID=$!

# Wait for the machine to go away BY ITSELF. If ABI_SYS_POWER works, QEMU
# exits when the guest writes SLP_EN; if it does not, this times out and
# the gate says so rather than quietly killing the VM and passing.
EXITED=0
for i in $(seq 1 400); do
    if ! kill -0 "$QPID" 2>/dev/null; then EXITED=1; break; fi
    sleep 2
done
if [ "$EXITED" != "1" ]; then
    MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
fi

if ! grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: the harness never reported"
    tail -5 "$LOG" 2>/dev/null
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
OUT=$(sed -n '/@@pw_dry_off/,/@@power_END/p' "$LOG" | kern_strip | tr -d '\r')
if ! echo "$OUT" | grep -q "@@power_END"; then
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

echo "=== -k does everything except pull the trigger ==="
ck pw_dry_off      "shutdown -k now"        'would power off now.*rc=0'
ck pw_dry_reboot   "shutdown -r -k"         'would reboot now.*rc=0'
ck pw_dry_halt     "shutdown -H -k"         'would halt now.*rc=0'
ck pw_poweroff_dry "poweroff -k"            'would power off now.*rc=0'
ck pw_reboot_dry   "reboot -k"              'would reboot now.*rc=0'
ck pw_halt_dry     "halt -k"                'would halt now.*rc=0'

echo "=== a typo must not take the machine down ==="
ck pw_no_time  "shutdown with no time is a usage error" 'usage.*rc=[1-9]'
ck pw_bad_time "an unparseable time is refused"         'invalid time.*rc=[1-9]'

echo "=== the warning says who it reached ==="
ck pw_broadcast "broadcasts, and admits it is this terminal only" \
                'going down NOW'

echo "=== and then it actually powers the machine off ==="
if [ "$EXITED" = "1" ]; then
    echo "  ok   QEMU exited on its own (the guest reached ACPI S5)"; PASS=$((PASS+1))
else
    echo "  FAIL QEMU had to be killed -- poweroff did not reach the hardware"
    FAIL=$((FAIL+1))
fi
if grep -aq "\[power\] pid .* requested action 0" "$LOG"; then
    echo "  ok   the kernel logged the request"; PASS=$((PASS+1))
else
    echo "  FAIL the kernel never logged a power request"; FAIL=$((FAIL+1))
fi
if grep -aq "POWER. poweroff RETURNED" "$LOG"; then
    echo "  FAIL poweroff RETURNED -- it did not power the machine off"
    FAIL=$((FAIL+1))
else
    echo "  ok   poweroff never returned, as it must not"; PASS=$((PASS+1))
fi

echo
echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
