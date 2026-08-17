# GAP 3: pathname expansion in a `for` list, not just in command arguments.
# Also pins the two rules that are easy to miss: results are SORTED, and a
# pattern matching nothing expands to itself. Runs in a wiped scratch dir.
count() { echo "n=$#"; }

> b.txt
> a.txt
> ab.txt
> c.log

for f in *.txt; do echo "t:$f"; done
for f in *.log; do echo "l:$f"; done
for f in ?.txt; do echo "q:$f"; done
for f in [ab].txt; do echo "cls:$f"; done
for f in *.none; do echo "nomatch:$f"; done
for f in *; do echo "all:$f"; done

count *.txt
count *.none
count "*.txt"

for f in *.txt *.log; do echo "multi:$f"; done
for f in "*.txt"; do echo "quoted:$f"; done
echo globfor-done
