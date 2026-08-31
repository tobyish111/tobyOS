# util-linux transcript for logs/utillinux.sh.
cd /tmp
printf 'a bb ccc\ndddd e ff\n' > tbl.txt
printf 'one\ntwo\nthree\n' > lst.txt
printf 'abcdefghij\n' > wide.txt
echo @@column_t
column -t tbl.txt
echo @@colrm
colrm 3 5 < wide.txt
echo @@col
printf 'aX\bY\n' | col
echo @@rename
touch ren_aaa.txt; rename aaa bbb ren_aaa.txt; ls ren_bbb.txt
echo @@whereis
whereis ls
echo @@namei
namei /etc/passwd
echo @@namei_missing
namei /etc/nosuchfile 2>&1; echo rc=$?
echo @@uuidgen
uuidgen
echo @@uuidgen_shape
# Checked without awk -F-: the guest's awk takes the program text as a
# filename when the separator is glued to the flag. cut/wc need no parsing.
uuidgen > /tmp/u.txt
wc -c < /tmp/u.txt
cut -c9,14,19,24 /tmp/u.txt
cut -c15 /tmp/u.txt
echo @@lsblk
lsblk | head -3
echo @@findmnt
findmnt | head -3
echo @@findmnt_one
findmnt /data | tail -1
echo @@lsns
lsns | head -2
echo @@wdctl
wdctl | head -3
echo @@prlimit
prlimit 2>&1; echo rc=$?
echo @@lslocks
lslocks 2>&1; echo rc=$?
echo @@lsipc
lsipc 2>&1; echo rc=$?
echo @@zramctl
zramctl 2>&1; echo rc=$?
echo @@END
