# S9: the job a plain `wait` keeps ($!), seen through jobs and wait PID.
(exit 3) & sleep 0.2; wait; jobs; echo "jobs after plain wait: $?"
wait -n; echo "then wait -n: $?"
(exit 4) & p=$!; sleep 0.2; wait; wait $p; echo "wait \$! after plain wait: $?"
wait -n; echo "then wait -n: $?"
(exit 5) & p=$!; (exit 6) & sleep 0.2; wait; wait $p; echo "wait older pid after plain wait: $?"
wait -n; echo "then wait -n: $?"
sleep 0.3 & wait; wait -n -p v; echo "wait -n -p after plain wait: $? [$([ "$v" = "$!" ] && echo bang)]"
(exit 7) & sleep 0.2; wait; (exit 8) & sleep 0.2; wait; wait -n; echo "two plain waits: $?"; wait -n; echo "then: $?"
echo done
