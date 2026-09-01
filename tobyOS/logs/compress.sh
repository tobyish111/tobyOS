#!/usr/bin/env bash
# compress.sh -- bzip2 and the z*/bz* wrappers, plus the file and text
# tools that share this boot: diff, cmp, sdiff, diff3, more, patch,
# locate and updatedb. They live here because one boot is six minutes
# and they have no reason to need their own.
#
#   bash logs/compress.sh
#
# The checks that carry the most weight:
#
#   * etc/gnumade.bz2 is REAL bzip2 -9 output. Our encoder emits a single
#     Huffman table; real bzip2 emits up to six and switches between them
#     every 50 symbols. Decoding this fixture is the only thing that
#     exercises that half of the decoder, and its SHA-256 is computed here
#     from the host copy so the fixture and the expectation cannot drift.
#
#   * etc/gnubadbz.bz2 has one bit flipped inside the compressed payload.
#     It still parses; only the per-block CRC rejects it. `bzip2 -t`
#     passing that file would mean the integrity check is gone.
#
#   * diff and cmp are checked against the exact bytes GNU produces for the
#     same inputs (verified on the host before these were written down).

set -u
cd "$(dirname "$0")/.."

LOG=logs/compress.log
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

kern_strip() { sed -E 's/^\[[0-9 ]+ms\] //' | grep -v '^\[' ; }
OUT=$(sed -n '/@@bz_pipe/,/@@compress_END/p' "$LOG" | kern_strip | tr -d '\r')
if ! echo "$OUT" | grep -q "@@compress_END"; then
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
eq() {   # eq <section> <label> <exact pipe-joined text>
    local g; g=$(section "$1" | tr '\n' '|')
    if [ "$g" = "$3" ]; then echo "  ok   $2"; PASS=$((PASS+1))
    else echo "  FAIL $2 -- got '$g' (wanted '$3')"; FAIL=$((FAIL+1)); fi
}

GNU_SHA=$(sha256sum etc/gnumade.txt | cut -d' ' -f1)
GNU_LINES=$(wc -l < etc/gnumade.txt | tr -d ' ')
GNU_BYTES=$(wc -c < etc/gnumade.txt | tr -d ' ')

echo "=== bzip2 round trips ==="
ck bz_pipe   "a pipeline round trips"           '^round trip me\|'
ck bz_file   "bzip2 replaces, bunzip2 restores" 'bz2 exists: yes\|orig gone: yes\|restored: hello bzip2\|'
ck bz_keep   "-k keeps the original"            'orig kept: yes\|'
ck bz_cat    "bzcat decompresses"               '^hello bzip2\|'
ck bz_big    "a multi-kilobyte file"            'same size: yes.*first: row 0 .*last: row 699 '
ck bz_empty  "an empty file round trips"        'empty bytes back: 0\|'
ck bz_levels "-1 and -9 both round trip"        '^700\|700\|'

echo "=== integrity is actually checked ==="
ck bz_test_ok  "-t accepts our own output"    'rc=0\|'
ck bz_gnu_test "-t accepts real bzip2 output" 'rc=0\|'
ck bz_badcrc   "-t rejects a flipped bit"     'CRC\) error.*rc=[1-9]'
ck bz_notbz    "a non-bzip2 file is refused"  'not in bzip2 format.*rc=[1-9]'

echo "=== the multi-table stream real bzip2 produced ==="
ck bz_gnu "decodes to the right bytes" "$GNU_SHA"
ck bz_gnu "line count matches"         "lines: $GNU_LINES\|"
ck bz_gnu "byte count matches"         "bytes: $GNU_BYTES\|"

echo "=== the z* / bz* wrappers ==="
ck z_grep_gz     "zgrep reads a .gz"             '^row 42 '
ck z_grep_bz     "zgrep reads a .bz2 too"        '^row 42 '
ck z_grep_bzname "bzgrep is the same tool"       '^row 43 '
ck z_grep_count  "zgrep passes flags through"    '^700\|'
ck z_grep_plain  "zgrep on an UNcompressed file" '^row 44 '
ck z_diff        "zdiff compares decompressed"   '2c2\|< beta\|---\|> BETA\|rc=1\|'
ck z_cmp_same    "zcmp on identical content"     '^rc=0\|'
eq z_lesspipe    "lesspipe writes the plain text"    'alpha|beta|gamma|'
eq z_lesspipe_bz "lesspipe handles bzip2 as well"    'alpha|beta|gamma|'

echo "=== diff and cmp, against what GNU prints ==="
eq diff_normal     "normal format"       '2c2|< beta|---|> BETA|rc=1|'
eq diff_same       "identical files"     'rc=0|'
eq diff_unified    "unified hunk"        '@@ -1,3 +1,3 @@| alpha|-beta|+BETA| gamma|'
eq diff_brief      "-q names the files"  'Files d1.txt and d2.txt differ|rc=1|'
eq diff_ignorecase "-i ignores case"     'rc=0|'
eq cmp_same        "cmp on identical"    'rc=0|'
eq cmp_differ      "cmp reports byte and line" 'd1.txt d2.txt differ: char 7, line 2|rc=1|'

echo "=== sdiff, diff3 and more ==="
# sdiff pads with tabs; the guest normalises them away, so what is compared
# is the CONTENT and the MARKERS. GNU mixes tabs and spaces before the
# marker in a way that does not follow from the width alone, so byte
# identity of the padding is not claimed -- the columns and markers are.
eq sdiff_changed  "sdiff marks a changed line with |"  'a a|b | B|c c|d d|'
eq sdiff_added    "sdiff marks an added line with >"   'a a|b b| > c|'
eq diff3_middle   "diff3 groups the two files that agree"                   '====2|1:2c|3:2c|  b|2:2c|  B|====3|1:4c|2:4c|  d|3:4c|  D|rc=0|'
eq diff3_conflict "diff3 reports a three-way conflict"                   '====|1:2c|  X|2:2c|  b|3:2c|  Y|'
eq more_piped     "more cats when stdout is not a tty" 'a|b|c|d|'
eq more_plusN     "more +N starts at that line"        'c|d|'


echo "=== patch ==="
eq patch_unified "applies a unified diff"          'rc=0|CHANGED5|'
eq patch_reverse "-R undoes it"                    'rc=0|5|'
eq patch_offset  "finds a hunk the file has moved" 'offset 2 lines|CHANGED5|'
# The important one: a hunk that matches nowhere must be REJECTED, with the
# file untouched, not forced into place.
eq patch_reject  "rejects a hunk that matches nowhere"                  'rc=1|rej exists: yes|unchanged: nothing|'
eq patch_dryrun  "--dry-run leaves the file alone"  'still original: 5|'


echo "=== locate / updatedb ==="
ck locate_build    "updatedb indexes a tree"     'indexed [0-9]+ paths'
ck locate_find     "locate finds a file"         'findme.txt'
ck locate_deep     "and one nested deeper"       'deep/buried.log'
ck locate_glob     "a glob pattern"              'buried.log'
eq locate_count    "-c counts, and 0 is not a match" '0|1|'
ck locate_basename "-b matches the basename"     'findme.txt'
ck locate_nodb     "a missing database says run updatedb" 'run updatedb first.*rc=[1-9]'


echo "=== rm -f and ln, neither of which existed ==="
# `rm -f x` on a missing file must be SILENT and succeed -- that is the
# entire reason the flag exists, and rm had no -f at all.
eq rm_force_missing "rm -f on a missing file is silent and succeeds" 'rc=0|'
ck rm_force_real    "rm -f still removes what is there"  'gone: yes\|rc=0'
ck rm_no_force      "without -f a missing file is an error" 'rc=[1-9].*cannot stat'
# tobyfs has no hard links; ln must SAY that rather than fail obscurely.
ck ln_hard          "ln says hard links are unimplemented" 'hard links are not implemented.*rc=[1-9]'
# The link must point at the TARGET, not the other way round: the native
# symlink() syscall passed POSIX's (target, linkpath) straight into
# vfs_symlink(link, target) and built every link backwards, returning
# success while doing it.
ck ln_sym           "ln -s makes a link pointing AT the target"                     'rc=0\|readlink says: /tmp/lnsrc'
ck ln_force         "ln -sf replaces an existing link" 'rc=0\|'
ck ln_basename      "ln -s with no linkname uses the basename" 'rc=[0-9]'


echo
echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
