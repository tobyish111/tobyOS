# AN OPEN FILE IS THE FILE ITS NAME IS: `[ /dev/fd/N -ef NAME ]` holds for
# a file on tmpfs, tobyfs and the initrd, through a hard link, after a
# rename, for a directory and for /dev/shm. fstat answered from the
# descriptor's own copy (an inode hashed from the name it was opened by),
# so the renamed file differed in both shells and the initrd file in tsh
# (finding 106; logs/b18 fsid; the kernel side is lxposix's linux-fdid).
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
same /dev/fd/3 $d/r2 different-files
same /dev/fd/5 $d/f different-mounts
exec 3<&- 4<&- 5<&- 6<&- 7<&- 8<&- 9<&-
rm -rf $d $D /dev/shm/fsid.$$
echo end
