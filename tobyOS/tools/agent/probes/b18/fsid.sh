# fsid.sh: an open file is the file its name is (finding 106) -- fstat's
# device and inode against stat's, seen through `[ /dev/fd/N -ef NAME ]`
# and busybox stat, on tmpfs, tobyfs, the initrd and /proc; a hard link, a
# file renamed while open, a directory. On Linux every line says same.
exec 2>&1
same() { if [ "$1" -ef "$2" ]; then echo "$3 same"; else echo "$3 DIFFERENT"; fi; }
d=/tmp/fsid.$$; mkdir -p $d
D=/data/fsid.$$; mkdir -p $D 2>/dev/null || D=$d/data
echo x > $d/f; exec 3< $d/f; same /dev/fd/3 $d/f tmpfs-file
echo x > $D/f; exec 4< $D/f; same /dev/fd/4 $D/f data-file
exec 5< /etc/passwd; same /dev/fd/5 /etc/passwd initrd-file
ln $d/f $d/g; exec 6< $d/g; same /dev/fd/6 $d/f tmpfs-link
echo y > $d/r; exec 7< $d/r; mv $d/r $d/r2; same /dev/fd/7 $d/r2 tmpfs-renamed
exec 8< $d; same /dev/fd/8 $d tmpfs-dir
echo z > /dev/shm/fsid.$$; exec 9< /dev/shm/fsid.$$; same /dev/fd/9 /dev/shm/fsid.$$ shm-file
same /dev/fd/3 $d/g tmpfs-fd-vs-link
same /dev/fd/3 /dev/fd/6 fd-vs-fd
if command -v busybox >/dev/null 2>&1; then
  for n in 3 4 5 6; do
    a=$(busybox stat -L -c '%d:%i' /dev/fd/$n 2>&1)
    case $n in 3) p=$d/f;; 4) p=$D/f;; 5) p=/etc/passwd;; 6) p=$d/f;; esac
    b=$(busybox stat -c '%d:%i' $p 2>&1)
    [ "$a" = "$b" ] && echo "bb$n same" || echo "bb$n DIFFERENT"
  done
fi
exec 3<&- 4<&- 5<&- 6<&- 7<&- 8<&- 9<&-
rm -rf $d $D /dev/shm/fsid.$$
echo end
