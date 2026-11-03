# Under set -m, the line straight after `kill %%` still finds the job: bash
# has not been told of its death yet, so `fg` brings it back, prints its
# command, and the death is fg's -- status 143 (Oils 2016, finding 606). tsh
# waited at that line until the job had died, reported it and dropped it,
# and `fg` had no job: "fg: current: no such job", status 1, every run.
# Both shells race the dying job here -- bash brought it back 20 runs in 20
# over SSH with a pipeline between the job's start and the kill, as Oils
# 2016 has it, 12 in 20 without one, and saw it dead twice in 20 in a
# shparity boot (logs/b43 kfrep1.txt, kfgate.txt) -- so the case runs it
# ten times and says whether three or more brought the job back (the
# settle, none of them).
f=$(mktemp)
printf '%s\n' 'set -m' 'sleep 5 &' 'echo x | grep -q x' 'kill %%' 'fg' 'echo "fg=$?"' > "$f"
back=0
for i in 1 2 3 4 5 6 7 8 9 10; do
    out=$("$BASH" "$f" 2>/dev/null)
    [ "$out" = "sleep 5
fg=143" ] && back=$((back + 1))
done
[ $back -ge 3 ] && echo "three or more runs: fg brought it back" || echo "fewer: the job was gone"
rm -f "$f"
