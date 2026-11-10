# S9: bash's saved statuses of reaped background pids (bgpids)
(exit 3) & wait $! $! $!; echo "three times: $?"
(exit 4) & p=$!; wait -n; wait $p; echo "after wait -n: $?"
(exit 5) & p=$!; sleep 0.2; wait $p; wait; wait $p; echo "after plain wait: $?"
(exit 6) & p=$!; wait $p; (exit 7) & q=$!; wait $q; wait $p; echo "older one: $?"
(exit 8) & p=$!; sleep 0.2; jobs >/dev/null; wait $p; echo "after jobs: $?"
echo done
