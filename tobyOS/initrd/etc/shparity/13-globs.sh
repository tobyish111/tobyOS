# Pathname expansion. Two things here are load-bearing and easy to miss:
# glob results are SORTED, and a pattern that matches nothing expands to
# ITSELF (bash's default -- no nullglob). Runs in a wiped scratch directory.
> a.txt
> b.txt
> c.dat
> ab.txt
for f in *.txt; do echo "txt:$f"; done
for f in *.dat; do echo "dat:$f"; done
for f in ?.txt; do echo "q:$f"; done
for f in [ab].txt; do echo "cls:$f"; done
for f in *.none; do echo "nomatch:$f"; done
for f in *; do echo "all:$f"; done
echo "quoted-not-globbed: *.txt"
echo globs-done
