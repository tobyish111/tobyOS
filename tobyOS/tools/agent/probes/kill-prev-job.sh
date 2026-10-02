sleep 1 &
sleep 2 &
kill -15 %-
echo kill=$?
wait %-
echo wait=$?
kill %+ 2>/dev/null; wait 2>/dev/null
sleep 0.1 & a=$(date +%s%N 2>/dev/null); sleep 0.2 & kill -0 %- 2>/dev/null; echo alive=$?
wait
