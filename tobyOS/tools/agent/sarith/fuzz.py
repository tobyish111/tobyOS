"""Random arithmetic cases for the bash comparison (harness.c / gen.py).

    fuzz.py SEED COUNT > cases-fuzz.txt

Each case gets a random setup (scalars holding numbers or expressions, an
indexed array, sometimes set -u) and a random expression built from every
token kind the evaluator has -- constants in each base, names, elements,
all operators, assignments, ++/--, parentheses, ?: and commas -- usually
well-formed, sometimes damaged (a token dropped or doubled). Deterministic
for a SEED.
"""
import random
import sys

NAMES = ['x', 'y', 'z', 'i', 'j']
CONSTS = ['0', '1', '2', '3', '7', '10', '42', '255', '0x1f', '0X10', '017', '010',
          '2#101', '8#17', '16#ff', '36#z', '64#@', '64#_', '64#Z', '9223372036854775807']
BINOPS = ['+', '-', '*', '/', '%', '**', '<<', '>>', '<', '<=', '>', '>=', '==',
          '!=', '&', '^', '|', '&&', '||', ',']
ASSIGN = ['=', '+=', '-=', '*=', '/=', '%=', '<<=', '>>=', '&=', '^=', '|=']
# no `~`: a `~` sends bash's $(( )) text through tilde expansion, which
# backslash-quotes it -- a shell quirk, not the evaluator's
UNARY = ['-', '+', '!']


def atom(r, d):
    k = r.randrange(10)
    if k < 3:
        return r.choice(CONSTS)
    if k < 6:
        return r.choice(NAMES)
    if k == 6:
        return 'a[%s]' % (expr(r, d + 1) if d < 3 else r.choice(['0', '1', '2', 'i', '-1']))
    if k == 7:
        n = r.choice(NAMES + ['a[1]', 'a[i]'])
        return r.choice(['++' + n, '--' + n, n + '++', n + '--'])
    if k == 8 and d < 4:
        return '(' + expr(r, d + 1) + ')'
    return r.choice(UNARY) + atom(r, d + 1) if d < 5 else r.choice(CONSTS)


def expr(r, d=0):
    if d > 4:
        return atom(r, d)
    k = r.randrange(10)
    if k < 4:
        return atom(r, d)
    if k < 7:
        return expr(r, d + 1) + ' ' + r.choice(BINOPS) + ' ' + expr(r, d + 1)
    if k == 7:
        n = r.choice(NAMES + ['a[0]', 'a[2]', 'a[j]'])
        return n + ' ' + r.choice(ASSIGN) + ' ' + expr(r, d + 1)
    if k == 8:
        return expr(r, d + 1) + ' ? ' + expr(r, d + 1) + ' : ' + expr(r, d + 1)
    return expr(r, d + 1) + ', ' + expr(r, d + 1)


def damage(r, e):
    toks = e.split(' ')
    if len(toks) < 2:
        return e
    i = r.randrange(len(toks))
    if r.random() < 0.5:
        del toks[i]
    else:
        toks.insert(i, toks[i])
    return ' '.join(toks)


def setup(r):
    out = []
    if r.random() < 0.1:
        out.append('-u')
    for n in NAMES:
        if r.random() < 0.6:
            v = r.choice(['0', '1', '3', '5', '-2', '12', '0x10', '07', '2+3', 'i+1', 'x', 'y*2'])
            out.append('%s=%s' % (n, v))
    if r.random() < 0.7:
        for k in range(r.randrange(1, 4)):
            out.append('a[%d]=%s' % (k, r.choice(['1', '4', '9', '2*3', '-1'])))
    return ' '.join(out)


def main():
    seed, count = int(sys.argv[1]), int(sys.argv[2])
    r = random.Random(seed)
    for _ in range(count):
        e = expr(r)
        if r.random() < 0.25:
            d = damage(r, e)
            if d.count('(') == d.count(')') and d.count('[') == d.count(']'):
                e = d
        e = e.replace('\t', ' ')
        print(setup(r) + '\t' + e)


if __name__ == '__main__':
    main()
