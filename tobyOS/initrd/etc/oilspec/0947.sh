sleep 0.1 &
pid=$!
sleep 0.1 &
kill %2 $pid
echo $?
