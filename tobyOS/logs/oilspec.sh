#!/bin/bash
# oilspec.sh -- THE THIRD-PARTY SHELL CONFORMANCE GATE for tobyOS.
#
# shparity.sh gates the tobyOS shell against 54 cases we wrote ourselves. That
# corpus can only ever contain bugs we already suspected; after enough rounds
# of add-a-case-then-fix-it it stops being evidence and becomes a description
# of what we happen to do. This gate runs a corpus written by SOMEONE ELSE --
# the Oils spec suite (third_party/oils-spec), ~2,776 cases whose entire
# purpose is to pin down where bash, dash, mksh, ash and zsh disagree. Those
# are exactly the corners a from-scratch shell gets wrong, and none of them
# were chosen here.
#
# Run it:  bash logs/oilspec.sh            (whole corpus, bitmap + census)
#          bash logs/oilspec.sh 0100-0199  (one band, with full per-case diffs)
#
# THE ORACLE IS NOT THE CORPUS'S OWN EXPECTATIONS. The .test.sh files carry
# recorded `## STDOUT:` blocks, but those were recorded years ago against other
# builds on another machine; trusting them would turn every stale expectation
# into a phantom tsh bug we would then "fix". Instead:
#
#   * logs/oilspec_host.py runs every case under the REAL bash and dash on THIS
#     machine and splits them into POSIX (bash and dash agree exactly) and
#     BASH-ONLY (they differ), dropping any case that is nondeterministic even
#     across two runs of bash.
#   * this gate runs each case in the guest under the unmodified GNU bash 5.2
#     tobyOS already ships and under /bin/tsh, and requires identical stdout
#     and identical exit status.
#
# The POSIX-compliance number is the pass rate over the POSIX subset. The
# BASH-ONLY subset measures the superset contract instead. Both are reported;
# neither is allowed to launder the other.
cd /c/CustomOS/tobyOS || exit 1
QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
LOG="logs/oilspec.log"
FILTER="${1:-}"

export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t
MAKE_TMP=("CC=TMP='C:\\t' TEMP='C:\\t' clang" "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc")

# The manifest is the authority on how many cases there SHOULD be, not a glob.
# `ls initrd/etc/oilspec/*.sh` expands to a 75 KB command line here and has been
# observed to come back short (1110 of 2776) without printing an error -- which
# then reads as "the corpus shrank" rather than "the count is unreliable". The
# manifest is one file written by the extractor, so counting it cannot truncate.
CASES=$(grep -c . initrd/etc/oilspec/manifest.tsv 2>/dev/null || echo 0)
ONDISK=$(find initrd/etc/oilspec -name '*.sh' -type f | wc -l)
[ "$CASES" -gt 0 ] || {
    echo "NO CORPUS -- run: python logs/oilspec_extract.py"; exit 1; }
[ "$ONDISK" -eq "$CASES" ] || {
    echo "CORPUS INCONSISTENT: manifest says $CASES, $ONDISK .sh files on disk"
    echo "  re-run: python logs/oilspec_extract.py"; exit 1; }
echo "=== corpus: $CASES cases from third_party/oils-spec ==="

# kernel.c carries the OILSPEC_BOOT hook, so it must be recompiled whenever the
# flag changes -- make cannot see a -D change as a reason to rebuild. Same trap
# shparity.sh documents.
echo "=== build gate ISO ${FILTER:+(filter=$FILTER)} ==="
# The filter travels as a FILE in the corpus, not as a -D string macro. As a
# macro the quotes were eaten somewhere in make -> sh -> clang, the define
# expanded to the integer expression `0001-0200`, and the gate dereferenced
# (char *)1 - 200 and died before printing its banner -- which looks exactly
# like a hang. There is no quoting layer to lose here.
printf '%s' "$FILTER" > initrd/etc/oilspec/FILTER
touch src/kernel.c
make "${MAKE_TMP[@]}" iso EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DOILSPEC_BOOT" \
    >logs/oilspec.build.log 2>&1
BUILD_RC=$?
# THE EXIT STATUS, not just "is there an ISO". A previous run leaves tobyOS.iso
# on disk, so a compile error passed the file test and the gate then measured
# the LAST build's code while reporting it as this one's -- a whole run's worth
# of numbers attributed to a change that was never compiled.
[ $BUILD_RC -eq 0 ] || { echo "BUILD FAILED (rc=$BUILD_RC)";
                         grep -E 'error|Error' logs/oilspec.build.log | head -20;
                         exit 1; }
[ -f tobyOS.iso ] || { echo "ISO BUILD FAIL"; tail -40 logs/oilspec.build.log; exit 1; }

# GATE 0: the flags and the payload must actually be IN the artefacts. A build
# that quietly skipped the recompile, or an initrd missing the corpus, would
# otherwise produce a clean-looking SKIP that reads like "nothing to fix".
echo "=== gate 0: payload really shipped ==="
for f in bin/tsh bin/oilspec etc/oilspec/bin/argv.py etc/oilspec/EXCLUDE etc/oilspec/POSIX; do
    tar -tf build/initrd.tar 2>/dev/null | grep -qx "$f" \
        && echo "  $(printf '%-26s' "$f") PRESENT" \
        || { echo "  $(printf '%-26s' "$f") MISSING -- abort"; exit 1; }
done
# PRESENT is not ENOUGH. The helper binaries once shipped mode 0644 because
# etc/oilspec was added to the tar list that stages data: they were present,
# and every case that called argv.py still failed to run it. 327 cases -- 126
# of them POSIX -- reported a shell divergence that was a permission bit. A
# gate that only checks existence cannot see that, so check the bit.
for f in bin/tsh bin/oilspec etc/oilspec/bin/argv.py etc/oilspec/bin/printenv.py; do
    MODE=$(tar -tvf build/initrd.tar 2>/dev/null | awk -v f="$f" '$NF == f {print $1}')
    case "$MODE" in
        *x*) echo "  $(printf '%-26s' "$f") EXECUTABLE ($MODE)" ;;
        *)   echo "  $(printf '%-26s' "$f") NOT EXECUTABLE ($MODE) -- abort"; exit 1 ;;
    esac
done
INTAR=$(tar -tf build/initrd.tar 2>/dev/null | grep -c '^etc/oilspec/[0-9]*\.sh$')
echo "  corpus in tar:             $INTAR / $CASES"
[ "$INTAR" -eq "$CASES" ] || { echo "  corpus SHORT -- abort"; exit 1; }
# The filter must be the one that was ASKED for. A stale FILTER left in the
# staging tree would silently run a different set of cases than the header says.
STAGED=$(tar -xOf build/initrd.tar etc/oilspec/FILTER 2>/dev/null)
echo "  filter in tar:             '${STAGED}' (asked: '${FILTER}')"
[ "$STAGED" = "$FILTER" ] || { echo "  filter MISMATCH -- abort"; exit 1; }
grep -qa 'OILSPEC' tobyos.bin \
    && echo "  OILSPEC_BOOT compiled into kernel" \
    || { echo "  OILSPEC_BOOT NOT in kernel -- abort"; exit 1; }

echo "=== boot ==="
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1; : > "$LOG"
# -m 4096, not the 512 the other harnesses use: with Chromium staged the initrd
# is ~825 MiB and Limine loads it whole before the kernel starts. At 512 the run
# dies in the BOOTLOADER, before a single line of gate output -- which reads
# like the gate is broken rather than like the box is too small.
"$QEMU" -cdrom tobyOS.iso \
  -drive file=disk.img,format=raw,if=ide,index=0,media=disk,cache=writethrough -boot d \
  -smp 4 -m 4096 -cpu qemu64,+smep,+smap -serial "file:$LOG" -no-reboot -display none &

# Stall detection. A single case CAN hang the guest -- tsh's printf reuse loop
# spun forever on a format with no conversion specifications, and the run simply
# stopped 2,500 cases short with no indication of where or why. Watching the
# serial log stop growing turns that into a named case instead of a timeout.
LAST_SIZE=0; STALL=0; STALL_LIMIT=120
for i in $(seq 1 5400); do
    grep -aq 'OILSPEC\] VERDICT' "$LOG" 2>/dev/null && break
    SIZE=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
    if [ "$SIZE" -eq "$LAST_SIZE" ]; then
        STALL=$((STALL + 1))
        if [ "$STALL" -ge "$STALL_LIMIT" ]; then
            echo "  STALLED: no serial output for ${STALL_LIMIT}s -- the guest is wedged."
            # Each case spawns bash first, so the bash-spawn count IS the
            # 1-based index of the case that hung.
            HUNG=$(grep -ac "created pid=3 ppid=2 'bash'" "$LOG" 2>/dev/null)
            HUNGID=$(printf '%04d' "$HUNG")
            echo "  last case started: $HUNGID  $(grep -a "^$HUNGID" initrd/etc/oilspec/manifest.tsv 2>/dev/null)"
            echo "  --- that case ---"
            sed 's/^/    /' "initrd/etc/oilspec/$HUNGID.sh" 2>/dev/null | head -20
            echo "  --- last serial ---"
            tail -5 "$LOG"
            break
        fi
    else
        STALL=0; LAST_SIZE=$SIZE
    fi
    if [ $((i % 60)) -eq 0 ]; then
        printf '  [%4ds] serial=%s bytes\n' "$i" "$SIZE"
    fi
    sleep 1
done
sleep 2
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1

echo "=== failures (bounded; full data is in the MAP) ==="
grep -a '\[oilspec\] \(FAIL\|BROKEN\|WARNING\)' "$LOG" | head -80

echo "=== verdict ==="
if grep -aq 'OILSPEC\] VERDICT' "$LOG"; then
    grep -a 'OILSPEC\] VERDICT' "$LOG"
else
    echo "NO VERDICT -- the gate never reported. Last 20 serial lines:"
    tail -20 "$LOG"
    exit 1
fi

echo "=== faults (empty=clean) ==="
grep -aiE 'KERNEL PANIC|#GP|#PF|EXCEPTION [0-9]|unhandled syscall' "$LOG" \
    | sort | uniq -c | head -20

# The census: join the guest's per-case bitmap against the host oracle's
# POSIX/BASH-ONLY split and the corpus manifest, so a failure is reported as a
# FEATURE ("word-split: 9 failing") rather than as a case number.
echo "=== census ==="
python logs/oilspec_report.py "$LOG"
echo "(oilspec end)"
