#case $SH in mksh|dash) exit ;; esac

sleep 0.1 &
pid=$!
kill -n 9 $pid
echo kill=$?

wait $pid
echo wait=$?
