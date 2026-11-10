"""dumphunks.py BASE TARGET -- every hunk of BASE -> TARGET in full, numbered
as split2.py numbers them, for mapping hunks to items by reading them:

    === HUNK 12  -340,2 +351,9
    - old line
    + new line

The MAP split2.py apply takes is then "INDEX ITEM" per hunk."""
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0] if '/' in __file__ else '.')
import split2   # noqa: E402

for i, h in enumerate(split2.diff_hunks(sys.argv[1], sys.argv[2])):
    print('=== HUNK %d  -%d,%d +%d,%d' % (i, h['os'], h['oc'], h['ns'], h['nc']))
    for l in h['old']:
        print('- ' + l)
    for l in h['new']:
        print('+ ' + l)
