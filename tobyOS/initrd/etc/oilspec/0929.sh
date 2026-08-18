case $SH in mksh) exit ;; esac  # mksh is flaky

sleep 0.1 &
pid=$!
kill -15 $pid
echo kill=$?

wait $pid
echo wait=$?  # 143 is 128 + SIGTERM
