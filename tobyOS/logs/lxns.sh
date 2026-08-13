#!/bin/bash
# Phase 3 slice 8: the namespace gate, as ONE command.
#
#   bash logs/lxns.sh            # namespace gate only
#   bash logs/lxns.sh --clean    # force a full rebuild first
#
# Use --clean whenever `struct proc` has changed (this Makefile has NO header
# dependency tracking, so stale objects keep the old field offsets and corrupt
# memory silently). Slice 8 added proc->uts_ns/ipc_ns/clone_ns_flags, so the
# first build after this slice MUST be a clean one.
#
# Exit status is the result: 0 = green, 1 = something failed.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs

FLAGS="-DFAST_BOOT -DQUICK_BOOT -DLXNS_BOOT"
LOG=logs/lxns.log
TIMEOUT=260

MK=(make "CC=TMP='C:\\t' TEMP='C:\\t' clang" "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc")

if [ "$1" = "--clean" ]; then
    echo "== make clean (struct proc changed)"
    "${MK[@]}" clean > logs/lxns.build.log 2>&1
fi

echo "== building ($FLAGS)"
touch src/kernel.c        # EXTRA_CFLAGS changes do NOT trigger a recompile
# Never hide the build behind >/dev/null: a failed build leaves the PREVIOUS
# iso in place and you test stale bits.
if ! "${MK[@]}" EXTRA_CFLAGS="$FLAGS" iso >> logs/lxns.build.log 2>&1; then
    echo "BUILD FAILED -- not running a stale iso. Errors:"
    grep -E ' error:' logs/lxns.build.log | head -30
    exit 1
fi

echo "== running (timeout ${TIMEOUT}s)"
rm -f "$LOG"
timeout "$TIMEOUT" "/c/Program Files/qemu/qemu-system-x86_64.exe" \
    -cdrom tobyOS.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -boot d -smp 4 -m 5120 \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial file:"$LOG" -no-reboot -display none > /dev/null 2>&1

echo
echo "== gate"
grep -a 'LXNS\|\[ns\]\|\[NSPROC\]\|\[NSBB\]\|\[NSIP\]\|\[nl\]\|^ns:\|ns: ' "$LOG" \
    | sed 's/^\[[0-9 ]*ms\] //' | head -80

RC=0
grep -aq 'LXNS. VERDICT: PASS' "$LOG" || RC=1

# ...AND the expected NUMBER of subtests. "VERDICT: PASS" alone is not enough:
# a test whose binary fails to stage simply does not run, the count drops, and
# the verdict stays PASS. That happened -- linux-ptrace was added, its Makefile
# and harness-table edits silently did not apply, and this gate reported GREEN
# 17/17 exactly as it had before the test existed. A missing test read as a
# pass, which is the same failure shape as a green test over a real bug.
#
# Raise this number when you add a subtest; a mismatch in EITHER direction is
# worth stopping for.
# 18 -> 20 with slice 12 cut 4: /bin/linux-netlink (the control plane, in C)
# and a busybox `ip link add` witness.
WANT_SUBTESTS=${WANT_SUBTESTS:-21}
GOT=$(grep -a 'LXNS. VERDICT' "$LOG" | grep -o 'subtests=[0-9]*' | head -1 | cut -d= -f2)
if [ -n "$GOT" ] && [ "$GOT" != "$WANT_SUBTESTS" ]; then
    echo "   !! LXNS ran $GOT subtests, expected $WANT_SUBTESTS -- a test that"
    echo "      does not run cannot fail, so the verdict alone cannot see this"
    RC=1
fi

# Liveness + fault gates. A run that wedged or faulted is not a pass even if
# the verdict line looks green -- the verdict may simply predate the damage.
HB=$(grep -ac '\[hb\]' "$LOG")
FAULTS=$(grep -ac 'EXCEPTION\|user-mode fault\|PANIC' "$LOG")
echo
echo "== health: heartbeats=$HB faults=$FAULTS"
[ "$HB" -lt 3 ] && { echo "   !! guest did not stay live"; RC=1; }
[ "$FAULTS" != "0" ] && { echo "   !! faults present"; RC=1; }

echo
[ "$RC" = "0" ] && echo "RESULT: GREEN" || echo "RESULT: RED"
exit $RC
