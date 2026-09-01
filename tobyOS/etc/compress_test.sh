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

# --- sdiff, diff3, more -------------------------------------------------
printf 'a\nb\nc\nd\n' > x1
printf 'a\nB\nc\nd\n' > x2
printf 'a\nb\nc\nD\n' > x3
echo @@sdiff_changed
sdiff -w 40 x1 x2 | tr '\t' ' ' | sed -E 's/[ ]+/ /g; s/ $//'
echo @@sdiff_added
printf 'a\nb\n' > y1; printf 'a\nb\nc\n' > y2
sdiff -w 40 y1 y2 | tr '\t' ' ' | sed -E 's/[ ]+/ /g; s/ $//'
echo @@diff3_middle
diff3 x1 x2 x3; echo rc=$?
echo @@diff3_conflict
printf 'a\nX\nc\n' > k1; printf 'a\nb\nc\n' > k2; printf 'a\nY\nc\n' > k3
diff3 k1 k2 k3
echo @@more_piped
more x1
echo @@more_plusN
more +3 x1

# --- patch ---------------------------------------------------------------
i=1; : > pb; while [ $i -le 12 ]; do echo $i >> pb; i=$((i+1)); done
sed '5s/^/CHANGED/' pb > pm
diff -u pb pm > p.patch
echo @@patch_unified
cp pb pt; patch pt < p.patch >/dev/null 2>&1; echo rc=$?; sed -n '5p' pt
echo @@patch_reverse
patch -R pt < p.patch >/dev/null 2>&1; echo rc=$?; sed -n '5p' pt
echo @@patch_offset
{ echo x; echo x; cat pb; } > pd
patch pd < p.patch 2>&1 | grep -o 'offset [0-9-]* line[s]*'; sed -n '7p' pd
echo @@patch_reject
printf 'nothing\nlike\nit\n' > pr
rm -f pr.rej
patch pr < p.patch >/dev/null 2>&1; echo rc=$?
echo "rej exists: $([ -f pr.rej ] && echo yes || echo no)"
echo "unchanged: $(head -1 pr)"
echo @@patch_dryrun
cp pb pdry; patch --dry-run pdry < p.patch >/dev/null 2>&1
echo "still original: $(sed -n '5p' pdry)"

# --- locate / updatedb ---------------------------------------------------
mkdir -p /tmp/lctest/deep
echo hi > /tmp/lctest/findme.txt
echo hi > /tmp/lctest/deep/buried.log
echo @@locate_build
updatedb -U /tmp/lctest -o /tmp/lc.db -v
echo @@locate_find
locate -d /tmp/lc.db findme
echo @@locate_deep
locate -d /tmp/lc.db buried
echo @@locate_glob
locate -d /tmp/lc.db "*.log"
echo @@locate_count
locate -d /tmp/lc.db -c hi_nothing_matches_this; locate -d /tmp/lc.db -c findme
echo @@locate_basename
locate -d /tmp/lc.db -b findme.txt
echo @@locate_nodb
locate -d /tmp/nosuch.db findme 2>&1; echo rc=$?

# --- rm -f and ln --------------------------------------------------------
echo @@rm_force_missing
rm -f /tmp/definitely_not_there 2>&1; echo rc=$?
echo @@rm_force_real
echo x > /tmp/rmme; rm -f /tmp/rmme; echo "gone: $([ -f /tmp/rmme ] && echo no || echo yes)"; echo rc=$?
echo @@rm_no_force
rm /tmp/definitely_not_there > /tmp/rmerr 2>&1; echo rc=$?; head -1 /tmp/rmerr
echo @@ln_hard
echo content > /tmp/lnsrc
rm -f /tmp/lnhard; ln /tmp/lnsrc /tmp/lnhard 2>&1; echo rc=$?
echo @@ln_sym
# `ln -s` is checked by READLINK, not by reading through the link. On tmpfs
# a symlink goes into the VFS's in-RAM table, and vfs_resolve_path() -- the
# only thing that consults that table -- has NO CALLERS, so open() never
# follows it. That is a VFS gap, not something ln can fix; what ln is
# responsible for is creating the link the right way round.
rm -f /tmp/lnsym; ln -s /tmp/lnsrc /tmp/lnsym; echo rc=$?
echo "readlink says: $(readlink /tmp/lnsym 2>&1)"
echo @@ln_force
ln -sf /tmp/lnsrc /tmp/lnsym; echo rc=$?
echo @@ln_basename
cd /tmp; rm -f lnsrc2; echo two > /tmp/dir_target 2>/dev/null; ln -s /tmp/lnsrc; echo rc=$?; cat /tmp/lnsrc

echo @@compress_END
