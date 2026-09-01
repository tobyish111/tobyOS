#!/usr/bin/env bash
# sedgate.sh -- sed, diffed against real GNU sed.
#
#   bash logs/sedgate.sh
#
# The same script, /etc/sed_test.sh, is run twice: by /bin/tsh inside the
# guest, where `sed` is ours, and by bash on this host, where `sed` is GNU
# sed. The two transcripts are compared section by section. Nothing here is
# an expectation written from memory -- the oracle is the real tool.
#
# This also guards the REGEX ENGINE. sed is its third consumer, and each one
# has found bugs in it: grep found three, sed found that `{n,m}` was being
# treated as `*` (so `x{3}` matched any number of x -- wrong output, not an
# error) and that `\n` matched the letter n. Those cases are in here.

set -u
cd "$(dirname "$0")/.."

LOG=logs/sedgate.log
rm -f "$LOG"

PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH" \
  make "CC=TMP='C:\t' TEMP='C:\t' clang" "HOST_CC=TMP='C:\t' TEMP='C:\t' gcc" \
       EXTRA_CFLAGS=-DCOREUTILS_SELFTEST iso >logs/.buildlog.txt 2>&1
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

for i in $(seq 1 220); do
    grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null && break
    sleep 2
done
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
if ! grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: the harness never reported"
    exit 1
fi

H="$(pwd)/logs/.sedhost"
rm -rf "$H"; mkdir -p "$H"
HOST_OUT=$(MSYS_NO_PATHCONV=1 WORKDIR="$H" bash etc/sed_test.sh 2>&1)
rm -rf "$H"

# A kernel diagnostic can interleave MID-LINE, not only at the start of
# one, so a filter that drops lines beginning with '[' leaves it behind and
# a perfectly correct tool looks broken. Cut an embedded diagnostic off
# where it starts, and swallow the blank line its own newline leaves.
kern_strip() {
    sed -E 's/^\[[0-9 ]+ms\] //' | grep -v '^\[' | awk '
        { if (match($0, /\[[0-9]+ ms\] \[[a-z_0-9]+\]/)) {
              print substr($0, 1, RSTART - 1); pending = 1; next }
          if (pending && $0 == "") { pending = 0; next }
          pending = 0; print }'
}
GUEST_OUT=$(sed -n '/@@s_basic/,/@@sed_END/p' "$LOG" | kern_strip | tr -d '\r')

if ! echo "$GUEST_OUT" | grep -q "@@sed_END"; then
    echo "ABORT: the guest transcript is incomplete"
    echo "$GUEST_OUT" | tail -5
    exit 1
fi

# /^@@[a-z_]/, not /^@@/ -- see logs/compress.sh: a unified diff hunk header
# would otherwise close a section on its own first line.
section() { echo "$1" | awk -v k="@@$2" '$0==k{f=1;next} /^@@[a-z_]/{f=0} f'; }

PASS=0; FAIL=0
cmp_case() {
    local name="$1" label="$2" b g
    b=$(section "$HOST_OUT" "$name")
    g=$(section "$GUEST_OUT" "$name")
    if [ "$b" = "$g" ]; then echo "  ok   $label"; PASS=$((PASS+1))
    else
        echo "  FAIL $label"
        echo "         gnu : $(echo "$b" | tr '\n' '|')"
        echo "         toby: $(echo "$g" | tr '\n' '|')"
        FAIL=$((FAIL+1))
    fi
}

echo "=== s/// ==="
for c in s_basic:"s/a/X/" s_global:"s///g" s_nth:"s///2" s_amp:"& in the replacement" \
         s_group:"ERE groups and \\1" s_bre_group:"BRE groups" s_upper:"\\U case conversion" \
         s_ucfirst:"\\u one character" s_icase:"the I flag" s_newline:"\\n in the replacement" \
         s_altdelim:"an alternate delimiter" s_anchors:"^ anchor" s_endanchor:"\$ anchor"; do
    cmp_case "${c%%:*}" "${c#*:}"
done

echo "=== the regex engine sed drives ==="
cmp_case re_star      "star"
cmp_case re_plus      "ERE plus"
cmp_case re_alt       "ERE alternation"
cmp_case re_interval  "ERE {3} is EXACTLY three"
cmp_case re_int_bre   "BRE \\{3\\} is exactly three"
cmp_case re_int_range "{2,3}"
cmp_case re_int_open  "{2,}"
cmp_case re_backref   "a backreference"
cmp_case re_tab       "\\t matches a tab, not the letter"

echo "=== addresses ==="
for c in a_line:"a line number" a_range:"a range" a_regex:"/re/" a_regexrange:"/re/,/re/" \
         a_last:"\$" a_negate:"! negation" a_step:"first~step" a_plus:"addr,+N" a_zero:"0,/re/"; do
    cmp_case "${c%%:*}" "${c#*:}"
done

echo "=== commands ==="
for c in c_delete:d c_delrange:"d over a range" c_equals:= c_y:y c_append:a c_insert:i \
         c_change:c c_quit:q c_list:l; do
    cmp_case "${c%%:*}" "${c#*:}"
done

echo "=== hold space ==="
cmp_case h_hold     "h and G"
cmp_case h_exchange "x"
cmp_case h_tac      "reverse a file"
cmp_case h_double   "G double-spaces"

echo "=== blocks, branches, multi-line ==="
cmp_case b_block   "{ }"
cmp_case b_nested  "nested { }"
cmp_case b_branch  "b to a label"
cmp_case b_tloop   "t loops until no substitution"
cmp_case b_njoin   "N joins two lines"
cmp_case b_dloop   "D restarts without reading"
cmp_case b_pfirst  "P prints the first line only"
cmp_case b_slurp   "slurp the whole file and join"

echo "=== options and files ==="
cmp_case o_multi_e     "-e twice"
cmp_case o_semicolon   "semicolon-separated"
cmp_case o_scriptfile  "-f a script file"
cmp_case o_wfile       "w writes a file"
cmp_case o_rfile       "r reads a file in"
cmp_case o_inplace     "-i edits in place"
cmp_case o_inplacebak  "-i.bak keeps a backup"
cmp_case o_exitcode    "q with an exit code"
cmp_case o_nonl        "a missing final newline is preserved"

echo
echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
