# AN ASYNCHRONOUS COMMAND WITHOUT JOB CONTROL IGNORES SIGINT AND SIGQUIT
# (batch 14), bash's setup_async_signals: the keyboard's signals belong to
# the foreground, so `kill -INT $!` leaves a background sleep running --
# and a background { } and every element of a background pipeline. Under
# set -m the job has its own group and SIGINT ends it. tsh's async
# children died of it.
exec 2>&1
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
