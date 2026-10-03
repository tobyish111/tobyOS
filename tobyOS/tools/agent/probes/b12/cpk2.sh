exec 2>&1
( sleep 5 ) &
q=$!
sleep 0.3
kill $q; echo "kill2=$?"
sleep 0.3
ps | grep -c 'sleep 5'
wait $q; echo "w=$?"
