# bzip2 / the z* and bz* wrappers / diff / cmp -- run by /bin/tsh in the guest.
#
# Two fixtures ship in the image and carry the weight here:
#
#   etc/gnumade.bz2   made by REAL bzip2 -9. It uses the multi-table,
#                     multi-selector encoding our own encoder never emits,
#                     so it is the only thing that exercises that half of
#                     the decoder.
#   etc/gnubadbz.bz2  the same file with one bit flipped in the middle of
#                     the compressed payload. It still parses; only the
#                     per-block CRC catches it. If -t ever passes this, the
#                     integrity check has stopped running.

cd "${WORKDIR:-/tmp}"

printf 'hello bzip2\n' > plain.txt
i=0
: > big.txt
while [ $i -lt 700 ]; do
  echo "row $i the quick brown fox jumps over the lazy dog padding" >> big.txt
  i=$((i + 1))
done
: > empty.txt
printf 'alpha\nbeta\ngamma\n'  > d1.txt
printf 'alpha\nBETA\ngamma\n'  > d2.txt

echo @@bz_pipe
printf 'round trip me\n' | bzip2 | bunzip2

echo @@bz_file
cp plain.txt f1.txt
bzip2 f1.txt
echo "bz2 exists: $([ -f f1.txt.bz2 ] && echo yes || echo no)"
echo "orig gone: $([ -f f1.txt ] && echo no || echo yes)"
bunzip2 f1.txt.bz2
echo "restored: $(cat f1.txt)"

echo @@bz_keep
cp plain.txt f2.txt
bzip2 -k f2.txt
echo "orig kept: $([ -f f2.txt ] && echo yes || echo no)"

echo @@bz_cat
bzip2 -c plain.txt > z1.bz2
bzcat z1.bz2

echo @@bz_big
bzip2 -c big.txt > big.bz2
bunzip2 -c big.bz2 > big.out
echo "same size: $([ "$(wc -c < big.txt)" = "$(wc -c < big.out)" ] && echo yes || echo no)"
echo "first: $(head -1 big.out)"
echo "last: $(tail -1 big.out)"

echo @@bz_empty
bzip2 -c empty.txt > e.bz2
echo "empty bytes back: $(bunzip2 -c e.bz2 | wc -c)"

echo @@bz_levels
bzip2 -1 -c big.txt | bunzip2 | wc -l
bzip2 -9 -c big.txt | bunzip2 | wc -l

echo @@bz_test_ok
bzip2 -t z1.bz2; echo rc=$?

echo @@bz_gnu
bzcat /etc/gnumade.bz2 > gnu.out
echo "lines: $(wc -l < gnu.out)"
echo "bytes: $(wc -c < gnu.out)"
sha256sum gnu.out | cut -d' ' -f1

echo @@bz_gnu_test
bzip2 -t /etc/gnumade.bz2; echo rc=$?

echo @@bz_badcrc
bzip2 -t /etc/gnubadbz.bz2 2>&1; echo rc=$?

echo @@bz_notbz
bunzip2 -c plain.txt 2>&1; echo rc=$?

# --- the wrappers -------------------------------------------------------
echo @@z_grep_gz
gzip -c big.txt > w.gz
zgrep "row 42 " w.gz

echo @@z_grep_bz
bzip2 -c big.txt > w.bz2
zgrep "row 42 " w.bz2

echo @@z_grep_bzname
bzgrep "row 43 " w.bz2

echo @@z_grep_count
zgrep -c "the quick" w.gz

echo @@z_grep_plain
zgrep "row 44 " big.txt

echo @@z_diff
gzip -c d1.txt > dd1.gz
gzip -c d2.txt > dd2.gz
zdiff dd1.gz dd2.gz; echo rc=$?

echo @@z_cmp_same
gzip -c d1.txt > dd1b.gz
zcmp dd1.gz dd1b.gz; echo rc=$?

echo @@z_lesspipe
lesspipe dd1.gz

echo @@z_lesspipe_bz
bzip2 -c d1.txt > dd1.bz2
lesspipe dd1.bz2

# --- diff and cmp themselves --------------------------------------------
echo @@diff_normal
diff d1.txt d2.txt; echo rc=$?

echo @@diff_same
diff d1.txt d1.txt; echo rc=$?

echo @@diff_unified
diff -u d1.txt d2.txt | tail -n +3

echo @@diff_brief
diff -q d1.txt d2.txt; echo rc=$?

echo @@diff_ignorecase
diff -i d1.txt d2.txt; echo rc=$?

echo @@cmp_same
cmp d1.txt d1.txt; echo rc=$?

echo @@cmp_differ
cmp d1.txt d2.txt; echo rc=$?

echo @@compress_END
