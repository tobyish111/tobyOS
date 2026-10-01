"""oilsflips.py RUN.json RUN.json [...] -- the Oils cases whose result is not
the same in every run (M1). Each RUN.json is a copy of logs/oilspec_prev.json
taken right after an oilspec run. Prints one line per case that changed, with
its result in each run in order, then a count. Exit 0 always."""
import json, sys

runs = [json.load(open(p))['results'] for p in sys.argv[1:]]
names = sys.argv[1:]
cases = sorted(set().union(*[r.keys() for r in runs]))
flips = []
for c in cases:
    vals = [r.get(c, '?') for r in runs]
    if len(set(vals)) > 1:
        flips.append((c, vals))
for c, vals in flips:
    print('%s  %s' % (c, '  '.join(vals)))
print('cases=%d runs=%d changed=%d  (%s)' % (len(cases), len(runs), len(flips),
                                             ' '.join(names)))
