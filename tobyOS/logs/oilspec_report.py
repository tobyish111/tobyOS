#!/usr/bin/env python3
"""oilspec_report.py -- turn the guest's MAP bitmap into a per-feature census.

The gate prints 2,776 results as a bitmap because the serial console cannot
afford a line each. This joins that bitmap back against

  * initrd/etc/oilspec/manifest.json  -- which spec file and case name each id
    came from, so a failure can be named after a FEATURE rather than a number;
  * logs/oilspec_host.json            -- the host oracle's POSIX / BASH-ONLY
    split, so the POSIX-compliance number is over the cases where real bash and
    real dash actually agree, and bash-only divergence cannot inflate it.

Usage: python logs/oilspec_report.py [logs/oilspec.log]
"""
import json, os, re, sys, collections

HERE = os.path.dirname(os.path.abspath(__file__))
LOG  = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'oilspec.log')

MAP_RE = re.compile(r'\[oilspec\] MAP (\d{4}) ([POXDETS?]+)\s*$')

CODE = {'P': 'pass', 'O': 'stdout-diff', 'X': 'exit-diff', 'D': 'both-diff',
        'T': 'timeout', 'E': 'broken', 'S': 'skipped', '?': 'never-ran'}


def load_map(path):
    """Decode the bitmap. Each MAP line carries the id of its FIRST case, so a
    dropped or interleaved line is detectable rather than silently shifting
    every later result onto the wrong case -- the serial wire does interleave."""
    results, seen_start = {}, []
    with open(path, 'rb') as f:
        text = f.read().decode('utf-8', 'replace')
    for line in text.split('\n'):
        m = MAP_RE.search(line.strip())
        if not m:
            continue
        start, chunk = m.group(1), m.group(2)
        seen_start.append(start)
        base = int(start)
        for i, ch in enumerate(chunk):
            results['%04d' % (base + i)] = ch
    return results, seen_start


def main():
    if not os.path.exists(LOG):
        print('no log at %s' % LOG); return 1
    results, starts = load_map(LOG)
    if not results:
        print('no MAP lines in %s -- the gate did not get that far' % LOG); return 1

    man = {m['id']: m for m in json.load(
        open(os.path.join(HERE, '..', 'initrd', 'etc', 'oilspec', 'manifest.json'),
             encoding='utf-8'))}
    host = {r['id']: r for r in json.load(
        open(os.path.join(HERE, 'oilspec_host.json'), encoding='utf-8'))}

    # Score the two subsets separately. Mixing them would let bash-only
    # divergence inflate -- or depress -- a number reported as "POSIX".
    tally = collections.defaultdict(lambda: collections.Counter())
    per_file = collections.defaultdict(lambda: collections.Counter())
    failures = collections.defaultdict(list)

    for cid, ch in sorted(results.items()):
        cls = host.get(cid, {}).get('class', 'UNKNOWN')
        if ch == 'S':
            continue
        tally[cls][ch] += 1
        stem = man.get(cid, {}).get('file', '?')
        per_file[stem][ch] += 1
        if ch not in ('P', 'S'):
            failures[stem].append((cid, ch, cls, man.get(cid, {}).get('name', '?')))

    print('--- score by class (host oracle: does real dash agree with real bash?) ---')
    for cls in ('POSIX', 'BASH-ONLY', 'UNKNOWN'):
        c = tally.get(cls)
        if not c:
            continue
        total = sum(c.values())
        p = c['P']
        pct = (100.0 * p / total) if total else 0.0
        label = {'POSIX':     'POSIX      (bash==dash: the compliance number)',
                 'BASH-ONLY': 'BASH-ONLY  (superset contract, not a POSIX claim)',
                 'UNKNOWN':   'UNKNOWN    (not classified by the host oracle)'}[cls]
        print('  %-52s %4d/%4d  %5.1f%%' % (label, p, total, pct))
        detail = ' '.join('%s=%d' % (CODE[k], v)
                          for k, v in sorted(c.items()) if k != 'P')
        if detail:
            print('  %-52s %s' % ('', detail))

    print()
    print('--- failing spec files, worst first (a file == a feature area) ---')
    ranked = sorted(per_file.items(),
                    key=lambda kv: -(sum(kv[1].values()) - kv[1]['P']))
    for stem, c in ranked[:28]:
        bad = sum(c.values()) - c['P']
        if not bad:
            continue
        posix_bad = sum(1 for f in failures[stem] if f[2] == 'POSIX')
        print('  %-28s %3d failing / %3d  (%d POSIX)'
              % (stem, bad, sum(c.values()), posix_bad))

    out = os.path.join(HERE, 'oilspec_failures.txt')
    with open(out, 'w', newline='\n', encoding='utf-8') as f:
        f.write('# id  code  class  file  case name\n')
        for stem, lst in sorted(failures.items()):
            for cid, ch, cls, name in lst:
                f.write('%s  %-11s  %-9s  %-24s  %s\n'
                        % (cid, CODE[ch], cls, stem, name))
    print()
    print('wrote %s (%d failing cases)'
          % (out, sum(len(v) for v in failures.values())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
