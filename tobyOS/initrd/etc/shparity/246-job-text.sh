# A job's name is its command as bash prints it: `jobs` shows
# `{ sleep 1; echo x; } > /dev/null &`, not the words tsh had expanded.
# (batch 12; probe p74 in C:\t\b12)
exec 2>&1
t=1
sleep $t &
jobs
{ sleep $t; echo   x; } >/dev/null &
jobs
for i in 1; do sleep $t; done &
jobs
( sleep 0.2;   echo y ) >/dev/null &
jobs
wait
jobs
echo "s=$?"
