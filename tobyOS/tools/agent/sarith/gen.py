"""Bash's side of the arithmetic comparison.

    gen.py CASES OUTDIR

For each case of CASES ("SETUP<TAB>EXPR", see harness.c) writes OUTDIR/NNNN.sh,
which sets the variables up, prints `val=$(( EXPR ))`, the status, and
declare -p of the same fixed list of names. run-bash.sh runs them all and
normalises bash's "NAME: line N: " away, giving what harness.c prints.
"""
import os
import sys

NAMES = "a b c i j k m n x y z s t u v w".split()


def main():
    cases, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    n = 0
    for line in open(cases, encoding='utf-8'):
        line = line.rstrip('\n').rstrip('\r')
        if not line or line.startswith('#') or '\t' not in line:
            continue
        setup, expr = line.split('\t', 1)
        n += 1
        body = []
        assoc = set()
        for t in setup.split():
            if t == '-u':
                body.append('set -u')
                continue
            name, val = t.split('=', 1)
            val = val.replace('\s', ' ')
            if '[' in name:
                body.append(f"{name}='{val}'")
            elif '{' in name:
                nm, key = name.split('{', 1)
                key = key.rstrip('}')
                if nm not in assoc:
                    body.append(f'declare -A {nm}')
                    assoc.add(nm)
                body.append(f"{nm}[{key}]='{val}'")
            else:
                body.append(f"{name}='{val}'")
        body.append(f'echo "val=$(( {expr} ))"')
        body.append('echo "st=$?"')
        body.append('for __n in ' + ' '.join(NAMES) + '; do declare -p $__n 2>/dev/null; done')
        with open(os.path.join(out, '%04d.sh' % n), 'w', newline='\n') as f:
            f.write('\n'.join(body) + '\n')


if __name__ == '__main__':
    main()
