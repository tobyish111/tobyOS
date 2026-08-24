#!/bin/bash
# tvi.sh -- the conformance gate for the native vi, /bin/tvi.
#
#   bash logs/tvi.sh
#
# WHAT IT MEASURES: /bin/tvitest drives /bin/tvi over a REAL pty with a
# scripted keystroke sequence, then compares the RESULTING FILE BYTES to
# what POSIX vi(1) requires. Not "did it exit 0" -- an editor's contract
# is what it leaves on disk. Not a screen comparison either: that would
# be a terminal-emulation test wearing an editor test's name.
#
# A pty is mandatory. tvi refuses to run when stdin is not a terminal
# (rather than scribble escapes into a pipe and call itself an editor),
# so no pipe-based harness can drive it.
#
# TWO HARNESS BUGS ARE MEMORIALISED IN programs/tvitest/main.c, and their
# SIGNATURES are the transferable part:
#   - only the two SHORTEST scripts passed  -> failure correlated with
#     LENGTH -> flow control: tvi repaints ~2 KB per keystroke, the pty
#     filled, and tvi blocked in write().
#   - the pass COUNT stayed the same but WHICH cases passed shuffled
#     between runs -> nondeterminism -> a busy-spin drain starving the
#     process it was waiting for. The bound must be REAL TIME, never an
#     iteration count; logs/ttyparity.sh solved this first.
#
# AND ONE REAL LIBRARY BUG THIS GATE FOUND: libtoby's regexec() returned
# rm_eo = strlen(string) for EVERY match, because its end-finding loop
# ignored its own loop variable. tvi's :s was the first consumer in the
# tree to read rm_eo, so it had been latent. See libtoby/src/regex.c.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
LOG=logs/tvi.log
FLAGS="-DFAST_BOOT -DQUICK_BOOT -DTVI_BOOT"

taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1

echo "== build ($FLAGS)"
touch src/kernel.c
if ! make -j4 "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
             "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
             EXTRA_CFLAGS="$FLAGS" iso > logs/tvi.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/tvi.build.log | head -20
    exit 1
fi
python -c "
import sys
d=open('tobyos.bin','rb').read()
sys.exit(0 if b'[TVI] harness exit=' in d else 1)" \
    || { echo "FAIL: TVI_BOOT harness not compiled into the kernel"; exit 1; }
for b in bin/tvi bin/tvitest; do
    tar -tf build/initrd.tar | grep -qx "$b" || { echo "FAIL: $b not in initrd"; exit 1; }
done

echo "== running"
rm -f "$LOG"
timeout 900 "$QEMU" -cdrom tobyOS.iso -boot d -smp 4 -m 5120 \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial file:"$LOG" -no-reboot -display none > /dev/null 2>&1
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

echo
echo "== cases"
# A WHOLE-LINE grep, deliberately: the [TVI] lines come from a userspace
# program and ARE newline-terminated, and an extraction like '[^[]*' would
# stop at the '[' inside an expected value such as "[a]b" -- which printed
# both sides of that diff as empty and made a real result unreadable.
# ('[[]TVI[]]' rather than '\[TVI\]': see the grep trap in logs/cputelem.sh.)
tr -d '\r' < "$LOG" | grep -a "^[[]TVI[]]"

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

ck  "harness ran"                  '\[TVI\] ==== native vi conformance'
ck  "every case passed"            '\[TVI\] VERDICT: PASS pass=[0-9]+ fail=0'
ck  "a meaningful number of cases" '\[TVI\] VERDICT: PASS pass=(5[0-9]|[6-9][0-9])'
nck "no case failed"               '\[TVI\]   FAIL'
nck "no case timed out"            'editor never exited'
nck "no pty/spawn error"           'spawn/pty error'
ck  "harness exited 0"             '\[TVI\] harness exit=0'

if tr -d '\r' < "$LOG" | grep -qaE 'KERNEL PANIC|PAGE FAULT|GENERAL PROTECTION'; then
    echo "  FAIL kernel faulted"; RC=1
else
    echo "  ok   no fault"
fi

echo
[ "$RC" = 0 ] && echo "RESULT: GREEN" || echo "RESULT: RED"
exit $RC
