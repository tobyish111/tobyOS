# symdev.sh: opening a device node through a symlink -- write, append,
# create without truncate (noclobber's open), read; and the same by name.
exec 2>&1
d=/tmp/sd.$$; mkdir -p $d
ln -s /dev/null $d/ln; ln -s /dev/zero $d/lz; ln -s $d/ln $d/ln2
N() { sed "s#/tmp/sd\.[0-9]*#/tmp/sd.N#g; s/^[^:]*: line [0-9]*: //"; }
{ echo a > $d/ln; } 2>&1 | N; echo "write st=${PIPESTATUS[0]}"
{ echo a >> $d/ln; } 2>&1 | N; echo "append st=${PIPESTATUS[0]}"
{ echo a > $d/ln2; } 2>&1 | N; echo "two-links st=${PIPESTATUS[0]}"
{ head -c 4 $d/lz | od -An -tx1; } 2>&1 | N; echo "read-zero st=${PIPESTATUS[0]}"
{ read -r x < $d/ln; } 2>&1 | N; echo "read-null st=${PIPESTATUS[0]}"
PYTHONHOME=${PYTHONHOME-/} PYTHONPATH=${PYTHONPATH-/lib/python310.zip} python3 - "$d" <<'PY'
import os, sys
d = sys.argv[1]
for tag, fl in (("wronly", os.O_WRONLY), ("creat", os.O_WRONLY | os.O_CREAT),
                ("creat-trunc", os.O_WRONLY | os.O_CREAT | os.O_TRUNC),
                ("rdwr", os.O_RDWR), ("rdonly", os.O_RDONLY)):
    for name in ("ln", "ln2"):
        try:
            fd = os.open(d + "/" + name, fl, 0o666)
            a = os.fstat(fd); b = os.stat(d + "/" + name)
            print("%-12s %-4s ok same=%s chr=%s" % (tag, name, (a.st_dev, a.st_ino) == (b.st_dev, b.st_ino), (a.st_mode & 0o170000) == 0o020000))
            os.close(fd)
        except OSError as e:
            print("%-12s %-4s %s" % (tag, name, e.strerror))
PY
rm -rf $d
echo end
