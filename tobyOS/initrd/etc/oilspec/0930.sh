sleep 0.1 & 
pid=$!
kill -KILL $pid 
echo kill=$?

wait $pid
echo wait=$?  # 137 is 128 + SIGKILL
