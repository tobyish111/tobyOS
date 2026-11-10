#!/bin/bash
# cputelem.sh -- the gate for slices 3 (hwmon/sensors) and 4 (cpufreq/cpupower).
#
#   bash logs/cputelem.sh
#
# THIS GATE CANNOT PROVE THE FEATURE WORKS, AND SAYS SO. QEMU advertises
# neither a thermal sensor (CPUID.06H:EAX bit 6) nor SpeedStep
# (CPUID.01H:ECX bit 7), so on the only machine we can boot on demand
# BOTH modules correctly decline to publish. What it proves is split in
# two, and the split is the point:
#
#   1. THE HONEST-ABSENCE PATH. No hwmon tree, no cpufreq tree, and the
#      two tools say plainly why and exit non-zero. They must never print
#      a nominal temperature or a guessed frequency. This is the half
#      that would regress silently, because "no output" looks like
#      "nothing to report" -- so it is asserted explicitly.
#
#   2. THE DECODERS, against known MSR bit patterns (CPUTELEM_SELFTEST).
#      The arithmetic is where a TjMax subtraction or a ratio field would
#      be silently wrong, and it needs no hardware to test. Nothing in it
#      is published; these are pure functions with spec-defined outputs.
#
# REAL HARDWARE IS STILL OWED. Until this runs on the EliteDesk (Haswell
# i5-4590: DTS + PTM + EIST all present, base ratio 33, TjMax 100) the
# POSITIVE path has never executed. Do not describe these slices as
# working on real hardware until that run exists.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
LOG=logs/cputelem.log
FLAGS="-DFAST_BOOT -DQUICK_BOOT -DPKGPROBE_BOOT -DCPUTELEM_SELFTEST"

taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1

echo "== build ($FLAGS)"
touch src/kernel.c
if ! make -j4 "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
             "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
             EXTRA_CFLAGS="$FLAGS" iso > logs/cputelem.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/cputelem.build.log | head -20
    exit 1
fi
python -c "
import sys
d=open('tobyos.bin','rb').read()
missing=[m for m in (b'[cputherm]', b'[CPUTELEM]', b'/class/hwmon/hwmon0') if m not in d]
sys.exit(1 if missing else 0)" || { echo "FAIL: kernel lacks the telemetry code"; exit 1; }
# The fabricated P-state ratios must be GONE, not merely superseded.
if grep -qa "typical max ratio" tobyos.bin; then
    echo "FAIL: the invented 'typical max ratio' P-state target is still compiled in"
    exit 1
fi
for b in bin/sensors bin/cpupower; do
    tar -tf build/initrd.tar | grep -qx "$b" || { echo "FAIL: $b not in initrd"; exit 1; }
done

echo "== running"
rm -f "$LOG"
timeout 420 "$QEMU" -cdrom tobyOS.iso -boot d -smp 4 -m 5120 \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial file:"$LOG" -no-reboot -display none > /dev/null 2>&1
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

echo
echo "== decoder self-test (the part that does NOT need hardware)"
# GREP TRAP (2026-08-24): with the msys grep this PATH selects, a
# '\[TAG\]' immediately followed by '[^[]' MISPARSES -- the backslashes
# are dropped, [CPUTELEM] becomes a CHARACTER CLASS matching any one of
# C,P,U,T,E,L,M, and 550 lines of boot log land here. It does NOT error;
# it matches the WRONG THING. The POSIX [[]TAG[]] spelling is used instead.
# grep -ao, not a whole-line grep: early-boot serial output is not reliably
# newline-separated, so a whole-line match on a line that also carries the
# bootloader banner dumps the ENTIRE log here. Same lesson as logs/dmi.sh.
tr -d '\r' < "$LOG" | grep -ao '[[]CPUTELEM[]][^[]*'

echo
echo "== what this machine reported"
for m in cputherm cpufreq; do
    tr -d '\r' < "$LOG" | grep -ao "[[]$m[]] [^[]*" | head -2
done
tr -d '\r' < "$LOG" | grep -ao '[[]sysfs[]] /sys/class/hwmon[^[]*' | head -1
tr -d '\r' < "$LOG" | grep -ao '[[]sysfs[]] /sys/devices/system/cpu[^[]*' | head -1

echo
echo "== the two tools, in the guest"
tr -d '\r' < "$LOG" \
    | sed -n '/PKGPROBE. --- hwmon tree/,/PKGPROBE. ==== done/p' \
    | grep -vE '^[[](elf|proc|fork|execve|sys_exit)' | sed 's/^[[][0-9 ]*ms[]] //'

echo
echo "== checks"
RC=0
ck () {
    if tr -d '\r' < "$LOG" | grep -qaE "$2"; then echo "  ok   $1"
    else echo "  FAIL $1   (no match for: $2)"; RC=1; fi
}
nck () {
    if tr -d '\r' < "$LOG" | grep -qaE "$2"; then
        echo "  FAIL $1   (unexpectedly matched: $2)"; RC=1
    else echo "  ok   $1"; fi
}

# --- the decoders (hardware-independent, so these are the real assertions)
ck  "therm decoder all vectors pass"  '\[CPUTELEM\] cputherm_decode: 6/6'
ck  "freq decoder all vectors pass"   '\[CPUTELEM\] cpufreq_msr_decode: 6/6'
ck  "decoder verdict PASS"            '\[CPUTELEM\] VERDICT: PASS'
nck "no decoder vector FAILed"        '\[CPUTELEM\].*FAIL'

# --- the honest-absence path
ck  "therm module explains its refusal" '\[cputherm\] no package thermal sensor: .+'
ck  "freq module explains its refusal"  '\[cpufreq\] (no P-state information|P-state control unavailable): .+'
ck  "hwmon publishes nothing"           '/sys/class/hwmon: no sensor -- nothing published'
ck  "cpufreq publishes nothing"         'cpufreq: no P-state info'
ck  "sensors says no sensors found"     'sensors: no sensors found'
ck  "sensors exits non-zero"            'sensors-rc=[1-9]'
ck  "cpupower says nothing published"   'cpupower: no cpufreq information published'
ck  "cpupower exits non-zero"           'cpupower-rc=[1-9]'

# --- and above all, nothing invented
nck "no fabricated temperature printed" '\+[0-9]+\.[0-9] C'
nck "no fabricated MHz printed"         'current frequency: +[0-9]+ kHz'

if tr -d '\r' < "$LOG" | grep -qaE 'KERNEL PANIC|PAGE FAULT|GENERAL PROTECTION'; then
    echo "  FAIL kernel faulted"; RC=1
else
    echo "  ok   no fault (an ungated rdmsr would #GP here)"
fi

echo
if [ "$RC" = 0 ]; then
    echo "RESULT: GREEN -- decoders proven, absence handled honestly."
    echo "        REAL-HARDWARE RUN STILL OWED: the positive path has"
    echo "        never executed. See logs/cputelem.sh header."
else
    echo "RESULT: RED"
fi
exit $RC
