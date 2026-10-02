"""oilsummary.py RESULTS [MANIFEST] -- condense an oilcases.sh report: per
case its id, spec file and title, the exit statuses, and a unified diff of
stdout (bash -> tsh) and of stderr, each cut to a few lines. For reading
hundreds of failures at once and grouping them by cause."""
import difflib
import sys

res = sys.argv[1]
man = sys.argv[2] if len(sys.argv) > 2 else 'initrd/etc/oilspec/manifest.tsv'
titles = {}
for line in open(man, encoding='utf-8', errors='replace'):
    p = line.rstrip('\n').split('\t')
    if len(p) >= 3:
        titles[p[0]] = (p[1], p[2])

cases = []
cur = None
sec = None
for line in open(res, encoding='utf-8', errors='replace'):
    line = line.rstrip('\n')
    if line.startswith('######## '):
        f = line.split()
        cur = {'id': f[1], 'v': f[2], 'hdr': line, 'bo': [], 'to': [], 'be': [], 'te': []}
        cases.append(cur)
        sec = None
        continue
    if cur is None:
        continue
    if line == '--- bash stdout':
        sec = 'bo'; continue
    if line == '--- tsh stdout':
        sec = 'to'; continue
    if line == '--- bash stderr':
        sec = 'be'; continue
    if line == '--- tsh stderr':
        sec = 'te'; continue
    if sec:
        cur[sec].append(line)


def short(a, b, n):
    d = [l for l in difflib.unified_diff(a, b, lineterm='', n=0)
         if not l.startswith(('---', '+++', '@@'))]
    out = d[:n]
    if len(d) > n:
        out.append('  ... (%d more)' % (len(d) - n))
    return out


for c in cases:
    spec, title = titles.get(c['id'], ('?', '?'))
    ex = c['hdr'].split('(', 1)[1].rstrip(')') if '(' in c['hdr'] else ''
    print('=== %s %s | %s | %s' % (c['id'], spec, title[:70], ex))
    for l in short(c['bo'], c['to'], 8):
        print('  out ' + l[:150])
    norm = lambda xs: [x.replace('/etc/oilspec/%s.sh' % c['id'], 'S') for x in xs]
    for l in short(norm(c['be']), norm(c['te']), 4):
        print('  err ' + l[:150])
