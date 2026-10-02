"""hunks.py -- split one file's working-tree diff into per-item commits.

    hunks.py list FILE                 index the hunks of `git diff -U0 FILE`
    hunks.py apply FILE MAP ITEM...    write FILE = HEAD + the hunks whose
                                       MAP entry is one of ITEMs (stages
                                       nothing; the working copy is saved
                                       to FILE.full first)
    hunks.py restore FILE              put FILE.full back

MAP is a text file, one line per hunk: "INDEX ITEM". Every hunk must be
mapped. A pure ADDITION may be split between items:

    11 C1 G@^/\\* bash's report_error C1@^/\\* A subscript's VALUE

gives the added lines to C1 until the first line matching the regex after
`@`, then to G until the next one, then to C1. Hunks are applied in order
to HEAD's text by line number; -U0 hunks never overlap. Bytes are carried
as latin-1, so a file git calls binary goes through unchanged.
"""
import re
import subprocess
import sys

HDR = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')
ENC = 'latin-1'


def hunks(path):
    d = subprocess.run(['git', 'diff', '--text', '-U0', '--no-color', '--', path],
                       capture_output=True, check=True).stdout.decode(ENC)
    out = []
    cur = None
    for line in d.split('\n'):
        m = HDR.match(line)
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


def head_text(path):
    rel = subprocess.run(['git', 'ls-files', '--full-name', path],
                         capture_output=True, check=True).stdout.decode().strip()
    return subprocess.run(['git', 'show', 'HEAD:' + rel], capture_output=True,
                          check=True).stdout.decode(ENC)


def snippet(lines):
    for l in lines:
        t = l.strip()
        if t and t not in ('{', '}', '*/', '/*', '*'):
            return t[:90]
    return ''


def read(path):
    return open(path, encoding=ENC, newline='').read()


def write(path, text):
    open(path, 'w', encoding=ENC, newline='').write(text)


def parse_map(fname):
    mp = {}
    for line in open(fname, encoding='utf-8'):
        if line.lstrip().startswith('#') or not line.strip():
            continue
        parts = line.split()
        mp[int(parts[0])] = parts[1:]
    return mp


def take(h, spec, want):
    """The lines hunk h contributes for the wanted items, or None for the
    old lines unchanged."""
    if len(spec) == 1:
        return h['new'] if spec[0] in want else None
    if h['oc'] != 0:
        sys.exit('only a pure addition can be split: %s' % spec)
    segs = [(spec[0], None)]
    for s in spec[1:]:
        item, rx = s.split('@', 1)
        segs.append((item, re.compile(rx)))
    res = []
    k = 0
    item = segs[0][0]
    for line in h['new']:
        if k + 1 < len(segs) and segs[k + 1][1].search(line):
            k += 1
            item = segs[k][0]
        if item in want:
            res.append(line)
    if k + 1 != len(segs):
        sys.exit('split marker not found: %s' % segs[k + 1][1].pattern)
    return res


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == 'list':
        for i, h in enumerate(hunks(path)):
            print('%3d  -%d,%d +%d,%d  | -%s | +%s' % (
                i, h['os'], h['oc'], h['ns'], h['nc'],
                snippet(h['old']), snippet(h['new'])))
        return
    if cmd == 'restore':
        write(path, read(path + '.full'))
        return
    if cmd == 'apply':
        mp = parse_map(sys.argv[3])
        want = set(sys.argv[4:])
        hs = hunks(path)
        missing = [i for i in range(len(hs)) if i not in mp]
        if missing:
            sys.exit('unmapped hunks: %s' % missing)
        write(path + '.full', read(path))
        old = head_text(path).split('\n')
        res = []
        pos = 0                       # next line of `old` (0-based) to copy
        for i, h in enumerate(hs):
            start = h['os'] - 1 if h['oc'] > 0 else h['os']
            res.extend(old[pos:start])
            pos = start
            lines = take(h, mp[i], want)
            if lines is not None:
                res.extend(lines)
                if h['oc'] > 0:
                    pos += h['oc']
        res.extend(old[pos:])
        write(path, '\n'.join(res))
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
