#!/bin/bash
# gateall.sh -- run the Linux-solidity gates as ONE command and print ONE verdict.
#
#   bash logs/gateall.sh            # core set   (~30 min)
#   bash logs/gateall.sh --full     # everything (hours)
#   bash logs/gateall.sh --list     # show the set and exit
#
# Exit status IS the result: 0 = every gate green, 1 = at least one red.
#
# ---------------------------------------------------------------------------
# WHY THIS EXISTS
#
# There were fourteen gate scripts and no single command that answered green or
# red, so "is Linux solid?" was fourteen invocations and a memory of which
# numbers are supposed to come back. Anything that tedious gets skipped, and a
# gate that gets skipped is not a gate.
#
# THREE FOOTGUNS THIS EXISTS TO CONTAIN, all of which cost real time on
# 2026-08-27:
#
#   1. EVERY GATE REBUILDS tobyOS.iso. Running one right after producing a
#      shippable image silently destroys it. This preserves the ISO up front
#      and restores it at the end, so a ship build survives a gate run.
#
#   2. lxsock.sh DOES NOT BUILD. Its header says it assumes an iso built with
#      -DLXSOCK_BOOT; point it at whatever ISO happens to be lying around and
#      it prints "SERVER NEVER LISTENED", which reads like a socket bug rather
#      than wrong build flags. Gates that need their own flags declare them
#      here and the runner builds first.
#
#   3. TWO HARNESSES AT ONCE POISON EACH OTHER. Every one of them starts by
#      taskkill-ing qemu, so a second run shoots the first one's guest down and
#      produces DEAD/FROZEN results that look exactly like kernel faults. This
#      shares ONE lock file with logs/flakeloop.sh so the two are mutually
#      exclusive, and stopping the wrapper that launched a background run does
#      NOT kill the loop underneath -- use the pid the refusal prints.
#
# AND ONE REPORTING TRAP: ttyparity.sh and shparity.sh EXIT 0 EVEN WHEN THEIR
# VERDICT IS FAIL (measured). So exit status alone is not trustworthy: each
# gate declares the verdict line it must print, a missing verdict is RED, and
# both signals have to agree or the gate is reported SUSPECT rather than
# quietly passed.
#
# oilspec is deliberately NOT in either set. It is a TRACKING gate, not a
# pass/fail one -- ~1140 of its 2757 cases are bash-only extensions tsh does
# not implement, so it is permanently "FAIL" by design and would make this
# suite permanently red. Run `bash logs/oilspec.sh` and compare failure SETS
# against the previous archive; see the oilspec memory for that recipe.
# ---------------------------------------------------------------------------
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs/flakehunt

# ---- the gate table -------------------------------------------------------
# name | script | verdict-must-contain | extra build flags (empty = self-building)
CORE="\
lxposix|logs/lxposix.sh|LXPOSIX] VERDICT: PASS|
lxns|logs/lxns.sh|LXNS] VERDICT: PASS|
ttyparity|logs/ttyparity.sh|TTYPARITY] VERDICT: PASS|
wdogidle|logs/wdogidle.sh|VERDICT: PASS|
lxsock|logs/lxsock.sh|RESULT: GREEN|-DFAST_BOOT -DQUICK_BOOT -DLXSOCK_BOOT"

FULL="$CORE
shparity|logs/shparity.sh|SHPARITY] VERDICT: PASS|
fsmatrix|logs/fsmatrix.sh|VERDICT: PASS|
fatinterop|logs/fatinterop.sh|VERDICT: PASS|
fsprobe|logs/fsprobe.sh|VERDICT: PASS|
edit|logs/edit.sh|VERDICT: PASS|"

SET="$CORE"; LABEL="core"
[ "$1" = "--full" ] && { SET="$FULL"; LABEL="full"; }
if [ "$1" = "--list" ]; then
    echo "$SET" | while IFS='|' read -r n s v f; do
        [ -z "$n" ] && continue
        printf "  %-11s %-22s %s\n" "$n" "$s" "${f:+(builds: $f)}"
    done
    exit 0
fi

# ---- mutual exclusion, shared with flakeloop.sh ----------------------------
LOCK=logs/flakehunt/.lock
if [ -e "$LOCK" ] && kill -0 "$(cat "$LOCK" 2>/dev/null)" 2>/dev/null; then
    echo "REFUSING TO START: another QEMU harness (pid $(cat "$LOCK")) is running."
    echo "  Both results would be garbage -- they taskkill each other's guest."
    echo "  Stop it with:  kill -9 $(cat "$LOCK")   (TaskStop alone will NOT)"
    exit 1
fi
echo $$ > "$LOCK"
trap 'rm -f "$LOCK"; restore_iso' EXIT INT TERM

# ---- preserve any shippable image -----------------------------------------
SAVED_ISO=""
restore_iso() {
    if [ -n "$SAVED_ISO" ] && [ -f "$SAVED_ISO" ]; then
        mv -f "$SAVED_ISO" tobyOS.iso 2>/dev/null
        [ -f "${SAVED_ISO%.iso}.bin" ] && mv -f "${SAVED_ISO%.iso}.bin" tobyos.bin 2>/dev/null
        echo "== restored the pre-run tobyOS.iso"
    fi
}
if [ -f tobyOS.iso ]; then
    SAVED_ISO="logs/gateall-preserved.iso"
    cp -f tobyOS.iso "$SAVED_ISO"
    [ -f tobyos.bin ] && cp -f tobyos.bin "${SAVED_ISO%.iso}.bin"
    echo "== preserved tobyOS.iso (restored when this finishes)"
fi

STAMP=$(date +%Y%m%d-%H%M%S)
OUT="logs/gateall-$STAMP"
mkdir -p "$OUT"
echo "== gateall $STAMP ($LABEL set) -> $OUT"
echo

RC=0
RESULTS=""
while IFS='|' read -r name script verdict flags; do
    [ -z "$name" ] && continue
    if [ ! -f "$script" ]; then
        printf "  %-11s MISSING (%s)\n" "$name" "$script"
        RESULTS="$RESULTS$name|MISSING|-\n"; RC=1; continue
    fi
    t0=$(date +%s)

    # Gates that do not build for themselves declare their flags above.
    if [ -n "$flags" ]; then
        touch src/kernel.c
        if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
                  "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
                  EXTRA_CFLAGS="$flags" iso > "$OUT/$name.build.log" 2>&1; then
            printf "  %-11s BUILD-FAILED\n" "$name"
            RESULTS="$RESULTS$name|BUILD-FAILED|-\n"; RC=1; continue
        fi
    fi

    bash "$script" > "$OUT/$name.out" 2>&1
    ex=$?
    t=$(( $(date +%s) - t0 ))

    # Two independent signals. They must AGREE.
    if grep -aqF "$verdict" "$OUT/$name.out" || grep -aqF "$verdict" "logs/$name.log" 2>/dev/null; then
        said_pass=1
    else
        said_pass=0
    fi

    if [ "$said_pass" = "1" ] && [ "$ex" = "0" ]; then
        state="PASS"
    elif [ "$said_pass" = "1" ] && [ "$ex" != "0" ]; then
        # ttyparity/shparity exit 0 on failure, so the reverse (verdict says
        # pass, status says fail) is the one that means something is off.
        state="SUSPECT"; RC=1
    elif [ "$said_pass" = "0" ] && [ "$ex" = "0" ]; then
        # The measured ttyparity case: FAIL verdict, exit 0. Trust the verdict.
        state="FAIL"; RC=1
    else
        state="FAIL"; RC=1
    fi

    printf "  %-11s %-8s %4ss   %s\n" "$name" "$state" "$t" "$OUT/$name.out"
    RESULTS="$RESULTS$name|$state|${t}s\n"
done <<< "$SET"

echo
echo "== summary ($LABEL)"
printf "$RESULTS" | while IFS='|' read -r n st t; do
    [ -z "$n" ] && continue
    printf "   %-11s %-12s %s\n" "$n" "$st" "$t"
done
echo
if [ "$RC" = "0" ]; then
    echo "GATEALL: GREEN -- every gate in the $LABEL set passed."
else
    echo "GATEALL: RED -- see the per-gate .out files in $OUT"
fi
exit $RC
