#!/usr/bin/env python3
"""oilspec_detail.py -- the per-case diffs a whole-corpus run printed, indexed.

A full run emits `[oilspec] FAIL NNNN ...` followed by the first few differing
lines, for every POSIX-class failure. That is the single densest diagnostic in
this arc -- and it arrives buried in a 5 MB serial log interleaved with kernel
process traces, in corpus order, with no indication of which feature area a
case belongs to.

This pulls those blocks out and reprints them grouped by spec file, next to the
case text and the host oracle's expectation, so a feature area can be read as
one thing instead of as nine case numbers scattered through a log.

    python logs/oilspec_detail.py                  # every POSIX failure, by file
    python logs/oilspec_detail.py redirect here-doc
    python logs/oilspec_detail.py --ids 1765 1766
"""
import json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORPUS = os.path.join(ROOT, 'initrd', 'etc', 'oilspec')
LOG = os.path.join(HERE, 'oilspec.log')

host = {r['id']: r for r in json.load(open(os.path.join(HERE, 'oilspec_host.json')))}
man = {r['id']: r for r in json.load(open(os.path.join(CORPUS, 'manifest.json')))}

# The serial log carries kernel lines interleaved with the gate's, so a block
# is "a FAIL line plus the indented lines that follow it", not "the next N
# lines". Anything not starting with [oilspec] simply is not part of it.
blocks = {}
cur = None
fail_re = re.compile(r'^\[oilspec\] FAIL (\d+)\s+(.*)$')
cont_re = re.compile(r'^\[oilspec\]     (.*)$')
with open(LOG, encoding='utf-8', errors='replace') as f:
    for line in f:
        line = line.rstrip('\n')
        m = fail_re.match(line)
        if m:
            cur = m.group(1)
            blocks[cur] = [m.group(2)]
            continue
        m = cont_re.match(line)
        if m and cur:
            blocks[cur].append(m.group(1))
            continue
        if line.startswith('[oilspec]'):
            cur = None

args = [a for a in sys.argv[1:]]
if args and args[0] == '--ids':
    ids = args[1:]
else:
    want = set(args)
    ids = []
    for line in open(os.path.join(HERE, 'oilspec_failures.txt'),
                     encoding='utf-8', errors='replace'):
        if line.startswith('#'):
            continue
        f = line.split()
        if len(f) >= 4 and f[2] == 'POSIX' and (not want or f[3] in want):
            ids.append(f[0])
    ids.sort(key=lambda i: (man.get(i, {}).get('file', ''), i))

print('# %d cases; %d have diff detail in %s' %
      (len(ids), sum(1 for i in ids if i in blocks), LOG))
last_file = None
for cid in ids:
    m = man.get(cid, {})
    if m.get('file') != last_file:
        last_file = m.get('file')
        print('\n########## %s ##########' % last_file)
    print('\n==== %s  %s' % (cid, m.get('name', '?')))
    try:
        body = open(os.path.join(CORPUS, cid + '.sh'), encoding='utf-8',
                    errors='replace').read()
    except OSError:
        body = '<missing>'
    for ln in body.rstrip('\n').split('\n'):
        print('  | ' + ln)
    h = host.get(cid, {})
    print('  -- host bash: status=%s stdout=%r' % (h.get('bash_status'),
                                                   h.get('bash_stdout')))
    for ln in blocks.get(cid, ['<no detail in log>']):
        print('  >> ' + ln)
