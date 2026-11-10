# devid.py: device descriptors -- fstat against stat (st_dev, st_ino,
# st_rdev) and readlink of /proc/self/fd/N, for each device node opened by
# its own name. On Linux every line says "same" and the link names the node.
import os
for name in ("/dev/null", "/dev/zero", "/dev/full", "/dev/random", "/dev/urandom", "/dev/tty"):
    try:
        fd = os.open(name, os.O_RDWR | os.O_NOCTTY)
    except OSError as e:
        print("%-13s open: %s" % (name, e.strerror))
        continue
    a = os.fstat(fd)
    b = os.stat(name)
    ident = (a.st_dev, a.st_ino) == (b.st_dev, b.st_ino)
    rdev = a.st_rdev == b.st_rdev
    try:
        link = os.readlink("/proc/self/fd/%d" % fd)
    except OSError as e:
        link = "readlink: " + e.strerror
    print("%-13s ident %s rdev %s (%d,%d / %d,%d) link %s" % (
        name, "same" if ident else "DIFF", "same" if rdev else "DIFF",
        os.major(a.st_rdev), os.minor(a.st_rdev), os.major(b.st_rdev), os.minor(b.st_rdev), link))
    os.close(fd)
