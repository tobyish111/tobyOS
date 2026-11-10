"""Damaged scripts for the bash-grammar differential test.

    fuzz.py CORPUS_DIR OUT_DIR SEED PER_FILE

Each script of CORPUS_DIR gives PER_FILE mutants: cut short at a random
byte, a span deleted, a shell token inserted, a byte replaced by a shell
metacharacter, a line duplicated or two lines swapped. Deterministic for a
SEED, so a failing case can be rebuilt. bash 5.2.15 (oracle.sh) and the
check (harness.c) must agree on every one of them.
"""
import os
import random
import sys

TOKENS = ['fi', 'then', 'else', 'elif', 'do', 'done', 'esac', 'in', ';;',
          ';&', ';;&', ')', '(', '{', '}', '&&', '||', '|', ';', '&', '!',
          'time', '[[', ']]', 'function', 'case', 'if', 'for', 'while',
          'select', 'coproc', '((', '))', '$(', '`', '"', "'", '${', '$((',
          '<<EOF', '<<-X', '>', '<', '>&', '2>', '{fd}>', '|&', '\n', 'x=(',
          '=(', '\\', '#', 'a=1', '[', ']', '$[', '<(', '>(', '@(', '!(',
          '*(', '-p', '--', 'until', '{a,b}', '\\\n', 'EOF\n', '$\'', '$"']
METAS = list(';&|()<>{}"\'`$\n#\\ ')


def mutate(rng, data):
    k = rng.randrange(7)
    n = len(data)
    if n == 0:
        return data + rng.choice(TOKENS)
    if k == 0:                              # cut short
        return data[:rng.randrange(n)]
    if k == 1:                              # delete a span
        i = rng.randrange(n)
        return data[:i] + data[i + rng.randint(1, 12):]
    if k == 2:                              # insert a token
        i = rng.randrange(n + 1)
        t = rng.choice(TOKENS)
        sep = rng.choice(['', ' ', ' '])
        return data[:i] + sep + t + sep + data[i:]
    if k == 3:                              # replace a byte
        i = rng.randrange(n)
        return data[:i] + rng.choice(METAS) + data[i + 1:]
    lines = data.split('\n')
    if k == 4 and lines:                    # duplicate a line
        i = rng.randrange(len(lines))
        lines.insert(i, lines[i])
        return '\n'.join(lines)
    if k == 5 and len(lines) > 1:           # swap two lines
        i, j = rng.randrange(len(lines)), rng.randrange(len(lines))
        lines[i], lines[j] = lines[j], lines[i]
        return '\n'.join(lines)
    # two token insertions
    for _ in range(2):
        i = rng.randrange(len(data) + 1)
        data = data[:i] + ' ' + rng.choice(TOKENS) + ' ' + data[i:]
    return data


def main():
    src, out, seed, per = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    os.makedirs(out, exist_ok=True)
    rng = random.Random(seed)
    for name in sorted(os.listdir(src)):
        if not name.endswith('.sh'):
            continue
        with open(os.path.join(src, name), 'rb') as f:
            data = f.read().decode('latin-1')
        for m in range(per):
            d = data
            for _ in range(rng.randint(1, 2)):
                d = mutate(rng, d)
            base = name[:-3]
            with open(os.path.join(out, '%s_%d.sh' % (base, m)), 'wb') as f:
                f.write(d.encode('latin-1'))


if __name__ == '__main__':
    main()
