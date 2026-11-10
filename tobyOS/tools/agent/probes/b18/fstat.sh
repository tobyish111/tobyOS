# fstat.sh: fstat(fd) against stat(name) -- device and inode -- for files
# on tmpfs, tobyfs, the initrd, /dev/shm, /proc and /dev, a hard link, a
# file renamed while open, a directory (finding 106). On Linux every line
# says "dev same ino same".
exec 2>&1
d=/tmp/fst.$$; mkdir -p $d
D=/data/fst.$$; mkdir -p $D 2>/dev/null || { D=$d/data; mkdir -p $D; }
echo x > $d/f; ln $d/f $d/g; echo y > $D/f; echo z > /dev/shm/fst.$$
echo r > $d/r
PYTHONHOME=${PYTHONHOME-/} PYTHONPATH=${PYTHONPATH-/lib/python310.zip} python3 - "$d" "$D" "$$" <<'EOF'
import os, sys
d, D, pid = sys.argv[1], sys.argv[2], sys.argv[3]
def cmp(tag, a, b):
    print("%-14s dev %s ino %s" % (tag, "same" if a.st_dev == b.st_dev else "DIFF",
                                  "same" if a.st_ino == b.st_ino else "DIFF"))
def one(tag, p, flags=os.O_RDONLY):
    fd = os.open(p, flags)
    cmp(tag, os.fstat(fd), os.stat(p))
    os.close(fd)
one("tmpfs", d + "/f")
one("tobyfs", D + "/f")
one("initrd", "/etc/passwd")
one("shm", "/dev/shm/fst." + pid)
one("proc", "/proc/self/status")
one("devnull", "/dev/null", os.O_WRONLY)
one("tmpfs-dir", d, os.O_RDONLY | os.O_DIRECTORY)
one("tobyfs-dir", D, os.O_RDONLY | os.O_DIRECTORY)
fd = os.open(d + "/g", os.O_RDONLY); cmp("link", os.fstat(fd), os.stat(d + "/f")); os.close(fd)
fd = os.open(d + "/r", os.O_RDONLY); os.rename(d + "/r", d + "/r2")
cmp("renamed", os.fstat(fd), os.stat(d + "/r2")); os.close(fd)
a = os.open(d + "/f", os.O_RDONLY); b = os.open(d + "/f", os.O_RDONLY)
cmp("two-opens", os.fstat(a), os.fstat(b)); os.close(a); os.close(b)
fd = os.open(d + "/f", os.O_RDONLY)
cmp("fd-magic", os.fstat(fd), os.stat("/dev/fd/%d" % fd)); os.close(fd)
fd = os.open(d + "/r2", os.O_RDONLY); os.rename(d + "/r2", d + "/r3")
cmp("magic-renamed", os.fstat(fd), os.stat("/dev/fd/%d" % fd)); os.close(fd)
EOF
rm -rf $d $D /dev/shm/fst.$$
echo end
