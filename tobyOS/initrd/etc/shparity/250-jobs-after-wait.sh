# A plain `wait`: the job it waited for is done with, one that ended
# meanwhile is kept if it is $!, and the current job (+) is chosen again
# as bash's reset_current does -- never a finished job. `jobs; jobs` lists
# a finished job once.
# (batch 12; probe p88 in C:\t\b12)
exec 2>&1
sleep 0.3 & wait; echo "a:"; jobs
( sleep 0.3 ) & wait; echo "b:"; jobs
sleep 0.6 & sleep 0.1 & wait; echo "c:"; jobs; echo "c2:"; jobs
sleep 0.1 & sleep 0.6; wait; echo "d:"; jobs
sleep 0.2 & wait $!; echo "e:"; jobs
sleep 1 & { sleep 1; } >/dev/null & ( sleep 0.2; echo y ) >/dev/null & wait; echo "f:"; jobs
