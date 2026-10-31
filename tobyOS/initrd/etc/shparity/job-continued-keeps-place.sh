# A stopped job continued from outside keeps its place: the current job
# stays `+` -- tsh gave `+` to another job that was stopped (finding 543).
set -m
sleep 300 & sleep 350 & sleep 400 &
kill -STOP %2
sleep 1
kill -STOP %3
sleep 1
kill -CONT %3
sleep 1
jobs
kill -CONT %2
sleep 1
jobs
exec 2>/dev/null
kill %1 %2 %3
wait
