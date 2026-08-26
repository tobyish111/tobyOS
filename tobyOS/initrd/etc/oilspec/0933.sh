case $SH in mksh|dash|zsh) exit ;; esac

sleep 0.1 &
pid=$!
kill -SigterM $pid
echo kill=$?
wait $pid
echo wait=$?

sleep 0.1 &
pid=$!
kill -terM $pid
echo kill=$?
wait $pid
echo wait=$?
