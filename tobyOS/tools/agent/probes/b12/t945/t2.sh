sleep 0.1 &
sleep 0.2 &
kill -15 %-
echo kill=$?
:
:
:
jobs
wait %-
echo wait=$?
