sleep 0.1 &
pid=$!
kill -s TERM $pid
echo kill=$?

wait $pid
echo wait=$?
