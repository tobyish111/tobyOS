exec 2>&1 </dev/null
# An asynchronous command without job control ignores SIGINT and SIGQUIT
# (bash's setup_async_signals): `kill -INT $!` leaves it running.
t() { local e=$((SECONDS + $1)); while [ $SECONDS -lt $e ]; do :; done; }
sleep 3 &
t 1
kill -INT $!; t 1
jobs
kill -QUIT %1; t 1
jobs
kill %1; wait
sleep 3 | sleep 3 &
t 1
kill -INT %1; t 1
jobs
kill %1; wait
{ sleep 3; echo "group lived"; } &
t 1
kill -INT $!; wait; echo "group: $?"
set -m
sleep 3 &
t 1
kill -INT $!; t 1
jobs
wait
echo end
