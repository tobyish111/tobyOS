exec 2>&1
sleep 0.1 &
sleep 0.4
jobs
echo "j=$?"
wait %1; echo "w1=$?"
sleep 0.1 &
p=$!
sleep 0.4
wait $p; echo "wp=$?"
sleep 0.1 & sleep 0.4; jobs; wait %%; echo "same-line w=$?"
sleep 3 & sleep 3 &
kill $(jobs -p); wait
sleep 1 & sleep 1 & jobs
wait
