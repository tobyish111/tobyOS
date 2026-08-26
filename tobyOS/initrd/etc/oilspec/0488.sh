case $SH in dash|mksh) exit ;; esac

sleep 1 &
kill -HUP $!
wait $! 2>err.txt
echo status=$?
grep -o "Hangup" err.txt
