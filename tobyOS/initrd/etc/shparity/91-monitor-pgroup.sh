# `set -m` with REAL process groups underneath. Until this slice the
# kernel LIED to both shells: setpgid was accept-and-return-0 and
# kill(-pgid) was a silent success that signalled nobody.
#
# Single-command jobs only: for a pipeline bash leads the group with the
# FIRST stage while $! names the LAST, so a -$! probe would ask about the
# wrong group. Timing is one-sided -- the job sleeps far longer than the
# probe path ever takes, and every wait is on an already-signalled job.
set -m
sleep 30 &
p=$!
kill -0 -- -$p
echo "1=$?"
kill -TERM -- -$p
echo "2=$?"
wait $p
echo "3=$?"
kill -0 -- -$p
echo "4=$?"
set +m
sleep 30 &
q=$!
kill -0 -- -$q
echo "5=$?"
kill -TERM $q
echo "6=$?"
wait $q
echo "7=$?"
echo end
