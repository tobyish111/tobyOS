#!/bin/bash
# flakeloop.sh -- unattended boot loop hunting the wake-after-death flake.
#
#   bash logs/flakeloop.sh [runs] [guest_idle_seconds]
#   bash logs/flakeloop.sh 40 300          # the overnight shape
#   SKIP_BUILD=1 bash logs/flakeloop.sh 3 60
#
# Exit status IS the result, repo convention: 0 = N boots, no misbehaviour.
# 1 = at least one HIT captured. A HIT is the POINT of this script -- it means
# the hunt produced evidence -- but it is still the kernel misbehaving, so it
# reports RED.
#
# ---------------------------------------------------------------------------
# WHY THIS EXISTS
#
# The flake is ~1-in-15 boots, idle-time, on an AP, in the scheduler switch
# path. It has fired twice with the guards SILENT and left only a corpse. As of
# dc9c955 sched_ctx_plausible() is unconditional, so the next fire should leave
# a NAMED refusal instead of a wild jump. Nobody has yet run enough boots to
# make it fire WITH that net in place. That is the whole job: sampling.
#
# ---------------------------------------------------------------------------
# WHAT THE FIRST SMOKE RUN TAUGHT (2026-08-26) -- both cost a false verdict:
#
# 1. THE CRASH DUMP REPLAYS FOREVER. /data/crash/last.dump is written by
#    panic.c and NEVER deleted (the whole of crash_recovery.c -- crash_clear
#    included -- has zero callers, and the live writer hardcodes boot_seq=1
#    behind a TODO). So EVERY boot re-prints an old panic through
#    /bin/crashinfo, and a naive grep for "unhandled exception" calls a
#    perfectly healthy boot a PANIC. It did, on run 1.
#
#    THE DISCRIMINATOR: live kernel output carries a "[NNNN ms]" timestamp.
#    The replay is the crashinfo PROGRAM's stdout and carries none. Every
#    fault pattern below is therefore anchored to ^[ NNNN ms].
#
# 2. TIME IS NOT WALL TIME. The guest runs at roughly half realtime on this
#    host, the first heartbeat lands ~63 s of GUEST time into boot, and
#    heartbeats are 2 s apart after that. A 120 s wall window bought ~0 s of
#    post-boot idle -- i.e. it never reached the state the flake lives in.
#    So idling is paced in HEARTBEATS (guest time), never in wall seconds.
#
# OTHER TRAPS ENCODED HERE (each already cost someone a session in this repo):
#   - grep -c already prints 0. cnt=$(grep -c ... || echo 0) yields the
#     two-line string "0\n0" and every later [ "$cnt" -gt N ] dies with
#     "integer expected" -- which is how validate.sh once reported ALIVE for
#     runs where the kernel never started.
#   - Serial logs contain binary. Without grep -a, grep prints
#     "binary file matches" and every count reads 0.
#   - MSYS timeout cannot kill QEMU. Use taskkill.
#   - Never confuse OUR kill with a freeze: judge liveness only while QEMU is
#     still running, and only on heartbeat COUNT stalling.
#   - EXTRA_CFLAGS does not trigger a recompile -- touch src/kernel.c.
#   - Build and QEMU want a real Windows-native TMP, and it must ride ON the
#     compiler variable or MSYS mangles it.
#   - Never hide the build: a failed build leaves the PREVIOUS iso in place
#     and the whole run then silently tests stale bits.
#   - DO NOT resolve a captured rip against whatever tobyos.bin is on disk
#     afterwards -- the next gate builds with different flags and addr2line
#     then produces confident nonsense. This script archives the exact binary.
#   - AND: a panic prints TWO rips. The one on the [kpf] line is the real trap
#     frame. The one in the register block below it is kpanic's own
#     capture_regs() return address (~0xffffffff800124xx, near-constant across
#     builds) and means NOTHING. Read the [kpf] line.
# ---------------------------------------------------------------------------
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t logs/flakehunt

# ---------------------------------------------------------------------------
# EXCLUSION LOCK. 2026-08-26: five flakeloop instances ended up running at once
# because stopping the wrapper that launched one does NOT kill the detached
# bash loop underneath it. Every loop begins each run with
# `taskkill //IM qemu-system-x86_64.exe`, which kills EVERY loop's guest -- so
# they silently shot each other down and produced DEAD/FROZEN runs that looked
# like kernel faults. Two of those bogus runs were nearly diagnosed as real
# bugs. This is the repo's "never two gates at once" law, enforced by the tool
# instead of by memory.
LOCK=logs/flakehunt/.lock
if [ -e "$LOCK" ] && kill -0 "$(cat "$LOCK" 2>/dev/null)" 2>/dev/null; then
    echo "REFUSING TO START: flakeloop pid $(cat "$LOCK") is already running."
    echo "  Its results and this one's would both be garbage (mutual taskkill)."
    echo "  Stop it with:  kill -9 $(cat "$LOCK")   (TaskStop alone will NOT)"
    exit 1
fi
mkdir -p logs/flakehunt
echo $$ > "$LOCK"
trap 'rm -f "$LOCK"' EXIT INT TERM

RUNS=${1:-40}
IDLE=${2:-300}                 # GUEST idle seconds after the first heartbeat
MEM=${MEM:-5120}
BOOT_MAX=${BOOT_MAX:-300}      # wall seconds to wait for heartbeat #1
STALL_POLLS=${STALL_POLLS:-15} # 15 polls = 30 s wall with hb every ~3.5 s wall
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"

# Live-fault patterns, anchored to the timestamp so the crashinfo replay of an
# OLD panic cannot masquerade as a fresh one. See note 1 above.
TS='^\[[0-9 ]+ ms\] '
PAT_GUARD="${TS}.*REFUSED switch into"
PAT_PANIC="${TS}.*(KERNEL PANIC|unhandled exception|KERNEL fault)"
PAT_WDOG="${TS}.*BITE event="
PAT_WILD="${TS}.*unknown number"
PAT_FAULT="${TS}.*(EXCEPTION|KERNEL PANIC|user-mode fault)"
PAT_SOFT="${TS}.*(REFUSED wake of dying|DROPPED queued dying)"

# -DSCHEDGUARD_SELFTEST is deliberately ON for the whole loop. The guard fires
# perhaps once in fifteen boots, so without it a clean 40-boot run cannot tell
# "the flake did not happen" apart from "the guard was inert" -- which is
# EXACTLY the failure mode that hid this bug for months (every do_switch
# validation used to sit behind #ifdef CHROMIUM_BOOT, so the shipped kernel had
# none of it). Each boot now self-certifies the predicate before it idles.
FLAGS="-DSCHEDGUARD_SELFTEST ${WORKLOAD_FLAGS:-}"

STAMP=$(date +%Y%m%d-%H%M%S)
OUT="logs/flakehunt/loop-$STAMP"
mkdir -p "$OUT"
TALLY="$OUT/tally.tsv"
printf 'run\tverdict\thb\tboot_s\tidle_hb\tdetail\n' > "$TALLY"

echo "=== flakeloop $STAMP: runs=$RUNS guest_idle=${IDLE}s mem=$MEM flags='$FLAGS'"
echo "=== output: $OUT"

# ---- GATE 0: build, and archive the binary the loop will actually boot -----
if [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "== building ($FLAGS)"
  touch src/kernel.c
  if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
            "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
            EXTRA_CFLAGS="$FLAGS" iso > "$OUT/build.log" 2>&1; then
      echo "BUILD FAILED -- refusing to boot a stale iso. Errors:"
      grep -E ' error:' "$OUT/build.log" | head -20
      exit 1
  fi
fi
cp tobyos.bin "$OUT/tobyos.bin" 2>/dev/null
echo "== archived tobyos.bin ($(stat -c %s "$OUT/tobyos.bin" 2>/dev/null) bytes) for later addr2line"

HITS=0
GUARD_PROVEN=0
SOFTS=0
SOFT_KEPT=0
PREVCRASH=0
REGRUNS=0
REGEVENTS=0
DSCHEDRUNS=0
DSCHEDEVENTS=0
STKRUNS=0
STKEVENTS=0

hbcount() { local c; c=$(grep -ac 'hb\] #' "$1" 2>/dev/null); [ -z "$c" ] && c=0; echo "$c"; }

for n in $(seq 1 "$RUNS"); do
  LOG="$OUT/run$(printf '%03d' "$n").log"
  taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1
  sleep 1
  rm -f "$LOG"
  T0=$(date +%s)

  "$QEMU" -cdrom tobyOS.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk -boot d \
    -smp 4 -m "$MEM" \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial file:"$LOG" -no-reboot -display none &
  QPID=$!

  verdict=""; detail=""; boot_s=0; hb=0

  check_hit() {
    # Returns 0 (true) and sets verdict/detail if this log shows a LIVE fault.
    local p v
    for p in "GUARD-FIRED:$PAT_GUARD" "PANIC:$PAT_PANIC" \
             "WDOG-BITE:$PAT_WDOG" "WILD-SYSCALL:$PAT_WILD"; do
      v=${p%%:*}
      if grep -aqE "${p#*:}" "$LOG" 2>/dev/null; then
        verdict="$v"
        detail=$(grep -aEm1 "${p#*:}" "$LOG" | tr -d '\r' | cut -c1-170)
        return 0
      fi
    done
    return 1
  }

  # ---- phase 1: boot. Wait for heartbeat #1 (~63 s guest, ~110 s wall). ----
  while :; do
    sleep 2
    if check_hit; then break; fi
    hb=$(hbcount "$LOG")
    [ "$hb" -ge 1 ] && break
    if [ $(( $(date +%s) - T0 )) -ge "$BOOT_MAX" ]; then
      verdict="DEAD"; detail="no heartbeat within ${BOOT_MAX}s wall"
      break
    fi
  done
  boot_s=$(( $(date +%s) - T0 ))

  # ---- phase 2: idle, paced in HEARTBEATS (guest time), not wall seconds ---
  if [ -z "$verdict" ]; then
    hb_start=$hb
    hb_target=$(( hb_start + IDLE / 2 ))     # heartbeats are 2 s of guest time
    prev=-1; stuck=0
    while [ "$hb" -lt "$hb_target" ]; do
      sleep 2
      if check_hit; then break; fi
      hb=$(hbcount "$LOG")
      if [ "$hb" = "$prev" ]; then stuck=$((stuck+1)); else stuck=0; fi
      prev=$hb
      if [ "$stuck" -ge "$STALL_POLLS" ]; then
        verdict="FROZEN"; detail="hb stalled at $hb for $((STALL_POLLS*2))s wall"
        break
      fi
    done
  fi

  idle_hb=$(( hb - ${hb_start:-0} ))
  kill $QPID 2>/dev/null
  taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1
  sleep 1

  # Post-mortem: a fault in the final two seconds, or the softer dying-proc
  # guards, which do not stop the machine and so never break the poll loop.
  if [ -z "$verdict" ]; then
    if grep -aqE "$PAT_FAULT" "$LOG" 2>/dev/null; then
      verdict="FAULT"; detail=$(grep -aEm1 "$PAT_FAULT" "$LOG" | tr -d '\r' | cut -c1-170)
    else
      verdict="ALIVE"
    fi
  fi

  # The dying-proc guards (DROPPED queued / REFUSED wake) are INFORMATIONAL,
  # not hits. Measured on a clean boot:
  #     [sched] DROPPED queued dying pid=2 'audioplay-boot' (state=4)
  # state 4 is PROC_TERMINATED -- a boot-time helper that exited while still
  # on the run queue, which is precisely the net doing its designed job. It
  # fires on ordinary boots, so calling it a hit would bury the real signal
  # under one kept log per boot. Counted; the first example is kept for
  # reference, the rest are tallied only.
  # SYSDIAG: the register-corruption instrument (-DSYSDIAG_REGCHECK) LOGS and
  # then SURVIVES, restoring the good pointer instead of faulting. So a boot
  # that caught the bug looks perfectly ALIVE and its log would be pruned --
  # destroying the evidence this build exists to collect. Checked here rather
  # than in check_hit deliberately: NOT breaking the poll loop early lets one
  # boot accumulate many events across the whole idle window.
  nreg=$(grep -ac "CALLEE-SAVED CORRUPTION" "$LOG" 2>/dev/null)
  [ -z "$nreg" ] && nreg=0
  if [ "$nreg" -gt 0 ]; then
    REGRUNS=$((REGRUNS+1))
    REGEVENTS=$((REGEVENTS+nreg))
    [ "$verdict" = "ALIVE" ] && verdict="REGCORRUPT"
    detail="${nreg}x $(grep -am1 "CALLEE-SAVED CORRUPTION" "$LOG" | tr -d '
' | cut -c1-150)"
  fi

  # DUAL-SCHEDULE tripwire (now unconditional in sched.c). Two CPUs switching
  # into the same proc share one kernel stack + register image -- the prime
  # suspect for the register corruption above, and invisible to
  # sched_ctx_plausible because the saved_rsp involved is perfectly valid.
  ndsc=$(grep -ac "DUAL-SCHEDULE" "$LOG" 2>/dev/null)
  [ -z "$ndsc" ] && ndsc=0
  if [ "$ndsc" -gt 0 ]; then
    DSCHEDRUNS=$((DSCHEDRUNS+1))
    DSCHEDEVENTS=$((DSCHEDEVENTS+ndsc))
    [ "$verdict" = "ALIVE" ] && verdict="DUAL-SCHED"
    detail="${ndsc}x $(grep -am1 "DUAL-SCHEDULE" "$LOG" | tr -d '
' | cut -c1-140)"
  fi

  # HYPOTHESIS 2: this CPU running a proc on another proc's syscall stack.
  nstk=$(grep -ac "SYSCALL-STACK MISMATCH" "$LOG" 2>/dev/null)
  [ -z "$nstk" ] && nstk=0
  if [ "$nstk" -gt 0 ]; then
    STKRUNS=$((STKRUNS+1))
    STKEVENTS=$((STKEVENTS+nstk))
    [ "$verdict" = "ALIVE" ] && verdict="STACK-MISMATCH"
    detail="${nstk}x $(grep -am1 "SYSCALL-STACK MISMATCH" "$LOG" | tr -d '
' | cut -c1-140)"
  fi

  soft=""
  if grep -aqE "$PAT_SOFT" "$LOG" 2>/dev/null; then
    soft=$(grep -aEm1 "$PAT_SOFT" "$LOG" | tr -d '\r' | cut -c1-170)
    SOFTS=$((SOFTS+1))
    [ -z "$detail" ] && detail="soft: $soft"
  fi

  # BACKSTOP, paired with the M28B dump-retire fix in kernel.c: the dump is
  # now renamed away once reported, so "crash dump present" at boot on any run
  # after the first means the PREVIOUS run panicked -- including in a window
  # this loop's polling could have missed entirely. A cheap independent check
  # on our own detection, which is worth having when the thing being hunted
  # has twice evaded the instrumentation aimed at it.
  if [ "$n" -gt 1 ] && grep -aq 'M28B: crash dump present' "$LOG" 2>/dev/null; then
    PREVCRASH=$((PREVCRASH+1))
    if [ "$verdict" = "ALIVE" ]; then
      verdict="PREV-CRASHED"
      detail="a dump was waiting at boot -- run $((n-1)) panicked and we missed it"
    fi
  fi

  # GATE 0, continued: the guard must be PROVEN live in this binary on this
  # boot, or a clean loop means nothing at all.
  if grep -aq 'SCHEDGUARD. VERDICT: PASS' "$LOG" 2>/dev/null; then
    GUARD_PROVEN=$((GUARD_PROVEN+1))
  elif grep -aq 'SCHEDGUARD. VERDICT: FAIL' "$LOG" 2>/dev/null; then
    verdict="GUARD-BROKEN"
    detail=$(grep -am1 'SCHEDGUARD. FAIL' "$LOG" | tr -d '\r' | cut -c1-170)
  fi

  printf '%d\t%s\t%s\t%s\t%s\t%s\n' "$n" "$verdict" "$hb" "$boot_s" "$idle_hb" "$detail" >> "$TALLY"
  printf 'run %3d/%s: %-12s hb=%-4s boot=%3ss idle=%3shb  %s\n' \
         "$n" "$RUNS" "$verdict" "$hb" "$boot_s" "$idle_hb" \
         "$(echo "$detail" | cut -c1-80)"

  if [ "$verdict" = "ALIVE" ]; then
    if [ -n "$soft" ] && [ "$SOFT_KEPT" = "0" ]; then
      SOFT_KEPT=1
      echo "    (soft-guard example kept for reference: $LOG)"
    else
      rm -f "$LOG"          # a clean boot is just a tally row; keep the disk
    fi
  else
    HITS=$((HITS+1))
    echo "    !! evidence kept: $LOG"
  fi
  unset hb_start
done

echo
echo "=== tally ($TALLY)"
awk -F'\t' 'NR>1{c[$2]++} END{for(v in c) printf "   %-13s %d\n", v, c[v]}' "$TALLY"
echo "   guard self-certified PASS on $GUARD_PROVEN/$RUNS boots"
echo "   soft dying-proc guard events (informational): $SOFTS/$RUNS boots"
echo "   dumps found waiting at boot (= a missed panic): $PREVCRASH"
echo "   REGISTER-CORRUPTION: $REGEVENTS event(s) across $REGRUNS boot(s)"
echo "   DUAL-SCHEDULE:       $DSCHEDEVENTS event(s) across $DSCHEDRUNS boot(s)"
echo "   SYSCALL-STACK MISMATCH: $STKEVENTS event(s) across $STKRUNS boot(s)"
echo "   median post-boot idle: $(awk -F'\t' 'NR>1{print $5}' "$TALLY" | sort -n | awk '{a[NR]=$1} END{if(NR)print a[int((NR+1)/2)]" heartbeats (~"a[int((NR+1)/2)]*2"s guest)"}')"
echo

if [ "$GUARD_PROVEN" -lt "$RUNS" ]; then
  echo "!! WARNING: the guard did not self-certify on every boot. A clean loop"
  echo "!! proves nothing on the boots where it did not."
fi

if [ "$HITS" = "0" ]; then
  echo "RESULT: GREEN -- $RUNS boots, no misbehaviour, nothing captured."
  echo "        If this stays green the flake likely needs WORKLOAD before the"
  echo "        idle (both archived fires followed a gate run), so rerun as:"
  echo "        WORKLOAD_FLAGS='-DLXPOSIX_BOOT' bash logs/flakeloop.sh $RUNS $IDLE"
  exit 0
fi

echo "RESULT: RED -- $HITS hit(s). Logs + the matching binary are in $OUT."
echo "        Read the [kpf] line's rip, NOT the register block's."
echo "        Resolve it against $OUT/tobyos.bin, NOT the tree's copy."
exit 1
