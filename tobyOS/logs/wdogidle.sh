#!/bin/bash
# wdogidle.sh -- does the watchdog notice when pid 0 stops running?
#
#   bash logs/wdogidle.sh
#
# WHY: on 2026-08-25 an EliteDesk froze at idle. The serial heartbeat --
# whose own comment says "if pid 0 ever wedges, the beat simply stops,
# which is itself the signal" -- stopped, and NOT ONE LINE came out. The
# watchdog only tested for a GLOBAL scheduler stall, and the other CPUs
# were scheduling fine, so it saw nothing wrong.
#
# This wedges pid 0 for real (15 s, interrupts on, BKL not held) and
# requires the watchdog to bite. A detector that never fires is worse
# than no detector, so this asserts the BITE, not just a clean boot.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
LOG=logs/wdogidle.log
FLAGS="-DFAST_BOOT -DQUICK_BOOT -DWDOG_IDLE_STALL_TEST"

echo "== building ($FLAGS)"
touch src/kernel.c
if ! make -j4 "CC=TMP='C:\t' TEMP='C:\t' clang" \
          "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
          EXTRA_CFLAGS="$FLAGS" iso > logs/wdogidle.build.log 2>&1; then
    echo "BUILD FAILED:"; grep -E ' error:' logs/wdogidle.build.log | head -20; exit 1
fi

echo "== running (timeout 240s)"
rm -f "$LOG"
timeout 240 "/c/Program Files/qemu/qemu-system-x86_64.exe" \
    -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -boot d -smp 4 -m 5120 -serial file:"$LOG" \
    -no-reboot -display none > /dev/null 2>&1
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

echo
grep -aF -e "[WDIDLE]" -e "[wdog]" "$LOG" | sed 's/^\[[0-9 ]*ms\] //'
echo
echo "== gate"
RC=0
ck () { if grep -aqF "$2" "$LOG"; then echo "  ok   $1"; else echo "  FAIL $1"; RC=1; fi; }
ck "pid 0 was actually wedged"        "[WDIDLE] wedging pid 0"
ck "watchdog BIT during the wedge"    "kind=idle_stall"
ck "it named the stall duration"      "idle loop stalled"
ck "pid 0 recovered afterwards"       "[WDIDLE] pid 0 released"
# Exactly one bite per episode: a detector that re-fires every second
# turns a freeze into a wall of identical lines.
#
# Count the kprintf BITE lines, NOT every line mentioning the kind --
# wdog_record_event deliberately reports through slog AND kprintf, so a
# single event produces two lines and the naive count said 2.
N=$(grep -acF "[wdog] BITE" "$LOG"); N=${N:-0}
if [ "$N" = "1" ]; then echo "  ok   exactly one bite (got $N)"
else echo "  FAIL expected exactly 1 bite, got $N"; RC=1; fi
ck "and it is the FIRST event, not a repeat" "BITE event=1 kind=idle_stall"
# And the beat must resume -- proving the bite did not wedge anything.
if [ "$(grep -acF '[hb] #' "$LOG")" -gt 5 ]; then echo "  ok   heartbeat still running after the bite"
else echo "  FAIL heartbeat did not resume"; RC=1; fi

# ---------------------------------------------------------------------------
# PHASE 2 -- THE NEGATIVE CASE. Added 2026-08-27.
#
# Phase 1 proves the watchdog FIRES when pid 0 is wedged. Nothing proved it
# STAYS SILENT when nothing is wrong, and it did not: a flake-hunt boot caught
#   [wdog] BITE kind=idle_stall reason='pid 0 idle loop stalled
#          18446744073709551615ms (beats=2908, ...)'
# on a healthy system. That number is UINT64_MAX -- `now` is sampled by
# wdog_check() and a kick from another CPU landing in the gap made
# g_last_idle_kick_ms NEWER than `now`, so the subtraction wrapped and beat any
# timeout. Fixed by snapshotting and guarding in BOTH bite arms (src/watchdog.c);
# wdog_status() had always guarded it, the deciding arms had not.
#
# A detector is only proven by BOTH directions -- the same reason the fs gate
# asserts the DENIAL. Cheap here: rebuild WITHOUT the wedge flag and require
# zero bites across a boot long enough to have caught the original.
echo
echo "== phase 2: healthy boot must produce NO bite"
FLAGS2="-DFAST_BOOT -DQUICK_BOOT"
LOG2=logs/wdogidle-healthy.log
touch src/kernel.c
if ! make -j4 "CC=TMP='C:	' TEMP='C:	' clang"           "HOST_CC=TMP='C:	' TEMP='C:	' gcc"           EXTRA_CFLAGS="$FLAGS2" iso > logs/wdogidle2.build.log 2>&1; then
    echo "  FAIL phase-2 build"; grep -E ' error:' logs/wdogidle2.build.log | head -10; exit 1
fi
rm -f "$LOG2"
timeout 240 "/c/Program Files/qemu/qemu-system-x86_64.exe"     -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk     -boot d -smp 4 -m 5120 -serial file:"$LOG2"     -no-reboot -display none > /dev/null 2>&1
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

NB=$(grep -acF "[wdog] BITE" "$LOG2"); NB=${NB:-0}
if [ "$NB" = "0" ]; then echo "  ok   no bite on a healthy boot"
else
    echo "  FAIL watchdog bit a healthy system ($NB time(s)) -- FALSE POSITIVE"
    grep -aF "[wdog] BITE" "$LOG2" | head -3 | sed 's/^\[[0-9 ]*ms\] //'
    RC=1
fi
# The boot must have been long enough for the check to run at all, or "no
# bite" is vacuous -- the same trap as a guard that never executes.
HB2=$(grep -acF '[hb] #' "$LOG2"); HB2=${HB2:-0}
if [ "$HB2" -gt 5 ]; then echo "  ok   healthy boot ran long enough to be meaningful ($HB2 beats)"
else echo "  FAIL healthy boot produced only $HB2 beats -- verdict is vacuous"; RC=1; fi

echo
if [ "$RC" = "0" ]; then echo "VERDICT: PASS"; else echo "VERDICT: FAIL"; fi
exit $RC
