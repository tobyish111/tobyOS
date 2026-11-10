"""hunksplit.py -- cumulative per-item states of one file, from a FIXED base.

hunks.py applies a map to the CURRENT HEAD, which moves with every commit;
splitting one file into several commits then means re-listing and
re-mapping after each. This computes every state once, before the first
commit: state.K = base + the hunks of ITEM1..ITEMK. Compile-check each
(PRECHECK every split state offline), then copy state.K over FILE and
commit, K = 1..n. MAP is hunks.py's format.

    hunksplit.py prep FILE MAP OUTDIR ITEM1 ITEM2 ...
        base = HEAD's FILE now; hunks = git diff -U0 FILE now; writes
        OUTDIR/state.K for K = 1..n: base + the hunks of ITEM1..ITEMK,
        and checks that the last state equals the working copy.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hunks as H

def build(base_lines, hs, mp, want):
    res = []
    pos = 0
    for i, h in enumerate(hs):
        start = h['os'] - 1 if h['oc'] > 0 else h['os']
        res.extend(base_lines[pos:start])
        pos = start
        lines = H.take(h, mp[i], want)
        if lines is not None:
            res.extend(lines)
            if h['oc'] > 0:
                pos += h['oc']
    res.extend(base_lines[pos:])
    return '\n'.join(res)

def main():
    cmd, path, mapf, outdir = sys.argv[1:5]
    items = sys.argv[5:]
    mp = H.parse_map(mapf)
    hs = H.hunks(path)
    missing = [i for i in range(len(hs)) if i not in mp]
    if missing:
        sys.exit('unmapped hunks: %s' % missing)
    used = set()
    for v in mp.values():
        for s in v:
            used.add(s.split('@', 1)[0])
    if used - set(items):
        sys.exit('items in the map not listed: %s' % (used - set(items)))
    base = H.head_text(path)
    base_lines = base.split('\n')
    os.makedirs(outdir, exist_ok=True)
    open(os.path.join(outdir, 'base'), 'w', encoding=H.ENC, newline='').write(base)
    for k in range(1, len(items) + 1):
        t = build(base_lines, hs, mp, set(items[:k]))
        open(os.path.join(outdir, 'state.%d' % k), 'w', encoding=H.ENC,
             newline='').write(t)
        print('state.%d  %s  %d bytes' % (k, '+'.join(items[:k]), len(t)))
    full = H.read(path).replace('\r\n', '\n')
    last = t.replace('\r\n', '\n')
    print('last state == working copy:', full == last)

main()
