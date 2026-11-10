case $SH in dash|bash|mksh) exit ;; esac

sleep 0.01 &
(exit 55) &
wait --verbose
echo wait $?

(exit 44) &
sleep 0.01 &
wait --all --verbose
echo wait --all $?
