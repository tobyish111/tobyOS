"""split2.py -- hunks.py between two SNAPSHOTS of a file rather than HEAD and
the working copy: a batch's work saved as a series of files (one before each
change) becomes a series of commits, each interval split by item.

    split2.py list BASE TARGET               index the hunks of BASE -> TARGET
    split2.py apply BASE TARGET MAP OUT ITEM...
                                             write OUT = BASE + the hunks whose
                                             MAP entry is one of ITEMs

MAP is hunks.py's: "INDEX ITEM", or for a pure addition
"INDEX ITEM1 ITEM2@REGEX ..." to split its added lines between items. Every
hunk must be mapped. Bytes are carried as latin-1.
"""
import re
import subprocess
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0] if '/' in __file__ else '.')
import hunks as H   # noqa: E402

ENC = 'latin-1'


def diff_hunks(base, target):
    d = subprocess.run(['git', 'diff', '--no-index', '--text', '-U0', '--no-color',
                        base, target], capture_output=True).stdout.decode(ENC)
    out = []
    cur = None
    for line in d.split('\n'):
        m = H.HDR.match(line)
        if m:
            cur = {'os': int(m.group(1)), 'oc': int(m.group(2) or 1),
                   'ns': int(m.group(3)), 'nc': int(m.group(4) or 1),
                   'old': [], 'new': []}
            out.append(cur)
            continue
        if cur is None:
            continue
        if line.startswith('-') and not line.startswith('---'):
            cur['old'].append(line[1:])
        elif line.startswith('+') and not line.startswith('+++'):
            cur['new'].append(line[1:])
    return out


def main():
    cmd = sys.argv[1]
    base, target = sys.argv[2], sys.argv[3]
    hs = diff_hunks(base, target)
    if cmd == 'list':
        for i, h in enumerate(hs):
            print('%3d  -%d,%d +%d,%d  | -%s | +%s' % (
                i, h['os'], h['oc'], h['ns'], h['nc'],
                H.snippet(h['old']), H.snippet(h['new'])))
        return
    if cmd == 'dump':
        for i, h in enumerate(hs):
            print('##### %d  -%d,%d +%d,%d' % (i, h['os'], h['oc'], h['ns'], h['nc']))
            for l in h['old']:
                print('-' + l)
            for l in h['new']:
                print('+' + l)
        return
    if cmd == 'apply':
        mp = H.parse_map(sys.argv[4])
        out = sys.argv[5]
        want = set(sys.argv[6:])
        missing = [i for i in range(len(hs)) if i not in mp]
        if missing:
            sys.exit('unmapped hunks: %s' % missing)
        old = H.read(base).split('\n')
        res = []
        pos = 0
        for i, h in enumerate(hs):
            start = h['os'] - 1 if h['oc'] > 0 else h['os']
            res.extend(old[pos:start])
            pos = start
            lines = H.take(h, mp[i], want)
            if lines is not None:
                res.extend(lines)
                if h['oc'] > 0:
                    pos += h['oc']
        res.extend(old[pos:])
        H.write(out, '\n'.join(res))
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
