#!/usr/bin/env bash
# textgate.sh -- sed, awk and grep, each diffed against the real GNU tool.
#
#   bash logs/textgate.sh            # all three
#   bash logs/textgate.sh awk grep   # a subset (the boot still runs all)
#
# The same three scripts -- /etc/sed_test.sh, /etc/awk_test.sh and
# /etc/grep_test.sh -- are run twice: by /bin/tsh inside the guest, where
# the tools are ours, and by bash on this host, where they are GNU sed 4.9,
# GNU awk 5.x and GNU grep 3.x. The transcripts are compared section by
# section. Nothing here is an expectation written from memory; the oracle
# is the real tool, and the section list is derived from the fixture so a
# new case is graded the moment it is added.
#
# This supersedes logs/sedgate.sh (which still works, and still covers sed
# alone) now that the same boot carries all three fixtures. It is also the
# regression gate for libtoby's REGEX ENGINE, which all three drive: the
# 2026-09-06 fragment-exit rewrite fixed a miscompile of every group whose
# last element was quantified -- `\([0-9.]*\)` and `(a*)` alike -- and the
# rx_ sections of the sed fixture are the cases that proved it.
#
# For a change to the ENGINE ONLY, note that libtoby's regex.c, sed, awk and
# grep all build against host gcc unchanged, so the same fixtures can be run
# on the host in seconds before paying for a boot.

set -u
cd "$(dirname "$0")/.."

WANT="${*:-sed awk grep}"
LOG=logs/textgate.log
rm -f "$LOG"

# FORCE A KERNEL REBUILD. EXTRA_CFLAGS is not a file, so make cannot see
# that it changed: src/kernel.o built by the PREVIOUS gate -- with that
# gate's -D flags -- is newer than kernel.c and gets reused, and the
# harness then simply never runs.
rm -f src/kernel.o
PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH" \
  make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
       EXTRA_CFLAGS=-DCOREUTILS_SELFTEST iso >logs/.buildlog.txt 2>&1
BUILD_RC=$?
if [ $BUILD_RC -ne 0 ]; then
    echo "ABORT: the build FAILED -- the ISO on disk is stale, so any"
    echo "       verdict below would have described code that is not in it."
    tail -30 logs/.buildlog.txt
    exit 1
fi
[ -f tobyOS.iso ] || { echo "ABORT: build produced no ISO"; exit 1; }

"/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
  -boot d -smp 4 -m 5120 -cpu max \
  -serial file:"$LOG" -no-reboot -display none 2>/dev/null &

for i in $(seq 1 450); do
    grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null && break
    sleep 2
done
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
if ! grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: the harness never reported"
    tail -20 "$LOG"
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

# /^@@[a-z_]/, not /^@@/ -- see logs/compress.sh: a unified diff hunk header
# would otherwise close a section on its own first line.
section() { echo "$1" | awk -v k="@@$2" '$0==k{f=1;next} /^@@[a-z_]/{f=0} f'; }

TOTAL_PASS=0; TOTAL_FAIL=0

grade() {                    # grade <tool> <fixture> <first-section> <end-marker>
    local tool="$1" fix="$2" first="$3" end="$4"
    case " $WANT " in *" $tool "*) ;; *) return 0 ;; esac

    local guest host H
    guest=$(sed -n "/@@$first/,/@@$end/p" "$LOG" | kern_strip | tr -d '\r')
    if ! echo "$guest" | grep -q "@@$end"; then
        echo "=== $tool: ABORT, the guest transcript is incomplete ==="
        echo "$guest" | tail -5
        TOTAL_FAIL=$((TOTAL_FAIL+1))
        return 1
    fi

    H="$(pwd)/logs/.${tool}host"
    rm -rf "$H"; mkdir -p "$H"
    # The same marker-splitting the guest transcript gets: a case whose
    # output does not end in a newline (ORS=";" say) otherwise leaves the
    # next @@ marker glued to it on ONE side only, and two identical
    # transcripts are then graded as different.
    host=$(MSYS_NO_PATHCONV=1 WORKDIR="$H" bash "$fix" 2>&1 | tr -d '\r' \
           | sed -E 's/(.)(@@[a-z_])/\1\n\2/g')
    rm -rf "$H"

    local pass=0 fail=0 s a b
    echo "=== $tool, against the real GNU tool ==="
    for s in $(grep -o '@@[a-zA-Z_0-9]*' "$fix" | sed 's/@@//' | grep -v "^$end\$"); do
        a=$(section "$host"  "$s")
        b=$(section "$guest" "$s")
        if [ "$a" = "$b" ]; then pass=$((pass+1))
        else
            fail=$((fail+1))
            echo "  FAIL $s"
            echo "         gnu : $(echo "$a" | tr '\n' '|')"
            echo "         toby: $(echo "$b" | tr '\n' '|')"
        fi
    done
    echo "  $tool: $pass ok, $fail failed"
    echo
    TOTAL_PASS=$((TOTAL_PASS+pass))
    TOTAL_FAIL=$((TOTAL_FAIL+fail))
}

grade sed  etc/sed_test.sh  s_basic sed_END
grade awk  etc/awk_test.sh  aw_nf   awk_END
grade grep etc/grep_test.sh gr_m1   grep_END

echo "TEXTGATE: $TOTAL_PASS ok, $TOTAL_FAIL failed"
grep -ac "PANIC\|#PF" "$LOG" >/dev/null 2>&1
echo "panics=$(grep -ac 'PANIC' "$LOG" 2>/dev/null || echo 0)"
[ "$TOTAL_FAIL" -eq 0 ]
