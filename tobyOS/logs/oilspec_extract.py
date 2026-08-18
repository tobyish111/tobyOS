#!/usr/bin/env python3
"""oilspec_extract.py -- turn the third-party Oils spec suite into a tobyOS corpus.

WHY A THIRD-PARTY SUITE
-----------------------
shparity's 54 hand-written cases were written by us, which means they can only
find bugs we already suspected. The Oils spec suite (third_party/oils-spec) is
~4000 cases written by someone else, for a different shell, explicitly to pin
down where bash/dash/mksh/ash/zsh disagree -- exactly the corners a from-scratch
shell gets wrong. Using it costs us the ability to claim the corpus is fair to
us, which is the point.

WHAT THIS DOES
--------------
Each spec file is a sequence of

    #### case name
    <shell code>
    ## STDOUT:
    <expected>
    ## END
    ## status: 0

with per-shell overrides (`## OK dash status: 2`, `## BUG bash stdout: ...`,
`## N-I mksh stdout-json: ""`). We parse that into one .sh file per case plus a
manifest, so both the host oracle and the in-guest runner see the SAME bytes.

WE DO NOT TRUST THE RECORDED EXPECTATIONS AS THE ORACLE. They were recorded
against other shells on another machine years ago; a stale expectation would
show up as a tsh "bug" that is nothing of the sort. The expectations are kept
only for classification. Pass/fail comes from running real bash and real dash
here, now -- see oilspec_host.py.
"""
import os, re, sys, json

SPEC = os.path.join(os.path.dirname(__file__), '..', 'third_party', 'oils-spec', 'spec')
OUT  = os.path.join(os.path.dirname(__file__), '..', 'initrd', 'etc', 'oilspec')

# YSH/Oil is Oils' own new language, not a POSIX shell -- those files measure
# nothing about tsh. `hay` is YSH config blocks. Everything else stays.
SKIP_PREFIX = ('ysh-', 'oil-', 'hay')
SKIP_FILES  = set()

CASE_RE = re.compile(r'^####\s*(.*)$')
# `## key: value`, `## STDOUT:` ... `## END`, and qualified `## OK dash status: 1`
META_RE = re.compile(r'^##\s+(?:(OK|BUG|N-I)\s+([A-Za-z0-9_. /-]+)\s+)?'
                     r'(STDOUT|STDERR|stdout|stderr|stdout-json|stderr-json|status|'
                     r'compare_shells|oils_failures_allowed|our_shell|suite|tags):'
                     r'\s?(.*)$')

def parse_file(path):
    """Yield (name, code, meta) for each #### case in one spec file."""
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.read().split('\n')

    file_meta, cases = {}, []
    cur = None
    block = None          # (key, accumulator) while inside STDOUT:/STDERR:
    for line in lines:
        if block is not None:
            if line.strip() == '## END':
                key, acc = block
                (cur['meta'] if cur else file_meta)[key] = '\n'.join(acc) + ('\n' if acc else '')
                block = None
            else:
                block[1].append(line)
            continue

        m = CASE_RE.match(line)
        if m:
            cur = {'name': m.group(1).strip(), 'code': [], 'meta': {}}
            cases.append(cur)
            continue

        m = META_RE.match(line)
        if m:
            qual, shells, key, val = m.groups()
            full = key if not qual else '%s %s %s' % (qual, shells.strip(), key)
            tgt = cur['meta'] if cur else file_meta
            if key in ('STDOUT', 'STDERR'):
                block = (full, [])
            else:
                tgt[full] = val
            continue

        # A `##` line that is not a directive is a comment; anything else is code.
        if line.startswith('## '):
            continue
        if cur is not None:
            cur['code'].append(line)

    return file_meta, cases

def main():
    files = sorted(f for f in os.listdir(SPEC) if f.endswith('.test.sh'))
    os.makedirs(OUT, exist_ok=True)
    for old in os.listdir(OUT):
        if old.endswith('.sh') or old == 'manifest.tsv':
            os.remove(os.path.join(OUT, old))

    manifest, n, skipped = [], 0, 0
    for fn in files:
        stem = fn[:-len('.test.sh')]
        if stem.startswith(SKIP_PREFIX) or fn in SKIP_FILES:
            skipped += 1
            continue
        file_meta, cases = parse_file(os.path.join(SPEC, fn))
        # `our_shell: ysh` marks a file whose cases are YSH even if the name isn't.
        if file_meta.get('our_shell', '').strip() in ('ysh', 'oil'):
            skipped += 1
            continue
        compare = file_meta.get('compare_shells', '')
        for i, c in enumerate(cases):
            code = '\n'.join(c['code']).strip('\n')
            if not code.strip():
                continue
            n += 1
            cid = '%04d' % n
            with open(os.path.join(OUT, cid + '.sh'), 'w', newline='\n',
                      encoding='utf-8', errors='surrogateescape') as g:
                g.write(code + '\n')
            manifest.append({
                'id': cid, 'file': stem, 'idx': i, 'name': c['name'],
                'compare_shells': compare, 'meta': c['meta'],
            })

    with open(os.path.join(OUT, 'manifest.tsv'), 'w', newline='\n', encoding='utf-8') as g:
        for m in manifest:
            g.write('%s\t%s\t%s\n' % (m['id'], m['file'], m['name'].replace('\t', ' ')))
    with open(os.path.join(OUT, 'manifest.json'), 'w', encoding='utf-8') as g:
        json.dump(manifest, g, indent=1)

    print('spec files:     %d (%d skipped as ysh/oil)' % (len(files), skipped))
    print('cases written:  %d -> %s' % (n, OUT))

if __name__ == '__main__':
    main()
