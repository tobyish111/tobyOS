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

# --- the sixteen that were busybox, and the new checksums ---------------
echo @@su_seq
seq 3; seq 2 4; seq -s, 1 3
echo @@su_tac
printf 'one\ntwo\nthree\n' > tf; tac tf
echo @@su_rmdir
mkdir -p /tmp/rd/a; rmdir /tmp/rd/a; echo "gone: $([ -d /tmp/rd/a ] && echo no || echo yes)"
rmdir /tmp/rd/nope 2>&1 | head -1; echo rc=$?
echo @@su_whoami
whoami
echo @@su_uname
uname; uname -s; uname -m; uname -r
echo @@su_which
which sed; which definitely_no_such_tool; echo rc=$?
echo @@su_free
free | head -2 | tail -1 | cut -c1-4
echo @@su_sync
sync; echo rc=$?
echo @@su_stat
echo hello > /tmp/statme
stat -c '%s %F' /tmp/statme
echo @@su_du
mkdir -p /tmp/dut; echo aaaa > /tmp/dut/f1; du -s /tmp/dut | cut -f2
echo @@su_xargs
printf 'a\nb\nc\n' | xargs echo
echo @@su_timeout
timeout 2 echo quick; echo rc=$?
echo @@su_yes
yes hi | head -2
echo @@su_dd
dd if=/tmp/statme of=/tmp/ddout bs=1 count=5 2>/dev/null
printf "%s\n" "$(cat /tmp/ddout)"
echo @@su_md5
printf "%s\n" "$(printf 'abc' | md5sum | cut -d' ' -f1)"
echo @@su_sha1
printf "%s\n" "$(printf 'abc' | sha1sum | cut -d' ' -f1)"
echo @@su_sha256
printf "%s\n" "$(printf 'abc' | sha256sum | cut -d' ' -f1)"
echo @@su_base64
printf "%s\n" "$(printf 'hello' | base64)"
printf "%s\n" "$(printf 'aGVsbG8=' | base64 -d)"

# --- ls, dmesg, vi ------------------------------------------------------
mkdir -p /tmp/lst
echo aa > /tmp/lst/alpha
echo bbbb > /tmp/lst/beta
mkdir -p /tmp/lst/sub
# Piped through cat on purpose: the guest console IS a terminal, so a bare
# `ls` there correctly prints COLUMNS. The one-per-line form is what a
# script sees, and it is the form worth pinning.
echo @@ls_plain
ls /tmp/lst | cat
echo @@ls_all
# A real dotfile: this VFS's readdir does not return "." or ".." at all,
# so -a is about hidden FILES here, not the two special entries.
echo x > /tmp/lst/.hidden
ls /tmp/lst | cat | grep -c hidden
ls -a /tmp/lst | cat | grep -c hidden
echo @@ls_long
ls -l /tmp/lst | cat | head -1
echo @@ls_long_mode
ls -l /tmp/lst | cat | grep alpha | cut -c1-10
echo @@ls_size_col
ls -l /tmp/lst | cat | grep beta | tr -s ' ' | cut -d' ' -f4
echo @@ls_classify
ls -F /tmp/lst | cat
echo @@ls_reverse
ls -r /tmp/lst | cat
echo @@ls_onefile
ls /tmp/lst/alpha | cat
echo @@ls_missing
ls /tmp/no_such_entry > /tmp/lserr 2>&1; echo rc=$?; head -1 /tmp/lserr
echo @@dmesg
dmesg -t | head -2 | cut -c1-5
echo @@dmesg_any
dmesg | wc -l
echo @@vi_exists
vi --version 2>&1 | head -1; which vi

# --- tar ----------------------------------------------------------------
rm -rf /tmp/tsrc /tmp/tout; mkdir -p /tmp/tsrc/sub
echo alpha > /tmp/tsrc/a.txt
printf 'deep\ncontent\n' > /tmp/tsrc/sub/c.txt
cd /tmp
echo @@tar_create
tar -cf t.tar tsrc; echo rc=$?
echo @@tar_list
tar -tf t.tar | sort
echo @@tar_extract
mkdir -p /tmp/tout && cd /tmp/tout && tar -xf ../t.tar && cat tsrc/sub/c.txt
cd /tmp
echo @@tar_gzip
tar -czf t.tgz tsrc; tar -tf t.tgz | sort | tail -1
echo @@tar_bzip2
tar -cjf t.tbz tsrc; tar -tf t.tbz | sort | tail -1
echo @@tar_roundtrip_z
rm -rf /tmp/tz && mkdir -p /tmp/tz && cd /tmp/tz && tar -xzf ../t.tgz && cat tsrc/a.txt
cd /tmp
echo @@tar_member
rm -rf /tmp/tm && mkdir -p /tmp/tm && cd /tmp/tm && tar -xf ../t.tar tsrc/a.txt && ls tsrc | cat
cd /tmp
echo @@tar_corrupt
tar -tf /etc/motd > /dev/null 2>&1; echo rc=$?

# --- the second batch that was busybox ----------------------------------
echo @@b2_printenv
FOO=bar printenv FOO 2>/dev/null || (export FOO=bar; printenv FOO)
echo @@b2_printenv_all
printenv | grep -c '='
echo @@b2_cksum
printf "%s\n" "$(printf 'abc' | cksum)"
echo @@b2_truncate
truncate -s 7 /tmp/trunc.bin; echo "size: $(wc -c < /tmp/trunc.bin)"
echo abcdefghij > /tmp/trunc2.bin; truncate -s 4 /tmp/trunc2.bin
echo "shrunk: $(wc -c < /tmp/trunc2.bin)"
echo @@b2_install
echo payload > /tmp/isrc; rm -rf /tmp/idst; mkdir -p /tmp/idst
install -m 644 /tmp/isrc /tmp/idst/; cat /tmp/idst/isrc
echo @@b2_install_d
rm -rf /tmp/idir; install -d /tmp/idir/deep; echo "made: $([ -d /tmp/idir/deep ] && echo yes || echo no)"
echo @@b2_pgrep
sleep 40 &
sleep 1
pgrep sleep | head -1 | grep -c '^[0-9]'
pkill -TERM sleep; echo "pkill rc=$?"
echo @@b2_tty
tty | cat
echo @@b2_stty
stty | head -1 | cut -d' ' -f1
echo @@b2_nice
nice echo ran 2>/dev/null
echo @@b2_mkfifo
mkfifo /tmp/fifo1 2>&1; echo rc=$?

# --- the third batch that was busybox -----------------------------------
printf 'a\nb\nd\n' > /tmp/c1
printf 'b\nc\nd\n' > /tmp/c2
echo @@b3_comm
comm /tmp/c1 /tmp/c2 | tr '	' ':'
echo @@b3_comm12
comm -12 /tmp/c1 /tmp/c2
echo @@b3_expand
printf 'a	b\n' > /tmp/tabf; expand -t4 /tmp/tabf | cat -A 2>/dev/null || expand -t4 /tmp/tabf | tr ' ' '.'
echo @@b3_fold
echo abcdefghij | fold -w 4
echo @@b3_nl
printf 'x\n\ny\n' | nl | tr -s ' '
echo @@b3_paste
paste /tmp/c1 /tmp/c2 | tr '	' ':'
echo @@b3_rev
echo abcde | rev
echo @@b3_split
printf '1\n2\n3\n4\n' > /tmp/sp; cd /tmp && split -l 2 /tmp/sp part && cat partaa && echo -- && cat partab
cd /tmp
echo @@b3_shuf
printf 'a\nb\nc\n' | shuf | sort | tr '\n' ','
echo @@b3_od
printf 'AB' | od | head -1 | tr -s ' '
echo @@b3_mktemp
T=$(mktemp /tmp/mt.XXXXXX); echo "created: $([ -f "$T" ] && echo yes || echo no)"
echo @@b3_groups
groups | head -1 | wc -w
echo @@b3_hostname
hostname
echo @@b3_hostname_set
hostname newname 2>&1; echo rc=$?
echo @@b3_readlink
ln -sf /tmp/c1 /tmp/lnk3; readlink /tmp/lnk3
echo @@b3_killall
sleep 40 &
sleep 1
killall sleep; echo "rc=$?"
echo @@b3_mount
mount | head -1 | wc -w
echo @@b3_umount
umount /tmp 2>&1; echo rc=$?

# --- zstd ---------------------------------------------------------------
echo @@zst_gnu
zstdcat /etc/gnumade.zst > /tmp/zg.out
echo "lines: $(wc -l < /tmp/zg.out)"
echo "bytes: $(wc -c < /tmp/zg.out)"
sha256sum /tmp/zg.out | cut -d' ' -f1
echo @@zst_test_ok
zstd -t /etc/gnumade.zst; echo rc=$?
echo @@zst_badsum
zstd -t /etc/gnubadzst.zst 2>&1; echo rc=$?
echo @@zst_notzst
unzstd -c /etc/motd 2>&1 | head -1; echo done
echo @@zst_roundtrip
cp /tmp/zg.out /tmp/zr.txt
zstd -q -k /tmp/zr.txt 2>/dev/null; echo "made: $([ -f /tmp/zr.txt.zst ] && echo yes || echo no)"
unzstd -c /tmp/zr.txt.zst > /tmp/zr.back 2>/dev/null
echo "same: $([ "$(wc -c < /tmp/zr.txt)" = "$(wc -c < /tmp/zr.back)" ] && echo yes || echo no)"
echo @@zst_pipe
printf 'round trip me\n' | zstd -q 2>/dev/null | unzstd

# --- zip / unzip --------------------------------------------------------
echo @@zip_list
unzip -l /etc/gnumade.zip | tr -s ' '
echo @@zip_extract
cd /tmp && rm -rf zx && mkdir zx && cd zx
unzip -q /etc/gnumade.zip
echo "bytes: $(wc -c < gnumade.txt)"
echo "lines: $(wc -l < gnumade.txt)"
sha256sum gnumade.txt | cut -d' ' -f1
cd /tmp
echo @@zip_test_ok
unzip -t /etc/gnumade.zip | tail -1
unzip -t /etc/gnumade.zip > /dev/null 2>&1; echo rc=$?
echo @@zip_badcrc
unzip -t /etc/gnubadzip.zip 2>&1 | grep -i crc | head -1
unzip -t /etc/gnubadzip.zip > /dev/null 2>&1; echo rc=$?
echo @@zip_pipe
unzip -p /etc/gnumade.zip | head -1
echo @@zip_roundtrip
cd /tmp && rm -rf zr && mkdir zr && cd zr
cp /tmp/zx/gnumade.txt big.txt
printf 'small\n' > small.txt
zip -q /tmp/ours.zip big.txt small.txt
echo "made: $([ -f /tmp/ours.zip ] && echo yes || echo no)"
echo "smaller: $([ "$(wc -c < /tmp/ours.zip)" -lt "$(wc -c < big.txt)" ] && echo yes || echo no)"
cd /tmp && rm -rf zb && mkdir zb && cd zb
unzip -q /tmp/ours.zip
cmp /tmp/zr/big.txt big.txt && echo "big: same" || echo "big: DIFFER"
cmp /tmp/zr/small.txt small.txt && echo "small: same" || echo "small: DIFFER"
cd /tmp
echo @@zip_store
cd /tmp/zr && zip -q -0 /tmp/st.zip small.txt && cd /tmp
unzip -p /tmp/st.zip small.txt
echo @@zip_member
cd /tmp && rm -rf zm && mkdir zm && cd zm
unzip -q /tmp/ours.zip small.txt
ls | tr '\n' ','
cd /tmp
echo @@zip_notzip
unzip -l /etc/motd 2>&1 | head -1
unzip -l /etc/motd > /dev/null 2>&1; echo rc=$?

echo @@compress_END
