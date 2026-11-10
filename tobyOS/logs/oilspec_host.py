#!/usr/bin/env python3
"""oilspec_host.py -- run the extracted Oils corpus under REAL bash and dash.

WHY THIS EXISTS
---------------
The corpus ships recorded expectations (`## STDOUT:`), but those were recorded
years ago against other builds on another machine. Trusting them as the oracle
would make every stale or environment-dependent expectation look like a tsh
bug, and we would then "fix" tsh to match a fiction. So this runs the corpus
under the real shells, here, now, and that is what classifies each case:

  POSIX     bash and dash produce identical stdout AND identical status.
            This is the POSIX core -- the part tsh MUST get right to claim
            POSIX compliance, and the number the compliance score is over.
  BASH-ONLY they disagree (or dash rejects the syntax). tsh follows bash here,
            per the superset contract; not a POSIX claim either way.
  UNUSABLE  a shell timed out or the case is non-deterministic across two runs
            of the SAME shell. Excluded from scoring in both directions --
            a corpus case that cannot decide anything must not be able to
            fail us either.

Non-determinism is measured, not assumed: every case runs under bash TWICE, in
different scratch directories, and any case whose two runs disagree is dropped.
That catches $RANDOM, $$, dates, and anything reading the wider filesystem
before those become mystery FAILs in the guest.
"""
import os, sys, json, subprocess, tempfile, shutil
from concurrent.futures import ThreadPoolExecutor

HERE    = os.path.dirname(os.path.abspath(__file__))
CORPUS  = os.path.join(HERE, '..', 'initrd', 'etc', 'oilspec')
SPECBIN = os.environ.get('SPECBIN') or os.path.join(HERE, 'specbin')
SCRATCH = os.environ.get('OILSPEC_SCRATCH') or tempfile.mkdtemp(prefix='oilspec')
TIMEOUT = 10

BASH = os.environ.get('HOST_BASH', 'bash')
DASH = os.environ.get('HOST_DASH', 'dash')

def run(shell_argv, case, tag):
    """Run one case in a private, freshly wiped directory."""
    cwd = os.path.join(SCRATCH, tag, os.path.basename(case)[:-3])
    shutil.rmtree(cwd, ignore_errors=True)
    os.makedirs(cwd, exist_ok=True)
    env = {
        'PATH': SPECBIN + os.pathsep + '/usr/bin' + os.pathsep + '/bin',
        'HOME': cwd, 'LC_ALL': 'C', 'LANG': 'C', 'TERM': 'dumb',
        'SHELL': shell_argv[0], 'PWD': cwd,
    }
    try:
        p = subprocess.run(shell_argv + [os.path.abspath(case)],
                           cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=TIMEOUT)
        return {'status': p.returncode, 'stdout': p.stdout.decode('utf-8', 'surrogateescape')}
    except subprocess.TimeoutExpired:
        return {'status': None, 'stdout': None, 'timeout': True}
    except OSError as e:
        return {'status': None, 'stdout': None, 'error': str(e)}

def classify(case):
    cid = os.path.basename(case)[:-3]
    b1 = run([BASH, '--norc', '--noprofile'], case, 'b1')
    b2 = run([BASH, '--norc', '--noprofile'], case, 'b2')
    d  = run([DASH], case, 'd')

    if b1.get('timeout') or b1.get('error'):
        return {'id': cid, 'class': 'UNUSABLE', 'why': 'bash timeout/error'}
    if (b1['stdout'], b1['status']) != (b2['stdout'], b2['status']):
        return {'id': cid, 'class': 'UNUSABLE', 'why': 'nondeterministic under bash'}

    rec = {'id': cid, 'bash_status': b1['status'], 'bash_stdout': b1['stdout']}
    if d.get('timeout') or d.get('error'):
        rec['class'] = 'BASH-ONLY'; rec['why'] = 'dash timeout/error'
    elif (d['stdout'], d['status']) == (b1['stdout'], b1['status']):
        rec['class'] = 'POSIX'
    else:
        rec['class'] = 'BASH-ONLY'
        rec['dash_status'] = d['status']; rec['dash_stdout'] = d['stdout']
    return rec

def main():
    cases = sorted(f for f in os.listdir(CORPUS) if f.endswith('.sh'))
    paths = [os.path.join(CORPUS, c) for c in cases]
    print('running %d cases x 3 shell invocations under bash/dash ...' % len(paths))
    out = []
    with ThreadPoolExecutor(max_workers=int(os.environ.get('JOBS', '12'))) as ex:
        for i, r in enumerate(ex.map(classify, paths)):
            out.append(r)
            if (i + 1) % 250 == 0:
                print('  %d/%d' % (i + 1, len(paths)), flush=True)

    dest = os.path.join(HERE, 'oilspec_host.json')
    with open(dest, 'w', encoding='utf-8') as f:
        json.dump(out, f)
    counts = {}
    for r in out:
        counts[r['class']] = counts.get(r['class'], 0) + 1
    print('--- host classification ---')
    for k in sorted(counts):
        print('  %-10s %d' % (k, counts[k]))
    print('wrote %s' % dest)
    shutil.rmtree(SCRATCH, ignore_errors=True)

if __name__ == '__main__':
    main()
