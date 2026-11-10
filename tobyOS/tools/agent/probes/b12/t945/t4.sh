sleep 0.1 &
sleep 0.2 &
kill -15 %-
echo kill=$?
sleep 0.05
jobs
wait %-
echo wait=$?
