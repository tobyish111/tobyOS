# Run on the guest (musl CPython): dump musl's wide-character classes, its
# wcwidth and its case maps for every code point in [lo,hi), as ranges.
import ctypes, sys
libc = ctypes.CDLL(None)
names = ['iswalpha', 'iswdigit', 'iswalnum', 'iswupper', 'iswlower', 'iswspace',
         'iswblank', 'iswprint', 'iswgraph', 'iswcntrl', 'iswpunct', 'iswxdigit']
fns = []
for n in names:
    f = getattr(libc, n)
    f.argtypes = [ctypes.c_uint]
    f.restype = ctypes.c_int
    fns.append(f)
tu = libc.towupper; tu.argtypes = [ctypes.c_uint]; tu.restype = ctypes.c_uint
tl = libc.towlower; tl.argtypes = [ctypes.c_uint]; tl.restype = ctypes.c_uint
ww = libc.wcwidth; ww.argtypes = [ctypes.c_uint]; ww.restype = ctypes.c_int
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
out = open(sys.argv[3], 'w')


def ranges(tag, pred):
    start = None
    for c in range(lo, hi):
        v = pred(c)
        if v and start is None:
            start = c
        elif not v and start is not None:
            out.write('C %s %x %x\n' % (tag, start, c - 1))
            start = None
    if start is not None:
        out.write('C %s %x %x\n' % (tag, start, hi - 1))


for n, f in zip(names, fns):
    ranges(n, lambda c, f=f: f(c) != 0)
for w, tag in ((0, 'iswwcw0'), (2, 'iswwcw2'), (-1, 'iswwcwn')):
    ranges(tag, lambda c, w=w: ww(c) == w)
for tag, f in (('U', tu), ('L', tl)):
    for c in range(lo, hi):
        d = f(c)
        if d != c:
            out.write('%s %x %x\n' % (tag, c, d))
out.write('END %x %x\n' % (lo, hi))
out.close()
