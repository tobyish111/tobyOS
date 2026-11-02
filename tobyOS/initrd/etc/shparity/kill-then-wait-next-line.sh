# Under set -m, a job kill has signalled to death has died by the time the
# reader takes the next line, as bash's has: `kill %1 %2 %3` and then
# `wait` report all three Terminated -- tsh did so about half its runs,
# else the last job alone (finding 600). A race in both shells (bash all
# three 20 runs in 20 alone, now and then not under load), so the case runs
# it ten times and says whether nine or more reported all three.
f=$(mktemp)
printf '%s\n' 'set -m' 'sleep 300 & sleep 350 & sleep 400 &' 'kill %1 %2 %3' 'wait' 'echo rc=$?' > "$f"
all=0
for i in 1 2 3 4 5 6 7 8 9 10; do
    n=$("$BASH" "$f" 2>&1 | grep -c Terminated)
    [ "$n" = 3 ] && all=$((all + 1))
done
[ $all -ge 9 ] && echo "nine or more runs: all three" || echo "fewer: not all three"
rm -f "$f"
