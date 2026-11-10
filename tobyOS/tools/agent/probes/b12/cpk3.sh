exec 2>&1
( sleep 4 ) &
q=$!
sleep 0.3
kill $q; echo "kill=$?"
t0=$SECONDS
wait $q; echo "w=$? dt=$((SECONDS-t0))"
