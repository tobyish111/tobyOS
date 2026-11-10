exec 2>&1 </dev/null
# A BACKGROUND PIPELINE IS A JOB OF SEVERAL PROCESSES (bash forks each
# element from the shell): jobs -l lists each, $! is the last one's pid,
# jobs -p the first's, notices and waits are about all of them.
P() { sed -E 's/[0-9]{2,}/N/g'; }
t() { local e=$((SECONDS + $1)); while [ $SECONDS -lt $e ]; do :; done; }
echo "== no job control"
sleep 2 | sleep 1 &
last=$!
jobs | P
jobs -l | P
[ "$(jobs -p)" != "$last" ] && echo "jobs -p is not \$!"
wait %1; echo "wait %1: $?"
true | (exit 3) &
wait $!; echo "wait \$!: $?"
(exit 4) | true &
wait %%; echo "wait %%: $?"
sleep 1 | sleep 3 &
first=$(jobs -p)
wait $first; echo "wait first: $?"
jobs | P
wait
echo "== kill without job control"
sleep 5 | sleep 5 &
t 1
kill %1; wait %1; echo "killed: $?"
sleep 5 | sleep 5 &
t 1
kill $!; t 1; jobs -l | P
wait %1; echo "waited: $?"
echo end
