exec 2>&1 </dev/null
# More of a background pipeline: wait's answer for a middle pid, the
# subshell level of its elements, stdin of an async command, PIPESTATUS,
# the report of a pipeline killed in a script, disown, jobs -r/-s.
P() { sed -E 's/[0-9]{2,}/N/g'; }
t() { local e=$((SECONDS + $1)); while [ $SECONDS -lt $e ]; do :; done; }
(sleep 1; exit 2) | (sleep 2; exit 3) &
first=$(jobs -p)
s=$SECONDS
wait $first; echo "wait first: $? after $((SECONDS - s))s"
jobs | P
(exit 2) | (sleep 1; exit 3) &
wait %1; echo "wait %1: $? PIPESTATUS=${PIPESTATUS[*]}"
echo "async subshell level: $(echo $BASH_SUBSHELL & wait)"
echo "async pipeline element: $(echo $BASH_SUBSHELL | cat & wait)"
echo "async group in a pipeline: $({ echo $BASH_SUBSHELL; } | cat & wait)"
echo "== stdin of an async command, no job control"
printf 'line\n' | $SH -c 'cat & wait; echo "end 1"'
printf 'line\n' | $SH -c 'cat | cat & wait; echo "end 2"'
printf 'line\n' | $SH -c '{ cat; } & wait; echo "end 3"'
printf 'line\n' | $SH -c 'cat < /dev/stdin & wait; echo "end 4"'
echo "== the same under set -m"
printf 'line\n' | $SH -c 'set -m; cat & wait; echo "end 5"'
echo "== an async pipeline element's EXIT trap"
{ trap 'echo element exit trap' EXIT; echo in element; } | cat &
wait
echo "== killed pipeline, no job control"
sleep 5 | sleep 5 &
t 1
kill %1
t 1
echo "after kill"
wait
sleep 5 | sleep 6 &
t 1
kill $!
t 1
echo "after kill last"
wait %1; echo "waited: $?"
echo "== jobs -r, -s, disown"
sleep 2 | sleep 2 &
jobs -r | P
jobs -s | P
disown %1
jobs | P
echo end
