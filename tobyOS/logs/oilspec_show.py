#!/usr/bin/env python3
"""oilspec_show.py -- print a case, its host-oracle expectation, and its class.

The corpus is 2,776 numbered files and the report names failures by id, so
every diagnostic round starts with "what IS case 1765". Doing that with three
separate cat/grep/python one-liners is how a case gets read against another
case's expectation. One command, one id, all three facts.

    python logs/oilspec_show.py 1765 1766 ...
    python logs/oilspec_show.py --file if_        (every failing case in a file)
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORPUS = os.path.join(ROOT, 'initrd', 'etc', 'oilspec')

host = {r['id']: r for r in json.load(open(os.path.join(HERE, 'oilspec_host.json')))}
man = {r['id']: r for r in json.load(open(os.path.join(CORPUS, 'manifest.json')))}


def failing_ids():
    path = os.path.join(HERE, 'oilspec_failures.txt')
    out = []
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('#'):
            continue
        f = line.split()
        if len(f) >= 4:
            out.append((f[0], f[2], f[3]))
    return out


def show(cid):
    m = man.get(cid, {})
    h = host.get(cid, {})
    print('==== %s  %s / %s  [%s]' % (cid, m.get('file', '?'), m.get('name', '?'),
                                      h.get('class', '?')))
    try:
        with open(os.path.join(CORPUS, cid + '.sh'), encoding='utf-8',
                  errors='replace') as f:
            body = f.read()
    except OSError:
        body = '<missing>'
    for ln in body.rstrip('\n').split('\n'):
        print('  | ' + ln)
    print('  -- bash status=%s stdout=%r' % (h.get('bash_status'),
                                             h.get('bash_stdout')))


args = sys.argv[1:]
if args and args[0] == '--file':
    want = args[1]
    cls = args[2] if len(args) > 2 else 'POSIX'
    ids = [i for (i, c, f) in failing_ids() if f == want and (cls == '*' or c == cls)]
    print('# %d failing cases in %s (%s)' % (len(ids), want, cls))
else:
    ids = args
for cid in ids:
    show(cid)
