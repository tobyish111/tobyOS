case $SH in dash|bash|mksh) exit ;; esac

sleep 0.01 &
(exit 55) &
true &
wait
echo wait $?

sleep 0.01 &
(exit 44) &
true &
wait --all
echo wait --all $?
