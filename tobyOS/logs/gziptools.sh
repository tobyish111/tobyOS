#!/usr/bin/env bash
# gziptools.sh -- gzip / gunzip / zcat gate.
#
#   bash logs/gziptools.sh
#
# Boots the image, runs /etc/gzip_test.sh under /bin/tsh, and checks the
# transcript. Two of the checks matter more than the rest:
#
#   * the GNU fixture. etc/gnumade.gz is real `gzip -9` output whose first
#     block is DYNAMIC Huffman. Our encoder only ever writes FIXED blocks,
#     so a round trip through our own compressor would never touch that
#     code path. Its SHA-256 is computed here from the host copy rather
#     than hardcoded, so the fixture and the expectation cannot drift.
#
#   * the corrupt fixture. etc/gnubadcrc.gz has one bit flipped in the
#     stored CRC-32 and an otherwise perfectly valid deflate stream. puff
#     does not check the trailer, so this only fails if gzip's own
#     verification is running. It is the difference between "it inflated"
#     and "it inflated to the right bytes".

set -u
cd "$(dirname "$0")/.."

LOG=logs/gziptools.log
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

for i in $(seq 1 200); do
    grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null && break
    sleep 2
done
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
if ! grep -aq "COREUTILS. VERDICT" "$LOG" 2>/dev/null; then
    echo "ABORT: the harness never reported"
    exit 1
fi

kern_strip() { sed -E 's/^\[[0-9 ]+ms\] //' | grep -v '^\[' ; }
OUT=$(sed -n '/@@gz_pipe/,/@@gz_END/p' "$LOG" | kern_strip | tr -d '\r')
if ! echo "$OUT" | grep -q "@@gz_END"; then
    echo "ABORT: the guest transcript is incomplete"
    echo "$OUT" | tail -5
    exit 1
fi

# The marker match is /^@@[a-z_]/, not /^@@/: a UNIFIED DIFF HUNK
# HEADER is literally "@@ -1,3 +1,3 @@", and a looser pattern ended the
# diff section on its own first line and reported the output as empty.
section() { echo "$OUT" | awk -v k="@@$1" '$0==k{f=1;next} /^@@[a-z_]/{f=0} f'; }

PASS=0; FAIL=0
ck() {   # ck <section> <label> <regex>
    local g; g=$(section "$1" | tr '\n' '|')
    if echo "$g" | grep -Eq "$3"; then echo "  ok   $2"; PASS=$((PASS+1))
    else echo "  FAIL $2 -- got '$g' (wanted /$3/)"; FAIL=$((FAIL+1)); fi
}

# The oracle for the GNU fixture, computed here so it cannot go stale.
GNU_SHA=$(sha256sum etc/gnumade.txt | cut -d' ' -f1)
GNU_LINES=$(wc -l < etc/gnumade.txt | tr -d ' ')
GNU_BYTES=$(wc -c < etc/gnumade.txt | tr -d ' ')
GNU_FIRST=$(head -1 etc/gnumade.txt)
GNU_LAST=$(tail -1 etc/gnumade.txt)

echo "=== round trips ==="
ck gz_pipe    "a pipeline round trips"        '^round trip me\|'
ck gz_file    "gzip replaces, gunzip restores" 'gz exists: yes\|orig gone: yes\|restored: hello gzip\|'
ck gz_keep    "-k keeps the original"          'orig kept: yes\|'
ck gz_stdout  "-c writes to stdout"            '^hello gzip\|'
ck gz_zcat    "zcat decompresses"              '^hello gzip\|'
ck gz_levels  "-1 and -9 both round trip"      '^900\|900\|'
ck gz_big     "a file past the 32 KiB window"  'same size: yes.*first: row 0 .*last: row 899 '
ck gz_empty   "an empty file round trips"      'empty bytes back: 0\|'

echo "=== integrity is actually checked ==="
ck gz_test_ok "-t accepts our own output"      'rc=0\|'
ck gz_gnu_test "-t accepts real GNU output"    'rc=0\|'
# The whole point of the corrupt fixture: it MUST be rejected, and for the
# stated reason. A pass here with rc=0 means the trailer check is gone.
ck gz_badcrc  "-t rejects a flipped CRC bit"   'crc error.*rc=[1-9]'
ck gz_notgz   "a non-gzip file is refused"     'not in gzip format.*rc=[1-9]'

echo "=== the dynamic-Huffman fixture GNU produced ==="
ck gz_gnu     "inflates to the right bytes"    "$GNU_SHA"
ck gz_gnu     "line count matches"             "lines: $GNU_LINES\|"
ck gz_gnu     "byte count matches"             "bytes: $GNU_BYTES\|"
ck gz_gnu     "first line matches"             "first: $GNU_FIRST\|"
ck gz_gnu     "last line matches"              "last: $GNU_LAST\|"

echo "=== listing ==="
ck gz_list    "-l prints a full row"           '^4\|'

echo
echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
